#include <Base/Types.h>
#include <Core/Containers/HashMap.h>
#include <Core/Containers/HashSet.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>

// HashMap·HashSet의 Erase가 원소 0개에서 삭제 칸을 비우는 규칙이다: 삭제 칸이 칸 수의 1/4 이상
// 쌓였을 때만 모든 칸을 비운다(HashMap.h EmptyClearDivisor). 큰 고점 뒤에 0과 1 사이를 오가도
// 쌍마다 표 전체를 다시 쓰지 않는다(Docs/Design/GarbageCollectionScale.ko.md 18.6절).
using namespace rocks;

namespace {

// 키 k를 칸 k & (칸 수 - 1)에서 시작하게 해 어느 칸이 삭제 칸이 되는지 시험이 정확히 정한다.
struct IdentityHash {
    usize operator()(int32 key) const noexcept {
        return static_cast<usize>(key);
    }
};

// HashMap의 문턱 분모와 같다(삭제 칸이 칸 수의 1/4 이상이면 비운다).
constexpr usize ClearDivisor = 4;
constexpr int32 Peak = 1000;
// Peak개를 담는 칸 수다(부하율 1/2 이하의 2의 거듭제곱).
constexpr usize PeakSlots = 2048;

} // namespace

// 고점까지 넣고 모두 지운 뒤 넣기·지우기 쌍을 되풀이하면 삭제 칸이 쌍마다 하나씩 늘다가 문턱에서
// 0이 된다(쌍마다 비우지 않는다). 그 사이 조회·순회·다시 넣기가 맞다.
TEST(CoreHashMapEmptyingKeepsTombstonesBelowThreshold) {
    SystemAllocator memory;
    {
        HashMap<int32, int32, IdentityHash> map(memory);
        for (int32 key = 0; key != Peak; ++key) {
            REQUIRE(map.Insert(int32{key}, key * 3));
        }

        REQUIRE(map.SlotCount() == PeakSlots);
        // 마지막 삭제에서 삭제 칸(1000)이 문턱(512)을 넘어 한꺼번에 비운다. 용량은 그대로다.
        for (int32 key = 0; key != Peak; ++key) {
            CHECK(map.Erase(key));
        }

        CHECK(map.IsEmpty());
        CHECK(map.DeletedSlotCount() == 0);
        CHECK(map.SlotCount() == PeakSlots);

        const usize threshold = PeakSlots / ClearDivisor;
        const usize pairs = 2 * threshold + 7;
        for (usize pair = 1; pair <= pairs; ++pair) {
            // 쌍마다 다른 칸에서 시작하는 키다(앞 쌍이 남긴 삭제 칸을 다시 쓰지 않는다).
            const int32 key = static_cast<int32>(pair);
            REQUIRE(map.Insert(int32{key}, key * 5));
            CHECK(map.Count() == 1);
            const int32* value = map.Find(key);
            REQUIRE(value != nullptr);
            CHECK(*value == key * 5);
            // 없는 키: 빈 칸에서 시작하는 키와 삭제 칸에서 시작하는 키다.
            CHECK(map.Find(key + 1) == nullptr);
            CHECK(map.Find(key - 1) == nullptr);
            usize visited = 0;
            int32 seen = 0;
            map.ForEach([&](const int32& stored, const int32&) noexcept {
                ++visited;
                seen = stored;
            });
            CHECK(visited == 1);
            CHECK(seen == key);
            CHECK(map.Erase(key));
            CHECK(!map.Erase(key));
            CHECK(map.IsEmpty());
            CHECK(map.Find(key) == nullptr);
            // 문턱에 닿는 쌍에서만 비운다.
            REQUIRE(map.DeletedSlotCount() == pair % threshold);
            REQUIRE(map.SlotCount() == PeakSlots);
        }

        // 남은 삭제 칸(7개 — 칸 1025..1031) 위에 다시 넣으면 그 칸을 다시 쓴다.
        REQUIRE(map.DeletedSlotCount() == 7);
        const int32 reused = static_cast<int32>(pairs);
        REQUIRE(map.Insert(int32{reused}, 77));
        CHECK(map.DeletedSlotCount() == 6);
        // 삭제 칸이 남은 채로 많이 넣어도 내용이 맞다(삽입의 정리가 처리한다).
        for (int32 key = 5000; key != 5000 + Peak; ++key) {
            REQUIRE(map.Insert(int32{key}, key + 1));
        }

        CHECK(map.Count() == static_cast<usize>(Peak) + 1);
        CHECK((map.Count() + map.DeletedSlotCount()) * 4 <= map.SlotCount() * 3);
        for (int32 key = 5000; key != 5000 + Peak; ++key) {
            const int32* value = map.Find(key);
            REQUIRE(value != nullptr);
            CHECK(*value == key + 1);
        }

        const int32* kept = map.Find(reused);
        REQUIRE(kept != nullptr);
        CHECK(*kept == 77);
    }

    CHECK(memory.Stats().liveAllocations == 0);
}

// 삭제 칸이 적을 때는 0개가 돼도 비우지 않는다. 빈 Map의 Compact·ShrinkSlack은 삭제 칸이 남아
// 있어도 저장소를 돌려준다. 가장 작은 표(8칸)의 문턱은 2다.
TEST(CoreHashMapEmptyWithFewTombstonesIsNotCleared) {
    SystemAllocator memory;
    {
        HashMap<int32, int32, IdentityHash> map(memory);
        REQUIRE(map.Reserve(32));
        constexpr usize Slots = 64;
        REQUIRE(map.SlotCount() == Slots);
        for (int32 key = 0; key != 3; ++key) {
            REQUIRE(map.Insert(int32{key}, key));
        }

        for (int32 key = 0; key != 3; ++key) {
            CHECK(map.Erase(key));
        }

        CHECK(map.IsEmpty());
        CHECK(map.Count() == 0);
        CHECK(map.DeletedSlotCount() == 3);
        CHECK(map.Find(0) == nullptr);
        CHECK(map.Find(40) == nullptr);
        CHECK(!map.Contains(2));
        usize visited = 0;
        map.ForEach([&](const int32&, const int32&) noexcept { ++visited; });
        CHECK(visited == 0);
        // 문턱(64 / 4 = 16)에 닿을 때까지 쌍마다 하나씩 는다.
        for (int32 key = 3; key != 15; ++key) {
            REQUIRE(map.Insert(int32{key}, key));
            CHECK(map.Erase(key));
            REQUIRE(map.DeletedSlotCount() == static_cast<usize>(key) + 1);
        }

        REQUIRE(map.Insert(int32{15}, 15));
        CHECK(map.Erase(15));
        CHECK(map.DeletedSlotCount() == 0);
        CHECK(map.SlotCount() == Slots);

        // 삭제 칸이 남은 빈 Map을 줄이면 저장소를 돌려준다.
        REQUIRE(map.Insert(int32{20}, 20));
        CHECK(map.Erase(20));
        CHECK(map.DeletedSlotCount() == 1);
        REQUIRE(map.ShrinkSlack());
        CHECK(map.SlotCount() == 0);
        CHECK(map.DeletedSlotCount() == 0);
        CHECK(memory.Stats().liveAllocations == 0);

        // 가장 작은 표(8칸): 삭제 칸 하나는 남고 둘째에서 비운다.
        REQUIRE(map.Insert(int32{1}, 10));
        REQUIRE(map.SlotCount() == 8);
        CHECK(map.Erase(1));
        CHECK(map.DeletedSlotCount() == 1);
        REQUIRE(map.Insert(int32{2}, 20));
        CHECK(map.Erase(2));
        CHECK(map.DeletedSlotCount() == 0);
        REQUIRE(map.Insert(int32{1}, 11));
        const int32* value = map.Find(1);
        REQUIRE(value != nullptr);
        CHECK(*value == 11);
        REQUIRE(map.Compact());
        CHECK(map.Count() == 1);
        CHECK(map.Erase(1));
        CHECK(map.DeletedSlotCount() == 1);
        REQUIRE(map.Compact());
        CHECK(map.SlotCount() == 0);
    }

    CHECK(memory.Stats().liveAllocations == 0);
}

// EraseIf와 Clear는 모든 칸을 보므로 0개가 되면 문턱과 무관하게 삭제 칸이 없다.
TEST(CoreHashMapEraseIfAndClearLeaveNoTombstonesWhenEmpty) {
    SystemAllocator memory;
    {
        HashMap<int32, int32, IdentityHash> map(memory);
        REQUIRE(map.Reserve(32));
        for (int32 key = 0; key != 3; ++key) {
            REQUIRE(map.Insert(int32{key}, key));
        }

        auto removed = map.EraseIf([](const int32&, const int32&) noexcept { return true; });
        REQUIRE(removed);
        CHECK(removed.Value() == 3);
        CHECK(map.IsEmpty());
        CHECK(map.DeletedSlotCount() == 0);

        REQUIRE(map.Insert(int32{7}, 7));
        REQUIRE(map.Insert(int32{8}, 8));
        CHECK(map.Erase(7));
        CHECK(map.DeletedSlotCount() == 1);
        map.Clear();
        CHECK(map.IsEmpty());
        CHECK(map.DeletedSlotCount() == 0);
        CHECK(map.SlotCount() == 64);
    }

    CHECK(memory.Stats().liveAllocations == 0);
}

// HashSet도 같은 규칙이다(HashMap 위에 있다).
TEST(CoreHashSetEmptyingKeepsTombstonesBelowThreshold) {
    SystemAllocator memory;
    {
        HashSet<int32, IdentityHash> set(memory);
        for (int32 key = 0; key != Peak; ++key) {
            REQUIRE(set.Add(int32{key}));
        }

        REQUIRE(set.SlotCount() == PeakSlots);
        for (int32 key = 0; key != Peak; ++key) {
            CHECK(set.Erase(key));
        }

        CHECK(set.IsEmpty());
        CHECK(set.DeletedSlotCount() == 0);
        const usize threshold = PeakSlots / ClearDivisor;
        for (usize pair = 1; pair <= threshold + 3; ++pair) {
            const int32 key = static_cast<int32>(pair);
            REQUIRE(set.Add(int32{key}));
            CHECK(set.Contains(key));
            CHECK(!set.Contains(key + 1));
            CHECK(set.Erase(key));
            CHECK(set.IsEmpty());
            CHECK(!set.Contains(key));
            REQUIRE(set.DeletedSlotCount() == pair % threshold);
        }

        CHECK(set.SlotCount() == PeakSlots);
        REQUIRE(set.Add(int32{4000}));
        CHECK(set.Count() == 1);
        CHECK(set.Contains(4000));
    }

    CHECK(memory.Stats().liveAllocations == 0);
}
