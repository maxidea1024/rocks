#include "../Common/DirectoryWatchInternal.h"
#include "../Posix/PosixFileError.h"

#include <Platform/Memory.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <new>

// Linux(Android) inotify backend다. inotify는 하위 디렉터리를 자동으로 감시하지 않으므로
// 디렉터리마다 watch를 두고 wd → 상대 경로 표를 유지한다. 새 디렉터리는 IN_CREATE를 받는 즉시
// watch를 추가하며, watch를 걸기 전에 그 안에 생긴 항목은 디렉터리를 열거해 Added로 보낸다(경합
// 보정). 이름 변경은 MOVED_FROM/MOVED_TO의 cookie로 짝을 짓고 짝이 없으면 Removed·Added로 보낸다.
namespace rocks::platform::detail {
namespace {

constexpr uint32 WatchMask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB |
                             IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF |
                             IN_ONLYDIR | IN_DONT_FOLLOW | IN_EXCL_UNLINK;
constexpr usize EventBufferBytes = 64u * 1024u;
constexpr usize PathCapacity = 4096u;

struct WatchEntry {
    int wd = -1;
    char* path = nullptr; // 루트 기준 상대 경로, 루트는 ""
    usize pathBytes = 0;
};

char* CopyText(const char* text, usize bytes) noexcept {
    auto* copy = static_cast<char*>(AlignedAllocate(bytes + 1u, 1));
    if (copy != nullptr) {
        if (bytes != 0) {
            platform::MemoryCopy(copy, text, bytes);
        }
        copy[bytes] = '\0';
    }
    return copy;
}

} // namespace

struct WatchBackend {
    int fd = -1;
    char* root = nullptr;
    usize rootBytes = 0;
    // wd 오름차순(커널이 늘어나는 wd를 발급하므로 추가는 끝에 붙는다).
    WatchEntry* entries = nullptr;
    usize count = 0;
    usize capacity = 0;
    bool rootRemoved = false;
    // IN_Q_OVERFLOW 뒤 watch 표를 다시 만들어야 한다(놓친 생성·이동 때문에 표가 낡았다).
    bool resync = false;
    // Open 중 감시하지 못한 하위 트리의 Overflow다. 첫 Poll이 넘긴다.
    WatchEventQueue startup;
    // 이번 Collect에서 짝을 기다리는 MOVED_FROM이다.
    bool movePending = false;
    uint32 moveCookie = 0;
    bool moveIsDirectory = false;
    char movePath[PathCapacity];
    usize movePathBytes = 0;
    char scratch[PathCapacity];
    char absolute[PathCapacity * 2];
    alignas(inotify_event) char buffer[EventBufferBytes];
};

namespace {

WatchEntry* FindEntry(WatchBackend& backend, int wd) noexcept {
    usize low = 0;
    usize high = backend.count;
    while (low < high) {
        const usize middle = low + (high - low) / 2u;
        if (backend.entries[middle].wd < wd) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    return low < backend.count && backend.entries[low].wd == wd ? &backend.entries[low] : nullptr;
}

bool InsertEntry(WatchBackend& backend, int wd, const char* path, usize pathBytes) noexcept {
    if (WatchEntry* existing = FindEntry(backend, wd)) {
        // 같은 디렉터리를 다시 watch하면 커널은 같은 wd를 돌려준다. 경로만 갱신한다.
        char* copy = CopyText(path, pathBytes);
        if (copy == nullptr) {
            return false;
        }
        AlignedFree(existing->path);
        existing->path = copy;
        existing->pathBytes = pathBytes;
        return true;
    }
    if (backend.count == backend.capacity) {
        const usize next = backend.capacity == 0 ? 64u : backend.capacity * 2u;
        auto* grown = static_cast<WatchEntry*>(
            AlignedAllocate(next * sizeof(WatchEntry), alignof(WatchEntry)));
        if (grown == nullptr) {
            return false;
        }
        if (backend.count != 0) {
            platform::MemoryCopy(grown, backend.entries, backend.count * sizeof(WatchEntry));
        }
        AlignedFree(backend.entries);
        backend.entries = grown;
        backend.capacity = next;
    }
    char* copy = CopyText(path, pathBytes);
    if (copy == nullptr) {
        return false;
    }
    usize position = backend.count;
    while (position > 0 && backend.entries[position - 1u].wd > wd) {
        backend.entries[position] = backend.entries[position - 1u];
        --position;
    }
    backend.entries[position] = {wd, copy, pathBytes};
    ++backend.count;
    return true;
}

void EraseEntry(WatchBackend& backend, WatchEntry* entry) noexcept {
    AlignedFree(entry->path);
    const usize index = static_cast<usize>(entry - backend.entries);
    for (usize position = index; position + 1u < backend.count; ++position) {
        backend.entries[position] = backend.entries[position + 1u];
    }
    --backend.count;
}

bool HasPrefix(const char* path, usize pathBytes, const char* prefix, usize prefixBytes) noexcept {
    return pathBytes >= prefixBytes && platform::MemoryCompare(path, prefix, prefixBytes) == 0 &&
           (pathBytes == prefixBytes || path[prefixBytes] == '/');
}

// 상대 경로의 절대 경로를 backend.absolute에 만든다.
bool MakeAbsolute(WatchBackend& backend, const char* relative, usize relativeBytes) noexcept {
    usize bytes = 0;
    return JoinRelativePath(backend.root, backend.rootBytes, relative, relativeBytes,
                            backend.absolute, sizeof(backend.absolute), bytes);
}

// relative 디렉터리와 그 하위 디렉터리에 watch를 건다. announce면 안에서 찾은 항목을 Added로 보낸다
// (watch를 걸기 전에 생긴 항목을 놓치지 않기 위해). 깊이는 재귀 호출 깊이로 제한한다.
FileStatus AddTree(WatchBackend& backend, const char* relative, usize relativeBytes,
                   WatchEventQueue* announce, uint32 depth) noexcept {
    // 감시하지 못한 하위 트리는 Overflow로 알린다(Open 중이면 첫 Poll에 넘길 startup 큐).
    WatchEventQueue& notices = announce != nullptr ? *announce : backend.startup;
    const bool root = relativeBytes == 0;
    if (depth > 256u) {
        notices.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Directory, relative,
                     relativeBytes);
        return {};
    }
    if (!MakeAbsolute(backend, relative, relativeBytes)) {
        return {FileCode::Overflow, 0};
    }
    const int wd = inotify_add_watch(backend.fd, backend.absolute, WatchMask);
    if (wd < 0) {
        const int error = errno;
        // 그 사이 지워진 디렉터리는 무시한다. watch 한도(ENOSPC)는 놓친 변경이 생기므로 Overflow다.
        if (!root && (error == ENOENT || error == ENOTDIR)) {
            return {};
        }
        if (!root) {
            // 이 하위 트리는 감시할 수 없다(watch 한도·권한). 그 아래를 다시 검색하게 한다.
            notices.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Directory, relative,
                         relativeBytes);
            return {};
        }
        return PosixFileError(error);
    }
    if (!InsertEntry(backend, wd, relative, relativeBytes)) {
        inotify_rm_watch(backend.fd, wd);
        return {FileCode::OutOfMemory, 0};
    }
    DIR* directory = opendir(backend.absolute);
    if (directory == nullptr) {
        if (errno != ENOENT) {
            notices.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Directory, relative,
                         relativeBytes);
        }
        return {};
    }
    // 자식 이름을 모은 뒤 닫고 재귀한다(열린 디렉터리 수를 깊이와 무관하게 유지).
    char* names = nullptr;
    usize namesUsed = 0;
    usize namesCapacity = 0;
    FileStatus status{};
    while (dirent* item = readdir(directory)) {
        const char* name = item->d_name;
        if ((name[0] == '.' && name[1] == '\0') ||
            (name[0] == '.' && name[1] == '.' && name[2] == '\0')) {
            continue;
        }
        const usize nameBytes = platform::CStringLength(name);
        usize childBytes = 0;
        if (!JoinRelativePath(relative, relativeBytes, name, nameBytes, backend.scratch,
                              sizeof(backend.scratch), childBytes)) {
            // 경로가 너무 길어 알릴 수 없는 자식이다. 이 디렉터리를 다시 검색하게 한다.
            notices.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Directory, relative,
                         relativeBytes);
            continue;
        }
        unsigned char type = item->d_type;
        if (type == DT_UNKNOWN) {
            struct stat info;
            if (MakeAbsolute(backend, backend.scratch, childBytes) &&
                lstat(backend.absolute, &info) == 0) {
                type = S_ISDIR(info.st_mode) ? DT_DIR : (S_ISLNK(info.st_mode) ? DT_LNK : DT_REG);
            }
        }
        if (announce != nullptr) {
            announce->Push(DirectoryChangeKind::Added,
                           type == DT_DIR ? DirectoryEntryKind::Directory
                                          : DirectoryEntryKind::File,
                           backend.scratch, childBytes);
        }
        if (type != DT_DIR) {
            continue;
        }
        if (namesUsed + nameBytes + 1u > namesCapacity) {
            const usize next = namesCapacity == 0 ? 1024u : namesCapacity * 2u + nameBytes;
            auto* grown = static_cast<char*>(AlignedAllocate(next, 1));
            if (grown == nullptr) {
                status = {FileCode::OutOfMemory, 0};
                break;
            }
            if (namesUsed != 0) {
                platform::MemoryCopy(grown, names, namesUsed);
            }
            AlignedFree(names);
            names = grown;
            namesCapacity = next;
        }
        platform::MemoryCopy(names + namesUsed, name, nameBytes + 1u);
        namesUsed += nameBytes + 1u;
    }
    closedir(directory);
    for (usize offset = 0; status && offset < namesUsed;) {
        const char* name = names + offset;
        const usize nameBytes = platform::CStringLength(name);
        offset += nameBytes + 1u;
        // 재귀 깊이만큼 스택을 쓰지 않도록 자식 경로는 힙에 둔다.
        auto* child = static_cast<char*>(AlignedAllocate(PathCapacity, 1));
        if (child == nullptr) {
            status = {FileCode::OutOfMemory, 0};
            break;
        }
        usize childBytes = 0;
        if (JoinRelativePath(relative, relativeBytes, name, nameBytes, child, PathCapacity,
                             childBytes)) {
            status = AddTree(backend, child, childBytes, announce, depth + 1u);
        }
        AlignedFree(child);
    }
    AlignedFree(names);
    return status;
}

// prefix 경로(와 그 아래)의 watch를 없앤다.
void RemoveTree(WatchBackend& backend, const char* prefix, usize prefixBytes) noexcept {
    for (usize index = backend.count; index > 0; --index) {
        WatchEntry& entry = backend.entries[index - 1u];
        if (HasPrefix(entry.path, entry.pathBytes, prefix, prefixBytes)) {
            inotify_rm_watch(backend.fd, entry.wd);
            EraseEntry(backend, &entry);
        }
    }
}

// from 경로(와 그 아래)의 watch 경로를 to로 바꾸고 바꾼 항목 수를 돌려준다. 할당에 실패하면
// false다.
bool RenameTree(WatchBackend& backend, const char* from, usize fromBytes, const char* to,
                usize toBytes, usize& renamed) noexcept {
    renamed = 0;
    for (usize index = 0; index < backend.count; ++index) {
        WatchEntry& entry = backend.entries[index];
        if (!HasPrefix(entry.path, entry.pathBytes, from, fromBytes)) {
            continue;
        }
        const usize tail = entry.pathBytes - fromBytes;
        const usize bytes = toBytes + tail;
        auto* path = static_cast<char*>(AlignedAllocate(bytes + 1u, 1));
        if (path == nullptr) {
            return false;
        }
        MemoryCopy(path, to, toBytes);
        MemoryCopy(path + toBytes, entry.path + fromBytes, tail);
        path[bytes] = '\0';
        AlignedFree(entry.path);
        entry.path = path;
        entry.pathBytes = bytes;
        ++renamed;
    }
    return true;
}

// 모든 watch를 없앤다(resync 전).
void RemoveAll(WatchBackend& backend) noexcept {
    for (usize index = 0; index < backend.count; ++index) {
        inotify_rm_watch(backend.fd, backend.entries[index].wd);
        AlignedFree(backend.entries[index].path);
    }
    backend.count = 0;
}

void FlushMove(WatchBackend& backend, WatchEventQueue& out) noexcept {
    if (!backend.movePending) {
        return;
    }
    backend.movePending = false;
    out.Push(DirectoryChangeKind::Removed,
             backend.moveIsDirectory ? DirectoryEntryKind::Directory : DirectoryEntryKind::File,
             backend.movePath, backend.movePathBytes);
    if (backend.moveIsDirectory) {
        RemoveTree(backend, backend.movePath, backend.movePathBytes);
    }
}

void FreeBackend(WatchBackend* backend) noexcept {
    for (usize index = 0; index < backend->count; ++index) {
        AlignedFree(backend->entries[index].path);
    }
    AlignedFree(backend->entries);
    AlignedFree(backend->root);
    if (backend->fd >= 0) {
        close(backend->fd);
    }
    backend->~WatchBackend();
    AlignedFree(backend);
}

void HandleEvent(WatchBackend& backend, const inotify_event& event, WatchEventQueue& out) noexcept {
    if ((event.mask & IN_Q_OVERFLOW) != 0) {
        out.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Unknown, "", 0);
        backend.resync = true;
        return;
    }
    WatchEntry* entry = FindEntry(backend, event.wd);
    if (entry == nullptr) {
        return; // 이미 지운 watch의 늦은 이벤트다.
    }
    if ((event.mask & (IN_IGNORED | IN_UNMOUNT)) != 0) {
        // 루트 watch가 없어지면(언마운트·삭제) 더 이상 이벤트가 오지 않는다.
        if (entry->pathBytes == 0) {
            out.Push(DirectoryChangeKind::RootRemoved, DirectoryEntryKind::Directory, "", 0);
            backend.rootRemoved = true;
        }
        if ((event.mask & IN_IGNORED) != 0) {
            EraseEntry(backend, entry);
        }
        return;
    }
    if ((event.mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
        if (entry->pathBytes == 0) {
            out.Push(DirectoryChangeKind::RootRemoved, DirectoryEntryKind::Directory, "", 0);
            backend.rootRemoved = true;
        }
        // 하위 디렉터리 자신의 삭제·이동은 부모 디렉터리의 이벤트로 알린다.
        return;
    }
    const usize nameBytes = event.len != 0 ? platform::CStringLength(event.name) : 0;
    usize pathBytes = 0;
    if (!JoinRelativePath(entry->path, entry->pathBytes, event.name, nameBytes, backend.scratch,
                          sizeof(backend.scratch), pathBytes)) {
        out.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Unknown, "", 0);
        return;
    }
    const bool isDirectory = (event.mask & IN_ISDIR) != 0;
    const DirectoryEntryKind kind =
        isDirectory ? DirectoryEntryKind::Directory : DirectoryEntryKind::File;
    if ((event.mask & IN_MOVED_TO) != 0) {
        if (backend.movePending && backend.moveCookie == event.cookie) {
            backend.movePending = false;
            out.Push(DirectoryChangeKind::Renamed, kind, backend.scratch, pathBytes,
                     backend.movePath, backend.movePathBytes);
            if (isDirectory) {
                usize renamed = 0;
                char path[PathCapacity];
                MemoryCopy(path, backend.scratch, pathBytes + 1u);
                if (!RenameTree(backend, backend.movePath, backend.movePathBytes, path, pathBytes,
                                renamed)) {
                    out.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Unknown, "", 0);
                } else if (renamed == 0) {
                    // 만든 직후 이름을 바꾼 디렉터리는 아직 watch가 없다(IN_CREATE 처리 때 이미
                    // 옮겨져 AddTree가 ENOENT). 새 경로에 watch를 걸고 안의 항목을 알린다.
                    (void)AddTree(backend, path, pathBytes, &out, 0);
                }
            }
            return;
        }
        FlushMove(backend, out);
        out.Push(DirectoryChangeKind::Added, kind, backend.scratch, pathBytes);
        if (isDirectory) {
            char path[PathCapacity];
            platform::MemoryCopy(path, backend.scratch, pathBytes + 1u);
            (void)AddTree(backend, path, pathBytes, &out, 0);
        }
        return;
    }
    FlushMove(backend, out);
    if ((event.mask & IN_MOVED_FROM) != 0) {
        backend.movePending = true;
        backend.moveCookie = event.cookie;
        backend.moveIsDirectory = isDirectory;
        platform::MemoryCopy(backend.movePath, backend.scratch, pathBytes + 1u);
        backend.movePathBytes = pathBytes;
        return;
    }
    if ((event.mask & IN_CREATE) != 0) {
        out.Push(DirectoryChangeKind::Added, kind, backend.scratch, pathBytes);
        if (isDirectory) {
            char path[PathCapacity];
            platform::MemoryCopy(path, backend.scratch, pathBytes + 1u);
            (void)AddTree(backend, path, pathBytes, &out, 0);
        }
        return;
    }
    if ((event.mask & IN_DELETE) != 0) {
        out.Push(DirectoryChangeKind::Removed, kind, backend.scratch, pathBytes);
        return;
    }
    if ((event.mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB)) != 0 && !isDirectory) {
        out.Push(DirectoryChangeKind::Modified, kind, backend.scratch, pathBytes);
    }
}

} // namespace

FileStatus WatchOpen(const char* path, usize pathBytes, WatchBackend*& backend) noexcept {
    if (pathBytes >= PathCapacity) {
        return {FileCode::Overflow, 0};
    }
    void* memory = AlignedAllocate(sizeof(WatchBackend), alignof(WatchBackend));
    if (memory == nullptr) {
        return {FileCode::OutOfMemory, 0};
    }
    auto* created = ::new (memory) WatchBackend();
    // 루트가 link면 OS가 해석한 대상을 감시한다(IN_DONT_FOLLOW는 link 자체를 거부하므로 먼저 푼다).
    char given[PathCapacity];
    MemoryCopy(given, path, pathBytes);
    given[pathBytes] = '\0';
    char resolved[PATH_MAX];
    if (realpath(given, resolved) == nullptr) {
        const FileStatus failed = PosixFileError(errno);
        FreeBackend(created);
        return failed;
    }
    const usize rootBytes = platform::CStringLength(resolved);
    created->root = CopyText(resolved, rootBytes);
    if (created->root == nullptr) {
        FreeBackend(created);
        return {FileCode::OutOfMemory, 0};
    }
    created->rootBytes = rootBytes;
    struct stat info;
    if (stat(created->root, &info) != 0) {
        const FileStatus failed = PosixFileError(errno);
        FreeBackend(created);
        return failed;
    }
    if (!S_ISDIR(info.st_mode)) {
        FreeBackend(created);
        return {FileCode::WrongType, 0};
    }
    created->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (created->fd < 0) {
        const FileStatus failed = PosixFileError(errno);
        FreeBackend(created);
        return failed;
    }
    if (const FileStatus added = AddTree(*created, "", 0, nullptr, 0); !added) {
        FreeBackend(created);
        return added;
    }
    if (created->count == 0) {
        FreeBackend(created);
        return {FileCode::NotFound, 0};
    }
    backend = created;
    return {};
}

FileStatus WatchCollect(WatchBackend& backend, WatchEventQueue& out) noexcept {
    out.TakeAll(backend.startup);
    // 쓰기가 계속 쏟아져도 Poll이 돌아오도록 한 번에 읽는 버퍼 수를 제한한다.
    for (int round = 0; round < 64 && !backend.rootRemoved; ++round) {
        const ssize_t bytes = read(backend.fd, backend.buffer, sizeof(backend.buffer));
        if (bytes < 0) {
            const int error = errno;
            if (error == EAGAIN || error == EWOULDBLOCK) {
                break;
            }
            if (error == EINTR) {
                continue;
            }
            FlushMove(backend, out);
            out.Push(DirectoryChangeKind::Overflow, DirectoryEntryKind::Unknown, "", 0);
            return PosixFileError(error);
        }
        if (bytes == 0) {
            break;
        }
        for (usize offset = 0; offset + sizeof(inotify_event) <= static_cast<usize>(bytes);) {
            const auto* event = reinterpret_cast<const inotify_event*>(backend.buffer + offset);
            const usize total = sizeof(inotify_event) + event->len;
            if (offset + total > static_cast<usize>(bytes)) {
                break;
            }
            HandleEvent(backend, *event, out);
            offset += total;
            if (backend.rootRemoved) {
                break;
            }
        }
    }
    // 짝을 찾지 못한 MOVED_FROM은 감시 루트 밖으로 나간 것으로 본다(계약상 Removed).
    if (!backend.rootRemoved) {
        FlushMove(backend, out);
    }
    if (backend.resync && !backend.rootRemoved) {
        backend.resync = false;
        RemoveAll(backend);
        if (!AddTree(backend, "", 0, &out, 0) || backend.count == 0) {
            out.Push(DirectoryChangeKind::RootRemoved, DirectoryEntryKind::Directory, "", 0);
            backend.rootRemoved = true;
        }
    }
    return {};
}

bool WatchWait(WatchBackend& backend, uint32 timeoutMilliseconds) noexcept {
    if (backend.rootRemoved) {
        return false;
    }
    pollfd descriptor{backend.fd, POLLIN, 0};
    const int timeout =
        timeoutMilliseconds > 0x7FFFFFFFu ? 0x7FFFFFFF : static_cast<int>(timeoutMilliseconds);
    for (;;) {
        const int ready = poll(&descriptor, 1, timeout);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        return ready > 0;
    }
}

void WatchClose(WatchBackend* backend) noexcept {
    if (backend != nullptr) {
        FreeBackend(backend);
    }
}

} // namespace rocks::platform::detail
