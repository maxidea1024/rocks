#include <Platform/File.h>
#include <Platform/Fatal.h>
#include "FileInternal.h"

namespace rocks::platform {
namespace {

struct ActiveFile {
    explicit ActiveFile(const File* value) noexcept : file(value) {
        for (auto* item = active; item; item = item->previous) {
            if (item->file == file) {
                return;
            }
        }
        previous = active;
        active = this;
        entered = true;
    }

    ~ActiveFile() noexcept {
        if (entered) {
            active = previous;
        }
    }

    const File* file;
    ActiveFile* previous = nullptr;
    bool entered = false;
    static thread_local ActiveFile* active;
};
thread_local ActiveFile* ActiveFile::active = nullptr;

FileStatus Error(FileCode code) noexcept {
    return {code, 0};
}

bool ValidMode(FileOpenMode mode) noexcept {
    return mode == FileOpenMode::ReadExisting || mode == FileOpenMode::ReadWriteExisting ||
           mode == FileOpenMode::CreateNew || mode == FileOpenMode::CreateAlways;
}

bool ValidOrigin(FileSeekOrigin origin) noexcept {
    return origin == FileSeekOrigin::Begin || origin == FileSeekOrigin::Current ||
           origin == FileSeekOrigin::End;
}

} // namespace

namespace detail {
bool ValidFilePath(const char* path, usize bytes) noexcept {
    if (!path || bytes == 0) {
        return false;
    }
    usize i = 0;
    while (i < bytes) {
        const auto first = static_cast<uint8>(path[i]);
        if (first == 0) {
            return false;
        }
        if (first < 0x80) {
            ++i;
            continue;
        }
        usize length = 0;
        if (first >= 0xC2 && first <= 0xDF) {
            length = 2;
        } else if (first >= 0xE0 && first <= 0xEF) {
            length = 3;
        } else if (first >= 0xF0 && first <= 0xF4) {
            length = 4;
        } else {
            return false;
        }
        if (length > bytes - i) {
            return false;
        }
        const auto second = static_cast<uint8>(path[i + 1]);
        if (second < 0x80 || second > 0xBF || (first == 0xE0 && second < 0xA0) ||
            (first == 0xED && second >= 0xA0) || (first == 0xF0 && second < 0x90) ||
            (first == 0xF4 && second >= 0x90)) {
            return false;
        }
        for (usize j = 2; j < length; ++j) {
            const auto next = static_cast<uint8>(path[i + j]);
            if (next < 0x80 || next > 0xBF) {
                return false;
            }
        }
        i += length;
    }
    return true;
}
} // namespace detail

File::~File() noexcept {
    if (!Close()) {
        Fatal("File close failed during destruction");
    }
}

FileStatus File::Open(const char* path, FileOpenMode mode) noexcept {
    return Open(PhysicalFileSystem(), path, mode);
}

FileStatus File::Open(FileSystem& system, const char* path, FileOpenMode mode) noexcept {
    if (!path) {
        // 같은 File의 callback 재진입은 인자 오류보다 먼저 Busy를 반환한다.
        return Open(system, path, 0, mode);
    }

    usize bytes = 0;
    while (path[bytes] != '\0') {
        if (bytes == MaxSize - 1) {
            return Error(FileCode::Overflow);
        }
        ++bytes;
    }
    return Open(system, path, bytes, mode);
}

FileStatus File::Open(FileSystem& system, const char* path, usize pathBytes,
                      FileOpenMode mode) noexcept {
    ActiveFile entry(this);
    if (!entry.entered) {
        return Error(FileCode::Busy);
    }
    if (!ValidMode(mode) || !detail::ValidFilePath(path, pathBytes)) {
        return Error(FileCode::InvalidArgument);
    }
    LockGuard lock(mutex_);
    if (handle_) {
        return Error(FileCode::AlreadyOpen);
    }
    FileHandle* opened = nullptr;
    const auto result = system.Open(path, pathBytes, mode, opened);
    if ((!result && opened) || (result && !opened)) {
        Fatal("FileSystem Open violated handle ownership contract");
    }
    if (result) {
        handle_ = opened;
    }
    return result;
}

FileTransfer File::Read(void* destination, usize bytes) noexcept {
    ActiveFile entry(this);
    if (!entry.entered) {
        return {Error(FileCode::Busy), 0};
    }
    if (!destination && bytes != 0) {
        return {Error(FileCode::InvalidArgument), 0};
    }
    LockGuard lock(mutex_);
    if (!handle_) {
        return {Error(FileCode::NotOpen), 0};
    }
    auto result = handle_->Read(destination, bytes);
    if (result.bytes > bytes) {
        Fatal("FileHandle reported more bytes than requested");
    }
    return result;
}

FileTransfer File::Write(const void* source, usize bytes) noexcept {
    ActiveFile entry(this);
    if (!entry.entered) {
        return {Error(FileCode::Busy), 0};
    }
    if (!source && bytes != 0) {
        return {Error(FileCode::InvalidArgument), 0};
    }
    LockGuard lock(mutex_);
    if (!handle_) {
        return {Error(FileCode::NotOpen), 0};
    }
    auto result = handle_->Write(source, bytes);
    if (result.bytes > bytes) {
        Fatal("FileHandle reported more bytes than requested");
    }
    return result;
}

FileTransfer File::ReadExact(void* destination, usize bytes) noexcept {
    ActiveFile entry(this);
    if (!entry.entered) {
        return {Error(FileCode::Busy), 0};
    }
    if (!destination && bytes != 0) {
        return {Error(FileCode::InvalidArgument), 0};
    }
    LockGuard lock(mutex_);
    if (!handle_) {
        return {Error(FileCode::NotOpen), 0};
    }
    auto* target = static_cast<uint8*>(destination);
    usize total = 0;
    while (total < bytes) {
        const auto result = handle_->Read(target + total, bytes - total);
        if (result.bytes > bytes - total) {
            Fatal("FileHandle reported more bytes than requested");
        }
        total += result.bytes;
        if (!result.status) {
            return {result.status, total};
        }
        if (result.bytes == 0) {
            return {Error(FileCode::EndOfFile), total};
        }
    }
    return {{}, total};
}

FileTransfer File::WriteAll(const void* source, usize bytes) noexcept {
    ActiveFile entry(this);
    if (!entry.entered) {
        return {Error(FileCode::Busy), 0};
    }
    if (!source && bytes != 0) {
        return {Error(FileCode::InvalidArgument), 0};
    }
    LockGuard lock(mutex_);
    if (!handle_) {
        return {Error(FileCode::NotOpen), 0};
    }
    const auto* data = static_cast<const uint8*>(source);
    usize total = 0;
    while (total < bytes) {
        const auto result = handle_->Write(data + total, bytes - total);
        if (result.bytes > bytes - total) {
            Fatal("FileHandle reported more bytes than requested");
        }
        total += result.bytes;
        if (!result.status) {
            return {result.status, total};
        }
        if (result.bytes == 0) {
            return {Error(FileCode::NoProgress), total};
        }
    }
    return {{}, total};
}

FileStatus File::Seek(int64 offset, FileSeekOrigin origin, uint64& position) noexcept {
    ActiveFile entry(this);
    if (!entry.entered) {
        return Error(FileCode::Busy);
    }
    if (!ValidOrigin(origin)) {
        return Error(FileCode::InvalidArgument);
    }
    LockGuard lock(mutex_);
    if (!handle_) {
        return Error(FileCode::NotOpen);
    }
    uint64 next = 0;
    auto result = handle_->Seek(offset, origin, next);
    if (result) {
        position = next;
    }
    return result;
}

FileStatus File::Size(uint64& bytes) noexcept {
    ActiveFile entry(this);
    if (!entry.entered) {
        return Error(FileCode::Busy);
    }
    LockGuard lock(mutex_);
    if (!handle_) {
        return Error(FileCode::NotOpen);
    }
    uint64 size = 0;
    auto result = handle_->Size(size);
    if (result) {
        bytes = size;
    }
    return result;
}

FileStatus File::Close() noexcept {
    ActiveFile entry(this);
    if (!entry.entered) {
        return Error(FileCode::Busy);
    }
    LockGuard lock(mutex_);
    if (!handle_) {
        return {};
    }
    FileHandle* prior = handle_;
    handle_ = nullptr;
    const auto result = prior->Close();
    prior->Destroy();
    return result;
}

bool File::IsOpen() noexcept {
    ActiveFile entry(this);
    if (!entry.entered) {
        Fatal("File queried during reentrant callback");
    }
    LockGuard lock(mutex_);
    return handle_ != nullptr;
}

} // namespace rocks::platform
