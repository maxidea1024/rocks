#include "../../AssetRuntime/Tests/ManagerTestSupport.h"
#include "../Private/StreamedBulkPayload.h"
#include "AudioEngineTestSupport.h"

#include <AssetRuntime/StreamedBulk.h>
#include <AudioAssets/AudioAssetsTypes.h>
#include <AudioAssets/SoundClip.generated.h>
#include <AudioAssets/SoundStreamData.h>
#include <AudioTools/AudioEncoder.h>
#include <AudioTools/AudioPlatformDataWriter.h>
#include <Platform/Process.h>
#include <TestSupport/HostFailure.h>
#include <TestSupport/NFailAllocator.h>

#include <string.h>

// AU6b1 엔진 쪽 sidecar 시험이다(Docs/Design/AudioSystem 3.46.3·3.46.8절): StreamedBulkPayload
// 어댑터를 실제 AssetManager(gate IO — 완료를 시험이 내보낸다)의 sidecar bulk에 씌워 범위 읽기·
// callback 정확히 한 번·상태·즉시 실패·취소·callback 안 손잡이 파괴·Pending 소멸 Fatal을 보고,
// LoadCookedClip이 같은 쿠킹 데이터면 sidecar를 열지 않고, 열기 오류를 넘기며, Unsupported(패키지
// 안 bulk)면 상주 경로로 가는지 본다. 시간 대기 없음.
using namespace rocks;

namespace rocks::test {
bool RegisterProcessProbe(const char* mode,
                          int (*function)(int, const char* const*) noexcept) noexcept;
} // namespace rocks::test

namespace audio_sidecar_tests_file {

using namespace rocks::asset_test;

namespace {

// sidecar bulk 크기다(블록 경계를 넘는다).
constexpr uint32 SidecarBytes = 150000;
constexpr uint32 ResidentBytes = 2000;
constexpr uint64 SidecarAsset = 1;
constexpr uint64 ResidentAsset = 2;
constexpr uint32 SilentRate = 48000;

// 완료를 시험이 내보내는 범위 IO다(hold면 쌓아 두고 Complete가 내보낸다).
class GateStreamedIo final : public StreamedBulkIo {
public:
    struct File {
        explicit File(Allocator& allocator) noexcept : path(allocator), bytes(allocator) {
        }

        String path;
        Array<uint8> bytes;
    };

    struct Held {
        StreamedBulkIoRead* read = nullptr;
        File* file = nullptr;
        bool cancelled = false;
    };

    explicit GateStreamedIo(Allocator& allocator) noexcept : files(allocator), held(allocator) {
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

    Result<StreamedBulkFile*> Open(const char* path, usize pathBytes, Span<uint8> header,
                                   uint64& fileBytes) noexcept override {
        for (File& file : files) {
            if (file.path.View() != StringView(path, pathBytes)) {
                continue;
            }

            fileBytes = file.bytes.Size();
            for (usize index = 0; index < header.Size(); ++index) {
                header[index] = index < file.bytes.Size() ? file.bytes[index] : 0;
            }

            return Result<StreamedBulkFile*>::Success(reinterpret_cast<StreamedBulkFile*>(&file));
        }

        return Result<StreamedBulkFile*>::Failure(ErrorCode::NotFound);
    }

    Result<void> Read(StreamedBulkFile& opened, StreamedBulkIoRead& read) noexcept override {
        const Held entry{&read, reinterpret_cast<File*>(&opened), false};
        if (hold) {
            (void)held.TryPushBack(entry);
            return Result<void>::Success();
        }

        Finish(entry);
        return Result<void>::Success();
    }

    void Cancel(StreamedBulkIoRead& read) noexcept override {
        for (Held& entry : held) {
            if (entry.read == &read) {
                entry.cancelled = true;
            }
        }
    }

    void Release(StreamedBulkIoRead&) noexcept override {
    }

    void Close(StreamedBulkFile*) noexcept override {
    }

    void Complete() noexcept {
        while (!held.IsEmpty()) {
            const Held entry = held[0];
            (void)held.TryRemoveAt(0);
            Finish(entry);
        }
    }

    Array<File> files;
    Array<Held> held;
    bool hold = false;

private:
    static constexpr usize MaxFiles = 4;

    static void Finish(const Held& entry) noexcept {
        StreamedBulkIoRead& read = *entry.read;
        StreamedBulkIoResult result;
        if (entry.cancelled) {
            result.error = ErrorCode::Cancelled;
        } else {
            const Array<uint8>& bytes = entry.file->bytes;
            const uint64 available =
                read.offset >= bytes.Size()
                    ? 0
                    : (bytes.Size() - read.offset < read.bytes ? bytes.Size() - read.offset
                                                               : read.bytes);
            memcpy(read.destination, bytes.Data() + read.offset, static_cast<usize>(available));
            result.bytes = available;
        }

        read.complete(read, result);
    }
};

// 장면: SidecarAsset(sidecar bulk), ResidentAsset(패키지 안 bulk)이다.
struct SidecarRig {
    explicit SidecarRig(AssetEnv& sceneEnv) noexcept
        : env(sceneEnv), rig(sceneEnv), io(sceneEnv.allocator), expected(sceneEnv.allocator) {
    }

    bool Start() noexcept {
        rig.scene.Add(SidecarAsset, SidecarBytes).sidecar = true;
        rig.scene.Add(ResidentAsset, ResidentBytes);
        if (!rig.Cook()) {
            return false;
        }

        for (usize index = 0; index < rig.scene.FileCount(); ++index) {
            if (!rig.scene.SidecarAt(index).IsEmpty() &&
                !io.Add(rig.scene.SidecarPathAt(index), rig.scene.SidecarAt(index).View())) {
                return false;
            }
        }

        rig.config.streamedIo = &io;
        FillCompressible(expected, SidecarBytes, static_cast<uint32>(SidecarAsset));
        return rig.Start();
    }

    AssetLease Load(uint64 id) noexcept {
        AssetRequest request;
        request.id = FixedId(id);
        auto lease = rig.manager->LoadBlocking(request, "AudioSidecarTests");
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

    const Package* PackageOf(uint64 id) noexcept {
        auto package = rig.manager->Loader().FindPackage(FixedId(id));
        return package ? Pointer<Package>(rig.manager->Registry(), package.Value()) : nullptr;
    }

    // sidecar bulk를 어댑터로 연다(실패면 null).
    SharedPtr<SoundStreamedPayload> OpenPayload(AssetLease& lease) noexcept {
        lease = Load(SidecarAsset);
        auto bulk = rig.manager->OpenStreamedBulk(lease, PixelsOf(lease));
        if (!bulk) {
            return SharedPtr<SoundStreamedPayload>{};
        }

        auto made = engine_audio_detail::MakeStreamedBulkPayload(env.allocator, Move(bulk.Value()));
        return made ? Move(made.Value()) : SharedPtr<SoundStreamedPayload>{};
    }

    // IO와 worker를 비울 때까지 내보낸다.
    void Drain() noexcept {
        for (uint32 round = 0; round < 64 && (!io.held.IsEmpty() || !rig.worker.pending.IsEmpty());
             ++round) {
            io.Complete();
            rig.worker.RunAll();
        }
    }

    AssetEnv& env;
    ManagerRig rig;
    GateStreamedIo io;
    Array<uint8> expected;
};

// callback 기록이다. destroy가 있으면 callback 안에서 그 손잡이를, release가 있으면 그 payload
// 참조를 놓는다.
struct ReadRecord {
    uint32 calls = 0;
    ErrorCode error = ErrorCode::None;
    SoundPayloadRead* destroy = nullptr;
    SharedPtr<SoundStreamedPayload>* release = nullptr;
};

void RecordRead(void* context, ErrorCode error) noexcept {
    auto& record = *static_cast<ReadRecord*>(context);
    ++record.calls;
    record.error = error;
    if (record.destroy != nullptr) {
        record.destroy->Reset();
    }

    if (record.release != nullptr) {
        record.release->Reset();
    }
}

} // namespace

// 범위 읽기(3.46.3절): 어댑터가 StreamedBulk 크기를 주고, 블록 경계를 넘는 범위가 Pending → 완료
// 뒤 Done이고 callback이 정확히 한 번(None)이며 byte가 맞다. SetPriority는 callback을 부르지
// 않는다. 즉시 실패(범위 넘침·null callback)는 반환값이고 callback이 없다. 제출 전 Cancel은
// Cancelled callback 한 번이다. callback 안에서 자기 손잡이를 파괴해도 된다.
TEST(EngineAudio_StreamedBulkPayloadReads) {
    AssetEnv env;
    REQUIRE(env.Ready());
    SidecarRig stream(env);
    REQUIRE(stream.Start());
    AssetLease lease;
    SharedPtr<SoundStreamedPayload> payload = stream.OpenPayload(lease);
    REQUIRE(payload);
    CHECK(payload->Bytes() == SidecarBytes);
    stream.rig.worker.hold = true;
    stream.io.hold = true;

    Array<uint8> buffer(env.allocator);
    REQUIRE(buffer.TryResize(SidecarBytes / 2));
    constexpr uint64 Offset = SidecarBytes / 4;
    ReadRecord record;
    auto read =
        payload->Read(Offset, buffer.View(), SoundReadPriority::Normal, &RecordRead, &record);
    REQUIRE(read);
    CHECK(read.Value()->State() == SoundReadState::Pending && record.calls == 0);
    read.Value()->SetPriority(SoundReadPriority::Critical);
    CHECK(record.calls == 0);
    stream.Drain();
    CHECK(record.calls == 1 && record.error == ErrorCode::None);
    CHECK(read.Value()->State() == SoundReadState::Done);
    CHECK(SameBytes(buffer.View(),
                    stream.expected.View().Slice(static_cast<usize>(Offset), buffer.Size())));

    // 즉시 실패: callback이 없다.
    uint8 one = 0;
    ReadRecord rejected;
    CHECK(payload
              ->Read(SidecarBytes, Span<uint8>(&one, 1), SoundReadPriority::High, &RecordRead,
                     &rejected)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(payload->Read(0, Span<uint8>(&one, 1), SoundReadPriority::High, nullptr, &rejected)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(rejected.calls == 0);

    // 제출 전 취소(worker가 쥐고 있다): Cancelled 한 번.
    ReadRecord cancelled;
    auto pending =
        payload->Read(0, Span<uint8>(&one, 1), SoundReadPriority::Low, &RecordRead, &cancelled);
    REQUIRE(pending);
    pending.Value()->Cancel();
    stream.Drain();
    CHECK(cancelled.calls == 1 && cancelled.error == ErrorCode::Cancelled);
    CHECK(pending.Value()->State() == SoundReadState::Cancelled);

    // callback 안에서 자기 손잡이를 파괴한다.
    ReadRecord destroying;
    auto self =
        payload->Read(7, Span<uint8>(&one, 1), SoundReadPriority::High, &RecordRead, &destroying);
    REQUIRE(self);
    SoundPayloadRead owned = Move(self.Value());
    destroying.destroy = &owned;
    stream.Drain();
    CHECK(destroying.calls == 1 && destroying.error == ErrorCode::None && !owned);
    CHECK(one == stream.expected[7]);

    // Read가 돌아오기 전 완료(IO·worker가 그 자리에서 끝낸다): 손잡이를 받을 때 이미 Done이다.
    stream.rig.worker.hold = false;
    stream.io.hold = false;
    ReadRecord inlineRecord;
    auto finished = payload->Read(SidecarBytes - 1, Span<uint8>(&one, 1), SoundReadPriority::High,
                                  &RecordRead, &inlineRecord);
    REQUIRE(finished);
    CHECK(inlineRecord.calls == 1 && inlineRecord.error == ErrorCode::None);
    CHECK(finished.Value()->State() == SoundReadState::Done);
    CHECK(one == stream.expected[SidecarBytes - 1]);

    // callback 안에서 payload의 마지막 참조를 놓는다(손잡이는 StreamedBulk보다 오래 산다).
    stream.rig.worker.hold = true;
    stream.io.hold = true;
    ReadRecord releasing;
    releasing.release = &payload;
    auto last =
        payload->Read(3, Span<uint8>(&one, 1), SoundReadPriority::Normal, &RecordRead, &releasing);
    REQUIRE(last);
    stream.Drain();
    CHECK(releasing.calls == 1 && releasing.error == ErrorCode::None && !payload);
    CHECK(last.Value()->State() == SoundReadState::Done && one == stream.expected[3]);
}

// 할당 실패(3.46.3절): 손잡이·중계 할당 실패는 즉시 OutOfMemory이고 callback이 없다. 실패가 풀리면
// 같은 payload로 읽는다.
TEST(EngineAudio_StreamedBulkPayloadAllocationFailure) {
    AssetEnv env;
    REQUIRE(env.Ready());
    SidecarRig stream(env);
    REQUIRE(stream.Start());
    AssetLease lease = stream.Load(SidecarAsset);
    auto bulk = stream.rig.manager->OpenStreamedBulk(lease, stream.PixelsOf(lease));
    REQUIRE(bulk);
    test::NFailAllocator failing(env.allocator);
    auto made = engine_audio_detail::MakeStreamedBulkPayload(failing, Move(bulk.Value()));
    REQUIRE(made);
    SharedPtr<SoundStreamedPayload> payload = Move(made.Value());
    uint8 one = 0;
    ReadRecord record;
    // 1: 손잡이, 2: 중계.
    for (uint64 attempt = 1; attempt <= 2; ++attempt) {
        failing.FailOnAttempt(attempt);
        CHECK(
            payload->Read(0, Span<uint8>(&one, 1), SoundReadPriority::Normal, &RecordRead, &record)
                .Error() == ErrorCode::OutOfMemory);
    }

    stream.rig.worker.hold = true;
    stream.io.hold = true;
    CHECK(record.calls == 0);
    failing.DisableFailure();
    auto read =
        payload->Read(0, Span<uint8>(&one, 1), SoundReadPriority::Normal, &RecordRead, &record);
    REQUIRE(read);
    stream.Drain();
    CHECK(record.calls == 1 && record.error == ErrorCode::None && one == stream.expected[0]);
}

namespace {

// 1 s mono PCM16 플랫폼 데이터(무음)다.
Array<uint8> SilentPlatformData(Allocator& allocator, uint32 frames) noexcept {
    Array<uint8> bytes(allocator);
    Array<float> samples(allocator);
    if (!samples.TryResize(frames)) {
        return bytes;
    }

    audio::PlanarConstView view;
    view.channelCount = 1;
    view.frameCount = frames;
    view.channels[0] = samples.Data();
    audio::Pcm16AudioEncoder encoder;
    audio::EncodedAudio encoded(allocator);
    if (!encoder.Encode(view, SilentRate, audio::DefaultAudioChannelMask(1), {}, encoded) ||
        !audio::WriteAudioPlatformData(encoded, {}, bytes)) {
        bytes.Clear();
    }

    return bytes;
}

// 고정 byte를 돌려준다(context = Array<uint8>).
Result<Span<const uint8>> ReadFixed(void* context, const BulkDataRef&) noexcept {
    return Result<Span<const uint8>>::Success(static_cast<const Array<uint8>*>(context)->View());
}

} // namespace

// LoadCookedClip(3.46.3절): 같은 쿠킹 데이터면 아무것도 하지 않는다(무효 lease여도 sidecar를 열지
// 않는다). 아니면 streamedPayload를 연다 — 무효 lease는 StaleHandle을 넘긴다. 패키지 안 bulk
// (Unsupported)면 상주 경로로 가서 reader 결과를 본다(시험 bulk는 오디오 데이터가 아니라
// CorruptData).
TEST(EngineAudio_LoadCookedClipOpensSidecar) {
    AssetEnv env;
    REQUIRE(env.Ready());
    SidecarRig stream(env);
    REQUIRE(stream.Start());
    engine_audio_test::AudioEngine fixture(engine_audio_test::DeterministicOptions(false));
    REQUIRE(fixture.initialized);
    AssetLease resident = stream.Load(ResidentAsset);
    REQUIRE(resident.IsValid());
    const BulkDataRef residentBulk = stream.PixelsOf(resident);
    const Package* package = stream.PackageOf(ResidentAsset);
    REQUIRE(package != nullptr);

    TypeRegistry types(env.allocator);
    ModuleRegistration registration(types, env.allocator);
    REQUIRE(RegisterAudioAssetsTypes(registration));
    ObjectRegistry objects(env.allocator, types, {0x4155364231534344ull, 1});
    auto made = objects.NewObject<SoundClip>(objects.OwnerAccess(), TypeIdOf<SoundClip>());
    REQUIRE(made);
    auto borrowed =
        objects.ResolveForWrite<SoundClip>(objects.OwnerAccess(), made.Value().Handle());
    REQUIRE(borrowed);
    SoundClip& clip = *borrowed.Value().Get().Value();
    clip.platformData = residentBulk;
    clip.streamedPayload = residentBulk;
    clip.channelCount = 1;
    clip.sampleRate = SilentRate;
    clip.frameCount = SilentRate;
    AssetManager& assets = *stream.rig.manager;

    // 열기 오류는 그대로다(무효 lease).
    CHECK(fixture.audio->LoadCookedClip(clip, *package, assets, AssetLease()).Error() ==
          ErrorCode::StaleHandle);
    // 패키지 안 bulk는 Unsupported → 상주 경로(reader가 준 byte가 플랫폼 데이터가 아니다).
    CHECK(fixture.audio->LoadCookedClip(clip, *package, assets, resident).Error() ==
          ErrorCode::CorruptData);
    CHECK(!clip.HasCookedData() && !clip.Stream());

    // 같은 쿠킹 데이터(키 = platformData·차원)를 이미 올렸으면 열지 않는다: 무효 lease여도
    // 성공이다.
    clip.streamedPayload = BulkDataRef{};
    Array<uint8> bytes = SilentPlatformData(env.allocator, SilentRate);
    REQUIRE(!bytes.IsEmpty());
    REQUIRE(clip.LoadCooked(env.allocator, &ReadFixed, &bytes, fixture.audio->DecoderTable(),
                            fixture.audio->CookedLoadOptions()));
    REQUIRE(clip.HasCookedData());
    const SoundStreamData* loaded = clip.Stream().Get();
    clip.streamedPayload = residentBulk;
    CHECK(fixture.audio->LoadCookedClip(clip, *package, assets, AssetLease()));
    CHECK(clip.Stream().Get() == loaded);
}

namespace {

int SidecarPayloadFatalProbe(int, const char* const*) noexcept {
    rocks::test::PrepareFatalTestProcess();
    AssetEnv env;
    if (!env.Ready()) {
        return 2;
    }

    SidecarRig stream(env);
    if (!stream.Start()) {
        return 3;
    }

    AssetLease lease;
    SharedPtr<SoundStreamedPayload> payload = stream.OpenPayload(lease);
    if (!payload) {
        return 4;
    }

    stream.io.hold = true;
    uint8 byte = 0;
    ReadRecord record;
    auto read =
        payload->Read(0, Span<uint8>(&byte, 1), SoundReadPriority::Normal, &RecordRead, &record);
    if (!read) {
        return 4;
    }

    // Pending 손잡이를 파괴한다.
    SoundPayloadRead doomed = Move(read.Value());
    doomed.Reset();
    return 0;
}

const bool SidecarPayloadProbeRegistered =
    rocks::test::RegisterProcessProbe("audio-sidecar-payload", &SidecarPayloadFatalProbe);

bool SidecarPayloadProbeFails(const char* message) noexcept {
    char executable[4096];
    usize executableBytes = 0;
    if (!platform::GetExecutablePath(executable, sizeof(executable), executableBytes)) {
        return false;
    }

    constexpr char Mode[] = "audio-sidecar-payload";
    const platform::ProcessArgument arguments[] = {{"--process-probe", 15},
                                                   {Mode, sizeof(Mode) - 1}};
    platform::ProcessStartInfo info;
    info.executable = executable;
    info.executableBytes = executableBytes;
    info.arguments = arguments;
    info.argumentCount = 2;
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

// Pending 손잡이 소멸은 StreamedBulk 규칙 그대로 Fatal이다(자식 프로세스 — 3.46.3절).
TEST(EngineAudio_StreamedBulkPayloadPendingDestroyIsFatal) {
    REQUIRE(SidecarPayloadProbeRegistered);
    CHECK(SidecarPayloadProbeFails("BulkReadHandle destroyed while its read"));
}

} // namespace audio_sidecar_tests_file
