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
// 보장하는 것은 MSVC 런타임 기준 집합뿐이다: placement new·delete, 전역 operator new·delete 선언,
// align_val_t, nothrow_t·nothrow. launder, bad_alloc, bad_array_new_length,
// hardware_destructive_interference_size, destroying_delete_t, set_new_handler, 기능 시험 매크로
// (__cpp_lib_*)는 MSVC에서 이 헤더만으로 보이지 않는다. libc++에서는 <new> 전체가 보이므로, 그런
// 이름을 쓰는 코드는 Posix·Mac 전용 소스에서 컴파일되다가 공통 소스로 옮기면 Windows에서만 깨진다.
// 그런 이름이 필요하면 <new>를 include한다. 시험 소스 밖이면 정책의 허용 목록
// (PolicyChecks.PlacementNewAllowances)에 경로와 사유를 올린다. <vcruntime_new.h>를 직접
// include하지 않는다(MSVC 런타임에만 있다).
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
