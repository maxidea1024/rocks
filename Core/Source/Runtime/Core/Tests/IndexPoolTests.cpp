#include <Core/Containers/IndexPool.h>
#include <Core/Math/RandomStream.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace index_pool_tests_file {

namespace {

struct Record {
    uint32 a = 0;
    uint64 b = 0;
};

} // namespace

TEST(IndexPool_ReusesFreedIndicesLastInFirstOut) {
    SystemAllocator allocator;
    IndexPool<Record> pool(allocator);
    auto first = pool.TryAllocate();
    auto second = pool.TryAllocate();
    auto third = pool.TryAllocate();
    REQUIRE(first && second && third);
    CHECK(first.Value() == 0 && second.Value() == 1 && third.Value() == 2);
    CHECK(pool.LiveCount() == 3 && pool.SlotCount() == 3 && pool.FreeCount() == 0);

    pool[second.Value()].a = 7;
    pool.Free(first.Value());
    pool.Free(third.Value());
    CHECK(!pool.IsLive(first.Value()) && pool.IsLive(second.Value()) && !pool.IsLive(9));
    CHECK(pool.LiveCount() == 1 && pool.FreeCount() == 2);

    // 가장 최근에 반환한 색인부터 다시 주고, 다시 준 원소는 값 초기화되어 있다.
    pool[third.Value()].a = 99;
    auto reused = pool.TryAllocate();
    REQUIRE(reused);
    CHECK(reused.Value() == third.Value() && pool[reused.Value()].a == 0);
    auto reusedAgain = pool.TryAllocate();
    REQUIRE(reusedAgain);
    CHECK(reusedAgain.Value() == first.Value());
    CHECK(pool[second.Value()].a == 7);

    pool.Clear();
    CHECK(pool.LiveCount() == 0 && pool.SlotCount() == 0 && pool.Capacity() >= 3);
    auto restarted = pool.TryAllocate();
    REQUIRE(restarted);
    CHECK(restarted.Value() == 0);
}

// 확보한 뒤의 할당·반환은 allocator를 부르지 않는다. 확보 실패는 원소와 색인을 바꾸지 않는다.
TEST(IndexPool_ReservedAllocationNeverAllocates) {
    SystemAllocator system;
    test::NFailAllocator allocator(system);
    IndexPool<Record> pool(allocator);
    REQUIRE(pool.TryReserveAdditional(10));
    CHECK(pool.ReservedAvailable() >= 10);

    allocator.FailOnAttempt(1);
    uint32 indices[10];
    for (uint32 index = 0; index < 10; ++index) {
        indices[index] = pool.AllocateReserved();
        pool[indices[index]].b = index;
    }
    for (uint32 index = 0; index < 10; index += 2) {
        pool.Free(indices[index]);
    }
    CHECK(allocator.Attempts() == 0);
    allocator.DisableFailure();

    // 재사용 목록 5개로 충분하므로 할당하지 않는다.
    allocator.FailOnAttempt(1);
    CHECK(pool.TryReserveAdditional(5));
    CHECK(allocator.Attempts() == 0);

    // 모자라면 늘려야 하는데 실패하면 내용은 그대로다.
    const usize capacity = pool.Capacity();
    allocator.FailOnAttempt(1);
    CHECK(pool.TryReserveAdditional(static_cast<uint32>(capacity) + 1).Error() ==
          ErrorCode::OutOfMemory);
    allocator.FailOnAttempt(2);
    CHECK(pool.TryReserveAdditional(static_cast<uint32>(capacity) + 1).Error() ==
          ErrorCode::OutOfMemory);
    allocator.DisableFailure();
    CHECK(pool.LiveCount() == 5 && pool.SlotCount() == 10 && pool.Capacity() == capacity);
    for (uint32 index = 1; index < 10; index += 2) {
        CHECK(pool.IsLive(indices[index]) && pool[indices[index]].b == index);
    }
}

TEST(IndexPool_MaxElementsBoundsIndexWidth) {
    SystemAllocator allocator;
    IndexPool<Record> pool(allocator, 3);
    REQUIRE(pool.TryReserveAdditional(3));
    CHECK(pool.TryReserveAdditional(4).Error() == ErrorCode::Overflow);
    CHECK(pool.TryReserve(4).Error() == ErrorCode::Overflow);
    REQUIRE(pool.TryAllocate());
    REQUIRE(pool.TryAllocate());
    REQUIRE(pool.TryAllocate());
    CHECK(pool.TryAllocate().Error() == ErrorCode::Overflow);
    pool.Free(1);
    auto again = pool.TryAllocate();
    REQUIRE(again);
    CHECK(again.Value() == 1);
}

TEST(IndexPool_MoveTransfersIndicesAndShrinkKeepsThem) {
    SystemAllocator allocator;
    IndexPool<Record> pool(allocator);
    for (uint32 index = 0; index < 40; ++index) {
        auto allocated = pool.TryAllocate();
        REQUIRE(allocated);
        pool[allocated.Value()].a = index + 100;
    }
    for (uint32 index = 0; index < 40; index += 3) {
        pool.Free(index);
    }

    IndexPool<Record> moved(Move(pool));
    CHECK(pool.LiveCount() == 0 && pool.SlotCount() == 0);
    CHECK(moved.SlotCount() == 40 && moved.FreeCount() == 14);
    CHECK(moved.IsLive(1) && moved[1].a == 101 && !moved.IsLive(3));

    REQUIRE(moved.TryShrink());
    CHECK(moved.Capacity() == 40);
    CHECK(moved.IsLive(38) && moved[38].a == 138);
    // 재사용 목록은 축소 뒤에도 그대로다(마지막에 반환한 39부터).
    auto reused = moved.TryAllocate();
    REQUIRE(reused);
    CHECK(reused.Value() == 39);

    // 원본은 같은 allocator로 다시 쓸 수 있다.
    auto fresh = pool.TryAllocate();
    REQUIRE(fresh);
    CHECK(fresh.Value() == 0);

    for (uint32 index = 0; index < 40; ++index) {
        if (moved.IsLive(index)) {
            moved.Free(index);
        }
    }
    REQUIRE(moved.TryShrink());
    CHECK(moved.SlotCount() == 0 && moved.Capacity() == 0);
}

// 무작위 할당·반환을 단순 모형과 대조한다.
TEST(IndexPool_RandomizedOracle) {
    SystemAllocator allocator;
    IndexPool<Record> pool(allocator);
    constexpr uint32 Limit = 300;
    bool live[Limit] = {};
    uint32 liveCount = 0;
    RandomStream random(test::CurrentSeed() ^ 0x1D3E);
    for (uint32 step = 0; step < 20000; ++step) {
        if (random.NextUInt32Below(2) == 0 && pool.SlotCount() < Limit - 1) {
            auto allocated = pool.TryAllocate();
            REQUIRE(allocated);
            CHECK(!live[allocated.Value()]);
            live[allocated.Value()] = true;
            pool[allocated.Value()].a = allocated.Value();
            ++liveCount;
        } else if (liveCount != 0) {
            uint32 candidate = random.NextUInt32Below(pool.SlotCount());
            while (!live[candidate]) {
                candidate = (candidate + 1) % pool.SlotCount();
            }
            CHECK(pool[candidate].a == candidate);
            pool.Free(candidate);
            live[candidate] = false;
            --liveCount;
        }
        CHECK(pool.LiveCount() == liveCount);
    }
    for (uint32 index = 0; index < pool.SlotCount(); ++index) {
        CHECK(pool.IsLive(index) == live[index]);
    }
}
} // namespace index_pool_tests_file
