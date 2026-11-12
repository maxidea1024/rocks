#pragma once

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

// caller가 sizeof(T) 이상의 정렬된 storage와 객체 수명 시작 가능 여부를 보장한다.
// 메모리를 할당하지 않으며, 반환 객체의 소멸과 storage 회수도 caller 책임이다.
template <typename T, typename... Args>
T* ConstructAt(void* storage, Args&&... args) noexcept(__is_nothrow_constructible(T, Args&&...)) {
    return ::new (storage) T(Forward<Args>(args)...);
}

// 살아 있는 객체의 수명만 끝낸다. nullptr·중복 소멸을 허용하지 않으며 storage는 해제하지 않는다.
template <typename T>
void DestroyAt(T* object) noexcept(__is_nothrow_destructible(T)) {
    object->~T();
}

} // namespace rocks
