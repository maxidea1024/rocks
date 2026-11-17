#pragma once

#include <Base/TypeTraits.h>
#include <Core/Traits.h>
#include <new>

namespace rocks {

// 값 범주만 rvalue로 바꾼다. 실제 자원 이동이나 원본 초기화는 대상 타입의 연산이 수행한다.
template <typename T>
constexpr RemoveReferenceT<T>&& Move(T&& value) noexcept {
    return static_cast<RemoveReferenceT<T>&&>(value);
}

// 전달받은 인자의 원래 lvalue/rvalue 구분을 생성자·callable까지 유지한다.
template <typename T>
constexpr T&& Forward(RemoveReferenceT<T>& value) noexcept {
    return static_cast<T&&>(value);
}

template <typename T>
constexpr T&& Forward(RemoveReferenceT<T>&& value) noexcept {
    static_assert(!IsLvalueReferenceV<T>);
    return static_cast<T&&>(value);
}

// storage에 T(args...)를 생성자 호출로 만든다. 그 생성이 non-trivial일 때만 호출할 수 있다:
// IsConstructibleV<T, Args&&...>이고 !IsTriviallyConstructibleV<T, Args&&...>(생성 인자별 판정,
// IsTrivialV<T> 하나로 가르지 않는다). ConstructAt<int>(p)·ConstructAt<int>(p, 1)처럼 trivial한
// 생성은 컴파일 오류다 — 값 저장이 필요한 generic 경로는 InitializeAt을 쓴다.
// storage: sizeof(T) 이상, alignof(T)로 정렬된, 객체 수명을 시작할 수 있는 저장소(caller 보장).
// 반환: 생성한 객체. 메모리를 할당하지 않으며 객체 소멸(DestroyAt)과 storage 회수는 caller
// 책임이다.
//
// 사용 예:
//   Widget* widget = ConstructAt<Widget>(memory, allocator, name);
template <typename T, typename... Args>
    requires(IsConstructibleV<T, Args && ...> && !IsTriviallyConstructibleV<T, Args && ...>)
T* ConstructAt(void* storage, Args&&... args) noexcept(IsNothrowConstructibleV<T, Args&&...>) {
    return ::new (storage) T(Forward<Args>(args)...);
}

// storage에 T의 수명을 시작하고 값을 초기화하는 generic 진입점이다. trivial 생성이어도 초기화를
// 생략하지 않는다: InitializeAt<int>(p)는 0을, InitializeAt<int>(p, 7)는 7을 저장한다. 인자 평가,
// 변환 생성자, 사용자 정의 이동 생성자의 부작용도 그대로다. non-trivial 생성만 ConstructAt으로
// 보낸다. storage·반환·수명 규칙은 ConstructAt과 같다. 연속 범위의 복사·이동은 컨테이너의 bulk 경로
// (memcpy/memmove)를 쓴다.
//
// 사용 예:
//   T* slot = InitializeAt<T>(storage, Forward<Args>(args)...);   // T가 int여도 컴파일된다
template <typename T, typename... Args>
    requires(IsConstructibleV<T, Args && ...>)
T* InitializeAt(void* storage, Args&&... args) noexcept(IsNothrowConstructibleV<T, Args&&...>) {
    if constexpr (IsTriviallyConstructibleV<T, Args&&...>) {
        return ::new (storage) T(Forward<Args>(args)...);
    } else {
        return ConstructAt<T>(storage, Forward<Args>(args)...);
    }
}

// 살아 있는 객체의 non-trivial 소멸자를 부른다. IsDestructibleV<T>이고 !IsTriviallyDestructibleV<T>
// 일 때만 호출할 수 있고, DestroyAt(int*)처럼 trivial 소멸은 컴파일 오류다. generic 코드는
// if constexpr (!IsTriviallyDestructibleV<T>)로 호출과 소멸 전용 반복문을 함께 없앤다(생략한
// trivial 객체의 수명은 저장소 재사용·반환 때 끝난다). nullptr·중복 소멸은 허용하지 않으며
// storage는 해제하지 않는다.
//
// 사용 예:
//   if constexpr (!IsTriviallyDestructibleV<T>) {
//       DestroyAt(value);
//   }
template <typename T>
    requires(IsDestructibleV<T> && !IsTriviallyDestructibleV<T>)
void DestroyAt(T* object) noexcept(IsNothrowDestructibleV<T>) {
    object->~T();
}

} // namespace rocks
