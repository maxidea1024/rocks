#pragma once

// 컴파일 시간 타입 검사와 변환의 공용 진입점이다. 실행 시 코드·할당·STL 의존성이 없다.
// Base/Types.h가 NumericLimits를 통해 이 헤더를 쓰므로 Types.h·Compiler.h를 include하지 않는다.
// 값은 Trait<...>::Value 또는 TraitV<...>, 변환은 Trait<...>::Type 또는 TraitT<...>로 읽는다.
// 판별 대상의 완전성 등 전제는 대응하는 C++ type trait과 같다. 불완전 타입의 결과를 캐시하지
// 않는다. compiler intrinsic은 이 헤더 안에서만 사용한다. 새 기능도 이 API에 추가한 뒤 사용처에서
// 참조한다.

namespace rocks {

template <typename T, T Constant>
struct IntegralConstant {
    static constexpr T Value = Constant;
    using ValueType = T;
    using Type = IntegralConstant;

    constexpr operator ValueType() const noexcept {
        return Value;
    }
    constexpr ValueType operator()() const noexcept {
        return Value;
    }
};

template <bool Value>
using BoolConstant = IntegralConstant<bool, Value>;

using TrueType = BoolConstant<true>;
using FalseType = BoolConstant<false>;

template <bool Condition, typename IfTrue, typename IfFalse>
struct Conditional {
    using Type = IfTrue;
};

template <typename IfTrue, typename IfFalse>
struct Conditional<false, IfTrue, IfFalse> {
    using Type = IfFalse;
};

template <bool Condition, typename IfTrue, typename IfFalse>
using ConditionalT = typename Conditional<Condition, IfTrue, IfFalse>::Type;

template <bool Condition, typename T = void>
struct EnableIf {};

template <typename T>
struct EnableIf<true, T> {
    using Type = T;
};

template <bool Condition, typename T = void>
using EnableIfT = typename EnableIf<Condition, T>::Type;

template <typename...>
using VoidT = void;

template <typename T>
struct TypeIdentity {
    using Type = T;
};

template <typename T>
using TypeIdentityT = typename TypeIdentity<T>::Type;

// 남은 피연산자를 인스턴스화하지 않는 논리 조합이다. 선택된 피연산자의 부가 멤버도 보존한다.
template <typename...>
struct Conjunction : TrueType {};

template <typename First>
struct Conjunction<First> : First {};

template <typename First, typename... Rest>
struct Conjunction<First, Rest...>
    : ConditionalT<static_cast<bool>(First::Value), Conjunction<Rest...>, First> {};

template <typename... Traits>
inline constexpr bool ConjunctionV = static_cast<bool>(Conjunction<Traits...>::Value);

template <typename...>
struct Disjunction : FalseType {};

template <typename First>
struct Disjunction<First> : First {};

template <typename First, typename... Rest>
struct Disjunction<First, Rest...>
    : ConditionalT<static_cast<bool>(First::Value), First, Disjunction<Rest...>> {};

template <typename... Traits>
inline constexpr bool DisjunctionV = static_cast<bool>(Disjunction<Traits...>::Value);

template <typename Trait>
struct Negation : BoolConstant<!static_cast<bool>(Trait::Value)> {};

template <typename Trait>
inline constexpr bool NegationV = Negation<Trait>::Value;

// 분류·한정·클래스 속성. compiler 확장 한정 포인터도 동일한 intrinsic으로 판정한다.
template <typename T>
struct IsVoid : BoolConstant<__is_void(T)> {};

template <typename T>
inline constexpr bool IsVoidV = IsVoid<T>::Value;

template <typename T>
struct IsIntegral : BoolConstant<__is_integral(T)> {};

template <typename T>
inline constexpr bool IsIntegralV = IsIntegral<T>::Value;

template <typename T>
struct IsFloatingPoint : BoolConstant<__is_floating_point(T)> {};

template <typename T>
inline constexpr bool IsFloatingPointV = IsFloatingPoint<T>::Value;

template <typename T>
struct IsArray : BoolConstant<__is_array(T)> {};

template <typename T>
inline constexpr bool IsArrayV = IsArray<T>::Value;

template <typename T>
struct IsBoundedArray : BoolConstant<__is_bounded_array(T)> {};

template <typename T>
inline constexpr bool IsBoundedArrayV = IsBoundedArray<T>::Value;

template <typename T>
struct IsUnboundedArray : BoolConstant<__is_unbounded_array(T)> {};

template <typename T>
inline constexpr bool IsUnboundedArrayV = IsUnboundedArray<T>::Value;

template <typename T>
struct IsEnum : BoolConstant<__is_enum(T)> {};

template <typename T>
inline constexpr bool IsEnumV = IsEnum<T>::Value;

template <typename T>
struct IsScopedEnum : BoolConstant<__is_scoped_enum(T)> {};

template <typename T>
inline constexpr bool IsScopedEnumV = IsScopedEnum<T>::Value;

template <typename T>
struct IsUnion : BoolConstant<__is_union(T)> {};

template <typename T>
inline constexpr bool IsUnionV = IsUnion<T>::Value;

template <typename T>
struct IsClass : BoolConstant<__is_class(T)> {};

template <typename T>
inline constexpr bool IsClassV = IsClass<T>::Value;

template <typename T>
struct IsFunction : BoolConstant<__is_function(T)> {};

template <typename T>
inline constexpr bool IsFunctionV = IsFunction<T>::Value;

template <typename T>
struct IsPointer : BoolConstant<__is_pointer(T)> {};

template <typename T>
inline constexpr bool IsPointerV = IsPointer<T>::Value;

template <typename T>
struct IsLvalueReference : BoolConstant<__is_lvalue_reference(T)> {};

template <typename T>
inline constexpr bool IsLvalueReferenceV = IsLvalueReference<T>::Value;

template <typename T>
struct IsRvalueReference : BoolConstant<__is_rvalue_reference(T)> {};

template <typename T>
inline constexpr bool IsRvalueReferenceV = IsRvalueReference<T>::Value;

template <typename T>
struct IsReference : BoolConstant<__is_reference(T)> {};

template <typename T>
inline constexpr bool IsReferenceV = IsReference<T>::Value;

template <typename T>
struct IsMemberPointer : BoolConstant<__is_member_pointer(T)> {};

template <typename T>
inline constexpr bool IsMemberPointerV = IsMemberPointer<T>::Value;

template <typename T>
struct IsMemberObjectPointer : BoolConstant<__is_member_object_pointer(T)> {};

template <typename T>
inline constexpr bool IsMemberObjectPointerV = IsMemberObjectPointer<T>::Value;

template <typename T>
struct IsMemberFunctionPointer : BoolConstant<__is_member_function_pointer(T)> {};

template <typename T>
inline constexpr bool IsMemberFunctionPointerV = IsMemberFunctionPointer<T>::Value;

template <typename T>
struct IsArithmetic : BoolConstant<__is_arithmetic(T)> {};

template <typename T>
inline constexpr bool IsArithmeticV = IsArithmetic<T>::Value;

template <typename T>
struct IsFundamental : BoolConstant<__is_fundamental(T)> {};

template <typename T>
inline constexpr bool IsFundamentalV = IsFundamental<T>::Value;

template <typename T>
struct IsScalar : BoolConstant<__is_scalar(T)> {};

template <typename T>
inline constexpr bool IsScalarV = IsScalar<T>::Value;

template <typename T>
struct IsObject : BoolConstant<__is_object(T)> {};

template <typename T>
inline constexpr bool IsObjectV = IsObject<T>::Value;

template <typename T>
struct IsCompound : BoolConstant<__is_compound(T)> {};

template <typename T>
inline constexpr bool IsCompoundV = IsCompound<T>::Value;

template <typename T>
struct IsConst : BoolConstant<__is_const(T)> {};

template <typename T>
inline constexpr bool IsConstV = IsConst<T>::Value;

template <typename T>
struct IsVolatile : BoolConstant<__is_volatile(T)> {};

template <typename T>
inline constexpr bool IsVolatileV = IsVolatile<T>::Value;

template <typename T>
struct IsSigned : BoolConstant<__is_signed(T)> {};

template <typename T>
inline constexpr bool IsSignedV = IsSigned<T>::Value;

template <typename T>
struct IsUnsigned : BoolConstant<__is_unsigned(T)> {};

template <typename T>
inline constexpr bool IsUnsignedV = IsUnsigned<T>::Value;

template <typename T>
struct IsTrivial : BoolConstant<__is_trivial(T)> {};

template <typename T>
inline constexpr bool IsTrivialV = IsTrivial<T>::Value;

template <typename T>
struct IsTriviallyCopyable : BoolConstant<__is_trivially_copyable(T)> {};

template <typename T>
inline constexpr bool IsTriviallyCopyableV = IsTriviallyCopyable<T>::Value;

template <typename T>
struct IsStandardLayout : BoolConstant<__is_standard_layout(T)> {};

template <typename T>
inline constexpr bool IsStandardLayoutV = IsStandardLayout<T>::Value;

template <typename T>
struct IsEmpty : BoolConstant<__is_empty(T)> {};

template <typename T>
inline constexpr bool IsEmptyV = IsEmpty<T>::Value;

template <typename T>
struct IsPolymorphic : BoolConstant<__is_polymorphic(T)> {};

template <typename T>
inline constexpr bool IsPolymorphicV = IsPolymorphic<T>::Value;

template <typename T>
struct IsAbstract : BoolConstant<__is_abstract(T)> {};

template <typename T>
inline constexpr bool IsAbstractV = IsAbstract<T>::Value;

template <typename T>
struct IsFinal : BoolConstant<__is_final(T)> {};

template <typename T>
inline constexpr bool IsFinalV = IsFinal<T>::Value;

template <typename T>
struct IsAggregate : BoolConstant<__is_aggregate(T)> {};

template <typename T>
inline constexpr bool IsAggregateV = IsAggregate<T>::Value;

template <typename T>
struct IsDestructible : BoolConstant<__is_destructible(T)> {};

template <typename T>
inline constexpr bool IsDestructibleV = IsDestructible<T>::Value;

template <typename T>
struct IsNothrowDestructible : BoolConstant<__is_nothrow_destructible(T)> {};

template <typename T>
inline constexpr bool IsNothrowDestructibleV = IsNothrowDestructible<T>::Value;

template <typename T>
struct IsTriviallyDestructible : BoolConstant<__is_trivially_destructible(T)> {};

template <typename T>
inline constexpr bool IsTriviallyDestructibleV = IsTriviallyDestructible<T>::Value;

template <typename T>
struct IsNullPointer : BoolConstant<__is_same(__remove_cv(T), decltype(nullptr))> {};

template <typename T>
inline constexpr bool IsNullPointerV = IsNullPointer<T>::Value;

template <typename T>
struct HasVirtualDestructor : BoolConstant<__has_virtual_destructor(T)> {};

template <typename T>
inline constexpr bool HasVirtualDestructorV = HasVirtualDestructor<T>::Value;

template <typename T>
struct HasUniqueObjectRepresentations : BoolConstant<__has_unique_object_representations(T)> {};

template <typename T>
inline constexpr bool HasUniqueObjectRepresentationsV = HasUniqueObjectRepresentations<T>::Value;

// 동일 타입·기반 관계·암시적 변환이다. Base/Derived와 From/To의 인자 순서를 지킨다.
template <typename A, typename B>
struct IsSame : BoolConstant<__is_same(A, B)> {};

template <typename A, typename B>
inline constexpr bool IsSameV = IsSame<A, B>::Value;

template <typename Base, typename Derived>
struct IsBaseOf : BoolConstant<__is_base_of(Base, Derived)> {};

template <typename Base, typename Derived>
inline constexpr bool IsBaseOfV = IsBaseOf<Base, Derived>::Value;

template <typename From, typename To>
struct IsConvertible : BoolConstant<__is_convertible_to(From, To)> {};

template <typename From, typename To>
inline constexpr bool IsConvertibleV = IsConvertible<From, To>::Value;

template <typename From, typename To>
struct IsNothrowConvertible : BoolConstant<__is_nothrow_convertible(From, To)> {};

template <typename From, typename To>
inline constexpr bool IsNothrowConvertibleV = IsNothrowConvertible<From, To>::Value;

template <typename To, typename From>
struct IsAssignable : BoolConstant<__is_assignable(To, From)> {};

template <typename To, typename From>
inline constexpr bool IsAssignableV = IsAssignable<To, From>::Value;

template <typename To, typename From>
struct IsNothrowAssignable : BoolConstant<__is_nothrow_assignable(To, From)> {};

template <typename To, typename From>
inline constexpr bool IsNothrowAssignableV = IsNothrowAssignable<To, From>::Value;

template <typename To, typename From>
struct IsTriviallyAssignable : BoolConstant<__is_trivially_assignable(To, From)> {};

template <typename To, typename From>
inline constexpr bool IsTriviallyAssignableV = IsTriviallyAssignable<To, From>::Value;

template <typename T, typename... Args>
struct IsConstructible : BoolConstant<__is_constructible(T, Args...)> {};

template <typename T, typename... Args>
inline constexpr bool IsConstructibleV = IsConstructible<T, Args...>::Value;

template <typename T, typename... Args>
struct IsNothrowConstructible : BoolConstant<__is_nothrow_constructible(T, Args...)> {};

template <typename T, typename... Args>
inline constexpr bool IsNothrowConstructibleV = IsNothrowConstructible<T, Args...>::Value;

template <typename T, typename... Args>
struct IsTriviallyConstructible : BoolConstant<__is_trivially_constructible(T, Args...)> {};

template <typename T, typename... Args>
inline constexpr bool IsTriviallyConstructibleV = IsTriviallyConstructible<T, Args...>::Value;

// 타입 변환. 참조 불가능한 void·cv/ref 한정 함수는 AddReference에서 원래 타입으로 남는다.
// 포인터·참조 제거는 주소 공간·unaligned 등 Clang 확장 한정도 처리해야 하므로 intrinsic을 쓴다.
template <typename T>
struct RemoveConst {
    using Type = __remove_const(T);
};

template <typename T>
using RemoveConstT = typename RemoveConst<T>::Type;

template <typename T>
struct RemoveVolatile {
    using Type = __remove_volatile(T);
};

template <typename T>
using RemoveVolatileT = typename RemoveVolatile<T>::Type;

template <typename T>
struct RemoveCv {
    using Type = __remove_cv(T);
};

template <typename T>
using RemoveCvT = typename RemoveCv<T>::Type;

template <typename T>
struct RemoveReference {
    using Type = __remove_reference_t(T);
};

template <typename T>
using RemoveReferenceT = typename RemoveReference<T>::Type;

template <typename T>
struct RemoveCvRef {
    using Type = __remove_cvref(T);
};

template <typename T>
using RemoveCvRefT = typename RemoveCvRef<T>::Type;

template <typename T>
struct AddConst {
    using Type = const T;
};

template <typename T>
using AddConstT = typename AddConst<T>::Type;

template <typename T>
struct AddVolatile {
    using Type = volatile T;
};

template <typename T>
using AddVolatileT = typename AddVolatile<T>::Type;

template <typename T>
struct AddCv {
    using Type = const volatile T;
};

template <typename T>
using AddCvT = typename AddCv<T>::Type;

template <typename T>
struct AddLvalueReference {
    using Type = __add_lvalue_reference(T);
};

template <typename T>
using AddLvalueReferenceT = typename AddLvalueReference<T>::Type;

template <typename T>
struct AddRvalueReference {
    using Type = __add_rvalue_reference(T);
};

template <typename T>
using AddRvalueReferenceT = typename AddRvalueReference<T>::Type;

template <typename T>
struct AddPointer {
    using Type = __add_pointer(T);
};

template <typename T>
using AddPointerT = typename AddPointer<T>::Type;

template <typename T>
struct RemovePointer {
    using Type = __remove_pointer(T);
};

template <typename T>
using RemovePointerT = typename RemovePointer<T>::Type;

template <typename T>
struct RemoveExtent {
    using Type = __remove_extent(T);
};

template <typename T>
using RemoveExtentT = typename RemoveExtent<T>::Type;

template <typename T>
struct RemoveAllExtents {
    using Type = __remove_all_extents(T);
};

template <typename T>
using RemoveAllExtentsT = typename RemoveAllExtents<T>::Type;

template <typename T>
struct Decay {
    using Type = __decay(T);
};

template <typename T>
using DecayT = typename Decay<T>::Type;

template <typename T>
struct MakeSigned {
    using Type = __make_signed(T);
};

template <typename T>
using MakeSignedT = typename MakeSigned<T>::Type;

template <typename T>
struct MakeUnsigned {
    using Type = __make_unsigned(T);
};

template <typename T>
using MakeUnsignedT = typename MakeUnsigned<T>::Type;

template <typename T>
struct UnderlyingType {};

template <typename T>
    requires IsEnumV<T>
struct UnderlyingType<T> {
    using Type = __underlying_type(T);
};

template <typename T>
using UnderlyingTypeT = typename UnderlyingType<T>::Type;

template <typename T>
struct RemoveRestrict {
    using Type = __remove_restrict(T);
};

template <typename T>
using RemoveRestrictT = typename RemoveRestrict<T>::Type;

// Clang _Atomic(T) 확장 전용이다. atomic이 아닌 타입에는 Type이 없어 탐지 문맥에서 실패한다.
// _Atomic(T) 부분 특수화는 Clang이 추론하지 못하므로 평가하지 않는 load 식의 결과 타입을 사용한다.
template <typename T, typename = void>
struct RemoveAtomic {};

template <typename T>
struct RemoveAtomic<T, VoidT<decltype(__c11_atomic_load(static_cast<T*>(nullptr), 0))>> {
    using Type = decltype(__c11_atomic_load(static_cast<T*>(nullptr), 0));
};

template <typename T>
using RemoveAtomicT = typename RemoveAtomic<T>::Type;

// void도 합법적인 검사 입력이다. T&·T&&를 직접 조합하지 않고 안전한 참조 변환을 거친다.
template <typename T>
struct IsDefaultConstructible : IsConstructible<T> {};

template <typename T>
inline constexpr bool IsDefaultConstructibleV = IsDefaultConstructible<T>::Value;

template <typename T>
struct IsCopyConstructible : IsConstructible<T, AddLvalueReferenceT<const T>> {};

template <typename T>
inline constexpr bool IsCopyConstructibleV = IsCopyConstructible<T>::Value;

template <typename T>
struct IsMoveConstructible : IsConstructible<T, AddRvalueReferenceT<T>> {};

template <typename T>
inline constexpr bool IsMoveConstructibleV = IsMoveConstructible<T>::Value;

template <typename T>
struct IsCopyAssignable : IsAssignable<AddLvalueReferenceT<T>, AddLvalueReferenceT<const T>> {};

template <typename T>
inline constexpr bool IsCopyAssignableV = IsCopyAssignable<T>::Value;

template <typename T>
struct IsMoveAssignable : IsAssignable<AddLvalueReferenceT<T>, AddRvalueReferenceT<T>> {};

template <typename T>
inline constexpr bool IsMoveAssignableV = IsMoveAssignable<T>::Value;

template <typename T>
struct IsNothrowDefaultConstructible : IsNothrowConstructible<T> {};

template <typename T>
inline constexpr bool IsNothrowDefaultConstructibleV = IsNothrowDefaultConstructible<T>::Value;

template <typename T>
struct IsNothrowCopyConstructible : IsNothrowConstructible<T, AddLvalueReferenceT<const T>> {};

template <typename T>
inline constexpr bool IsNothrowCopyConstructibleV = IsNothrowCopyConstructible<T>::Value;

template <typename T>
struct IsNothrowMoveConstructible : IsNothrowConstructible<T, AddRvalueReferenceT<T>> {};

template <typename T>
inline constexpr bool IsNothrowMoveConstructibleV = IsNothrowMoveConstructible<T>::Value;

template <typename T>
struct IsNothrowCopyAssignable
    : IsNothrowAssignable<AddLvalueReferenceT<T>, AddLvalueReferenceT<const T>> {};

template <typename T>
inline constexpr bool IsNothrowCopyAssignableV = IsNothrowCopyAssignable<T>::Value;

template <typename T>
struct IsNothrowMoveAssignable
    : IsNothrowAssignable<AddLvalueReferenceT<T>, AddRvalueReferenceT<T>> {};

template <typename T>
inline constexpr bool IsNothrowMoveAssignableV = IsNothrowMoveAssignable<T>::Value;

template <typename T>
struct IsTriviallyDefaultConstructible : IsTriviallyConstructible<T> {};

template <typename T>
inline constexpr bool IsTriviallyDefaultConstructibleV = IsTriviallyDefaultConstructible<T>::Value;

template <typename T>
struct IsTriviallyCopyConstructible : IsTriviallyConstructible<T, AddLvalueReferenceT<const T>> {};

template <typename T>
inline constexpr bool IsTriviallyCopyConstructibleV = IsTriviallyCopyConstructible<T>::Value;

template <typename T>
struct IsTriviallyMoveConstructible : IsTriviallyConstructible<T, AddRvalueReferenceT<T>> {};

template <typename T>
inline constexpr bool IsTriviallyMoveConstructibleV = IsTriviallyMoveConstructible<T>::Value;

template <typename T>
struct IsTriviallyCopyAssignable
    : IsTriviallyAssignable<AddLvalueReferenceT<T>, AddLvalueReferenceT<const T>> {};

template <typename T>
inline constexpr bool IsTriviallyCopyAssignableV = IsTriviallyCopyAssignable<T>::Value;

template <typename T>
struct IsTriviallyMoveAssignable
    : IsTriviallyAssignable<AddLvalueReferenceT<T>, AddRvalueReferenceT<T>> {};

template <typename T>
inline constexpr bool IsTriviallyMoveAssignableV = IsTriviallyMoveAssignable<T>::Value;

// 배열 차원과 길이. 배열이 아니거나 해당 차원이 없으면 0이다.
template <typename T>
struct Rank : IntegralConstant<decltype(sizeof(0)), __array_rank(T)> {};

template <typename T>
inline constexpr decltype(sizeof(0)) RankV = Rank<T>::Value;

template <typename T, unsigned Dimension = 0>
struct Extent : IntegralConstant<decltype(sizeof(0)), __array_extent(T, Dimension)> {};

template <typename T, unsigned Dimension = 0>
inline constexpr decltype(sizeof(0)) ExtentV = Extent<T, Dimension>::Value;

template <typename T, T... Values>
struct IntegerSequence {
    using ValueType = T;
    static constexpr decltype(sizeof(0)) Size = sizeof...(Values);
};

template <decltype(sizeof(0))... Indices>
using IndexSequence = IntegerSequence<decltype(sizeof(0)), Indices...>;

template <typename T, T Count>
using MakeIntegerSequence = __make_integer_seq<IntegerSequence, T, Count>;

template <decltype(sizeof(0)) Count>
using MakeIndexSequence = MakeIntegerSequence<decltype(sizeof(0)), Count>;

template <typename... Types>
using IndexSequenceFor = MakeIndexSequence<sizeof...(Types)>;

template <decltype(sizeof(0)) Index, typename... Types>
using TypePackElement = __type_pack_element<Index, Types...>;

// 평가되지 않는 문맥(decltype 등)에서만 쓴다. 정의가 없으므로 실제 호출은 링크 오류다.
template <typename T>
AddRvalueReferenceT<T> DeclVal() noexcept;

// 호출 형질은 함수 객체·함수 포인터·멤버 포인터를 지원한다. 멤버 포인터의 대상은 객체 또는
// 역참조 가능한 포인터류다. STL reference_wrapper는 받지 않으므로 호출자가 .get()을 넘긴다.
// 보조 선언은 decltype/noexcept에만 사용하며 호출하거나 정의하지 않는다.
// 보조 함수 이름을 한정해 사용자 타입의 ADL 함수가 판정에 끼어들지 못하게 한다.
namespace type_traits_detail {

template <typename Class, typename Object>
    requires(IsSameV<Class, RemoveCvRefT<Object>> || IsBaseOfV<Class, RemoveCvRefT<Object>>)
Object&& InvokeObject(Object&& object) noexcept;

template <typename Class, typename Object>
    requires(!IsSameV<Class, RemoveCvRefT<Object>> && !IsBaseOfV<Class, RemoveCvRefT<Object>>)
auto InvokeObject(Object&& object) noexcept(noexcept(*static_cast<Object&&>(object)))
    -> decltype(*static_cast<Object&&>(object));

template <typename Function, typename... Args>
auto InvokeExpression(Function&& function, Args&&... args) noexcept(
    noexcept(static_cast<Function&&>(function)(static_cast<Args&&>(args)...)))
    -> decltype(static_cast<Function&&>(function)(static_cast<Args&&>(args)...));

template <typename Member, typename Class, typename Object, typename... Args>
    requires IsFunctionV<Member>
auto InvokeExpression(Member Class::* member, Object&& object, Args&&... args) noexcept(
    noexcept((type_traits_detail::InvokeObject<Class>(static_cast<Object&&>(object)).*
              member)(static_cast<Args&&>(args)...)))
    -> decltype((type_traits_detail::InvokeObject<Class>(static_cast<Object&&>(object)).*
                 member)(static_cast<Args&&>(args)...));

template <typename Member, typename Class, typename Object>
    requires(!IsFunctionV<Member>)
auto InvokeExpression(Member Class::* member, Object&& object) noexcept(
    noexcept(type_traits_detail::InvokeObject<Class>(static_cast<Object&&>(object)).*member))
    -> decltype(type_traits_detail::InvokeObject<Class>(static_cast<Object&&>(object)).*member);

template <typename, typename Function, typename... Args>
struct InvokeResultImpl {};

template <typename Function, typename... Args>
struct InvokeResultImpl<
    VoidT<decltype(type_traits_detail::InvokeExpression(DeclVal<Function>(), DeclVal<Args>()...))>,
    Function, Args...> {
    using Type =
        decltype(type_traits_detail::InvokeExpression(DeclVal<Function>(), DeclVal<Args>()...));
    static constexpr bool Nothrow =
        noexcept(type_traits_detail::InvokeExpression(DeclVal<Function>(), DeclVal<Args>()...));
};

} // namespace type_traits_detail

template <typename Function, typename... Args>
struct InvokeResult : type_traits_detail::InvokeResultImpl<void, Function, Args...> {};

template <typename Function, typename... Args>
using InvokeResultT = typename InvokeResult<Function, Args...>::Type;

template <typename Function, typename... Args>
    struct IsInvocable : BoolConstant < requires {
    typename InvokeResult<Function, Args...>::Type;
}>{};

template <typename Function, typename... Args>
inline constexpr bool IsInvocableV = IsInvocable<Function, Args...>::Value;

namespace type_traits_detail {

template <typename, typename Function, typename... Args>
struct NothrowInvocable : FalseType {};

template <typename Function, typename... Args>
struct NothrowInvocable<VoidT<InvokeResultT<Function, Args...>>, Function, Args...>
    : BoolConstant<InvokeResult<Function, Args...>::Nothrow> {};

template <typename Result, typename, typename Function, typename... Args>
struct InvocableReturn : FalseType {
    static constexpr bool Nothrow = false;
};

template <typename Result, typename Function, typename... Args>
struct InvocableReturn<Result, VoidT<InvokeResultT<Function, Args...>>, Function, Args...>
    : BoolConstant<IsVoidV<Result> || IsConvertibleV<InvokeResultT<Function, Args...>, Result>> {
    static constexpr bool Nothrow =
        InvokeResult<Function, Args...>::Nothrow &&
        (IsVoidV<Result> || IsNothrowConvertibleV<InvokeResultT<Function, Args...>, Result>);
};

} // namespace type_traits_detail

template <typename Function, typename... Args>
struct IsNothrowInvocable : type_traits_detail::NothrowInvocable<void, Function, Args...> {};

template <typename Function, typename... Args>
inline constexpr bool IsNothrowInvocableV = IsNothrowInvocable<Function, Args...>::Value;

template <typename Result, typename Function, typename... Args>
struct IsInvocableR : type_traits_detail::InvocableReturn<Result, void, Function, Args...> {};

template <typename Result, typename Function, typename... Args>
inline constexpr bool IsInvocableRV = IsInvocableR<Result, Function, Args...>::Value;

template <typename Result, typename Function, typename... Args>
struct IsNothrowInvocableR
    : BoolConstant<type_traits_detail::InvocableReturn<Result, void, Function, Args...>::Nothrow> {
};

template <typename Result, typename Function, typename... Args>
inline constexpr bool IsNothrowInvocableRV = IsNothrowInvocableR<Result, Function, Args...>::Value;

} // namespace rocks
