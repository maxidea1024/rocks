#include <Core/Algorithms.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;

namespace {

struct Descending {
    bool operator()(int left, int right) const noexcept {
        return left > right;
    }
};

struct MoveOnly {
    static inline int live = 0;
    static inline int created = 0;
    static inline int destroyed = 0;
    int key;
    int serial;

    MoveOnly(int keyValue, int serialValue) noexcept : key(keyValue), serial(serialValue) {
        ++live;
        ++created;
    }
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly& operator=(MoveOnly&&) = delete;
    // 주소 연산자를 덮어쓴 원소도 실제 storage 주소를 사용해 교환해야 한다.
    MoveOnly* operator&() noexcept {
        return nullptr;
    }
    MoveOnly(MoveOnly&& other) noexcept : key(other.key), serial(other.serial) {
        other.key = -1;
        other.serial = -1;
        ++live;
        ++created;
    }
    ~MoveOnly() noexcept {
        --live;
        ++destroyed;
    }
};

struct KeyCompare {
    bool operator()(const MoveOnly& left, const MoveOnly& right) const noexcept {
        return left.key < right.key;
    }
    bool operator()(const MoveOnly& left, int right) const noexcept {
        return left.key < right;
    }
    bool operator()(int left, const MoveOnly& right) const noexcept {
        return left < right.key;
    }
};

struct DeletedAddress {
    int value;
    explicit DeletedAddress(int number) noexcept : value(number) {
    }
    DeletedAddress(const DeletedAddress&) = delete;
    DeletedAddress& operator=(const DeletedAddress&) = delete;
    DeletedAddress& operator=(DeletedAddress&&) = delete;
    DeletedAddress(DeletedAddress&&) noexcept = default;
    DeletedAddress* operator&() = delete;
};

uint32 Next(uint32& state) noexcept {
    state = state * 1664525u + 1013904223u;
    return state;
}

} // namespace

TEST(AlgorithmsEmptySingletonAndBounds) {
    Span<int> empty;
    algo::Sort(empty);
    CHECK(algo::LowerBound(empty, 7) == 0);
    CHECK(algo::UpperBound(empty, 7) == 0);
    CHECK(algo::BinarySearch(empty, 7) == algo::NotFound);

    int values[] = {7};
    Span<int> single(values);
    algo::Sort(single);
    CHECK(values[0] == 7);
    CHECK(algo::LowerBound(single, 6) == 0);
    CHECK(algo::UpperBound(single, 6) == 0);
    CHECK(algo::LowerBound(single, 7) == 0);
    CHECK(algo::UpperBound(single, 7) == 1);
    CHECK(algo::BinarySearch(single, 7) == 0);
    CHECK(algo::LowerBound(single, 8) == 1);
    CHECK(algo::UpperBound(single, 8) == 1);
    CHECK(algo::BinarySearch(single, 8) == algo::NotFound);
}

TEST(AlgorithmsDescendingAndDuplicateBounds) {
    int values[] = {2, 4, 4, -1, 3, 4, 0};
    Descending compare;
    algo::Sort(Span<int>(values), compare);
    const int expected[] = {4, 4, 4, 3, 2, 0, -1};
    for (usize index = 0; index < 7; ++index) {
        CHECK(values[index] == expected[index]);
    }
    Span<const int> readOnly(values, 7);
    CHECK(algo::LowerBound(readOnly, 4, compare) == 0);
    CHECK(algo::UpperBound(readOnly, 4, compare) == 3);
    CHECK(algo::BinarySearch(readOnly, 4, compare) == 0);
    CHECK(algo::LowerBound(readOnly, 1, compare) == 5);
    CHECK(algo::UpperBound(readOnly, 1, compare) == 5);
    CHECK(algo::BinarySearch(readOnly, 1, compare) == algo::NotFound);
}

TEST(AlgorithmsMoveOnlyLifetimeAndHeterogeneousSearch) {
    MoveOnly::live = MoveOnly::created = MoveOnly::destroyed = 0;
    {
        MoveOnly values[] = {{3, 0}, {1, 1}, {3, 2}, {2, 3}, {1, 4}};
        KeyCompare compare;
        algo::Sort(Span<MoveOnly>(values), compare);
        CHECK(MoveOnly::live == 5);
        bool seen[] = {false, false, false, false, false};
        for (usize i = 0; i < 5; ++i) {
            if (i > 0) {
                CHECK(values[i - 1].key <= values[i].key);
            }
            REQUIRE(values[i].serial >= 0 && values[i].serial < 5);
            CHECK(!seen[values[i].serial]);
            seen[values[i].serial] = true;
        }
        Span<const MoveOnly> readOnly(values, 5);
        CHECK(algo::LowerBound(readOnly, 1, compare) == 0);
        CHECK(algo::UpperBound(readOnly, 1, compare) == 2);
        CHECK(algo::BinarySearch(readOnly, 2, compare) == 2);
        CHECK(algo::BinarySearch(readOnly, 7, compare) == algo::NotFound);
    }
    CHECK(MoveOnly::live == 0);
    CHECK(MoveOnly::created == MoveOnly::destroyed);
}

TEST(AlgorithmsSortWithDeletedAddressOperator) {
    DeletedAddress values[] = {DeletedAddress(7), DeletedAddress(2), DeletedAddress(4)};
    const auto less = [](const DeletedAddress& left, const DeletedAddress& right) noexcept {
        return left.value < right.value;
    };
    algo::Sort(Span<DeletedAddress>(values), less);
    CHECK(values[0].value == 2 && values[1].value == 4 && values[2].value == 7);
}

TEST(AlgorithmsRandomizedSortAndSearchAgainstIndependentOracle) {
    uint32 state = 0x953a71u;
    for (usize count = 0; count <= 128; ++count) {
        for (usize iteration = 0; iteration < 12; ++iteration) {
            int actual[128] = {};
            int expected[128] = {};
            for (usize i = 0; i < count; ++i) {
                const int value = static_cast<int>(Next(state) % 37) - 18;
                actual[i] = value;
                expected[i] = value;
            }
            // 단순 삽입 정렬과 순차 검색을 독립적인 기준으로 사용한다.
            for (usize i = 1; i < count; ++i) {
                const int value = expected[i];
                usize j = i;
                while (j != 0 && expected[j - 1] > value) {
                    expected[j] = expected[j - 1];
                    --j;
                }
                expected[j] = value;
            }
            Span<int> view(actual, count);
            algo::Sort(view);
            for (usize i = 0; i < count; ++i) {
                CHECK(actual[i] == expected[i]);
            }
            for (int key = -20; key <= 20; ++key) {
                usize lower = 0;
                while (lower < count && expected[lower] < key) {
                    ++lower;
                }
                usize upper = lower;
                while (upper < count && expected[upper] == key) {
                    ++upper;
                }
                CHECK(algo::LowerBound(view, key) == lower);
                CHECK(algo::UpperBound(view, key) == upper);
                CHECK(algo::BinarySearch(view, key) == (lower == upper ? algo::NotFound : lower));
            }
        }
    }
}

TEST(AlgorithmsSortComparatorCanComparePointersByPointee) {
    int elements[] = {9, 3, 5};
    int* pointers[] = {&elements[0], &elements[1], &elements[2]};
    const auto byValue = [](const int* left, const int* right) noexcept { return *left < *right; };
    algo::Sort(Span<int*>(pointers), byValue);
    CHECK(*pointers[0] == 3 && *pointers[1] == 5 && *pointers[2] == 9);
}

TEST(AlgorithmsSortAndSearchArrayViews) {
    SystemAllocator allocator;
    {
        Array<int> values(allocator);
        REQUIRE(values.TryPushBack(9));
        REQUIRE(values.TryPushBack(-2));
        REQUIRE(values.TryPushBack(9));
        REQUIRE(values.TryPushBack(4));
        values.Sort();
        CHECK(values[0] == -2 && values[1] == 4 && values[2] == 9 && values[3] == 9);
        const Array<int>& readOnly = values;
        CHECK(algo::LowerBound(readOnly.View(), 9) == 2);
        CHECK(algo::UpperBound(readOnly.View(), 9) == 4);
        CHECK(algo::BinarySearch(readOnly.View(), 5) == algo::NotFound);
        values.Sort(Descending{});
        CHECK(values[0] == 9 && values[1] == 9 && values[2] == 4 && values[3] == -2);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(AlgorithmsSortWorstCaseComparisonBound) {
    constexpr usize count = 513;
    int values[count];
    usize comparisons = 0;
    auto counted = [&comparisons](int left, int right) noexcept {
        ++comparisons;
        return left < right;
    };
    for (int shape = 0; shape < 4; ++shape) {
        for (usize i = 0; i < count; ++i) {
            values[i] = shape == 0   ? static_cast<int>(i)
                        : shape == 1 ? static_cast<int>(count - i)
                        : shape == 2 ? 1
                                     : static_cast<int>(i % 7);
        }
        comparisons = 0;
        algo::Sort(Span<int>(values), counted);
        for (usize i = 1; i < count; ++i) {
            CHECK(values[i - 1] <= values[i]);
        }
        CHECK(comparisons < 32 * count);
    }
}
