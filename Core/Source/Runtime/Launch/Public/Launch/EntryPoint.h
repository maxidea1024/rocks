#pragma once

#include <Base/Compiler.h>
#include <Launch/Application.h>

// 실행 파일의 플랫폼 진입점을 만드는 매크로다(UE IMPLEMENT_APPLICATION 대응, Docs/Design/
// CrashHandlingAndLaunch.ko.md 7.4절 D3·D7). 실행 파일 소스 하나에 한 번만 쓴다. Describe는
// `const rocks::launch::ApplicationDescription& Describe() noexcept` 모양의 함수다.
//
// - ROCKS_IMPLEMENT_APPLICATION(Describe): 콘솔 프로그램. Windows는 wmain(UTF-16 인자), 그
//   밖은 main이다. 빌드 규칙은 WindowsSubsystem.Console(기본)이다.
// - ROCKS_IMPLEMENT_WINDOWED_APPLICATION(Describe): Windows GUI 프로그램(콘솔 창 없음). Windows는
//   wWinMain이고 인자는 CRT가 나눈 __argc·__wargv다. 빌드 규칙에 WindowsSubsystem = Windows를 둔다.
//   그 밖의 플랫폼은 콘솔과 같은 main이다.
// 매크로와 빌드 규칙의 subsystem이 맞지 않으면 진입 기호가 없어 링크 오류로 드러난다. Windows에서
// 좁은 문자 main(char**)은 쓰지 않는다(ANSI argv는 비 ASCII 인자를 깨뜨린다).
//
// 이 헤더는 <Windows.h>를 넣지 않는다. wWinMain은 선언만으로 쓴다(HINSTANCE는 struct HINSTANCE__*).
//
// 사용 예:
//   ROCKS_IMPLEMENT_APPLICATION(Describe)
#if ROCKS_PLATFORM_WINDOWS

struct HINSTANCE__;

#define ROCKS_IMPLEMENT_APPLICATION(DescribeFunction)                                              \
    int wmain(int argc, wchar_t** argv) {                                                          \
        return ::rocks::launch::RunApplication(DescribeFunction(), argc, argv);                    \
    }

#define ROCKS_IMPLEMENT_WINDOWED_APPLICATION(DescribeFunction)                                     \
    extern "C" int __stdcall wWinMain(HINSTANCE__*, HINSTANCE__*, wchar_t*, int) {                 \
        return ::rocks::launch::RunWindowedApplication(DescribeFunction());                        \
    }

#else

#define ROCKS_IMPLEMENT_APPLICATION(DescribeFunction)                                              \
    int main(int argc, char** argv) {                                                              \
        return ::rocks::launch::RunApplication(DescribeFunction(), argc, argv);                    \
    }

#define ROCKS_IMPLEMENT_WINDOWED_APPLICATION(DescribeFunction)                                     \
    ROCKS_IMPLEMENT_APPLICATION(DescribeFunction)

#endif
