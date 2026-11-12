#include <TestSupport/Test.h>
#include <Core/Optional.h>
#include <Core/ScopeExit.h>

namespace rocks::test {
namespace {

// 생성·이동·소멸 횟수로 수명 계약을 확인하는 이동 전용 타입이다.
struct Tracked {
    static inline int alive = 0;
    static inline int destroyed = 0;
    int value = 0;
    bool movedFrom = false;

    explicit Tracked(int number) noexcept : value(number) {
        ++alive;
    }

    Tracked(Tracked&& other) noexcept : value(other.value) {
        other.movedFrom = true;
        ++alive;
    }

    Tracked& operator=(Tracked&&) = delete;
    Tracked(const Tracked&) = delete;

    ~Tracked() noexcept {
        --alive;
        ++destroyed;
    }

    friend bool operator==(const Tracked& a, const Tracked& b) noexcept {
        return a.value == b.value;
    }
};

struct Copyable {
    int value = 0;
    friend bool operator==(const Copyable&, const Copyable&) noexcept = default;
};

} // namespace

TEST(Optional_EmptyAndEngagedStates) {
    Optional<int> empty;
    CHECK(!empty.HasValue() && !empty && empty == NullOpt);
    CHECK(empty.TryGet() == nullptr);
    CHECK(empty.ValueOr(7) == 7);
    Optional<int> value = 5;
    CHECK(value.HasValue() && *value == 5 && value.Value() == 5);
    CHECK(value.ValueOr(7) == 5);
    *value.TryGet() = 9;
    CHECK(value.Value() == 9);
    value = NullOpt;
    CHECK(!value.HasValue());
    CHECK(Optional<int>(3) == Optional<int>(3));
    CHECK(!(Optional<int>(3) == Optional<int>(4)));
    CHECK(!(Optional<int>(3) == Optional<int>()));
    CHECK(Optional<int>() == Optional<int>());
}

TEST(Optional_MoveOnlyLifetime) {
    Tracked::alive = 0;
    Tracked::destroyed = 0;
    {
        Optional<Tracked> first(Tracked(1));
        CHECK(Tracked::alive == 1);
        Optional<Tracked> second(Move(first));
        // 이동 후 원본은 값을 보유한 채 이동 후 상태다(UE와 같다).
        CHECK(first.HasValue() && first->movedFrom && second->value == 1);
        CHECK(Tracked::alive == 2);
        second.Emplace(2);
        CHECK(second->value == 2 && Tracked::alive == 2);
        first.Reset();
        CHECK(!first.HasValue() && Tracked::alive == 1);
        first = Move(second);
        CHECK(first->value == 2 && second.HasValue());
        second = Optional<Tracked>();
        CHECK(!second.HasValue() && Tracked::alive == 1);
        // 이동 전용 T도 값 비교를 사용할 수 있다.
        CHECK(first == Optional<Tracked>(Tracked(2)));
        CHECK(!(first == Optional<Tracked>(Tracked(3))));
    }
    CHECK(Tracked::alive == 0);
}

TEST(Optional_EmplaceFromOwnValueIsSafe) {
    Optional<Copyable> value(Copyable{41});
    // 인자가 현재 값을 참조한다. 기존 값을 먼저 파괴하면 읽기 전에 수명이 끝난다.
    value.Emplace(value.Value());
    CHECK(value.Value().value == 41);
    Optional<Copyable>& alias = value;
    value = alias; // 자기 대입
    CHECK(value.Value().value == 41);
    Optional<Copyable> copy = value;
    CHECK(copy == value);
}

TEST(ScopeExit_RunsOnceAndCanBeDismissed) {
    int calls = 0;
    {
        auto guard = MakeScopeExit([&]() noexcept { ++calls; });
        CHECK(calls == 0);
    }
    CHECK(calls == 1);
    {
        auto guard = MakeScopeExit([&]() noexcept { ++calls; });
        guard.Dismiss();
    }
    CHECK(calls == 1);
    {
        ROCKS_ON_SCOPE_EXIT {
            calls += 10;
        };
        ROCKS_ON_SCOPE_EXIT {
            // 나중에 만든 guard가 먼저 실행된다.
            CHECK(calls == 1);
            calls *= 2;
        };
    }
    CHECK(calls == 12);
    const auto named = [&]() noexcept { ++calls; };
    {
        auto guard = MakeScopeExit(named); // lvalue 함수 객체는 복사된다.
    }
    CHECK(calls == 13);
}

namespace {

// 소멸자가 값을 지우는 타입이다. 기존 값을 먼저 파괴하는 잘못된 Emplace는 지워진 값을 복사하게
// 된다.
struct Poisoning {
    int value = 0;

    explicit Poisoning(int number) noexcept : value(number) {
    }

    Poisoning(const Poisoning& other) noexcept : value(other.value) {
    }

    Poisoning(Poisoning&& other) noexcept : value(other.value) {
    }

    ~Poisoning() noexcept {
        value = -1;
    }
};

#define ROCKS_TEST_TWO_GUARDS(counter)                                                             \
    ROCKS_ON_SCOPE_EXIT {                                                                          \
        counter += 1;                                                                              \
    };                                                                                             \
    ROCKS_ON_SCOPE_EXIT {                                                                          \
        counter += 2;                                                                              \
    }

} // namespace

TEST(Optional_EmplaceFromOwnValueWithObservableDestructor) {
    Optional<Poisoning> value(Poisoning(41));
    value.Emplace(value.Value());
    CHECK(value.Value().value == 41);
    value.Emplace(Move(value.Value()));
    CHECK(value.Value().value == 41);
}

TEST(ScopeExit_MultipleGuardsFromOneMacroExpansion) {
    int counter = 0;
    {
        // 한 줄로 전개되는 두 guard도 이름이 겹치지 않아야 한다(__COUNTER__ 사용).
        ROCKS_TEST_TWO_GUARDS(counter);
    }
    CHECK(counter == 3);
}

} // namespace rocks::test
