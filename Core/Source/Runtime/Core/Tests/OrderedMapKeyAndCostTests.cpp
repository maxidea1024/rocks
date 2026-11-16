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

namespace {

// 할당 byte를 센다(Reserve가 실제로 잡는 저장소 크기).
struct ByteCounting final : Allocator {
    explicit ByteCounting(Allocator& inner) noexcept : inner_(inner) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        total += bytes;
        ++calls;
        return inner_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* value) noexcept override {
        return inner_.Deallocate(value);
    }

    usize total = 0;
    usize calls = 0;

private:
    Allocator& inner_;
};

// 빈 컨테이너 C의 Reserve(count)가 할당한 byte다(할당이 없으면 0).
template <class C>
usize ReservedByAllocator(usize count) noexcept {
    SystemAllocator system;
    ByteCounting counting(system);
    {
        C container(counting);
        CHECK(container.Reserve(count));
        CHECK(counting.calls <= 1);
    }

    return counting.total;
}

template <class C>
void CheckReservedBytes() noexcept {
    const usize counts[] = {0, 1, 3, 4, 5, 511, 512, 513, 1023, 1024, 1025, 65536, 65537};
    for (usize count : counts) {
        usize predicted = 0;
        REQUIRE(C::ReservedBytes(count, predicted));
        CHECK(predicted == ReservedByAllocator<C>(count));
    }
}

struct Wide {
    uint64 a = 0;
    uint64 b = 0;
    uint8 c = 0;
};

} // namespace

// ReservedBytes(count)는 빈 컨테이너의 Reserve(count)가 실제로 할당하는 byte와 같다(항목의
// hash·정렬 padding, 2의 거듭제곱 용량 올림, 색인 칸 포함 — 리플렉션 B9f2의 2단계 몫 선청구가 이
// 값을 쓴다). 키·값 크기와 정렬이 다른 여러 형식, 2의 거듭제곱 경계 앞뒤의 count에서 확인하고,
// Clear한 뒤의 Reserve는 이보다 많이 잡지 않으며, MaximumCapacity를 넘는 count는 false다.
TEST(OrderedMap_ReservedBytesMatchesReserveAllocation) {
    CheckReservedBytes<OrderedMap<uint32, int32>>();
    CheckReservedBytes<OrderedMap<String, float>>();
    CheckReservedBytes<OrderedMap<uint8, Wide>>();
    CheckReservedBytes<OrderedMap<Guid, uint64>>();
    CheckReservedBytes<OrderedSet<bool>>();
    CheckReservedBytes<OrderedSet<uint32>>();
    CheckReservedBytes<OrderedSet<String>>();

    SystemAllocator system;
    ByteCounting counting(system);
    {
        OrderedMap<uint32, int32> map(counting);
        REQUIRE(map.Reserve(100));
        REQUIRE(map.Insert(uint32{1}, int32{1}));
        map.Clear();
        const usize before = counting.total;
        REQUIRE(map.Reserve(300));
        usize bound = 0;
        REQUIRE((OrderedMap<uint32, int32>::ReservedBytes(300, bound)));
        CHECK(counting.total - before <= bound);
    }

    usize bytes = 0;
    CHECK(!(OrderedMap<uint32, int32>::ReservedBytes(OrderedMap<uint32, int32>::MaximumCapacity + 1,
                                                     bytes)));
}

namespace {

// 해시·동등 비교 호출 수를 센다. 모든 키가 한 색인 칸에서 시작하므로 조회 한 번의 동등 비교 수는
// 그 앞에 놓인 산 원소 수다.
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

} // namespace

// FindOrAddWith는 키가 없을 때도 해시 한 번·동등 비교 한 바퀴만 한다(factory 동안 구조 변경이 막혀
// 키 없음이 유지되므로 붙일 때 다시 조회하지 않는다). 이전에는 해시 2번·비교 2바퀴였다.
TEST(OrderedMap_FindOrAddWithLooksUpOnce) {
    SystemAllocator allocator;
    {
        OrderedMap<int32, int32, CountingZeroHash, CountingEqual> map(allocator);
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
        const int32 expected[] = {0, 1, 2, 9};
        CHECK(KeysAre(map, expected, 4));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
