#include <TestSupport/Test.h>
#include <Core/Text/StringView.h>
#include <Core/Variant.h>

namespace rocks::test {
namespace {

struct Tracked {
    static inline int alive = 0;
    static inline int copies = 0;
    static inline int moves = 0;
    int value = 0;

    Tracked() noexcept {
        ++alive;
    }

    explicit Tracked(int number) noexcept : value(number) {
        ++alive;
    }

    Tracked(const Tracked& other) noexcept : value(other.value) {
        ++alive;
        ++copies;
    }

    Tracked(Tracked&& other) noexcept : value(other.value) {
        other.value = -1;
        ++alive;
        ++moves;
    }

    Tracked& operator=(const Tracked&) = delete;
    Tracked& operator=(Tracked&&) = delete;

    ~Tracked() noexcept {
        --alive;
    }

    friend bool operator==(const Tracked& left, const Tracked& right) noexcept {
        return left.value == right.value;
    }

    static void Reset() noexcept {
        alive = 0;
        copies = 0;
        moves = 0;
    }
};

struct MoveOnly {
    int value;

    explicit MoveOnly(int number) noexcept : value(number) {
    }

    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;

    MoveOnly(MoveOnly&& other) noexcept : value(other.value) {
        other.value = 0;
    }

    MoveOnly& operator=(MoveOnly&&) = delete;

    friend bool operator==(const MoveOnly& left, const MoveOnly& right) noexcept {
        return left.value == right.value;
    }
};

struct NoDefault {
    int value;

    explicit NoDefault(int number) noexcept : value(number) {
    }

    friend bool operator==(const NoDefault& left, const NoDefault& right) noexcept {
        return left.value == right.value;
    }
};

using Mixed = Variant<int, float, StringView>;

// 컴파일 시간 계약.
static_assert(Mixed::Count == 3 && VariantSizeV<const Mixed&> == 3);
static_assert(Mixed::IndexOf<float> == 1 && Mixed::IndexOf<double> == Mixed::Count);
static_assert(IsSameV<VariantAlternativeT<2, Mixed>, StringView>);
static_assert(__is_constructible(Variant<long, float>, long));
static_assert(!__is_constructible(Variant<long, float>, int)); // 변환이 필요한 값은 명시해야 한다
static_assert(!__is_constructible(Variant<bool, StringView>, const char*));
static_assert(__is_constructible(Variant<long, float>, InPlaceType<long>, int));
static_assert(!__is_constructible(Variant<int, MoveOnly>, const Variant<int, MoveOnly>&));
static_assert(__is_constructible(Variant<int, MoveOnly>, Variant<int, MoveOnly>&&));
static_assert(!__is_constructible(Variant<NoDefault, int>)); // 첫 대안이 기본 생성 불가
static_assert(__is_constructible(Variant<int, NoDefault>));
static_assert(sizeof(Variant<char, double>) == 16 && alignof(Variant<char, double>) == 8);
static_assert(sizeof(Variant<char>) == 2);

} // namespace

TEST(Variant_ConstructQueryAndAccess) {
    Mixed defaulted;
    CHECK(defaulted.Index() == 0 && defaulted.Is<int>() && defaulted.Get<int>() == 0);
    Mixed number = 3.5f;
    CHECK(number.Is<float>() && number.Get<float>() == 3.5f && number.Index() == 1);
    CHECK(number.TryGet<int>() == nullptr && number.TryGet<float>() != nullptr);
    *number.TryGet<float>() = 4.0f;
    CHECK(number.Get<float>() == 4.0f);
    Mixed text(InPlaceType<StringView>{}, "hello", 4u);
    CHECK(text.Is<StringView>() && text.Get<StringView>() == StringView("hell"));
    const Mixed& constant = text;
    CHECK(constant.TryGet<StringView>()->ByteCount() == 4 && constant.TryGet<float>() == nullptr);
    Variant<long, float> wide(InPlaceType<long>{}, 7);
    CHECK(wide.Get<long>() == 7);
    Variant<EmptyVariantState, int> optional;
    CHECK(optional.Is<EmptyVariantState>());
    optional = 5;
    CHECK(optional.Get<int>() == 5);
}

TEST(Variant_SetEmplaceAndLifetime) {
    Tracked::Reset();
    {
        Variant<int, Tracked> value = 1;
        CHECK(Tracked::alive == 0);
        value.Emplace<Tracked>(10);
        CHECK(value.Get<Tracked>().value == 10 && Tracked::alive == 1);
        // 현재 값을 인자로 다시 만들어도 안전하다(새 값을 먼저 만든다).
        value.Emplace<Tracked>(value.Get<Tracked>());
        CHECK(value.Get<Tracked>().value == 10 && Tracked::alive == 1);
        value.Set(value.Get<Tracked>());
        CHECK(value.Get<Tracked>().value == 10 && Tracked::alive == 1);
        value.Set(5);
        CHECK(value.Is<int>() && Tracked::alive == 0);
        value = Tracked(3);
        CHECK(value.Get<Tracked>().value == 3 && Tracked::alive == 1);
    }
    CHECK(Tracked::alive == 0);
}

TEST(Variant_CopyMoveAndAssignAcrossAlternatives) {
    Tracked::Reset();
    {
        Variant<int, Tracked> first(InPlaceType<Tracked>{}, 4);
        Variant<int, Tracked> copy = first;
        CHECK(copy.Get<Tracked>().value == 4 && Tracked::alive == 2 && Tracked::copies == 1);
        Variant<int, Tracked> moved = Move(first);
        CHECK(moved.Get<Tracked>().value == 4 && first.Is<Tracked>() &&
              first.Get<Tracked>().value == -1);
        Variant<int, Tracked> number = 9;
        number = copy; // int → Tracked
        CHECK(number.Get<Tracked>().value == 4 && Tracked::alive == 4);
        copy = Variant<int, Tracked>(2); // Tracked → int
        CHECK(copy.Get<int>() == 2 && Tracked::alive == 3);
        Variant<int, Tracked>& alias = number;
        number = alias; // 자기 대입
        CHECK(number.Get<Tracked>().value == 4);
        number = Move(alias); // 자기 이동 대입은 값을 유지한다
        CHECK(number.Get<Tracked>().value == 4);
        Variant<int, MoveOnly> owner(InPlaceType<MoveOnly>{}, 8);
        Variant<int, MoveOnly> target = 0;
        target = Move(owner);
        CHECK(target.Get<MoveOnly>().value == 8 && owner.Get<MoveOnly>().value == 0);
        CHECK(Tracked::alive == 3);
    }
    CHECK(Tracked::alive == 0);
}

TEST(Variant_VisitAndEquality) {
    Mixed value = 2;
    auto describe = [](const auto& alternative) noexcept -> int {
        if constexpr (IsSameV<RemoveConstT<RemoveReferenceT<decltype(alternative)>>, int>) {
            return alternative * 10;
        } else if constexpr (IsSameV<RemoveConstT<RemoveReferenceT<decltype(alternative)>>,
                                     float>) {
            return static_cast<int>(alternative);
        } else {
            return static_cast<int>(alternative.ByteCount()) + 1000;
        }
    };
    CHECK(Visit(describe, value) == 20);
    value = 3.75f;
    CHECK(Visit(describe, value) == 3);
    value = StringView("abc");
    CHECK(Visit(describe, value) == 1003);
    // void 반환과 lvalue 수정.
    Variant<int, float> mutableValue = 4;
    Visit([](auto& alternative) noexcept { alternative = alternative + 1; }, mutableValue);
    CHECK(mutableValue.Get<int>() == 5);
    // rvalue Variant는 값을 rvalue로 넘긴다.
    Variant<int, MoveOnly> owner(InPlaceType<MoveOnly>{}, 6);
    const int taken = Visit(
        [](auto&& alternative) noexcept -> int {
            if constexpr (IsSameV<RemoveReferenceT<decltype(alternative)>, MoveOnly>) {
                MoveOnly local(Move(alternative));
                return local.value;
            } else {
                return alternative;
            }
        },
        Move(owner));
    CHECK(taken == 6 && owner.Get<MoveOnly>().value == 0);
    // 같은 대안이고 값이 같을 때만 같다.
    CHECK(Mixed(1) == Mixed(1) && !(Mixed(1) == Mixed(2)) && !(Mixed(1) == Mixed(1.0f)));
    using WithNoDefault = Variant<int, NoDefault>;
    CHECK(WithNoDefault(InPlaceType<NoDefault>{}, 3) == WithNoDefault(InPlaceType<NoDefault>{}, 3));
    CHECK(!(WithNoDefault(InPlaceType<NoDefault>{}, 3) == WithNoDefault(3)));
    // 이동 전용·복사 추적 대안도 대안의 ==를 쓴다.
    using WithMoveOnly = Variant<int, MoveOnly>;
    CHECK(WithMoveOnly(InPlaceType<MoveOnly>{}, 4) == WithMoveOnly(InPlaceType<MoveOnly>{}, 4));
    Tracked::Reset();
    using WithTracked = Variant<int, Tracked>;
    CHECK(!(WithTracked(InPlaceType<Tracked>{}, 1) == WithTracked(InPlaceType<Tracked>{}, 2)));
    CHECK(Tracked::alive == 0);
}

} // namespace rocks::test
