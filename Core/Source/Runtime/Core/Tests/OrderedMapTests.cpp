#include <Base/Types.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Math/RandomStream.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <TestSupport/HostFailure.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;

namespace {

// 모든 키를 한 색인 칸에서 시작하게 해 충돌 사슬·삭제 칸 뒤 탐색을 검증한다.
struct CollisionHash {
    usize operator()(int32) const noexcept {
        return 0;
    }
};

// 해시는 키마다 다르지만 낮은 bit가 모두 같아 색인 mask 안에서는 전부 충돌한다. 저장한 해시의
// 높은 bit 비교와 탐사가 함께 맞아야 한다.
struct HighBitsHash {
    uint64 operator()(int32 key) const noexcept {
        return static_cast<uint64>(static_cast<uint32>(key)) << 40;
    }
};

// 첫 칸이 색인의 마지막 칸이라 탐사가 처음으로 돌아가야 한다.
struct WraparoundHash {
    usize operator()(int32) const noexcept {
        return MaxSize;
    }
};

// 해시 순서가 키 순서와 반대가 되게 한다. 순회 순서가 해시와 무관함을 본다.
struct ReverseHash {
    uint64 operator()(int32 key) const noexcept {
        return NumericLimits<uint64>::Max() - static_cast<uint64>(static_cast<uint32>(key));
    }
};

struct ModuloHash {
    uint64 operator()(int32 key) const noexcept {
        return static_cast<uint64>(key % 7);
    }
};

struct SaltedModuloHash {
    int32 salt;
    int32 modulus;

    usize operator()(int32 value) const noexcept {
        return static_cast<usize>((value % modulus) ^ salt);
    }
};

struct ModuloEqual {
    int32 modulus;

    bool operator()(int32 left, int32 right) const noexcept {
        return left % modulus == right % modulus;
    }
};

struct alignas(64) TrackedValue {
    static inline int32 live = 0;
    int32 value;

    explicit TrackedValue(int32 number) noexcept : value(number) {
        ++live;
    }

    TrackedValue(const TrackedValue&) = delete;
    TrackedValue& operator=(const TrackedValue&) = delete;

    TrackedValue(TrackedValue&& other) noexcept : value(other.value) {
        other.value = -1;
        ++live;
    }

    ~TrackedValue() noexcept {
        --live;
    }
};

// Map의 키 순서(ForEach와 반복자 둘 다)가 expected와 같은지 본다.
template <typename Map>
bool HasKeySequence(const Map& map, const int32* expected, usize count) {
    if (map.Count() != count) {
        return false;
    }

    bool same = true;
    usize visited = 0;
    map.ForEach([&](const int32& key, const auto&) noexcept {
        if (visited >= count || key != expected[visited]) {
            same = false;
        }

        ++visited;
    });
    usize iterated = 0;
    for (auto entry : map) {
        if (iterated >= count || entry.key != expected[iterated]) {
            same = false;
        }

        ++iterated;
    }

    return same && visited == count && iterated == count;
}

template <typename Map, usize N>
bool HasKeys(const Map& map, const int32 (&expected)[N]) {
    return HasKeySequence(map, expected, N);
}

} // namespace

TEST(OrderedMap_IterationFollowsInsertionOrderNotHashOrder) {
    SystemAllocator allocator;
    {
        OrderedMap<int32, int32, ReverseHash> map(allocator);
        CHECK(map.IsEmpty());
        CHECK(map.begin() == map.end());
        const int32 keys[] = {50, 3, 99, 7, 1000, -5, 0, 42};
        for (int32 key : keys) {
            auto inserted = map.Insert(key, key * 2);
            REQUIRE(inserted);
            CHECK(inserted.Value() == InsertOutcome::Inserted);
        }

        CHECK(HasKeys(map, keys));
        usize position = 0;
        for (auto [key, value] : map) {
            CHECK(key == keys[position]);
            value += 1;
            ++position;
        }

        const OrderedMap<int32, int32, ReverseHash>& view = map;
        auto cursor = view.cbegin();
        CHECK(cursor->key == 50);
        CHECK(cursor->value == 101);
        ++cursor;
        CHECK((*cursor).key == 3);
        for (int32 key : keys) {
            REQUIRE(map.Find(key) != nullptr);
            CHECK(*map.Find(key) == key * 2 + 1);
            REQUIRE(map.FindStoredKey(key) != nullptr);
            CHECK(*map.FindStoredKey(key) == key);
        }

        CHECK(!map.Contains(1));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(OrderedMap_AssignKeepsPositionAndReinsertAppends) {
    SystemAllocator allocator;
    {
        OrderedMap<int32, int32> map(allocator);
        REQUIRE(map.Insert(1, 10));
        REQUIRE(map.Insert(2, 20));
        REQUIRE(map.Insert(3, 30));

        // 있는 키의 대입은 값만 바꾸고 자리를 지킨다(Sparks m[k] = v).
        auto assigned = map.InsertOrAssign(2, 200);
        REQUIRE(assigned);
        CHECK(assigned.Value() == InsertOutcome::AlreadyPresent);
        const int32 afterAssign[] = {1, 2, 3};
        CHECK(HasKeys(map, afterAssign));
        CHECK(*map.Find(2) == 200);

        // Insert·FindOrAdd 중복은 값도 순서도 바꾸지 않는다.
        auto duplicate = map.Insert(1, -1);
        REQUIRE(duplicate);
        CHECK(duplicate.Value() == InsertOutcome::AlreadyPresent);
        CHECK(*map.Find(1) == 10);
        auto existing = map.FindOrAdd(3, -3);
        REQUIRE(existing);
        CHECK(existing.Value().outcome == InsertOutcome::AlreadyPresent);
        CHECK(*existing.Value().value == 30);
        CHECK(HasKeys(map, afterAssign));

        // 지운 뒤 다시 넣은 키는 끝에 붙는다.
        CHECK(map.Erase(1));
        CHECK(!map.Erase(1));
        const int32 afterErase[] = {2, 3};
        CHECK(HasKeys(map, afterErase));
        auto reinserted = map.InsertOrAssign(1, 11);
        REQUIRE(reinserted);
        CHECK(reinserted.Value() == InsertOutcome::Inserted);
        auto added = map.FindOrAdd(4);
        REQUIRE(added);
        CHECK(added.Value().outcome == InsertOutcome::Inserted);
        CHECK(*added.Value().value == 0);
        const int32 afterReinsert[] = {2, 3, 1, 4};
        CHECK(HasKeys(map, afterReinsert));
        CHECK(*map.Find(1) == 11);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(OrderedMap_EraseCompactsInPlaceAndKeepsOrder) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        OrderedMap<int32, int32> map(allocator);
        REQUIRE(map.Reserve(64));
        CHECK(map.Capacity() == 64);
        for (int32 key = 0; key != 64; ++key) {
            REQUIRE(map.Insert(key, key * 3));
        }

        const uint64 attempts = allocator.Attempts();
        // 키 0·2는 위치 0·2라 두 항목 간격이다. 압축이 일어나면 위치 0·1로 한 항목 간격이 된다.
        const uptr gapBefore =
            reinterpret_cast<uptr>(map.Find(2)) - reinterpret_cast<uptr>(map.Find(0));
        // 홀수 키를 지우는 동안 죽은 항목이 산 항목의 절반을 넘으면 Erase가 제자리 압축한다. 압축은
        // 할당 없이 남은 원소의 상대 순서를 지킨다.
        for (int32 key = 1; key < 64; key += 2) {
            CHECK(map.Erase(key));
            CHECK(!map.Contains(key));
        }

        const uptr gapAfter =
            reinterpret_cast<uptr>(map.Find(2)) - reinterpret_cast<uptr>(map.Find(0));
        CHECK(gapAfter * 2 == gapBefore);

        CHECK(allocator.Attempts() == attempts);
        CHECK(map.Capacity() == 64);
        int32 evens[32] = {};
        for (int32 index = 0; index != 32; ++index) {
            evens[index] = index * 2;
        }

        CHECK(HasKeySequence(map, evens, 32));
        for (int32 key = 0; key < 64; key += 2) {
            REQUIRE(map.Find(key) != nullptr);
            CHECK(*map.Find(key) == key * 3);
        }

        // 끝 원소를 지우면 죽은 항목 없이 바로 떼어 낸다. 새 키는 남은 원소 뒤에 붙는다.
        CHECK(map.Erase(62));
        REQUIRE(map.Insert(100, 300));
        evens[31] = 100;
        CHECK(HasKeySequence(map, evens, 32));

        // Compact는 순서를 지킨 채 최소 용량으로 줄인다.
        REQUIRE(map.Compact());
        CHECK(map.Capacity() == 32);
        CHECK(HasKeySequence(map, evens, 32));
        REQUIRE(map.Compact());
        CHECK(map.Capacity() == 32);

        // 전부 지우면 비고, 용량을 유지한 채 다시 쓸 수 있다.
        for (int32 index = 0; index != 32; ++index) {
            CHECK(map.Erase(evens[index]));
        }

        CHECK(map.IsEmpty());
        CHECK(map.begin() == map.end());
        CHECK(map.Capacity() == 32);
        REQUIRE(map.Insert(7, 70));
        const int32 single[] = {7};
        CHECK(HasKeys(map, single));
        REQUIRE(map.Compact());
        CHECK(map.Capacity() == 4);
        map.Clear();
        CHECK(map.IsEmpty());
        CHECK(map.Capacity() == 4);
        REQUIRE(map.Compact());
        CHECK(map.Capacity() == 0);
    }
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(OrderedMap_ReserveKeepsPointersStableUntilCount) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        OrderedMap<int32, int32> map(allocator);
        REQUIRE(map.Reserve(100));
        CHECK(map.Capacity() == 128);
        REQUIRE(map.Insert(0, 0));
        int32* first = map.Find(0);
        const uint64 attempts = allocator.Attempts();
        for (int32 key = 1; key != 100; ++key) {
            REQUIRE(map.Insert(key, key));
        }

        // 예약한 원소 수까지는 할당도 이동도 없다.
        CHECK(allocator.Attempts() == attempts);
        CHECK(map.Find(0) == first);

        auto overflow = map.Reserve(MaxSize);
        CHECK(!overflow);
        CHECK(overflow.Error() == ErrorCode::Overflow);
        CHECK(map.Count() == 100);
        CHECK(map.Find(0) == first);

        // 죽은 항목 때문에 끝자리가 모자라면 Reserve가 제자리 압축으로 자리를 만든다(할당 없음).
        OrderedMap<int32, int32> dense(allocator);
        REQUIRE(dense.Reserve(8));
        for (int32 key = 0; key != 8; ++key) {
            REQUIRE(dense.Insert(key, key));
        }

        CHECK(dense.Erase(1));
        CHECK(dense.Erase(2));
        const uint64 denseAttempts = allocator.Attempts();
        REQUIRE(dense.Reserve(8));
        // 키 7은 압축 때 위치 7에서 5로 옮겨진다. Reserve가 압축하지 않았다면 아래 삽입이 압축해 이
        // 포인터가 바뀐다.
        int32* seven = dense.Find(7);
        REQUIRE(dense.Insert(8, 8));
        REQUIRE(dense.Insert(9, 9));
        CHECK(allocator.Attempts() == denseAttempts);
        CHECK(dense.Find(7) == seven);
        CHECK(dense.Capacity() == 8);
        const int32 expected[] = {0, 3, 4, 5, 6, 7, 8, 9};
        CHECK(HasKeys(dense, expected));
    }
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(OrderedMap_AllocationFailureLeavesMapUnchanged) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        OrderedMap<int32, TrackedValue> map(allocator);
        allocator.FailOnAttempt(1);
        TrackedValue firstValue(1);
        auto first = map.Insert(int32{1}, Move(firstValue));
        CHECK(!first);
        CHECK(first.Error() == ErrorCode::OutOfMemory);
        CHECK(map.IsEmpty());
        CHECK(map.Capacity() == 0);
        CHECK(firstValue.value == 1);

        allocator.DisableFailure();
        for (int32 key = 0; key != 4; ++key) {
            REQUIRE(map.Insert(int32{key}, TrackedValue(key * 10)));
        }

        CHECK(map.Capacity() == 4);
        TrackedValue* pointer = map.Find(2);
        REQUIRE(pointer != nullptr);
        auto cursor = map.begin();
        ++cursor;

        // 성장 할당 실패: 원소·순서·용량·포인터·입력·반복자가 그대로다.
        allocator.FailOnAttempt(1);
        TrackedValue grown(40);
        auto growth = map.Insert(int32{4}, Move(grown));
        CHECK(!growth);
        CHECK(growth.Error() == ErrorCode::OutOfMemory);
        CHECK(grown.value == 40);
        CHECK(map.Count() == 4);
        CHECK(map.Capacity() == 4);
        CHECK(map.Find(2) == pointer);
        CHECK((*cursor).key == 1);
        ++cursor;
        CHECK(cursor->key == 2);
        const int32 unchanged[] = {0, 1, 2, 3};
        CHECK(HasKeys(map, unchanged));

        allocator.FailOnAttempt(1);
        auto reserve = map.Reserve(1024);
        CHECK(!reserve);
        CHECK(reserve.Error() == ErrorCode::OutOfMemory);
        CHECK(map.Capacity() == 4);
        CHECK(map.Find(2) == pointer);
        CHECK(HasKeys(map, unchanged));

        // factory 실패와 factory 뒤 저장소 실패 모두 키 인자와 Map을 보존한다.
        allocator.DisableFailure();
        int32 factoryCalls = 0;
        auto factoryFailure = map.FindOrAddWith(int32{9}, [&]() noexcept -> Result<TrackedValue> {
            ++factoryCalls;
            return Result<TrackedValue>::Failure(ErrorCode::InvalidArgument);
        });
        CHECK(!factoryFailure);
        CHECK(factoryFailure.Error() == ErrorCode::InvalidArgument);
        allocator.FailOnAttempt(1);
        auto storageFailure = map.FindOrAddWith(int32{9}, [&]() noexcept -> Result<TrackedValue> {
            ++factoryCalls;
            return Result<TrackedValue>::Success(TrackedValue(90));
        });
        CHECK(!storageFailure);
        CHECK(storageFailure.Error() == ErrorCode::OutOfMemory);
        CHECK(factoryCalls == 2);
        CHECK(map.Find(2) == pointer);
        CHECK(HasKeys(map, unchanged));
        CHECK(TrackedValue::live == 4 + 2);

        // 축소 할당 실패도 그대로다.
        allocator.DisableFailure();
        REQUIRE(map.Reserve(64));
        pointer = map.Find(2);
        allocator.FailOnAttempt(1);
        auto shrink = map.Compact();
        CHECK(!shrink);
        CHECK(shrink.Error() == ErrorCode::OutOfMemory);
        CHECK(map.Capacity() == 64);
        CHECK(map.Find(2) == pointer);
        CHECK(HasKeys(map, unchanged));
        allocator.DisableFailure();
        REQUIRE(map.Compact());
        CHECK(map.Capacity() == 4);
        CHECK(HasKeys(map, unchanged));
        REQUIRE(map.Find(3) != nullptr);
        CHECK(map.Find(3)->value == 30);
    }
    CHECK(TrackedValue::live == 0);
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(OrderedMap_CollisionsAndMaskAliasedHashes) {
    SystemAllocator allocator;
    {
        OrderedMap<int32, int32, CollisionHash> colliding(allocator);
        OrderedMap<int32, int32, HighBitsHash> masked(allocator);
        OrderedMap<int32, int32, WraparoundHash> wrapping(allocator);
        for (int32 key = 0; key != 200; ++key) {
            REQUIRE(colliding.Insert(key, key * 3));
            REQUIRE(masked.Insert(key, key * 3));
            REQUIRE(wrapping.Insert(key, key * 3));
        }

        // 충돌 사슬의 앞·중간을 지워도 그 뒤 키의 탐색이 끊기면 안 된다.
        for (int32 key = 0; key < 200; key += 2) {
            CHECK(colliding.Erase(key));
            CHECK(masked.Erase(key));
            CHECK(wrapping.Erase(key));
        }

        for (int32 key = 0; key != 200; ++key) {
            const bool present = key % 2 != 0;
            CHECK(colliding.Contains(key) == present);
            CHECK(masked.Contains(key) == present);
            CHECK(wrapping.Contains(key) == present);
            if (present) {
                CHECK(*colliding.Find(key) == key * 3);
                CHECK(*masked.Find(key) == key * 3);
                CHECK(*wrapping.Find(key) == key * 3);
            }
        }

        // 해시가 mask 아래에서 같아도 높은 bit가 다르면 다른 키다.
        auto duplicate = masked.Insert(int32{1}, int32{-1});
        REQUIRE(duplicate);
        CHECK(duplicate.Value() == InsertOutcome::AlreadyPresent);
        for (int32 key = 200; key != 300; ++key) {
            REQUIRE(colliding.Insert(key, key));
            REQUIRE(masked.Insert(key, key));
        }

        int32 expected[200] = {};
        usize count = 0;
        for (int32 key = 1; key < 200; key += 2) {
            expected[count++] = key;
        }

        for (int32 key = 200; key != 300; ++key) {
            expected[count++] = key;
        }

        CHECK(HasKeySequence(colliding, expected, count));
        CHECK(HasKeySequence(masked, expected, count));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 공개 API로는 탐사 길이가 보이지 않아 색인 재건(삭제 칸 1/4 초과) 자체는 가려내지 못한다. 넣고
// 지우기를 오래 반복해도 할당 없이 정확함만 고정한다(재건 경로는 리뷰 probe가 내부 불변식으로
// 봤다).
TEST(OrderedMap_InsertEraseChurnStaysWithoutAllocation) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        OrderedMap<int32, int32> map(allocator);
        REQUIRE(map.Reserve(16));
        const uint64 attempts = allocator.Attempts();
        for (int32 key = 0; key != 2000; ++key) {
            REQUIRE(map.Insert(key, key));
            if (key >= 6) {
                CHECK(map.Erase(key - 6));
            }
        }

        CHECK(allocator.Attempts() == attempts);
        CHECK(map.Capacity() == 16);
        const int32 expected[] = {1994, 1995, 1996, 1997, 1998, 1999};
        CHECK(HasKeys(map, expected));
        for (int32 key = 0; key != 2000; ++key) {
            CHECK(map.Contains(key) == (key >= 1994));
        }
    }
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(OrderedMap_ElementLifetimeAcrossCompactionAndMove) {
    SystemAllocator first;
    SystemAllocator second;
    {
        OrderedMap<int32, TrackedValue> map(first);
        for (int32 key = 0; key != 40; ++key) {
            REQUIRE(map.Insert(int32{key}, TrackedValue(key)));
        }

        CHECK(TrackedValue::live == 40);
        for (int32 key = 0; key < 40; key += 3) {
            CHECK(map.Erase(key));
        }

        CHECK(TrackedValue::live == static_cast<int32>(map.Count()));
        map.ForEach(
            [](const int32& key, TrackedValue& value) noexcept { CHECK(value.value == key); });

        // 이동은 allocator·저장소를 옮기고 원본은 같은 allocator로 다시 쓸 수 있다.
        OrderedMap<int32, TrackedValue> moved(Move(map));
        CHECK(map.IsEmpty());
        CHECK(moved.Count() == 26);
        REQUIRE(map.Insert(int32{1}, TrackedValue(1)));
        OrderedMap<int32, TrackedValue> target(second);
        REQUIRE(target.Insert(int32{99}, TrackedValue(99)));
        target = Move(moved);
        CHECK(!target.Contains(99));
        CHECK(target.Count() == 26);
        CHECK(TrackedValue::live == 27);
        REQUIRE(target.Find(1) != nullptr);
        CHECK(target.Find(1)->value == 1);
        auto replaced = target.InsertOrAssign(int32{1}, TrackedValue(-10));
        REQUIRE(replaced);
        CHECK(target.Find(1)->value == -10);
        CHECK(TrackedValue::live == 27);
        target.Clear();
        CHECK(TrackedValue::live == 1);
        map.Reset();
        CHECK(TrackedValue::live == 0);
        CHECK(second.Stats().liveAllocations == 0);

        // 상태 있는 정책도 함께 옮긴다.
        OrderedMap<int32, int32, SaltedModuloHash, ModuloEqual> policy(
            first, SaltedModuloHash{119, 10}, ModuloEqual{10});
        REQUIRE(policy.Insert(7, 50));
        OrderedMap<int32, int32, SaltedModuloHash, ModuloEqual> policyMoved(Move(policy));
        REQUIRE(policyMoved.Find(17) != nullptr);
        CHECK(*policyMoved.Find(17) == 50);
    }
    CHECK(TrackedValue::live == 0);
    CHECK(first.Stats().liveAllocations == 0);
    CHECK(second.Stats().liveAllocations == 0);
}

TEST(OrderedMap_TraversalGuardsStructuralChanges) {
    SystemAllocator allocator;
    {
        OrderedMap<int32, int32> map(allocator);
        REQUIRE(map.Insert(1, 10));
        REQUIRE(map.Insert(2, 20));
        int32 visits = 0;
        map.ForEach([&](const int32& key, int32& value) noexcept {
            ++visits;
            value += 1;
            CHECK(map.Contains(key));
            CHECK(map.Insert(int32{3}, int32{30}).Error() == ErrorCode::Busy);
            CHECK(map.InsertOrAssign(int32{3}, int32{30}).Error() == ErrorCode::Busy);
            CHECK(map.FindOrAdd(int32{3}).Error() == ErrorCode::Busy);
            CHECK(map.Reserve(64).Error() == ErrorCode::Busy);
            CHECK(map.Compact().Error() == ErrorCode::Busy);
            auto erased = map.EraseIf([](const int32&, const int32&) noexcept { return true; });
            CHECK(!erased);
            CHECK(erased.Error() == ErrorCode::Busy);
            int32 nested = 0;
            map.ForEach([&](const int32&, int32&) noexcept { ++nested; });
            CHECK(nested == 2);
        });
        CHECK(visits == 2);
        CHECK(map.Count() == 2);
        CHECK(*map.Find(1) == 11);
        CHECK(*map.Find(2) == 21);

        // factory 안에서는 조회만 되고 구조 변경은 Busy다.
        auto created = map.FindOrAddWith(int32{5}, [&]() noexcept -> Result<int32> {
            CHECK(map.Contains(1));
            CHECK(map.Insert(int32{6}, int32{60}).Error() == ErrorCode::Busy);
            return Result<int32>::Success(50);
        });
        REQUIRE(created);
        CHECK(created.Value().outcome == InsertOutcome::Inserted);
        CHECK(*created.Value().value == 50);
        const int32 expected[] = {1, 2, 5};
        CHECK(HasKeys(map, expected));

        // 값 교체는 반복자를 무효화하지 않는다.
        int32 iterated = 0;
        for (auto entry : map) {
            REQUIRE(map.InsertOrAssign(int32{entry.key}, entry.key * 100));
            ++iterated;
        }

        CHECK(iterated == 3);
        CHECK(*map.Find(5) == 500);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(OrderedMap_EqualityIncludesOrder) {
    SystemAllocator allocator;
    {
        OrderedMap<int32, int32> left(allocator);
        OrderedMap<int32, int32> right(allocator);
        CHECK(left == right);
        REQUIRE(left.Insert(1, 10));
        REQUIRE(left.Insert(2, 20));
        REQUIRE(right.Insert(2, 20));
        REQUIRE(right.Insert(1, 10));
        // 같은 쌍이라도 순서가 다르면 다르다.
        CHECK(!(left == right));
        CHECK(left != right);
        CHECK(left == left);

        CHECK(right.Erase(2));
        REQUIRE(right.Insert(2, 20));
        CHECK(left == right);
        REQUIRE(right.InsertOrAssign(2, 21));
        CHECK(left != right);
        CHECK(left.SequenceEquals(
            right, [](const int32& a, const int32& b) noexcept { return a / 10 == b / 10; }));

        // 죽은 항목이 섞여 있어도 산 원소의 순서로 비교한다.
        REQUIRE(left.Insert(3, 30));
        REQUIRE(left.Insert(4, 40));
        CHECK(left.Erase(3));
        REQUIRE(right.InsertOrAssign(2, 20));
        REQUIRE(right.Insert(4, 40));
        CHECK(left == right);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(OrderedMap_EraseIfKeepsSurvivorOrder) {
    SystemAllocator allocator;
    {
        OrderedMap<int32, int32> map(allocator);
        const int32 keys[] = {9, 4, 7, 2, 8, 1, 6, 3, 5};
        for (int32 key : keys) {
            REQUIRE(map.Insert(key, key * 10));
        }

        int32 order[9] = {};
        usize calls = 0;
        auto removed = map.EraseIf([&](const int32& key, const int32& value) noexcept {
            order[calls++] = key;
            CHECK(value == key * 10);
            return key % 2 == 0;
        });
        REQUIRE(removed);
        CHECK(removed.Value() == 4);
        // predicate도 삽입 순서로 불린다.
        CHECK(calls == 9);
        bool sameOrder = true;
        for (usize index = 0; index != 9; ++index) {
            sameOrder = sameOrder && order[index] == keys[index];
        }

        CHECK(sameOrder);
        const int32 survivors[] = {9, 7, 1, 3, 5};
        CHECK(HasKeys(map, survivors));
        auto none = map.EraseIf([](const int32&, const int32&) noexcept { return false; });
        REQUIRE(none);
        CHECK(none.Value() == 0);
        auto all = map.EraseIf([](const int32&, const int32&) noexcept { return true; });
        REQUIRE(all);
        CHECK(all.Value() == 5);
        CHECK(map.IsEmpty());
        REQUIRE(map.Insert(1, 1));
        const int32 single[] = {1};
        CHECK(HasKeys(map, single));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(OrderedMap_StringKeysAndHeterogeneousLookup) {
    SystemAllocator allocator;
    {
        OrderedMap<String, int32> map(allocator);
        const char* names[] = {"Gamma", "alpha", "Beta", "delta"};
        for (int32 index = 0; index != 4; ++index) {
            String key(allocator);
            REQUIRE(key.TryAssign(names[index]));
            REQUIRE(map.Insert(Move(key), int32{index}));
        }

        // 순서는 넣은 순서이고 문자열 정렬이 아니다.
        int32 position = 0;
        for (auto entry : map) {
            CHECK(entry.key.View() == StringView(names[position]));
            CHECK(entry.value == position);
            ++position;
        }

        CHECK(position == 4);
        REQUIRE(map.Find(StringView("Beta")) != nullptr);
        CHECK(*map.Find(StringView("Beta")) == 2);
        CHECK(!map.Contains(StringView("beta")));
        CHECK(map.Erase(StringView("Gamma")));
        REQUIRE(map.FindStoredKey(StringView("alpha")) != nullptr);
        CHECK(map.begin()->key.View() == StringView("alpha"));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(OrderedMap_RejectsArgumentsAliasingStorage) {
    SystemAllocator allocator;
    {
        OrderedMap<int32, int32> map(allocator);
        REQUIRE(map.Insert(1, 10));
        REQUIRE(map.Insert(2, 20));
        int32* stored = map.Find(1);
        REQUIRE(stored != nullptr);
        // 저장소 원소를 새 값이나 교체 값으로 직접 넘기면 성장·교체 중 사라질 수 있어 거부한다.
        auto aliasedInsert = map.Insert(int32{3}, *stored);
        CHECK(!aliasedInsert);
        CHECK(aliasedInsert.Error() == ErrorCode::InvalidArgument);
        auto aliasedAssign = map.InsertOrAssign(int32{2}, *stored);
        CHECK(!aliasedAssign);
        CHECK(aliasedAssign.Error() == ErrorCode::InvalidArgument);
        CHECK(*map.Find(2) == 20);
        const int32 expected[] = {1, 2};
        CHECK(HasKeys(map, expected));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

// 단순 배열 모형(삽입 순서 그대로)과 같은 연산을 해 내용·순서를 대조한다.
template <typename Hasher>
void RunReferenceModel(uint64 seed) {
    constexpr int32 KeyRange = 48;
    SystemAllocator allocator;
    {
        OrderedMap<int32, int32, Hasher> map(allocator);
        int32 keys[KeyRange] = {};
        int32 values[KeyRange] = {};
        usize count = 0;
        auto find = [&](int32 key) {
            for (usize index = 0; index != count; ++index) {
                if (keys[index] == key) {
                    return index;
                }
            }

            return IndexNone;
        };
        RandomStream random(seed);
        for (int32 step = 0; step != 6000; ++step) {
            const int32 key = static_cast<int32>(random.NextUInt32Below(KeyRange));
            const int32 value = step;
            const uint32 operation = random.NextUInt32Below(10);
            const usize found = find(key);
            if (operation < 3) {
                auto result = map.Insert(key, value);
                REQUIRE(result);
                CHECK((result.Value() == InsertOutcome::AlreadyPresent) == (found != IndexNone));
                if (found == IndexNone) {
                    keys[count] = key;
                    values[count] = value;
                    ++count;
                }
            } else if (operation < 5) {
                REQUIRE(map.InsertOrAssign(key, value));
                if (found == IndexNone) {
                    keys[count] = key;
                    values[count] = value;
                    ++count;
                } else {
                    values[found] = value;
                }
            } else if (operation < 9) {
                CHECK(map.Erase(key) == (found != IndexNone));
                if (found != IndexNone) {
                    for (usize index = found; index + 1 < count; ++index) {
                        keys[index] = keys[index + 1];
                        values[index] = values[index + 1];
                    }

                    --count;
                }
            } else if (step % 3 == 0) {
                REQUIRE(map.Compact());
            } else {
                REQUIRE(map.Reserve(static_cast<usize>(key)));
            }

            REQUIRE(map.Count() == count);
            if (step % 97 == 0 || step == 5999) {
                REQUIRE(HasKeySequence(map, keys, count));
                for (usize index = 0; index != count; ++index) {
                    REQUIRE(map.Find(keys[index]) != nullptr);
                    CHECK(*map.Find(keys[index]) == values[index]);
                }
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace

TEST(OrderedMap_MatchesReferenceModel) {
    RunReferenceModel<Hash<int32>>(1);
    RunReferenceModel<ModuloHash>(2);
    RunReferenceModel<CollisionHash>(3);
}

namespace rocks::test {

// 별도 child에서만 실행한다. 요청한 Fatal이 나지 않으면 0을 돌려 음성 대조를 실패시킨다.
// 0: 삽입 뒤 반복자 사용, 1: ForEach 중 Erase.
int RunOrderedMapNegativeControl(uint32 operation) noexcept {
    PrepareFatalTestProcess();
    SystemAllocator allocator;
    OrderedMap<int32, int32> map(allocator);
    if (!map.Insert(1, 1) || !map.Insert(2, 2)) {
        return 2;
    }

    if (operation == 0) {
        int32 visited = 0;
        for (auto entry : map) {
            visited += entry.value;
            if (!map.Insert(100 + visited, 0)) {
                return 2;
            }
        }

        return 0;
    }

    map.ForEach([&](const int32&, int32&) noexcept { (void)map.Erase(1); });
    return 0;
}

} // namespace rocks::test
