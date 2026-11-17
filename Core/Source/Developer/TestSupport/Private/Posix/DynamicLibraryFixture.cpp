#include <TestSupport/DynamicLibraryFixture.h>

#include <string.h>
#include <sys/utsname.h>

// macOS·Android 시험 지원이다. 시험 라이브러리 생성(Mach-O는 코드 서명까지, ELF는 dynamic 구역)은
// 후속이므로 OS 공급 라이브러리 정보만 준다. 두 OS가 이 구현을 함께 쓰므로 실행 중인 커널로
// 가린다(ProcessProbe.cpp와 같다).
namespace rocks::test {

namespace {

bool IsDarwin() noexcept {
    utsname name{};
    return uname(&name) == 0 && strcmp(name.sysname, "Darwin") == 0;
}

bool CopyText(const char* text, char* buffer, usize capacity) noexcept {
    const usize bytes = strlen(text);
    if (bytes + 1u > capacity) {
        return false;
    }

    memcpy(buffer, text, bytes + 1u);
    return true;
}

} // namespace

bool DynamicLibraryFixturesSupported() noexcept {
    return false;
}

const char* DynamicLibraryExtension() noexcept {
    return IsDarwin() ? ".dylib" : ".so";
}

bool WriteFixtureLibrary(const char*, const FixtureLibrary&) noexcept {
    return false;
}

bool SystemLibraryForTest(char* path, usize capacity, const char*& systemName,
                          const char*& symbol) noexcept {
    symbol = "malloc";
    // macOS 시스템 라이브러리는 dyld 공유 캐시에만 있고 디스크 파일은 없지만 이 절대 경로로 열린다.
    if (IsDarwin()) {
        systemName = "libSystem.B.dylib";
        return CopyText("/usr/lib/libSystem.B.dylib", path, capacity);
    }

    // Android는 64-bit 프로세스만 지원하므로 lib64다.
    systemName = "libc.so";
    return CopyText("/system/lib64/libc.so", path, capacity);
}

bool LibraryIsLoaded(const char*) noexcept {
    return false;
}

bool LoadWithDefaultSearch(const char*) noexcept {
    return false;
}

bool PrependSearchPath(const char*) noexcept {
    return false;
}

} // namespace rocks::test
