#include <Platform/Generic/Path.h>
#include <Platform/Path.h>
#include <Platform/Sync.h>

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

// Android 경로 backend다. 앱 전용 저장소(내부 files·cache)는 Java 쪽 Context에서만 알 수 있으므로
// 앱이 시작 시 RegisterApplicationDirectories로 넘겨야 한다. 등록 전에는 UserData·UserCache가
// NotFound이며 Temporary·Home은 셸 실행(adb shell) 환경 값을 쓴다.
namespace rocks::platform {
namespace {

struct AndroidPlatformPath : GenericPlatformPath {
    static constexpr PathStyle HostStyle() noexcept {
        return PathStyle::Posix;
    }
};

constexpr usize MaxRegistered = PATH_MAX;

struct ApplicationDirectories {
    Mutex mutex;
    char data[MaxRegistered]{};
    char cache[MaxRegistered]{};
    usize dataBytes = 0;
    usize cacheBytes = 0;
};

ApplicationDirectories& Registered() noexcept {
    static ApplicationDirectories directories;
    return directories;
}

usize Length(const char* text) noexcept {
    usize length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

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

// 등록된 값을 잠금 안에서 복사한다. 등록되지 않았으면 NotFound다.
FileStatus WriteRegistered(bool cache, char* buffer, usize capacity, usize& bytes) noexcept {
    ApplicationDirectories& directories = Registered();
    LockGuard lock(directories.mutex);
    const usize length = cache ? directories.cacheBytes : directories.dataBytes;
    if (length == 0) {
        return {FileCode::NotFound, 0};
    }
    return Write(cache ? directories.cache : directories.data, length, buffer, capacity, bytes);
}

bool CopyRegistered(const char* source, char* target, usize& targetBytes) noexcept {
    const usize length = Length(source);
    if (length == 0 || length >= MaxRegistered || source[0] != '/') {
        return false;
    }
    for (usize index = 0; index <= length; ++index) {
        target[index] = source[index];
    }
    targetBytes = length;
    return true;
}

} // namespace

PathStyle HostPathStyle() noexcept {
    return AndroidPlatformPath::HostStyle();
}

char NativePathSeparator() noexcept {
    return AndroidPlatformPath::NativeSeparator();
}

FileStatus RegisterApplicationDirectories(const char* dataPath, const char* cachePath) noexcept {
    if (dataPath == nullptr || cachePath == nullptr) {
        return {FileCode::InvalidArgument, 0};
    }
    char data[MaxRegistered];
    char cache[MaxRegistered];
    usize dataBytes = 0;
    usize cacheBytes = 0;
    if (!CopyRegistered(dataPath, data, dataBytes) ||
        !CopyRegistered(cachePath, cache, cacheBytes)) {
        return {FileCode::InvalidArgument, 0};
    }
    ApplicationDirectories& directories = Registered();
    LockGuard lock(directories.mutex);
    for (usize index = 0; index <= dataBytes; ++index) {
        directories.data[index] = data[index];
    }
    for (usize index = 0; index <= cacheBytes; ++index) {
        directories.cache[index] = cache[index];
    }
    directories.dataBytes = dataBytes;
    directories.cacheBytes = cacheBytes;
    return {};
}

FileStatus GetStandardDirectory(StandardDirectory kind, char* buffer, usize capacity,
                                usize& bytes) noexcept {
    if (buffer == nullptr && capacity != 0) {
        return {FileCode::InvalidArgument, 0};
    }
    switch (kind) {
    case StandardDirectory::Executable: {
        char path[PATH_MAX];
        const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
        if (length <= 0) {
            return {FileCode::SystemError, static_cast<uint32>(errno)};
        }
        usize end = static_cast<usize>(length);
        while (end > 0 && path[end - 1] != '/') {
            --end;
        }
        return Write(path, end, buffer, capacity, bytes);
    }
    case StandardDirectory::Working: {
        char path[PATH_MAX];
        if (getcwd(path, sizeof(path)) == nullptr) {
            return {errno == ERANGE ? FileCode::Overflow : FileCode::SystemError,
                    static_cast<uint32>(errno)};
        }
        return Write(path, Length(path), buffer, capacity, bytes);
    }
    case StandardDirectory::Temporary: {
        // 앱이면 등록된 cache 디렉터리, 셸 실행이면 TMPDIR 또는 /data/local/tmp다.
        auto registered = WriteRegistered(true, buffer, capacity, bytes);
        if (registered.code != FileCode::NotFound) {
            return registered;
        }
        const char* fallback = getenv("TMPDIR");
        if (fallback == nullptr || fallback[0] == '\0') {
            fallback = "/data/local/tmp";
        }
        return Write(fallback, Length(fallback), buffer, capacity, bytes);
    }
    case StandardDirectory::Home: {
        auto registered = WriteRegistered(false, buffer, capacity, bytes);
        if (registered.code != FileCode::NotFound) {
            return registered;
        }
        const char* home = getenv("HOME");
        if (home == nullptr || home[0] == '\0') {
            return {FileCode::NotFound, 0};
        }
        return Write(home, Length(home), buffer, capacity, bytes);
    }
    case StandardDirectory::UserData:
        return WriteRegistered(false, buffer, capacity, bytes);
    case StandardDirectory::UserCache:
        return WriteRegistered(true, buffer, capacity, bytes);
    }
    return {FileCode::InvalidArgument, 0};
}

} // namespace rocks::platform
