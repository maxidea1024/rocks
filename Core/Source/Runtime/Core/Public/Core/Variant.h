#pragma once

#include <Base/Types.h>
#include <Core/Move.h>
#include <Core/Traits.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks {

// Variant의 "값 없음" 대안이다(UE FEmptyVariantState 대응). 첫 대안으로 두면 기본 생성이 빈 상태가
// 된다. 예: Variant<EmptyVariantState, int, StringView>
struct EmptyVariantState {
    friend constexpr bool operator==(EmptyVariantState, EmptyVariantState) noexcept {
        return true;
    }
};

// 생성할 대안 타입을 명시하는 표지다(UE TInPlaceType 대응).
// 예: Variant<int, Name> value(InPlaceType<Name>{}, "Player");
template <typename T>
struct InPlaceType {
    explicit constexpr InPlaceType() noexcept = default;
};

template <typename... Types>
class Variant;

namespace detail {

template <typename... Types>
struct VariantTypeList {};

template <typename T, typename... Types>
constexpr usize VariantIndexOf() noexcept {
    constexpr bool matches[] = {IsSameV<T, Types>...};
    for (usize index = 0; index < sizeof...(Types); ++index) {
        if (matches[index]) {
            return index;
        }
    }
    return sizeof...(Types);
}

// Types 안에서 T가 나오는 횟수다.
template <typename T, typename... Types>
inline constexpr usize VariantCountOf = (static_cast<usize>(IsSameV<T, Types>) + ... + 0);

template <typename... Types>
constexpr usize VariantStorageSize() noexcept {
    usize size = 1;
    ((size = sizeof(Types) > size ? sizeof(Types) : size), ...);
    return size;
}

template <typename T>
using VariantDecayT = RemoveConstT<RemoveReferenceT<T>>;

template <typename T>
inline constexpr bool IsVariantV = false;

template <typename... Types>
inline constexpr bool IsVariantV<Variant<Types...>> = true;

} // namespace detail

// 여러 타입 중 정확히 하나의 값을 담는 값 타입이다(UE TVariant 대응). heap 할당이 없다.
// 사용 예:
//   Variant<int, float, StringView> value = 3;      // 대안과 정확히 같은 타입만 암묵 변환
//   value.Set(StringView("name"));
//   if (const StringView* text = value.TryGet<StringView>()) { Use(*text); }
//   float scale = value.Get<float>();               // 다른 대안이면 Fatal
//   Visit([](const auto& alternative) noexcept { Print(alternative); }, value);
//
// 계약:
// - 항상 대안 하나를 담는다(빈 상태가 필요하면 EmptyVariantState를 대안으로 둔다). 기본 생성은
//   첫 대안을 값 초기화한다(UE와 같다).
// - 대안 타입은 서로 달라야 하고 참조·void가 아니어야 하며 이동 생성·소멸이 noexcept여야 한다.
//   복사는 모든 대안이 noexcept 복사 가능할 때만 있다.
// - 값으로부터의 생성·대입은 대안과 정확히 같은 타입(const·참조 제거 후)만 받는다. int를
//   Variant<long, float>에 넣는 것처럼 변환이 필요한 경우는 InPlaceType 또는 Emplace<T>로 명시한다.
// - Get<T>는 현재 대안이 T가 아니면 Fatal이고 TryGet<T>는 nullptr를 반환한다. 반환 참조·포인터는
//   다음 Set·Emplace·대입·소멸 전까지만 유효하다.
// - 대입·Emplace·Set은 새 값을 먼저 만든 뒤 기존 값을 파괴하고 옮긴다. 인자나 대입 원본이 현재
//   값(또는 현재 값이 소유한 객체)을 가리켜도 안전하다. 대신 이동이 한 번 더 일어난다.
// - 이동 후 원본은 같은 대안을 유지하고 값은 그 타입의 이동 후 상태다.
// - 같은 Variant의 동시 변경과, 대안 소멸자에서 같은 Variant를 다시 변경하는 재진입은 지원하지
// 않는다.
template <typename... Types>
class Variant {
    static_assert(sizeof...(Types) > 0, "Variant needs at least one alternative");
    static_assert(sizeof...(Types) < 256, "Variant supports at most 255 alternatives");
    static_assert((!__is_reference(Types) && ...), "Variant alternatives must not be references");
    static_assert((!__is_void(Types) && ...), "Variant alternatives must not be void");
    static_assert(((detail::VariantCountOf<Types, Types...> == 1) && ...),
                  "Variant alternatives must be unique types");
    static_assert((IsNothrowMoveConstructibleV<Types> && ...),
                  "Variant alternatives need a noexcept move constructor");
    static_assert((IsNothrowDestructibleV<Types> && ...),
                  "Variant alternatives need a noexcept destructor");

public:
    static constexpr usize Count = sizeof...(Types);

    // T의 대안 index다. T가 대안이 아니면 Count다.
    template <typename T>
    static constexpr usize IndexOf = detail::VariantIndexOf<T, Types...>();

    using AlternativeList = detail::VariantTypeList<Types...>;

    Variant() noexcept
        requires(__is_nothrow_constructible(TypePackElement<0, Types...>))
    {
        ConstructAt<TypePackElement<0, Types...>>(storage_);
        index_ = 0;
    }

    template <typename T>
        requires(IndexOf<detail::VariantDecayT<T>> < Count &&
                 __is_nothrow_constructible(detail::VariantDecayT<T>, T &&))
    Variant(T&& value) noexcept {
        using Alternative = detail::VariantDecayT<T>;
        ConstructAt<Alternative>(storage_, Forward<T>(value));
        index_ = static_cast<uint8>(IndexOf<Alternative>);
    }

    template <typename T, typename... Args>
        requires(IndexOf<T> < Count && __is_nothrow_constructible(T, Args && ...))
    explicit Variant(InPlaceType<T>, Args&&... args) noexcept {
        ConstructAt<T>(storage_, Forward<Args>(args)...);
        index_ = static_cast<uint8>(IndexOf<T>);
    }

    Variant(const Variant& other) noexcept
        requires((__is_nothrow_constructible(Types, const Types&) && ...))
    {
        CopyConstructFrom(other, MakeIndexSequence<Count>{});
    }

    Variant(Variant&& other) noexcept {
        MoveConstructFrom(other, MakeIndexSequence<Count>{});
    }

    ~Variant() noexcept {
        DestroyCurrent(MakeIndexSequence<Count>{});
    }

    Variant& operator=(const Variant& other) noexcept
        requires((__is_nothrow_constructible(Types, const Types&) && ...))
    {
        if (this != &other) {
            Variant copy(other);
            Replace(copy);
        }
        return *this;
    }

    Variant& operator=(Variant&& other) noexcept {
        if (this != &other) {
            Variant moved(Move(other));
            Replace(moved);
        }
        return *this;
    }

    template <typename T>
        requires(IndexOf<detail::VariantDecayT<T>> < Count &&
                 __is_nothrow_constructible(detail::VariantDecayT<T>, T &&))
    Variant& operator=(T&& value) noexcept {
        Set(Forward<T>(value));
        return *this;
    }

    // 현재 대안의 index다(0부터).
    usize Index() const noexcept {
        return index_;
    }

    // 현재 대안이 T인지다. T는 대안이어야 한다(아니면 컴파일 오류).
    template <typename T>
    bool Is() const noexcept {
        static_assert(IndexOf<T> < Count, "T is not an alternative of this Variant");
        return index_ == IndexOf<T>;
    }

    template <typename T>
    T& Get() & noexcept {
        Check<T>();
        return Ref<T>();
    }

    template <typename T>
    const T& Get() const& noexcept {
        Check<T>();
        return Ref<T>();
    }

    template <typename T>
    T&& Get() && noexcept {
        Check<T>();
        return Move(Ref<T>());
    }

    // 현재 대안이 T가 아니면 nullptr이다.
    template <typename T>
    T* TryGet() noexcept {
        return Is<T>() ? &Ref<T>() : nullptr;
    }

    template <typename T>
    const T* TryGet() const noexcept {
        return Is<T>() ? &Ref<T>() : nullptr;
    }

    // T를 args로 만들어 현재 값을 바꾼다. 새 값을 먼저 만들므로 args가 현재 값을 참조해도 안전하다.
    template <typename T, typename... Args>
    T& Emplace(Args&&... args) noexcept {
        static_assert(IndexOf<T> < Count, "T is not an alternative of this Variant");
        static_assert(__is_nothrow_constructible(T, Args && ...),
                      "Variant::Emplace requires a noexcept constructor");
        T replacement(Forward<Args>(args)...);
        DestroyCurrent(MakeIndexSequence<Count>{});
        ConstructAt<T>(storage_, Move(replacement));
        index_ = static_cast<uint8>(IndexOf<T>);
        return Ref<T>();
    }

    // value와 정확히 같은 타입의 대안으로 바꾼다.
    template <typename T>
        requires(IndexOf<detail::VariantDecayT<T>> < Count)
    detail::VariantDecayT<T>& Set(T&& value) noexcept {
        return Emplace<detail::VariantDecayT<T>>(Forward<T>(value));
    }

    // 같은 대안이고 그 값이 ==이면 같다.
    friend bool operator==(const Variant& left, const Variant& right) noexcept {
        return left.index_ == right.index_ && EqualSame(left, right, MakeIndexSequence<Count>{});
    }

private:
    template <typename T>
    void Check() const noexcept {
        if (!Is<T>()) {
            platform::Fatal("Variant::Get called with a different alternative");
        }
    }

    // 같은 저장소에 다른 타입 객체를 다시 만들므로 launder로 포인터 가정을 끊는다.
    template <typename T>
    T& Ref() noexcept {
        return *platform::Launder(reinterpret_cast<T*>(storage_));
    }

    template <typename T>
    const T& Ref() const noexcept {
        return *platform::Launder(reinterpret_cast<const T*>(storage_));
    }

    template <usize... Indices>
    void DestroyCurrent(IndexSequence<Indices...>) noexcept {
        (void)((index_ == Indices ? (DestroyAt(&Ref<TypePackElement<Indices, Types...>>()), true)
                                  : false) ||
               ...);
    }

    template <usize... Indices>
    void CopyConstructFrom(const Variant& other, IndexSequence<Indices...>) noexcept {
        (void)((other.index_ == Indices
                    ? (ConstructAt<TypePackElement<Indices, Types...>>(
                           storage_, other.template Ref<TypePackElement<Indices, Types...>>()),
                       true)
                    : false) ||
               ...);
        index_ = other.index_;
    }

    template <usize... Indices>
    void MoveConstructFrom(Variant& other, IndexSequence<Indices...>) noexcept {
        (void)((other.index_ == Indices
                    ? (ConstructAt<TypePackElement<Indices, Types...>>(
                           storage_,
                           Move(other.template Ref<TypePackElement<Indices, Types...>>())),
                       true)
                    : false) ||
               ...);
        index_ = other.index_;
    }

    // 이미 만든 임시 값으로 현재 값을 바꾼다. source는 this와 무관한 객체다.
    void Replace(Variant& source) noexcept {
        DestroyCurrent(MakeIndexSequence<Count>{});
        MoveConstructFrom(source, MakeIndexSequence<Count>{});
    }

    template <usize... Indices>
    static bool EqualSame(const Variant& left, const Variant& right,
                          IndexSequence<Indices...>) noexcept {
        bool equal = false;
        (void)((left.index_ == Indices
                    ? (equal = left.template Ref<TypePackElement<Indices, Types...>>() ==
                               right.template Ref<TypePackElement<Indices, Types...>>(),
                       true)
                    : false) ||
               ...);
        return equal;
    }

    alignas(Types...) unsigned char storage_[detail::VariantStorageSize<Types...>()];
    uint8 index_ = 0;
};

// ---- traits ----

template <typename T>
struct VariantSize;

template <typename... Types>
struct VariantSize<Variant<Types...>> {
    static constexpr usize Value = sizeof...(Types);
};

template <typename T>
inline constexpr usize VariantSizeV = VariantSize<detail::VariantDecayT<T>>::Value;

template <usize Index, typename T>
struct VariantAlternative;

template <usize Index, typename... Types>
struct VariantAlternative<Index, Variant<Types...>> {
    using Type = TypePackElement<Index, Types...>;
};

template <usize Index, typename T>
using VariantAlternativeT = typename VariantAlternative<Index, T>::Type;

// ---- Visit ----

namespace detail {

template <typename Visitor, typename VariantType, typename Alternative>
decltype(auto) VisitAlternative(Visitor&& visitor, VariantType&& variant) noexcept {
    return Forward<Visitor>(visitor)(Forward<VariantType>(variant).template Get<Alternative>());
}

template <typename Visitor, typename VariantType, typename... Types>
decltype(auto) VisitVariant(Visitor&& visitor, VariantType&& variant,
                            VariantTypeList<Types...>) noexcept {
    using Result = decltype(VisitAlternative<Visitor, VariantType, TypePackElement<0, Types...>>(
        DeclVal<Visitor>(), DeclVal<VariantType>()));
    static_assert((IsSameV<Result, decltype(VisitAlternative<Visitor, VariantType, Types>(
                                       DeclVal<Visitor>(), DeclVal<VariantType>()))> &&
                   ...),
                  "Visit requires the visitor to return the same type for every alternative");
    using Function = Result (*)(Visitor&&, VariantType&&) noexcept;
    static constexpr Function table[] = {&VisitAlternative<Visitor, VariantType, Types>...};
    return table[variant.Index()](Forward<Visitor>(visitor), Forward<VariantType>(variant));
}

} // namespace detail

// 현재 대안 값으로 visitor를 호출한다(UE Visit 대응, 단일 Variant). visitor는 모든 대안에 대해
// 같은 반환 타입이어야 한다. rvalue Variant면 값을 rvalue로 넘긴다. 여러 Variant 조합은 Visit을
// 중첩한다.
template <typename Visitor, typename VariantType>
    requires detail::IsVariantV<detail::VariantDecayT<VariantType>>
decltype(auto) Visit(Visitor&& visitor, VariantType&& variant) noexcept {
    return detail::VisitVariant(Forward<Visitor>(visitor), Forward<VariantType>(variant),
                                typename detail::VariantDecayT<VariantType>::AlternativeList{});
}

} // namespace rocks
