#include "../Common/AsyncFileInternal.h"
#include "../Common/FileInternal.h"

#include <Platform/Fatal.h>
#include <Platform/Memory.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// macOS backend다. 커널 비동기 I/O(dispatch I/O·POSIX AIO)는 일반 파일에서 요청별 취소·순서·성능을
// 실기기로 검증하기 전까지 사용하지 않고, 공용 WorkerPool이 pread/pwrite로 위치 지정 읽기·쓰기를
// 수행한다.
// 이 파일의 POSIX 호출(open/fstat/pread/close)은 iOS·Android backend에서도 같은 의미로 재사용할
// 수 있도록 Apple 전용 API를 쓰지 않는다. 오류 변환과 경로 복사는 Mac/File.cpp와 같은 규칙이며,
// POSIX 공용 계층을 도입할 때 하나로 합친다.
// 주의: 이 backend는 macOS 실기기에서 아직 빌드·실행하지 않았다.
namespace rocks::platform::detail {
namespace {

FileStatus PosixError(int error) noexcept {
    FileCode code = FileCode::SystemError;
    switch (error) {
    case ENOENT:
    case ENOTDIR:
        code = FileCode::NotFound;
        break;
    case EACCES:
    case EPERM:
        code = FileCode::AccessDenied;
        break;
    case EINVAL:
        code = FileCode::InvalidArgument;
        break;
    case ENOMEM:
        code = FileCode::OutOfMemory;
        break;
    case EOVERFLOW:
        code = FileCode::Overflow;
        break;
    case ENOTSUP:
        code = FileCode::Unsupported;
        break;
    default:
        break;
    }
    return {code, static_cast<uint32>(error)};
}

int DescriptorOf(uptr handle) noexcept {
    return static_cast<int>(handle);
}

} // namespace

AsyncIoBackend PlatformDefaultAsyncIoBackend() noexcept {
    return AsyncIoBackend::WorkerPool;
}

FileStatus AsyncOpenNative(const char* path, usize pathBytes, bool overlapped,
                           uptr& handle) noexcept {
    if (overlapped) {
        return {FileCode::Unsupported, 0};
    }
    if (!ValidFilePath(path, pathBytes) || pathBytes == MaxSize) {
        return {FileCode::InvalidArgument, 0};
    }
    auto* native = static_cast<char*>(AlignedAllocate(pathBytes + 1, alignof(char)));
    if (native == nullptr) {
        return {FileCode::OutOfMemory, 0};
    }
    for (usize index = 0; index < pathBytes; ++index) {
        native[index] = path[index];
    }
    native[pathBytes] = '\0';
    int descriptor;
    do {
        descriptor = open(native, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    } while (descriptor < 0 && errno == EINTR);
    const int openError = errno;
    AlignedFree(native);
    if (descriptor < 0) {
        return PosixError(openError);
    }
    struct stat state{};
    if (fstat(descriptor, &state) != 0) {
        const int statError = errno;
        if (close(descriptor) != 0) {
            Fatal("macOS async file open rollback failed");
        }
        return PosixError(statError);
    }
    if (!S_ISREG(state.st_mode)) {
        if (close(descriptor) != 0) {
            Fatal("macOS async file open rollback failed");
        }
        return {FileCode::Unsupported, 0};
    }
    handle = static_cast<uptr>(descriptor);
    return {};
}

FileStatus AsyncOpenWriteNative(const char* path, usize pathBytes, bool overlapped,
                                FileOpenMode mode, uptr& handle) noexcept {
    if (overlapped) {
        return {FileCode::Unsupported, 0};
    }
    int flags = O_RDWR | O_CLOEXEC | O_NONBLOCK;
    switch (mode) {
    case FileOpenMode::ReadWriteExisting:
        break;
    case FileOpenMode::CreateNew:
        flags |= O_CREAT | O_EXCL;
        break;
    case FileOpenMode::CreateAlways:
        flags |= O_CREAT | O_TRUNC;
        break;
    default:
        return {FileCode::InvalidArgument, 0};
    }
    if (!ValidFilePath(path, pathBytes) || pathBytes == MaxSize) {
        return {FileCode::InvalidArgument, 0};
    }
    auto* native = static_cast<char*>(AlignedAllocate(pathBytes + 1, alignof(char)));
    if (native == nullptr) {
        return {FileCode::OutOfMemory, 0};
    }
    for (usize index = 0; index < pathBytes; ++index) {
        native[index] = path[index];
    }
    native[pathBytes] = '\0';
    int descriptor;
    do {
        // 동기 File(Mac/File.cpp)과 같은 권한이다.
        descriptor = open(native, flags, 0600);
    } while (descriptor < 0 && errno == EINTR);
    const int openError = errno;
    AlignedFree(native);
    if (descriptor < 0) {
        return PosixError(openError);
    }
    struct stat state{};
    if (fstat(descriptor, &state) != 0) {
        const int statError = errno;
        if (close(descriptor) != 0) {
            Fatal("macOS async write file open rollback failed");
        }
        return PosixError(statError);
    }
    if (!S_ISREG(state.st_mode)) {
        if (close(descriptor) != 0) {
            Fatal("macOS async write file open rollback failed");
        }
        return {FileCode::Unsupported, 0};
    }
    handle = static_cast<uptr>(descriptor);
    return {};
}

// F_FULLFSYNC는 장치 캐시까지 내린다. 지원하지 않는 파일 시스템이면 fsync로 대신한다.
FileStatus AsyncFlushNative(uptr handle) noexcept {
    if (fcntl(DescriptorOf(handle), F_FULLFSYNC) == 0) {
        return {};
    }
    if (errno != ENOTSUP && errno != EINVAL) {
        return PosixError(errno);
    }
    if (fsync(DescriptorOf(handle)) != 0) {
        return PosixError(errno);
    }
    return {};
}

FileStatus AsyncCloseNative(uptr handle) noexcept {
    // close의 EINTR 이후 descriptor 상태는 보장되지 않으므로 재시도하지 않는다.
    if (close(DescriptorOf(handle)) != 0) {
        return PosixError(errno);
    }
    return {};
}

FileStatus AsyncSizeNative(uptr handle, uint64& bytes) noexcept {
    struct stat state{};
    if (fstat(DescriptorOf(handle), &state) != 0) {
        return PosixError(errno);
    }
    if (state.st_size < 0) {
        return {FileCode::SystemError, 0};
    }
    bytes = static_cast<uint64>(state.st_size);
    return {};
}

FileStatus AsyncPositionalRead(uptr handle, uint64 offset, void* destination, usize bytes,
                               usize& transferred) noexcept {
    transferred = 0;
    auto* output = static_cast<unsigned char*>(destination);
    while (transferred < bytes) {
        const uint64 position = offset + transferred;
        if (position > static_cast<uint64>(0x7fffffffffffffffll)) {
            return {FileCode::Overflow, 0};
        }
        const usize remaining = bytes - transferred;
        const usize chunk = remaining > 0x7ffff000u ? 0x7ffff000u : remaining;
        const ssize_t read =
            pread(DescriptorOf(handle), output + transferred, chunk, static_cast<off_t>(position));
        if (read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return PosixError(errno);
        }
        if (read == 0) {
            return {}; // EOF
        }
        transferred += static_cast<usize>(read);
    }
    return {};
}

FileStatus AsyncPositionalWrite(uptr handle, uint64 offset, const void* source, usize bytes,
                                usize& transferred) noexcept {
    transferred = 0;
    const auto* input = static_cast<const unsigned char*>(source);
    while (transferred < bytes) {
        const uint64 position = offset + transferred;
        if (position > static_cast<uint64>(0x7fffffffffffffffll)) {
            return {FileCode::Overflow, 0};
        }
        const usize remaining = bytes - transferred;
        const usize chunk = remaining > 0x7ffff000u ? 0x7ffff000u : remaining;
        const ssize_t written =
            pwrite(DescriptorOf(handle), input + transferred, chunk, static_cast<off_t>(position));
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return PosixError(errno);
        }
        if (written == 0) {
            return {FileCode::NoProgress, 0};
        }
        transferred += static_cast<usize>(written);
    }
    return {};
}

bool AsyncNativeSupported() noexcept {
    return false;
}

FileStatus AsyncNativeStart(AsyncContextState&) noexcept {
    return {FileCode::Unsupported, 0};
}

void AsyncNativeStop(AsyncContextState&) noexcept {
}

FileStatus AsyncNativeAttach(AsyncContextState&, AsyncFileState&) noexcept {
    return {FileCode::Unsupported, 0};
}

void AsyncNativeIssue(AsyncContextState&, AsyncRequestState&) noexcept {
    Fatal("macOS native async I/O is not supported");
}

void AsyncNativeCancel(AsyncRequestState&) noexcept {
}

void AsyncNativeCancelFile(AsyncFileState&) noexcept {
}

} // namespace rocks::platform::detail
