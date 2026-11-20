// Windows SDK 헤더를 감싸는 쌍의 시작이다(Docs/Design/UnityBuildPch.ko.md 6.1절 "매크로를 남기는
// 헤더"). include guard가 없다 — 감쌀 때마다 다시 include한다. 사용법과 숨기는 매크로는
// WindowsHeadersEnd.h에 있다.
//
// 하는 일은 둘이다.
// - Windows.h를 열고 A/W 함수 이름 매크로를 지운다(번역 단위에서 처음일 때). 그래서 감싼 구간의
//   헤더는 언제나 그 매크로가 없는 상태로 파싱된다. ID3D12InfoQueue::GetMessage 같은 COM 메서드가
//   include 순서와 상관없이 제 이름으로 선언된다(UE Windows/WindowsHWrapper.h와 같은 순서).
// - 앞선 WindowsHeadersEnd.h가 숨긴 near·far·small·interface·FAR·NEAR를 되돌린다. Windows 헤더는
//   include guard 때문에 이것들을 다시 정의하지 않는데, 뒤에 처음 include되는 COM·DirectX 헤더는
//   있어야 파싱된다.
#include <Base/Compiler.h>

#if ROCKS_PLATFORM_WINDOWS

#ifndef ROCKS_WINDOWS_MACROS_HIDDEN
#include <Windows.h>

#include <Platform/Win64/WindowsHeadersEnd.h>
#endif

#undef ROCKS_WINDOWS_MACROS_HIDDEN

#pragma pop_macro("FAR")
#pragma pop_macro("NEAR")
#pragma pop_macro("near")
#pragma pop_macro("far")
#pragma pop_macro("small")
#pragma pop_macro("interface")
#pragma pop_macro("min")
#pragma pop_macro("max")

#endif
