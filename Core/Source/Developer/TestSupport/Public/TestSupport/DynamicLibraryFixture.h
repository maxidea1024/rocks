#pragma once

#include <Base/Types.h>

// DynamicLibrary 시험(PlatformBase/Tests/DynamicLibraryTests.cpp)이 쓰는 플랫폼 도구다.
//
// 시험 라이브러리: Rocks.Build에는 공유 라이브러리 모듈 종류가 없고 더하지 않으므로
// (ShaderSystem.ko.md 8절), 시험이 실행 중에 OS 로더가 받는 최소 공유 라이브러리를 직접 쓴다.
// 코드 없이 데이터 export와 import만 담는다(DllMain·초기화 코드 없음). 그래서 실제 OS 로더의 경로
// 검색·의존 해결·심볼 조회·해제를 그대로 시험하면서 시험 지원 코드가 컴파일러·링커에 기대지 않는다.
// 현재는 Windows(PE32+, 실행 중인 프로세스와 같은 machine)만 만든다. macOS(Mach-O·코드 서명)와
// Android(ELF)는 후속이며 그 플랫폼에서는 DynamicLibraryFixturesSupported가 false다.
namespace rocks::test {

// 데이터 export 하나다. 이름이 가리키는 주소에 value(uint32, little endian)가 있다.
struct FixtureExport {
    const char* name = nullptr;
    uint32 value = 0;
};

// 시험 라이브러리를 쓰는 입력이다. importLibrary가 null이 아니면 그 라이브러리(파일 이름, 예:
// "B.dll")의 importSymbol을 import하고, 로더가 채우는 import 칸(포인터 하나)을 importSlotExport
// 이름으로 export한다. 시험은 그 칸의 값이 의존 라이브러리의 export 주소와 같은지로 의존 해결을
// 확인한다.
struct FixtureLibrary {
    const FixtureExport* exports = nullptr;
    usize exportCount = 0;
    const char* importLibrary = nullptr;
    const char* importSymbol = nullptr;
    const char* importSlotExport = nullptr;
};

// 이 플랫폼에서 WriteFixtureLibrary로 시험 라이브러리를 만들 수 있는지다(Windows true).
bool DynamicLibraryFixturesSupported() noexcept;

// 이 플랫폼의 공유 라이브러리 확장자다(".dll", ".dylib", ".so").
const char* DynamicLibraryExtension() noexcept;

// path(UTF-8 절대 경로)에 시험 라이브러리를 쓴다. 기존 파일은 덮어쓴다. 지원하지 않는 플랫폼이거나
// 입력이 잘못되었거나 쓰기에 실패하면 false다.
bool WriteFixtureLibrary(const char* path, const FixtureLibrary& library) noexcept;

// OS가 공급하는 시험 대상 라이브러리다. path는 절대 경로(Windows System32의 kernel32.dll, macOS
// /usr/lib/libSystem.B.dylib, Android /system/lib64/libc.so), systemName은 OpenSystem 이름,
// symbol은 그 안에 있는 export다. path가 capacity에 들어가지 않으면 false다.
bool SystemLibraryForTest(char* path, usize capacity, const char*& systemName,
                          const char*& symbol) noexcept;

// 파일 이름(기본 이름, 예: "A.dll")의 라이브러리가 지금 이 프로세스에 올라와 있는지다. 확인할 수
// 없는 플랫폼은 false다(시험 라이브러리를 만들 수 있는 플랫폼에서만 쓴다).
bool LibraryIsLoaded(const char* fileName) noexcept;

// 대조군: OS 기본 검색 순서(Windows는 LoadLibraryExW 플래그 0 — 실행 파일 디렉터리, 시스템
// 디렉터리, 현재 디렉터리, PATH)로 path를 열고 바로 내린다. 열렸으면 true다. probe 자식에서
// DynamicLibrary가 현재 디렉터리·PATH의 의존 DLL을 쓰지 않는다는 음성 시험이 헛돌지 않음(기본
// 검색이면 찾음)을 보이는 데 쓴다.
bool LoadWithDefaultSearch(const char* path) noexcept;

// 현재 프로세스의 PATH 맨 앞에 directory(UTF-8)를 더한다. probe 자식에서만 쓴다(시험 프로세스의
// 환경을 바꾸지 않는다). 실패하면 false다.
bool PrependSearchPath(const char* directory) noexcept;

} // namespace rocks::test
