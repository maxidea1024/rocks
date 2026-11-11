#include "../Common/AsyncFileInternal.h"
#include "WindowsFileCommon.h"

#include <Platform/Fatal.h>

#include <new>

// Windows backend다. Native는 FILE_FLAG_OVERLAPPED 핸들과 IO completion port, 전용 완료 스레드를
// 사용한다. 모든 결과(즉시 실패 포함)는 completion packet으로 완료 스레드에 전달하여 callback 실행
// 스레드를 하나로 고정한다. WorkerPool은 동기 핸들에 OVERLAPPED offset을 지정한 위치 지정
// 읽기·쓰기를 쓴다.
//
// OVERLAPPED는 요청 레코드 안에 있으며 OS가 completion packet을 큐에 넣고 완료 스레드가 꺼낼 때까지
// 주소와 수명이 유지되어야 한다. CancelIoEx는 취소를 요청할 뿐이며 완료 packet은 여전히 도착한다.
namespace rocks::platform::detail {
namespace {

constexpr ULONG_PTR ShutdownKey = 1;

struct NativeContext {
    HANDLE port;
};

struct NativeRequest {
    OVERLAPPED overlapped;
    AsyncRequestState* owner;
    // 즉시 실패를 packet으로 보낼 때의 Win32 오류다. 0이면 GetQueuedCompletionStatus 결과를 쓴다.
    DWORD presetError;
};

static_assert(sizeof(NativeContext) <= sizeof(AsyncContextState::native));
static_assert(sizeof(NativeRequest) <= sizeof(AsyncRequestState::native));
static_assert(alignof(NativeRequest) <= 16 && alignof(NativeContext) <= 16);

NativeContext& NativeOf(AsyncContextState& context) noexcept {
    return *__builtin_launder(reinterpret_cast<NativeContext*>(context.native));
}

NativeRequest& NativeOf(AsyncRequestState& request) noexcept {
    return *__builtin_launder(reinterpret_cast<NativeRequest*>(request.native));
}

HANDLE HandleOf(uptr handle) noexcept {
    return reinterpret_cast<HANDLE>(handle);
}

// 완료 결과를 Rocks 결과로 바꾼다. EOF는 0 byte 성공, 취소는 Cancelled다.
AsyncIoResult ToResult(DWORD error, DWORD transferred) noexcept {
    AsyncIoResult result{};
    result.bytes = transferred;
    if (error == ERROR_SUCCESS || error == ERROR_HANDLE_EOF) {
        return result;
    }
    if (error == ERROR_OPERATION_ABORTED) {
        result.status = {FileCode::Cancelled, static_cast<uint32>(error)};
        return result;
    }
    result.status = NativeError(error);
    return result;
}

void CompletionEntry(void* raw) noexcept {
    auto& context = *static_cast<AsyncContextState*>(raw);
    const HANDLE port = NativeOf(context).port;
    for (;;) {
        DWORD transferred = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* overlapped = nullptr;
        const BOOL dequeued =
            GetQueuedCompletionStatus(port, &transferred, &key, &overlapped, INFINITE);
        const DWORD error = dequeued ? ERROR_SUCCESS : GetLastError();
        if (overlapped == nullptr) {
            if (dequeued && key == ShutdownKey) {
                return;
            }
            Fatal("AsyncIoContext completion port wait failed");
        }
        // OVERLAPPED는 NativeRequest의 첫 멤버다.
        auto* native = reinterpret_cast<NativeRequest*>(overlapped);
        AsyncRequestState& request = *native->owner;
        const DWORD finalError = native->presetError != 0 ? native->presetError : error;
        AsyncIoResult result = ToResult(finalError, transferred);
        // 쓰기는 짧은 성공을 허용하지 않는다. overlapped 파일 쓰기는 보통 전부 끝나거나 오류지만,
        // 일부만 끝난 성공이 오면 실패로 드러낸다.
        if (request.write && result.status && result.bytes != request.bytes) {
            result.status = {result.bytes == 0 ? FileCode::NoProgress : FileCode::SystemError, 0};
        }
        CompleteAsyncRequest(request, result);
    }
}

} // namespace

AsyncIoBackend PlatformDefaultAsyncIoBackend() noexcept {
    return AsyncIoBackend::Native;
}

FileStatus AsyncOpenNative(const char* path, usize pathBytes, bool overlapped,
                           uptr& handle) noexcept {
    WidePath native;
    auto valid = native.Initialize(path, pathBytes);
    if (!valid) {
        return valid;
    }
    const DWORD flags = FILE_ATTRIBUTE_NORMAL | (overlapped ? FILE_FLAG_OVERLAPPED : 0);
    HANDLE file = CreateFileW(native.Value(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, flags, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return NativeError(GetLastError());
    }
    // 일반 디스크 파일만 허용한다. FILE_TYPE_UNKNOWN은 오류와 구분하기 위해 마지막 오류를 지운다.
    SetLastError(ERROR_SUCCESS);
    const DWORD type = GetFileType(file);
    const DWORD typeError = GetLastError();
    if (type != FILE_TYPE_DISK) {
        if (!CloseHandle(file)) {
            Fatal("AsyncReadFile open rollback failed");
        }
        return typeError != ERROR_SUCCESS ? NativeError(typeError)
                                          : FileStatus{FileCode::Unsupported, 0};
    }
    handle = reinterpret_cast<uptr>(file);
    return {};
}

FileStatus AsyncOpenWriteNative(const char* path, usize pathBytes, bool overlapped,
                                FileOpenMode mode, uptr& handle) noexcept {
    DWORD disposition = OPEN_EXISTING;
    switch (mode) {
    case FileOpenMode::ReadWriteExisting:
        break;
    case FileOpenMode::CreateNew:
        disposition = CREATE_NEW;
        break;
    case FileOpenMode::CreateAlways:
        disposition = CREATE_ALWAYS;
        break;
    default:
        return {FileCode::InvalidArgument, 0};
    }
    WidePath native;
    auto valid = native.Initialize(path, pathBytes);
    if (!valid) {
        return valid;
    }
    // 동기 File과 같은 공유 모드다. Size를 위해 읽기 권한도 함께 연다.
    const DWORD flags = FILE_ATTRIBUTE_NORMAL | (overlapped ? FILE_FLAG_OVERLAPPED : 0);
    HANDLE file = CreateFileW(native.Value(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              disposition, flags, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return NativeError(GetLastError());
    }
    SetLastError(ERROR_SUCCESS);
    const DWORD type = GetFileType(file);
    const DWORD typeError = GetLastError();
    if (type != FILE_TYPE_DISK) {
        if (!CloseHandle(file)) {
            Fatal("AsyncWriteFile open rollback failed");
        }
        return typeError != ERROR_SUCCESS ? NativeError(typeError)
                                          : FileStatus{FileCode::Unsupported, 0};
    }
    handle = reinterpret_cast<uptr>(file);
    return {};
}

FileStatus AsyncFlushNative(uptr handle) noexcept {
    if (!FlushFileBuffers(HandleOf(handle))) {
        return NativeError(GetLastError());
    }
    return {};
}

FileStatus AsyncCloseNative(uptr handle) noexcept {
    if (!CloseHandle(HandleOf(handle))) {
        return NativeError(GetLastError());
    }
    return {};
}

FileStatus AsyncSizeNative(uptr handle, uint64& bytes) noexcept {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(HandleOf(handle), &size) || size.QuadPart < 0) {
        return NativeError(GetLastError());
    }
    bytes = static_cast<uint64>(size.QuadPart);
    return {};
}

FileStatus AsyncPositionalRead(uptr handle, uint64 offset, void* destination, usize bytes,
                               usize& transferred) noexcept {
    transferred = 0;
    auto* output = static_cast<unsigned char*>(destination);
    while (transferred < bytes) {
        const usize remaining = bytes - transferred;
        const DWORD chunk = remaining > 0x7ffff000u ? 0x7ffff000u : static_cast<DWORD>(remaining);
        const uint64 position = offset + transferred;
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(position);
        overlapped.OffsetHigh = static_cast<DWORD>(position >> 32);
        DWORD read = 0;
        if (!ReadFile(HandleOf(handle), output + transferred, chunk, &read, &overlapped)) {
            const DWORD error = GetLastError();
            if (error == ERROR_HANDLE_EOF) {
                return {};
            }
            return NativeError(error);
        }
        if (read == 0) {
            return {}; // EOF
        }
        transferred += read;
    }
    return {};
}

FileStatus AsyncPositionalWrite(uptr handle, uint64 offset, const void* source, usize bytes,
                                usize& transferred) noexcept {
    transferred = 0;
    const auto* input = static_cast<const unsigned char*>(source);
    while (transferred < bytes) {
        const usize remaining = bytes - transferred;
        const DWORD chunk = remaining > 0x7ffff000u ? 0x7ffff000u : static_cast<DWORD>(remaining);
        const uint64 position = offset + transferred;
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(position);
        overlapped.OffsetHigh = static_cast<DWORD>(position >> 32);
        DWORD written = 0;
        if (!WriteFile(HandleOf(handle), input + transferred, chunk, &written, &overlapped)) {
            return NativeError(GetLastError());
        }
        if (written == 0) {
            return {FileCode::NoProgress, 0};
        }
        transferred += written;
    }
    return {};
}

bool AsyncNativeSupported() noexcept {
    return true;
}

FileStatus AsyncNativeStart(AsyncContextState& context) noexcept {
    const HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (port == nullptr) {
        return NativeError(GetLastError());
    }
    ::new (context.native) NativeContext{port};
    if (context.completionThread.Start(CompletionEntry, &context) != ThreadStatus::Success) {
        if (!CloseHandle(port)) {
            Fatal("AsyncIoContext completion port rollback failed");
        }
        return {FileCode::SystemError, 0};
    }
    return {};
}

void AsyncNativeStop(AsyncContextState& context) noexcept {
    NativeContext& native = NativeOf(context);
    if (!PostQueuedCompletionStatus(native.port, 0, ShutdownKey, nullptr)) {
        Fatal("AsyncIoContext shutdown packet failed");
    }
    if (context.completionThread.Join() != ThreadStatus::Success) {
        Fatal("AsyncIoContext completion thread join failed");
    }
    if (!CloseHandle(native.port)) {
        Fatal("AsyncIoContext completion port close failed");
    }
    native.port = nullptr;
}

FileStatus AsyncNativeAttach(AsyncContextState& context, AsyncFileState& file) noexcept {
    if (CreateIoCompletionPort(HandleOf(file.handle), NativeOf(context).port, 0, 0) == nullptr) {
        return NativeError(GetLastError());
    }
    return {};
}

void AsyncNativeIssue(AsyncContextState& context, AsyncRequestState& request) noexcept {
    NativeRequest& native = *::new (request.native) NativeRequest{};
    native.owner = &request;
    native.overlapped.Offset = static_cast<DWORD>(request.offset);
    native.overlapped.OffsetHigh = static_cast<DWORD>(request.offset >> 32);
    if (request.bytes == 0) {
        // 0 byte 읽기·쓰기는 OS에 보내지 않고 같은 완료 경로로 성공을 통지한다.
        native.presetError = 0;
        if (!PostQueuedCompletionStatus(NativeOf(context).port, 0, 0, &native.overlapped)) {
            Fatal("AsyncReadFile completion packet post failed");
        }
        return;
    }
    const BOOL issued =
        request.write ? WriteFile(HandleOf(request.file->handle), request.source,
                                  static_cast<DWORD>(request.bytes), nullptr, &native.overlapped)
                      : ReadFile(HandleOf(request.file->handle), request.destination,
                                 static_cast<DWORD>(request.bytes), nullptr, &native.overlapped);
    if (issued) {
        return; // 동기 완료도 completion packet이 큐에 들어간다.
    }
    const DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING) {
        return;
    }
    // 즉시 실패는 packet이 생기지 않는다. 같은 완료 경로로 결과를 보낸다.
    native.presetError = error;
    if (!PostQueuedCompletionStatus(NativeOf(context).port, 0, 0, &native.overlapped)) {
        Fatal("AsyncReadFile completion packet post failed");
    }
}

void AsyncNativeCancel(AsyncRequestState& request) noexcept {
    NativeRequest& native = NativeOf(request);
    // ERROR_NOT_FOUND는 이미 완료되어 취소할 것이 없다는 뜻이다. 결과는 완료 packet으로 확인한다.
    CancelIoEx(HandleOf(request.file->handle), &native.overlapped);
}

void AsyncNativeCancelFile(AsyncFileState& file) noexcept {
    CancelIoEx(HandleOf(file.handle), nullptr);
}

} // namespace rocks::platform::detail
