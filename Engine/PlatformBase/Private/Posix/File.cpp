#include <Platform/File.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include "../Common/FileInternal.h"
#include "PosixFileError.h"
#include "PosixPlatform.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <new>

namespace rocks::platform {
namespace {

FileStatus NativeError(int error) noexcept {
    return detail::PosixFileError(error);
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

FileInfo InfoFromStat(const struct stat& state, bool isLink) noexcept {
    FileInfo info;
    info.type = S_ISREG(state.st_mode)   ? FileType::File
                : S_ISDIR(state.st_mode) ? FileType::Directory
                                         : FileType::Other;
    info.isLink = isLink;
    info.readOnly = (state.st_mode & S_IWUSR) == 0;
    info.size =
        info.type == FileType::File && state.st_size > 0 ? static_cast<uint64>(state.st_size) : 0;
    info.modifiedTicks = detail::PosixModifiedTicks(state);
    return info;
}

class PosixFileHandle final : public FileHandle {
public:
    explicit PosixFileHandle(int handle) noexcept : handle_(handle) {
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
        this->~PosixFileHandle();
        AlignedFree(this);
    }

private:
    int handle_;
};

class PosixFileSystem final : public FileSystem {
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
        void* storage = AlignedAllocate(sizeof(PosixFileHandle), alignof(PosixFileHandle));
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
                Fatal("POSIX file open rollback failed");
            }
            AlignedFree(storage);
            return failure;
        }
        if (!S_ISREG(state.st_mode)) {
            if (close(handle) != 0) {
                Fatal("POSIX file open rollback failed");
            }
            AlignedFree(storage);
            return {FileCode::Unsupported, 0};
        }
        out = ::new (storage) PosixFileHandle(handle);
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
            const int error = errno;
            // unlink는 디렉터리에 EPERM(macOS) 또는 EISDIR(Linux)을 낸다. 실제 디렉터리면
            // WrongType으로 알린다.
            struct stat state{};
            if ((error == EPERM || error == EISDIR) && lstat(native.Value(), &state) == 0 &&
                S_ISDIR(state.st_mode)) {
                return {FileCode::WrongType, static_cast<uint32>(error)};
            }
            return NativeError(error);
        }
        return {};
    }

    FileStatus Stat(const char* path, usize bytes, FileInfo& info) noexcept override {
        NativePath native;
        auto valid = native.Initialize(path, bytes);
        if (!valid) {
            return valid;
        }
        struct stat self{};
        if (lstat(native.Value(), &self) != 0) {
            return NativeError(errno);
        }
        struct stat target{};
        if (stat(native.Value(), &target) != 0) {
            return NativeError(errno); // 대상이 없는 link는 NotFound다.
        }
        info = InfoFromStat(target, S_ISLNK(self.st_mode));
        return {};
    }

    FileStatus MakeDirectory(const char* path, usize bytes) noexcept override {
        NativePath native;
        auto valid = native.Initialize(path, bytes);
        if (!valid) {
            return valid;
        }
        // 파일과 같이 소유자 전용 권한이다(파일 0600, 디렉터리 0700).
        if (mkdir(native.Value(), 0700) != 0) {
            return NativeError(errno);
        }
        return {};
    }

    FileStatus DeleteDirectory(const char* path, usize bytes) noexcept override {
        NativePath native;
        auto valid = native.Initialize(path, bytes);
        if (!valid) {
            return valid;
        }
        struct stat self{};
        if (lstat(native.Value(), &self) != 0) {
            return NativeError(errno);
        }
        if (S_ISLNK(self.st_mode)) {
            if (unlink(native.Value()) != 0) {
                return NativeError(errno);
            }
            return {};
        }
        if (!S_ISDIR(self.st_mode)) {
            return {FileCode::WrongType, 0};
        }
        if (rmdir(native.Value()) != 0) {
            const int error = errno;
            return error == EEXIST ? FileStatus{FileCode::DirectoryNotEmpty, ENOTEMPTY}
                                   : NativeError(error);
        }
        return {};
    }

    FileStatus EnumerateDirectory(const char* path, usize bytes, DirectoryVisitor visitor,
                                  void* context) noexcept override {
        if (visitor == nullptr) {
            return {FileCode::InvalidArgument, 0};
        }
        NativePath native;
        auto valid = native.Initialize(path, bytes);
        if (!valid) {
            return valid;
        }
        DIR* directory = opendir(native.Value());
        if (directory == nullptr) {
            const int error = errno;
            return error == ENOTDIR ? FileStatus{FileCode::WrongType, ENOTDIR} : NativeError(error);
        }
        FileStatus result{};
        for (;;) {
            errno = 0;
            const struct dirent* item = readdir(directory);
            if (item == nullptr) {
                if (errno != 0) {
                    result = NativeError(errno);
                }
                break;
            }
            const char* name = item->d_name;
            if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
                continue;
            }
            struct stat state{};
            if (fstatat(dirfd(directory), name, &state, AT_SYMLINK_NOFOLLOW) != 0) {
                const int error = errno;
                if (error == ENOENT) {
                    continue; // 열거 중 사라진 항목
                }
                result = NativeError(error);
                break;
            }
            DirectoryEntry entry;
            entry.name = name;
            usize nameBytes = 0;
            while (name[nameBytes] != '\0') {
                ++nameBytes;
            }
            entry.nameBytes = nameBytes;
            entry.info = InfoFromStat(state, S_ISLNK(state.st_mode));
            if (!visitor(entry, context)) {
                break;
            }
        }
        if (closedir(directory) != 0 && result) {
            result = NativeError(errno);
        }
        return result;
    }

    FileStatus Move(const char* from, usize fromBytes, const char* to, usize toBytes,
                    bool replaceExisting) noexcept override {
        NativePath source;
        auto valid = source.Initialize(from, fromBytes);
        if (!valid) {
            return valid;
        }
        NativePath target;
        valid = target.Initialize(to, toBytes);
        if (!valid) {
            return valid;
        }
        // 덮어쓰지 않는 이동은 대상이 있으면 원자적으로 실패한다(PosixRenameNoReplace).
        const int result = replaceExisting
                               ? rename(source.Value(), target.Value())
                               : detail::PosixRenameNoReplace(source.Value(), target.Value());
        if (result != 0) {
            return NativeError(errno);
        }
        return {};
    }

    FileStatus Copy(const char* from, usize fromBytes, const char* to, usize toBytes,
                    bool replaceExisting) noexcept override {
        NativePath source;
        auto valid = source.Initialize(from, fromBytes);
        if (!valid) {
            return valid;
        }
        NativePath target;
        valid = target.Initialize(to, toBytes);
        if (!valid) {
            return valid;
        }
        int input;
        do {
            input = open(source.Value(), O_RDONLY | O_CLOEXEC);
        } while (input < 0 && errno == EINTR);
        if (input < 0) {
            return NativeError(errno);
        }
        struct stat state{};
        if (fstat(input, &state) != 0 || !S_ISREG(state.st_mode)) {
            const FileStatus failure = S_ISDIR(state.st_mode) ? FileStatus{FileCode::WrongType, 0}
                                       : state.st_mode == 0   ? NativeError(errno)
                                                            : FileStatus{FileCode::Unsupported, 0};
            close(input);
            return failure;
        }
        const int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (replaceExisting ? O_TRUNC : O_EXCL);
        int output;
        do {
            output = open(target.Value(), flags, 0600);
        } while (output < 0 && errno == EINTR);
        if (output < 0) {
            const FileStatus failure = NativeError(errno);
            close(input);
            return failure;
        }
        FileStatus result{};
        char chunk[64 * 1024];
        for (;;) {
            const ssize_t read = ::read(input, chunk, sizeof(chunk));
            if (read < 0) {
                if (errno == EINTR) {
                    continue;
                }
                result = NativeError(errno);
                break;
            }
            if (read == 0) {
                break;
            }
            ssize_t done = 0;
            while (done < read) {
                const ssize_t written =
                    ::write(output, chunk + done, static_cast<usize>(read - done));
                if (written < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    result = NativeError(errno);
                    break;
                }
                if (written == 0) {
                    result = {FileCode::NoProgress, 0};
                    break;
                }
                done += written;
            }
            if (!result) {
                break;
            }
        }
        if (close(output) != 0 && result) {
            result = NativeError(errno);
        }
        close(input);
        if (!result) {
            unlink(target.Value()); // 실패한 복사본을 남기지 않는다.
        }
        return result;
    }
};
} // namespace

FileSystem& PhysicalFileSystem() noexcept {
    static PosixFileSystem system;
    return system;
}
} // namespace rocks::platform
