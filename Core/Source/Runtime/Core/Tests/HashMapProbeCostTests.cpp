#include <Base/Types.h>
#include <Core/Containers/HashMap.h>
#include <Core/Math/RandomStream.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace hash_map_probe_cost_tests_file {

namespace {

// 키 k를 칸 k & (칸 수 - 1)에서 시작하게 해 어느 칸이 빈 칸·삭제 칸인지 시험이 정확히 정한다.
struct IdentityHash {
    usize operator()(int32 key) const noexcept {
        return static_cast<usize>(key);
    }
};

// 해시·동등 비교 호출 수를 센다(시간 없이 조회 횟수를 결정적으로 잰다). 모든 키가 한 칸에서
// 시작하므로 조회 한 번의 동등 비교 수는 그 앞에 놓인 원소 수다.
struct CountingZeroHash {
    static inline usize calls = 0;

    usize operator()(int32) const noexcept {
        ++calls;
        return 0;
    }
};

struct CountingEqual {
    static inline usize calls = 0;

    bool operator()(const int32& left, const int32& right) const noexcept {
        ++calls;
        return left == right;
    }
};

// 이동하면 원본을 -1로 바꿔, 실패한 삽입이 입력을 소비하지 않았는지 본다.
// live는 살아 있는 객체 수다. 재해시·성장이 값을 새거나 두 번 소멸하지 않았는지
// 시험 끝에 0으로 확인한다.
struct MovedValue {
    static inline int32 live = 0;
    int32 value;

    explicit MovedValue(int32 number) noexcept : value(number) {
        ++live;
    }

    MovedValue(const MovedValue&) = delete;
    MovedValue& operator=(const MovedValue&) = delete;

    MovedValue(MovedValue&& other) noexcept : value(other.value) {
        other.value = -1;
        ++live;
    }

    ~MovedValue() noexcept {
        --live;
    }
};

template <typename Map>
bool FilledWithinThreeQuarters(const Map& map) {
    return (map.Count() + map.DeletedSlotCount()) * 4 <= map.SlotCount() * 3;
}

} // namespace

// 원소 수를 Capacity() 가까이(칸 1024개에 511개) 고정한 채 삭제·재삽입을 10만 번 해도 (점유 + 삭제)
// 칸은 늘 칸 수의 3/4 이하다(고치기 전 규칙 "삭제 > 칸/4 그리고 삭제 > 산 원소"는 이 부하에서 거의
// 작동하지 않아 0.996까지 찼다). 첫 정리는 산 원소(511)가 칸 1024개의 3/8을 넘어 두 배 성장이고, 그
// 뒤 정리는 칸 2048개에서 같은 크기 재해시다. 키 선택은 seed를 고정한 RandomStream이라 결정적이다.
TEST(CoreHashMapDeletedSlotsStayBelowThreeQuartersUnderChurn) {
    constexpr usize Live = 511;
    constexpr usize InitialSlots = 1024;
    constexpr usize GrownSlots = InitialSlots * 2;
    constexpr uint32 Rounds = 100000;
    SystemAllocator allocator;
    {
        HashMap<int32, int32> map(allocator);
        REQUIRE(map.Reserve(InitialSlots / 2));
        REQUIRE(map.SlotCount() == InitialSlots);
        int32 keys[Live] = {};
        int32 nextKey = 0;
        for (usize index = 0; index != Live; ++index) {
            keys[index] = nextKey;
            REQUIRE(map.Insert(int32{nextKey}, nextKey * 3));
            ++nextKey;
        }

        const uint64 allocationsBefore = allocator.Stats().totalAllocations;
        RandomStream random(0x5eed);
        bool bounded = true;
        bool consistent = true;
        usize largestFilledPerMille = 0;
        for (uint32 round = 0; round != Rounds; ++round) {
            const usize victim = random.NextUInt32Below(static_cast<uint32>(Live));
            consistent = consistent && map.Erase(keys[victim]);
            bounded = bounded && FilledWithinThreeQuarters(map);
            keys[victim] = nextKey;
            auto inserted = map.Insert(int32{nextKey}, nextKey * 3);
            consistent = consistent && inserted && inserted.Value() == InsertOutcome::Inserted;
            ++nextKey;
            bounded = bounded && FilledWithinThreeQuarters(map);
            const usize filled = (map.Count() + map.DeletedSlotCount()) * 1000 / map.SlotCount();
            largestFilledPerMille = filled > largestFilledPerMille ? filled : largestFilledPerMille;
        }

        CHECK(consistent);
        CHECK(bounded);
        CHECK(largestFilledPerMille <= 750);
        CHECK(map.Count() == Live);
        CHECK(map.SlotCount() == GrownSlots);
        // 성장 뒤 같은 크기 정리 직후의 합은 산 원소 수(Live 이하)다. 다음 정리는
        // 합이 칸 수의 3/4에 이르러야 하므로 정리마다 (GrownSlots * 3/4 - Live)번
        // 이상의 삽입이 앞선다. 그래서 재해시는 처음 성장 한 번에 많아야
        // Rounds / (GrownSlots * 3/4 - Live)번을 더한 수다.
        const uint64 rehashes = allocator.Stats().totalAllocations - allocationsBefore;
        CHECK(rehashes >= 1);
        CHECK(rehashes <= 1 + Rounds / (GrownSlots * 3 / 4 - Live));
        bool found = true;
        for (usize index = 0; index != Live; ++index) {
            const int32* value = map.Find(keys[index]);
            found = found && value != nullptr && *value == keys[index] * 3;
        }

        CHECK(found);
        CHECK(map.Find(nextKey) == nullptr);
        CHECK(map.Find(-1) == nullptr);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 산 원소가 Capacity()의 3/4 이하일 때의 정리는 같은 크기 재해시다. 그 할당이 실패하면 입력·기존
// 주소·칸 수·삭제 칸을 그대로 두고, 다시 하면 삭제 칸을 모두 없앤다.
TEST(CoreHashCollectionsAutomaticTombstoneCleanupFailure) {
    MovedValue::live = 0;
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        HashMap<int32, MovedValue, IdentityHash> map(allocator);
        REQUIRE(map.Reserve(16));
        REQUIRE(map.SlotCount() == 32);
        for (int32 key = 0; key != 16; ++key) {
            REQUIRE(map.Insert(int32{key}, MovedValue(key * 10)));
        }

        for (int32 key = 0; key != 13; ++key) {
            CHECK(map.Erase(key));
        }

        // 칸 16..23은 비어 있어 이 삽입들은 빈 칸을 차지한다.
        // -> (점유 + 삭제) = 11 + 13 = 24 = 칸 32개의 3/4.
        for (int32 key = 16; key != 24; ++key) {
            REQUIRE(map.Insert(int32{key}, MovedValue(key * 10)));
        }

        CHECK(map.Count() == 11);
        CHECK(map.DeletedSlotCount() == 13);
        MovedValue* old = map.Find(14);
        REQUIRE(old != nullptr);
        MovedValue candidate(240);
        allocator.FailOnAttempt(1);
        auto fail = map.Insert(int32{24}, Move(candidate));
        CHECK(!fail);
        CHECK(fail.Error() == ErrorCode::OutOfMemory);
        CHECK(allocator.Attempts() == 1);
        CHECK(candidate.value == 240);
        CHECK(map.SlotCount() == 32);
        CHECK(map.DeletedSlotCount() == 13);
        CHECK(map.Count() == 11);
        CHECK(map.Find(14) == old);
        CHECK(old->value == 140);
        CHECK(map.Find(24) == nullptr);

        allocator.DisableFailure();
        auto inserted = map.Insert(int32{24}, Move(candidate));
        REQUIRE(inserted);
        CHECK(inserted.Value() == InsertOutcome::Inserted);
        CHECK(allocator.Attempts() == 1);
        CHECK(map.SlotCount() == 32);
        CHECK(map.DeletedSlotCount() == 0);
        CHECK(map.Count() == 12);
        for (int32 key = 0; key != 25; ++key) {
            const MovedValue* value = map.Find(key);
            CHECK((value != nullptr) == (key >= 13));
            CHECK(value == nullptr || value->value == key * 10);
        }
    }

    CHECK(MovedValue::live == 0);
    CHECK(backing.Stats().liveAllocations == 0);
    CHECK(backing.Stats().liveBytes == 0);
}

// 산 원소가 Capacity()의 3/4을 넘을 때의 정리는 두 배 성장이다. 실패는 상태를 보존하고, 성공하면
// 칸 수가 두 배이며 삭제 칸이 없다.
TEST(CoreHashMapCrowdedTombstoneCleanupGrows) {
    MovedValue::live = 0;
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        HashMap<int32, MovedValue, IdentityHash> map(allocator);
        REQUIRE(map.Reserve(16));
        REQUIRE(map.SlotCount() == 32);
        for (int32 key = 0; key != 16; ++key) {
            REQUIRE(map.Insert(int32{key}, MovedValue(key * 10)));
        }

        for (int32 key = 0; key != 8; ++key) {
            CHECK(map.Erase(key));
        }

        for (int32 key = 16; key != 24; ++key) {
            REQUIRE(map.Insert(int32{key}, MovedValue(key * 10)));
        }

        for (int32 key = 8; key != 12; ++key) {
            CHECK(map.Erase(key));
        }

        // 산 원소 12 + 삭제 칸 12 = 24 = 칸 32개의 3/4이다. 다음 삽입 뒤 산 원소 13은 칸 32개의
        // 3/8(12)을 넘는다.
        CHECK(map.Count() == 12);
        CHECK(map.DeletedSlotCount() == 12);
        CHECK(FilledWithinThreeQuarters(map));
        MovedValue* old = map.Find(20);
        REQUIRE(old != nullptr);
        MovedValue candidate(240);
        allocator.FailOnAttempt(1);
        auto fail = map.Insert(int32{24}, Move(candidate));
        CHECK(!fail);
        CHECK(fail.Error() == ErrorCode::OutOfMemory);
        CHECK(candidate.value == 240);
        CHECK(map.SlotCount() == 32);
        CHECK(map.Capacity() == 16);
        CHECK(map.DeletedSlotCount() == 12);
        CHECK(map.Find(20) == old);

        allocator.DisableFailure();
        REQUIRE(map.Insert(int32{24}, Move(candidate)));
        CHECK(allocator.Attempts() == 1);
        CHECK(map.SlotCount() == 64);
        CHECK(map.Capacity() == 32);
        CHECK(map.DeletedSlotCount() == 0);
        CHECK(map.Count() == 13);
        for (int32 key = 0; key != 25; ++key) {
            const MovedValue* value = map.Find(key);
            CHECK((value != nullptr) == (key >= 12));
            CHECK(value == nullptr || value->value == key * 10);
        }
    }

    CHECK(MovedValue::live == 0);
    CHECK(backing.Stats().liveAllocations == 0);
    CHECK(backing.Stats().liveBytes == 0);
}

// FindOrAddWith는 키가 없을 때도 해시 한 번·동등 비교 한 바퀴(앞에 놓인 원소 수)만 한다. factory
// 동안 구조 변경이 막혀 키 없음이 유지되므로 삽입 때 다시 조회하지 않는다.
TEST(CoreHashMapFindOrAddWithLooksUpOnce) {
    SystemAllocator allocator;
    {
        HashMap<int32, int32, CountingZeroHash, CountingEqual> map(allocator);
        for (int32 key = 0; key != 3; ++key) {
            REQUIRE(map.Insert(int32{key}, key + 100));
        }

        CountingZeroHash::calls = 0;
        CountingEqual::calls = 0;
        usize factoryCalls = 0;
        auto added = map.FindOrAddWith(int32{9}, [&]() noexcept {
            ++factoryCalls;
            return Result<int32>::Success(int32{909});
        });
        REQUIRE(added);
        CHECK(added.Value().outcome == InsertOutcome::Inserted);
        CHECK(*added.Value().value == 909);
        CHECK(factoryCalls == 1);
        CHECK(CountingZeroHash::calls == 1);
        CHECK(CountingEqual::calls == 3);

        CountingZeroHash::calls = 0;
        CountingEqual::calls = 0;
        auto present = map.FindOrAddWith(int32{1}, [&]() noexcept {
            ++factoryCalls;
            return Result<int32>::Success(int32{0});
        });
        REQUIRE(present);
        CHECK(present.Value().outcome == InsertOutcome::AlreadyPresent);
        CHECK(*present.Value().value == 101);
        CHECK(factoryCalls == 1);
        CHECK(CountingZeroHash::calls == 1);
        CHECK(CountingEqual::calls == 2);
        CHECK(map.Count() == 4);
        REQUIRE(map.Find(9) != nullptr);
        CHECK(*map.Find(9) == 909);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
} // namespace hash_map_probe_cost_tests_file
