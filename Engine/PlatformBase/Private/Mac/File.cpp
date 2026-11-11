#include <Platform/File.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include "../Common/FileInternal.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <new>

namespace rocks::platform {
namespace {

FileStatus NativeError(int error) noexcept {
    FileCode code = FileCode::SystemError;
    switch (error) {
    case ENOENT:
    case ENOTDIR:
        code = FileCode::NotFound;
        break;
    case EEXIST:
        code = FileCode::AlreadyExists;
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
    }
    return {code, static_cast<uint32>(error)};
}

class NativePath {
public:
    ~NativePath() noexcept {
        AlignedFree(value_);
    }
    FileStatus Initialize(const char* path, usize bytes) noexcept {
        if (!detail::ValidFilePath(path, bytes)) {
            return {FileCode::InvalidArgument, 0};
        }
        if (bytes == MaxSize) {
            return {FileCode::Overflow, 0};
        }
        value_ = static_cast<char*>(AlignedAllocate(bytes + 1, alignof(char)));
        if (!value_) {
            return {FileCode::OutOfMemory, 0};
        }
        for (usize i = 0; i < bytes; ++i) {
            value_[i] = path[i];
        }
        value_[bytes] = '\0';
        return {};
    }
    const char* Value() const noexcept {
        return value_;
    }

private:
    char* value_ = nullptr;
};

class MacFileHandle final : public FileHandle {
public:
    explicit MacFileHandle(int handle) noexcept : handle_(handle) {
    }
    FileTransfer Read(void* destination, usize bytes) noexcept override {
        if (bytes == 0) {
            return {{}, 0};
        }
        const usize amount =
            bytes > static_cast<usize>(SSIZE_MAX) ? static_cast<usize>(SSIZE_MAX) : bytes;
        ssize_t count;
        do {
            count = read(handle_, destination, amount);
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            return {NativeError(errno), 0};
        }
        return {{}, static_cast<usize>(count)};
    }
    FileTransfer Write(const void* source, usize bytes) noexcept override {
        if (bytes == 0) {
            return {{}, 0};
        }
        const usize amount =
            bytes > static_cast<usize>(SSIZE_MAX) ? static_cast<usize>(SSIZE_MAX) : bytes;
        ssize_t count;
        do {
            count = write(handle_, source, amount);
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            return {NativeError(errno), 0};
        }
        return {{}, static_cast<usize>(count)};
    }
    FileStatus Seek(int64 offset, FileSeekOrigin origin, uint64& position) noexcept override {
        static_assert(sizeof(off_t) >= sizeof(int64));
        int whence = SEEK_SET;
        if (origin == FileSeekOrigin::Current) {
            whence = SEEK_CUR;
        } else if (origin == FileSeekOrigin::End) {
            whence = SEEK_END;
        } else if (origin != FileSeekOrigin::Begin) {
            return {FileCode::InvalidArgument, 0};
        }
        const off_t result = lseek(handle_, static_cast<off_t>(offset), whence);
        if (result < 0) {
            return NativeError(errno);
        }
        position = static_cast<uint64>(result);
        return {};
    }
    FileStatus Size(uint64& bytes) noexcept override {
        struct stat state{};
        if (fstat(handle_, &state) != 0) {
            return NativeError(errno);
        }
        if (state.st_size < 0) {
            return {FileCode::SystemError, 0};
        }
        bytes = static_cast<uint64>(state.st_size);
        return {};
    }
    FileStatus Close() noexcept override {
        const int value = handle_;
        handle_ = -1;
        // EINTR에서도 이전 번호 재시도는 다른 스레드의 새 fd를 닫을 수 있어 금지한다.
        if (close(value) != 0) {
            return NativeError(errno);
        }
        return {};
    }
    void Destroy() noexcept override {
        this->~MacFileHandle();
        AlignedFree(this);
    }

private:
    int handle_;
};

class MacFileSystem final : public FileSystem {
public:
    FileStatus Open(const char* path, usize bytes, FileOpenMode mode,
                    FileHandle*& out) noexcept override {
        if (out != nullptr) {
            return {FileCode::InvalidArgument, 0};
        }
        int flags = O_CLOEXEC | O_NONBLOCK;
        switch (mode) {
        case FileOpenMode::ReadExisting:
            flags |= O_RDONLY;
            break;
        case FileOpenMode::ReadWriteExisting:
            flags |= O_RDWR;
            break;
        case FileOpenMode::CreateNew:
            flags |= O_RDWR | O_CREAT | O_EXCL;
            break;
        case FileOpenMode::CreateAlways:
            flags |= O_RDWR | O_CREAT | O_TRUNC;
            break;
        default:
            return {FileCode::InvalidArgument, 0};
        }
        NativePath native;
        auto valid = native.Initialize(path, bytes);
        if (!valid) {
            return valid;
        }
        void* storage = AlignedAllocate(sizeof(MacFileHandle), alignof(MacFileHandle));
        if (!storage) {
            return {FileCode::OutOfMemory, 0};
        }
        int handle;
        do {
            handle = open(native.Value(), flags, 0600);
        } while (handle < 0 && errno == EINTR);
        if (handle < 0) {
            const auto failure = NativeError(errno);
            AlignedFree(storage);
            return failure;
        }
        struct stat state{};
        if (fstat(handle, &state) != 0) {
            const auto failure = NativeError(errno);
            if (close(handle) != 0) {
                Fatal("macOS file open rollback failed");
            }
            AlignedFree(storage);
            return failure;
        }
        if (!S_ISREG(state.st_mode)) {
            if (close(handle) != 0) {
                Fatal("macOS file open rollback failed");
            }
            AlignedFree(storage);
            return {FileCode::Unsupported, 0};
        }
        out = ::new (storage) MacFileHandle(handle);
        return {};
    }
    FileStatus Exists(const char* path, usize bytes, bool& exists) noexcept override {
        NativePath native;
        auto valid = native.Initialize(path, bytes);
        if (!valid) {
            return valid;
        }
        struct stat state{};
        if (stat(native.Value(), &state) != 0) {
            const int error = errno;
            if (error == ENOENT || error == ENOTDIR) {
                exists = false;
                return {};
            }
            return NativeError(error);
        }
        exists = S_ISREG(state.st_mode);
        return {};
    }
    FileStatus Delete(const char* path, usize bytes) noexcept override {
        NativePath native;
        auto valid = native.Initialize(path, bytes);
        if (!valid) {
            return valid;
        }
        if (unlink(native.Value()) != 0) {
            return NativeError(errno);
        }
        return {};
    }
};
} // namespace

FileSystem& PhysicalFileSystem() noexcept {
    static MacFileSystem system;
    return system;
}
} // namespace rocks::platform
