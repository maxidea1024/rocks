#include "../Apple/ApplePlatformPath.h"

#include <Platform/Memory.h>
#include <Platform/Path.h>

#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <pwd.h>
#include <stdlib.h>
#include <unistd.h>

// 주의: macOS 실기기에서 아직 빌드·실행하지 않았다.
namespace rocks::platform {
namespace {

FileStatus Write(const char* text, usize length, char* buffer, usize capacity,
                 usize& bytes) noexcept {
    while (length > 1 && text[length - 1] == '/') {
        --length;
    }
    bytes = length;
    if (length >= capacity) {
        return {FileCode::Overflow, 0};
    }
    for (usize index = 0; index < length; ++index) {
        buffer[index] = text[index];
    }
    buffer[length] = '\0';
    return {};
}

usize Length(const char* text) noexcept {
    usize length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

// HOME이 비었으면 사용자 DB에서 홈을 찾는다.
const char* HomeDirectory(char* scratch, usize capacity) noexcept {
    const char* home = getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        return home;
    }
    struct passwd entry{};
    struct passwd* found = nullptr;
    if (getpwuid_r(getuid(), &entry, scratch, capacity, &found) == 0 && found != nullptr &&
        found->pw_dir != nullptr) {
        return found->pw_dir;
    }
    return nullptr;
}

FileStatus HomeChild(const char* suffix, char* buffer, usize capacity, usize& bytes) noexcept {
    char scratch[4096];
    const char* home = HomeDirectory(scratch, sizeof(scratch));
    if (home == nullptr || home[0] == '\0') {
        return {FileCode::NotFound, 0};
    }
    usize homeLength = Length(home);
    const usize suffixLength = Length(suffix);
    // 끝 '/'를 뗀다(HOME=/Users/x/). 루트("/")는 suffix가 없을 때만 그대로 둔다.
    while (homeLength > (suffixLength == 0 ? 1u : 0u) && home[homeLength - 1] == '/') {
        --homeLength;
    }
    bytes = homeLength + suffixLength;
    if (bytes >= capacity) {
        return {FileCode::Overflow, 0};
    }
    for (usize index = 0; index < homeLength; ++index) {
        buffer[index] = home[index];
    }
    for (usize index = 0; index < suffixLength; ++index) {
        buffer[homeLength + index] = suffix[index];
    }
    buffer[bytes] = '\0';
    return {};
}

} // namespace

PathStyle HostPathStyle() noexcept {
    return ApplePlatformPath::HostStyle();
}

char NativePathSeparator() noexcept {
    return ApplePlatformPath::NativeSeparator();
}

FileStatus RegisterApplicationDirectories(const char*, const char*) noexcept {
    return {FileCode::Unsupported, 0};
}

FileStatus GetStandardDirectory(StandardDirectory kind, char* buffer, usize capacity,
                                usize& bytes) noexcept {
    if (buffer == nullptr && capacity != 0) {
        return {FileCode::InvalidArgument, 0};
    }
    switch (kind) {
    case StandardDirectory::Executable: {
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        auto* raw = static_cast<char*>(AlignedAllocate(size + 1, 1));
        if (raw == nullptr) {
            return {FileCode::OutOfMemory, 0};
        }
        if (_NSGetExecutablePath(raw, &size) != 0) {
            AlignedFree(raw);
            return {FileCode::SystemError, 0};
        }
        char resolved[PATH_MAX];
        const char* path = realpath(raw, resolved) != nullptr ? resolved : raw;
        usize length = Length(path);
        while (length > 0 && path[length - 1] != '/') {
            --length;
        }
        auto status = Write(path, length, buffer, capacity, bytes);
        AlignedFree(raw);
        return status;
    }
    case StandardDirectory::Working: {
        // getcwd(nullptr, 0)은 필요한 크기로 할당한다(macOS·bionic 지원).
        char* path = getcwd(nullptr, 0);
        if (path == nullptr) {
            return {FileCode::SystemError, static_cast<uint32>(errno)};
        }
        auto status = Write(path, Length(path), buffer, capacity, bytes);
        free(path);
        return status;
    }
    case StandardDirectory::Temporary: {
        char path[PATH_MAX];
        const size_t length = confstr(_CS_DARWIN_USER_TEMP_DIR, path, sizeof(path));
        if (length > 0 && length <= sizeof(path)) {
            return Write(path, Length(path), buffer, capacity, bytes);
        }
        const char* fallback = getenv("TMPDIR");
        if (fallback == nullptr || fallback[0] == '\0') {
            fallback = "/tmp";
        }
        return Write(fallback, Length(fallback), buffer, capacity, bytes);
    }
    case StandardDirectory::Home:
        return HomeChild("", buffer, capacity, bytes);
    case StandardDirectory::UserData:
        return HomeChild("/Library/Application Support", buffer, capacity, bytes);
    case StandardDirectory::UserCache:
        return HomeChild("/Library/Caches", buffer, capacity, bytes);
    }
    return {FileCode::InvalidArgument, 0};
}

} // namespace rocks::platform
