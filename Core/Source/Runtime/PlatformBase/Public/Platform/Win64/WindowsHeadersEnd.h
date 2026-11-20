// Windows SDK 헤더를 감싸는 쌍의 끝이다(Docs/Design/UnityBuildPch.ko.md 6.1절 "매크로를 남기는
// 헤더"). include guard가 없다 — 감쌀 때마다 다시 include한다.
//
// Windows SDK 헤더는 near·far·small·interface 같은 평범한 이름과 A/W 함수 이름(CreateDirectory →
// CreateDirectoryW)을 매크로로 정의해 번역 단위 끝까지 남긴다. 유니티 묶음에서는 그 매크로가 뒤
// 구성원까지 살아남아 같은 이름을 쓰는 소스를 깨뜨린다. 이 헤더가 그 매크로를 지운다.
//
// 사용 예:
//   #include <Platform/Win64/WindowsHeadersBegin.h>
//
//   (여기에 Windows SDK 헤더를 include한다: d3d11_4.h, dxgi1_6.h, ShlObj.h, ...)
//
//   #include <Platform/Win64/WindowsHeadersEnd.h>
//
// - 감싼 구간에는 Windows SDK 헤더만 둔다. 저장소 헤더는 구간 밖에서 include한다.
// - WindowsHeadersBegin.h가 Windows.h를 열므로 구간에 Windows.h를 따로 적지 않아도 된다.
// - near·far·small·interface·min·max는 저장했다가 다음 WindowsHeadersBegin.h가 되돌린다(SDK 헤더를
//   파싱하는 데 필요하다). FAR·NEAR는 그동안 빈 매크로로 둔다. DEFINE_GUID·FD_SET 같은 SDK 매크로가
//   쓰는 자리에서 FAR로 펼쳐지는데, 원래 정의(#define FAR far)는 far를 숨긴 뒤에는 컴파일되지
//   않는다.
// - A/W 함수 이름 매크로는 지우고 되돌리지 않는다. 이 헤더 뒤의 코드는 W 이름을 직접 쓴다
//   (CreateDirectoryW). 목록은 UE Windows/PostWindowsApi.h와 같다. 엔진 식별자와 겹치는 매크로가
//   더 나오면 여기에 더한다.
// - 유니티를 켠 모듈에서 감싸지 않은 include는 정책 검사 unity-macro-leak이 거부한다.
#include <Base/Compiler.h>

#if ROCKS_PLATFORM_WINDOWS

#ifndef ROCKS_WINDOWS_MACROS_HIDDEN
#define ROCKS_WINDOWS_MACROS_HIDDEN 1

#pragma push_macro("FAR")
#pragma push_macro("NEAR")
#pragma push_macro("near")
#pragma push_macro("far")
#pragma push_macro("small")
#pragma push_macro("interface")
#pragma push_macro("min")
#pragma push_macro("max")

#undef FAR
#undef NEAR
#undef near
#undef far
#undef small
#undef interface
#undef min
#undef max

#define FAR
#define NEAR

#endif // ROCKS_WINDOWS_MACROS_HIDDEN

#undef CaptureStackBackTrace
#undef CopyFile
#undef CreateDesktop
#undef CreateDirectory
#undef CreateFont
#undef DeleteFile
#undef DrawText
#undef FindWindow
#undef GetClassInfo
#undef GetClassName
#undef GetCommandLine
#undef GetCurrentTime
#undef GetEnvironmentVariable
#undef GetFileAttributes
#undef GetFreeSpace
#undef GetMessage
#undef GetNextSibling
#undef GetObject
#undef GetProp
#undef GetTempFileName
#undef IsMaximized
#undef IsMinimized
#undef LoadString
#undef MemoryBarrier
#undef MoveFile
#undef PlaySound
#undef PostMessage
#undef ReportEvent
#undef SendMessage
#undef SetPort
#undef SetProp
#undef UpdateResource
#undef Yield

#endif
