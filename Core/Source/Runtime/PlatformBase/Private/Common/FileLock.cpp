#include "FileInternal.h"

#include <Platform/Clock.h>
#include <Platform/File.h>
#include <Platform/Thread.h>
#include <Platform/Memory.h>

namespace rocks::platform {

FileLock::~FileLock() noexcept {
    Release();
}

FileStatus FileLock::Acquire(const char* path, uint32 timeoutMilliseconds) noexcept {
    return Acquire(path, path != nullptr ? platform::CStringLength(path) : 0, timeoutMilliseconds);
}

FileStatus FileLock::Acquire(const char* path, usize pathBytes,
                             uint32 timeoutMilliseconds) noexcept {
    if (held_) {
        return {FileCode::AlreadyOpen, 0};
    }
    if (path == nullptr || pathBytes == 0 || !detail::ValidFilePath(path, pathBytes)) {
        return {FileCode::InvalidArgument, 0};
    }
    uptr handle = 0;
    if (const FileStatus opened = detail::LockFileOpen(path, pathBytes, handle); !opened) {
        return opened;
    }
    const uint64 deadline =
        MonotonicNanoseconds() + static_cast<uint64>(timeoutMilliseconds) * 1000000u;
    for (;;) {
        bool acquired = false;
        if (const FileStatus tried = detail::LockFileTry(handle, acquired); !tried) {
            detail::LockFileClose(handle, false);
            return tried;
        }
        if (acquired) {
            bool current = false;
            if (const FileStatus checked =
                    detail::LockFileIsCurrent(path, pathBytes, handle, current);
                !checked) {
                detail::LockFileClose(handle, true);
                return checked;
            }
            if (current) {
                handle_ = handle;
                held_ = true;
                return {};
            }
            // 잠그기 전에 다른 쪽이 잠금 파일을 지우거나 바꿨다. 지금 경로의 파일을 다시 연다.
            detail::LockFileClose(handle, true);
            if (const FileStatus reopened = detail::LockFileOpen(path, pathBytes, handle);
                !reopened) {
                return reopened;
            }
            if (MonotonicNanoseconds() >= deadline) {
                detail::LockFileClose(handle, false);
                return {FileCode::Busy, 0};
            }
            continue;
        }
        if (MonotonicNanoseconds() >= deadline) {
            detail::LockFileClose(handle, false);
            return {FileCode::Busy, 0};
        }
        SleepFor(50);
    }
}

void FileLock::Release() noexcept {
    if (!held_) {
        return;
    }
    detail::LockFileClose(handle_, true);
    handle_ = 0;
    held_ = false;
}

} // namespace rocks::platform
