#include <Core/Containers/Array.h>
#include <Core/Containers/HashMap.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Containers/SlotArray.h>
#include <Core/Containers/SparseArray.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Optional.h>
#include <Core/Variant.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 컨테이너 최적화(ConstructAt/DestroyAt 제약, InitializeAt, trivial 범위 경로, UE slack
// 성장·축소)의 계약 시험이다. BitArray의 word 경로는 BitArrayWordTests.cpp에 있다.

using namespace rocks;
namespace container_optimization_tests_file {

namespace {

template <typename T, typename... Args>
concept CanConstructAt =
    requires(void* p, Args&&... args) { ConstructAt<T>(p, Forward<Args>(args)...); };

template <typename T>
concept CanDestroyAt = requires(T* p) { DestroyAt(p); };

// 소멸은 trivial이지만 이동 생성은 사용자 정의다. 범위 경로가 이동을 byte 복사로 생략하면 moves가
// 기대값보다 작아진다.
struct CustomMove {
    static inline int moves = 0;
    int value;

    explicit CustomMove(int v) noexcept : value(v) {
    }

    CustomMove(CustomMove&& other) noexcept : value(other.value) {
        ++moves;
    }
};

struct Owned {
    static inline int alive = 0;
    int value;

    explicit Owned(int v) noexcept : value(v) {
        ++alive;
    }

    Owned(Owned&& other) noexcept : value(other.value) {
        ++alive;
    }

    ~Owned() noexcept {
        --alive;
    }
};

// 변환 생성자의 부작용(값 변환)이 InitializeAt에서도 보존되는지 본다.
struct Doubled {
    int value;

    Doubled(int v) noexcept : value(v * 2) {
    }
};

struct OwnedHash {
    uint64 operator()(const Owned& item) const noexcept {
        return static_cast<uint64>(static_cast<uint32>(item.value)) * 0x9E3779B97F4A7C15ull;
    }
};

struct OwnedEqual {
    bool operator()(const Owned& left, const Owned& right) const noexcept {
        return left.value == right.value;
    }
};

static_assert(!CanConstructAt<int>);
static_assert(!CanConstructAt<int, int>);
static_assert(!CanConstructAt<CustomMove*, CustomMove*>);
static_assert(!CanDestroyAt<int>);
static_assert(!CanDestroyAt<CustomMove>);
static_assert(CanConstructAt<Owned, int>);
static_assert(CanDestroyAt<Owned>);
static_assert(CanConstructAt<CustomMove, CustomMove&&>);
static_assert(CanConstructAt<Doubled, int>);
static_assert(IsTriviallyDestructibleV<CustomMove>);
static_assert(IsTriviallyDestructibleV<Optional<int>>);
static_assert(IsTriviallyDestructibleV<Result<int>>);
static_assert(IsTriviallyDestructibleV<Variant<int, float>>);
static_assert(!IsTriviallyDestructibleV<Optional<Owned>>);
static_assert(!IsTriviallyDestructibleV<Result<Owned>>);
static_assert(!IsTriviallyDestructibleV<Variant<int, Owned>>);

// UE 5.8 DefaultCalculateSlackGrow의 대표값을 독립 상수로 검사한다.
static_assert(detail::GrowthCapacityWithin(0, 1, 1000) == 4);
static_assert(detail::GrowthCapacityWithin(0, 64, 1000) == 64);
static_assert(detail::GrowthCapacityWithin(0, 2, 3) == 2);
static_assert(detail::GrowthCapacityWithin(4, 5, 1000) == 22);
static_assert(detail::GrowthCapacityWithin(22, 23, 1000) == 47);
static_assert(detail::GrowthCapacityWithin(1, 1000, 2000) == 1391);
// 성장은 지금 용량이 아니라 required 기준이다.
static_assert(detail::GrowthCapacityWithin(900, 1000, 2000) == 1391);
// 컨테이너 한도에서 포화하고, 곱셈·덧셈 overflow 없이 판정한다.
static_assert(detail::GrowthCapacityWithin(100, 101, 110) == 110);
static_assert(detail::GrowthCapacityWithin(1, 1000, 1391) == 1391);
static_assert(detail::GrowthCapacityWithin(1, 1000, 1390) == 1390);
static_assert(detail::GrowthCapacityWithin(8, NumericLimits<usize>::Max() - 8,
                                           NumericLimits<usize>::Max()) ==
              NumericLimits<usize>::Max());
static_assert(detail::GrowthCapacityWithin(8, NumericLimits<usize>::Max() / 4 * 3,
                                           NumericLimits<usize>::Max()) ==
              NumericLimits<usize>::Max());

// 축소: (여유 byte >= 16KiB 또는 3 × required < 2 × capacity) 그리고 (여유 > 64 또는 빈 컨테이너).
static_assert(detail::ShrinkCapacity(192, 128, 1) == 192);
static_assert(detail::ShrinkCapacity(192, 127, 1) == 127);
static_assert(detail::ShrinkCapacity(300, 199, 1) == 199);
static_assert(detail::ShrinkCapacity(300, 200, 1) == 300);
static_assert(detail::ShrinkCapacity(164, 100, 1) == 164);
static_assert(detail::ShrinkCapacity(165, 100, 1) == 100);
static_assert(detail::ShrinkCapacity(66384, 50000, 1) == 50000);
static_assert(detail::ShrinkCapacity(66383, 50000, 1) == 66383);
static_assert(detail::ShrinkCapacity(1000, 900, 163) == 1000);
static_assert(detail::ShrinkCapacity(1000, 900, 164) == 900);
static_assert(detail::ShrinkCapacity(4, 0, 1) == 0);
static_assert(detail::ShrinkCapacity(10, 10, 1) == 10);
static_assert(detail::ShrinkCapacity(NumericLimits<usize>::Max(), NumericLimits<usize>::Max() - 1,
                                     NumericLimits<usize>::Max()) == NumericLimits<usize>::Max());

template <typename T>
T MakeItem(int value) noexcept {
    if constexpr (IsSameV<T, Owned>) {
        return Owned(value);
    } else {
        return value;
    }
}

int ItemValue(const Owned& item) noexcept {
    return item.value;
}

int ItemValue(int item) noexcept {
    return item;
}

// K와 V의 소멸 필요성을 각각 판정하는지 본다. trivial 쪽은 소멸하지 않고, non-trivial 쪽은 삽입·
// 성장·삭제·덮어쓰기·압축·Clear·Reset·파괴에서 정확히 한 번씩 소멸한다.
template <typename Map, typename K, typename V>
void CheckMapLifetime(Allocator& allocator) {
    constexpr int PerEntry = (IsSameV<K, Owned> ? 1 : 0) + (IsSameV<V, Owned> ? 1 : 0);
    Owned::alive = 0;
    {
        Map map(allocator);
        for (int i = 0; i < 40; ++i) {
            REQUIRE(map.Insert(MakeItem<K>(i), MakeItem<V>(i * 3)));
        }

        CHECK(map.Count() == 40);
        CHECK(Owned::alive == 40 * PerEntry);
        for (int i = 0; i < 40; i += 4) {
            CHECK(map.Erase(MakeItem<K>(i)));
        }

        CHECK(Owned::alive == 30 * PerEntry);
        REQUIRE(map.InsertOrAssign(MakeItem<K>(1), MakeItem<V>(1000)));
        CHECK(Owned::alive == 30 * PerEntry);
        REQUIRE(map.ShrinkSlack());
        REQUIRE(map.Compact());
        CHECK(Owned::alive == 30 * PerEntry);
        for (int i = 0; i < 40; ++i) {
            const auto* found = map.Find(MakeItem<K>(i));
            if (i % 4 == 0) {
                CHECK(found == nullptr);
            } else {
                REQUIRE(found != nullptr);
                CHECK(ItemValue(*found) == (i == 1 ? 1000 : i * 3));
            }
        }

        map.Clear();
        CHECK(map.Count() == 0);
        CHECK(Owned::alive == 0);
        for (int i = 0; i < 5; ++i) {
            REQUIRE(map.Insert(MakeItem<K>(i), MakeItem<V>(i)));
        }

        CHECK(Owned::alive == 5 * PerEntry);
        map.Reset();
        CHECK(Owned::alive == 0);
        REQUIRE(map.Insert(MakeItem<K>(7), MakeItem<V>(8)));
        CHECK(Owned::alive == PerEntry);
    }

    CHECK(Owned::alive == 0);
}

} // namespace

TEST(ContainerOptimizationInitializationMovementAndLifetime) {
    SystemAllocator allocator;
    {
        alignas(Doubled) unsigned char storage[sizeof(Doubled) + sizeof(int)];
        int* value = InitializeAt<int>(storage);
        CHECK(*value == 0);
        value = InitializeAt<int>(storage, 73);
        CHECK(*value == 73);
        int evaluated = 0;
        auto next = [&evaluated]() noexcept { return ++evaluated; };

        value = InitializeAt<int>(storage, next());
        CHECK(evaluated == 1 && *value == 1);
        Doubled* doubled = InitializeAt<Doubled>(storage, 21);
        CHECK(doubled->value == 42);
        CustomMove source(5);
        CustomMove::moves = 0;
        CustomMove* moved = InitializeAt<CustomMove>(storage, Move(source));
        CHECK(CustomMove::moves == 1 && moved->value == 5);

        Array<int> values(allocator);
        REQUIRE(values.TryResize(130));
        for (int item : values) {
            CHECK(item == 0);
        }

        for (usize i = 0; i < values.Size(); ++i) {
            values[i] = static_cast<int>(i);
        }

        REQUIRE(values.TryEmplaceAt(63, values[100]));
        CHECK(values[63] == 100 && values[64] == 63);
        REQUIRE(values.TryRemoveAt(1, 64));
        CHECK(values[0] == 0 && values[1] == 64 && values.Size() == 67);
        REQUIRE(values.TryAppend(values.View()));
        CHECK(values.Size() == 134 && values[67] == 0 && values[68] == 64);

        Array<CustomMove> custom(allocator);
        REQUIRE(custom.TryReserve(4));
        REQUIRE(custom.TryEmplaceBack(17));
        CustomMove::moves = 0;
        REQUIRE(custom.TryReserve(8));
        CHECK(CustomMove::moves == 1 && custom[0].value == 17);

        Array<Owned> owned(allocator);
        REQUIRE(owned.TryEmplaceBack(5));
        REQUIRE(owned.TryReserve(16));
        CHECK(Owned::alive == 1);
        owned.Clear();
        CHECK(Owned::alive == 0);

        HashMap<int, Owned> hash(allocator);
        OrderedMap<int, Owned> ordered(allocator);
        REQUIRE(hash.Insert(1, Owned(7)));
        REQUIRE(ordered.Insert(2, Owned(8)));
        CHECK(Owned::alive == 2);
        hash.Reset();
        ordered.Reset();
        CHECK(Owned::alive == 0);

        SparseArray<Owned> sparse(allocator);
        SlotArray<Owned> slots(allocator);
        REQUIRE(sparse.TryEmplace(9));
        REQUIRE(slots.TryEmplace(10));
        CHECK(Owned::alive == 2);
    }

    CHECK(Owned::alive == 0);
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 소멸이 trivial이어도 사용자 정의 이동 생성자는 삽입·순서 삭제·swap 삭제·재할당에서 원소마다
// 불린다.
TEST(ContainerOptimizationCustomMoveIsNotSkipped) {
    SystemAllocator allocator;
    Array<CustomMove> values(allocator);
    REQUIRE(values.TryReserve(8));
    for (int i = 0; i < 4; ++i) {
        REQUIRE(values.TryEmplaceBack(i));
    }

    CustomMove::moves = 0;
    REQUIRE(values.TryEmplaceAt(0, 99));
    CHECK(CustomMove::moves == 5);
    CHECK(values[0].value == 99 && values[1].value == 0 && values[4].value == 3);

    CustomMove::moves = 0;
    REQUIRE(values.TryRemoveAt(0));
    CHECK(CustomMove::moves == 4);
    CHECK(values[0].value == 0 && values[3].value == 3);

    CustomMove::moves = 0;
    REQUIRE(values.TryRemoveAtSwap(0));
    CHECK(CustomMove::moves == 1);
    CHECK(values.Size() == 3 && values[0].value == 3 && values[1].value == 1);

    for (int i = 0; i < 5; ++i) {
        REQUIRE(values.TryEmplaceBack(10 + i));
    }

    REQUIRE(values.Size() == values.Capacity());
    CustomMove::moves = 0;
    REQUIRE(values.TryEmplaceAt(1, 77));
    CHECK(CustomMove::moves == 8);
    CHECK(values[0].value == 3 && values[1].value == 77 && values[2].value == 1);
}

// byte 복사 경로에서도 자기 원소 삽입·자기 배열 append(재할당 포함)·중첩 이동이 안전하고, 할당
// 실패는 원소·크기·용량·주소와 입력을 보존한다.
TEST(ContainerOptimizationArrayAliasingAndFailurePreservesState) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        Array<int32> values(allocator);
        REQUIRE(values.TryReserve(8));
        for (int32 i = 0; i < 8; ++i) {
            REQUIRE(values.TryPushBack(i * 10));
        }

        REQUIRE(values.Size() == values.Capacity());
        REQUIRE(values.TryEmplaceAt(2, values[7]));
        const int32 expectedInsert[] = {0, 10, 70, 20, 30, 40, 50, 60, 70};
        REQUIRE(values.Size() == 9);
        for (usize i = 0; i < 9; ++i) {
            CHECK(values[i] == expectedInsert[i]);
        }

        REQUIRE(values.TryEmplaceAt(1, values[6]));
        CHECK(values[1] == 50 && values[2] == 10 && values[9] == 70);

        Array<int32> doubled(allocator);
        REQUIRE(doubled.TryReserve(4));
        for (int32 i = 1; i <= 4; ++i) {
            REQUIRE(doubled.TryPushBack(i));
        }

        REQUIRE(doubled.TryAppend(doubled.View()));
        const int32 expectedAppend[] = {1, 2, 3, 4, 1, 2, 3, 4};
        REQUIRE(doubled.Size() == 8);
        for (usize i = 0; i < 8; ++i) {
            CHECK(doubled[i] == expectedAppend[i]);
        }

        REQUIRE(doubled.TryRemoveAt(1, 2));
        CHECK(doubled.Size() == 6 && doubled[0] == 1 && doubled[1] == 4 && doubled[2] == 1);
        REQUIRE(doubled.TryRemoveAtSwap(0, 2));
        CHECK(doubled.Size() == 4 && doubled[0] == 3 && doubled[1] == 4 && doubled[2] == 1);
    }

    {
        Array<int32> full(allocator);
        REQUIRE(full.TryReserve(4));
        for (int32 i = 1; i <= 4; ++i) {
            REQUIRE(full.TryPushBack(i));
        }

        Array<int32> source(allocator);
        REQUIRE(source.TryPushBack(9));
        const int32 external[] = {5, 6, 7};
        const int32* data = full.Data();
        auto unchanged = [&]() {
            CHECK(full.Data() == data && full.Size() == 4 && full.Capacity() == 4);
            CHECK(full[0] == 1 && full[1] == 2 && full[2] == 3 && full[3] == 4);
            CHECK(external[0] == 5 && external[1] == 6 && external[2] == 7);
            CHECK(source.Size() == 1 && source[0] == 9);
        };

        allocator.FailOnAttempt(1);
        auto inserted = full.TryEmplaceAt(1, full[3]);
        CHECK(!inserted && inserted.Error() == ErrorCode::OutOfMemory);
        unchanged();
        allocator.FailOnAttempt(1);
        auto pushed = full.TryPushBack(full[0]);
        CHECK(!pushed && pushed.Error() == ErrorCode::OutOfMemory);
        unchanged();
        allocator.FailOnAttempt(1);
        auto selfAppended = full.TryAppend(full.View());
        CHECK(!selfAppended && selfAppended.Error() == ErrorCode::OutOfMemory);
        unchanged();
        allocator.FailOnAttempt(1);
        auto appended = full.TryAppend(Span<const int32>(external, 3));
        CHECK(!appended && appended.Error() == ErrorCode::OutOfMemory);
        unchanged();
        allocator.FailOnAttempt(1);
        auto moved = full.TryAppend(Move(source));
        CHECK(!moved && moved.Error() == ErrorCode::OutOfMemory);
        unchanged();
        allocator.FailOnAttempt(1);
        auto resized = full.TryResize(5);
        CHECK(!resized && resized.Error() == ErrorCode::OutOfMemory);
        unchanged();
        allocator.FailOnAttempt(1);
        auto uninitialized = full.TryAppendUninitialized(1);
        CHECK(!uninitialized && uninitialized.Error() == ErrorCode::OutOfMemory);
        unchanged();
        allocator.DisableFailure();

        // 빈 범위는 null 포인터로도 아무 일 없이 성공한다.
        Array<int32> empty(allocator);
        REQUIRE(empty.TryAppend(Span<const int32>()));
        REQUIRE(empty.TryResize(0));
        auto none = empty.TryAppendUninitialized(0);
        REQUIRE(none);
        CHECK(none.Value().Size() == 0);
        REQUIRE(empty.TryRemoveAt(0, 0));
        CHECK(empty.Data() == nullptr && empty.Capacity() == 0);
    }

    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(ContainerOptimizationMapKeyValueLifetimeIsIndependent) {
    SystemAllocator allocator;
    CheckMapLifetime<HashMap<int, int>, int, int>(allocator);
    CheckMapLifetime<HashMap<int, Owned>, int, Owned>(allocator);
    CheckMapLifetime<HashMap<Owned, int, OwnedHash, OwnedEqual>, Owned, int>(allocator);
    CheckMapLifetime<HashMap<Owned, Owned, OwnedHash, OwnedEqual>, Owned, Owned>(allocator);
    CheckMapLifetime<OrderedMap<int, int>, int, int>(allocator);
    CheckMapLifetime<OrderedMap<int, Owned>, int, Owned>(allocator);
    CheckMapLifetime<OrderedMap<Owned, int, OwnedHash, OwnedEqual>, Owned, int>(allocator);
    CheckMapLifetime<OrderedMap<Owned, Owned, OwnedHash, OwnedEqual>, Owned, Owned>(allocator);
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ContainerOptimizationShrinkSlackFailureAndHysteresis) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        Array<int> values(allocator);
        REQUIRE(values.TryReserve(192));
        REQUIRE(values.TryResize(128));
        values[0] = 123;
        int* original = values.Data();
        REQUIRE(values.TryShrinkSlack());
        CHECK(values.Capacity() == 192 && values.Data() == original);
        REQUIRE(values.TryResize(127));
        allocator.FailOnAttempt(1);
        auto shrunk = values.TryShrinkSlack();
        CHECK(!shrunk && values.Size() == 127 && values.Capacity() == 192);
        CHECK(values.Data() == original && values[0] == 123);
        allocator.DisableFailure();
        REQUIRE(values.TryShrinkSlack());
        CHECK(values.Capacity() == 127 && values[0] == 123);

        // 비어 있으면 여유가 64개 이하라도 반환한다.
        Array<int> empty(allocator);
        REQUIRE(empty.TryReserve(10));
        REQUIRE(empty.TryShrinkSlack());
        CHECK(empty.Capacity() == 0 && empty.Data() == nullptr);
    }

    CHECK(backing.Stats().liveAllocations == 0);
}

// 맵은 open addressing의 power-of-two 슬롯과 부하율 1/2를 유지하며 축소한다. SparseArray는 살아
// 있는 index를 바꾸지 않고 끝의 빈 slot만 회수한다.
TEST(ContainerOptimizationShrinkSlackForMapsAndSparseArray) {
    SystemAllocator allocator;
    {
        HashMap<int32, int32> map(allocator);
        for (int32 i = 0; i < 1000; ++i) {
            REQUIRE(map.Insert(i, i));
        }

        for (int32 i = 10; i < 1000; ++i) {
            CHECK(map.Erase(i));
        }

        REQUIRE(map.ShrinkSlack());
        CHECK(map.SlotCount() == 32 && map.Capacity() == 16);
        for (int32 i = 0; i < 10; ++i) {
            REQUIRE(map.Find(i) != nullptr);
            CHECK(*map.Find(i) == i);
        }

        REQUIRE(map.ShrinkSlack());
        CHECK(map.SlotCount() == 32);
        map.Clear();
        REQUIRE(map.ShrinkSlack());
        CHECK(map.SlotCount() == 0);
    }

    {
        OrderedMap<int32, int32> ordered(allocator);
        for (int32 i = 0; i < 200; ++i) {
            REQUIRE(ordered.Insert(i, i * 2));
        }

        const usize before = ordered.Capacity();
        for (int32 i = 10; i < 200; ++i) {
            CHECK(ordered.Erase(i));
        }

        REQUIRE(ordered.ShrinkSlack());
        CHECK(ordered.Capacity() < before && ordered.Capacity() >= 10);
        int32 expected = 0;
        ordered.ForEach([&expected](const int32& key, int32& value) noexcept {
            CHECK(key == expected && value == expected * 2);
            ++expected;
        });

        CHECK(expected == 10);
    }

    {
        SparseArray<int32> sparse(allocator);
        for (int32 i = 0; i < 200; ++i) {
            REQUIRE(sparse.TryAdd(i));
        }

        for (usize i = 50; i < 200; ++i) {
            REQUIRE(sparse.TryRemoveAt(i));
        }

        REQUIRE(sparse.TryShrinkSlack());
        CHECK(sparse.Capacity() == 50);
        for (usize i = 0; i < 40; ++i) {
            REQUIRE(sparse.TryRemoveAt(i));
        }

        REQUIRE(sparse.TryShrinkSlack());
        CHECK(sparse.Capacity() == 50 && sparse.Size() == 10);
        for (usize i = 40; i < 50; ++i) {
            CHECK(sparse.IsValidIndex(i) && sparse[i] == static_cast<int32>(i));
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
} // namespace container_optimization_tests_file
