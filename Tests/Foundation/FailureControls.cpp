#include "Tests/Support/Test.h"
#include "Tests/Support/HostFailure.h"
#include <Core/Assert.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Platform/Thread.h>

namespace rocks::test {
namespace {

void ParallelEntry(void*) noexcept {
    for (int i = 0; i < 50; ++i) {
        RecordFailure("parallel-negative-control", __FILE__, __LINE__);
    }
}

void AssertEntry(void*) noexcept {
    ROCKS_ASSERT(false);
}

} // namespace

void RunParallelNegativeControl() noexcept {
    platform::Thread workers[4];
    for (auto& worker : workers) {
        ROCKS_ASSERT(worker.Start(ParallelEntry, nullptr) == platform::ThreadStatus::Success);
    }
    for (auto& worker : workers) {
        ROCKS_ASSERT(worker.WaitForCompletion(10000) == platform::WaitResult::Signaled);
        ROCKS_ASSERT(worker.Join() == platform::ThreadStatus::Success);
    }
}

int RunAssertWithLockedStderr() noexcept {
    PrepareFatalTestProcess();
    LockStandardErrorForTest();
    platform::Thread worker;
    if (worker.Start(AssertEntry, nullptr) != platform::ThreadStatus::Success) {
        UnlockStandardErrorForTest();
        return 2;
    }
    // 정상 경로는 worker의 assert가 프로세스를 종료한다. 교착을 성공으로 넘기지 않는다.
    if (worker.WaitForCompletion(5000) != platform::WaitResult::Signaled) {
        platform::Fatal("Assertion worker did not terminate under the stream lock");
    }
    const auto joined = worker.Join();
    UnlockStandardErrorForTest();
    return joined == platform::ThreadStatus::Success ? 0 : 2;
}

int RunUnjoinedThreadNegativeControl() noexcept {
    PrepareFatalTestProcess();
    {
        platform::Thread thread;
        if (thread.Start([](void*) noexcept {}) != platform::ThreadStatus::Success ||
            thread.WaitForCompletion(10000) != platform::WaitResult::Signaled) {
            return 2;
        }
        // entry 완료와 native 소유권 회수는 다르다. Join 누락을 조용히 허용하면 안 된다.
    }
    return 0;
}

int RunOutstandingNameLeaseNegativeControl() noexcept {
    PrepareFatalTestProcess();
    SystemAllocator allocator;
    NameLease reader;
    {
        NamePool pool(allocator);
        ROCKS_ASSERT(pool.Initialize());
        auto name = pool.TryIntern("still-owned");
        ROCKS_ASSERT(name);
        auto lease = pool.Acquire(name.Value());
        ROCKS_ASSERT(lease);
        reader = Move(lease.Value());
        // 살아 있는 reader를 남기고 pool을 먼저 파괴하는 계약 위반 반례다.
    }
    return 0;
}

} // namespace rocks::test
