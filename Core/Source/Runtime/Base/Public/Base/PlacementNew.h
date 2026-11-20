#pragma once

// placement new(::new (storage) T(...))의 선언을 여는 헤더다. 엔진 소스는 <new> 대신 이 헤더를
// include한다(Docs/Design/UnityBuildPch.ko.md 8.4절 "placement new 헤더", 정책
// placement-new-header).
//
// MSVC STL의 <new>는 <exception>과 그것이 끌어오는 <type_traits>·<cstdlib> 등을 함께 열어, PCH를
// 쓰지 않는 번역 단위마다 0.1 s 넘게 든다. placement new 선언은 <new>가 include하는
// <vcruntime_new.h>에 있으므로 그것만 연다. 다른 표준 라이브러리(libc++·libstdc++)에서는 <new>가
// 가벼워 그대로 쓴다.
//
// 보이는 것: placement new·delete와 전역 operator new·delete 선언. launder·bad_alloc처럼 <new>의
// 다른 것이 필요하면 <new>를 include한다(시험 소스 밖에서는 정책 예외가 필요하다).
//
// 사용 예:
//   #include <Base/PlacementNew.h>
//   Widget* widget = ::new (storage) Widget(argument);
// 값 생성은 보통 Core/Move.h의 ConstructAt·InitializeAt을 쓴다(그 헤더가 이 헤더를 include한다).

#if __has_include(<vcruntime_new.h>)
#include <vcruntime_new.h>
#else
#include <new>
#endif
