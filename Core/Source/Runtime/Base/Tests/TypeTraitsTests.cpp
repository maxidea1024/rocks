// 공개 traits는 다른 Base 헤더보다 먼저 포함해도 독립적으로 사용할 수 있어야 한다.
#include <Base/TypeTraits.h>
#include <Base/Types.h>
#include <TestSupport/Test.h>

// 독립 기준은 표준 라이브러리다. 엔진 런타임의 구현과 달리 테스트는 STL을 사용할 수 있다.
#include <type_traits>

// 호출 형질의 구현 보조 이름과 같은 ADL 함수가 있어도 호출 불가능한 타입은 그대로다.
namespace rocks::test::traits_adl_collision {
struct NonCallable {};
int InvokeExpression(NonCallable) noexcept;
} // namespace rocks::test::traits_adl_collision

static_assert(!rocks::IsInvocableV<rocks::test::traits_adl_collision::NonCallable>);
static_assert(!rocks::IsNothrowInvocableV<rocks::test::traits_adl_collision::NonCallable>);

namespace rocks::test {
namespace {

struct TraitBase {};
struct TraitDerived : TraitBase {};
struct TraitPrivateDerived : private TraitBase {};
struct TraitLeft : TraitBase {};
struct TraitRight : TraitBase {};
struct TraitAmbiguousDerived : TraitLeft, TraitRight {};
struct TraitIncomplete;
struct TraitFinal final {};
struct TraitAggregate {
    int value;
};
union TraitUnion {
    int integer;
    float real;
};
enum TraitEnum : unsigned short {
    TraitEnumValue
};
enum class TraitScopedEnum : unsigned char {
    Value
};
struct TraitAbstract {
    virtual void Operation() = 0;
    virtual ~TraitAbstract() = default;
};
struct TraitPolymorphic {
    virtual ~TraitPolymorphic() = default;
};
struct TraitThrowing {
    TraitThrowing() noexcept(false);
    TraitThrowing(const TraitThrowing&) noexcept(false);
    TraitThrowing(TraitThrowing&&) noexcept(false);
    TraitThrowing& operator=(const TraitThrowing&) noexcept(false);
    TraitThrowing& operator=(TraitThrowing&&) noexcept(false);
    ~TraitThrowing() noexcept(false);
};
struct TraitMoveOnly {
    TraitMoveOnly() = default;
    TraitMoveOnly(const TraitMoveOnly&) = delete;
    TraitMoveOnly(TraitMoveOnly&&) noexcept = default;
    TraitMoveOnly& operator=(const TraitMoveOnly&) = delete;
    TraitMoveOnly& operator=(TraitMoveOnly&&) noexcept = default;
};
struct TraitPrivateDestructor {
private:
    ~TraitPrivateDestructor() = default;
};

using TraitFunction = int(double);
using TraitNoexceptFunction = int(double) noexcept;
using TraitQualifiedFunction = int(double) const& noexcept;

template <typename T>
consteval bool ClassificationMatchesStandard() {
    return IsVoidV<T> == std::is_void_v<T> && IsNullPointerV<T> == std::is_null_pointer_v<T> &&
           IsIntegralV<T> == std::is_integral_v<T> &&
           IsFloatingPointV<T> == std::is_floating_point_v<T> &&
           IsArrayV<T> == std::is_array_v<T> && IsEnumV<T> == std::is_enum_v<T> &&
           IsUnionV<T> == std::is_union_v<T> && IsClassV<T> == std::is_class_v<T> &&
           IsFunctionV<T> == std::is_function_v<T> && IsPointerV<T> == std::is_pointer_v<T> &&
           IsLvalueReferenceV<T> == std::is_lvalue_reference_v<T> &&
           IsRvalueReferenceV<T> == std::is_rvalue_reference_v<T> &&
           IsReferenceV<T> == std::is_reference_v<T> &&
           IsMemberPointerV<T> == std::is_member_pointer_v<T> &&
           IsMemberObjectPointerV<T> == std::is_member_object_pointer_v<T> &&
           IsMemberFunctionPointerV<T> == std::is_member_function_pointer_v<T> &&
           IsArithmeticV<T> == std::is_arithmetic_v<T> &&
           IsFundamentalV<T> == std::is_fundamental_v<T> && IsScalarV<T> == std::is_scalar_v<T> &&
           IsObjectV<T> == std::is_object_v<T> && IsCompoundV<T> == std::is_compound_v<T> &&
           IsConstV<T> == std::is_const_v<T> && IsVolatileV<T> == std::is_volatile_v<T> &&
           IsSignedV<T> == std::is_signed_v<T> && IsUnsignedV<T> == std::is_unsigned_v<T> &&
           IsBoundedArrayV<T> == std::is_bounded_array_v<T> &&
           IsUnboundedArrayV<T> == std::is_unbounded_array_v<T>;
}

template <typename... Types>
consteval bool ClassificationsMatchStandard() {
    return (ClassificationMatchesStandard<Types>() && ...);
}

static_assert(ClassificationsMatchStandard<
              void, const void, volatile void, decltype(nullptr), const decltype(nullptr), bool,
              char, signed char, unsigned char, wchar_t, char8_t, char16_t, char32_t, short,
              unsigned short, int, unsigned int, long, unsigned long, long long, unsigned long long,
              float, double, long double, const volatile int, int&, const int&, int&&, int*,
              const int*, int* const, int* volatile, int[3], const int[3], int[][4], int (&)[3],
              TraitFunction, TraitNoexceptFunction, TraitQualifiedFunction, TraitFunction*,
              TraitFunction&, TraitBase, TraitIncomplete, TraitUnion, TraitEnum, TraitScopedEnum,
              int TraitBase::*, TraitQualifiedFunction TraitBase::*>());

template <typename T>
consteval bool TransformationsMatchStandard() {
    return IsSameV<RemoveConstT<T>, std::remove_const_t<T>> &&
           IsSameV<RemoveVolatileT<T>, std::remove_volatile_t<T>> &&
           IsSameV<RemoveCvT<T>, std::remove_cv_t<T>> &&
           IsSameV<AddConstT<T>, std::add_const_t<T>> &&
           IsSameV<AddVolatileT<T>, std::add_volatile_t<T>> &&
           IsSameV<AddCvT<T>, std::add_cv_t<T>> &&
           IsSameV<RemoveReferenceT<T>, std::remove_reference_t<T>> &&
           IsSameV<RemoveCvRefT<T>, std::remove_cvref_t<T>> &&
           IsSameV<AddLvalueReferenceT<T>, std::add_lvalue_reference_t<T>> &&
           IsSameV<AddRvalueReferenceT<T>, std::add_rvalue_reference_t<T>> &&
           IsSameV<RemovePointerT<T>, std::remove_pointer_t<T>> &&
           IsSameV<AddPointerT<T>, std::add_pointer_t<T>> &&
           IsSameV<RemoveExtentT<T>, std::remove_extent_t<T>> &&
           IsSameV<RemoveAllExtentsT<T>, std::remove_all_extents_t<T>> &&
           IsSameV<DecayT<T>, std::decay_t<T>>;
}

template <typename... Types>
consteval bool TransformationsMatchStandardFor() {
    return (TransformationsMatchStandard<Types>() && ...);
}

static_assert(TransformationsMatchStandardFor<
              void, const void, const volatile void, int, const volatile int, int&, const int&,
              int&&, int*, const int*, int* const volatile, int[3], int[][4], const int (&)[3],
              TraitFunction, TraitNoexceptFunction, TraitQualifiedFunction, TraitFunction*,
              TraitFunction&, TraitIncomplete>());
static_assert(IsSameV<RemoveCvRefT<const int (&)[3]>, int[3]>);
static_assert(IsSameV<DecayT<const int (&)[3]>, const int*>);
static_assert(IsSameV<decltype(DeclVal<void>()), void>);
static_assert(IsSameV<decltype(DeclVal<const void>()), void>);
static_assert(IsSameV<decltype(DeclVal<int&>()), int&>);
static_assert(IsSameV<decltype(DeclVal<int>()), int&&>);
static_assert(IsSameV<typename RemoveReference<int&&>::Type, int>);
static_assert(IsSameV<typename TypeIdentity<const int&>::Type, const int&>);
static_assert(IsSameV<TypeIdentityT<TraitIncomplete>, TraitIncomplete>);
static_assert(IsSameV<VoidT<void, TraitIncomplete, TraitQualifiedFunction>, void>);

template <typename T>
consteval bool IntegerTransformsMatchStandard() {
    return IsSameV<MakeSignedT<T>, std::make_signed_t<T>> &&
           IsSameV<MakeUnsignedT<T>, std::make_unsigned_t<T>>;
}

static_assert(IntegerTransformsMatchStandard<char>());
static_assert(IntegerTransformsMatchStandard<wchar_t>());
static_assert(IntegerTransformsMatchStandard<char8_t>());
static_assert(IntegerTransformsMatchStandard<char16_t>());
static_assert(IntegerTransformsMatchStandard<char32_t>());
static_assert(IntegerTransformsMatchStandard<const volatile unsigned long>());
static_assert(IntegerTransformsMatchStandard<TraitEnum>());
static_assert(IntegerTransformsMatchStandard<const TraitScopedEnum>());
static_assert(IsSameV<UnderlyingTypeT<TraitEnum>, unsigned short>);
static_assert(IsSameV<UnderlyingTypeT<const TraitScopedEnum>, unsigned char>);
static_assert(!IsScopedEnumV<TraitEnum> && IsScopedEnumV<TraitScopedEnum>);
static_assert(!IsScopedEnumV<int> && !IsScopedEnumV<TraitIncomplete>);
static_assert(RankV<int> == 0 && RankV<const int[][3][4]> == 3);
static_assert(ExtentV<int[][3][4]> == 0 && ExtentV<int[][3][4], 1> == 3 &&
              ExtentV<int[][3][4], 2> == 4 && ExtentV<int[][3][4], 3> == 0);
static_assert(RankV<int (&)[3]> == 0 && ExtentV<int> == 0);

template <typename T>
consteval bool LifetimeMatchesStandard() {
    return IsDefaultConstructibleV<T> == std::is_default_constructible_v<T> &&
           IsCopyConstructibleV<T> == std::is_copy_constructible_v<T> &&
           IsMoveConstructibleV<T> == std::is_move_constructible_v<T> &&
           IsCopyAssignableV<T> == std::is_copy_assignable_v<T> &&
           IsMoveAssignableV<T> == std::is_move_assignable_v<T> &&
           IsDestructibleV<T> == std::is_destructible_v<T> &&
           IsNothrowDefaultConstructibleV<T> == std::is_nothrow_default_constructible_v<T> &&
           IsNothrowCopyConstructibleV<T> == std::is_nothrow_copy_constructible_v<T> &&
           IsNothrowMoveConstructibleV<T> == std::is_nothrow_move_constructible_v<T> &&
           IsNothrowCopyAssignableV<T> == std::is_nothrow_copy_assignable_v<T> &&
           IsNothrowMoveAssignableV<T> == std::is_nothrow_move_assignable_v<T> &&
           IsNothrowDestructibleV<T> == std::is_nothrow_destructible_v<T> &&
           IsTriviallyDefaultConstructibleV<T> == std::is_trivially_default_constructible_v<T> &&
           IsTriviallyCopyConstructibleV<T> == std::is_trivially_copy_constructible_v<T> &&
           IsTriviallyMoveConstructibleV<T> == std::is_trivially_move_constructible_v<T> &&
           IsTriviallyCopyAssignableV<T> == std::is_trivially_copy_assignable_v<T> &&
           IsTriviallyMoveAssignableV<T> == std::is_trivially_move_assignable_v<T> &&
           IsTriviallyDestructibleV<T> == std::is_trivially_destructible_v<T>;
}

template <typename... Types>
consteval bool LifetimesMatchStandard() {
    return (LifetimeMatchesStandard<Types>() && ...);
}

static_assert(
    LifetimesMatchStandard<void, const void, int, const int, int&, int&&, int[3], int[],
                           TraitFunction, TraitQualifiedFunction, TraitAggregate, TraitMoveOnly,
                           TraitThrowing, TraitAbstract, TraitPrivateDestructor>());
static_assert(IsConstructibleV<int, double> == std::is_constructible_v<int, double>);
static_assert(IsAssignableV<int&, double> == std::is_assignable_v<int&, double>);
static_assert(!IsConstructibleV<TraitMoveOnly, const TraitMoveOnly&>);
static_assert(IsNothrowConstructibleV<TraitMoveOnly, TraitMoveOnly&&>);
static_assert(!IsNothrowAssignableV<TraitThrowing&, TraitThrowing&&>);

template <typename T>
consteval bool LayoutMatchesStandard() {
    return IsTrivialV<T> == std::is_trivial_v<T> &&
           IsTriviallyCopyableV<T> == std::is_trivially_copyable_v<T> &&
           IsStandardLayoutV<T> == std::is_standard_layout_v<T> &&
           IsEmptyV<T> == std::is_empty_v<T> && IsPolymorphicV<T> == std::is_polymorphic_v<T> &&
           IsAbstractV<T> == std::is_abstract_v<T> && IsFinalV<T> == std::is_final_v<T> &&
           IsAggregateV<T> == std::is_aggregate_v<T> &&
           HasVirtualDestructorV<T> == std::has_virtual_destructor_v<T> &&
           HasUniqueObjectRepresentationsV<T> == std::has_unique_object_representations_v<T>;
}

static_assert(LayoutMatchesStandard<int>() && LayoutMatchesStandard<TraitBase>() &&
              LayoutMatchesStandard<TraitAggregate>() && LayoutMatchesStandard<TraitUnion>() &&
              LayoutMatchesStandard<TraitFinal>() && LayoutMatchesStandard<TraitMoveOnly>() &&
              LayoutMatchesStandard<TraitAbstract>() && LayoutMatchesStandard<TraitPolymorphic>());
static_assert(IsBaseOfV<TraitBase, TraitBase> && IsBaseOfV<TraitBase, TraitDerived>);
static_assert(IsBaseOfV<TraitBase, TraitPrivateDerived> &&
              IsBaseOfV<TraitBase, TraitAmbiguousDerived>);
static_assert(!IsConvertibleV<TraitPrivateDerived*, TraitBase*> &&
              !IsConvertibleV<TraitAmbiguousDerived*, TraitBase*>);
static_assert(!IsBaseOfV<int, int> && !IsBaseOfV<TraitDerived, TraitBase>);
static_assert(IsConvertibleV<void, void> && !IsConvertibleV<int, void> &&
              !IsConvertibleV<void, int>);

using TraitSeven = IntegralConstant<int, 7>;
static_assert(TraitSeven::Value == 7 && TraitSeven{}() == 7 && static_cast<int>(TraitSeven{}) == 7);
static_assert(IsSameV<TraitSeven::ValueType, int> && IsSameV<TraitSeven::Type, TraitSeven>);
static_assert(TrueType::Value && !FalseType::Value && BoolConstant<true>::Value);
static_assert(IsSameV<ConditionalT<true, int, void>, int> &&
              IsSameV<ConditionalT<false, int, void>, void>);
static_assert(IsSameV<EnableIfT<true>, void> && IsSameV<EnableIfT<true, int>, int>);

template <typename T>
concept HasTraitType = requires { typename T::Type; };
static_assert(!HasTraitType<EnableIf<false>>);
static_assert(!HasTraitType<UnderlyingType<void>> && !HasTraitType<UnderlyingType<int>> &&
              !HasTraitType<UnderlyingType<TraitIncomplete>>);
static_assert(!HasTraitType<RemoveAtomic<int>>);

// 아직 정의하지 않은 피연산자에 닿으면 오류다. 논리 조합은 단락 평가를 보장해야 한다.
struct TraitUninstantiable;
struct TraitTrueWithMarker : IntegralConstant<int, 7> {
    using Marker = int;
};
static_assert(Conjunction<>::Value && !Disjunction<>::Value);
static_assert(ConjunctionV<TrueType, TrueType> && !ConjunctionV<TrueType, FalseType>);
static_assert(DisjunctionV<FalseType, TrueType> && !DisjunctionV<FalseType, FalseType>);
static_assert(!Conjunction<FalseType, TraitUninstantiable>::Value);
static_assert(Disjunction<TrueType, TraitUninstantiable>::Value);
static_assert(Conjunction<TrueType, TraitTrueWithMarker>::Value == 7);
static_assert(Disjunction<FalseType, TraitTrueWithMarker>::Value == 7);
static_assert(IsSameV<typename Conjunction<TrueType, TraitTrueWithMarker>::Marker, int>);
static_assert(Negation<FalseType>::Value && !Negation<TraitSeven>::Value);
static_assert(IsSameV<MakeIndexSequence<0>, IndexSequence<>>);
static_assert(IsSameV<MakeIndexSequence<4>, IndexSequence<0, 1, 2, 3>>);
static_assert(IsSameV<MakeIntegerSequence<int, 3>, IntegerSequence<int, 0, 1, 2>>);
static_assert(IsSameV<IndexSequenceFor<void, int, TraitIncomplete>, IndexSequence<0, 1, 2>>);
static_assert(IntegerSequence<int, 1, 4, 9>::Size == 3);
static_assert(IsSameV<TypePackElement<1, void, const int&, TraitIncomplete>, const int&>);

struct TraitCallable {
    int operator()(double) const noexcept;
};
struct TraitLvalueCallable {
    int operator()() & noexcept;
};
struct TraitRvalueCallable {
    int operator()() && noexcept;
};
struct TraitDeletedCallable {
    int operator()() = delete;
};
struct TraitOverloadedCallable {
    int operator()(int) const noexcept;
    void operator()(void*) const noexcept(false);
};
struct TraitThrowingConversion {
    operator int() const noexcept(false);
};
struct TraitExplicitConversion {
    explicit operator int() const noexcept;
};
struct TraitReturnsThrowingConversion {
    TraitThrowingConversion operator()() const noexcept;
};
struct TraitReturnsExplicitConversion {
    TraitExplicitConversion operator()() const noexcept;
};
struct TraitReceiver {
    int value;
    int Read(double) const& noexcept;
    int Consume() && noexcept;
    void Throwing() noexcept(false);
};
struct TraitReceiverDerived : TraitReceiver {};
struct TraitReceiverPointer {
    TraitReceiver& operator*() const noexcept;
};
struct TraitThrowingReceiverPointer {
    TraitReceiver& operator*() const noexcept(false);
};

template <typename F, typename... Args>
consteval bool InvocationMatchesStandard() {
    if constexpr (IsInvocableV<F, Args...> != std::is_invocable_v<F, Args...> ||
                  IsNothrowInvocableV<F, Args...> != std::is_nothrow_invocable_v<F, Args...> ||
                  IsInvocableRV<void, F, Args...> != std::is_invocable_r_v<void, F, Args...> ||
                  IsInvocableRV<int, F, Args...> != std::is_invocable_r_v<int, F, Args...> ||
                  IsNothrowInvocableRV<void, F, Args...> !=
                      std::is_nothrow_invocable_r_v<void, F, Args...> ||
                  IsNothrowInvocableRV<int, F, Args...> !=
                      std::is_nothrow_invocable_r_v<int, F, Args...>) {
        return false;
    } else if constexpr (std::is_invocable_v<F, Args...>) {
        return IsSameV<InvokeResultT<F, Args...>, std::invoke_result_t<F, Args...>>;
    } else {
        return !HasTraitType<InvokeResult<F, Args...>>;
    }
}

static_assert(InvocationMatchesStandard<void>());
static_assert(InvocationMatchesStandard<int, int>());
static_assert(InvocationMatchesStandard<TraitCallable, float>());
static_assert(InvocationMatchesStandard<const TraitCallable&, int>());
static_assert(InvocationMatchesStandard<TraitCallable>());
static_assert(InvocationMatchesStandard<TraitLvalueCallable&>());
static_assert(InvocationMatchesStandard<TraitLvalueCallable>());
static_assert(InvocationMatchesStandard<TraitRvalueCallable>());
static_assert(InvocationMatchesStandard<TraitRvalueCallable&>());
static_assert(InvocationMatchesStandard<TraitDeletedCallable>());
static_assert(InvocationMatchesStandard<TraitOverloadedCallable, int>());
static_assert(InvocationMatchesStandard<TraitOverloadedCallable, void*>());
static_assert(InvocationMatchesStandard<TraitOverloadedCallable, TraitBase>());
static_assert(InvocationMatchesStandard<TraitReturnsThrowingConversion>());
static_assert(InvocationMatchesStandard<TraitReturnsExplicitConversion>());
static_assert(InvocationMatchesStandard<TraitFunction*, int>());
static_assert(InvocationMatchesStandard<TraitNoexceptFunction&, int>());
static_assert(InvocationMatchesStandard<decltype(&TraitReceiver::Read), TraitReceiver&, int>());
static_assert(
    InvocationMatchesStandard<decltype(&TraitReceiver::Read), const TraitReceiver*, int>());
static_assert(
    InvocationMatchesStandard<decltype(&TraitReceiver::Read), TraitReceiverDerived&, int>());
static_assert(
    InvocationMatchesStandard<decltype(&TraitReceiver::Read), TraitReceiverPointer, int>());
static_assert(
    InvocationMatchesStandard<decltype(&TraitReceiver::Read), TraitThrowingReceiverPointer, int>());
static_assert(InvocationMatchesStandard<decltype(&TraitReceiver::Read), TraitBase&, int>());
static_assert(InvocationMatchesStandard<decltype(&TraitReceiver::Consume), TraitReceiver>());
static_assert(InvocationMatchesStandard<decltype(&TraitReceiver::Consume), TraitReceiver&>());
static_assert(InvocationMatchesStandard<decltype(&TraitReceiver::Throwing), TraitReceiver&>());
static_assert(InvocationMatchesStandard<decltype(&TraitReceiver::value), TraitReceiver&>());
static_assert(InvocationMatchesStandard<decltype(&TraitReceiver::value), const TraitReceiver&>());
static_assert(InvocationMatchesStandard<decltype(&TraitReceiver::value), TraitReceiver>());
static_assert(InvocationMatchesStandard<decltype(&TraitReceiver::value), TraitReceiverPointer>());
static_assert(
    InvocationMatchesStandard<decltype(&TraitReceiver::value), TraitThrowingReceiverPointer>());
static_assert(InvocationMatchesStandard<decltype(&TraitReceiver::value), TraitReceiver&, int>());
static_assert(IsSameV<InvokeResultT<decltype(&TraitReceiver::value), TraitReceiver&>, int&>);
static_assert(
    IsSameV<InvokeResultT<decltype(&TraitReceiver::value), const TraitReceiver&>, const int&>);
static_assert(IsSameV<InvokeResultT<decltype(&TraitReceiver::value), TraitReceiver>, int&&>);
static_assert(IsNothrowConvertibleV<TraitThrowingConversion, int> ==
              std::is_nothrow_convertible_v<TraitThrowingConversion, int>);
static_assert(!IsConvertibleV<TraitExplicitConversion, int>);
static_assert(!IsNothrowInvocableRV<int, TraitReturnsThrowingConversion>);
static_assert(IsNothrowInvocableRV<void, TraitReturnsThrowingConversion>);

} // namespace

TEST(Base_TypeTraitsContracts) {
    CHECK((IsSameV<typename IsSame<int, int>::Type, TrueType>));
    CHECK((IsSameV<decltype(MakeIndexSequence<4>::Size), const usize>));
    CHECK((IsBaseOf<TraitBase, TraitDerived>::Value));
}

} // namespace rocks::test
