#pragma once

namespace rocks {

// Core에 필요한 타입 변환·검사만 제공한다. 실행 시 코드나 저장소를 만들지 않는다.
// 완전한 범용 traits 라이브러리를 대신하는 계약은 아니다.
template <typename T>
struct RemoveReference {
    using Type = T;
};

template <typename T>
struct RemoveReference<T&> {
    using Type = T;
};

template <typename T>
struct RemoveReference<T&&> {
    using Type = T;
};

template <typename T>
using RemoveReferenceT = typename RemoveReference<T>::Type;

template <typename T>
struct RemoveConst {
    using Type = T;
};

template <typename T>
struct RemoveConst<const T> {
    using Type = T;
};

template <typename T>
using RemoveConstT = typename RemoveConst<T>::Type;

template <typename A, typename B>
inline constexpr bool IsSameV = false;

template <typename T>
inline constexpr bool IsSameV<T, T> = true;

template <typename T>
inline constexpr bool IsLvalueReferenceV = false;

template <typename T>
inline constexpr bool IsLvalueReferenceV<T&> = true;

// 컨테이너가 재배치 이후 실패 복구를 요구하지 않도록 생성·소멸의 noexcept 전제를 검사한다.
// compiler intrinsic 사용은 지원하는 Clang 계열 toolchain의 경계 안에 둔다.
template <typename T>
inline constexpr bool IsNothrowMoveConstructibleV = __is_nothrow_constructible(T, T&&);

template <typename T>
inline constexpr bool IsNothrowDestructibleV = __is_nothrow_destructible(T);

} // namespace rocks
