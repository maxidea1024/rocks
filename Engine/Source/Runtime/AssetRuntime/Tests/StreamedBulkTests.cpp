#include "ManagerTestSupport.h"

#include <AssetTestFixtures/TestAssets.generated.h>
#include <Core/FileError.h>
#include <Core/ScopeExit.h>
#include <Platform/AsyncFile.h>
#include <Platform/Process.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <Task/WorkerScheduler.h>
#include <TestSupport/HostFailure.h>
#include <TestSupport/NFailAllocator.h>

#include <stdio.h>
#include <string.h>

// 스트리밍 bulk 범위 읽기 시험이다(Docs/Design/AssetSystem 20.6, 20.4 런타임): 범위 경계(0·끝·블록
// 경계·0 byte)와 블록 경계 IO, 파일 하나를 같이 씀, 짧은 읽기·블록 해시·크기·머리·없음·IO 오류,
// 제출 전·뒤 취소, 우선순위 순서·승격, 완료 재정렬(gate IO — 시간 대기 없음), callback 정확히 한
// 번·worker 스레드, callback 안 자기 손잡이 파괴, 마지막 손잡이 뒤 파일 닫힘, lease 반납 뒤 읽기,
// Shutdown(큐 취소·진행 IO 취소·뒤 Open·Read ShuttingDown), 수명 위반 Fatal(death test), 할당 실패
// 주입, OpenStreamedBulk 거부, worker 제출 실패, scope Revoke·WrongThread·진행 상한 0, 디스크
// IO(AsyncStreamedBulkIo — 동시 읽기·완료 오류 주입·블록 손상).
using namespace rocks;

namespace rocks::test {
bool RegisterProcessProbe(const char* mode,
                          int (*function)(int, const char* const*) noexcept) noexcept;
} // namespace rocks::test

namespace streamed_bulk_tests_file {

using namespace rocks::asset_test;

namespace {

// sidecar bulk 크기다(64 KiB 블록 넷 — 블록 경계를 넘는 범위를 만든다).
constexpr uint32 BigBytes = 200000;
constexpr uint32 ResidentBytes = 3000;
constexpr uint64 BlockBytes = SidecarHashBlockBytes;
// 첫 sidecar entry 위치다(머리 64 byte 뒤 쿠킹 정렬 4096).
constexpr uint64 FirstEntryOffset = 4096;

// 완료를 시험이 내보내는 범위 IO다(hold면 쌓아 두고 Complete*가 내보낸다).
class TestStreamedIo final : public StreamedBulkIo {
public:
    struct File {
        explicit File(Allocator& allocator) noexcept : path(allocator), bytes(allocator) {
        }

        String path;
        Array<uint8> bytes;
        ErrorCode openError = ErrorCode::None;
        ErrorCode readError = ErrorCode::None;
        // 읽은 byte를 이만큼 줄여 보고한다(짧은 읽기).
        uint64 shortBy = 0;
    };

    struct Held {
        StreamedBulkIoRead* read = nullptr;
        File* file = nullptr;
        bool cancelled = false;
    };

    struct Record {
        uint64 offset = 0;
        uint64 bytes = 0;
    };

    explicit TestStreamedIo(Allocator& allocator) noexcept
        : files(allocator), held(allocator), reads(allocator) {
        // File 주소가 열린 파일 handle이라 옮기지 않는다.
        (void)files.TryReserve(MaxFiles);
    }

    bool Add(StringView path, Span<const uint8> bytes) noexcept {
        if (files.Size() == MaxFiles) {
            return false;
        }

        auto added = files.TryEmplaceBack(files.GetAllocator());
        return added && added.Value()->path.TryAssign(path) &&
               added.Value()->bytes.TryAppend(bytes);
    }

    File* Find(StringView path) noexcept {
        for (File& file : files) {
            if (file.path.View() == path) {
                return &file;
            }
        }

        return nullptr;
    }

    Result<StreamedBulkFile*> Open(const char* path, usize pathBytes, Span<uint8> header,
                                   uint64& fileBytes) noexcept override {
        ++opens;
        File* file = Find(StringView(path, pathBytes));
        if (file == nullptr) {
            return Result<StreamedBulkFile*>::Failure(ErrorCode::NotFound);
        }

        if (file->openError != ErrorCode::None) {
            return Result<StreamedBulkFile*>::Failure(file->openError);
        }

        fileBytes = file->bytes.Size();
        for (usize index = 0; index < header.Size(); ++index) {
            header[index] = index < file->bytes.Size() ? file->bytes[index] : 0;
        }

        return Result<StreamedBulkFile*>::Success(reinterpret_cast<StreamedBulkFile*>(file));
    }

    Result<void> Read(StreamedBulkFile& opened, StreamedBulkIoRead& read) noexcept override {
        if (onRead != nullptr) {
            onRead(onReadContext);
        }

        if (failRead != ErrorCode::None) {
            return Result<void>::Failure(failRead);
        }

        (void)reads.TryPushBack(Record{read.offset, read.bytes});
        ++active;
        Held entry{&read, reinterpret_cast<File*>(&opened), false};
        if (hold) {
            (void)held.TryPushBack(entry);
            return Result<void>::Success();
        }

        Finish(entry);
        return Result<void>::Success();
    }

    void Cancel(StreamedBulkIoRead& read) noexcept override {
        ++cancels;
        for (Held& entry : held) {
            if (entry.read == &read) {
                entry.cancelled = true;
            }
        }

        if (onCancel != nullptr) {
            onCancel->Signal();
        }
    }

    void Release(StreamedBulkIoRead&) noexcept override {
        ++released;
        --active;
    }

    void Close(StreamedBulkFile*) noexcept override {
        ++closes;
    }

    // 쌓인 완료를 앞에서 count개 내보낸다.
    void Complete(usize count = NumericLimits<usize>::Max()) noexcept {
        for (usize done = 0; done < count && !held.IsEmpty(); ++done) {
            const Held entry = held[0];
            (void)held.TryRemoveAt(0);
            Finish(entry);
        }
    }

    void CompleteReverse() noexcept {
        while (!held.IsEmpty()) {
            const Held entry = held[held.Size() - 1];
            (void)held.TryPopBack();
            Finish(entry);
        }
    }

    Array<File> files;
    Array<Held> held;
    Array<Record> reads;
    bool hold = false;
    // 취소된 쌓인 완료를 Cancelled로 내보낼지다(아니면 원래 결과 — 경합에서 끝난 경우).
    bool cancelCompletes = true;
    ErrorCode failRead = ErrorCode::None;
    // Cancel마다 알린다(진행 IO 취소를 기다리는 도우미 스레드 — 명시 gate).
    platform::ManualResetEvent* onCancel = nullptr;
    // Read 첫머리에서 부른다(제출 중 취소 시험).
    void (*onRead)(void* context) noexcept = nullptr;
    void* onReadContext = nullptr;
    uint32 opens = 0;
    uint32 closes = 0;
    uint32 cancels = 0;
    uint32 released = 0;
    uint32 active = 0;

private:
    static constexpr usize MaxFiles = 8;

    void Finish(const Held& entry) noexcept {
        StreamedBulkIoRead& read = *entry.read;
        const File& file = *entry.file;
        StreamedBulkIoResult result;
        if (entry.cancelled && cancelCompletes) {
            result.error = ErrorCode::Cancelled;
        } else if (file.readError != ErrorCode::None) {
            result.error = file.readError;
        } else {
            const uint64 size = file.bytes.Size();
            const uint64 available =
                read.offset >= size
                    ? 0
                    : (size - read.offset < read.bytes ? size - read.offset : read.bytes);
            memcpy(read.destination, file.bytes.Data() + read.offset, available);
            result.bytes = available > file.shortBy ? available - file.shortBy : 0;
        }

        read.complete(read, result);
    }
};

// callback 기록이다. worker가 돌리는 동안만 inWorker가 참이다(callback 스레드 확인).
bool InWorker = false;

struct Recorder {
    uint32 calls = 0;
    BulkReadResult last;
    bool onWorker = false;
    uint32 id = 0;
    Array<uint32>* order = nullptr;
    // 참이면 callback 안에서 handle을 파괴한다.
    BulkReadHandle* destroy = nullptr;
};

void Record(void* context, const BulkReadResult& result) noexcept {
    auto& recorder = *static_cast<Recorder*>(context);
    ++recorder.calls;
    recorder.last = result;
    recorder.onWorker = InWorker;
    if (recorder.order != nullptr) {
        (void)recorder.order->TryPushBack(recorder.id);
    }

    if (recorder.destroy != nullptr) {
        *recorder.destroy = BulkReadHandle();
    }
}

// 장면: 1·2(sidecar bulk BigBytes — 서로 다른 sidecar 파일), 3(패키지 안 bulk).
struct StreamRig {
    explicit StreamRig(AssetEnv& sceneEnv) noexcept
        : env(sceneEnv), rig(sceneEnv), io(sceneEnv.allocator), expected(sceneEnv.allocator) {
    }

    bool Start(bool withIo = true, uint32 maxInFlight = StreamedBulkMaxInFlight,
               Allocator* allocator = nullptr) noexcept {
        rig.scene.Add(1, BigBytes).sidecar = true;
        rig.scene.Add(2, BigBytes).sidecar = true;
        rig.scene.Add(3, ResidentBytes);
        if (!rig.Cook()) {
            return false;
        }

        for (usize index = 0; index < rig.scene.FileCount(); ++index) {
            if (!rig.scene.SidecarAt(index).IsEmpty() &&
                !io.Add(rig.scene.SidecarPathAt(index), rig.scene.SidecarAt(index).View())) {
                return false;
            }
        }

        if (withIo) {
            rig.config.streamedIo = streamedIo != nullptr ? streamedIo : &io;
        }

        if (worker != nullptr) {
            rig.config.loader.worker = worker;
        }

        rig.config.streamedMaxInFlight = maxInFlight;
        if (allocator != nullptr) {
            rig.config.loader.allocator = allocator;
        }

        FillCompressible(expected, BigBytes, 1);
        return rig.Start();
    }

    AssetLease Load(uint64 id) noexcept {
        AssetRequest request;
        request.id = FixedId(id);
        auto lease = rig.manager->LoadBlocking(request, "StreamedBulkTests");
        return lease ? Move(lease.Value()) : AssetLease();
    }

    BulkDataRef PixelsOf(const AssetLease& lease) noexcept {
        BulkDataRef ref;
        (void)rig.manager->With(lease, [&](ObjectHandle object) noexcept {
            TestAsset* asset = Pointer<TestAsset>(rig.manager->Registry(), object);
            if (asset != nullptr) {
                ref = asset->pixels;
            }
        });
        return ref;
    }

    StreamedBulk Open(uint64 id, AssetLease& lease) noexcept {
        lease = Load(id);
        auto bulk = rig.manager->OpenStreamedBulk(lease, PixelsOf(lease));
        return bulk ? Move(bulk.Value()) : StreamedBulk();
    }

    // 쌓인 worker 작업을 돌린다(그동안 InWorker 참).
    void RunWorker() noexcept {
        InWorker = true;
        rig.worker.RunAll();
        InWorker = false;
    }

    // IO와 worker를 비울 때까지 내보낸다.
    void Drain() noexcept {
        for (uint32 round = 0; round < 64 && (!io.held.IsEmpty() || !rig.worker.pending.IsEmpty());
             ++round) {
            io.Complete();
            RunWorker();
        }
    }

    bool Matches(Span<const uint8> bytes, uint64 offset) const noexcept {
        return SameBytes(bytes, expected.View().Slice(static_cast<usize>(offset), bytes.Size()));
    }

    AssetEnv& env;
    ManagerRig rig;
    TestStreamedIo io;
    Array<uint8> expected;
    // Start 전에 채우면 시험 IO·시험 worker 대신 쓴다(디스크 시험).
    StreamedBulkIo* streamedIo = nullptr;
    AssetLoaderWorker* worker = nullptr;
};

} // namespace

// 범위 읽기(AS 20.6 4·6): 0·1 byte·블록 경계를 넘는 범위·끝·전체·0 byte가 Done이고 byte가 맞다.
// IO는 블록 경계로 넓혀 읽고(시작 offset은 블록 배수), 파일은 한 번만 연다(같은 lease의
// StreamedBulk 둘). callback은 worker에서 정확히 한 번이다. 다른 에셋의 sidecar는 따로 연다.
TEST(StreamedBulk_ReadsAndVerifies) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StreamRig stream(env);
    REQUIRE(stream.Start());
    AssetLease lease;
    StreamedBulk bulk = stream.Open(1, lease);
    REQUIRE(bulk.IsValid());
    CHECK(bulk.Bytes() == BigBytes);
    stream.rig.worker.hold = true;
    struct Range {
        uint64 offset;
        uint64 bytes;
    };
    const Range ranges[] = {
        {0, 1},        {255, 2},           {BlockBytes - 10, 20}, {BigBytes - 1, 1},
        {0, BigBytes}, {BlockBytes * 2, 0}};
    for (const Range& range : ranges) {
        Array<uint8> buffer(env.allocator);
        REQUIRE(buffer.TryResize(static_cast<usize>(range.bytes)));
        Recorder recorder;
        auto handle =
            bulk.Read(range.offset, buffer.View(), BulkReadPriority::Normal, Record, &recorder);
        REQUIRE(handle);
        CHECK(recorder.calls == 0);
        stream.Drain();
        CHECK(recorder.calls == 1 && recorder.onWorker);
        CHECK(recorder.last.error == ErrorCode::None && recorder.last.bytes == range.bytes);
        CHECK(handle.Value().State() == BulkReadState::Done);
        CHECK(stream.Matches(buffer.View(), range.offset));
    }

    for (const TestStreamedIo::Record& read : stream.io.reads) {
        CHECK(read.offset % BlockBytes == 0);
    }

    CHECK(stream.io.opens == 1);
    auto reopened = stream.rig.manager->OpenStreamedBulk(lease, stream.PixelsOf(lease));
    REQUIRE(reopened);
    StreamedBulk again = Move(reopened.Value());
    uint8 one = 0;
    Recorder recorder;
    // 손잡이는 callback까지 쥔다(Pending 소멸은 Fatal).
    auto againHandle =
        again.Read(7, Span<uint8>(&one, 1), BulkReadPriority::Normal, Record, &recorder);
    REQUIRE(againHandle);
    stream.Drain();
    CHECK(recorder.calls == 1 && stream.io.opens == 1 && one == stream.expected[7]);
    // 로드는 worker를 돌려야 끝난다(LoadBlocking) — 여는 동안만 hold를 푼다.
    stream.rig.worker.hold = false;
    AssetLease otherLease;
    StreamedBulk other = stream.Open(2, otherLease);
    REQUIRE(other.IsValid());
    stream.rig.worker.hold = true;
    Recorder otherRecorder;
    auto otherHandle =
        other.Read(0, Span<uint8>(&one, 1), BulkReadPriority::Normal, Record, &otherRecorder);
    REQUIRE(otherHandle);
    stream.Drain();
    CHECK(otherRecorder.last.error == ErrorCode::None && stream.io.opens == 2);
}

// 거부(AS 20.6 3·4): OpenStreamedBulk — 패키지 안 bulk Unsupported, 빈 ref InvalidArgument, 없는
// bulkId NotFound, 다른 값 TypeMismatch, 빈 lease StaleHandle, IO 없는 AssetManager Unsupported.
// Read — 빈 StreamedBulk·null callback·범위 넘침(끝·offset) InvalidArgument(callback 없음).
TEST(StreamedBulk_Rejections) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StreamRig stream(env);
    REQUIRE(stream.Start());
    AssetManager& manager = *stream.rig.manager;
    AssetLease resident = stream.Load(3);
    REQUIRE(resident.IsValid());
    CHECK(manager.OpenStreamedBulk(resident, stream.PixelsOf(resident)).Error() ==
          ErrorCode::Unsupported);
    AssetLease lease = stream.Load(1);
    const BulkDataRef pixels = stream.PixelsOf(lease);
    CHECK(manager.OpenStreamedBulk(lease, BulkDataRef{}).Error() == ErrorCode::InvalidArgument);
    BulkDataRef missing = pixels;
    missing.bulkId = 9;
    CHECK(manager.OpenStreamedBulk(lease, missing).Error() == ErrorCode::NotFound);
    BulkDataRef wrong = pixels;
    wrong.rawBytes += 1;
    CHECK(manager.OpenStreamedBulk(lease, wrong).Error() == ErrorCode::TypeMismatch);
    CHECK(manager.OpenStreamedBulk(AssetLease(), pixels).Error() == ErrorCode::StaleHandle);

    auto opened = manager.OpenStreamedBulk(lease, pixels);
    REQUIRE(opened);
    StreamedBulk& bulk = opened.Value();
    uint8 buffer[16];
    Recorder recorder;
    StreamedBulk empty;
    CHECK(empty.Read(0, Span<uint8>(buffer, 1), BulkReadPriority::Normal, Record, &recorder)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(bulk.Read(0, Span<uint8>(buffer, 1), BulkReadPriority::Normal, nullptr, &recorder)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(bulk.Read(BigBytes - 8, Span<uint8>(buffer, 16), BulkReadPriority::Normal, Record,
                    &recorder)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(
        bulk.Read(BigBytes + 1, Span<uint8>(buffer, 0), BulkReadPriority::Normal, Record, &recorder)
            .Error() == ErrorCode::InvalidArgument);
    CHECK(recorder.calls == 0);

    AssetEnv bareEnv;
    REQUIRE(bareEnv.Ready());
    StreamRig bare(bareEnv);
    REQUIRE(bare.Start(false));
    AssetLease bareLease = bare.Load(1);
    CHECK(bare.rig.manager->OpenStreamedBulk(bareLease, bare.PixelsOf(bareLease)).Error() ==
          ErrorCode::Unsupported);
}

// 손상(AS 20.6 5·6): 같은 크기로 블록 1의 byte 하나를 바꾸면 블록 1을 덮는 읽기만 CorruptData(블록
// 0 읽기는 Done), 짧은 읽기 CorruptData, 읽기 IO 오류는 그 오류, 즉시 실패한 IO 제출도 그 오류,
// sidecar 크기 다름·머리 손상 CorruptData(모든 읽기 — 실패가 파일에 남는다), 파일 없음 NotFound.
TEST(StreamedBulk_CorruptionAndIoErrors) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StreamRig stream(env);
    REQUIRE(stream.Start());
    AssetLease lease;
    StreamedBulk bulk = stream.Open(1, lease);
    REQUIRE(bulk.IsValid());
    TestStreamedIo::File* file = stream.io.Find(stream.rig.scene.SidecarPathOf(1));
    REQUIRE(file != nullptr);
    auto readOne = [&](uint64 offset) noexcept {
        uint8 byte = 0;
        Recorder recorder;
        auto handle =
            bulk.Read(offset, Span<uint8>(&byte, 1), BulkReadPriority::Normal, Record, &recorder);
        if (!handle) {
            return handle.Error();
        }

        stream.Drain();
        // callback이 정확히 한 번이 아니면 시험이 쓰지 않는 값으로 알린다.
        return recorder.calls == 1 ? recorder.last.error : ErrorCode::AlreadyExists;
    };
    file->bytes[static_cast<usize>(BlockBytes + 10)] ^= 1;
    CHECK(readOne(BlockBytes + 100 - FirstEntryOffset) == ErrorCode::CorruptData);
    CHECK(readOne(10) == ErrorCode::None);
    file->bytes[static_cast<usize>(BlockBytes + 10)] ^= 1;
    file->shortBy = 1;
    CHECK(readOne(10) == ErrorCode::CorruptData);
    file->shortBy = 0;
    file->readError = ErrorCode::IoError;
    CHECK(readOne(10) == ErrorCode::IoError);
    file->readError = ErrorCode::None;
    stream.io.failRead = ErrorCode::Busy;
    CHECK(readOne(10) == ErrorCode::Busy);
    stream.io.failRead = ErrorCode::None;
    CHECK(readOne(10) == ErrorCode::None);

    // 크기 다름·머리 손상·없음·열기 IO 오류는 열기에서 걸리고 파일 기록에 남는다.
    struct Case {
        int kind;
        ErrorCode error;
    };
    const Case cases[] = {{0, ErrorCode::CorruptData},
                          {1, ErrorCode::CorruptData},
                          {2, ErrorCode::NotFound},
                          {3, ErrorCode::IoError}};
    for (const Case& one : cases) {
        AssetEnv caseEnv;
        REQUIRE(caseEnv.Ready());
        StreamRig broken(caseEnv);
        REQUIRE(broken.Start());
        TestStreamedIo::File* target = broken.io.Find(broken.rig.scene.SidecarPathOf(1));
        REQUIRE(target != nullptr);
        if (one.kind == 0) {
            REQUIRE(target->bytes.TryPushBack(0));
        } else if (one.kind == 1) {
            target->bytes[12] ^= 1;
        } else if (one.kind == 2) {
            target->path.Clear();
            REQUIRE(target->path.TryAssign("moved"));
        } else {
            target->openError = ErrorCode::IoError;
        }

        AssetLease brokenLease;
        StreamedBulk brokenBulk = broken.Open(1, brokenLease);
        REQUIRE(brokenBulk.IsValid());
        for (int attempt = 0; attempt < 2; ++attempt) {
            uint8 byte = 0;
            Recorder recorder;
            REQUIRE(brokenBulk.Read(0, Span<uint8>(&byte, 1), BulkReadPriority::Normal, Record,
                                    &recorder));
            broken.Drain();
            CHECK(recorder.calls == 1 && recorder.last.error == one.error);
        }

        CHECK(broken.io.opens == 1);
    }
}

// 큐(AS 20.6 5·7): 진행 상한 1에서 A가 진행 중이면 B(Low)·C(High)·D(Normal)·E(Critical)는 큐에서
// 우선순위 순으로 나간다. B를 Critical로 올리면 E보다 먼저다(같은 우선순위는 제출 순). D는 제출 전
// 취소라 IO 없이 Cancelled. 제출 뒤 취소는 IO 취소 요청이고 결과는 Cancelled(취소 적용) 또는
// Done(경합에서 이미 끝남)이다. 진행 상한 4에서 거꾸로 끝나도 모두 Done이고 callback은 끝난 순이다.
TEST(StreamedBulk_QueueOrderAndCancel) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StreamRig stream(env);
    REQUIRE(stream.Start(true, 1));
    AssetLease lease;
    StreamedBulk bulk = stream.Open(1, lease);
    REQUIRE(bulk.IsValid());
    // 파일을 먼저 연다(열기 작업이 끝나야 진행 상한이 뜻을 갖는다).
    {
        uint8 byte = 0;
        Recorder recorder;
        REQUIRE(bulk.Read(0, Span<uint8>(&byte, 1), BulkReadPriority::Normal, Record, &recorder));
        stream.Drain();
    }

    stream.io.hold = true;
    stream.io.reads.Clear();
    uint8 bytes[5] = {};
    Recorder recorders[5];
    const BulkReadPriority priorities[] = {BulkReadPriority::Normal, BulkReadPriority::Low,
                                           BulkReadPriority::High, BulkReadPriority::Normal,
                                           BulkReadPriority::Critical};
    BulkReadHandle handles[5];
    for (uint32 index = 0; index < 5; ++index) {
        // 블록마다 다른 offset(IO 기록으로 순서를 본다 — 블록 0·1·2·3·0 대신 끝 블록까지).
        const uint64 offset = index * (BlockBytes / 2);
        auto handle = bulk.Read(offset, Span<uint8>(&bytes[index], 1), priorities[index], Record,
                                &recorders[index]);
        REQUIRE(handle);
        handles[index] = Move(handle.Value());
    }

    REQUIRE(stream.io.reads.Size() == 1);
    handles[1].SetPriority(BulkReadPriority::Critical);
    handles[3].Cancel();
    stream.RunWorker();
    CHECK(recorders[3].calls == 1 && recorders[3].last.error == ErrorCode::Cancelled);
    CHECK(handles[3].State() == BulkReadState::Cancelled);
    // A 다음 B(올림), E, C 순이다.
    const uint32 order[] = {1, 4, 2};
    for (uint32 next : order) {
        stream.io.Complete(1);
        stream.RunWorker();
        REQUIRE(!stream.io.reads.IsEmpty());
        const uint64 expectedOffset = FirstEntryOffset + next * (BlockBytes / 2);
        CHECK(stream.io.reads[stream.io.reads.Size() - 1].offset ==
              expectedOffset / BlockBytes * BlockBytes);
    }

    stream.io.Complete();
    stream.RunWorker();
    for (uint32 index = 0; index < 5; ++index) {
        CHECK(recorders[index].calls == 1);
    }

    CHECK(stream.io.reads.Size() == 4);

    // 제출 뒤 취소: 취소가 적용되면 Cancelled, 경합으로 이미 끝났으면 Done.
    uint8 late = 0;
    Recorder cancelled;
    auto cancelledHandle =
        bulk.Read(1, Span<uint8>(&late, 1), BulkReadPriority::Normal, Record, &cancelled);
    REQUIRE(cancelledHandle && stream.io.held.Size() == 1);
    cancelledHandle.Value().Cancel();
    CHECK(stream.io.cancels == 1);
    stream.io.Complete();
    stream.RunWorker();
    CHECK(cancelled.last.error == ErrorCode::Cancelled &&
          cancelledHandle.Value().State() == BulkReadState::Cancelled);
    stream.io.cancelCompletes = false;
    Recorder raced;
    auto racedHandle =
        bulk.Read(1, Span<uint8>(&late, 1), BulkReadPriority::Normal, Record, &raced);
    REQUIRE(racedHandle);
    racedHandle.Value().Cancel();
    stream.io.Complete();
    stream.RunWorker();
    CHECK(raced.last.error == ErrorCode::None &&
          racedHandle.Value().State() == BulkReadState::Done);

    // 완료 재정렬(진행 상한 4).
    AssetEnv wideEnv;
    REQUIRE(wideEnv.Ready());
    StreamRig wide(wideEnv);
    REQUIRE(wide.Start(true, 4));
    AssetLease wideLease;
    StreamedBulk wideBulk = wide.Open(1, wideLease);
    REQUIRE(wideBulk.IsValid());
    wide.rig.worker.hold = true;
    wide.io.hold = true;
    Array<uint32> finished(wideEnv.allocator);
    Recorder wideRecorders[3];
    BulkReadHandle wideHandles[3];
    uint8 wideBytes[3] = {};
    for (uint32 index = 0; index < 3; ++index) {
        wideRecorders[index].id = index;
        wideRecorders[index].order = &finished;
        auto handle = wideBulk.Read(index * BlockBytes, Span<uint8>(&wideBytes[index], 1),
                                    BulkReadPriority::Normal, Record, &wideRecorders[index]);
        REQUIRE(handle);
        wideHandles[index] = Move(handle.Value());
    }

    wide.RunWorker();
    REQUIRE(wide.io.held.Size() == 3);
    wide.io.CompleteReverse();
    wide.RunWorker();

    REQUIRE(finished.Size() == 3);
    CHECK(finished[0] == 2 && finished[1] == 1 && finished[2] == 0);
    for (uint32 index = 0; index < 3; ++index) {
        CHECK(wideRecorders[index].last.error == ErrorCode::None &&
              wideBytes[index] == wide.expected[index * BlockBytes]);
    }
}

// 수명(AS 20.6 7·9): callback 안에서 자기 손잡이를 파괴해도 된다. 손잡이는 StreamedBulk보다 오래
// 살 수 있고, 파일은 마지막 StreamedBulk·요청이 놓일 때 한 번 닫힌다. lease를 놓아도 읽는다.
TEST(StreamedBulk_LifetimeRules) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StreamRig stream(env);
    REQUIRE(stream.Start());
    AssetLease lease;
    StreamedBulk bulk = stream.Open(1, lease);
    REQUIRE(bulk.IsValid());
    stream.rig.worker.hold = true;
    uint8 byte = 0;
    BulkReadHandle self;
    Recorder destroying;
    destroying.destroy = &self;
    auto handle =
        bulk.Read(3, Span<uint8>(&byte, 1), BulkReadPriority::Normal, Record, &destroying);
    REQUIRE(handle);
    self = Move(handle.Value());
    stream.Drain();
    CHECK(destroying.calls == 1 && !self.IsValid());

    lease = AssetLease();
    REQUIRE(stream.rig.Pump());
    Recorder afterLease;
    auto outliving =
        bulk.Read(5, Span<uint8>(&byte, 1), BulkReadPriority::Normal, Record, &afterLease);
    REQUIRE(outliving);
    BulkReadHandle kept = Move(outliving.Value());
    bulk = StreamedBulk();
    CHECK(stream.io.closes == 0);
    stream.Drain();
    CHECK(afterLease.last.error == ErrorCode::None && byte == stream.expected[5]);
    CHECK(stream.io.closes == 0);
    kept = BulkReadHandle();
    CHECK(stream.io.closes == 1 && stream.io.active == 0);
}

// 종료(AS 20.6 10): 큐의 요청은 Cancelled, 진행 IO는 취소 요청을 받고(도우미 스레드가 명시 gate로
// 끝낸다) Cancelled, Shutdown이 모든 callback을 기다린 뒤 새 Open·Read는 ShuttingDown이다.
TEST(StreamedBulk_Shutdown) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StreamRig stream(env);
    REQUIRE(stream.Start(true, 1));
    AssetLease lease;
    StreamedBulk bulk = stream.Open(1, lease);
    REQUIRE(bulk.IsValid());
    uint8 bytes[2] = {};
    {
        Recorder warm;
        REQUIRE(bulk.Read(0, Span<uint8>(bytes, 1), BulkReadPriority::Normal, Record, &warm));
        stream.Drain();
    }

    stream.io.hold = true;
    Recorder running;
    Recorder queued;
    auto runningHandle =
        bulk.Read(0, Span<uint8>(&bytes[0], 1), BulkReadPriority::Normal, Record, &running);
    auto queuedHandle =
        bulk.Read(1, Span<uint8>(&bytes[1], 1), BulkReadPriority::Normal, Record, &queued);
    REQUIRE(runningHandle && queuedHandle && stream.io.held.Size() == 1);
    platform::ManualResetEvent cancelRequested;
    stream.io.onCancel = &cancelRequested;
    struct Helper {
        platform::ManualResetEvent* gate;
        TestStreamedIo* io;
    } helper{&cancelRequested, &stream.io};
    platform::Thread thread;
    REQUIRE(thread.Start(
                [](void* context) noexcept {
                    auto& state = *static_cast<Helper*>(context);
                    (void)state.gate->Wait();
                    state.io->Complete();
                },
                &helper) == platform::ThreadStatus::Success);
    REQUIRE(stream.rig.manager->Shutdown());
    (void)thread.Join();
    CHECK(running.calls == 1 && running.last.error == ErrorCode::Cancelled);
    CHECK(queued.calls == 1 && queued.last.error == ErrorCode::Cancelled);
    Recorder after;
    CHECK(bulk.Read(0, Span<uint8>(bytes, 1), BulkReadPriority::Normal, Record, &after).Error() ==
          ErrorCode::ShuttingDown);
    AssetLease none;
    CHECK(stream.rig.manager->OpenStreamedBulk(none, BulkDataRef{}).Error() ==
          ErrorCode::ShuttingDown);

    // 진행 IO 둘 중 하나는 이미 사용자가 취소했다: Shutdown은 나머지 하나에만 취소를 보낸다.
    AssetEnv wideEnv;
    REQUIRE(wideEnv.Ready());
    StreamRig wide(wideEnv);
    REQUIRE(wide.Start(true, 2));
    AssetLease wideLease;
    StreamedBulk wideBulk = wide.Open(1, wideLease);
    REQUIRE(wideBulk.IsValid());
    {
        Recorder warm;
        REQUIRE(wideBulk.Read(0, Span<uint8>(bytes, 1), BulkReadPriority::Normal, Record, &warm));
        wide.Drain();
    }

    wide.io.hold = true;
    Recorder userCancelled;
    Recorder shutdownCancelled;
    auto userHandle = wideBulk.Read(0, Span<uint8>(&bytes[0], 1), BulkReadPriority::Normal, Record,
                                    &userCancelled);
    auto shutdownHandle = wideBulk.Read(BlockBytes, Span<uint8>(&bytes[1], 1),
                                        BulkReadPriority::Normal, Record, &shutdownCancelled);
    REQUIRE(userHandle && shutdownHandle && wide.io.held.Size() == 2);
    userHandle.Value().Cancel();
    REQUIRE(wide.io.cancels == 1);
    platform::ManualResetEvent wideCancel;
    wide.io.onCancel = &wideCancel;
    Helper wideHelper{&wideCancel, &wide.io};
    platform::Thread wideThread;
    REQUIRE(wideThread.Start(
                [](void* context) noexcept {
                    auto& state = *static_cast<Helper*>(context);
                    (void)state.gate->Wait();
                    state.io->Complete();
                },
                &wideHelper) == platform::ThreadStatus::Success);
    REQUIRE(wide.rig.manager->Shutdown());
    (void)wideThread.Join();
    CHECK(wide.io.cancels == 2);
    CHECK(userCancelled.calls == 1 && userCancelled.last.error == ErrorCode::Cancelled);
    CHECK(shutdownCancelled.calls == 1 && shutdownCancelled.last.error == ErrorCode::Cancelled);
}

// 할당 실패(AS 20.6 4·6): OpenStreamedBulk와 Read·검증의 모든 할당 지점에서 OutOfMemory(즉시 실패
// 또는 callback)이고, 다 지나면 Done이다.
TEST(StreamedBulk_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    test::NFailAllocator failing(env.allocator);
    StreamRig stream(env);
    REQUIRE(stream.Start(true, StreamedBulkMaxInFlight, &failing));
    AssetLease lease = stream.Load(1);
    REQUIRE(lease.IsValid());
    const BulkDataRef pixels = stream.PixelsOf(lease);
    uint32 openFailures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        // FailOnAttempt는 계수를 0부터 다시 센다.
        failing.FailOnAttempt(attempt);
        auto opened = stream.rig.manager->OpenStreamedBulk(lease, pixels);
        const bool hit = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!hit) {
            REQUIRE(opened);
            break;
        }

        CHECK(!opened && opened.Error() == ErrorCode::OutOfMemory);
        ++openFailures;
    }

    CHECK(openFailures > 0);
    auto bulk = stream.rig.manager->OpenStreamedBulk(lease, pixels);
    REQUIRE(bulk);
    uint32 readFailures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        uint8 buffer[32] = {};
        Recorder recorder;
        failing.FailOnAttempt(attempt);
        auto handle = bulk.Value().Read(BlockBytes - 16, Span<uint8>(buffer, 32),
                                        BulkReadPriority::Normal, Record, &recorder);
        stream.Drain();
        const bool hit = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!handle) {
            CHECK(handle.Error() == ErrorCode::OutOfMemory);
            ++readFailures;
            continue;
        }

        CHECK(recorder.calls == 1);
        if (!hit) {
            CHECK(recorder.last.error == ErrorCode::None &&
                  stream.Matches(Span<const uint8>(buffer, 32), BlockBytes - 16));
            break;
        }

        CHECK(recorder.last.error == ErrorCode::None ||
              recorder.last.error == ErrorCode::OutOfMemory);
        readFailures += recorder.last.error == ErrorCode::OutOfMemory ? 1 : 0;
    }

    CHECK(readFailures >= 2);
}

// worker 제출 실패(AS 20.6 8): 열기 작업을 못 걸면 파일이 그 오류로 실패해 읽기마다 그 오류이고
// (IO 열기 없음 — Game 스레드에서 열지 않는다) callback은 그 자리에서 한 번이다. 0 byte 읽기의
// 끝내기 작업도 그 자리에서 Done이다. 마지막 StreamedBulk를 놓고 다시 열면 새로 연다. 검증 작업을
// 못 걸면 해시 없이 그 오류로 끝나고 IO 자원은 돌려준다.
TEST(StreamedBulk_WorkerRunFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StreamRig stream(env);
    REQUIRE(stream.Start());
    AssetManager& manager = *stream.rig.manager;
    AssetLease lease = stream.Load(1);
    REQUIRE(lease.IsValid());
    const BulkDataRef pixels = stream.PixelsOf(lease);
    {
        auto opened = manager.OpenStreamedBulk(lease, pixels);
        REQUIRE(opened);
        stream.rig.worker.failRun = ErrorCode::Busy;
        for (int attempt = 0; attempt < 2; ++attempt) {
            uint8 byte = 0;
            Recorder recorder;
            auto handle = opened.Value().Read(0, Span<uint8>(&byte, 1), BulkReadPriority::Normal,
                                              Record, &recorder);
            REQUIRE(handle);
            CHECK(recorder.calls == 1 && recorder.last.error == ErrorCode::Busy &&
                  !recorder.onWorker);
            CHECK(handle.Value().State() == BulkReadState::Failed);
        }

        CHECK(stream.io.opens == 0);
        Recorder empty;
        auto none = opened.Value().Read(0, Span<uint8>(), BulkReadPriority::Normal, Record, &empty);
        REQUIRE(none);
        CHECK(empty.calls == 1 && empty.last.error == ErrorCode::None);
        stream.rig.worker.failRun = ErrorCode::None;
    }

    auto reopened = manager.OpenStreamedBulk(lease, pixels);
    REQUIRE(reopened);
    uint8 byte = 0;
    Recorder warm;
    auto warmHandle =
        reopened.Value().Read(0, Span<uint8>(&byte, 1), BulkReadPriority::Normal, Record, &warm);
    REQUIRE(warmHandle);
    stream.Drain();
    CHECK(warm.last.error == ErrorCode::None && stream.io.opens == 1);

    stream.io.hold = true;
    uint8 buffer[8] = {};
    Recorder verify;
    auto verifyHandle = reopened.Value().Read(BlockBytes - 4, Span<uint8>(buffer, 8),
                                              BulkReadPriority::Normal, Record, &verify);
    REQUIRE(verifyHandle && stream.io.held.Size() == 1);
    stream.rig.worker.failRun = ErrorCode::Busy;
    stream.io.Complete();
    stream.rig.worker.failRun = ErrorCode::None;
    CHECK(verify.calls == 1 && verify.last.error == ErrorCode::Busy && stream.io.active == 0);
    CHECK(verifyHandle.Value().State() == BulkReadState::Failed);

    // IO 결과가 이미 실패면 제출 오류가 아니라 그 오류다.
    TestStreamedIo::File* file = stream.io.Find(stream.rig.scene.SidecarPathOf(1));
    REQUIRE(file != nullptr);
    file->readError = ErrorCode::IoError;
    Recorder ioFailed;
    auto ioFailedHandle = reopened.Value().Read(0, Span<uint8>(buffer, 8), BulkReadPriority::Normal,
                                                Record, &ioFailed);
    REQUIRE(ioFailedHandle && stream.io.held.Size() == 1);
    stream.rig.worker.failRun = ErrorCode::Busy;
    stream.io.Complete();
    stream.rig.worker.failRun = ErrorCode::None;
    file->readError = ErrorCode::None;
    CHECK(ioFailed.calls == 1 && ioFailed.last.error == ErrorCode::IoError &&
          stream.io.active == 0);
}

// 취소 경합(AS 20.6 7·11): io.Read가 도는 동안 들어온 취소는 Read가 돌아온 뒤 IO 취소 한 번으로
// 가고 결과는 Cancelled다. IO가 끝났지만 검증 작업 전인 요청의 취소는 IO 취소를 부르되(구현은
// 무동작) 결과는 원래 결과(Done)이고 IO 자원은 돌려준다.
TEST(StreamedBulk_CancelRaces) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StreamRig stream(env);
    REQUIRE(stream.Start(true, 1));
    AssetLease lease;
    StreamedBulk bulk = stream.Open(1, lease);
    REQUIRE(bulk.IsValid());
    uint8 bytes[3] = {};
    {
        Recorder warm;
        REQUIRE(bulk.Read(0, Span<uint8>(bytes, 1), BulkReadPriority::Normal, Record, &warm));
        stream.Drain();
    }

    // A가 끝나면 큐의 B가 제출되고, B의 io.Read 안에서 B를 취소한다.
    stream.io.hold = true;
    Recorder first;
    Recorder second;
    auto firstHandle =
        bulk.Read(0, Span<uint8>(&bytes[0], 1), BulkReadPriority::Normal, Record, &first);
    auto secondHandle =
        bulk.Read(1, Span<uint8>(&bytes[1], 1), BulkReadPriority::Normal, Record, &second);
    REQUIRE(firstHandle && secondHandle && stream.io.held.Size() == 1);
    struct Hook {
        BulkReadHandle* handle;
        TestStreamedIo* io;
        uint32 cancelsInRead;
    } hook{&secondHandle.Value(), &stream.io, 0};
    stream.io.onReadContext = &hook;
    stream.io.onRead = [](void* context) noexcept {
        auto& state = *static_cast<Hook*>(context);
        state.io->onRead = nullptr;
        state.handle->Cancel();
        // 제출 중이라 IO 취소는 아직 가지 않는다.
        state.cancelsInRead = state.io->cancels;
    };
    stream.io.Complete(1);
    CHECK(first.calls == 1 && first.last.error == ErrorCode::None);
    CHECK(hook.cancelsInRead == 0 && stream.io.cancels == 1 && stream.io.held.Size() == 1);
    stream.io.Complete();
    CHECK(second.calls == 1 && second.last.error == ErrorCode::Cancelled &&
          secondHandle.Value().State() == BulkReadState::Cancelled);

    // 완료 뒤·검증 전 취소.
    stream.rig.worker.hold = true;
    Recorder late;
    auto lateHandle =
        bulk.Read(2, Span<uint8>(&bytes[2], 1), BulkReadPriority::Normal, Record, &late);
    REQUIRE(lateHandle && stream.io.held.Size() == 1);
    stream.io.Complete();
    REQUIRE(stream.rig.worker.pending.Size() == 1);
    lateHandle.Value().Cancel();
    CHECK(stream.io.cancels == 2 && late.calls == 0);
    stream.RunWorker();
    CHECK(late.calls == 1 && late.last.error == ErrorCode::None && bytes[2] == stream.expected[2]);
    CHECK(lateHandle.Value().State() == BulkReadState::Done && stream.io.active == 0);
}

// lease·스레드 규칙(AS 20.6 3·9): scope Revoke 뒤에도 연 StreamedBulk는 읽고, 회수된 lease로 여는
// 것은 StaleHandle이다. Game 스레드가 아닌 스레드의 OpenStreamedBulk는 WrongThread다. streamedIo가
// 있는데 진행 상한이 0이면 Initialize가 InvalidArgument다.
TEST(StreamedBulk_LeaseAndThreadRules) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StreamRig stream(env);
    REQUIRE(stream.Start());
    AssetManager& manager = *stream.rig.manager;
    auto scope = manager.CreateScope();
    REQUIRE(scope);
    AssetRequest request;
    request.id = FixedId(1);
    request.scope = &scope.Value();
    auto loaded = manager.LoadBlocking(request, "StreamedBulkTests");
    REQUIRE(loaded);
    AssetLease lease = Move(loaded.Value());
    const BulkDataRef pixels = stream.PixelsOf(lease);
    auto opened = manager.OpenStreamedBulk(lease, pixels);
    REQUIRE(opened);
    REQUIRE(scope.Value().Revoke());
    CHECK(!lease.IsValid());
    REQUIRE(stream.rig.Pump());
    CHECK(manager.OpenStreamedBulk(lease, pixels).Error() == ErrorCode::StaleHandle);
    uint8 byte = 0;
    Recorder recorder;
    auto handle =
        opened.Value().Read(9, Span<uint8>(&byte, 1), BulkReadPriority::Normal, Record, &recorder);
    REQUIRE(handle);
    stream.Drain();
    CHECK(recorder.calls == 1 && recorder.last.error == ErrorCode::None &&
          byte == stream.expected[9]);

    AssetLease other = stream.Load(1);
    REQUIRE(other.IsValid());
    struct OffThread {
        AssetManager* manager;
        AssetLease* lease;
        BulkDataRef pixels;
        ErrorCode error;
    } off{&manager, &other, pixels, ErrorCode::None};
    platform::Thread thread;
    REQUIRE(thread.Start(
                [](void* context) noexcept {
                    auto& state = *static_cast<OffThread*>(context);
                    auto result = state.manager->OpenStreamedBulk(*state.lease, state.pixels);
                    state.error = result ? ErrorCode::None : result.Error();
                },
                &off) == platform::ThreadStatus::Success);
    (void)thread.Join();
    CHECK(off.error == ErrorCode::WrongThread);

    AssetEnv zeroEnv;
    REQUIRE(zeroEnv.Ready());
    StreamRig zero(zeroEnv);
    CHECK(!zero.Start(true, 0));
    REQUIRE(zero.rig.manager);
    CHECK(zero.rig.manager->Initialize().Error() == ErrorCode::InvalidArgument);
}

namespace {

// 디스크 읽기 하나의 기록이다(callback이 done을 알린다 — 명시 gate).
struct DiskRead {
    Span<uint8> target{};
    uint64 offset = 0;
    platform::ManualResetEvent done;
    BulkReadResult result;
    uint32 calls = 0;
};

void RecordDisk(void* context, const BulkReadResult& result) noexcept {
    auto& read = *static_cast<DiskRead*>(context);
    ++read.calls;
    read.result = result;
    read.done.Signal();
}

// 읽고 callback까지 기다린다(callback이 정확히 한 번이 아니면 시험이 쓰지 않는 값).
ErrorCode ReadOnDisk(StreamedBulk& bulk, DiskRead& read) noexcept {
    auto handle = bulk.Read(read.offset, read.target, BulkReadPriority::Normal, RecordDisk, &read);
    if (!handle) {
        return handle.Error();
    }

    (void)read.done.Wait();
    return read.calls == 1 ? read.result.error : ErrorCode::AlreadyExists;
}

} // namespace

// 디스크 IO(AS 20.6 1·6 — AsyncStreamedBulkIo + WorkerSchedulerLoaderWorker): 임시 디렉터리에 쓴
// sidecar를 실제 비동기 IO로 읽는다. 블록 경계를 넘는 범위와 전체가 byte까지 맞고, 4 스레드가 시작
// gate를 함께 지나 동시에 읽어도 모두 Done이다. IO 완료 오류 주입은 그 오류다. 블록 2의 byte
// 하나를 바꿔 쓴 다른 sidecar(에셋 2)는 그 블록을 덮는 읽기만 CorruptData다. 읽기를 건 채
// Shutdown하면 callback은 한 번(Done 또는 Cancelled)이다.
TEST(StreamedBulk_DiskIo) {
    AssetEnv env;
    REQUIRE(env.Ready());
    char root[96] = {};
    const int count = snprintf(root, sizeof(root), "rocks-streamed-bulk-%u",
                               static_cast<unsigned>(platform::CurrentProcessId()));
    REQUIRE(count > 0 && static_cast<usize>(count) < sizeof(root));
    REQUIRE(platform::MakeDirectoryTree(platform::PhysicalFileSystem(), root,
                                        static_cast<usize>(count)));
    auto removeRoot = MakeScopeExit([&]() noexcept {
        CHECK(platform::DeleteDirectoryTree(platform::PhysicalFileSystem(), root,
                                            static_cast<usize>(count)));
    });
    platform::AsyncIoContext io;
    REQUIRE(io.Start(platform::DefaultAsyncIoBackend()));
    WorkerScheduler scheduler(env.allocator);
    WorkerSchedulerConfig workers;
    workers.workerCount = 2;
    REQUIRE(scheduler.Start(workers));
    {
        AsyncStreamedBulkIo files(env.allocator, io);
        WorkerSchedulerLoaderWorker worker(scheduler, env.allocator);
        StreamRig stream(env);
        stream.rig.scene.root = StringView(root, static_cast<usize>(count));
        stream.streamedIo = &files;
        stream.worker = &worker;
        REQUIRE(stream.Start());
        const StringView sidecarPath = stream.rig.scene.SidecarPathOf(1);
        const Array<uint8>& sidecar = stream.rig.scene.SidecarOf(1);
        REQUIRE(MakeParent(sidecarPath));
        REQUIRE(WriteWholeFile(sidecarPath, sidecar.View()));
        Array<uint8> damaged(env.allocator);
        REQUIRE(damaged.TryAppend(stream.rig.scene.SidecarOf(2).View()));
        const uint64 damagedAt = BlockBytes * 2 + 7;
        damaged[static_cast<usize>(damagedAt)] ^= 1;
        const StringView damagedPath = stream.rig.scene.SidecarPathOf(2);
        REQUIRE(MakeParent(damagedPath));
        REQUIRE(WriteWholeFile(damagedPath, damaged.View()));

        AssetLease lease = stream.Load(1);
        REQUIRE(lease.IsValid());
        const BulkDataRef pixels = stream.PixelsOf(lease);
        Array<uint8> whole(env.allocator);
        REQUIRE(whole.TryResize(BigBytes));
        uint8 edge[64] = {};
        constexpr uint32 ThreadCount = 4;
        constexpr usize ThreadBytes = 4000;
        uint8 threadBytes[ThreadCount][ThreadBytes] = {};
        DiskRead wholeRead;
        DiskRead edgeRead;
        DiskRead threadReads[ThreadCount];
        DiskRead faultRead;
        DiskRead damagedRead;
        DiskRead cleanRead;
        DiskRead lastRead;
        {
            auto opened = stream.rig.manager->OpenStreamedBulk(lease, pixels);
            REQUIRE(opened);
            StreamedBulk& bulk = opened.Value();
            wholeRead.target = whole.View();
            CHECK(ReadOnDisk(bulk, wholeRead) == ErrorCode::None);
            CHECK(stream.Matches(whole.View(), 0));
            edgeRead.target = Span<uint8>(edge, sizeof(edge));
            edgeRead.offset = BlockBytes - 32;
            CHECK(ReadOnDisk(bulk, edgeRead) == ErrorCode::None);
            CHECK(stream.Matches(Span<const uint8>(edge, sizeof(edge)), BlockBytes - 32));

            // 동시 읽기: 모든 스레드가 시작 gate를 지난 뒤 읽고 자기 callback을 기다린다.
            platform::ManualResetEvent start;
            struct ThreadRead {
                StreamedBulk* bulk;
                platform::ManualResetEvent* start;
                DiskRead* read;
                ErrorCode error;
            };
            ThreadRead threadStates[ThreadCount];
            platform::Thread threads[ThreadCount];
            for (uint32 index = 0; index < ThreadCount; ++index) {
                threadReads[index].target = Span<uint8>(threadBytes[index], ThreadBytes);
                threadReads[index].offset = BlockBytes * (index % 3) + 1000 * index;
                threadStates[index] =
                    ThreadRead{&bulk, &start, &threadReads[index], ErrorCode::AlreadyExists};
                REQUIRE(threads[index].Start(
                            [](void* context) noexcept {
                                auto& state = *static_cast<ThreadRead*>(context);
                                (void)state.start->Wait();
                                state.error = ReadOnDisk(*state.bulk, *state.read);
                            },
                            &threadStates[index]) == platform::ThreadStatus::Success);
            }

            start.Signal();
            for (uint32 index = 0; index < ThreadCount; ++index) {
                (void)threads[index].Join();
                CHECK(threadStates[index].error == ErrorCode::None);
                CHECK(stream.Matches(Span<const uint8>(threadBytes[index], ThreadBytes),
                                     threadReads[index].offset));
            }

#if !ROCKS_BUILD_RELEASE
            const platform::FileStatus injected{platform::FileCode::SystemError, 5};
            io.InjectFault({platform::AsyncIoFaultPoint::Completion, injected, 0});
            faultRead.target = Span<uint8>(edge, 1);
            CHECK(ReadOnDisk(bulk, faultRead) == FileErrorCode(injected));
            io.ClearFault();
#endif
        }

        AssetLease damagedLease = stream.Load(2);
        REQUIRE(damagedLease.IsValid());
        {
            auto opened =
                stream.rig.manager->OpenStreamedBulk(damagedLease, stream.PixelsOf(damagedLease));
            REQUIRE(opened);
            damagedRead.target = Span<uint8>(edge, 1);
            damagedRead.offset = damagedAt - FirstEntryOffset;
            CHECK(ReadOnDisk(opened.Value(), damagedRead) == ErrorCode::CorruptData);
            cleanRead.target = Span<uint8>(edge, 1);
            CHECK(ReadOnDisk(opened.Value(), cleanRead) == ErrorCode::None);
        }

        // 진행 중 Shutdown(실제 IO 취소·Release 순서): 전체 읽기를 건 채 Shutdown하면 callback은
        // 한 번이고 결과는 Done(이미 끝남) 또는 Cancelled다. Shutdown은 모든 callback이 돌아온 뒤에
        // 돌아오므로 그 뒤 기록을 지워도 된다(ManualResetEvent는 caller가 끝난 뒤 파괴).
        {
            auto opened = stream.rig.manager->OpenStreamedBulk(lease, pixels);
            REQUIRE(opened);
            lastRead.target = whole.View();
            auto pending = opened.Value().Read(0, lastRead.target, BulkReadPriority::Normal,
                                               RecordDisk, &lastRead);
            REQUIRE(pending);
            lease = AssetLease();
            damagedLease = AssetLease();
            REQUIRE(stream.rig.manager->Shutdown());
            CHECK(lastRead.calls == 1 && (lastRead.result.error == ErrorCode::None ||
                                          lastRead.result.error == ErrorCode::Cancelled));
            CHECK(pending.Value().State() != BulkReadState::Pending);
        }

        stream.rig.manager.Reset();
    }

    REQUIRE(scheduler.Shutdown());
    CHECK(io.Shutdown());
}

namespace {

int StreamedBulkFatalProbe(int argc, const char* const* argv) noexcept {
    rocks::test::PrepareFatalTestProcess();
    constexpr int ScenarioArgument = 3;
    if (argc <= ScenarioArgument) {
        return 5;
    }

    AssetEnv env;
    if (!env.Ready()) {
        return 2;
    }

    StreamRig stream(env);
    if (!stream.Start()) {
        return 3;
    }

    AssetLease lease;
    StreamedBulk bulk = stream.Open(1, lease);
    if (!bulk.IsValid()) {
        return 4;
    }

    if (strcmp(argv[ScenarioArgument], "pending-handle") == 0) {
        stream.io.hold = true;
        uint8 byte = 0;
        Recorder recorder;
        auto handle =
            bulk.Read(0, Span<uint8>(&byte, 1), BulkReadPriority::Normal, Record, &recorder);
        if (!handle) {
            return 4;
        }

        // Pending 손잡이를 파괴한다.
        BulkReadHandle doomed = Move(handle.Value());
        (void)doomed;
        return 0;
    }

    // 살아 있는 StreamedBulk를 남기고 AssetManager를 파괴한다.
    lease = AssetLease();
    stream.rig.manager.Reset();
    return 0;
}

const bool StreamedBulkProbeRegistered =
    rocks::test::RegisterProcessProbe("streamed-bulk", &StreamedBulkFatalProbe);

bool StreamedBulkProbeFails(const char* scenario, const char* message) noexcept {
    char executable[4096];
    usize executableBytes = 0;
    if (!platform::GetExecutablePath(executable, sizeof(executable), executableBytes)) {
        return false;
    }

    const platform::ProcessArgument arguments[] = {
        {"--process-probe", 15}, {"streamed-bulk", 13}, {scenario, strlen(scenario)}};
    platform::ProcessStartInfo info;
    info.executable = executable;
    info.executableBytes = executableBytes;
    info.arguments = arguments;
    info.argumentCount = 3;
    info.output = platform::ProcessStreamMode::Null;
    info.error = platform::ProcessStreamMode::Pipe;
    static char error[4096];
    platform::ProcessRunOptions options;
    options.error = error;
    options.errorCapacity = sizeof(error) - 1;
    const platform::ProcessRunResult run = platform::RunProcess(info, options);
    if (!run.status || run.timedOut) {
        return false;
    }

    error[run.errorBytes < sizeof(error) ? run.errorBytes : sizeof(error) - 1] = '\0';
    const bool abnormal = run.exit.kind != platform::ProcessExitKind::Exited || run.exit.code != 0;
    return abnormal && strstr(error, message) != nullptr;
}

} // namespace

// 수명 위반(AS 20.6 7·10): Pending 손잡이 소멸, 살아 있는 StreamedBulk를 남긴 AssetManager 파괴는
// 자식 프로세스가 Fatal로 끝나고 그 문구를 남긴다.
TEST(StreamedBulk_LifetimeViolationsAreFatal) {
    REQUIRE(StreamedBulkProbeRegistered);
    CHECK(StreamedBulkProbeFails("pending-handle", "BulkReadHandle destroyed while its read"));
    CHECK(StreamedBulkProbeFails("live-bulk", "streamed bulk handles are alive"));
}

} // namespace streamed_bulk_tests_file
