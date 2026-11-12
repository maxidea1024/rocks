#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;

namespace {

// 객체 수명과 이동 후 상태를 따로 관찰한다. 테스트 결과를 구현의 이동 횟수에 의존시키지 않는다.
struct Value {
    static inline int live = 0;
    static inline int constructed = 0;
    static inline int destroyed = 0;
    int value = 0;

    Value() noexcept {
        ++live;
        ++constructed;
    }
    explicit Value(int number) noexcept : value(number) {
        ++live;
        ++constructed;
    }
    Value(const Value& other) noexcept : value(other.value) {
        ++live;
        ++constructed;
    }
    Value(Value&& other) noexcept : value(other.value) {
        other.value = -1;
        ++live;
        ++constructed;
    }
    Value& operator=(const Value&) = delete;
    Value& operator=(Value&&) = delete;
    ~Value() noexcept {
        --live;
        ++destroyed;
    }
    bool operator==(const Value& other) const noexcept {
        return value == other.value;
    }
    static void ResetCounts() noexcept {
        live = 0;
        constructed = 0;
        destroyed = 0;
    }
};

struct MoveOnly {
    int value;
    explicit MoveOnly(int number) noexcept : value(number) {
    }
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly& operator=(MoveOnly&&) = delete;
    MoveOnly(MoveOnly&& other) noexcept : value(other.value) {
        other.value = -1;
    }
    ~MoveOnly() noexcept = default;
};

uint32 Next(uint32& state) noexcept {
    state = state * 1664525u + 1013904223u;
    return state;
}

} // namespace

TEST(ArrayInsertAliasedValuesWithAndWithoutGrowth) {
    Value::ResetCounts();
    SystemAllocator allocator;
    {
        Array<Value> values(allocator);
        REQUIRE(values.TryReserve(4));
        REQUIRE(values.TryEmplaceBack(10));
        REQUIRE(values.TryEmplaceBack(20));
        REQUIRE(values.TryEmplaceBack(30));
        auto* data = values.Data();
        REQUIRE(values.TryInsert(values[2], 1));
        CHECK(values.Data() == data);
        CHECK(values.Size() == 4);
        CHECK(values[0].value == 10 && values[1].value == 30 && values[2].value == 20 &&
              values[3].value == 30);
        REQUIRE(values.TryInsert(values[0], 2));
        CHECK(values.Size() == 5);
        CHECK(values[0].value == 10 && values[1].value == 30 && values[2].value == 10 &&
              values[3].value == 20 && values[4].value == 30);
        CHECK(Value::live == static_cast<int>(values.Size()));
    }
    CHECK(Value::live == 0 && Value::constructed == Value::destroyed);
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ArrayInsertFailureLeavesAliasedMoveSourceAndCapacity) {
    Value::ResetCounts();
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        Array<Value> values(allocator);
        REQUIRE(values.TryReserve(2));
        REQUIRE(values.TryEmplaceBack(5));
        REQUIRE(values.TryEmplaceBack(8));
        auto* original = values.Data();
        allocator.FailOnAttempt(1);
        auto result = values.TryInsert(Move(values[0]), 1);
        CHECK(!result && result.Error() == ErrorCode::OutOfMemory);
        CHECK(values.Data() == original && values.Size() == 2 && values.Capacity() == 2);
        CHECK(values[0].value == 5 && values[1].value == 8);
        CHECK(Value::live == 2);
        auto invalid = values.TryInsert(Move(values[0]), 3);
        CHECK(!invalid && invalid.Error() == ErrorCode::InvalidArgument);
        CHECK(values[0].value == 5 && allocator.Attempts() == 1);
        allocator.DisableFailure();
        REQUIRE(values.TryInsert(Move(values[0]), 1));
        CHECK(values[0].value == -1 && values[1].value == 5 && values[2].value == 8);
    }
    CHECK(Value::live == 0 && Value::constructed == Value::destroyed);
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(ArrayAppendSelfAndBorrowedPartialRanges) {
    Value::ResetCounts();
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        Array<Value> values(allocator);
        REQUIRE(values.TryReserve(3));
        REQUIRE(values.TryEmplaceBack(1));
        REQUIRE(values.TryEmplaceBack(2));
        REQUIRE(values.TryEmplaceBack(3));
        auto* original = values.Data();
        allocator.FailOnAttempt(1);
        auto failed = values.TryAppend(values);
        CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
        CHECK(values.Data() == original && values.Size() == 3);
        CHECK(values[0].value == 1 && values[1].value == 2 && values[2].value == 3);
        allocator.DisableFailure();
        REQUIRE(values.TryAppend(Span<const Value>(values.Data() + 1, 2)));
        CHECK(values.Size() == 5);
        CHECK(values[0].value == 1 && values[1].value == 2 && values[2].value == 3 &&
              values[3].value == 2 && values[4].value == 3);
        REQUIRE(values.TryReserve(12));
        original = values.Data();
        REQUIRE(values.TryAppend(values));
        CHECK(values.Data() == original && values.Size() == 10);
        for (usize i = 0; i < 5; ++i) {
            CHECK(values[i].value == values[i + 5].value);
        }
        CHECK(!values.TryAppend(Move(values)));
        CHECK(values.Size() == 10);
    }
    CHECK(Value::live == 0 && Value::constructed == Value::destroyed);
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(ArrayMoveAppendPreservesBothArraysOnFailure) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        Array<MoveOnly> source(allocator);
        Array<MoveOnly> destination(allocator);
        REQUIRE(destination.TryReserve(1));
        REQUIRE(destination.TryEmplaceBack(3));
        REQUIRE(source.TryEmplaceBack(7));
        REQUIRE(source.TryEmplaceBack(9));
        auto* oldSource = source.Data();
        auto* oldDestination = destination.Data();
        allocator.FailOnAttempt(1);
        auto failed = destination.TryAppend(Move(source));
        CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
        CHECK(source.Data() == oldSource && source.Size() == 2);
        CHECK(destination.Data() == oldDestination && destination.Size() == 1);
        CHECK(source[0].value == 7 && source[1].value == 9 && destination[0].value == 3);
        allocator.DisableFailure();
        REQUIRE(destination.TryAppend(Move(source)));
        CHECK(source.IsEmpty() && source.Data() == oldSource);
        CHECK(destination.Size() == 3);
        CHECK(destination[0].value == 3 && destination[1].value == 7 && destination[2].value == 9);
    }
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(ArrayResizeShrinkRemovePopAndFindContracts) {
    Value::ResetCounts();
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        Array<Value> values(allocator);
        REQUIRE(values.TryResize(3));
        CHECK(values.Size() == 3 && Value::live == 3);
        values[0].value = 1;
        values[1].value = 2;
        values[2].value = 1;
        Value target(1);
        CHECK(values.Find(target) == 0);
        CHECK(values.FindLast(target) == 2 && values.Contains(target));
        Value missing(99);
        CHECK(values.Find(missing) == Array<Value>::NotFound && !values.Contains(missing));
        auto* original = values.Data();
        const usize capacity = values.Capacity();
        allocator.FailOnAttempt(1);
        auto failed = values.TryResize(capacity + 10);
        CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
        CHECK(values.Data() == original && values.Size() == 3 && values[1].value == 2);
        allocator.DisableFailure();
        REQUIRE(values.TryResize(1));
        CHECK(values.Size() == 1 && values.Capacity() == capacity);
        REQUIRE(values.TryRemoveAt(values.Size(), 0));
        CHECK(!values.TryRemoveAt(values.Size(), 1));
        CHECK(!values.TryRemoveAtSwap(MaxSize, 1));
        allocator.FailOnAttempt(1);
        auto shrinkFailed = values.TryShrink();
        CHECK(!shrinkFailed && shrinkFailed.Error() == ErrorCode::OutOfMemory);
        CHECK(values.Data() == original && values.Capacity() == capacity);
        allocator.DisableFailure();
        REQUIRE(values.TryShrink());
        CHECK(values.Capacity() == 1 && values[0].value == 1);
        auto popped = values.TryPopBack();
        REQUIRE(popped);
        CHECK(popped.Value().value == 1 && values.IsEmpty());
        CHECK(!values.TryPopBack());
        values.Reset();
        CHECK(values.Capacity() == 1);
        values.Empty();
        CHECK(values.Capacity() == 0 && values.Data() == nullptr);
        CHECK(&values.GetAllocator() == &allocator);
        auto overflow = values.TryResize(MaxSize);
        CHECK(!overflow && overflow.Error() == ErrorCode::Overflow);
    }
    CHECK(Value::live == 0 && Value::constructed == Value::destroyed);
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(ArrayRemoveAtSwapEveryRangeMatchesIndependentModel) {
    SystemAllocator allocator;
    for (usize size = 0; size <= 16; ++size) {
        for (usize first = 0; first <= size; ++first) {
            for (usize count = 0; count <= size - first; ++count) {
                Array<int32> values(allocator);
                for (usize i = 0; i < size; ++i) {
                    REQUIRE(values.TryPushBack(static_cast<int32>(i)));
                }
                int32 expected[16]{};
                for (usize i = 0; i < size; ++i) {
                    expected[i] = static_cast<int32>(i);
                }
                const usize trailing = size - first - count;
                const usize moved = trailing < count ? trailing : count;
                for (usize i = 0; i < moved; ++i) {
                    expected[first + i] = static_cast<int32>(size - moved + i);
                }
                REQUIRE(values.TryRemoveAtSwap(first, count));
                CHECK(values.Size() == size - count);
                for (usize i = 0; i < values.Size(); ++i) {
                    CHECK(values[i] == expected[i]);
                }
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ArrayRandomizedOperationsMatchIndependentFixedArrayOracle) {
    SystemAllocator allocator;
    Array<int32> actual(allocator);
    int32 expected[128]{};
    usize count = 0;
    uint32 random = 0x5a17e89u;
    for (usize step = 0; step < 5000; ++step) {
        uint32 op = Next(random) % 5;
        if (count >= 96) {
            op = 2;
        }
        if (op == 0) {
            const int32 value = static_cast<int32>(Next(random) % 10000);
            REQUIRE(actual.TryPushBack(value));
            expected[count++] = value;
        } else if (op == 1) {
            const usize position = Next(random) % (count + 1);
            const int32 value = static_cast<int32>(Next(random) % 10000);
            REQUIRE(actual.TryInsert(value, position));
            for (usize i = count; i > position; --i) {
                expected[i] = expected[i - 1];
            }
            expected[position] = value;
            ++count;
        } else if (op == 2 && count != 0) {
            const usize position = Next(random) % count;
            const usize remove = Next(random) % (count - position + 1);
            REQUIRE(actual.TryRemoveAt(position, remove));
            for (usize i = position; i + remove < count; ++i) {
                expected[i] = expected[i + remove];
            }
            count -= remove;
        } else if (op == 3 && count != 0) {
            const usize position = Next(random) % count;
            const usize remove = Next(random) % (count - position + 1);
            const usize trailing = count - position - remove;
            const usize moved = trailing < remove ? trailing : remove;
            for (usize i = 0; i < moved; ++i) {
                expected[position + i] = expected[count - moved + i];
            }
            REQUIRE(actual.TryRemoveAtSwap(position, remove));
            count -= remove;
        } else if (op == 4) {
            const usize newCount = Next(random) % 40;
            REQUIRE(actual.TryResize(newCount));
            for (usize i = count; i < newCount; ++i) {
                expected[i] = 0;
            }
            count = newCount;
        }
        CHECK(actual.Size() == count);
        for (usize i = 0; i < count; ++i) {
            CHECK(actual[i] == expected[i]);
        }
    }
}
