// Windows SDK 헤더를 감싸는 쌍의 시작이다(Docs/Design/UnityBuildPch.ko.md 6.1절 "매크로를 남기는
// 헤더"). include guard가 없다 — 감쌀 때마다 다시 include한다. 사용법과 숨기는 매크로는
// WindowsHeadersEnd.h에 있다.
//
// 하는 일은 둘이다.
// - Windows.h를 열고 WindowsHeadersEnd.h를 거쳐 A/W 함수 이름 매크로를 지운다. 그래서 감싼 구간의
//   헤더는 언제나 그 매크로가 없는 상태로 파싱된다. ID3D12InfoQueue::GetMessage 같은 COM 메서드가
//   include 순서와 상관없이 제 이름으로 선언된다(UE Windows/WindowsHWrapper.h와 같은 순서).
// - WindowsHeadersEnd.h가 지운 평범한 이름을 SDK와 같은 정의로 다시 정의한다. Windows 헤더는
//   include guard 때문에 이것들을 다시 정의하지 않는데, 뒤에 처음 include되는 COM·DirectX 헤더는
//   있어야 파싱된다. SDK 헤더가 같은 이름을 다시 정의해도 토큰이 같아 경고가 없다.
//
// #pragma push_macro/pop_macro를 쓰지 않는다. clang의 PCH는 push 스택을 저장하지 않아서, PCH
// 안의 End가 저장한 것을 PCH를 쓰는 번역 단위의 Begin이 되돌리지 못한다. 정의를 직접 다시 쓰면
// 번역 단위나 PCH 경계를 넘는 상태가 없다.
#include <Base/Compiler.h>

#if ROCKS_PLATFORM_WINDOWS

#include <Windows.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

// minwindef.h의 정의다.
#undef FAR
#undef NEAR
#define near
#define far
#define FAR far
#define NEAR near
#define pascal __stdcall
#define cdecl

// rpcndr.h의 정의다.
#define small char
#define hyper __int64

// combaseapi.h·basetyps.h의 정의다.
#ifndef __STRUCT__
#define __STRUCT__ struct
#endif

#define interface __STRUCT__

#endif
