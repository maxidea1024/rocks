#include <TestSupport/Test.h>
#include <Core/Containers/BoundedMpscQueue.h>
#include <Core/Diagnostics/LogBlockPool.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>

using namespace rocks;

namespace {

// 시험이 멈추지 않도록 거는 안전 상한이다(정상 진행은 이 시간에 기대지 않는다).
constexpr uint32 SafetyTimeoutMilliseconds = 30000;

void Join(platform::Thread& thread) noexcept {
    if (thread.WaitForCompletion(SafetyTimeoutMilliseconds) != platform::WaitResult::Signaled ||
        thread.Join() != platform::ThreadStatus::Success) {
        platform::Fatal("queue test worker did not finish");
    }
}

constexpr uint32 Producers = 4;
constexpr uint32 ItemsPerProducer = 20000;

struct StressContext {
    BoundedMpscQueue<uint64>* queue = nullptr;
    uint32 producer = 0;
};

void Produce(void* raw) noexcept {
    auto& context = *static_cast<StressContext*>(raw);
    for (uint32 sequence = 0; sequence < ItemsPerProducer; ++sequence) {
        const uint64 value = (static_cast<uint64>(context.producer) << 32) | sequence;
        // 가득 차면 소비자가 비울 때까지 양보한다(TryPush는 기다리지 않는다).
        while (!context.queue->TryPush(value)) {
            platform::YieldThread();
        }
    }
}

struct StuckContext {
    BoundedMpscQueue<uint64>* queue = nullptr;
    platform::ManualResetEvent reserved;
    platform::ManualResetEvent release;
    bool pushed = false;
};

void ProduceWhileStuck(void* raw) noexcept {
    auto& context = *static_cast<StuckContext*>(raw);
    context.pushed = context.queue->TryPushWith([&](uint64& slot) noexcept {
        slot = 100;
        // 칸을 예약한 채로 멈춘다(예약과 공개 사이에서 선점된 생산자를 흉내 낸다).
        context.reserved.Signal();
        if (context.release.Wait(SafetyTimeoutMilliseconds) != platform::WaitResult::Signaled) {
            platform::Fatal("stuck producer was never released");
        }
    });
}

} // namespace

TEST(BoundedMpscQueueOrderFullAndWrap) {
    SystemAllocator allocator;
    BoundedMpscQueue<uint64> invalid;
    CHECK(invalid.Initialize(allocator, 3).Error() == ErrorCode::InvalidArgument);
    CHECK(invalid.Initialize(allocator, 1).Error() == ErrorCode::InvalidArgument);

    // 용량 2로 여러 바퀴 돌려 순번이 한 바퀴를 넘어도 맞는지 본다.
    BoundedMpscQueue<uint64> queue;
    REQUIRE(queue.Initialize(allocator, 2).IsOk());
    CHECK(queue.Capacity() == 2 && queue.IsEmpty());
    CHECK(queue.Initialize(allocator, 2).Error() == ErrorCode::InvalidArgument);
    uint64 value = 0;
    for (uint64 round = 0; round < 1000; ++round) {
        REQUIRE(queue.TryPush(round * 2));
        REQUIRE(queue.TryPush(round * 2 + 1));
        CHECK(!queue.TryPush(uint64{7})); // 가득 참: 기다리지 않고 false
        CHECK(queue.ApproximateSize() == 2);
        REQUIRE(queue.TryPop(value));
        CHECK(value == round * 2);
        REQUIRE(queue.TryPop(value));
        CHECK(value == round * 2 + 1);
        CHECK(!queue.TryPop(value) && queue.IsEmpty());
    }

    CHECK(queue.PushPosition() == 2000 && queue.PopPosition() == 2000);
}

// 생산자 4개 × 20000건을 작은 큐로 보낸다. 모든 값이 정확히 한 번, 생산자별 순서대로 나와야 한다.
TEST(BoundedMpscQueueStressExactlyOnceInProducerOrder) {
    SystemAllocator allocator;
    BoundedMpscQueue<uint64> queue;
    REQUIRE(queue.Initialize(allocator, 64).IsOk());
    StressContext contexts[Producers];
    platform::Thread threads[Producers];
    for (uint32 index = 0; index < Producers; ++index) {
        contexts[index].queue = &queue;
        contexts[index].producer = index;
        if (threads[index].Start(Produce, &contexts[index]) != platform::ThreadStatus::Success) {
            platform::Fatal("queue test producer start failed");
        }
    }

    uint32 next[Producers]{};
    uint64 received = 0;
    bool ordered = true;
    while (received < static_cast<uint64>(Producers) * ItemsPerProducer) {
        uint64 value = 0;
        if (!queue.TryPop(value)) {
            platform::YieldThread();
            continue;
        }

        const auto producer = static_cast<uint32>(value >> 32);
        const auto sequence = static_cast<uint32>(value);
        if (producer >= Producers || sequence != next[producer]) {
            ordered = false;
        } else {
            ++next[producer];
        }

        ++received;
    }

    for (platform::Thread& thread : threads) {
        Join(thread);
    }

    CHECK(ordered);
    for (uint32 index = 0; index < Producers; ++index) {
        CHECK(next[index] == ItemsPerProducer);
    }

    CHECK(queue.IsEmpty());
}

// 맨 앞 칸을 예약한 생산자가 멈추면 소비자는 그 칸에서 기다리고(TryPop false), 다른 생산자는 계속
// 넣는다. 멈춘 생산자가 공개하면 순서대로 나온다. 시간 대신 이벤트로 순서를 만든다.
TEST(BoundedMpscQueueStuckProducerBlocksOnlyConsumer) {
    SystemAllocator allocator;
    BoundedMpscQueue<uint64> queue;
    REQUIRE(queue.Initialize(allocator, 8).IsOk());
    StuckContext context;
    context.queue = &queue;
    platform::Thread stuck;
    if (stuck.Start(ProduceWhileStuck, &context) != platform::ThreadStatus::Success ||
        context.reserved.Wait(SafetyTimeoutMilliseconds) != platform::WaitResult::Signaled) {
        platform::Fatal("stuck producer did not reserve");
    }

    // 다른 생산자(이 스레드)는 막히지 않는다.
    CHECK(queue.TryPush(uint64{1}) && queue.TryPush(uint64{2}) && queue.TryPush(uint64{3}));
    uint64 value = 0;
    CHECK(!queue.TryPop(value) && !queue.IsEmpty());
    context.release.Signal();
    Join(stuck);
    CHECK(context.pushed);
    const uint64 expected[] = {100, 1, 2, 3};
    for (uint64 item : expected) {
        REQUIRE(queue.TryPop(value));
        CHECK(value == item);
    }

    CHECK(queue.IsEmpty());
}

namespace {

constexpr uint32 PoolThreads = 4;
constexpr uint32 PoolRounds = 20000;

struct PoolContext {
    LogBlockPool* pool = nullptr;
    uint8 marker = 0;
    bool exclusive = true;
    uint32 borrowed = 0;
};

// 빌린 블록에 자기 표시를 쓰고, 돌려주기 전에 그대로인지 본다(두 스레드가 같은 블록을 빌리면
// 깨진다).
void UsePool(void* raw) noexcept {
    auto& context = *static_cast<PoolContext*>(raw);
    for (uint32 round = 0; round < PoolRounds; ++round) {
        LogBlock block;
        if (!context.pool->TryBorrow(block)) {
            platform::YieldThread();
            continue;
        }

        ++context.borrowed;
        for (usize index = 0; index < block.bytes.Size(); ++index) {
            block.bytes[index] = context.marker;
        }

        platform::YieldThread();
        for (usize index = 0; index < block.bytes.Size(); ++index) {
            context.exclusive = context.exclusive && block.bytes[index] == context.marker;
        }

        context.pool->Release(block.index);
    }
}

} // namespace

TEST(LogBlockPoolBorrowReleaseAndExhaustion) {
    SystemAllocator allocator;
    LogBlockPool pool;
    CHECK(pool.Initialize(allocator, 0, 64).Error() == ErrorCode::InvalidArgument);
    CHECK(pool.Initialize(allocator, 2, 1).Error() == ErrorCode::InvalidArgument);
    REQUIRE(pool.Initialize(allocator, 3, 64).IsOk());
    CHECK(pool.BlockBytes() == 64 && pool.BlockCount() == 3 && pool.ApproximateAvailable() == 3);
    LogBlock blocks[3];
    for (LogBlock& block : blocks) {
        REQUIRE(pool.TryBorrow(block));
        CHECK(block.bytes.Size() == 64 && block.index < 3);
        CHECK(pool.Resolve(block.index).Data() == block.bytes.Data());
    }

    LogBlock extra;
    CHECK(!pool.TryBorrow(extra) && pool.ApproximateAvailable() == 0);
    CHECK(blocks[0].index != blocks[1].index && blocks[1].index != blocks[2].index &&
          blocks[0].index != blocks[2].index);
    CHECK(pool.Resolve(3).Size() == 0);
    pool.Release(blocks[1].index);
    REQUIRE(pool.TryBorrow(extra));
    CHECK(extra.index == blocks[1].index);
    pool.Release(extra.index);
    pool.Release(blocks[0].index);
    pool.Release(blocks[2].index);
    CHECK(pool.ApproximateAvailable() == 3);
}

// 여러 스레드가 동시에 빌리고 돌려준다. 같은 블록을 두 스레드가 동시에 갖지 않고(ABA 없음), 끝나면
// 모든 블록이 돌아온다.
TEST(LogBlockPoolConcurrentBorrowIsExclusive) {
    SystemAllocator allocator;
    LogBlockPool pool;
    REQUIRE(pool.Initialize(allocator, 3, 32).IsOk());
    PoolContext contexts[PoolThreads];
    platform::Thread threads[PoolThreads];
    for (uint32 index = 0; index < PoolThreads; ++index) {
        contexts[index].pool = &pool;
        contexts[index].marker = static_cast<uint8>(index + 1);
        if (threads[index].Start(UsePool, &contexts[index]) != platform::ThreadStatus::Success) {
            platform::Fatal("pool test thread start failed");
        }
    }

    for (platform::Thread& thread : threads) {
        Join(thread);
    }

    uint32 borrowed = 0;
    for (const PoolContext& context : contexts) {
        CHECK(context.exclusive);
        borrowed += context.borrowed;
    }

    CHECK(borrowed != 0);
    CHECK(pool.ApproximateAvailable() == 3);
    LogBlock all[3];
    for (LogBlock& block : all) {
        CHECK(pool.TryBorrow(block));
    }

    LogBlock none;
    CHECK(!pool.TryBorrow(none));
}
