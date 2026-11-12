#include <Core/Containers/SlotArray.h>
#include <Core/Math/RandomStream.h>
#include <Core/Memory/SystemAllocator.h>
#include <Tests/Support/NFailAllocator.h>
#include <Tests/Support/Test.h>

using namespace rocks;

namespace {

struct Tracked {
    static inline int live = 0;
    int value;
    const Tracked* self;

    explicit Tracked(int initial) noexcept : value(initial), self(this) {
        ++live;
    }
    Tracked(const Tracked& other) noexcept : value(other.value), self(this) {
        ++live;
    }
    Tracked(Tracked&& other) noexcept : value(other.value), self(this) {
        other.value = -1;
        ++live;
    }
    ~Tracked() noexcept {
        --live;
        self = nullptr;
    }
    Tracked& operator=(const Tracked&) = delete;

    bool Intact() const noexcept {
        return self == this;
    }
};

bool Holds(const SlotArray<Tracked>& slots, SlotHandle handle, int value) noexcept {
    const Tracked* found = slots.Find(handle);
    return found != nullptr && found->value == value && found->Intact();
}

} // namespace

TEST(SlotArray_HandlesBecomeStaleAfterRemoval) {
    SystemAllocator allocator;
    Tracked::live = 0;
    {
        SlotArray<Tracked> slots(allocator);
        auto a = slots.TryEmplace(1);
        auto b = slots.TryEmplace(2);
        auto c = slots.TryEmplace(3);
        REQUIRE(a && b && c);
        CHECK(a.Value() == (SlotHandle{0, 1}) && b.Value() == (SlotHandle{1, 1}));
        CHECK(slots.Size() == 3 && slots.SlotCount() == 3 && Tracked::live == 3);
        REQUIRE(slots.TryRemove(b.Value()));
        CHECK(!slots.IsValid(b.Value()) && slots.Find(b.Value()) == nullptr);
        CHECK(slots.TryRemove(b.Value()).Error() == ErrorCode::StaleHandle);
        // 같은 slot을 재사용하지만 generation이 달라 이전 handle은 계속 stale이다.
        auto d = slots.TryEmplace(4);
        REQUIRE(d);
        CHECK(d.Value() == (SlotHandle{1, 2}));
        CHECK(!slots.IsValid(b.Value()) && Holds(slots, d.Value(), 4));
        CHECK(Holds(slots, a.Value(), 1) && Holds(slots, c.Value(), 3));
        CHECK(slots[d.Value()].value == 4 && Tracked::live == 3);

        // 비어 있는·범위 밖·generation이 다른 handle.
        CHECK(!slots.IsValid(SlotHandle{}) && slots.Find(SlotHandle{}) == nullptr);
        CHECK(slots.TryRemove(SlotHandle{}).Error() == ErrorCode::InvalidArgument);
        CHECK(slots.TryRemove(SlotHandle{99, 1}).Error() == ErrorCode::StaleHandle);
        CHECK(slots.TryRemove(SlotHandle{0, 2}).Error() == ErrorCode::StaleHandle);
        CHECK(!slots.IsValid(SlotHandle{0, 0}) && slots.Size() == 3);
    }
    CHECK(Tracked::live == 0);
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SlotArray_GenerationLimitRetiresSlot) {
    SystemAllocator allocator;
    SlotArray<int> slots(allocator, 3);
    CHECK(slots.MaxGeneration() == 3);
    SlotHandle previous[3]{};
    for (uint32 generation = 1; generation <= 3; ++generation) {
        auto added = slots.TryAdd(static_cast<int>(generation));
        REQUIRE(added);
        CHECK(added.Value() == (SlotHandle{0, generation}));
        previous[generation - 1] = added.Value();
        REQUIRE(slots.TryRemove(added.Value()));
    }
    // generation 3에서 지운 slot 0은 폐기되어 다시 쓰지 않는다.
    CHECK(slots.RetiredCount() == 1 && slots.SlotCount() == 1 && slots.IsEmpty());
    auto fresh = slots.TryAdd(10);
    REQUIRE(fresh);
    CHECK(fresh.Value() == (SlotHandle{1, 1}));
    for (const SlotHandle& handle : previous) {
        CHECK(!slots.IsValid(handle));
    }
    // 폐기된 slot은 반복에도 나타나지 않는다.
    usize visits = 0;
    for (auto it = slots.begin(); it != slots.end(); ++it) {
        CHECK(it.Handle() == fresh.Value() && *it == 10);
        ++visits;
    }
    CHECK(visits == 1);
}

TEST(SlotArray_ClearInvalidatesHandlesAndKeepsSlots) {
    SystemAllocator allocator;
    Tracked::live = 0;
    {
        SlotArray<Tracked> slots(allocator);
        SlotHandle handles[5]{};
        for (int i = 0; i < 5; ++i) {
            auto added = slots.TryEmplace(i);
            REQUIRE(added);
            handles[i] = added.Value();
        }
        const usize capacity = slots.Capacity();
        slots.Clear();
        CHECK(slots.IsEmpty() && Tracked::live == 0 && slots.SlotCount() == 5);
        CHECK(slots.Capacity() == capacity);
        for (const SlotHandle& handle : handles) {
            CHECK(!slots.IsValid(handle));
        }
        // 재사용 slot은 새 generation을 받는다(Clear는 slot 역순으로 비우므로 0부터 재사용).
        auto reused = slots.TryEmplace(7);
        REQUIRE(reused);
        CHECK(reused.Value() == (SlotHandle{0, 2}) && !slots.IsValid(handles[0]));
    }
    CHECK(Tracked::live == 0);
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SlotArray_TakeReturnsValueAndInvalidatesHandle) {
    SystemAllocator allocator;
    Tracked::live = 0;
    {
        SlotArray<Tracked> slots(allocator);
        auto added = slots.TryEmplace(42);
        REQUIRE(added);
        auto taken = slots.TryTake(added.Value());
        REQUIRE(taken);
        CHECK(taken.Value().value == 42 && Tracked::live == 1 && slots.IsEmpty());
        CHECK(!slots.IsValid(added.Value()));
        auto again = slots.TryTake(added.Value());
        CHECK(!again && again.Error() == ErrorCode::StaleHandle);
        CHECK(slots.TryTake(SlotHandle{}).Error() == ErrorCode::InvalidArgument);
    }
    CHECK(Tracked::live == 0);
}

TEST(SlotArray_GrowthFailureAndSelfAliasPreserveState) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    Tracked::live = 0;
    {
        SlotArray<Tracked> slots(failing);
        SlotHandle handles[4]{};
        for (int i = 0; i < 4; ++i) {
            auto added = slots.TryEmplace(i);
            REQUIRE(added);
            handles[i] = added.Value();
        }
        REQUIRE(slots.SlotCount() == slots.Capacity());
        const usize capacity = slots.Capacity();
        failing.FailOnAttempt(1);
        auto failure = slots.TryEmplace(99);
        CHECK(!failure && failure.Error() == ErrorCode::OutOfMemory);
        failing.FailOnAttempt(1);
        auto reserve = slots.TryReserve(100);
        CHECK(!reserve && reserve.Error() == ErrorCode::OutOfMemory);
        failing.DisableFailure();
        CHECK(slots.Size() == 4 && slots.SlotCount() == 4 && slots.Capacity() == capacity);
        for (int i = 0; i < 4; ++i) {
            CHECK(Holds(slots, handles[i], i));
        }
        CHECK(Tracked::live == 4);
        // 확장하면서 기존 원소를 인자로 복사한다.
        auto copied = slots.TryAdd(slots[handles[2]]);
        REQUIRE(copied);
        CHECK(slots.Capacity() > capacity && Holds(slots, copied.Value(), 2));
        for (int i = 0; i < 4; ++i) {
            CHECK(Holds(slots, handles[i], i));
        }
        CHECK(slots.TryReserve(MaxSize).Error() == ErrorCode::Overflow);
    }
    CHECK(Tracked::live == 0);
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(SlotArray_IterationReportsLiveHandles) {
    SystemAllocator allocator;
    SlotArray<int> slots(allocator);
    SlotHandle handles[10]{};
    for (int i = 0; i < 10; ++i) {
        auto added = slots.TryAdd(i);
        REQUIRE(added);
        handles[i] = added.Value();
    }
    REQUIRE(slots.TryRemove(handles[0]));
    REQUIRE(slots.TryRemove(handles[4]));
    REQUIRE(slots.TryRemove(handles[9]));
    usize visits = 0;
    bool matches = true;
    uint32 previous = 0;
    for (auto it = slots.begin(); it != slots.end(); ++it) {
        const SlotHandle handle = it.Handle();
        matches = matches && slots.IsValid(handle) && *it == static_cast<int>(handle.index) &&
                  (visits == 0 || handle.index > previous);
        previous = handle.index;
        ++visits;
        if (handle.index == 5) {
            REQUIRE(slots.TryRemove(handle)); // 현재 원소를 지워도 계속 진행한다
            CHECK(it.Handle().IsNull());
        }
    }
    CHECK(visits == 7 && matches && slots.Size() == 6);
    const SlotArray<int>& constant = slots;
    int sum = 0;
    for (const int& value : constant) {
        sum += value;
    }
    CHECK(sum == 1 + 2 + 3 + 6 + 7 + 8);
}

TEST(SlotArray_MoveTransfersGenerations) {
    SystemAllocator allocator;
    SlotArray<int> source(allocator, 5);
    auto first = source.TryAdd(1);
    REQUIRE(first);
    REQUIRE(source.TryRemove(first.Value()));
    auto second = source.TryAdd(2);
    REQUIRE(second);
    SlotArray<int> moved(Move(source));
    CHECK(source.IsEmpty() && source.SlotCount() == 0 && source.Capacity() == 0);
    CHECK(moved.MaxGeneration() == 5 && moved.IsValid(second.Value()));
    CHECK(!moved.IsValid(first.Value()) && moved[second.Value()] == 2);
    SlotArray<int> assigned(allocator);
    REQUIRE(assigned.TryAdd(9));
    assigned = Move(moved);
    CHECK(assigned.IsValid(second.Value()) && assigned.Size() == 1);
    // 이동된 원본은 generation 1부터 다시 발급한다. 이전 기록의 handle(first)은 새 원소를 가리킬 수
    // 있으며 이는 문서화된 caller 책임이다(검출하지 않는다).
    auto reissued = source.TryAdd(3);
    REQUIRE(reissued);
    CHECK(reissued.Value() == (SlotHandle{0, 1}) && reissued.Value() == first.Value());
}

TEST(SlotArray_IteratorNoticesSlotReuse) {
    SystemAllocator allocator;
    SlotArray<int> slots(allocator);
    for (int i = 0; i < 4; ++i) {
        REQUIRE(slots.TryAdd(i));
    }
    usize visits = 0;
    for (auto it = slots.begin(); it != slots.end(); ++it) {
        ++visits;
        if (it.Handle().index == 1) {
            REQUIRE(slots.TryRemove(it.Handle()));
            auto replacement = slots.TryAdd(999); // 같은 slot 1을 새 generation으로 재사용한다
            REQUIRE(replacement);
            CHECK(replacement.Value() == (SlotHandle{1, 2}));
            // 반복자는 방문한 원소(generation 1)를 기억하므로 새 원소를 자기 원소로 보지 않는다.
            CHECK(it.Handle().IsNull());
        }
    }
    CHECK(visits == 4 && slots.Size() == 4);
}

TEST(SlotArray_MaxGenerationOneRetiresOnEveryRelease) {
    SystemAllocator allocator;
    Tracked::live = 0;
    {
        SlotArray<Tracked> slots(allocator, 1);
        auto a = slots.TryEmplace(1);
        auto b = slots.TryEmplace(2);
        auto c = slots.TryEmplace(3);
        REQUIRE(a && b && c);
        REQUIRE(slots.TryRemove(a.Value()));
        auto taken = slots.TryTake(b.Value());
        REQUIRE(taken);
        CHECK(taken.Value().value == 2);
        slots.Clear();
        CHECK(slots.RetiredCount() == 3 && slots.IsEmpty() && slots.SlotCount() == 3);
        auto fresh = slots.TryEmplace(4);
        REQUIRE(fresh);
        CHECK(fresh.Value() == (SlotHandle{3, 1}));
        CHECK(!slots.IsValid(a.Value()) && !slots.IsValid(b.Value()) && !slots.IsValid(c.Value()));
    }
    CHECK(Tracked::live == 0);
}

TEST(SlotArray_RandomizedOracle) {
    // 모델: slot마다 사용 여부·값·generation·폐기 여부와 free list 순서를 흉내 낸다. 낮은
    // maxGeneration으로 폐기 경로도 자주 지난다.
    constexpr usize Limit = 256;
    constexpr uint32 MaxGeneration = 6;
    SystemAllocator allocator;
    SlotArray<int> slots(allocator, MaxGeneration);
    bool used[Limit]{};
    bool retired[Limit]{};
    int values[Limit]{};
    uint32 generations[Limit]{};
    usize freeList[Limit]{};
    usize freeCount = 0;
    usize slotCount = 0;
    usize retiredCount = 0;
    SlotHandle issued[512]{};
    usize issuedCount = 0;
    auto release = [&](usize index) {
        used[index] = false;
        if (generations[index] >= MaxGeneration) {
            retired[index] = true;
            ++retiredCount;
            return;
        }
        ++generations[index];
        for (usize i = freeCount; i != 0; --i) {
            freeList[i] = freeList[i - 1];
        }
        freeList[0] = index;
        ++freeCount;
    };
    RandomStream random(test::CurrentSeed() ^ 0x510ull);
    for (int step = 0; step < 6000; ++step) {
        const uint32 operation = random.NextUInt32Below(8);
        const int value = static_cast<int>(random.NextUInt32Below(1000000));
        if (operation < 3) {
            usize expected = slotCount;
            if (freeCount != 0) {
                expected = freeList[0];
                for (usize i = 0; i + 1 < freeCount; ++i) {
                    freeList[i] = freeList[i + 1];
                }
                --freeCount;
            } else if (slotCount == Limit) {
                continue;
            } else {
                generations[slotCount++] = 1;
            }
            auto added = slots.TryAdd(value);
            REQUIRE(added);
            CHECK(added.Value() ==
                  (SlotHandle{static_cast<uint32>(expected), generations[expected]}));
            used[expected] = true;
            values[expected] = value;
            issued[issuedCount++ % 512] = added.Value();
        } else if (operation < 6 && issuedCount != 0) {
            // 과거에 발급한 handle(살아 있거나 stale)로 지운다.
            const usize pick =
                random.NextUInt32Below(static_cast<uint32>(issuedCount < 512 ? issuedCount : 512));
            const SlotHandle handle = issued[pick];
            const bool live = used[handle.index] && generations[handle.index] == handle.generation;
            const auto removed = slots.TryRemove(handle);
            CHECK(static_cast<bool>(removed) == live);
            if (live) {
                release(handle.index);
            } else {
                CHECK(removed.Error() == ErrorCode::StaleHandle);
            }
        } else if (operation == 6 && random.NextUInt32Below(32) == 0) {
            slots.Clear();
            for (usize index = slotCount; index != 0; --index) {
                if (used[index - 1]) {
                    release(index - 1);
                }
            }
        }
        usize size = 0;
        bool same = slots.SlotCount() == slotCount && slots.RetiredCount() == retiredCount;
        for (usize index = 0; index < slotCount && same; ++index) {
            const SlotHandle current{static_cast<uint32>(index), generations[index]};
            same = slots.IsValid(current) == used[index] &&
                   (!used[index] || slots[current] == values[index]) &&
                   (!retired[index] || !used[index]);
            size += used[index] ? 1 : 0;
        }
        CHECK(same && slots.Size() == size);
        if (!same) {
            break;
        }
    }
    // 폐기된 slot이 실제로 생겼는지(경로를 지났는지) 확인한다.
    CHECK(retiredCount != 0);
}
