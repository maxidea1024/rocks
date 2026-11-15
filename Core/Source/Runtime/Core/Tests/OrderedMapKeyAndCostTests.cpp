#include <Base/Types.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Containers/OrderedSet.h>
#include <Core/Math/RandomStream.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/String.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;

namespace {

struct ZeroHash {
    usize operator()(int32) const noexcept {
        return 0;
    }
};

// 이동 생성 횟수를 센다. 압축·성장으로 옮겨진 원소 수를 시간 없이 결정적으로 잰다.
struct MoveCounted {
    static inline usize moves = 0;
    int32 value;

    explicit MoveCounted(int32 number) noexcept : value(number) {
    }

    MoveCounted(const MoveCounted&) = delete;
    MoveCounted& operator=(const MoveCounted&) = delete;

    MoveCounted(MoveCounted&& other) noexcept : value(other.value) {
        ++moves;
    }

    ~MoveCounted() noexcept = default;
};

template <typename Map>
bool KeysAre(const Map& map, const int32* expected, usize count) {
    if (map.Count() != count) {
        return false;
    }

    usize index = 0;
    bool same = true;
    for (auto entry : map) {
        same = same && index < count && entry.key == expected[index];
        ++index;
    }

    return same && index == count;
}

} // namespace

// 가득 찬 상태에서 "가운데 삭제 + Reserve(Count() + 1) + 삽입"을 반복해도 Reserve가 매번 O(n)
// 압축을 하지 않는다(죽은 항목이 적으면 두 배 성장으로 상환). 옮긴 원소 수로 본다.
TEST(OrderedMap_ReserveAfterEraseStaysAmortized) {
    SystemAllocator allocator;
    {
        constexpr int32 Initial = 1024;
        constexpr int32 Rounds = 128;
        OrderedMap<int32, MoveCounted> map(allocator, SeededHash<int32>(HashSeed{5, 6}));
        REQUIRE(map.Reserve(Initial));
        for (int32 key = 0; key != Initial; ++key) {
            REQUIRE(map.Insert(int32{key}, MoveCounted(key)));
        }

        MoveCounted::moves = 0;
        for (int32 round = 0; round != Rounds; ++round) {
            CHECK(map.Erase(100 + round * 3));
            REQUIRE(map.Reserve(map.Count() + 1));
            REQUIRE(map.Insert(int32{Initial + round}, MoveCounted(Initial + round)));
        }

        // 고치기 전에는 한 바퀴마다 가운데 뒤 원소 전부(약 900개)를 옮겼다(합계 약 11만). 지금은 한
        // 번 두 배 성장(1023개)과 삽입 128번이다.
        CHECK(MoveCounted::moves <= static_cast<usize>(Initial + Rounds));
        CHECK(map.Capacity() == static_cast<usize>(Initial) * 2);
        int32 expected[Initial] = {};
        usize count = 0;
        for (int32 key = 0; key != Initial; ++key) {
            const bool erased = key >= 100 && key < 100 + Rounds * 3 && (key - 100) % 3 == 0;
            if (!erased) {
                expected[count++] = key;
            }
        }

        for (int32 round = 0; round != Rounds; ++round) {
            expected[count++] = Initial + round;
        }

        CHECK(KeysAre(map, expected, count));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 모두 충돌하는 해시에서 대량 EraseIf는 앞 일부만 색인 칸을 하나씩 지우고 나머지는 색인을 한 번
// 다시 짓는다. predicate 안 중첩 조회는 이미 지운(아직 색인이 남은) 원소를 찾지 않아야 한다.
TEST(OrderedMap_BulkEraseIfWithCollidingHashesKeepsLookups) {
    SystemAllocator allocator;
    {
        constexpr int32 Total = 2000;
        OrderedMap<int32, int32, ZeroHash> map(allocator);
        for (int32 key = 0; key != Total; ++key) {
            REQUIRE(map.Insert(key, key * 2));
        }

        bool nestedOk = true;
        auto removed = map.EraseIf([&](const int32& key, const int32& value) noexcept {
            nestedOk = nestedOk && value == key * 2 && map.Contains(key);
            if (key > 0 && (key - 1) % 4 != 0) {
                // 바로 앞 키는 이미 지웠다.
                nestedOk = nestedOk && !map.Contains(key - 1);
            }

            return key % 4 != 0;
        });
        REQUIRE(removed);
        CHECK(removed.Value() == static_cast<usize>(Total / 4 * 3));
        CHECK(nestedOk);
        int32 expected[Total / 4] = {};
        for (int32 index = 0; index != Total / 4; ++index) {
            expected[index] = index * 4;
        }

        CHECK(KeysAre(map, expected, Total / 4));
        for (int32 key = 0; key != Total; ++key) {
            CHECK(map.Contains(key) == (key % 4 == 0));
        }

        // 다시 지은 색인으로 삽입·삭제가 이어진다.
        REQUIRE(map.Insert(1, 1));
        CHECK(map.Erase(0));
        CHECK(map.Contains(1));
        CHECK(!map.Contains(0));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 용량 상한(2^31) 판정은 할당 전에 한다. 상한 자체는 크기 계산을 지나 할당기까지 간다.
TEST(OrderedMap_CapacityLimitIsCheckedBeforeAllocation) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        using Map = OrderedMap<int32, int32>;
        Map map(allocator);
        CHECK(Map::MaximumCapacity == static_cast<usize>(1) << 31);
        allocator.FailOnAttempt(1);
        auto over = map.Reserve(Map::MaximumCapacity + 1);
        CHECK(!over);
        CHECK(over.Error() == ErrorCode::Overflow);
        CHECK(allocator.Attempts() == 0);
        if constexpr (sizeof(usize) == sizeof(uint64)) {
            auto limit = map.Reserve(Map::MaximumCapacity);
            CHECK(!limit);
            CHECK(limit.Error() == ErrorCode::OutOfMemory);
            CHECK(allocator.Attempts() == 1);
        }

        CHECK(map.Capacity() == 0);
    }
    CHECK(backing.Stats().liveAllocations == 0);
}

// Name 키는 비교 identity로 찾는다(대소문자 표시 변형은 같은 키). B9f2의 OrderedMap<Name, float>·
// OrderedSet<Name> 사용처다.
TEST(OrderedMap_NameKeysUseComparisonIdentity) {
    SystemAllocator allocator;
    {
        NamePool pool(allocator);
        REQUIRE(pool.Initialize());
        auto weapon = pool.TryIntern("Weapon");
        auto armor = pool.TryIntern("Armor");
        auto numbered = pool.TryIntern("Weapon_2");
        auto variant = pool.TryIntern("WEAPON");
        REQUIRE(weapon && armor && numbered && variant);
        {
            OrderedMap<Name, float> map(allocator);
            REQUIRE(map.Insert(weapon.Value(), 1.0f));
            REQUIRE(map.Insert(armor.Value(), 2.0f));
            REQUIRE(map.Insert(numbered.Value(), 3.0f));
            auto duplicate = map.Insert(variant.Value(), 9.0f);
            REQUIRE(duplicate);
            CHECK(duplicate.Value() == InsertOutcome::AlreadyPresent);
            REQUIRE(map.Find(variant.Value()) != nullptr);
            CHECK(*map.Find(variant.Value()) == 1.0f);
            CHECK(map.Erase(armor.Value()));
            REQUIRE(map.Insert(armor.Value(), 4.0f));
            const Name expected[] = {weapon.Value(), numbered.Value(), armor.Value()};
            usize index = 0;
            for (auto entry : map) {
                CHECK(index < 3 && entry.key == expected[index]);
                ++index;
            }

            CHECK(index == 3);

            OrderedSet<Name> tags(allocator);
            REQUIRE(tags.Add(numbered.Value()));
            REQUIRE(tags.Add(weapon.Value()));
            auto again = tags.Add(variant.Value());
            REQUIRE(again);
            CHECK(again.Value() == InsertOutcome::AlreadyPresent);
            CHECK(tags.Count() == 2);
            CHECK(*tags.begin() == numbered.Value());
        }

        REQUIRE(pool.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 순서는 해시와 무관하므로 seed가 달라도 같은 연산이면 순서·값·결과가 같다.
TEST(OrderedMap_OrderAndResultsDoNotDependOnSeed) {
    SystemAllocator allocator;
    {
        OrderedMap<int32, int32> first(allocator, SeededHash<int32>(HashSeed{1, 2}));
        OrderedMap<int32, int32> second(allocator, SeededHash<int32>(HashSeed{3, 4}));
        OrderedMap<int32, int32> process(allocator);
        RandomStream random(11);
        for (int32 step = 0; step != 3000; ++step) {
            const int32 key = static_cast<int32>(random.NextUInt32Below(200));
            if (random.NextUInt32Below(3) == 0) {
                const bool erased = first.Erase(key);
                CHECK(second.Erase(key) == erased);
                CHECK(process.Erase(key) == erased);
            } else {
                auto inserted = first.InsertOrAssign(key, step);
                REQUIRE(inserted);
                auto other = second.InsertOrAssign(key, step);
                auto own = process.InsertOrAssign(key, step);
                REQUIRE(other && own);
                CHECK(other.Value() == inserted.Value());
                CHECK(own.Value() == inserted.Value());
            }

            if (step % 250 == 0) {
                CHECK(first == second);
                CHECK(first == process);
            }
        }

        CHECK(first == second);
        CHECK(first == process);

        OrderedSet<String> left(allocator, SeededHash<String>(HashSeed{7, 8}));
        OrderedSet<String> right(allocator, SeededHash<String>(HashSeed{9, 10}));
        const char* words[] = {"delta", "alpha", "charlie", "bravo"};
        for (const char* word : words) {
            String a(allocator);
            String b(allocator);
            REQUIRE(a.TryAssign(word) && b.TryAssign(word));
            REQUIRE(left.Add(Move(a)) && right.Add(Move(b)));
        }

        CHECK(left == right);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}
