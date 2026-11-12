#include "FileInternal.h"

#include <Platform/File.h>
#include <Platform/Memory.h>
#include <Platform/Path.h>

// FileSystem 확장 연산의 기본 구현(Unsupported)과 FileSystem 위의 재귀 helper다. OS 호출은 하지
// 않는다.
namespace rocks::platform {

FileStatus FileSystem::Stat(const char*, usize, FileInfo&) noexcept {
    return {FileCode::Unsupported, 0};
}

FileStatus FileSystem::MakeDirectory(const char*, usize) noexcept {
    return {FileCode::Unsupported, 0};
}

FileStatus FileSystem::DeleteDirectory(const char*, usize) noexcept {
    return {FileCode::Unsupported, 0};
}

FileStatus FileSystem::EnumerateDirectory(const char*, usize, DirectoryVisitor, void*) noexcept {
    return {FileCode::Unsupported, 0};
}

FileStatus FileSystem::Move(const char*, usize, const char*, usize, bool) noexcept {
    return {FileCode::Unsupported, 0};
}

FileStatus FileHandle::Flush() noexcept {
    return {FileCode::Unsupported, 0};
}

FileStatus FileSystem::Replace(const char*, usize, const char*, usize) noexcept {
    return {FileCode::Unsupported, 0};
}

FileStatus FileSystem::SetModifiedTime(const char*, usize, int64) noexcept {
    return {FileCode::Unsupported, 0};
}

FileStatus FileSystem::SyncDirectory(const char*, usize) noexcept {
    return {FileCode::Unsupported, 0};
}

FileStatus FileSystem::Copy(const char*, usize, const char*, usize, bool) noexcept {
    return {FileCode::Unsupported, 0};
}

namespace {

bool IsSeparator(char c) noexcept {
    return c == '/' || (HostPathStyle() == PathStyle::Windows && c == '\\');
}

bool IsAsciiLetter(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// 만들 수 없는 앞부분(루트·드라이브·UNC 서버와 공유 이름)의 길이다.
usize RootLength(const char* path, usize bytes) noexcept {
    if (HostPathStyle() == PathStyle::Windows) {
        if (bytes >= 2 && IsSeparator(path[0]) && IsSeparator(path[1])) {
            // \\server\share\ 까지 건너뛴다.
            usize index = 2;
            for (int part = 0; part < 2; ++part) {
                while (index < bytes && !IsSeparator(path[index])) {
                    ++index;
                }
                if (index < bytes) {
                    ++index;
                }
            }
            return index;
        }
        if (bytes >= 2 && IsAsciiLetter(path[0]) && path[1] == ':') {
            return bytes >= 3 && IsSeparator(path[2]) ? 3 : 2;
        }
    }
    return bytes >= 1 && IsSeparator(path[0]) ? 1 : 0;
}

// 이미 있는 항목이 디렉터리인지 확인한다.
FileStatus RequireDirectory(FileSystem& system, const char* path, usize bytes) noexcept {
    FileInfo info;
    auto status = system.Stat(path, bytes, info);
    if (!status) {
        return status;
    }
    return info.type == FileType::Directory ? FileStatus{} : FileStatus{FileCode::WrongType, 0};
}

// 디렉터리 하나의 직접 자식을 [kind][이름][NUL] 레코드로 모은다. 지우는 동안 같은 디렉터리를
// 열거하지 않기 위해서다.
struct ChildList {
    char* data = nullptr;
    usize size = 0;
    usize capacity = 0;
    bool failed = false;

    ~ChildList() noexcept {
        AlignedFree(data);
    }

    bool Append(char kind, const char* name, usize nameBytes) noexcept {
        const usize needed = 1 + nameBytes + 1;
        if (needed > MaxSize - size) {
            return false;
        }
        if (size + needed > capacity) {
            usize grown = capacity < 256 ? 256 : capacity;
            while (grown < size + needed) {
                if (grown > MaxSize / 2) {
                    grown = size + needed;
                    break;
                }
                grown *= 2;
            }
            auto* replacement = static_cast<char*>(AlignedAllocate(grown, 1));
            if (replacement == nullptr) {
                return false;
            }
            for (usize index = 0; index < size; ++index) {
                replacement[index] = data[index];
            }
            AlignedFree(data);
            data = replacement;
            capacity = grown;
        }
        data[size++] = kind;
        for (usize index = 0; index < nameBytes; ++index) {
            data[size++] = name[index];
        }
        data[size++] = '\0';
        return true;
    }
};

constexpr char ChildFile = 'f';
constexpr char ChildDirectory = 'd';
constexpr char ChildLink = 'l';

bool CollectChild(const DirectoryEntry& entry, void* raw) noexcept {
    auto& list = *static_cast<ChildList*>(raw);
    const char kind = entry.info.isLink                        ? ChildLink
                      : entry.info.type == FileType::Directory ? ChildDirectory
                                                               : ChildFile;
    if (!list.Append(kind, entry.name, entry.nameBytes)) {
        list.failed = true;
        return false;
    }
    return true;
}

FileStatus DeleteTreeContents(FileSystem& system, const char* path, usize bytes) noexcept;

// path + 구분자 + name을 새 버퍼에 만든다. 실패하면 nullptr이다.
char* JoinChild(const char* path, usize bytes, const char* name, usize nameBytes,
                usize& joinedBytes) noexcept {
    // Windows 드라이브 상대 경로("C:")는 그 드라이브의 현재 디렉터리를 뜻하므로 구분자를 넣으면
    // 드라이브 루트("C:\\name")가 된다. 구분자 없이 붙인다("C:name").
    const bool driveRelative = HostPathStyle() == PathStyle::Windows && bytes == 2 &&
                               IsAsciiLetter(path[0]) && path[1] == ':';
    const bool needSeparator = bytes != 0 && !IsSeparator(path[bytes - 1]) && !driveRelative;
    if (nameBytes > MaxSize - bytes - 2) {
        return nullptr;
    }
    joinedBytes = bytes + (needSeparator ? 1 : 0) + nameBytes;
    auto* joined = static_cast<char*>(AlignedAllocate(joinedBytes + 1, 1));
    if (joined == nullptr) {
        return nullptr;
    }
    usize cursor = 0;
    for (usize index = 0; index < bytes; ++index) {
        joined[cursor++] = path[index];
    }
    if (needSeparator) {
        joined[cursor++] = NativePathSeparator();
    }
    for (usize index = 0; index < nameBytes; ++index) {
        joined[cursor++] = name[index];
    }
    joined[cursor] = '\0';
    return joined;
}

FileStatus DeleteTreeContents(FileSystem& system, const char* path, usize bytes) noexcept {
    ChildList children;
    auto listed = system.EnumerateDirectory(path, bytes, CollectChild, &children);
    if (!listed) {
        return listed;
    }
    if (children.failed) {
        return {FileCode::OutOfMemory, 0};
    }
    usize cursor = 0;
    while (cursor < children.size) {
        const char kind = children.data[cursor++];
        const char* name = children.data + cursor;
        const usize nameBytes = CStringLength(name);
        cursor += nameBytes + 1;
        usize childBytes = 0;
        char* child = JoinChild(path, bytes, name, nameBytes, childBytes);
        if (child == nullptr) {
            return {FileCode::OutOfMemory, 0};
        }
        FileStatus status{};
        if (kind == ChildDirectory) {
            status = DeleteTreeContents(system, child, childBytes);
            if (status) {
                status = system.DeleteDirectory(child, childBytes);
            }
        } else {
            // 파일과 link(디렉터리 link 포함)는 대상이 아니라 그 항목 자체를 지운다.
            status = system.Delete(child, childBytes);
        }
        AlignedFree(child);
        if (!status) {
            return status;
        }
    }
    return {};
}

} // namespace

FileStatus MakeDirectoryTree(FileSystem& system, const char* path, usize pathBytes) noexcept {
    if (path == nullptr || !detail::ValidFilePath(path, pathBytes)) {
        return {FileCode::InvalidArgument, 0};
    }
    usize end = pathBytes;
    const usize root = RootLength(path, pathBytes);
    while (end > root && IsSeparator(path[end - 1])) {
        --end;
    }
    if (end <= root) {
        // 루트·드라이브 자체는 만들 수 없고 이미 있어야 한다.
        return RequireDirectory(system, path, pathBytes);
    }
    for (usize index = root; index <= end; ++index) {
        if (index != end && !IsSeparator(path[index])) {
            continue;
        }
        if (index == root || IsSeparator(path[index - 1])) {
            continue; // 연속 구분자
        }
        auto made = system.MakeDirectory(path, index);
        if (!made) {
            if (made.code != FileCode::AlreadyExists) {
                return made;
            }
            auto existing = RequireDirectory(system, path, index);
            if (!existing) {
                return existing;
            }
        }
    }
    return {};
}

FileStatus DeleteDirectoryTree(FileSystem& system, const char* path, usize pathBytes) noexcept {
    if (path == nullptr || !detail::ValidFilePath(path, pathBytes)) {
        return {FileCode::InvalidArgument, 0};
    }
    // 끝 구분자가 있으면 OS가 link를 따라가 해석하므로("link/") 루트가 아닌 한 떼어낸다.
    const usize root = RootLength(path, pathBytes);
    while (pathBytes > root && pathBytes > 1 && IsSeparator(path[pathBytes - 1])) {
        --pathBytes;
    }
    // 마지막 구성 요소가 "."·".."이면 거부한다. POSIX는 "link/."·"link/.."를 link 대상(또는 그
    // 부모) 기준으로 해석하고 Windows는 문자열로 정규화하므로, 지울 대상이 플랫폼마다 달라진다.
    usize nameStart = pathBytes;
    while (nameStart > root && !IsSeparator(path[nameStart - 1])) {
        --nameStart;
    }
    const usize nameBytes = pathBytes - nameStart;
    if ((nameBytes == 1 && path[nameStart] == '.') ||
        (nameBytes == 2 && path[nameStart] == '.' && path[nameStart + 1] == '.')) {
        return {FileCode::InvalidArgument, 0};
    }
    FileInfo info;
    auto status = system.Stat(path, pathBytes, info);
    if (status.code == FileCode::NotFound) {
        // 대상이 없는 link는 Stat이 NotFound다. link 자체가 있으면 지운다(없으면 그대로 NotFound).
        auto removed = system.Delete(path, pathBytes);
        return removed.code == FileCode::NotFound ? status : removed;
    }
    if (!status) {
        return status;
    }
    if (info.isLink) {
        // 경로 자체가 link면 대상을 비우지 않고 link만 지운다. Delete는 파일·디렉터리 link를 모두
        // 지운다(대상 종류와 link 종류가 어긋난 Windows link 포함).
        return system.Delete(path, pathBytes);
    }
    if (info.type != FileType::Directory) {
        return {FileCode::WrongType, 0};
    }
    status = DeleteTreeContents(system, path, pathBytes);
    if (!status) {
        return status;
    }
    return system.DeleteDirectory(path, pathBytes);
}

} // namespace rocks::platform
