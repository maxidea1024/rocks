#include "../Common/DirectoryWatchInternal.h"
#include "../Posix/PosixFileError.h"

#include <Platform/Clock.h>
#include <Platform/Memory.h>
#include <Platform/Sync.h>

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <new>

// macOS FSEvents backend다. 파일 단위 이벤트(kFSEventStreamCreateFlagFileEvents)를 전용 직렬
// dispatch queue로 받아 mutex로 보호한 큐에 쌓고, Poll·Wait가 그 큐를 비운다. FSEvents는 같은
// 경로의 여러 변경을 flag 묶음 하나로 합치고 이름 변경의 두 경로를 짝지어 주지 않으므로, 이벤트
// 경로의 현재 상태(lstat)를 보고 Added·Removed·Modified로 정한다(Renamed는 만들지 않는다). 경로는
// 실제 경로(realpath)로 보고되므로 루트도 realpath로 맞춘 뒤 상대 경로를 만든다.
//
// 이 파일은 현재 저장소에서 macOS로 빌드·실행 검증하지 않았다(PlatformLayer.ko.md 참고).
namespace rocks::platform::detail {
namespace {

constexpr CFTimeInterval LatencySeconds = 0.05;

} // namespace

struct WatchBackend {
    FSEventStreamRef stream = nullptr;
    dispatch_queue_t queue = nullptr;
    bool scheduled = false;
    bool started = false;
    char root[PATH_MAX];
    usize rootBytes = 0;
    Mutex mutex;
    ConditionVariable changed;
    WatchEventQueue events;
    bool rootRemoved = false;
};

namespace {

void PushLocked(WatchBackend& backend, DirectoryChangeKind kind, DirectoryEntryKind entry,
                const char* path, usize pathBytes) noexcept {
    backend.events.Push(kind, entry, path, pathBytes);
}

void Callback(ConstFSEventStreamRef, void* info, size_t count, void* eventPaths,
              const FSEventStreamEventFlags flags[], const FSEventStreamEventId[]) {
    auto& backend = *static_cast<WatchBackend*>(info);
    auto** paths = static_cast<char**>(eventPaths);
    LockGuard lock(backend.mutex);
    for (size_t index = 0; index < count && !backend.rootRemoved; ++index) {
        const FSEventStreamEventFlags flag = flags[index];
        if ((flag & (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped |
                     kFSEventStreamEventFlagKernelDropped)) != 0) {
            PushLocked(backend, DirectoryChangeKind::Overflow, DirectoryEntryKind::Unknown, "", 0);
            continue;
        }
        if ((flag & kFSEventStreamEventFlagRootChanged) != 0) {
            PushLocked(backend, DirectoryChangeKind::RootRemoved, DirectoryEntryKind::Directory, "",
                       0);
            backend.rootRemoved = true;
            continue;
        }
        const char* path = paths[index];
        const usize pathBytes = platform::CStringLength(path);
        // 루트 아래(루트 + '/')의 경로만 상대 경로로 바꾼다. 루트 자신의 이벤트는 알리지 않는다.
        // 루트가 "/"이면 구분자를 따로 건너뛰지 않는다.
        const usize skip = backend.rootBytes > 1u ? backend.rootBytes + 1u : backend.rootBytes;
        if (pathBytes <= skip || !platform::MemoryEquals(path, backend.root, backend.rootBytes) ||
            (backend.rootBytes > 1u && path[backend.rootBytes] != '/')) {
            continue;
        }
        const char* relative = path + skip;
        const usize relativeBytes = pathBytes - skip;
        DirectoryEntryKind entry = DirectoryEntryKind::Unknown;
        if ((flag & kFSEventStreamEventFlagItemIsDir) != 0) {
            entry = DirectoryEntryKind::Directory;
        } else if ((flag & kFSEventStreamEventFlagItemIsFile) != 0) {
            entry = DirectoryEntryKind::File;
        }
        struct stat info;
        const bool exists = lstat(path, &info) == 0;
        DirectoryChangeKind kind = DirectoryChangeKind::Modified;
        if (!exists) {
            kind = DirectoryChangeKind::Removed;
        } else if ((flag & (kFSEventStreamEventFlagItemCreated |
                            kFSEventStreamEventFlagItemRenamed)) != 0) {
            kind = DirectoryChangeKind::Added;
        } else if (entry == DirectoryEntryKind::Directory) {
            continue; // 디렉터리 메타데이터 변경은 알리지 않는다.
        }
        PushLocked(backend, kind, entry, relative, relativeBytes);
    }
    backend.changed.NotifyAll();
}

void FreeBackend(WatchBackend* backend) noexcept {
    if (backend->stream != nullptr) {
        if (backend->started) {
            FSEventStreamStop(backend->stream);
        }
        if (backend->scheduled) {
            FSEventStreamInvalidate(backend->stream);
        }
    }
    if (backend->queue != nullptr) {
        // Invalidate 뒤에는 새 callback이 없다. 이미 queue에 들어간 callback이 끝나도록 비운다.
        dispatch_sync_f(backend->queue, nullptr, [](void*) {});
    }
    if (backend->stream != nullptr) {
        FSEventStreamRelease(backend->stream);
    }
    if (backend->queue != nullptr) {
        dispatch_release(backend->queue);
    }
    backend->~WatchBackend();
    AlignedFree(backend);
}

} // namespace

FileStatus WatchOpen(const char* path, usize pathBytes, WatchBackend*& backend) noexcept {
    if (pathBytes >= PATH_MAX) {
        return {FileCode::Overflow, 0};
    }
    char given[PATH_MAX];
    platform::MemoryCopy(given, path, pathBytes);
    given[pathBytes] = '\0';
    struct stat info;
    if (stat(given, &info) != 0) {
        return PosixFileError(errno);
    }
    if (!S_ISDIR(info.st_mode)) {
        return {FileCode::WrongType, 0};
    }
    void* memory = AlignedAllocate(sizeof(WatchBackend), alignof(WatchBackend));
    if (memory == nullptr) {
        return {FileCode::OutOfMemory, 0};
    }
    auto* created = ::new (memory) WatchBackend();
    if (realpath(given, created->root) == nullptr) {
        const FileStatus failed = PosixFileError(errno);
        FreeBackend(created);
        return failed;
    }
    created->rootBytes = platform::CStringLength(created->root);
    CFStringRef rootString =
        CFStringCreateWithCString(kCFAllocatorDefault, created->root, kCFStringEncodingUTF8);
    if (rootString == nullptr) {
        FreeBackend(created);
        return {FileCode::OutOfMemory, 0};
    }
    const void* values[] = {rootString};
    CFArrayRef paths = CFArrayCreate(kCFAllocatorDefault, values, 1, &kCFTypeArrayCallBacks);
    CFRelease(rootString);
    if (paths == nullptr) {
        FreeBackend(created);
        return {FileCode::OutOfMemory, 0};
    }
    FSEventStreamContext context{};
    context.info = created;
    created->stream =
        FSEventStreamCreate(kCFAllocatorDefault, &Callback, &context, paths,
                            kFSEventStreamEventIdSinceNow, LatencySeconds,
                            kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer |
                                kFSEventStreamCreateFlagWatchRoot);
    CFRelease(paths);
    if (created->stream == nullptr) {
        FreeBackend(created);
        return {FileCode::SystemError, 0};
    }
    created->queue = dispatch_queue_create("rocks.directory-watch", DISPATCH_QUEUE_SERIAL);
    if (created->queue == nullptr) {
        FreeBackend(created);
        return {FileCode::OutOfMemory, 0};
    }
    FSEventStreamSetDispatchQueue(created->stream, created->queue);
    created->scheduled = true;
    created->started = FSEventStreamStart(created->stream);
    if (!created->started) {
        FreeBackend(created);
        return {FileCode::SystemError, 0};
    }
    backend = created;
    return {};
}

FileStatus WatchCollect(WatchBackend& backend, WatchEventQueue& out) noexcept {
    LockGuard lock(backend.mutex);
    out.TakeAll(backend.events);
    return {};
}

bool WatchWait(WatchBackend& backend, uint32 timeoutMilliseconds) noexcept {
    LockGuard lock(backend.mutex);
    const uint64 start = MonotonicNanoseconds();
    const uint64 limit = static_cast<uint64>(timeoutMilliseconds) * 1000000u;
    while (backend.events.IsEmpty() && !backend.rootRemoved) {
        const uint64 elapsed = MonotonicNanoseconds() - start;
        if (elapsed >= limit) {
            return false;
        }
        const uint64 remaining = (limit - elapsed + 999999u) / 1000000u;
        if (backend.changed.Wait(backend.mutex, static_cast<uint32>(remaining)) ==
            WaitResult::Failed) {
            return false;
        }
    }
    return !backend.events.IsEmpty();
}

void WatchClose(WatchBackend* backend) noexcept {
    if (backend != nullptr) {
        FreeBackend(backend);
    }
}

} // namespace rocks::platform::detail
