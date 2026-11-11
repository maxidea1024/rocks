#include <Core/Containers/SparseArray.h>
#include <Core/Math/RandomStream.h>
#include <Core/Memory/SystemAllocator.h>
#include <Tests/Support/NFailAllocator.h>
#include <Tests/Support/Test.h>

using namespace rocks;

namespace {

// 수명과 이동을 추적한다. self는 객체가 제자리에서 생성·이동 생성되었는지(byte 복사가 아닌지) 본다.
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

bool Holds(const SparseArray<Tracked>& array, usize index, int value) noexcept {
    return array.IsValidIndex(index) && array[index].value == value && array[index].Intact();
}

} // namespace

TEST(SparseArray_IndicesStayStableAcrossRemovalAndGrowth) {
    SystemAllocator allocator;
    Tracked::live = 0;
    {
        SparseArray<Tracked> array(allocator);
        for (int i = 0; i < 5; ++i) {
            auto added = array.TryEmplace(i * 10);
            REQUIRE(added && added.Value() == static_cast<usize>(i));
        }
        REQUIRE(array.TryRemoveAt(1));
        REQUIRE(array.TryRemoveAt(3));
        CHECK(array.Size() == 3 && array.MaxIndex() == 5 && Tracked::live == 3);
        CHECK(!array.IsValidIndex(1) && !array.IsValidIndex(3) && !array.IsValidIndex(5));
        // 빈 자리가 없을 때까지 채운 뒤 용량을 넘겨도 기존 index의 값은 그대로다.
        for (int i = 0; i < 40; ++i) {
            REQUIRE(array.TryEmplace(100 + i));
        }
        CHECK(Holds(array, 0, 0) && Holds(array, 2, 20) && Holds(array, 4, 40));
        CHECK(array.Size() == 43 && array.MaxIndex() == 43 && Tracked::live == 43);
        for (usize index = 0; index < array.MaxIndex(); ++index) {
            CHECK(array.IsValidIndex(index) && array[index].Intact());
        }
    }
    CHECK(Tracked::live == 0);
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparseArray_FreeListReuseOrder) {
    SystemAllocator allocator;
    SparseArray<int> array(allocator);
    for (int i = 0; i < 6; ++i) {
        REQUIRE(array.TryAdd(i));
    }
    // 가장 최근에 지운 자리부터 재사용한다(LIFO).
    REQUIRE(array.TryRemoveAt(1));
    REQUIRE(array.TryRemoveAt(4));
    REQUIRE(array.TryRemoveAt(2));
    auto first = array.TryAdd(40);
    auto second = array.TryAdd(41);
    REQUIRE(first && second);
    CHECK(first.Value() == 2 && second.Value() == 4);
    // 가장 낮은 빈 index를 고른다.
    REQUIRE(array.TryRemoveAt(5));
    auto lowest = array.TryEmplaceAtLowestFreeIndex(50);
    REQUIRE(lowest);
    CHECK(lowest.Value() == 1);
    auto next = array.TryAdd(51);
    REQUIRE(next);
    CHECK(next.Value() == 5);
    auto appended = array.TryAdd(60);
    REQUIRE(appended);
    CHECK(appended.Value() == 6 && array.MaxIndex() == 7);

    // 지정 index: 사용 중이면 AlreadyExists, 빈 자리는 그 자리를 쓰고 free list에서 뺀다.
    auto occupied = array.TryEmplaceAt(3, 99);
    CHECK(!occupied && occupied.Error() == ErrorCode::AlreadyExists && array[3] == 3);
    REQUIRE(array.TryRemoveAt(0));
    REQUIRE(array.TryRemoveAt(3));
    auto exact = array.TryEmplaceAt(0, 70);
    REQUIRE(exact);
    CHECK(exact.Value() == 0 && array[0] == 70);
    auto reuse = array.TryAdd(71);
    REQUIRE(reuse);
    CHECK(reuse.Value() == 3);

    // MaxIndex() 너머는 사이를 빈 slot으로 채우고 낮은 index부터 재사용한다.
    auto far = array.TryEmplaceAt(10, 100);
    REQUIRE(far);
    CHECK(far.Value() == 10 && array.MaxIndex() == 11 && array.Size() == 8);
    for (usize index = 7; index < 10; ++index) {
        CHECK(!array.IsValidIndex(index));
    }
    for (usize expected = 7; expected < 10; ++expected) {
        auto filled = array.TryAdd(static_cast<int>(expected));
        REQUIRE(filled);
        CHECK(filled.Value() == expected);
    }
    CHECK(array.Size() == 11 && array.MaxIndex() == 11);
}

TEST(SparseArray_InvalidOperationsLeaveStateUnchanged) {
    SystemAllocator allocator;
    SparseArray<int> array(allocator);
    CHECK(array.TryRemoveAt(0).Error() == ErrorCode::InvalidArgument);
    REQUIRE(array.TryAdd(1));
    REQUIRE(array.TryAdd(2));
    REQUIRE(array.TryRemoveAt(0));
    CHECK(array.TryRemoveAt(0).Error() == ErrorCode::NotFound);
    CHECK(array.TryRemoveAt(2).Error() == ErrorCode::InvalidArgument);
    CHECK(array.TryRemoveAt(MaxSize).Error() == ErrorCode::InvalidArgument);
    CHECK(array.Size() == 1 && array.MaxIndex() == 2 && array[1] == 2);
    CHECK(!array.IsValidIndex(0) && !array.IsValidIndex(MaxSize));
    auto far = array.TryEmplaceAt(SparseArray<int>::MaxSlots, 5);
    CHECK(!far && far.Error() == ErrorCode::Overflow);
    CHECK(array.TryReserve(MaxSize).Error() == ErrorCode::Overflow);
    CHECK(array.Size() == 1 && array.MaxIndex() == 2);
}

TEST(SparseArray_IterationSkipsHolesAndToleratesRemoval) {
    SystemAllocator allocator;
    SparseArray<int> array(allocator);
    for (int i = 0; i < 130; ++i) {
        REQUIRE(array.TryAdd(i));
    }
    // word 경계(63·64·127·128)를 포함해 지운다.
    const usize removed[] = {0, 5, 63, 64, 100, 127, 129};
    for (usize index : removed) {
        REQUIRE(array.TryRemoveAt(index));
    }
    usize visits = 0;
    usize previous = 0;
    bool ascending = true;
    bool matches = true;
    for (auto it = array.begin(); it != array.end(); ++it) {
        ascending = ascending && (visits == 0 || it.Index() > previous);
        matches = matches && *it == static_cast<int>(it.Index());
        previous = it.Index();
        ++visits;
    }
    CHECK(visits == 123 && ascending && matches);

    // 반복 중 현재 원소와 앞쪽·뒤쪽 원소를 지워도 남은 원소를 정확히 한 번 방문한다.
    usize seen = 0;
    for (auto it = array.begin(); it != array.end(); ++it) {
        const usize index = it.Index();
        ++seen;
        if (index % 3 == 0) {
            REQUIRE(array.TryRemoveAt(index));
        }
        if (index == 10) {
            REQUIRE(array.TryRemoveAt(11)); // 아직 방문하지 않은 원소
        }
    }
    CHECK(seen == 122);

    const SparseArray<int>& constant = array;
    usize total = 0;
    for (const int& value : constant) {
        total += static_cast<usize>(value % 3 != 0);
    }
    CHECK(total == constant.Size());

    // 반복 중 용량을 넘겨 추가해도 반복자는 index로 계속 진행한다.
    usize grownVisits = 0;
    const usize before = array.MaxIndex();
    for (auto it = array.begin(); it != array.end(); ++it) {
        if (grownVisits == 0) {
            for (int i = 0; i < 300; ++i) {
                REQUIRE(array.TryEmplaceAt(before + static_cast<usize>(i), 1000 + i));
            }
        }
        ++grownVisits;
    }
    CHECK(grownVisits == array.Size());
}

TEST(SparseArray_CompactAndCompactStable) {
    SystemAllocator allocator;
    Tracked::live = 0;
    {
        SparseArray<Tracked> stable(allocator);
        SparseArray<Tracked> unordered(allocator);
        for (int i = 0; i < 10; ++i) {
            REQUIRE(stable.TryEmplace(i));
            REQUIRE(unordered.TryEmplace(i));
        }
        const usize holes[] = {1, 2, 5, 9};
        for (usize index : holes) {
            REQUIRE(stable.TryRemoveAt(index));
            REQUIRE(unordered.TryRemoveAt(index));
        }
        CHECK(stable.CompactStable());
        const int stableOrder[] = {0, 3, 4, 6, 7, 8};
        CHECK(stable.Size() == 6 && stable.MaxIndex() == 6);
        for (usize index = 0; index < 6; ++index) {
            CHECK(Holds(stable, index, stableOrder[index]));
        }
        // 끝 원소를 앞의 빈 자리로 옮긴다: 1←8, 2←7, 5←6.
        CHECK(unordered.Compact());
        const int compactOrder[] = {0, 8, 7, 3, 4, 6};
        CHECK(unordered.Size() == 6 && unordered.MaxIndex() == 6);
        for (usize index = 0; index < 6; ++index) {
            CHECK(Holds(unordered, index, compactOrder[index]));
        }
        CHECK(Tracked::live == 12);
        // 빈 자리가 없으면 옮기지 않으며 이후 추가는 끝에 붙는다.
        CHECK(!stable.Compact() && !stable.CompactStable());
        auto added = stable.TryEmplace(42);
        REQUIRE(added);
        CHECK(added.Value() == 6);
        // 끝의 빈 자리만 있으면 옮기지 않고 MaxIndex()만 줄인다.
        REQUIRE(stable.TryRemoveAt(6));
        CHECK(!stable.Compact() && stable.MaxIndex() == 6);
    }
    CHECK(Tracked::live == 0);
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparseArray_ShrinkTrimsTailAndReserveKeepsMaxIndex) {
    SystemAllocator allocator;
    SparseArray<int> array(allocator);
    REQUIRE(array.TryReserve(100));
    CHECK(array.Capacity() == 100 && array.MaxIndex() == 0 && array.IsEmpty());
    for (int i = 0; i < 20; ++i) {
        REQUIRE(array.TryAdd(i));
    }
    for (usize index = 10; index < 20; ++index) {
        REQUIRE(array.TryRemoveAt(index));
    }
    REQUIRE(array.TryRemoveAt(3));
    REQUIRE(array.TryShrink());
    CHECK(array.MaxIndex() == 10 && array.Capacity() == 10 && array.Size() == 9);
    // 사이의 빈 자리(3)는 남고 다음 추가가 쓴다.
    auto reused = array.TryAdd(33);
    REQUIRE(reused);
    CHECK(reused.Value() == 3);
    auto appended = array.TryAdd(34);
    REQUIRE(appended);
    CHECK(appended.Value() == 10);
    for (int i = 0; i < 9; ++i) {
        CHECK(array[static_cast<usize>(i)] == (i == 3 ? 33 : i));
    }
    // 모두 지우고 줄이면 저장소를 반환한다.
    array.Clear();
    CHECK(array.IsEmpty() && array.MaxIndex() == 0 && array.Capacity() != 0);
    REQUIRE(array.TryShrink());
    CHECK(array.Capacity() == 0);
    REQUIRE(array.TryAdd(1));
    array.Empty();
    CHECK(array.Capacity() == 0 && array.MaxIndex() == 0);
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparseArray_GrowthFailurePreservesState) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    Tracked::live = 0;
    {
        SparseArray<Tracked> array(failing);
        for (int i = 0; i < 4; ++i) {
            REQUIRE(array.TryEmplace(i));
        }
        REQUIRE(array.TryRemoveAt(2));
        REQUIRE(array.TryEmplace(20)); // 빈 자리 재사용은 할당하지 않는다
        REQUIRE(array.MaxIndex() == array.Capacity());
        const usize capacity = array.Capacity();
        const Tracked* address = &array[0];
        auto unchanged = [&]() {
            return Holds(array, 0, 0) && Holds(array, 1, 1) && Holds(array, 2, 20) &&
                   Holds(array, 3, 3) && array.Size() == 4 && array.MaxIndex() == 4 &&
                   array.Capacity() == capacity && &array[0] == address && Tracked::live == 4;
        };
        // 원소 저장소 할당 실패(사용 표시 bit는 64 단위라 이미 충분하다).
        failing.FailOnAttempt(1);
        auto nodes = array.TryEmplace(99);
        CHECK(!nodes && nodes.Error() == ErrorCode::OutOfMemory && failing.Attempts() == 1);
        CHECK(unchanged());
        // 사용 표시 bit 할당 실패와, bit는 늘었지만 원소 저장소가 실패한 경우.
        for (uint64 attempt = 1; attempt <= 2; ++attempt) {
            failing.FailOnAttempt(attempt);
            auto far = array.TryEmplaceAt(200 * attempt, 99);
            CHECK(!far && far.Error() == ErrorCode::OutOfMemory && failing.Attempts() == attempt);
            CHECK(unchanged());
        }
        failing.DisableFailure();
        // 실패 뒤에도 정상적으로 확장하고 빈 자리 순서도 그대로다.
        REQUIRE(array.TryRemoveAt(1));
        auto reused = array.TryEmplace(11);
        REQUIRE(reused);
        CHECK(reused.Value() == 1);
        for (int i = 0; i < 20; ++i) {
            REQUIRE(array.TryEmplace(100 + i));
        }
        CHECK(Holds(array, 3, 3) && Holds(array, 1, 11) && array.Size() == 24);
    }
    CHECK(Tracked::live == 0);
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(SparseArray_ReserveFailurePreservesState) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    SparseArray<int> array(failing);
    REQUIRE(array.TryAdd(7));
    for (uint64 attempt = 1; attempt <= 2; ++attempt) {
        failing.FailOnAttempt(attempt);
        auto reserved = array.TryReserve(1000 * attempt);
        CHECK(!reserved && reserved.Error() == ErrorCode::OutOfMemory);
        failing.DisableFailure();
        CHECK(array.Size() == 1 && array.MaxIndex() == 1 && array[0] == 7);
        CHECK(array.Capacity() < 1000);
    }
}

TEST(SparseArray_SelfAliasedArgumentDuringGrowth) {
    SystemAllocator allocator;
    Tracked::live = 0;
    {
        SparseArray<Tracked> array(allocator);
        REQUIRE(array.TryEmplace(5));
        while (array.MaxIndex() < array.Capacity()) {
            REQUIRE(array.TryEmplace(6));
        }
        const usize capacity = array.Capacity();
        auto copied = array.TryAdd(array[0]); // 확장하며 기존 원소를 복사한다
        REQUIRE(copied);
        CHECK(array.Capacity() > capacity);
        CHECK(Holds(array, copied.Value(), 5) && Holds(array, 0, 5));
        auto far = array.TryEmplaceAt(array.Capacity() + 3, array[0]);
        REQUIRE(far);
        CHECK(Holds(array, far.Value(), 5) && Holds(array, 0, 5));
    }
    CHECK(Tracked::live == 0);
}

TEST(SparseArray_MoveTransfersOwnership) {
    SystemAllocator allocator;
    SystemAllocator other;
    Tracked::live = 0;
    {
        SparseArray<Tracked> source(allocator);
        for (int i = 0; i < 6; ++i) {
            REQUIRE(source.TryEmplace(i));
        }
        REQUIRE(source.TryRemoveAt(2));
        SparseArray<Tracked> moved(Move(source));
        CHECK(source.IsEmpty() && source.MaxIndex() == 0 && source.Capacity() == 0);
        CHECK(moved.Size() == 5 && !moved.IsValidIndex(2) && Holds(moved, 5, 5));
        auto reused = moved.TryEmplace(22);
        REQUIRE(reused);
        CHECK(reused.Value() == 2);
        REQUIRE(source.TryEmplace(1)); // 이동 뒤 원본을 다시 쓸 수 있다
        SparseArray<Tracked> assigned(other);
        REQUIRE(assigned.TryEmplace(9));
        assigned = Move(moved);
        CHECK(other.Stats().liveAllocations == 0);
        CHECK(&assigned.GetAllocator() == &allocator && assigned.Size() == 6);
        CHECK(Tracked::live == 7);
    }
    CHECK(Tracked::live == 0);
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparseArray_RandomizedOracle) {
    // 모델: 사용 여부·값과 free list 순서(앞이 다음 재사용)를 그대로 흉내 낸다.
    constexpr usize Limit = 160;
    SystemAllocator allocator;
    SparseArray<int> array(allocator);
    bool used[Limit]{};
    int values[Limit]{};
    usize freeList[Limit]{};
    usize freeCount = 0;
    usize maxIndex = 0;
    auto pushFree = [&](usize index) {
        for (usize i = freeCount; i != 0; --i) {
            freeList[i] = freeList[i - 1];
        }
        freeList[0] = index;
        ++freeCount;
    };
    auto unlink = [&](usize index) {
        usize at = 0;
        while (freeList[at] != index) {
            ++at;
        }
        for (usize i = at; i + 1 < freeCount; ++i) {
            freeList[i] = freeList[i + 1];
        }
        --freeCount;
    };
    RandomStream random(test::CurrentSeed() ^ 0x5a17ull);
    for (int step = 0; step < 6000; ++step) {
        const uint32 operation = random.NextUInt32Below(10);
        const int value = static_cast<int>(random.NextUInt32Below(1000000));
        if (operation < 3 && maxIndex < Limit) {
            usize expected = maxIndex;
            if (freeCount != 0) {
                expected = freeList[0];
                unlink(expected);
            }
            auto added = array.TryAdd(value);
            REQUIRE(added);
            CHECK(added.Value() == expected);
            if (expected == maxIndex) {
                ++maxIndex;
            }
            used[expected] = true;
            values[expected] = value;
        } else if (operation < 5) {
            const usize index = random.NextUInt32Below(static_cast<uint32>(maxIndex + 2));
            const auto removed = array.TryRemoveAt(index);
            if (index < maxIndex && used[index]) {
                CHECK(removed);
                used[index] = false;
                pushFree(index);
            } else {
                CHECK(!removed);
            }
        } else if (operation == 5 && maxIndex < Limit) {
            usize expected = maxIndex;
            for (usize i = 0; i < maxIndex; ++i) {
                if (!used[i]) {
                    expected = i;
                    break;
                }
            }
            if (expected != maxIndex) {
                unlink(expected);
            } else {
                ++maxIndex;
            }
            auto added = array.TryEmplaceAtLowestFreeIndex(value);
            REQUIRE(added);
            CHECK(added.Value() == expected);
            used[expected] = true;
            values[expected] = value;
        } else if (operation == 6) {
            const usize index = random.NextUInt32Below(static_cast<uint32>(Limit));
            auto added = array.TryEmplaceAt(index, value);
            if (index < maxIndex && used[index]) {
                CHECK(!added && added.Error() == ErrorCode::AlreadyExists);
            } else {
                REQUIRE(added);
                CHECK(added.Value() == index);
                if (index < maxIndex) {
                    unlink(index);
                } else {
                    for (usize slot = index; slot > maxIndex; --slot) {
                        pushFree(slot - 1);
                    }
                    maxIndex = index + 1;
                }
                used[index] = true;
                values[index] = value;
            }
        } else if (operation == 7 && random.NextUInt32Below(8) == 0) {
            const bool stable = random.NextUInt32Below(2) == 0;
            usize write = 0;
            if (stable) {
                array.CompactStable();
                for (usize read = 0; read < maxIndex; ++read) {
                    if (used[read]) {
                        values[write++] = values[read];
                    }
                }
            } else {
                array.Compact();
                usize last = maxIndex;
                usize size = 0;
                for (usize i = 0; i < maxIndex; ++i) {
                    size += used[i] ? 1 : 0;
                }
                for (usize hole = 0; hole < size; ++hole) {
                    if (used[hole]) {
                        continue;
                    }
                    do {
                        --last;
                    } while (!used[last]);
                    values[hole] = values[last];
                    used[hole] = true;
                    used[last] = false;
                }
                write = size;
            }
            for (usize i = 0; i < Limit; ++i) {
                used[i] = i < write;
            }
            maxIndex = write;
            freeCount = 0;
        } else if (operation == 8 && random.NextUInt32Below(8) == 0) {
            REQUIRE(array.TryShrink());
            while (maxIndex != 0 && !used[maxIndex - 1]) {
                unlink(maxIndex - 1);
                --maxIndex;
            }
        } else if (operation == 9 && random.NextUInt32Below(64) == 0) {
            array.Clear();
            for (usize i = 0; i < Limit; ++i) {
                used[i] = false;
            }
            maxIndex = 0;
            freeCount = 0;
        }
        // 매 단계 전체 상태를 대조한다.
        usize size = 0;
        bool same = array.MaxIndex() == maxIndex;
        for (usize i = 0; i < maxIndex && same; ++i) {
            same = array.IsValidIndex(i) == used[i] && (!used[i] || array[i] == values[i]);
            size += used[i] ? 1 : 0;
        }
        CHECK(same && array.Size() == size && array.MaxIndex() - array.Size() == freeCount);
        if (!same) {
            break;
        }
    }
}
