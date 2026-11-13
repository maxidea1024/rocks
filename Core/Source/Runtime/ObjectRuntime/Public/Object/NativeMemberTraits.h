#pragma once

#include <Object/Object.h>
#include <Object/ObjectRefArray.h>
#include <Object/ObjectRefs.h>
#include <Object/StructRefArray.h>

namespace rocks {

// ROCKS_NATIVE 멤버(리플렉션 클래스의 비영속 런타임 멤버) 타입 검사 형질이다. MetaCompiler가 Clang
// AST로 멤버 타입을 재귀 검사하고(RMC2205·RMC2206), 생성 코드가 직접 타입에 대해 같은 조건을
// static_assert로 한 번 더 둔다. 사용 예(생성 코드):
//   static_assert(!IsNativeStrongReference<decltype(::game::Thing::cache)>, "...");
//   static_assert(!RequiresWorldOnlyOwner<decltype(::game::Thing::lease)>, "...");
namespace detail {

template <class T, class = void>
struct IsCompleteType {
    static constexpr bool value = false;
};

template <class T>
struct IsCompleteType<T, decltype(void(sizeof(T)))> {
    static constexpr bool value = true;
};

template <class T>
struct NativeStrongReference {
    static constexpr bool value = false;
};

template <class T>
struct NativeStrongReference<ObjectRef<T>> {
    static constexpr bool value = true;
};

template <class T>
struct NativeStrongReference<RootRef<T>> {
    static constexpr bool value = true;
};

template <class T>
struct NativeStrongReference<ObjectRefArray<T>> {
    static constexpr bool value = true;
};

template <class S>
struct NativeStrongReference<StructRefArray<S>> {
    static constexpr bool value = true;
};

// Registry가 관리하는 참조 컨테이너(ObjectRefArray·StructRefArray)면 참이다. 직접 타입만 본다.
// 생성 코드가 struct의 ContainsReferenceArray 형질을 C++ 멤버 타입과 대조할 때 쓴다(encode 고정이
// 그 형질에 기대므로 틀린 모델 값이 고정 누락이 되지 않게). 사용 예(생성 코드):
//   static_assert(GeneratedType<S>::ContainsReferenceArray ||
//                 !::rocks::detail::RegistryContainer<decltype(S::slots)>::value, "...");
template <class T>
struct RegistryContainer {
    static constexpr bool value = false;
};

template <class T>
struct RegistryContainer<ObjectRefArray<T>> {
    static constexpr bool value = true;
};

template <class S>
struct RegistryContainer<StructRefArray<S>> {
    static constexpr bool value = true;
};

// 불완전 타입에는 __is_base_of를 쓸 수 없으므로 완전한 타입일 때만 판정한다.
template <class T, bool Complete = IsCompleteType<T>::value>
struct IsObjectType {
    static constexpr bool value = false;
};

template <class T>
struct IsObjectType<T, true> {
    static constexpr bool value = __is_base_of(Object, T);
};

// Object 파생 타입을 가리키는(포인터·참조) 멤버는 추적되지 않는 강한 간선이 되므로 금지한다.
// 포인터·참조의 대상이 다시 강한 참조이면(ObjectRef<T>*, T**) 그것도 금지한다. 불완전 타입은 여기서
// 판정할 수 없어 허용한다(MetaCompiler의 AST 검사가 아는 범위에서 본다).
template <class T>
struct NativeStrongReference<T*> {
    static constexpr bool value =
        IsObjectType<__remove_cv(T)>::value || NativeStrongReference<__remove_cv(T)>::value;
};

template <class T>
struct NativeStrongReference<T&> : NativeStrongReference<T*> {};

template <class T>
struct NativeStrongReference<T&&> : NativeStrongReference<T*> {};

template <class T, class = void>
struct RequiresWorldOnlyOwnerOf {
    static constexpr bool value = false;
};

template <class T>
struct RequiresWorldOnlyOwnerOf<T, decltype(void(T::RocksRequiresWorldOnlyOwner))> {
    static constexpr bool value = T::RocksRequiresWorldOnlyOwner;
};

} // namespace detail

// T(배열이면 원소)가 ObjectRef·RootRef·ObjectRefArray·StructRefArray이거나 Object 파생 타입을
// 가리키는 raw 포인터·참조(대상이 다시 강한 참조인 경우 포함)면 참이다. WeakRef와 ObjectHandle은
// 추적하지 않는 참조라 거짓이다. 직접 타입만 본다 — 멤버를 값으로 담는 타입의 필드·템플릿 인자는
// MetaCompiler의 AST 재귀 검사(RMC2205)가 본다.
template <class T>
inline constexpr bool IsNativeStrongReference =
    detail::NativeStrongReference<__remove_cv(__remove_all_extents(T))>::value;

// T(배열이면 원소)가 `static constexpr bool RocksRequiresWorldOnlyOwner = true;`를 선언했으면
// 참이다. 그런 타입은 WorldOnly 클래스의 ROCKS_NATIVE 멤버로만 둘 수 있다.
template <class T>
inline constexpr bool RequiresWorldOnlyOwner =
    detail::RequiresWorldOnlyOwnerOf<__remove_cv(__remove_all_extents(T))>::value;

} // namespace rocks
