#include "StreamedBulkSystem.h"

#include <AssetRuntime/PackageReader.h>
#include <Core/Diagnostics/Log.h>
#include <Core/FileError.h>
#include <Core/Move.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>

#include <string.h>

// 스트리밍 bulk 범위 읽기다(Docs/Design/AssetSystem 20.6 — PR12c).
namespace rocks {

namespace {

constexpr LogCategory LogStreamedBulk{"StreamedBulk"};

// ---- 기본 범위 IO(AsyncReadFile) ----

struct AsyncStreamedFile {
    platform::AsyncReadFile file;
};

struct AsyncStreamedRead {
    platform::AsyncIoRequest request;
    StreamedBulkIoRead* read = nullptr;
};

void OnAsyncStreamedRead(const platform::AsyncIoResult& result, void* context) noexcept {
    auto& state = *static_cast<AsyncStreamedRead*>(context);
    StreamedBulkIoResult out;
    out.bytes = result.bytes;
    if (!result.status) {
        out.error = result.status.code == platform::FileCode::Cancelled
                        ? ErrorCode::Cancelled
                        : FileErrorCode(result.status);
    }

    // complete가 마지막 접근이다(그 뒤 AssetManager가 Release로 이 상태를 지울 수 있다).
    state.read->complete(*state.read, out);
}

template <class T>
void Destroy(Allocator& allocator, T* object) noexcept {
    object->~T();
    (void)allocator.Deallocate(object);
}

} // namespace

Result<StreamedBulkFile*> AsyncStreamedBulkIo::Open(const char* path, usize pathBytes,
                                                    Span<uint8> header,
                                                    uint64& fileBytes) noexcept {
    auto memory = allocator_.Allocate(sizeof(AsyncStreamedFile), alignof(AsyncStreamedFile));
    if (!memory) {
        return Result<StreamedBulkFile*>::Failure(memory.Error());
    }

    auto* state = InitializeAt<AsyncStreamedFile>(memory.Value());
    auto fail = [&](platform::FileStatus status) noexcept {
        Destroy(allocator_, state);
        return Result<StreamedBulkFile*>::Failure(FileErrorCode(status));
    };
    if (const platform::FileStatus opened = state->file.Open(io_, path, pathBytes); !opened) {
        return fail(opened);
    }

    uint64 size = 0;
    if (const platform::FileStatus sized = state->file.Size(size); !sized) {
        (void)state->file.Close();
        return fail(sized);
    }

    // 머리는 제출 뒤 기다린다(worker에서 불린다 — 막아도 된다). 짧은 파일은 읽은 만큼만이다.
    memset(header.Data(), 0, header.Size());
    const usize headerBytes = size < header.Size() ? static_cast<usize>(size) : header.Size();
    if (headerBytes != 0) {
        platform::AsyncIoRequest request;
        if (const platform::FileStatus submitted =
                state->file.SubmitRead(request, 0, header.Data(), headerBytes);
            !submitted) {
            (void)state->file.Close();
            return fail(submitted);
        }

        (void)request.Wait();
        const platform::AsyncIoResult result = request.Result();
        if (!result.status) {
            (void)state->file.Close();
            return fail(result.status);
        }
    }

    fileBytes = size;
    return Result<StreamedBulkFile*>::Success(reinterpret_cast<StreamedBulkFile*>(state));
}

Result<void> AsyncStreamedBulkIo::Read(StreamedBulkFile& file, StreamedBulkIoRead& read) noexcept {
    auto memory = allocator_.Allocate(sizeof(AsyncStreamedRead), alignof(AsyncStreamedRead));
    if (!memory) {
        return Result<void>::Failure(memory.Error());
    }

    auto* state = InitializeAt<AsyncStreamedRead>(memory.Value());
    state->read = &read;
    read.backendState = state;
    auto& opened = reinterpret_cast<AsyncStreamedFile&>(file);
    const platform::FileStatus submitted =
        opened.file.SubmitRead(state->request, read.offset, read.destination,
                               static_cast<usize>(read.bytes), OnAsyncStreamedRead, state);
    if (!submitted) {
        read.backendState = nullptr;
        Destroy(allocator_, state);
        return Result<void>::Failure(FileErrorCode(submitted));
    }

    return Result<void>::Success();
}

void AsyncStreamedBulkIo::Cancel(StreamedBulkIoRead& read) noexcept {
    if (read.backendState != nullptr) {
        static_cast<AsyncStreamedRead*>(read.backendState)->request.Cancel();
    }
}

void AsyncStreamedBulkIo::Release(StreamedBulkIoRead& read) noexcept {
    // complete가 불린 요청뿐이다(요청 객체의 소멸은 기다리지 않는다).
    auto* state = static_cast<AsyncStreamedRead*>(read.backendState);
    read.backendState = nullptr;
    Destroy(allocator_, state);
}

void AsyncStreamedBulkIo::Close(StreamedBulkFile* file) noexcept {
    auto* state = reinterpret_cast<AsyncStreamedFile*>(file);
    (void)state->file.Close();
    Destroy(allocator_, state);
}

namespace detail {

namespace {

// 끝내기 작업을 worker에서 돌린다. 제출이 실패하면(종료·할당 실패) 원인을 로그에 남기고 지금
// 스레드에서 바로 돌린다(AS 20.6 8 — 무거운 일이 없는 FinishJob만. 열기는 파일 실패로, 검증은
// 제출 오류로 끝낸다).
void RunOrInline(StreamedBulkSystem& system, AssetLoaderWorker::Job job, void* context) noexcept {
    if (auto ran = system.worker.Run(job, context); !ran) {
        Log::Warning(LogStreamedBulk,
                     "Streamed bulk work could not be scheduled ({Error}); running it on the "
                     "current thread",
                     ErrorCodeName(ran.Error()));
        job(context);
    }
}

// [start, start + bytes)를 덮는 블록 경계 범위다(끝은 파일 끝에서 자른다 — bytes > 0).
void WidenToBlocks(const StreamedBulkFileRecord& file, uint64 start, uint64 bytes,
                   uint64& rangeStart, uint64& rangeEnd) noexcept {
    const uint64 blockBytes = file.hashBlockBytes;
    rangeStart = start / blockBytes * blockBytes;
    const uint64 aligned = (start + bytes + blockBytes - 1) / blockBytes * blockBytes;
    rangeEnd = aligned < file.fileBytes ? aligned : file.fileBytes;
}

bool Before(const StreamedBulkReadOp& left, const StreamedBulkReadOp& right) noexcept {
    if (left.priority != right.priority) {
        return static_cast<uint8>(left.priority) > static_cast<uint8>(right.priority);
    }

    return left.sequence < right.sequence;
}

void RemoveFrom(Array<StreamedBulkReadOp*>& list, StreamedBulkReadOp* op) noexcept {
    for (usize index = 0; index < list.Size(); ++index) {
        if (list[index] == op) {
            (void)list.TryRemoveAt(index);
            return;
        }
    }
}

// 결과를 정하고 callback을 부른 뒤 시스템 계수를 놓는다(worker — AS 20.6 6·7).
void Deliver(StreamedBulkReadOp* op, ErrorCode error) noexcept {
    StreamedBulkSystem& system = *op->system;
    BulkReadResult result;
    result.error = error;
    result.bytes = error == ErrorCode::None ? op->destination.Size() : 0;
    const BulkReadState state = error == ErrorCode::None        ? BulkReadState::Done
                                : error == ErrorCode::Cancelled ? BulkReadState::Cancelled
                                                                : BulkReadState::Failed;
    {
        platform::LockGuard lock(system.mutex);
        op->phase = StreamedReadPhase::Finished;
    }

    // 상태를 callback 전에 정한다(callback 안에서 자기 손잡이를 파괴할 수 있다).
    platform::AtomicStore<platform::MemoryOrder::Release>(&op->state, static_cast<uint8>(state));
    op->callback(op->context, result);
    system.ReleaseOp(op);
    system.Dispatch();
    // 깨움은 뮤텍스 안에서 한다 — 놓은 뒤에는 시스템을 만지지 않는다(Shutdown이 바로 돌아간다).
    platform::LockGuard lock(system.mutex);
    if (--system.outstanding == 0) {
        system.idle.NotifyAll();
    }
}

void FinishJob(void* context) noexcept {
    auto* op = static_cast<StreamedBulkReadOp*>(context);
    Deliver(op, op->finishError);
}

// IO가 끝난 요청을 검증한다: 짧은 읽기·덮은 블록 SHA-256 → 요청 범위 복사(AS 20.6 6).
void VerifyJob(void* context) noexcept {
    auto* op = static_cast<StreamedBulkReadOp*>(context);
    StreamedBulkSystem& system = *op->system;
    {
        platform::LockGuard lock(system.mutex);
        --system.inFlight;
        op->phase = StreamedReadPhase::Verifying;
        op->ioStarted = false;
        RemoveFrom(system.submitted, op);
        // mutex 밖에서 진행 중인 IO 취소가 끝나야 요청 자원을 돌려줄 수 있다.
        while (op->ioPins != 0) {
            (void)system.unpinned.Wait(system.mutex);
        }
    }

    system.io.Release(op->ioRead);
    ErrorCode error = op->ioResult.error;
    const StreamedBulkFileRecord& file = *op->file;
    if (error == ErrorCode::None && op->ioResult.bytes < op->scratch.Size()) {
        error = ErrorCode::CorruptData;
    }

    const uint64 blockBytes = file.hashBlockBytes;
    const uint64 firstBlock = op->rangeStart / blockBytes;
    for (uint64 at = 0; error == ErrorCode::None && at < op->scratch.Size(); at += blockBytes) {
        const uint64 piece =
            op->scratch.Size() - at < blockBytes ? op->scratch.Size() - at : blockBytes;
        const Span<const uint8> chunk(op->scratch.Data() + at, static_cast<usize>(piece));
        if (!(Sha256::Hash(chunk) ==
              file.blockHashes[static_cast<usize>(firstBlock + at / blockBytes)])) {
            error = ErrorCode::CorruptData;
        }
    }

    if (error == ErrorCode::None) {
        memcpy(op->destination.Data(),
               op->scratch.Data() + static_cast<usize>(op->fileOffset - op->rangeStart),
               op->destination.Size());
    }

    op->scratch = Array<uint8>(system.allocator);
    Deliver(op, error);
}

void OnIoComplete(StreamedBulkIoRead& read, const StreamedBulkIoResult& result) noexcept {
    auto* op = static_cast<StreamedBulkReadOp*>(read.owner);
    StreamedBulkSystem& system = *op->system;
    op->ioResult = result;
    // 제출이 돌아올 때까지 Shutdown이 끝나지 않게 붙든다(worker가 먼저 전달을 마쳐도 시스템이
    // 산다).
    {
        platform::LockGuard lock(system.mutex);
        ++system.outstanding;
    }

    if (auto ran = system.worker.Run(VerifyJob, op); !ran) {
        // 해시 검증을 IO 완료 스레드에서 하지 않는다 — 제출 오류로 끝낸다(AS 20.6 8).
        Log::Warning(LogStreamedBulk,
                     "Streamed bulk verification could not be scheduled ({Error}); failing the "
                     "read",
                     ErrorCodeName(ran.Error()));
        if (op->ioResult.error == ErrorCode::None) {
            op->ioResult.error = ran.Error();
        }

        VerifyJob(op);
    }

    platform::LockGuard lock(system.mutex);
    if (--system.outstanding == 0) {
        system.idle.NotifyAll();
    }
}

// 제출한 요청이 IO 전에 끝날 때다(scratch 할당 실패·취소·즉시 실패).
void FailSubmitted(StreamedBulkReadOp* op, ErrorCode error) noexcept {
    StreamedBulkSystem& system = *op->system;
    {
        platform::LockGuard lock(system.mutex);
        --system.inFlight;
        op->ioStarted = false;
        op->phase = StreamedReadPhase::Finishing;
        op->finishError = error;
        RemoveFrom(system.submitted, op);
    }

    op->scratch = Array<uint8>(system.allocator);
    RunOrInline(system, FinishJob, op);
}

// 요청 범위를 덮는 블록 경계 범위를 scratch에 읽기 시작한다.
void StartRead(StreamedBulkReadOp* op) noexcept {
    StreamedBulkSystem& system = *op->system;
    const StreamedBulkFileRecord& file = *op->file;
    uint64 rangeEnd = 0;
    WidenToBlocks(file, op->fileOffset, op->destination.Size(), op->rangeStart, rangeEnd);
    if (auto sized = op->scratch.TryResize(static_cast<usize>(rangeEnd - op->rangeStart)); !sized) {
        FailSubmitted(op, sized.Error());
        return;
    }

    op->ioRead = StreamedBulkIoRead{};
    op->ioRead.offset = op->rangeStart;
    op->ioRead.destination = op->scratch.Data();
    op->ioRead.bytes = op->scratch.Size();
    op->ioRead.complete = OnIoComplete;
    op->ioRead.owner = op;
    bool cancelled = false;
    {
        platform::LockGuard lock(system.mutex);
        if (op->cancelRequested) {
            // 제출 전에 취소됐다.
            cancelled = true;
        } else {
            // 완료·검증·전달이 Read 반환보다 먼저 끝나도 요청이 살아 있게 계수를 하나 더 쥔다.
            ++op->refs;
        }
    }

    if (cancelled) {
        FailSubmitted(op, ErrorCode::Cancelled);
        return;
    }

    if (auto read = system.io.Read(*file.file, op->ioRead); !read) {
        FailSubmitted(op, read.Error());
        system.ReleaseOp(op);
        return;
    }

    bool cancelIo = false;
    {
        platform::LockGuard lock(system.mutex);
        // 검증 작업이 이미 가져갔으면 IO 자원은 돌려준 뒤다 — 취소할 것이 없다.
        if (op->phase == StreamedReadPhase::Submitted) {
            op->ioStarted = true;
            if (op->cancelRequested) {
                // 제출 중에 들어온 취소를 지금 보낸다.
                ++op->ioPins;
                cancelIo = true;
            }
        }
    }

    if (cancelIo) {
        system.CancelPinned(op);
    }

    system.ReleaseOp(op);
}

// 파일을 열고 크기·머리를 본다(worker — AS 20.6 5).
void OpenJob(void* context) noexcept {
    auto* file = static_cast<StreamedBulkFileRecord*>(context);
    StreamedBulkSystem& system = *file->system;
    uint8 header[SidecarHeaderBytes];
    uint64 fileBytes = 0;
    auto opened = system.io.Open(file->path.CStr(), file->path.View().ByteCount(),
                                 Span<uint8>(header, SidecarHeaderBytes), fileBytes);
    ErrorCode failure = ErrorCode::None;
    StreamedBulkFile* handle = nullptr;
    if (!opened) {
        failure = opened.Error();
    } else {
        handle = opened.Value();
        if (fileBytes != file->fileBytes ||
            !ReadSidecarHeader(Span<const uint8>(header, SidecarHeaderBytes), file->header)) {
            failure = ErrorCode::CorruptData;
        }
    }

    if (failure != ErrorCode::None && handle != nullptr) {
        system.io.Close(handle);
        handle = nullptr;
    }

    {
        platform::LockGuard lock(system.mutex);
        file->phase =
            failure == ErrorCode::None ? StreamedFilePhase::Open : StreamedFilePhase::Failed;
        file->failure = failure;
        file->file = handle;
    }

    system.ReleaseFile(file);
    system.Dispatch();
    platform::LockGuard lock(system.mutex);
    if (--system.outstanding == 0) {
        system.idle.NotifyAll();
    }
}

} // namespace

StreamedBulkSystem::~StreamedBulkSystem() {
    platform::LockGuard lock(mutex);
    if (!files.IsEmpty() || liveOps != 0) {
        platform::Fatal("AssetManager destroyed while streamed bulk handles are alive");
    }
}

Result<StreamedBulkFileRecord*>
StreamedBulkSystem::Acquire(StringView path, const PackageSidecarState& sidecar) noexcept {
    platform::LockGuard lock(mutex);
    if (shuttingDown) {
        return Result<StreamedBulkFileRecord*>::Failure(ErrorCode::ShuttingDown);
    }

    for (StreamedBulkFileRecord* file : files) {
        if (file->path.View() == path) {
            ++file->refs;
            return Result<StreamedBulkFileRecord*>::Success(file);
        }
    }

    auto memory =
        allocator.Allocate(sizeof(StreamedBulkFileRecord), alignof(StreamedBulkFileRecord));
    if (!memory) {
        return Result<StreamedBulkFileRecord*>::Failure(memory.Error());
    }

    auto* file = InitializeAt<StreamedBulkFileRecord>(memory.Value(), allocator);
    file->system = this;
    file->fileBytes = sidecar.fileBytes;
    file->hashBlockBytes = sidecar.hashBlockBytes;
    file->header = sidecar.header;
    file->refs = 1;
    Result<void> copied = file->path.TryAssign(path);
    if (copied) {
        copied = file->blockHashes.TryAppend(sidecar.blockHashes.View());
    }

    if (copied) {
        copied = files.TryPushBack(file);
    }

    if (!copied) {
        Destroy(allocator, file);
        return Result<StreamedBulkFileRecord*>::Failure(copied.Error());
    }

    return Result<StreamedBulkFileRecord*>::Success(file);
}

void StreamedBulkSystem::ReleaseFile(StreamedBulkFileRecord* file) noexcept {
    {
        platform::LockGuard lock(mutex);
        if (--file->refs != 0) {
            return;
        }

        for (usize index = 0; index < files.Size(); ++index) {
            if (files[index] == file) {
                (void)files.TryRemoveAt(index);
                break;
            }
        }
    }

    // 계수가 0이면 진행 IO도 열기 작업도 없다(둘 다 계수를 쥔다).
    if (file->file != nullptr) {
        io.Close(file->file);
    }

    Destroy(allocator, file);
}

Result<StreamedBulkReadOp*> StreamedBulkSystem::Submit(StreamedBulkFileRecord* file,
                                                       uint64 fileOffset, Span<uint8> destination,
                                                       BulkReadPriority priority,
                                                       BulkReadCallback callback,
                                                       void* context) noexcept {
    StreamedBulkReadOp* op = nullptr;
    {
        platform::LockGuard lock(mutex);
        if (shuttingDown) {
            return Result<StreamedBulkReadOp*>::Failure(ErrorCode::ShuttingDown);
        }

        auto memory = allocator.Allocate(sizeof(StreamedBulkReadOp), alignof(StreamedBulkReadOp));
        if (!memory) {
            return Result<StreamedBulkReadOp*>::Failure(memory.Error());
        }

        op = InitializeAt<StreamedBulkReadOp>(memory.Value(), allocator);
        op->system = this;
        op->file = file;
        op->fileOffset = fileOffset;
        op->destination = destination;
        op->priority = priority;
        op->sequence = sequence++;
        op->callback = callback;
        op->context = context;
        // 손잡이 하나 + 시스템 하나다.
        op->refs = 2;
        if (destination.IsEmpty()) {
            op->phase = StreamedReadPhase::Finishing;
            op->finishError = ErrorCode::None;
        } else if (auto pushed = queue.TryPushBack(op); !pushed) {
            Destroy(allocator, op);
            return Result<StreamedBulkReadOp*>::Failure(pushed.Error());
        }

        ++file->refs;
        ++outstanding;
        ++liveOps;
    }

    if (destination.IsEmpty()) {
        RunOrInline(*this, FinishJob, op);
    } else {
        Dispatch();
    }

    return Result<StreamedBulkReadOp*>::Success(op);
}

void StreamedBulkSystem::Cancel(StreamedBulkReadOp* op) noexcept {
    bool finish = false;
    bool cancelIo = false;
    {
        platform::LockGuard lock(mutex);
        if (op->phase == StreamedReadPhase::Queued) {
            RemoveFrom(queue, op);
            op->phase = StreamedReadPhase::Finishing;
            op->finishError = ErrorCode::Cancelled;
            finish = true;
        } else if (op->phase == StreamedReadPhase::Submitted && !op->cancelRequested) {
            // io.Read가 아직 돌아오지 않았으면 StartRead가 돌아온 뒤 취소를 보낸다.
            op->cancelRequested = true;
            if (op->ioStarted) {
                ++op->ioPins;
                cancelIo = true;
            }
        }
    }

    if (cancelIo) {
        CancelPinned(op);
    }

    if (finish) {
        RunOrInline(*this, FinishJob, op);
    }
}

void StreamedBulkSystem::CancelPinned(StreamedBulkReadOp* op) noexcept {
    // 핀이 있는 동안 검증 작업이 Release·전달을 미루므로 요청과 시스템이 산다.
    io.Cancel(op->ioRead);
    platform::LockGuard lock(mutex);
    if (--op->ioPins == 0) {
        unpinned.NotifyAll();
    }
}

void StreamedBulkSystem::SetPriority(StreamedBulkReadOp* op, BulkReadPriority priority) noexcept {
    platform::LockGuard lock(mutex);
    if (op->phase == StreamedReadPhase::Queued) {
        op->priority = priority;
    }
}

void StreamedBulkSystem::ReleaseOp(StreamedBulkReadOp* op) noexcept {
    StreamedBulkFileRecord* file = nullptr;
    {
        platform::LockGuard lock(mutex);
        if (--op->refs != 0) {
            return;
        }

        --liveOps;
        file = op->file;
    }

    Destroy(allocator, op);
    ReleaseFile(file);
}

void StreamedBulkSystem::Shutdown() noexcept {
    Array<StreamedBulkReadOp*> cancelled(allocator);
    {
        platform::LockGuard lock(mutex);
        if (shuttingDown) {
            return;
        }

        shuttingDown = true;
        for (StreamedBulkReadOp* op : queue) {
            op->phase = StreamedReadPhase::Finishing;
            op->finishError = ErrorCode::Cancelled;
        }

        cancelled = Move(queue);
        queue = Array<StreamedBulkReadOp*>(allocator);
    }

    // 큐에 있던 요청을 먼저 끝낸다(큐가 비었으니 새 제출은 없다).
    for (StreamedBulkReadOp* op : cancelled) {
        RunOrInline(*this, FinishJob, op);
    }

    // 진행 IO에 취소를 하나씩 보낸다(할당 없이 — 이미 취소 요청된 것은 건너뛴다).
    for (;;) {
        StreamedBulkReadOp* pinned = nullptr;
        {
            platform::LockGuard lock(mutex);
            for (StreamedBulkReadOp* op : submitted) {
                if (!op->cancelRequested) {
                    op->cancelRequested = true;
                    if (op->ioStarted) {
                        ++op->ioPins;
                        pinned = op;
                        break;
                    }
                }
            }
        }

        if (pinned == nullptr) {
            break;
        }

        CancelPinned(pinned);
    }

    platform::LockGuard lock(mutex);
    while (outstanding != 0) {
        (void)idle.Wait(mutex);
    }
}

void StreamedBulkSystem::Dispatch() noexcept {
    for (;;) {
        StreamedBulkReadOp* submit = nullptr;
        StreamedBulkReadOp* failed = nullptr;
        StreamedBulkFileRecord* open = nullptr;
        {
            platform::LockGuard lock(mutex);
            StreamedBulkReadOp* best = nullptr;
            for (StreamedBulkReadOp* op : queue) {
                const StreamedFilePhase phase = op->file->phase;
                const bool ready = phase == StreamedFilePhase::Failed ||
                                   phase == StreamedFilePhase::Closed ||
                                   (phase == StreamedFilePhase::Open && inFlight < maxInFlight);
                if (ready && (best == nullptr || Before(*op, *best))) {
                    best = op;
                }
            }

            if (best == nullptr) {
                return;
            }

            StreamedBulkFileRecord& file = *best->file;
            if (file.phase == StreamedFilePhase::Closed) {
                // 열기 작업이 계수를 쥔다. 요청은 큐에 남아 열기를 기다린다.
                file.phase = StreamedFilePhase::Opening;
                ++file.refs;
                ++outstanding;
                open = &file;
            } else if (file.phase == StreamedFilePhase::Failed) {
                RemoveFrom(queue, best);
                best->phase = StreamedReadPhase::Finishing;
                best->finishError = file.failure;
                failed = best;
            } else {
                // 진행 목록 push가 실패하면 IO를 시작하지 않고 그 오류로 끝낸다.
                RemoveFrom(queue, best);
                if (auto pushed = submitted.TryPushBack(best); !pushed) {
                    best->phase = StreamedReadPhase::Finishing;
                    best->finishError = pushed.Error();
                    failed = best;
                } else {
                    best->phase = StreamedReadPhase::Submitted;
                    ++inFlight;
                    submit = best;
                }
            }
        }

        if (open != nullptr) {
            if (auto ran = worker.Run(OpenJob, open); !ran) {
                // 열기는 지금 스레드에서 돌리지 않는다(Game 스레드 동기 IO 금지) — 파일 실패로
                // 둔다.
                Log::Warning(LogStreamedBulk,
                             "Streamed bulk file open could not be scheduled ({Error})",
                             ErrorCodeName(ran.Error()));
                {
                    platform::LockGuard lock(mutex);
                    open->phase = StreamedFilePhase::Failed;
                    open->failure = ran.Error();
                }

                // 열기 작업 몫의 계수는 파일을 놓은 뒤 내린다(OpenJob과 같은 순서).
                ReleaseFile(open);
                platform::LockGuard lock(mutex);
                if (--outstanding == 0) {
                    idle.NotifyAll();
                }
            }
        }

        if (failed != nullptr) {
            RunOrInline(*this, FinishJob, failed);
        }

        if (submit != nullptr) {
            StartRead(submit);
        }
    }
}

} // namespace detail

// ---- 손잡이 ----

BulkReadHandle::BulkReadHandle(BulkReadHandle&& other) noexcept : op_(other.op_) {
    other.op_ = nullptr;
}

BulkReadHandle& BulkReadHandle::operator=(BulkReadHandle&& other) noexcept {
    if (this != &other) {
        Drop();
        op_ = other.op_;
        other.op_ = nullptr;
    }

    return *this;
}

BulkReadHandle::~BulkReadHandle() {
    Drop();
}

void BulkReadHandle::Drop() noexcept {
    if (op_ == nullptr) {
        return;
    }

    if (State() == BulkReadState::Pending) {
        platform::Fatal("BulkReadHandle destroyed while its read is pending");
    }

    op_->system->ReleaseOp(op_);
    op_ = nullptr;
}

BulkReadState BulkReadHandle::State() const noexcept {
    if (op_ == nullptr) {
        return BulkReadState::Done;
    }

    return static_cast<BulkReadState>(
        platform::AtomicLoad<platform::MemoryOrder::Acquire>(&op_->state));
}

void BulkReadHandle::Cancel() noexcept {
    if (op_ != nullptr) {
        op_->system->Cancel(op_);
    }
}

void BulkReadHandle::SetPriority(BulkReadPriority priority) noexcept {
    if (op_ != nullptr) {
        op_->system->SetPriority(op_, priority);
    }
}

StreamedBulk::StreamedBulk(StreamedBulk&& other) noexcept
    : file_(other.file_), offset_(other.offset_), rawBytes_(other.rawBytes_) {
    other.file_ = nullptr;
    other.rawBytes_ = 0;
}

StreamedBulk& StreamedBulk::operator=(StreamedBulk&& other) noexcept {
    if (this != &other) {
        Drop();
        file_ = other.file_;
        offset_ = other.offset_;
        rawBytes_ = other.rawBytes_;
        other.file_ = nullptr;
        other.rawBytes_ = 0;
    }

    return *this;
}

StreamedBulk::~StreamedBulk() {
    Drop();
}

void StreamedBulk::Drop() noexcept {
    if (file_ != nullptr) {
        file_->system->ReleaseFile(file_);
        file_ = nullptr;
        rawBytes_ = 0;
    }
}

Result<BulkReadHandle> StreamedBulk::Read(uint64 offset, Span<uint8> destination,
                                          BulkReadPriority priority, BulkReadCallback callback,
                                          void* context) noexcept {
    if (file_ == nullptr || callback == nullptr || offset > rawBytes_ ||
        destination.Size() > rawBytes_ - offset ||
        static_cast<uint8>(priority) > static_cast<uint8>(BulkReadPriority::Critical)) {
        return Result<BulkReadHandle>::Failure(ErrorCode::InvalidArgument);
    }

    // IO 한 번이 읽는 것은 블록 경계로 넓힌 범위다.
    if (!destination.IsEmpty()) {
        uint64 rangeStart = 0;
        uint64 rangeEnd = 0;
        detail::WidenToBlocks(*file_, offset_ + offset, destination.Size(), rangeStart, rangeEnd);
        if (rangeEnd - rangeStart > StreamedBulkMaxReadBytes) {
            return Result<BulkReadHandle>::Failure(ErrorCode::InvalidArgument);
        }
    }

    auto op =
        file_->system->Submit(file_, offset_ + offset, destination, priority, callback, context);
    if (!op) {
        return Result<BulkReadHandle>::Failure(op.Error());
    }

    return Result<BulkReadHandle>::Success(BulkReadHandle(op.Value()));
}

} // namespace rocks
