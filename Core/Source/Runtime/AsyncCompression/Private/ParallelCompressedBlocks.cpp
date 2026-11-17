#include "CompressedBlocksInternal.h"

#include <AsyncCompression/ParallelCompressedBlocks.h>
#include <Core/Assert.h>
#include <Core/Function.h>
#include <Core/Move.h>
#include <Platform/Atomics.h>
#include <Platform/Memory.h>
#include <Task/WorkerScheduler.h>

// 블록 컨테이너의 병렬 압축·병렬 읽기다. 블록 인코딩·해제는 Compression 모듈의 순차 경로와 같은
// 코드(CompressedBlocksInternal.h)를 쓰므로 결과 byte가 순차와 같다.
namespace rocks {

using namespace detail::blocks;

namespace {

// 블록 수가 이 값 이하이거나 원본이 ParallelMinimumBytes보다 작으면 병렬 API도 순차로 처리한다
// (helper 제출·worker 깨우기 비용이 일보다 크다).
constexpr uint64 SequentialBlockLimit = 4;
constexpr usize ParallelMinimumBytes = usize{1} << 20;
// helper 하나가 맡을 원본의 최소 크기다. helper 수는 원본 / 이 값을 넘지 않는다.
constexpr usize ParticipantMinimumBytes = usize{512} << 10;
// 병렬 job 하나의 최대 원본 크기다(취소·실패 반응성과 job 하나의 결과 메모리를 묶는다. 결과는 모든
// job이 끝날 때까지 보관하므로 전체 메모리는 압축 결과 크기만큼이다).
constexpr usize MaxJobRawBytes = usize{16} << 20;
constexpr uint32 MaxHelpers = 64;

// 병렬 job 분배다. 호출 스레드와 worker가 같은 카운터에서 job을 가져간다. 실패가 기록되면 새 job을
// 가져가지 않는다(가져간 job은 끝까지 처리한다). job은 번호 순서로만 나가므로, 실패한 job보다 앞선
// job은 모두 처리되고 가장 앞 실패가 결정적으로 정해진다.
struct JobQueue {
    uint32 jobCount = 0;
    uint32 nextJob = 0;
    uint32 failed = 0;

    bool TryClaim(uint32& job) noexcept {
        if (platform::AtomicLoad(&failed) != 0) {
            return false;
        }
        job = platform::AtomicFetchAdd(&nextJob, 1u);
        return job < jobCount;
    }

    void MarkFailed() noexcept {
        platform::AtomicStore(&failed, 1u);
    }
};

// 연속 블록을 job으로 묶는다. worker 수의 4배 정도 job을 만들되 job 하나는 MaxJobRawBytes 이하다.
uint32 BlocksPerJob(uint64 blockCount, uint32 blockSizeLog2, uint32 workers) noexcept {
    const uint64 targetJobs = (uint64{workers} + 1) * 4;
    uint64 blocks = (blockCount + targetJobs - 1) / targetJobs;
    const uint64 cap = Larger(1, MaxJobRawBytes >> blockSizeLog2);
    blocks = Smaller(Larger(blocks, 1), cap);
    return static_cast<uint32>(blocks);
}

// shared.Participate를 helper 작업들과 호출 스레드에서 실행하고 모든 helper가 끝날 때까지 기다린다.
// 제출이 실패하면 그 뒤 helper를 만들지 않는다(호출 스레드가 남은 job을 처리한다).
template <typename Shared>
void RunParticipants(WorkerScheduler& scheduler, Shared& shared, uint32 helpers,
                     Allocator& scratch) noexcept {
    WorkerTaskHandle handles[MaxHelpers];
    uint32 submitted = 0;
    while (submitted < helpers) {
        auto body = Function<void(WorkerTaskContext&)>::TryCreate(
            scratch,
            [&shared](WorkerTaskContext& context) noexcept { shared.Participate(&context); });
        if (!body) {
            break;
        }
        auto handle = scheduler.Submit(Move(body.Value()));
        if (!handle) {
            break;
        }
        handles[submitted++] = Move(handle.Value());
    }

    shared.Participate(nullptr);

    // body가 shared(호출 스레드의 stack)를 참조하므로 모든 helper가 terminal이 된 뒤 반환한다. 아직
    // worker가 꺼내지 않은 helper는 회수한다(worker가 바쁘거나 이 스레드가 쥔 lock을 기다려도
    // 끝난다). 실행 중인 helper만 기다리며, 그 helper는 가져간 job 하나(16MiB 이하)를 마치면
    // 반환한다. 호출 스레드가 worker가 아님을 미리 확인했으므로 Wait는 실패하지 않는다.
    for (uint32 index = 0; index < submitted; ++index) {
        auto revoked = handles[index].TryRevoke();
        ROCKS_ASSERT(revoked.IsOk());
        if (!revoked.Value()) {
            auto waited = handles[index].Wait(platform::InfiniteTimeout);
            ROCKS_ASSERT(waited.IsOk() && waited.Value() == platform::WaitResult::Signaled);
        }
    }
}

// helper 수다. worker 수·남은 job 수·최대치와 원본 크기(helper 하나에 ParticipantMinimumBytes
// 이상)로 제한한다.
uint32 HelperCount(WorkerScheduler& scheduler, uint32 jobCount, uint64 rawBytes) noexcept {
    uint64 helpers = scheduler.Stats().workerCount;
    helpers = Smaller(helpers, jobCount - 1);
    helpers = Smaller(helpers, MaxHelpers);
    helpers = Smaller(helpers, rawBytes / ParticipantMinimumBytes);
    return static_cast<uint32>(helpers);
}

// --- 병렬 압축 ---

struct CompressJob {
    Array<uint8> payload;
    ErrorCode error = ErrorCode::None;

    explicit CompressJob(Allocator& allocator) noexcept : payload(allocator) {
    }
};

struct CompressShared {
    JobQueue queue;
    CompressionFormat format;
    int32 level;
    uint32 blockSizeLog2;
    uint32 blockCount;
    uint32 blocksPerJob;
    Span<const uint8> source;
    uint8* table;
    CompressJob* jobs;
    Allocator* scratch;
    // 사전 블록이면 그 사전이다(호출 동안 settings가 handle을 가진다).
    const SharedPtr<detail::DictionaryState>* dictionary;

    ErrorCode RunJob(uint32 job, detail::CompressorState*& compressor) noexcept {
        Array<uint8>& payload = jobs[job].payload;
        const uint32 first = job * blocksPerJob;
        const uint32 end = blockCount - first < blocksPerJob ? blockCount : first + blocksPerJob;
        // job 결과는 원본보다 커지지 않으므로 한 번에 확보한다(블록마다 재할당하지 않는다).
        const usize jobRawBytes =
            static_cast<usize>((end == blockCount ? source.Size() : usize{end} << blockSizeLog2) -
                               (usize{first} << blockSizeLog2));
        auto reserved = payload.TryReserve(jobRawBytes);
        if (!reserved) {
            return reserved.Error();
        }
        for (uint32 block = first; block < end; ++block) {
            const usize rawBytes = BlockRawBytes(source.Size(), blockSizeLog2, blockCount, block);
            const Span<const uint8> raw(source.Data() + (usize{block} << blockSizeLog2), rawBytes);
            const usize base = payload.Size();
            auto grown = payload.TryAppendUninitialized(rawBytes);
            if (!grown) {
                return grown.Error();
            }
            auto stored =
                EncodeBlock(compressor, format, level, *scratch, raw, grown.Value(), *dictionary);
            if (!stored) {
                return stored.Error();
            }
            detail::RestoreLength(payload, base + stored.Value());
            StoreEntry(
                table, block, stored.Value(),
                detail::zlib::Crc32(Span<const uint8>(payload.Data() + base, stored.Value())));
        }
        return ErrorCode::None;
    }

    void Participate(WorkerTaskContext* context) noexcept {
        detail::CompressorState* compressor = nullptr;
        // helper는 job을 가져가기 전에 문맥을 만든다. 만들지 못하면 참여하지 않는다(호출 스레드가
        // 남은 job을 처리하므로 helper 자원 부족이 호출 전체를 실패시키지 않는다).
        if (context != nullptr && Compresses(format, level)) {
            auto created =
                detail::CreateCompressorState(format, level, *scratch, false, *dictionary);
            if (!created) {
                return;
            }
            compressor = created.Value();
        }
        uint32 job = 0;
        // worker는 취소 요청을 받으면 손을 뗀다. 호출 스레드가 남은 job을 처리한다.
        while ((context == nullptr || !context->IsCancellationRequested()) && queue.TryClaim(job)) {
            const ErrorCode error = RunJob(job, compressor);
            if (error != ErrorCode::None) {
                jobs[job].error = error;
                queue.MarkFailed();
            }
        }
        detail::DestroyCompressorState(compressor);
    }
};

// --- 병렬 읽기 ---

struct ReadShared {
    JobQueue queue;
    const CompressedBlocksReader* reader;
    const CompressedBlocksAccess::Entry* entries;
    CompressionFormat format;
    uint32 blockSizeLog2;
    uint32 firstBlock;
    uint32 lastBlock;
    uint32 blocksPerJob;
    uint64 rawOffset;
    Span<uint8> destination;
    Span<const uint8> stored;
    uint64 storedBase;
    bool verify;
    ErrorCode* errors;
    // reader의 사전이다(없으면 nullptr). 해제할 때마다 helper 문맥에 걸고 뗀다.
    const detail::DictionaryState* dictionary;
    Allocator* scratch;

    struct Worker {
        Decompressor decompressor;
        uint8* buffer = nullptr;
        bool created = false;
    };

    ErrorCode RunJob(uint32 job, Worker& worker) noexcept {
        if (!worker.created) {
            auto created = Decompressor::TryCreate(format, *scratch);
            if (!created) {
                return created.Error();
            }
            worker.decompressor = Move(created.Value());
            worker.created = true;
        }

        const uint64 rawSize = reader->RawSize();
        const uint32 count = reader->BlockCount();
        const uint64 rawEnd = rawOffset + destination.Size();
        const uint32 first = firstBlock + job * blocksPerJob;
        const uint32 end = lastBlock - first < blocksPerJob ? lastBlock + 1 : first + blocksPerJob;
        for (uint32 block = first; block < end; ++block) {
            const uint64 blockStart = uint64{block} << blockSizeLog2;
            const usize rawBytes = BlockRawBytes(rawSize, blockSizeLog2, count, block);
            const uint64 from = Larger(rawOffset, blockStart);
            const uint64 to = Smaller(rawEnd, blockStart + rawBytes);
            const Span<uint8> slice(destination.Data() + (from - rawOffset),
                                    static_cast<usize>(to - from));
            if (from == blockStart && to == blockStart + rawBytes) {
                auto decoded =
                    CompressedBlocksAccess::DecodeBlock(entries[block], stored, storedBase, verify,
                                                        worker.decompressor, slice, dictionary);
                if (!decoded) {
                    return decoded.Error();
                }
                continue;
            }

            // 읽기 범위 양 끝에서 블록 일부만 걸친다. 임시 버퍼로 해제해 필요한 부분만 복사한다.
            if (worker.buffer == nullptr) {
                worker.buffer =
                    static_cast<uint8*>(detail::CodecAllocate(*scratch, usize{1} << blockSizeLog2));
                if (worker.buffer == nullptr) {
                    return ErrorCode::OutOfMemory;
                }
            }
            auto decoded = CompressedBlocksAccess::DecodeBlock(
                entries[block], stored, storedBase, verify, worker.decompressor,
                Span<uint8>(worker.buffer, rawBytes), dictionary);
            if (!decoded) {
                return decoded.Error();
            }
            platform::MemoryCopy(slice.Data(), worker.buffer + (from - blockStart), slice.Size());
        }
        return ErrorCode::None;
    }

    void Participate(WorkerTaskContext* context) noexcept {
        Worker worker;
        // helper는 job을 가져가기 전에 해제 문맥을 만든다. 만들지 못하면 참여하지 않는다.
        if (context != nullptr) {
            auto created = Decompressor::TryCreate(format, *scratch);
            if (!created) {
                return;
            }
            worker.decompressor = Move(created.Value());
            worker.created = true;
        }
        uint32 job = 0;
        while ((context == nullptr || !context->IsCancellationRequested()) && queue.TryClaim(job)) {
            const ErrorCode error = RunJob(job, worker);
            if (error != ErrorCode::None) {
                errors[job] = error;
                queue.MarkFailed();
            }
        }
        detail::CodecFree(*scratch, worker.buffer);
    }
};

} // namespace

Result<usize> TryCompressBlocksAppend(WorkerScheduler& scheduler,
                                      const CompressedBlocksSettings& settings,
                                      Span<const uint8> source, Array<uint8>& output,
                                      Allocator& scratch) noexcept {
    if (scheduler.IsCurrentWorker()) {
        return Result<usize>::Failure(ErrorCode::WrongThread);
    }

    const uint32 blockSizeLog2 = settings.BlockSizeLog2();
    const uint64 count = BlockCountFor(source.Size(), blockSizeLog2);
    if (count <= SequentialBlockLimit || source.Size() < ParallelMinimumBytes) {
        return TryCompressBlocksAppend(settings, source, output, scratch);
    }
    if (count > MaxBlockCount) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }
    if (detail::OverlapsArrayStorage(source, output)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    const uint32 blockCount = static_cast<uint32>(count);
    const uint32 workers = scheduler.Stats().workerCount;
    const uint32 blocksPerJob = BlocksPerJob(blockCount, blockSizeLog2, workers);
    const uint32 jobCount = (blockCount + blocksPerJob - 1) / blocksPerJob;

    auto* table = static_cast<uint8*>(detail::CodecAllocate(scratch, EntryBytes * blockCount));
    auto* jobs =
        static_cast<CompressJob*>(detail::CodecAllocate(scratch, sizeof(CompressJob) * jobCount));
    if (table == nullptr || jobs == nullptr) {
        detail::CodecFree(scratch, table);
        detail::CodecFree(scratch, jobs);
        return Result<usize>::Failure(ErrorCode::OutOfMemory);
    }
    for (uint32 job = 0; job < jobCount; ++job) {
        ConstructAt<CompressJob>(jobs + job, scratch);
    }

    CompressShared shared{};
    shared.queue.jobCount = jobCount;
    shared.format = settings.Compression().Format();
    shared.level = settings.Compression().Level();
    shared.blockSizeLog2 = blockSizeLog2;
    shared.blockCount = blockCount;
    shared.blocksPerJob = blocksPerJob;
    shared.source = source;
    shared.table = table;
    shared.jobs = jobs;
    shared.scratch = &scratch;
    shared.dictionary = &CompressedBlocksSettingsAccess::Dictionary(settings);
    RunParticipants(scheduler, shared, HelperCount(scheduler, jobCount, source.Size()), scratch);

    // 가장 앞 job의 오류를 돌려준다. 성공이면 output을 한 번만 늘려 인덱스와 payload를 복사한다.
    ErrorCode error = ErrorCode::None;
    uint64 payloadSize = 0;
    for (uint32 job = 0; job < jobCount && error == ErrorCode::None; ++job) {
        error = jobs[job].error;
        payloadSize += jobs[job].payload.Size();
    }

    usize written = 0;
    if (error == ErrorCode::None) {
        const usize indexBytes = IndexBytesFor(blockCount);
        auto grown = output.TryAppendUninitialized(indexBytes + static_cast<usize>(payloadSize));
        if (!grown) {
            error = grown.Error();
        } else {
            uint8* cursor = grown.Value().Data();
            WriteIndex(cursor, shared.format, blockSizeLog2, source.Size(), blockCount, payloadSize,
                       table, shared.dictionary->Get());
            cursor += indexBytes;
            for (uint32 job = 0; job < jobCount; ++job) {
                platform::MemoryCopy(cursor, jobs[job].payload.Data(), jobs[job].payload.Size());
                cursor += jobs[job].payload.Size();
            }
            written = grown.Value().Size();
        }
    }

    for (uint32 job = 0; job < jobCount; ++job) {
        DestroyAt(jobs + job);
    }
    detail::CodecFree(scratch, jobs);
    detail::CodecFree(scratch, table);
    return error == ErrorCode::None ? Result<usize>::Success(written)
                                    : Result<usize>::Failure(error);
}

Result<void> TryReadCompressedBlocks(WorkerScheduler& scheduler,
                                     const CompressedBlocksReader& reader, uint64 rawOffset,
                                     Span<uint8> destination, Span<const uint8> stored,
                                     uint64 storedBase, Allocator& scratch, bool verify) noexcept {
    if (scheduler.IsCurrentWorker()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }
    if (!reader.IsOpen()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }
    if (!CompressedBlocksAccess::InRange(reader, rawOffset, destination.Size()) ||
        detail::Overlaps(destination.Data(), destination.Size(), stored.Data(), stored.Size())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (destination.Size() == 0) {
        return Result<void>::Success();
    }

    const uint32 first = static_cast<uint32>(rawOffset >> reader.BlockSizeLog2());
    const uint32 last =
        static_cast<uint32>((rawOffset + destination.Size() - 1) >> reader.BlockSizeLog2());
    const uint32 blocks = last - first + 1;
    const uint32 workers = scheduler.Stats().workerCount;
    const bool sequential =
        blocks <= SequentialBlockLimit || destination.Size() < ParallelMinimumBytes;
    const uint32 blocksPerJob =
        sequential ? blocks : BlocksPerJob(blocks, reader.BlockSizeLog2(), workers);
    const uint32 jobCount = (blocks + blocksPerJob - 1) / blocksPerJob;
    auto* errors =
        static_cast<ErrorCode*>(detail::CodecAllocate(scratch, sizeof(ErrorCode) * jobCount));
    if (errors == nullptr) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }
    for (uint32 job = 0; job < jobCount; ++job) {
        errors[job] = ErrorCode::None;
    }

    ReadShared shared{};
    shared.queue.jobCount = jobCount;
    shared.reader = &reader;
    shared.entries = CompressedBlocksAccess::Entries(reader);
    shared.format = reader.Format();
    shared.blockSizeLog2 = reader.BlockSizeLog2();
    shared.firstBlock = first;
    shared.lastBlock = last;
    shared.blocksPerJob = blocksPerJob;
    shared.rawOffset = rawOffset;
    shared.destination = destination;
    shared.stored = stored;
    shared.storedBase = storedBase;
    shared.verify = verify;
    shared.errors = errors;
    shared.dictionary = CompressedBlocksAccess::Dictionary(reader);
    shared.scratch = &scratch;
    RunParticipants(scheduler, shared,
                    jobCount > 1 ? HelperCount(scheduler, jobCount, destination.Size()) : 0,
                    scratch);

    ErrorCode error = ErrorCode::None;
    for (uint32 job = 0; job < jobCount && error == ErrorCode::None; ++job) {
        error = errors[job];
    }
    detail::CodecFree(scratch, errors);
    return error == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(error);
}

} // namespace rocks
