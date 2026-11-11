#include "AsyncFileInternal.h"
#include "FileInternal.h"
#include "WaitDeadline.h"

#include <Platform/Fatal.h>
#include <Platform/Memory.h>

#include <new>

namespace rocks::platform {
namespace detail {
namespace {

constexpr uint32 MaxWorkers = 64;

AsyncContextState& ContextOf(unsigned char* storage) noexcept {
    return *__builtin_launder(reinterpret_cast<AsyncContextState*>(storage));
}

const AsyncContextState& ContextOf(const unsigned char* storage) noexcept {
    return *__builtin_launder(reinterpret_cast<const AsyncContextState*>(storage));
}

AsyncFileState& FileOf(unsigned char* storage) noexcept {
    return *__builtin_launder(reinterpret_cast<AsyncFileState*>(storage));
}

const AsyncFileState& FileOf(const unsigned char* storage) noexcept {
    return *__builtin_launder(reinterpret_cast<const AsyncFileState*>(storage));
}

AsyncRequestState& RequestOf(unsigned char* storage) noexcept {
    return *__builtin_launder(reinterpret_cast<AsyncRequestState*>(storage));
}

const AsyncRequestState& RequestOf(const unsigned char* storage) noexcept {
    return *__builtin_launder(reinterpret_cast<const AsyncRequestState*>(storage));
}

bool IsInFlight(const AsyncRequestState& request) noexcept {
    return request.phase == AsyncPhase::Queued || request.phase == AsyncPhase::Running;
}

// worker는 큐에서 요청을 꺼내 위치 지정 동기 읽기·쓰기를 수행한다. 취소 표시된 대기 요청은 읽지
// 않고 취소로 완료한다. 실행 중인 읽기는 OS 호출이 끝날 때까지 취소할 수 없다.
void WorkerEntry(void* raw) noexcept {
    auto& context = *static_cast<AsyncContextState*>(raw);
    for (;;) {
        AsyncRequestState* request = nullptr;
        bool cancelled = false;
        {
            LockGuard lock(context.mutex);
            while (!context.stopping && context.queueHead == nullptr) {
                if (context.changed.Wait(context.mutex) == WaitResult::Failed) {
                    Fatal("AsyncIoContext worker wait failed");
                }
            }
            if (context.queueHead == nullptr) {
                return; // stopping이고 큐가 비었다.
            }
            request = context.queueHead;
            context.queueHead = request->nextQueued;
            if (context.queueHead == nullptr) {
                context.queueTail = nullptr;
            }
            request->nextQueued = nullptr;
            cancelled = request->cancelRequested;
            request->phase = AsyncPhase::Running;
        }
        AsyncIoResult result{};
        if (cancelled) {
            result.status = {FileCode::Cancelled, 0};
        } else if (request->write) {
            result.status = AsyncPositionalWrite(request->file->handle, request->offset,
                                                 request->source, request->bytes, result.bytes);
        } else {
            result.status = AsyncPositionalRead(request->file->handle, request->offset,
                                                request->destination, request->bytes, result.bytes);
        }
        CompleteAsyncRequest(*request, result);
    }
}

void StopWorkers(AsyncContextState& context, uint32 started) noexcept {
    {
        LockGuard lock(context.mutex);
        context.stopping = true;
    }
    context.changed.NotifyAll();
    for (uint32 index = 0; index < started; ++index) {
        if (context.workers[index].Join() != ThreadStatus::Success) {
            Fatal("AsyncIoContext worker join failed");
        }
    }
    for (uint32 index = 0; index < context.workerCount; ++index) {
        context.workers[index].~Thread();
    }
    AlignedFree(context.workers);
    context.workers = nullptr;
    context.workerCount = 0;
}

} // namespace

void CompleteAsyncRequest(AsyncRequestState& request, const AsyncIoResult& result) noexcept {
    AsyncContextState& context = *request.context;
    AsyncIoCallback callback = nullptr;
    void* callbackContext = nullptr;
    {
        LockGuard lock(context.mutex);
        // Native 제출 스레드가 OS 호출 직후 요청을 다시 확인하는 동안에는 게시하지 않는다.
        while (request.submitting) {
            if (context.changed.Wait(context.mutex) == WaitResult::Failed) {
                Fatal("AsyncIoContext completion wait failed");
            }
        }
        if (request.phase == AsyncPhase::Completed || request.phase == AsyncPhase::Idle) {
            Fatal("AsyncIoRequest completed more than once");
        }
        callback = request.callback;
        callbackContext = request.callbackContext;
        request.result = result;
        request.phase = AsyncPhase::Completed;
        --request.file->pending;
        --context.pending;
        // 이 지점 이후 요청과 파일은 대기자가 파괴할 수 있으므로 다시 접근하지 않는다.
    }
    context.changed.NotifyAll();
    if (callback != nullptr) {
        callback(result, callbackContext);
    }
}

} // namespace detail

using detail::AsyncContextState;
using detail::AsyncFileState;
using detail::AsyncPhase;
using detail::AsyncRequestState;

AsyncIoBackend DefaultAsyncIoBackend() noexcept {
    return detail::PlatformDefaultAsyncIoBackend();
}

// ---- AsyncIoContext ----

AsyncIoContext::AsyncIoContext() noexcept {
    static_assert(sizeof(AsyncContextState) <= sizeof(storage_));
    static_assert(alignof(AsyncContextState) <= 16);
    ::new (storage_) AsyncContextState{};
}

AsyncIoContext::~AsyncIoContext() noexcept {
    AsyncContextState& context = detail::ContextOf(storage_);
    if (context.running) {
        {
            LockGuard lock(context.mutex);
            if (context.openFiles != 0) {
                Fatal("AsyncIoContext destroyed while async files are open");
            }
        }
        if (!Shutdown()) {
            Fatal("AsyncIoContext shutdown failed during destruction");
        }
    }
    context.~AsyncContextState();
}

FileStatus AsyncIoContext::Start(AsyncIoBackend backend, uint32 workerCount) noexcept {
    AsyncContextState& context = detail::ContextOf(storage_);
    if (context.running) {
        return {FileCode::AlreadyOpen, 0};
    }
    context.stopping = false;
    context.backend = backend;
    if (backend == AsyncIoBackend::Native) {
        if (!detail::AsyncNativeSupported()) {
            return {FileCode::Unsupported, 0};
        }
        auto started = detail::AsyncNativeStart(context);
        if (!started) {
            return started;
        }
        context.running = true;
        return {};
    }
    if (backend != AsyncIoBackend::WorkerPool) {
        return {FileCode::InvalidArgument, 0};
    }
    if (workerCount == 0 || workerCount > detail::MaxWorkers) {
        return {FileCode::InvalidArgument, 0};
    }
    void* storage = AlignedAllocate(sizeof(Thread) * workerCount, alignof(Thread));
    if (storage == nullptr) {
        return {FileCode::OutOfMemory, 0};
    }
    context.workers = static_cast<Thread*>(storage);
    context.workerCount = workerCount;
    for (uint32 index = 0; index < workerCount; ++index) {
        ::new (&context.workers[index]) Thread();
    }
    for (uint32 index = 0; index < workerCount; ++index) {
        if (context.workers[index].Start(detail::WorkerEntry, &context) != ThreadStatus::Success) {
            // 이미 시작한 worker를 회수하고 원래 상태로 돌린다.
            detail::StopWorkers(context, index);
            context.stopping = false;
            return {FileCode::SystemError, 0};
        }
    }
    context.running = true;
    return {};
}

FileStatus AsyncIoContext::Shutdown() noexcept {
    AsyncContextState& context = detail::ContextOf(storage_);
    if (!context.running) {
        return {FileCode::NotOpen, 0};
    }
    {
        LockGuard lock(context.mutex);
        if (context.openFiles != 0 || context.pending != 0) {
            return {FileCode::Busy, 0};
        }
    }
    if (context.backend == AsyncIoBackend::Native) {
        {
            LockGuard lock(context.mutex);
            context.stopping = true;
        }
        detail::AsyncNativeStop(context);
    } else {
        detail::StopWorkers(context, context.workerCount);
    }
    context.running = false;
    context.stopping = false;
    return {};
}

bool AsyncIoContext::IsRunning() const noexcept {
    return detail::ContextOf(storage_).running;
}

AsyncIoBackend AsyncIoContext::Backend() const noexcept {
    return detail::ContextOf(storage_).backend;
}

// ---- 파일 공용 ----

namespace detail {
namespace {

// 진행 중 요청을 취소하고 OS가 완료를 알릴 때까지 기다린 뒤 닫는다. 버퍼를 먼저 놓으면 안 된다.
void DestroyFileState(AsyncFileState& file, const char* closeFailure) noexcept;
FileStatus CloseFileState(AsyncFileState& file) noexcept;

FileStatus OpenFileState(AsyncFileState& file, AsyncContextState& context, const char* path,
                         usize pathBytes, bool write, FileOpenMode mode) noexcept {
    if (file.open) {
        return {FileCode::AlreadyOpen, 0};
    }
    if (path == nullptr || !ValidFilePath(path, pathBytes)) {
        return {FileCode::InvalidArgument, 0};
    }
    if (write && mode != FileOpenMode::ReadWriteExisting && mode != FileOpenMode::CreateNew &&
        mode != FileOpenMode::CreateAlways) {
        return {FileCode::InvalidArgument, 0};
    }
    {
        // 열린 파일 수를 먼저 예약해 Shutdown과 경합하지 않게 한다.
        LockGuard lock(context.mutex);
        if (!context.running || context.stopping) {
            return {FileCode::NotOpen, 0};
        }
        ++context.openFiles;
    }
    auto release = [&context]() noexcept {
        LockGuard lock(context.mutex);
        --context.openFiles;
    };
    const bool overlapped = context.backend == AsyncIoBackend::Native;
    uptr handle = 0;
    auto opened = write ? AsyncOpenWriteNative(path, pathBytes, overlapped, mode, handle)
                        : AsyncOpenNative(path, pathBytes, overlapped, handle);
    if (!opened) {
        release();
        return opened;
    }
    file.context = &context;
    file.handle = handle;
    file.pending = 0;
    file.writable = write;
    if (overlapped) {
        auto attached = AsyncNativeAttach(context, file);
        if (!attached) {
            if (!AsyncCloseNative(handle)) {
                Fatal("Async file open rollback failed");
            }
            file.context = nullptr;
            file.handle = 0;
            file.writable = false;
            release();
            return attached;
        }
    }
    file.open = true;
    return {};
}

FileStatus CloseFileState(AsyncFileState& file) noexcept {
    if (!file.open) {
        return {FileCode::NotOpen, 0};
    }
    AsyncContextState& context = *file.context;
    {
        LockGuard lock(context.mutex);
        if (file.pending != 0) {
            return {FileCode::Busy, 0};
        }
        file.open = false;
        --context.openFiles;
    }
    const uptr handle = file.handle;
    file.handle = 0;
    file.context = nullptr;
    file.writable = false;
    // close 실패는 terminal이다. 같은 handle로 재시도하지 않는다.
    return AsyncCloseNative(handle);
}

void DestroyFileState(AsyncFileState& file, const char* closeFailure) noexcept {
    if (file.open) {
        AsyncContextState& context = *file.context;
        {
            LockGuard lock(context.mutex);
            if (file.pending != 0) {
                if (context.backend == AsyncIoBackend::Native) {
                    AsyncNativeCancelFile(file);
                } else {
                    for (auto* queued = context.queueHead; queued != nullptr;
                         queued = queued->nextQueued) {
                        if (queued->file == &file) {
                            queued->cancelRequested = true;
                        }
                    }
                }
            }
            while (file.pending != 0) {
                if (context.changed.Wait(context.mutex) == WaitResult::Failed) {
                    Fatal("Async file destruction wait failed");
                }
            }
        }
        if (!CloseFileState(file)) {
            Fatal(closeFailure);
        }
    }
    file.~AsyncFileState();
}

FileStatus SizeFileState(const AsyncFileState& file, uint64& bytes) noexcept {
    if (!file.open) {
        return {FileCode::NotOpen, 0};
    }
    return AsyncSizeNative(file.handle, bytes);
}

FileStatus SubmitFileRequest(AsyncFileState& file, AsyncRequestState& request, uint64 offset,
                             void* destination, const void* source, usize bytes, bool write,
                             AsyncIoCallback callback, void* callbackContext) noexcept {
    if (!file.open) {
        return {FileCode::NotOpen, 0};
    }
    const void* buffer = write ? source : destination;
    if ((bytes != 0 && buffer == nullptr) || bytes > 0xffffffffu ||
        offset > static_cast<uint64>(-1) - bytes) {
        return {FileCode::InvalidArgument, 0};
    }
    AsyncContextState& context = *file.context;
    {
        LockGuard lock(context.mutex);
        if (context.stopping) {
            return {FileCode::Busy, 0};
        }
        if (IsInFlight(request)) {
            return {FileCode::Busy, 0};
        }
        if (request.context != nullptr && request.context != &context) {
            // 다른 context로 재사용하면 대기자가 잘못된 mutex를 잡는다.
            return {FileCode::InvalidArgument, 0};
        }
        request.context = &context;
        request.file = &file;
        request.offset = offset;
        request.destination = write ? nullptr : destination;
        request.source = write ? source : nullptr;
        request.bytes = bytes;
        request.write = write;
        request.callback = callback;
        request.callbackContext = callbackContext;
        request.result = {};
        request.cancelRequested = false;
        request.nextQueued = nullptr;
        ++file.pending;
        ++context.pending;
        if (context.backend == AsyncIoBackend::WorkerPool) {
            request.phase = AsyncPhase::Queued;
            if (context.queueTail != nullptr) {
                context.queueTail->nextQueued = &request;
            } else {
                context.queueHead = &request;
            }
            context.queueTail = &request;
        } else {
            request.phase = AsyncPhase::Running;
            request.submitting = true;
        }
    }
    if (context.backend == AsyncIoBackend::WorkerPool) {
        context.changed.NotifyAll();
        return {};
    }
    AsyncNativeIssue(context, request);
    {
        LockGuard lock(context.mutex);
        request.submitting = false;
        if (request.cancelRequested) {
            // 발행 전에 들어온 취소 요청을 이제 OS에 전달한다.
            AsyncNativeCancel(request);
        }
    }
    context.changed.NotifyAll();
    return {};
}

usize PathLength(const char* path) noexcept {
    usize bytes = 0;
    if (path != nullptr) {
        while (path[bytes] != '\0') {
            ++bytes;
        }
    }
    return bytes;
}

} // namespace
} // namespace detail

// ---- AsyncReadFile ----

AsyncReadFile::AsyncReadFile() noexcept {
    static_assert(sizeof(AsyncFileState) <= sizeof(storage_));
    static_assert(alignof(AsyncFileState) <= 16);
    ::new (storage_) AsyncFileState{};
}

AsyncReadFile::~AsyncReadFile() noexcept {
    detail::DestroyFileState(detail::FileOf(storage_),
                             "AsyncReadFile close failed during destruction");
}

FileStatus AsyncReadFile::Open(AsyncIoContext& context, const char* path) noexcept {
    return Open(context, path, detail::PathLength(path));
}

FileStatus AsyncReadFile::Open(AsyncIoContext& contextObject, const char* path,
                               usize pathBytes) noexcept {
    return detail::OpenFileState(detail::FileOf(storage_),
                                 detail::ContextOf(contextObject.storage_), path, pathBytes, false,
                                 FileOpenMode::ReadExisting);
}

FileStatus AsyncReadFile::Close() noexcept {
    return detail::CloseFileState(detail::FileOf(storage_));
}

bool AsyncReadFile::IsOpen() const noexcept {
    return detail::FileOf(storage_).open;
}

FileStatus AsyncReadFile::Size(uint64& bytes) const noexcept {
    return detail::SizeFileState(detail::FileOf(storage_), bytes);
}

FileStatus AsyncReadFile::SubmitRead(AsyncIoRequest& requestObject, uint64 offset,
                                     void* destination, usize bytes, AsyncIoCallback callback,
                                     void* callbackContext) noexcept {
    return detail::SubmitFileRequest(detail::FileOf(storage_),
                                     detail::RequestOf(requestObject.storage_), offset, destination,
                                     nullptr, bytes, false, callback, callbackContext);
}

// ---- AsyncWriteFile ----

AsyncWriteFile::AsyncWriteFile() noexcept {
    static_assert(sizeof(AsyncFileState) <= sizeof(storage_));
    static_assert(alignof(AsyncFileState) <= 16);
    ::new (storage_) AsyncFileState{};
}

AsyncWriteFile::~AsyncWriteFile() noexcept {
    detail::DestroyFileState(detail::FileOf(storage_),
                             "AsyncWriteFile close failed during destruction");
}

FileStatus AsyncWriteFile::Open(AsyncIoContext& context, const char* path,
                                FileOpenMode mode) noexcept {
    return Open(context, path, detail::PathLength(path), mode);
}

FileStatus AsyncWriteFile::Open(AsyncIoContext& contextObject, const char* path, usize pathBytes,
                                FileOpenMode mode) noexcept {
    return detail::OpenFileState(detail::FileOf(storage_),
                                 detail::ContextOf(contextObject.storage_), path, pathBytes, true,
                                 mode);
}

FileStatus AsyncWriteFile::Close() noexcept {
    return detail::CloseFileState(detail::FileOf(storage_));
}

bool AsyncWriteFile::IsOpen() const noexcept {
    return detail::FileOf(storage_).open;
}

FileStatus AsyncWriteFile::Size(uint64& bytes) const noexcept {
    return detail::SizeFileState(detail::FileOf(storage_), bytes);
}

FileStatus AsyncWriteFile::Flush() noexcept {
    AsyncFileState& file = detail::FileOf(storage_);
    if (!file.open) {
        return {FileCode::NotOpen, 0};
    }
    {
        LockGuard lock(file.context->mutex);
        if (file.pending != 0) {
            return {FileCode::Busy, 0};
        }
    }
    return detail::AsyncFlushNative(file.handle);
}

FileStatus AsyncWriteFile::SubmitWrite(AsyncIoRequest& requestObject, uint64 offset,
                                       const void* source, usize bytes, AsyncIoCallback callback,
                                       void* callbackContext) noexcept {
    return detail::SubmitFileRequest(detail::FileOf(storage_),
                                     detail::RequestOf(requestObject.storage_), offset, nullptr,
                                     source, bytes, true, callback, callbackContext);
}

// ---- AsyncIoRequest ----

AsyncIoRequest::AsyncIoRequest() noexcept {
    static_assert(sizeof(AsyncRequestState) <= sizeof(storage_));
    static_assert(alignof(AsyncRequestState) <= 16);
    ::new (storage_) AsyncRequestState{};
}

AsyncIoRequest::~AsyncIoRequest() noexcept {
    AsyncRequestState& request = detail::RequestOf(storage_);
    if (request.context != nullptr) {
        bool inFlight = false;
        {
            LockGuard lock(request.context->mutex);
            inFlight = detail::IsInFlight(request);
        }
        if (inFlight) {
            Cancel();
            if (Wait() != WaitResult::Signaled) {
                Fatal("AsyncIoRequest destruction wait failed");
            }
        }
    }
    request.~AsyncRequestState();
}

bool AsyncIoRequest::IsComplete() const noexcept {
    const AsyncRequestState& request = detail::RequestOf(storage_);
    if (request.context == nullptr) {
        return false;
    }
    LockGuard lock(request.context->mutex);
    return request.phase == AsyncPhase::Completed;
}

WaitResult AsyncIoRequest::Wait(uint32 timeoutMs) noexcept {
    AsyncRequestState& request = detail::RequestOf(storage_);
    if (request.context == nullptr) {
        return WaitResult::Failed;
    }
    detail::WaitDeadline deadline(timeoutMs);
    AsyncContextState& context = *request.context;
    LockGuard lock(context.mutex);
    while (request.phase != AsyncPhase::Completed) {
        const uint32 remaining = deadline.RemainingMilliseconds();
        if (remaining == 0) {
            return WaitResult::TimedOut;
        }
        if (context.changed.Wait(context.mutex, remaining) == WaitResult::Failed) {
            return WaitResult::Failed;
        }
    }
    return WaitResult::Signaled;
}

void AsyncIoRequest::Cancel() noexcept {
    AsyncRequestState& request = detail::RequestOf(storage_);
    if (request.context == nullptr) {
        return;
    }
    AsyncContextState& context = *request.context;
    LockGuard lock(context.mutex);
    if (!detail::IsInFlight(request)) {
        return;
    }
    request.cancelRequested = true;
    if (context.backend == AsyncIoBackend::Native && !request.submitting) {
        detail::AsyncNativeCancel(request);
    }
    // WorkerPool의 대기 요청은 worker가 꺼낼 때 취소로 완료한다. 실행 중 I/O는 그대로 끝난다.
}

AsyncIoResult AsyncIoRequest::Result() const noexcept {
    const AsyncRequestState& request = detail::RequestOf(storage_);
    if (request.context == nullptr) {
        Fatal("AsyncIoRequest::Result called before completion");
    }
    LockGuard lock(request.context->mutex);
    if (request.phase != AsyncPhase::Completed) {
        Fatal("AsyncIoRequest::Result called before completion");
    }
    return request.result;
}

} // namespace rocks::platform
