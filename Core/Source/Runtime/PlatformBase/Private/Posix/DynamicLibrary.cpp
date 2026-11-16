#include "../Common/DynamicLibraryInternal.h"
#include "PosixFileError.h"
#include "PosixPlatform.h"

#include <Platform/Memory.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// POSIX(macOS·Android) dlopen backend다. 모든 심볼을 여는 때 결합하고(RTLD_NOW, 늦은 결합 실패가
// 호출 도중 프로세스를 끝내지 않게) 다른 라이브러리의 심볼 검색에 섞지 않는다(RTLD_LOCAL).
namespace rocks::platform::detail {

namespace {

constexpr int OpenFlags = RTLD_NOW | RTLD_LOCAL;

// NUL로 끝나는 사본을 소유한다. dlopen은 길이 지정 문자열을 받지 않는다.
class OwnedPath {
public:
    OwnedPath() noexcept = default;

    ~OwnedPath() noexcept {
        AlignedFree(value_);
    }

    OwnedPath(const OwnedPath&) = delete;
    OwnedPath& operator=(const OwnedPath&) = delete;

    // prefix(NUL 종료)와 text(bytes개)를 이어 담는다. 메모리가 없으면 false다.
    bool Assign(const char* prefix, const char* text, usize bytes) noexcept {
        usize prefixBytes = 0;
        while (prefix[prefixBytes] != '\0') {
            ++prefixBytes;
        }

        value_ = static_cast<char*>(AlignedAllocate(prefixBytes + bytes + 1u, alignof(char)));
        if (value_ == nullptr) {
            return false;
        }

        MemoryCopy(value_, prefix, prefixBytes);
        MemoryCopy(value_ + prefixBytes, text, bytes);
        value_[prefixBytes + bytes] = '\0';
        return true;
    }

    const char* Value() const noexcept {
        return value_;
    }

private:
    char* value_ = nullptr;
};

// 파일 앞 4 byte가 ELF 또는 Mach-O(thin 32·64-bit, fat 32·64-bit) 표지인지다.
bool HasLibraryMagic(const unsigned char* magic) noexcept {
    const bool elf = magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
    // thin Mach-O는 CPU byte 순서(arm64·x86-64 모두 little endian)로 0xFEEDFACE·0xFEEDFACF다.
    const bool thin = (magic[0] == 0xCE || magic[0] == 0xCF) && magic[1] == 0xFA &&
                      magic[2] == 0xED && magic[3] == 0xFE;
    // fat Mach-O는 big endian으로 0xCAFEBABE·0xCAFEBABF다.
    const bool fat = magic[0] == 0xCA && magic[1] == 0xFE && magic[2] == 0xBA &&
                     (magic[3] == 0xBE || magic[3] == 0xBF);
    return elf || thin || fat;
}

// dlopen 실패를 분류한다. dlopen은 errno를 정하지 않으므로 파일 상태를 다시 본다. 성공 경로에서는
// 파일을 보지 않는다(macOS 시스템 라이브러리는 디스크에 파일 없이 dyld 공유 캐시에만 있다).
FileStatus ClassifyOpenFailure(const char* path) noexcept {
    struct stat state{};
    if (stat(path, &state) != 0) {
        return PosixFileError(errno);
    }

    if (S_ISDIR(state.st_mode)) {
        return {FileCode::WrongType, static_cast<uint32>(EISDIR)};
    }

    const int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return PosixFileError(errno);
    }

    unsigned char magic[4]{};
    const ssize_t got = read(descriptor, magic, sizeof(magic));
    (void)close(descriptor);
    if (got != static_cast<ssize_t>(sizeof(magic)) || !HasLibraryMagic(magic)) {
        return {FileCode::WrongType, 0};
    }

    // 형식은 맞지만 dyld·링커가 거부했다(의존 라이브러리 없음, 다른 아키텍처, 서명 등).
    return {FileCode::SystemError, 0};
}

FileStatus Load(const char* path, uptr& handle) noexcept {
    void* library = dlopen(path, OpenFlags);
    if (library == nullptr) {
        return ClassifyOpenFailure(path);
    }

    handle = reinterpret_cast<uptr>(library);
    return {};
}

} // namespace

FileStatus DynamicLibraryOpenPath(const char* path, usize pathBytes, uptr& handle) noexcept {
    // '/'로 시작하지 않으면 dlopen이 검색 경로(현재 디렉터리 포함)에서 찾으므로 거부한다.
    if (path[0] != '/') {
        return {FileCode::InvalidArgument, 0};
    }

    OwnedPath native;
    if (!native.Assign("", path, pathBytes)) {
        return {FileCode::OutOfMemory, 0};
    }

    return Load(native.Value(), handle);
}

FileStatus DynamicLibraryOpenSystem(const char* name, usize nameBytes, uptr& handle) noexcept {
    const char* directory = PosixSystemLibraryDirectory();
    OwnedPath native;
    if (!native.Assign(directory, name, nameBytes)) {
        return {FileCode::OutOfMemory, 0};
    }

    if (directory[0] != '\0') {
        return Load(native.Value(), handle);
    }

    // 이름만으로 찾을 때는 확인할 파일 경로가 없다. 링커가 찾지 못한 것으로 본다.
    void* library = dlopen(native.Value(), OpenFlags);
    if (library == nullptr) {
        return {FileCode::NotFound, 0};
    }

    handle = reinterpret_cast<uptr>(library);
    return {};
}

void* DynamicLibraryFindSymbol(uptr handle, const char* symbol) noexcept {
    return dlsym(reinterpret_cast<void*>(handle), symbol);
}

FileStatus DynamicLibraryClose(uptr handle) noexcept {
    if (dlclose(reinterpret_cast<void*>(handle)) != 0) {
        return {FileCode::SystemError, 0};
    }

    return {};
}

} // namespace rocks::platform::detail
