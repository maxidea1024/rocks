#include <Core/Containers/BoundedSpscQueue.h>
#include <Core/Containers/IndexedFreeList.h>
#include <Core/Containers/IntrusiveMpscQueue.h>
#include <Core/Containers/TakeAllStack.h>
#include <Core/Memory/FixedBlockPool.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Atomics.h>
#include <Platform/Thread.h>
#include <TestSupport/StepScheduler.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace concurrent_container_tests_file {

using platform::MemoryOrder;

// Core 동시 컨테이너(Docs/Design/Synchronization 12.2절, SY7)를 본다. 단일 스레드 의미, 여러 스레드
// 경합의 결정적 결과(정확히 한 번·순서·수), 그리고 작은 시나리오의 모든 interleaving(14.3절 단계
// 스케줄러)을 단언한다. IndexedFreeList는 작은 태그 시험판으로 ABA를 재현하고 기본 태그는 막음을
// 본다.
namespace {

constexpr uint32 SpscCapacity = 4;
constexpr uint32 StressItems = 20000;
constexpr uint32 Producers = 4;

// ---- BoundedSpscQueue ----

struct SpscStress {
    BoundedSpscQueue<uint32> queue;
    uint32 received[StressItems] = {};
};

void ProduceSequence(void* raw) noexcept {
    auto& context = *static_cast<SpscStress*>(raw);
    for (uint32 value = 0; value < StressItems;) {
        if (context.queue.TryPush(value)) {
            ++value;
        } else {
            platform::YieldThread();
        }
    }
}

struct SpscScenario {
    BoundedSpscQueue<uint32, test::ScheduledSteps> queue;
    uint32 received[2] = {};
    uint32 count = 0;
};

void SpscActor(void* raw, uint32 actor) noexcept {
    auto& context = *static_cast<SpscScenario*>(raw);
    if (actor == 0) {
        for (uint32 value = 1; value <= 2;) {
            if (context.queue.TryPush(value)) {
                ++value;
            } else {
                test::StepScheduler::Blocked("producer.retry");
            }
        }

        return;
    }

    while (context.count < 2) {
        uint32 value = 0;
        if (context.queue.TryPop(value)) {
            context.received[context.count++] = value;
        } else {
            test::StepScheduler::Blocked("consumer.retry");
        }
    }
}

// ---- IntrusiveMpscQueue ----

struct MpscItem : IntrusiveMpscNode {
    uint32 producer = 0;
    uint32 sequence = 0;
};

struct MpscStress {
    IntrusiveMpscQueue<MpscItem> queue;
    MpscItem items[Producers][StressItems / Producers];
    platform::Atomic<uint32> nextProducer;
};

void ProduceItems(void* raw) noexcept {
    auto& context = *static_cast<MpscStress*>(raw);
    const uint32 producer = context.nextProducer.FetchAdd<MemoryOrder::Relaxed>(1);
    for (uint32 index = 0; index < StressItems / Producers; ++index) {
        MpscItem& item = context.items[producer][index];
        item.producer = producer;
        item.sequence = index;
        context.queue.Push(&item);
    }
}

struct MpscScenario {
    IntrusiveMpscQueue<MpscItem, test::ScheduledSteps> queue;
    MpscItem items[2];
    uint32 seen[2] = {};
    uint32 count = 0;
};

void MpscActor(void* raw, uint32 actor) noexcept {
    auto& context = *static_cast<MpscScenario*>(raw);
    if (actor < 2) {
        context.items[actor].producer = actor;
        context.queue.Push(&context.items[actor]);
        return;
    }

    while (context.count < 2) {
        if (MpscItem* item = context.queue.TryPop(); item != nullptr) {
            ++context.seen[item->producer];
            ++context.count;
        } else {
            test::StepScheduler::Blocked("consumer.retry");
        }
    }
}

// ---- TakeAllStack ----

struct StackItem : TakeAllStackNode {
    uint32 value = 0;
};

struct StackScenario {
    TakeAllStack<StackItem, test::ScheduledSteps> stack;
    StackItem items[2];
    uint32 seen[2] = {};
    uint32 count = 0;
};

void StackActor(void* raw, uint32 actor) noexcept {
    auto& context = *static_cast<StackScenario*>(raw);
    if (actor < 2) {
        context.items[actor].value = actor;
        if (!context.stack.Push(&context.items[actor])) {
            platform::Fatal("TakeAllStack rejected a push before Close");
        }

        return;
    }

    while (context.count < 2) {
        StackItem* list = context.stack.TakeAll();
        if (list == nullptr) {
            test::StepScheduler::Blocked("consumer.retry");
        }

        for (; list != nullptr; list = TakeAllStack<StackItem, test::ScheduledSteps>::Next(list)) {
            ++context.seen[list->value];
            ++context.count;
        }
    }
}

// ---- IndexedFreeList ----

constexpr uint32 FreeListCapacity = 3;
constexpr uint32 SmallTagBits = 2;

template <uint32 TagBits>
struct FreeListScenario {
    IndexedFreeList<24, TagBits, test::ScheduledSteps> list;
    uint32 popped[2][FreeListCapacity] = {};
    uint32 poppedCount[2] = {};
};

// 배우 0: 하나를 꺼낸다. 배우 1: 셋을 꺼내고 첫 것을 돌려준다(태그를 네 번 올려 2-bit 태그를
// 감는다).
template <uint32 TagBits>
void AbaActor(void* raw, uint32 actor) noexcept {
    auto& context = *static_cast<FreeListScenario<TagBits>*>(raw);
    uint32 index = 0;
    if (actor == 0) {
        if (context.list.TryPop(index)) {
            context.popped[0][context.poppedCount[0]++] = index;
        }

        return;
    }

    for (uint32 round = 0; round < FreeListCapacity; ++round) {
        if (context.list.TryPop(index)) {
            context.popped[1][context.poppedCount[1]++] = index;
        }
    }

    if (context.poppedCount[1] != 0) {
        context.list.Push(context.popped[1][0]);
        context.popped[1][0] = context.popped[1][--context.poppedCount[1]];
    }
}

struct FreeListExplore {
    IndexedFreeList<24, 40, test::ScheduledSteps> list;
    uint32 held[2] = {};
    bool got[2] = {};
};

void FreeListPopPushActor(void* raw, uint32 actor) noexcept {
    auto& context = *static_cast<FreeListExplore*>(raw);
    uint32 index = 0;
    context.got[actor] = context.list.TryPop(index);
    context.held[actor] = index;
}

struct PoolStress {
    FixedBlockPool pool;
    platform::Atomic<uint32> failures;
};

void AllocateAndFree(void* raw) noexcept {
    auto& context = *static_cast<PoolStress*>(raw);
    for (uint32 round = 0; round < StressItems / Producers; ++round) {
        auto block = context.pool.TryAllocate();
        if (!block) {
            context.failures.FetchAdd<MemoryOrder::Relaxed>(1);
            continue;
        }

        // 블록을 이 스레드만 쓰는지 본다: 표지를 쓰고 다시 읽는다.
        auto* mark = static_cast<uint32*>(block.Value().data);
        *mark = block.Value().index;
        platform::YieldThread();
        if (*mark != block.Value().index) {
            context.failures.FetchAdd<MemoryOrder::Relaxed>(1);
        }

        context.pool.Free(block.Value().index);
    }
}

} // namespace

TEST(BoundedSpscQueue_OrderFullEmptyAndWrap) {
    SystemAllocator allocator;
    BoundedSpscQueue<uint32> queue;
    CHECK(queue.Initialize(allocator, 3).Error() == ErrorCode::InvalidArgument);
    REQUIRE(queue.Initialize(allocator, SpscCapacity));
    CHECK(queue.Initialize(allocator, SpscCapacity).Error() == ErrorCode::InvalidArgument);
    CHECK(queue.Capacity() == SpscCapacity && queue.IsEmpty() && !queue.IsFull());
    uint32 value = 0;
    CHECK(!queue.TryPop(value));
    // 여러 바퀴 감아도 순서가 지켜지고 가득·빔을 정확히 본다.
    uint32 next = 0;
    uint32 expected = 0;
    for (uint32 round = 0; round < 3; ++round) {
        while (queue.TryPush(next)) {
            ++next;
        }

        CHECK(queue.IsFull() && queue.ApproximateSize() == SpscCapacity);
        while (queue.TryPop(value)) {
            CHECK(value == expected);
            ++expected;
        }

        CHECK(queue.IsEmpty());
    }

    CHECK(next == 3 * SpscCapacity && expected == next);
}

TEST(BoundedSpscQueue_StressKeepsEveryValueInOrder) {
    SystemAllocator allocator;
    SpscStress context;
    REQUIRE(context.queue.Initialize(allocator, 64));
    platform::Thread producer;
    REQUIRE(producer.Start(ProduceSequence, &context) == platform::ThreadStatus::Success);
    for (uint32 count = 0; count < StressItems;) {
        uint32 value = 0;
        if (context.queue.TryPop(value)) {
            context.received[count++] = value;
        } else {
            platform::YieldThread();
        }
    }

    REQUIRE(producer.Join() == platform::ThreadStatus::Success);
    bool ordered = true;
    for (uint32 index = 0; index < StressItems; ++index) {
        ordered = ordered && context.received[index] == index;
    }

    CHECK(ordered);
}

TEST(BoundedSpscQueue_AllInterleavings) {
    SystemAllocator allocator;
    SpscScenario context;
    REQUIRE(context.queue.Initialize(allocator, 2));
    test::StepScheduler scheduler;
    bool allOrdered = true;
    const uint32 schedules = scheduler.Explore(
        2, SpscActor, &context,
        [&]() noexcept {
            context.queue.Shutdown();
            (void)context.queue.Initialize(allocator, 2);
            context.count = 0;
        },
        [&]() noexcept {
            allOrdered = allOrdered && context.count == 2 && context.received[0] == 1 &&
                         context.received[1] == 2;
        });
    CHECK(allOrdered && schedules > 1);
}

TEST(IntrusiveMpscQueue_StressDeliversExactlyOnceInProducerOrder) {
    static MpscStress context;
    context.nextProducer.Store<MemoryOrder::Relaxed>(0);
    platform::Thread producers[Producers];
    for (platform::Thread& producer : producers) {
        REQUIRE(producer.Start(ProduceItems, &context) == platform::ThreadStatus::Success);
    }

    uint32 nextSequence[Producers] = {};
    bool ordered = true;
    for (uint32 count = 0; count < StressItems;) {
        if (MpscItem* item = context.queue.TryPop(); item != nullptr) {
            ordered = ordered && item->sequence == nextSequence[item->producer];
            ++nextSequence[item->producer];
            ++count;
        } else {
            platform::YieldThread();
        }
    }

    for (platform::Thread& producer : producers) {
        REQUIRE(producer.Join() == platform::ThreadStatus::Success);
    }

    CHECK(ordered && context.queue.IsEmpty() && context.queue.TryPop() == nullptr);
}

TEST(IntrusiveMpscQueue_AllInterleavings) {
    static MpscScenario context;
    test::StepScheduler scheduler;
    bool exactlyOnce = true;
    const uint32 schedules = scheduler.Explore(
        3, MpscActor, &context,
        [&]() noexcept {
            context.seen[0] = 0;
            context.seen[1] = 0;
            context.count = 0;
        },
        [&]() noexcept {
            exactlyOnce = exactlyOnce && context.seen[0] == 1 && context.seen[1] == 1 &&
                          context.queue.IsEmpty();
        });
    CHECK(exactlyOnce && schedules > 1);
}

TEST(TakeAllStack_LifoReverseAndClose) {
    TakeAllStack<StackItem> stack;
    StackItem items[3];
    for (uint32 index = 0; index < 3; ++index) {
        items[index].value = index;
        CHECK(stack.Push(&items[index]));
    }

    StackItem* list = TakeAllStack<StackItem>::Reverse(stack.TakeAll());
    uint32 expected = 0;
    for (; list != nullptr; list = TakeAllStack<StackItem>::Next(list)) {
        CHECK(list->value == expected);
        ++expected;
    }

    CHECK(expected == 3 && stack.TakeAll() == nullptr);
    CHECK(stack.Push(&items[0]));
    CHECK(stack.Close() == &items[0] && stack.IsClosed());
    CHECK(!stack.Push(&items[1]) && stack.TakeAll() == nullptr && stack.Close() == nullptr);
}

TEST(TakeAllStack_AllInterleavings) {
    static StackScenario context;
    test::StepScheduler scheduler;
    bool exactlyOnce = true;
    const uint32 schedules = scheduler.Explore(
        3, StackActor, &context,
        [&]() noexcept {
            context.seen[0] = 0;
            context.seen[1] = 0;
            context.count = 0;
        },
        [&]() noexcept {
            exactlyOnce = exactlyOnce && context.seen[0] == 1 && context.seen[1] == 1;
        });
    CHECK(exactlyOnce && schedules > 1);
}

TEST(IndexedFreeList_PopPushAndBounds) {
    SystemAllocator allocator;
    IndexedFreeList<> list;
    CHECK(list.Initialize(allocator, 0, IndexedFreeListStart::AllFree).Error() ==
          ErrorCode::InvalidArgument);
    REQUIRE(list.Initialize(allocator, FreeListCapacity, IndexedFreeListStart::AllFree));
    uint32 index = 99;
    for (uint32 expected = 0; expected < FreeListCapacity; ++expected) {
        CHECK(list.TryPop(index) && index == expected);
    }

    CHECK(!list.TryPop(index) && list.IsEmpty());
    list.Push(1);
    list.Push(2);
    CHECK(list.TryPop(index) && index == 2);
    CHECK(list.TryPop(index) && index == 1);
    IndexedFreeList<> empty;
    REQUIRE(empty.Initialize(allocator, FreeListCapacity, IndexedFreeListStart::Empty));
    CHECK(empty.IsEmpty() && !empty.TryPop(index));
}

TEST(IndexedFreeList_AllInterleavingsGiveDistinctIndices) {
    SystemAllocator allocator;
    static FreeListExplore context;
    test::StepScheduler scheduler;
    bool distinct = true;
    const uint32 schedules = scheduler.Explore(
        2, FreeListPopPushActor, &context,
        [&]() noexcept {
            context.list.Shutdown();
            (void)context.list.Initialize(allocator, 2, IndexedFreeListStart::AllFree);
        },
        [&]() noexcept {
            distinct =
                distinct && context.got[0] && context.got[1] && context.held[0] != context.held[1];
        });
    CHECK(distinct && schedules > 1); // 정적 목록이 이 시험의 할당자보다 오래 살지 않게 돌려준다.
    context.list.Shutdown();
}

// 2-bit 태그 시험판은 배우 0이 머리를 읽고 멈춘 사이 배우 1이 태그를 네 번 올려 같은 머리 값으로
// 되돌리면 낡은 next로 CAS가 성공해 배우 1이 쥔 색인을 다시 내준다(ABA). 40-bit 태그는 같은
// 대본에서 CAS가 실패해 다시 읽는다.
TEST(IndexedFreeList_SmallTagReproducesAbaAndDefaultTagPreventsIt) {
    SystemAllocator allocator;
    // 결정 0: 배우 0이 시작해 PopReadNext에서 멈춘다. 결정 1–5: 배우 1이 셋을 꺼내고 하나를 넣는다.
    const uint32 schedule[] = {0, 1, 1, 1, 1, 1, 1};
    test::StepScheduler scheduler;
    {
        static FreeListScenario<SmallTagBits> context;
        context.poppedCount[0] = 0;
        context.poppedCount[1] = 0;
        REQUIRE(
            context.list.Initialize(allocator, FreeListCapacity, IndexedFreeListStart::AllFree));
        scheduler.RunSchedule(2, AbaActor<SmallTagBits>, &context, schedule,
                              sizeof(schedule) / sizeof(schedule[0]));
        // 배우 0이 0을 받았고 목록 머리가 배우 1이 쥔 색인을 가리킨다.
        uint32 index = 0;
        const bool reissued = context.list.TryPop(index) && context.poppedCount[1] != 0 &&
                              (index == context.popped[1][0] || index == context.popped[1][1]);
        CHECK(context.poppedCount[0] == 1 && reissued);
        context.list.Shutdown();
    }

    {
        static FreeListScenario<40> context;
        context.poppedCount[0] = 0;
        context.poppedCount[1] = 0;
        REQUIRE(
            context.list.Initialize(allocator, FreeListCapacity, IndexedFreeListStart::AllFree));
        scheduler.RunSchedule(2, AbaActor<40>, &context, schedule,
                              sizeof(schedule) / sizeof(schedule[0]));
        // 배우 0과 배우 1이 쥔 색인은 서로 다르고 목록은 비었다.
        uint32 index = 0;
        bool distinct = context.poppedCount[0] == 1;
        for (uint32 held = 0; held < context.poppedCount[1]; ++held) {
            distinct = distinct && context.popped[1][held] != context.popped[0][0];
        }

        CHECK(distinct && !context.list.TryPop(index));
        context.list.Shutdown();
    }
}

TEST(FixedBlockPool_GrowsByPagesAndReusesBlocks) {
    SystemAllocator allocator;
    FixedBlockPool pool;
    CHECK(pool.Initialize(allocator, 24, 3, 4, 8).Error() == ErrorCode::InvalidArgument);
    REQUIRE(pool.Initialize(allocator, 24, 16, 4, 6));
    CHECK(pool.PageCount() == 0 && pool.BlockCapacity() == 6);
    FixedBlockPool::Block blocks[6];
    for (FixedBlockPool::Block& block : blocks) {
        auto allocated = pool.TryAllocate();
        REQUIRE(allocated);
        block = allocated.Value();
        CHECK(reinterpret_cast<uptr>(block.data) % 16 == 0 && pool.Data(block.index) == block.data);
    }

    // 상한(6)에 닿으면 CapacityExceeded이고 페이지는 둘(4 + 2)이다.
    CHECK(pool.PageCount() == 2 && pool.TryAllocate().Error() == ErrorCode::CapacityExceeded);
    pool.Free(blocks[3].index);
    auto reused = pool.TryAllocate();
    REQUIRE(reused);
    CHECK(reused.Value().index == blocks[3].index && pool.PageCount() == 2);
}

TEST(FixedBlockPool_ConcurrentAllocateFreeNeverSharesABlock) {
    SystemAllocator allocator;
    static PoolStress context;
    context.pool.Shutdown();
    context.failures.Store<MemoryOrder::Relaxed>(0);
    REQUIRE(context.pool.Initialize(allocator, sizeof(uint32), alignof(uint32), 2, Producers));
    platform::Thread threads[Producers];
    for (platform::Thread& thread : threads) {
        REQUIRE(thread.Start(AllocateAndFree, &context) == platform::ThreadStatus::Success);
    }

    for (platform::Thread& thread : threads) {
        REQUIRE(thread.Join() == platform::ThreadStatus::Success);
    }

    CHECK(context.failures.Load<MemoryOrder::Relaxed>() == 0);
    // 정적 풀이 이 시험의 할당자보다 오래 살지 않게 돌려준다.
    context.pool.Shutdown();
}

} // namespace concurrent_container_tests_file
