#include <Core/Memory/BinnedAllocator.h>
#include <Core/Memory/BudgetAllocator.h>
#include <Core/Memory/DebugAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Private/Memory/BinnedAllocatorTestAccess.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/SystemError.h>
#include <TestSupport/Test.h>

// Allocator::TrimMemory·MarkThreadIdle·MarkThreadActive다(Docs/Design/GarbageCollectionScale.ko.md
// 18.11절): 쉰다고 표시한 스레드의 캐시를 다른 스레드의 TrimMemory가 비우고, 쓰는 중인 캐시는
// 그대로 두며, 비우는 도중에 깨어난 소유 스레드는 끝을 기다린다. 순서는 명시 gate로 만든다(시간에
// 기대지 않는다).
using namespace rocks;
namespace binned_allocator_idle_trim_tests_file {

using rocks::detail::BinnedAllocatorTestAccess;

namespace {

constexpr uint32 WorkerTimeoutMs = 60000;
// 묶음 한도가 64개인 등급의 요청 크기와 그 블록 크기다.
constexpr usize SmallRequest = 64;
constexpr usize SmallBlockBytes = 64;
// 캐시(부분 묶음 + 가득 묶음)를 채우고 이전 칸까지 넘치게 하는 블록 수다.
constexpr uint32 FillBlocks = 256;
// 깨어난 뒤 정상 동작을 보는 할당 수다.
constexpr uint32 ResumeBlocks = 32;

// 두 스레드가 번갈아 올리는 단계 번호다. 한쪽이 Advance하고 다른 쪽이 그 번호를 기다린다.
struct PhaseGate {
    platform::Mutex mutex;
    platform::ConditionVariable changed;
    uint32 phase = 0;

    void Advance() noexcept {
        platform::LockGuard lock(mutex);
        ++phase;
        changed.NotifyAll();
    }

    void WaitFor(uint32 target) noexcept {
        platform::LockGuard lock(mutex);
        while (phase < target) {
            (void)changed.Wait(mutex);
        }
    }
};

void StartWorker(platform::Thread& thread, platform::Thread::Entry entry, void* context) noexcept {
    if (thread.Start(entry, context) != platform::ThreadStatus::Success) {
        platform::Fatal("Binned idle trim test worker failed to start");
    }
}

// Join은 스레드 종료 정리(캐시 반납)까지 끝난 뒤에 돌아온다.
void JoinWorker(platform::Thread& thread) noexcept {
    if (thread.WaitForCompletion(WorkerTimeoutMs) != platform::WaitResult::Signaled ||
        thread.Join() != platform::ThreadStatus::Success) {
        platform::Fatal("Binned idle trim test worker failed to finish");
    }
}

// count개를 할당했다가 모두 반환한다(캐시에 블록이 남는다). 실패 수를 돌려준다.
uint32 Churn(Allocator& allocator, uint32 count) noexcept {
    static constexpr uint32 MaxBlocks = 512;
    void* blocks[MaxBlocks];
    uint32 failures = count > MaxBlocks ? 1u : 0u;
    const uint32 used = count > MaxBlocks ? MaxBlocks : count;
    for (uint32 index = 0; index < used; ++index) {
        auto result = allocator.Allocate(SmallRequest, 16);
        blocks[index] = result ? result.Value() : nullptr;
        failures += blocks[index] == nullptr ? 1u : 0u;
    }

    for (uint32 index = 0; index < used; ++index) {
        if (blocks[index] != nullptr && !allocator.Deallocate(blocks[index])) {
            ++failures;
        }
    }

    return failures;
}

// worker 하나의 일이다: 캐시를 채우고, markIdle이면 쉰다고 표시한 뒤 단계 1을 알리고 단계 2를
// 기다린다. 깨어나면(markIdle일 때 다시 쓴다고 표시하고) 자기 캐시의 바이트를 적고 다시 할당해
// 본다.
struct IdleWork {
    BinnedAllocator* allocator = nullptr;
    PhaseGate gate;
    bool markIdle = false;
    uint32 failures = 0;
    usize bytesBeforeWait = 0;
    usize bytesAfterWait = 0;
    bool shortcutAfterResume = false;
};

void RunIdleWork(void* context) noexcept {
    auto& work = *static_cast<IdleWork*>(context);
    BinnedAllocator& allocator = *work.allocator;
    work.failures += Churn(allocator, FillBlocks);
    work.bytesBeforeWait = BinnedAllocatorTestAccess::CurrentThreadCacheBlockBytes(allocator);
    if (work.markIdle) {
        allocator.MarkThreadIdle();
    }

    work.gate.Advance();
    work.gate.WaitFor(2);
    if (work.markIdle) {
        allocator.MarkThreadActive();
    }

    work.bytesAfterWait = BinnedAllocatorTestAccess::CurrentThreadCacheBlockBytes(allocator);
    work.failures += Churn(allocator, ResumeBlocks);
    work.shortcutAfterResume = BinnedAllocatorTestAccess::IsCurrentThreadShortcut(allocator);
}

// 세 호출을 세는 allocator다(할당은 상위로 넘긴다). 래퍼가 넘기는지 본다.
class MarkCounter final : public Allocator {
public:
    explicit MarkCounter(Allocator& backing) noexcept : backing_(backing) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return backing_.Deallocate(address);
    }

    void TrimMemory() noexcept override {
        ++trims;
    }

    void MarkThreadIdle() noexcept override {
        ++idleMarks;
    }

    void MarkThreadActive() noexcept override {
        ++activeMarks;
    }

    uint32 trims = 0;
    uint32 idleMarks = 0;
    uint32 activeMarks = 0;

private:
    Allocator& backing_;
};

// 래퍼에 세 호출을 한 번씩 한다.
void CallAll(Allocator& allocator) noexcept {
    allocator.TrimMemory();
    allocator.MarkThreadIdle();
    allocator.MarkThreadActive();
}

} // namespace

// 쉰다고 표시한 스레드의 캐시는 다른 스레드의 TrimMemory가 비운다: 캐시의 블록이 slab으로 돌아가
// commit이 0이 된다. 표시는 그대로 남고, 깨어난 스레드는 빈 캐시로 정상 동작한다.
TEST(CoreBinnedTrimMemoryFlushesIdleThreadCache) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        IdleWork work;
        work.allocator = &allocator;
        work.markIdle = true;
        platform::Thread thread;
        StartWorker(thread, &RunIdleWork, &work);
        work.gate.WaitFor(1);
        // worker는 gate에서 기다린다(자기 캐시를 만지지 않는다).
        CHECK(work.bytesBeforeWait != 0);
        CHECK(BinnedAllocatorTestAccess::IdleThreadCacheCount(allocator) == 1);
        CHECK(BinnedAllocatorTestAccess::ThreadCacheBlockBytes(allocator) == work.bytesBeforeWait);
        CHECK(allocator.Usage().committedBytes != 0);
        allocator.TrimMemory();
        CHECK(BinnedAllocatorTestAccess::ThreadCacheBlockBytes(allocator) == 0);
        CHECK(allocator.Usage().committedBytes == 0);
        // 비운 뒤에도 쉬는 중이다(다시 쓴다고 알릴 때까지).
        CHECK(BinnedAllocatorTestAccess::IdleThreadCacheCount(allocator) == 1);
        work.gate.Advance();
        JoinWorker(thread);
        CHECK(work.failures == 0);
        CHECK(work.bytesAfterWait == 0);
        // 깨어난 뒤에는 다시 캐시와 지름길을 쓴다.
        CHECK(work.shortcutAfterResume);
        CHECK(allocator.Stats().liveAllocations == 0);
        allocator.TrimMemory();
        CHECK(allocator.Usage().committedBytes == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 쓰는 중인 스레드의 캐시는 다른 스레드의 TrimMemory가 건드리지 않는다(막혀 있어도 표시하지
// 않았으면 쓰는 중이다).
TEST(CoreBinnedTrimMemoryLeavesActiveThreadCaches) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        IdleWork work;
        work.allocator = &allocator;
        work.markIdle = false;
        platform::Thread thread;
        StartWorker(thread, &RunIdleWork, &work);
        work.gate.WaitFor(1);
        CHECK(work.bytesBeforeWait != 0);
        CHECK(BinnedAllocatorTestAccess::IdleThreadCacheCount(allocator) == 0);
        allocator.TrimMemory();
        // worker의 캐시는 그대로이고, 그 블록이 든 slab은 commit된 채다.
        CHECK(BinnedAllocatorTestAccess::ThreadCacheBlockBytes(allocator) == work.bytesBeforeWait);
        CHECK(allocator.Usage().committedBytes != 0);
        work.gate.Advance();
        JoinWorker(thread);
        CHECK(work.failures == 0);
        CHECK(work.bytesAfterWait == work.bytesBeforeWait);
        CHECK(allocator.Stats().liveAllocations == 0);
        // 스레드가 끝나며 캐시가 돌아왔다.
        allocator.TrimMemory();
        CHECK(allocator.Usage().committedBytes == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

namespace {

// 비우는 쪽(TrimMemory)을 캐시를 가져간 직후에 세워 두고 그 사이에 소유 스레드를 깨운다.
struct TrimWait {
    BinnedAllocator* allocator = nullptr;
    PhaseGate gate;
    // 0·1 값이다(원자 접근).
    uint32 flushed = 0;
    uint32 ownerWaited = 0;
    bool flushedWhenResumed = false;
    usize bytesWhenResumed = 0;
    uint32 failures = 0;
};

// 단계: 1 worker가 쉰다고 표시함 → 2 비우는 쪽이 캐시를 가져감(worker를 깨운다) → 3 worker가 비우는
// 중임을 보고 기다리러 감(비우는 쪽이 이어 간다).
void TrimWaitHook(void* context, BinnedAllocatorTestAccess::TrimPoint point) noexcept {
    auto& wait = *static_cast<TrimWait*>(context);
    switch (point) {
    case BinnedAllocatorTestAccess::TrimPoint::CacheClaimed:
        wait.gate.Advance();
        wait.gate.WaitFor(3);
        break;
    case BinnedAllocatorTestAccess::TrimPoint::CacheFlushed:
        platform::AtomicStore<platform::MemoryOrder::Release>(&wait.flushed, uint32{1});
        break;
    case BinnedAllocatorTestAccess::TrimPoint::OwnerWaiting:
        platform::AtomicStore<platform::MemoryOrder::Release>(&wait.ownerWaited, uint32{1});
        wait.gate.Advance();
        break;
    }
}

void RunTrimWaitOwner(void* context) noexcept {
    auto& wait = *static_cast<TrimWait*>(context);
    BinnedAllocator& allocator = *wait.allocator;
    wait.failures += Churn(allocator, FillBlocks);
    allocator.MarkThreadIdle();
    wait.gate.Advance();
    wait.gate.WaitFor(2);
    // 비우는 쪽이 이 캐시를 쥐고 서 있다. 끝날 때까지 돌아오지 않아야 한다.
    allocator.MarkThreadActive();
    wait.flushedWhenResumed =
        platform::AtomicLoad<platform::MemoryOrder::Acquire>(&wait.flushed) != 0;
    // 기다리지 않고 돌아온 경우에도 비우는 쪽이 멈춰 있지 않게 단계를 올린다(올바른 구현에서는 이미
    // 지난 단계다).
    wait.gate.Advance();
    if (!wait.flushedWhenResumed) {
        // 비우는 쪽이 아직 묶음을 만지고 있다: 캐시를 쓰지 않고 끝낸다(시험은 위 값으로 실패한다).
        return;
    }

    wait.bytesWhenResumed = BinnedAllocatorTestAccess::CurrentThreadCacheBlockBytes(allocator);
    wait.failures += Churn(allocator, ResumeBlocks);
}

} // namespace

// 비우는 도중에 깨어난 소유 스레드는 MarkThreadActive에서 비우기가 끝날 때까지 기다린 뒤 빈 캐시로
// 정상 동작한다.
TEST(CoreBinnedOwnerWaitsForTrimInProgress) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        TrimWait wait;
        wait.allocator = &allocator;
        platform::Thread thread;
        StartWorker(thread, &RunTrimWaitOwner, &wait);
        wait.gate.WaitFor(1);
        // worker는 gate에서 기다리는 중이라 hook을 걸어도 겹치지 않는다.
        BinnedAllocatorTestAccess::SetTrimHook(allocator, &TrimWaitHook, &wait);
        allocator.TrimMemory();
        JoinWorker(thread);
        BinnedAllocatorTestAccess::SetTrimHook(allocator, nullptr, nullptr);
        CHECK(platform::AtomicLoad<platform::MemoryOrder::Acquire>(&wait.ownerWaited) != 0);
        CHECK(wait.flushedWhenResumed);
        CHECK(wait.bytesWhenResumed == 0);
        CHECK(wait.failures == 0);
        CHECK(allocator.Stats().liveAllocations == 0);
        allocator.TrimMemory();
        CHECK(allocator.Usage().committedBytes == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

namespace {

struct IdleUse {
    BinnedAllocator* first = nullptr;
    BinnedAllocator* second = nullptr;
    uint32 failures = 0;
};

// 조건이 거짓이면 실패로 센다(worker 스레드에서는 CHECK를 쓰지 않는다).
void Expect(IdleUse& use, bool condition) noexcept {
    use.failures += condition ? 0u : 1u;
}

// 블록 하나를 할당했다가 반환한다.
void UseOnce(IdleUse& use, BinnedAllocator& allocator) noexcept {
    auto block = allocator.Allocate(SmallRequest, 16);
    Expect(use, block && block.Value() != nullptr && allocator.Deallocate(block.Value()));
}

void RunIdleUse(void* context) noexcept {
    auto& use = *static_cast<IdleUse*>(context);
    BinnedAllocator& first = *use.first;
    BinnedAllocator& second = *use.second;
    // 캐시가 없는 스레드의 표시는 아무것도 하지 않는다.
    first.MarkThreadIdle();
    first.MarkThreadActive();
    Expect(use, !BinnedAllocatorTestAccess::HasCurrentThreadCache(first));
    use.failures += Churn(first, FillBlocks);
    Expect(use, BinnedAllocatorTestAccess::IsCurrentThreadShortcut(first));
    const usize cached = BinnedAllocatorTestAccess::CurrentThreadCacheBlockBytes(first);
    Expect(use, cached != 0);

    // 쉬는 중: 지름길이 지워지고, 할당·반환은 캐시를 건드리지 않고 등급 잠금 경로로 간다.
    first.MarkThreadIdle();
    Expect(use, !BinnedAllocatorTestAccess::IsCurrentThreadShortcut(first));
    Expect(use, BinnedAllocatorTestAccess::IdleThreadCacheCount(first) == 1);
    UseOnce(use, first);
    Expect(use, BinnedAllocatorTestAccess::CurrentThreadCacheBlockBytes(first) == cached);
    Expect(use, !BinnedAllocatorTestAccess::IsCurrentThreadShortcut(first));
    // 그 사이에 다른 인스턴스를 써서 지름길이 그쪽을 가리켜도 같다.
    UseOnce(use, second);
    Expect(use, BinnedAllocatorTestAccess::IsCurrentThreadShortcut(second));
    UseOnce(use, first);
    Expect(use, BinnedAllocatorTestAccess::CurrentThreadCacheBlockBytes(first) == cached);
    Expect(use, BinnedAllocatorTestAccess::IsCurrentThreadShortcut(second));
    // 두 번 알려도 한 번 깨우면 쓰는 중이다.
    first.MarkThreadIdle();
    first.MarkThreadActive();
    Expect(use, BinnedAllocatorTestAccess::IdleThreadCacheCount(first) == 0);
    // 표시 없이 깨우는 것은 아무것도 하지 않는다.
    first.MarkThreadActive();
    Expect(use, BinnedAllocatorTestAccess::IdleThreadCacheCount(first) == 0);

    // 쓰는 중: 다시 캐시에서 꺼낸다(블록 하나만큼 줄었다가 반환으로 돌아온다).
    auto block = first.Allocate(SmallRequest, 16);
    Expect(use, block && block.Value() != nullptr);
    Expect(use, BinnedAllocatorTestAccess::CurrentThreadCacheBlockBytes(first) ==
                    cached - SmallBlockBytes);
    Expect(use, BinnedAllocatorTestAccess::IsCurrentThreadShortcut(first));
    Expect(use, block && first.Deallocate(block.Value()));

    // 쉰다고 표시할 때 지름길이 애초에 다른 인스턴스를 가리키고 있어도, 표시한 인스턴스의 호출은
    // 캐시 없이 간다.
    UseOnce(use, second);
    Expect(use, BinnedAllocatorTestAccess::IsCurrentThreadShortcut(second));
    first.MarkThreadIdle();
    Expect(use, BinnedAllocatorTestAccess::IsCurrentThreadShortcut(second));
    UseOnce(use, first);
    Expect(use, BinnedAllocatorTestAccess::CurrentThreadCacheBlockBytes(first) == cached);
    Expect(use, !BinnedAllocatorTestAccess::IsCurrentThreadShortcut(first));

    // 쉬는 스레드가 스스로 TrimMemory를 불러도 자기 캐시가 비고 표시는 남는다.
    first.TrimMemory();
    Expect(use, BinnedAllocatorTestAccess::CurrentThreadCacheBlockBytes(first) == 0);
    Expect(use, BinnedAllocatorTestAccess::IdleThreadCacheCount(first) == 1);
    first.MarkThreadActive();
    use.failures += Churn(first, ResumeBlocks);
    Expect(use, BinnedAllocatorTestAccess::IsCurrentThreadShortcut(first));
}

} // namespace

// 쉰다고 표시한 사이의 할당·반환은 캐시 없이 등급 잠금 경로로 간다 — 빠른 경로의 지름길이 이
// 인스턴스를 가리키던 경우와 다른 인스턴스를 가리키던 경우 모두. 표시는 세지 않는 상태다.
TEST(CoreBinnedIdleThreadAllocatesWithoutItsCache) {
    SystemAllocator system;
    {
        BinnedAllocator first(system);
        BinnedAllocator second(system);
        IdleUse use;
        use.first = &first;
        use.second = &second;
        platform::Thread thread;
        StartWorker(thread, &RunIdleUse, &use);
        JoinWorker(thread);
        CHECK(use.failures == 0);
        CHECK(first.Stats().liveAllocations == 0);
        CHECK(second.Stats().liveAllocations == 0);
        first.TrimMemory();
        second.TrimMemory();
        CHECK(first.Usage().committedBytes == 0);
        CHECK(second.Usage().committedBytes == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 상위 allocator 하나에 그대로 넘기는 래퍼는 TrimMemory와 두 표시도 넘긴다(Allocator.h).
TEST(CoreAllocatorWrappersForwardTrimAndIdleMarks) {
    SystemAllocator system;
    MarkCounter counter(system);
    {
        constexpr uint64 BudgetBytes = 1024;
        BudgetAllocator budget(counter, BudgetBytes);
        CallAll(budget);
        CHECK(counter.trims == 1);
        CHECK(counter.idleMarks == 1);
        CHECK(counter.activeMarks == 1);
    }

    {
        DebugAllocator debug(counter);
        CallAll(debug);
        CHECK(counter.trims == 2);
        CHECK(counter.idleMarks == 2);
        CHECK(counter.activeMarks == 2);
    }

    test::NFailAllocator failing(counter);
    CallAll(failing);
    CHECK(counter.trims == 3);
    CHECK(counter.idleMarks == 3);
    CHECK(counter.activeMarks == 3);
    // 기본 구현은 아무것도 하지 않는다(SystemAllocator).
    CallAll(system);
    CHECK(system.Stats().liveAllocations == 0);
}

namespace {

// 한 스레드가 캐시를 갖는 인스턴스 수다(스레드 기록의 처음 항목 수 4를 넘긴다).
constexpr uint32 ManyInstances = 6;
constexpr usize ManyInstanceReserveBytes = usize{64} << 20;

struct ManyInstanceUse {
    BinnedAllocator* allocators[ManyInstances]{};
    uint32 failures = 0;
    uint32 entries = 0;
    uint32 cachesHeld = 0;
};

void RunManyInstanceUse(void* context) noexcept {
    auto& use = *static_cast<ManyInstanceUse*>(context);
    constexpr uint32 Rounds = 2;
    for (uint32 round = 0; round < Rounds; ++round) {
        for (BinnedAllocator* allocator : use.allocators) {
            use.failures += Churn(*allocator, ResumeBlocks);
        }
    }

    use.entries = BinnedAllocatorTestAccess::CurrentThreadRecordEntries();
    for (BinnedAllocator* allocator : use.allocators) {
        use.cachesHeld += BinnedAllocatorTestAccess::HasCurrentThreadCache(*allocator) ? 1u : 0u;
    }
}

} // namespace

// 한 스레드가 인스턴스 여섯 개의 캐시를 가지면 스레드 기록의 항목 표가 늘어난다(처음 4칸). 스레드가
// 끝나면 여섯 캐시가 모두 돌아온다.
TEST(CoreBinnedThreadRecordGrowsPastInitialEntries) {
    SystemAllocator system;
    {
        BinnedAllocatorConfig config;
        config.reserveBytes = ManyInstanceReserveBytes;
        BinnedAllocator a(system, config);
        BinnedAllocator b(system, config);
        BinnedAllocator c(system, config);
        BinnedAllocator d(system, config);
        BinnedAllocator e(system, config);
        BinnedAllocator f(system, config);
        ManyInstanceUse use;
        BinnedAllocator* all[ManyInstances] = {&a, &b, &c, &d, &e, &f};
        for (uint32 index = 0; index < ManyInstances; ++index) {
            use.allocators[index] = all[index];
        }

        platform::Thread thread;
        StartWorker(thread, &RunManyInstanceUse, &use);
        JoinWorker(thread);
        CHECK(use.failures == 0);
        CHECK(use.entries == ManyInstances);
        CHECK(use.cachesHeld == ManyInstances);
        for (BinnedAllocator* allocator : all) {
            CHECK(BinnedAllocatorTestAccess::ThreadCacheCount(*allocator) == 0);
            CHECK(allocator->Stats().liveAllocations == 0);
            allocator->TrimMemory();
            CHECK(allocator->Usage().committedBytes == 0);
        }
    }

    CHECK(system.Stats().liveAllocations == 0);
}

namespace {

// OS 오류 값으로 써 두는 표지다(어느 플랫폼의 실제 오류 코드와도 겹치지 않아도 된다 — 그대로
// 남는지만 본다).
constexpr uint32 SentinelSystemError = 1234;

struct SystemErrorUse {
    BinnedAllocator* allocator = nullptr;
    uint32 afterAllocate = 0;
    uint32 afterDeallocate = 0;
    uint32 afterIdle = 0;
    uint32 afterActive = 0;
    bool allocated = false;
    bool hadCache = false;
};

void RunSystemErrorUse(void* context) noexcept {
    auto& use = *static_cast<SystemErrorUse*>(context);
    BinnedAllocator& allocator = *use.allocator;
    // 이 스레드의 첫 할당이다: 스레드 기록과 캐시를 만들고 종료 칸에 건다(느린 경로).
    test::SetSystemError(SentinelSystemError);
    auto block = allocator.Allocate(SmallRequest, 16);
    use.afterAllocate = test::SystemError();
    use.allocated = block && block.Value() != nullptr;
    test::SetSystemError(SentinelSystemError);
    const bool released = use.allocated && allocator.Deallocate(block.Value());
    use.afterDeallocate = test::SystemError();
    use.allocated = use.allocated && released;
    test::SetSystemError(SentinelSystemError);
    allocator.MarkThreadIdle();
    use.afterIdle = test::SystemError();
    test::SetSystemError(SentinelSystemError);
    allocator.MarkThreadActive();
    use.afterActive = test::SystemError();
    use.hadCache = BinnedAllocatorTestAccess::HasCurrentThreadCache(allocator);
}

} // namespace

// 스레드의 첫 Allocate·Deallocate(스레드 기록·캐시를 만들고 slab을 commit하는 느린 경로)와 두
// 표시가 호출 스레드의 OS 오류 값(Windows last-error, POSIX errno)을 바꾸지 않는다 — 실패한 OS
// 호출과 그 오류 값을 읽는 코드 사이에 할당이 끼어도 값이 남는다.
TEST(CoreBinnedFirstUseOnThreadKeepsSystemError) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        SystemErrorUse use;
        use.allocator = &allocator;
        platform::Thread thread;
        StartWorker(thread, &RunSystemErrorUse, &use);
        JoinWorker(thread);
        CHECK(use.allocated);
        CHECK(use.hadCache);
        CHECK(use.afterAllocate == SentinelSystemError);
        CHECK(use.afterDeallocate == SentinelSystemError);
        CHECK(use.afterIdle == SentinelSystemError);
        CHECK(use.afterActive == SentinelSystemError);
        CHECK(allocator.Stats().liveAllocations == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}
} // namespace binned_allocator_idle_trim_tests_file
