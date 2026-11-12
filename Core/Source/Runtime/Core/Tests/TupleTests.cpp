#include <TestSupport/Test.h>
#include <Core/Text/StringView.h>
#include <Core/Tuple.h>

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

    Tracked& operator=(const Tracked& other) noexcept {
        value = other.value;
        ++copies;
        return *this;
    }

    Tracked& operator=(Tracked&& other) noexcept {
        value = other.value;
        other.value = -1;
        ++moves;
        return *this;
    }

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

    MoveOnly& operator=(MoveOnly&& other) noexcept {
        value = other.value;
        other.value = 0;
        return *this;
    }
};

struct ExplicitOnly {
    int value;

    explicit ExplicitOnly(int number) noexcept : value(number) {
    }
};

// 컴파일 시간 계약: traits, decay, 암묵/명시 생성, constexpr 사용.
static_assert(TupleSizeV<Tuple<int, float, char>> == 3);
static_assert(TupleSizeV<const Tuple<>&> == 0);
static_assert(IsSameV<TupleElementT<1, Tuple<int, float&, char>>, float&>);
static_assert(IsSameV<decltype(MakeTuple(1, 2.0f)), Tuple<int, float>>);
static_assert(IsSameV<decltype(MakeTuple("text")), Tuple<const char*>>);
static_assert(IsSameV<decltype(MakeTuple(static_cast<const int&>(1))), Tuple<int>>);
static_assert(__is_convertible_to(int, Tuple<long>));
static_assert(!__is_convertible_to(int, Tuple<ExplicitOnly>));
static_assert(__is_constructible(Tuple<ExplicitOnly>, int));
static_assert(!__is_constructible(Tuple<int&>));          // 참조 원소는 기본 생성 없음
static_assert(!__is_constructible(Tuple<int, int>, int)); // 개수 불일치
static_assert(!__is_assignable(Tuple<MoveOnly>&, const Tuple<MoveOnly>&));
// 원소별 변환 생성: 모든 원소가 암묵 변환되면 암묵, 하나라도 명시 생성이면 명시다.
static_assert(__is_convertible_to(Tuple<int, float>, Tuple<long, double>));
static_assert(!__is_convertible_to(Tuple<int>, Tuple<ExplicitOnly>));
static_assert(__is_constructible(Tuple<ExplicitOnly>, Tuple<int>));
static_assert(!__is_constructible(Tuple<int, int>, Tuple<int>)); // 개수 불일치
static_assert(!__is_constructible(Tuple<MoveOnly>, const Tuple<MoveOnly>&));
static_assert(__is_constructible(Tuple<const int&>, Tuple<int&>));
static_assert(!__is_constructible(Tuple<long&>, Tuple<int&>)); // 다른 타입 참조로는 묶지 않는다
static_assert(__is_assignable(Tuple<MoveOnly>&, Tuple<MoveOnly>&&));
static_assert(Get<1>(MakeTuple(1, 2, 3)) == 2);
static_assert(MakeTuple(1, 2) == MakeTuple(1, 2));
static_assert(Tuple<int, double>() == MakeTuple(0, 0.0));

constexpr int SumWithBinding() noexcept {
    auto [a, b, c] = MakeTuple(1, 2, 3);
    return a + b + c;
}
static_assert(SumWithBinding() == 6);

} // namespace

TEST(Tuple_ConstructGetAndCompare) {
    Tuple<int, float, StringView> value(3, 0.5f, StringView("x"));
    CHECK(Get<0>(value) == 3 && Get<1>(value) == 0.5f && Get<2>(value) == StringView("x"));
    Get<0>(value) = 7;
    CHECK(value.Get<0>() == 7);
    const Tuple<int, float, StringView>& constant = value;
    CHECK(Get<1>(constant) == 0.5f);
    Tuple<int, int> same(1, 2); // 같은 타입 원소도 index로 구분한다.
    CHECK(Get<0>(same) == 1 && Get<1>(same) == 2);
    Tuple<int, double, bool> zero;
    CHECK(Get<0>(zero) == 0 && Get<1>(zero) == 0.0 && !Get<2>(zero));
    CHECK(MakeTuple(1, 2) == MakeTuple(1, 2));
    CHECK(!(MakeTuple(1, 2) == MakeTuple(1, 3)));
    Tuple<> empty;
    CHECK(empty == Tuple<>() && Tuple<>::Size == 0);
}

TEST(Tuple_StructuredBindings) {
    Tuple<int, StringView> value(4, StringView("name"));
    auto [number, text] = value; // 복사
    number = 9;
    CHECK(Get<0>(value) == 4 && text == StringView("name"));
    auto& [numberRef, textRef] = value; // 참조
    numberRef = 11;
    CHECK(Get<0>(value) == 11 && textRef == StringView("name"));
    const auto& [constNumber, constText] = value;
    CHECK(constNumber == 11 && constText.ByteCount() == 4);
    auto&& [moved] = MakeTuple(MoveOnly(5)); // rvalue 바인딩은 수명이 연장된 임시 객체를 가리킨다.
    CHECK(moved.value == 5);
    int a = 1;
    float b = 2.0f;
    auto [refA, refB] = Tie(a, b); // 참조 원소: 바인딩도 원래 변수를 가리킨다.
    refA = 10;
    refB = 20.0f;
    CHECK(a == 10 && b == 20.0f);
}

TEST(Tuple_ReferenceElementsAndTie) {
    int a = 1;
    int b = 2;
    Tuple<int&, int&> references(a, b);
    Get<0>(references) = 5;
    CHECK(a == 5);
    // 참조 원소 Tuple의 대입은 참조 대상에 대입한다(재결합하지 않는다).
    int c = 7;
    int d = 8;
    Tuple<int&, int&> others(c, d);
    references = others;
    CHECK(a == 7 && b == 8 && &Get<0>(references) == &a);
    Tie(a, b) = MakeTuple(30, 40);
    CHECK(a == 30 && b == 40);
    // 교환 같은 겹친 대입은 원소 순서대로 실행된다.
    Tie(a, b) = MakeTuple(b, a);
    CHECK(a == 40 && b == 30);
    // 다른 원소 타입에서의 변환 대입.
    long wide = 0;
    float ratio = 0.0f;
    Tie(wide, ratio) = Tuple<int, double>(12, 0.25);
    CHECK(wide == 12 && ratio == 0.25f);
    CHECK(Tie(a, b) == Tie(a, b));
}

TEST(Tuple_LifetimeCopyMoveAndSelfAssignment) {
    Tracked::Reset();
    {
        Tuple<Tracked, int> first(Tracked(1), 2);
        CHECK(Tracked::alive == 1 && Tracked::moves == 1);
        Tuple<Tracked, int> copy = first;
        CHECK(Tracked::alive == 2 && Tracked::copies == 1 && Get<0>(copy).value == 1);
        Tuple<Tracked, int> moved = Move(first);
        CHECK(Tracked::alive == 3 && Get<0>(moved).value == 1 && Get<0>(first).value == -1);
        Tuple<Tracked, int>& alias = copy;
        copy = alias; // 자기 대입
        CHECK(Get<0>(copy).value == 1 && Get<1>(copy) == 2);
        first = Move(moved);
        CHECK(Get<0>(first).value == 1 && Get<0>(moved).value == -1);
        copy = Tuple<Tracked, int>(Tracked(9), 3);
        CHECK(Get<0>(copy).value == 9 && Get<1>(copy) == 3);
        const Tuple<Tracked, int> expected(Tracked(9), 3);
        CHECK(!(copy == first) && copy == expected);
        CHECK(Tracked::alive == 4);
    }
    CHECK(Tracked::alive == 0);
}

TEST(Tuple_ApplyForwardsElements) {
    const auto sum = Apply([](int x, float y) noexcept { return static_cast<float>(x) + y; },
                           MakeTuple(2, 0.5f));
    CHECK(sum == 2.5f);
    // lvalue tuple은 참조로, rvalue tuple은 이동으로 넘긴다.
    Tuple<int, int> pair(1, 2);
    Apply(
        [](int& x, int& y) noexcept {
            x += 10;
            y += 20;
        },
        pair);
    CHECK(Get<0>(pair) == 11 && Get<1>(pair) == 22);
    Tuple<MoveOnly> owner(MoveOnly(8));
    const int taken =
        Apply([](MoveOnly&& value) noexcept { return MoveOnly(Move(value)).value; }, Move(owner));
    CHECK(taken == 8 && Get<0>(owner).value == 0);
    CHECK(Apply([]() noexcept { return 42; }, Tuple<>()) == 42);
}

TEST(Tuple_ConvertingConstruction) {
    const Tuple<int, float> narrow(3, 0.5f);
    Tuple<long, double> wide = narrow;
    CHECK(Get<0>(wide) == 3 && Get<1>(wide) == 0.5);
    Tuple<ExplicitOnly, long> explicitWide(Tuple<int, int>(7, 8));
    CHECK(Get<0>(explicitWide).value == 7 && Get<1>(explicitWide) == 8);
    // rvalue 변환은 원소를 이동한다.
    Tracked::Reset();
    {
        Tuple<Tracked, int> source(Tracked(4), 1);
        Tuple<Tracked, long> moved(Move(source));
        CHECK(Get<0>(moved).value == 4 && Get<0>(source).value == -1 && Get<1>(moved) == 1);
        CHECK(Tracked::copies == 0);
        Tuple<Tracked, long> copied(static_cast<const Tuple<Tracked, int>&>(source));
        CHECK(Tracked::copies == 1);
    }
    CHECK(Tracked::alive == 0);
    // 참조 원소: 같은 대상에 묶인다.
    int value = 5;
    Tuple<int&> reference(value);
    Tuple<const int&> view = reference;
    value = 6;
    CHECK(Get<0>(view) == 6 && &Get<0>(view) == &value);
    Tuple<const int&> fromRvalue = Tuple<int&>(value);
    CHECK(&Get<0>(fromRvalue) == &value);
}

} // namespace rocks::test
