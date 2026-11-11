#pragma once

#include <Base/Types.h>
#include <Core/Move.h>
#include <Core/Traits.h>

// 구조적 바인딩(auto [a, b] = tuple)은 언어 규칙상 std::tuple_size·std::tuple_element 특수화를
// 이름으로 찾는다. 이 두 trait의 기본 템플릿 선언을 얻으려고 여기서만 <utility>를 포함한다. STL
// 타입·할당·함수는 쓰지 않으며 policy 검사가 이 파일의 두 이름과 이 include만 허용한다.
// namespace std에 직접 전방 선언하는 방식은 표준 위반이고 libc++(macOS·iOS·Android)의 inline
// namespace(__1, __ndk1)와 충돌하므로 쓰지 않는다.
#include <utility>

namespace rocks {

template <typename... Types>
class Tuple;

namespace detail {

template <typename T>
struct TupleDecay {
    using Type = RemoveConstT<T>;
};

// 배열은 원소 포인터로 바꾼다(MakeTuple("text")는 const char*).
template <typename T, usize Count>
struct TupleDecay<T[Count]> {
    using Type = T*;
};

template <typename T>
using TupleDecayT = typename TupleDecay<RemoveReferenceT<T>>::Type;

template <typename T>
inline constexpr bool IsTupleV = false;

template <typename... Types>
inline constexpr bool IsTupleV<Tuple<Types...>> = true;

template <typename T>
inline constexpr bool IsRvalueReferenceV = false;

template <typename T>
inline constexpr bool IsRvalueReferenceV<T&&> = true;

struct TupleConstructTag {};

// 원소 하나를 담는다. 같은 타입 원소가 여러 개여도 Index로 구분된다.
template <usize Index, typename T>
struct TupleLeaf {
    T value;

    constexpr TupleLeaf() noexcept
        requires(!IsLvalueReferenceV<T>)
        : value() {
    }

    template <typename Arg>
    constexpr TupleLeaf(TupleConstructTag, Arg&& argument) noexcept
        : value(Forward<Arg>(argument)) {
    }
};

template <typename Sequence, typename... Types>
struct TupleStorage;

template <usize... Indices, typename... Types>
struct TupleStorage<IndexSequence<Indices...>, Types...> : TupleLeaf<Indices, Types>... {
    constexpr TupleStorage() noexcept = default;

    template <typename... Args>
    constexpr explicit TupleStorage(TupleConstructTag tag, Args&&... arguments) noexcept
        : TupleLeaf<Indices, Types>(tag, Forward<Args>(arguments))... {
    }
};

} // namespace detail

// 서로 다른 타입 값을 고정 개수로 묶는 값 타입이다(UE TTuple 대응). heap 할당이 없다.
// 사용 예:
//   Tuple<int, float> pair(3, 0.5f);
//   auto [count, ratio] = pair;              // 구조적 바인딩
//   int& first = Get<0>(pair);
//   auto made = MakeTuple(1, StringView("a")); // Tuple<int, StringView>
//   int a = 0; float b = 0.0f;
//   Tie(a, b) = pair;                        // 원소별 대입
//   float sum = Apply([](int x, float y) noexcept { return x + y; }, pair);
//
// 계약:
// - 원소 타입은 값 또는 lvalue 참조다(rvalue 참조 원소는 지원하지 않는다). 생성·대입은 noexcept
//   이어야 한다(엔진은 예외를 쓰지 않는다).
// - 참조 원소의 대입은 참조 대상에 대입한다(표준 tuple과 같다). 참조 원소가 있으면 기본 생성이
//   없다.
// - 기본 생성은 모든 원소를 값 초기화한다(int는 0).
// - 같은 개수의 Tuple끼리만 원소별 비교·대입한다. 서로 다른 원소 타입 사이의 변환 생성은 아직
//   제공하지 않는다.
// - 구조적 바인딩은 rocks::get(소문자, 언어의 tuple protocol이 찾는 이름)과 std::tuple_size·
//   std::tuple_element 특수화로 동작한다. 엔진 코드에서는 Get<I>를 쓴다.
template <typename... Types>
class Tuple : private detail::TupleStorage<MakeIndexSequence<sizeof...(Types)>, Types...> {
    static_assert((!detail::IsRvalueReferenceV<Types> && ...),
                  "Tuple does not support rvalue reference elements");
    static_assert((IsNothrowDestructibleV<Types> && ...),
                  "Tuple elements need noexcept destruction");

    using Storage = detail::TupleStorage<MakeIndexSequence<sizeof...(Types)>, Types...>;

public:
    static constexpr usize Size = sizeof...(Types);

    constexpr Tuple() noexcept
        requires((__is_nothrow_constructible(Types) && ...))
        : Storage() {
    }

    // 원소별 생성이다. 모든 원소가 암묵 변환 가능할 때만 암묵 생성이다.
    template <typename... Args>
        requires(sizeof...(Args) == sizeof...(Types) && sizeof...(Types) > 0 &&
                 !(sizeof...(Args) == 1 && (detail::IsTupleV<detail::TupleDecayT<Args>> && ...)) &&
                 (__is_nothrow_constructible(Types, Args &&) && ...))
    constexpr explicit(!(__is_convertible_to(Args&&, Types) && ...))
        Tuple(Args&&... arguments) noexcept
        : Storage(detail::TupleConstructTag{}, Forward<Args>(arguments)...) {
    }

    constexpr Tuple(const Tuple&) noexcept = default;
    constexpr Tuple(Tuple&&) noexcept = default;

    constexpr Tuple& operator=(const Tuple& other) noexcept
        requires((__is_assignable(Types&, const Types&) && ...))
    {
        AssignFrom(other, MakeIndexSequence<Size>{});
        return *this;
    }

    constexpr Tuple& operator=(Tuple&& other) noexcept
        requires((__is_assignable(Types&, Types &&) && ...))
    {
        MoveAssignFrom(other, MakeIndexSequence<Size>{});
        return *this;
    }

    // 원소 타입이 다른 같은 크기 Tuple에서 원소별 대입한다(Tie(a, b) = MakeTuple(1, 2.0f) 등).
    template <typename... Others>
        requires(sizeof...(Others) == sizeof...(Types) && !IsSameV<Tuple<Others...>, Tuple> &&
                 (__is_assignable(Types&, const Others&) && ...))
    constexpr Tuple& operator=(const Tuple<Others...>& other) noexcept {
        AssignFrom(other, MakeIndexSequence<Size>{});
        return *this;
    }

    template <typename... Others>
        requires(sizeof...(Others) == sizeof...(Types) && !IsSameV<Tuple<Others...>, Tuple> &&
                 (__is_assignable(Types&, Others &&) && ...))
    constexpr Tuple& operator=(Tuple<Others...>&& other) noexcept {
        MoveAssignFrom(other, MakeIndexSequence<Size>{});
        return *this;
    }

    ~Tuple() noexcept = default;

    template <usize Index>
    constexpr TypePackElement<Index, Types...>& Get() & noexcept {
        return Leaf<Index>().value;
    }

    template <usize Index>
    constexpr const TypePackElement<Index, Types...>& Get() const& noexcept {
        return Leaf<Index>().value;
    }

    template <usize Index>
    constexpr TypePackElement<Index, Types...>&& Get() && noexcept {
        return Forward<TypePackElement<Index, Types...>>(Leaf<Index>().value);
    }

    // 원소별 ==이다. 참조 원소는 참조 대상 값을 비교한다.
    friend constexpr bool operator==(const Tuple& left, const Tuple& right) noexcept {
        return Equal(left, right, MakeIndexSequence<Size>{});
    }

private:
    template <typename...>
    friend class Tuple;

    template <usize Index>
    constexpr detail::TupleLeaf<Index, TypePackElement<Index, Types...>>& Leaf() noexcept {
        static_assert(Index < Size, "Tuple index out of range");
        return static_cast<detail::TupleLeaf<Index, TypePackElement<Index, Types...>>&>(*this);
    }

    template <usize Index>
    constexpr const detail::TupleLeaf<Index, TypePackElement<Index, Types...>>&
    Leaf() const noexcept {
        static_assert(Index < Size, "Tuple index out of range");
        return static_cast<const detail::TupleLeaf<Index, TypePackElement<Index, Types...>>&>(
            *this);
    }

    // 자기 대입도 원소별 자기 대입이라 안전하다.
    template <typename... Others, usize... Indices>
    constexpr void AssignFrom(const Tuple<Others...>& other, IndexSequence<Indices...>) noexcept {
        ((Leaf<Indices>().value = other.template Leaf<Indices>().value), ...);
    }

    // 값 원소는 이동, 참조 원소는 참조 대상 lvalue로 대입한다.
    template <typename... Others, usize... Indices>
    constexpr void MoveAssignFrom(Tuple<Others...>& other, IndexSequence<Indices...>) noexcept {
        ((Leaf<Indices>().value =
              Forward<TypePackElement<Indices, Others...>>(other.template Leaf<Indices>().value)),
         ...);
    }

    template <usize... Indices>
    static constexpr bool Equal(const Tuple& left, const Tuple& right,
                                IndexSequence<Indices...>) noexcept {
        return ((left.template Leaf<Indices>().value == right.template Leaf<Indices>().value) &&
                ...);
    }
};

// ---- 원소 접근 ----

template <usize Index, typename... Types>
constexpr TypePackElement<Index, Types...>& Get(Tuple<Types...>& tuple) noexcept {
    return tuple.template Get<Index>();
}

template <usize Index, typename... Types>
constexpr const TypePackElement<Index, Types...>& Get(const Tuple<Types...>& tuple) noexcept {
    return tuple.template Get<Index>();
}

template <usize Index, typename... Types>
constexpr TypePackElement<Index, Types...>&& Get(Tuple<Types...>&& tuple) noexcept {
    return Move(tuple).template Get<Index>();
}

// 구조적 바인딩의 tuple protocol이 ADL로 찾는 이름이라 소문자다(STL 호환 begin/end와 같은 예외).
template <usize Index, typename... Types>
constexpr TypePackElement<Index, Types...>& get(Tuple<Types...>& tuple) noexcept {
    return tuple.template Get<Index>();
}

template <usize Index, typename... Types>
constexpr const TypePackElement<Index, Types...>& get(const Tuple<Types...>& tuple) noexcept {
    return tuple.template Get<Index>();
}

template <usize Index, typename... Types>
constexpr TypePackElement<Index, Types...>&& get(Tuple<Types...>&& tuple) noexcept {
    return Move(tuple).template Get<Index>();
}

// ---- traits ----

template <typename T>
struct TupleSize;

template <typename... Types>
struct TupleSize<Tuple<Types...>> {
    static constexpr usize Value = sizeof...(Types);
};

template <typename T>
inline constexpr usize TupleSizeV = TupleSize<RemoveConstT<RemoveReferenceT<T>>>::Value;

template <usize Index, typename T>
struct TupleElement;

template <usize Index, typename... Types>
struct TupleElement<Index, Tuple<Types...>> {
    using Type = TypePackElement<Index, Types...>;
};

template <usize Index, typename T>
using TupleElementT = typename TupleElement<Index, T>::Type;

// ---- 생성 도우미 ----

// 인자를 값으로 복사·이동해 담는다. 참조와 const는 벗기고 배열은 포인터로 바꾼다.
template <typename... Args>
constexpr Tuple<detail::TupleDecayT<Args>...> MakeTuple(Args&&... arguments) noexcept {
    return Tuple<detail::TupleDecayT<Args>...>(Forward<Args>(arguments)...);
}

// lvalue 참조를 묶는다. Tie(a, b) = tuple로 원소를 꺼내 대입한다.
template <typename... Types>
constexpr Tuple<Types&...> Tie(Types&... references) noexcept {
    return Tuple<Types&...>(references...);
}

namespace detail {

template <typename Function, typename TupleType, usize... Indices>
constexpr decltype(auto) ApplyTuple(Function&& function, TupleType&& tuple,
                                    IndexSequence<Indices...>) noexcept {
    return Forward<Function>(function)(Get<Indices>(Forward<TupleType>(tuple))...);
}

} // namespace detail

// tuple의 원소를 인자로 function을 호출한다. rvalue tuple이면 원소를 이동해 넘긴다.
template <typename Function, typename TupleType>
constexpr decltype(auto) Apply(Function&& function, TupleType&& tuple) noexcept {
    return detail::ApplyTuple(Forward<Function>(function), Forward<TupleType>(tuple),
                              MakeIndexSequence<TupleSizeV<TupleType>>{});
}

} // namespace rocks

// 구조적 바인딩용 std trait 특수화다(위 <utility> 주석 참고). const Tuple은 표준 라이브러리의
// const 특수화가 이 정의를 따른다.
template <typename... Types>
struct std::tuple_size<rocks::Tuple<Types...>> {
    static constexpr decltype(sizeof(0)) value = sizeof...(Types);
};

template <decltype(sizeof(0)) Index, typename... Types>
struct std::tuple_element<Index, rocks::Tuple<Types...>> {
    using type = rocks::TypePackElement<Index, Types...>;
};
