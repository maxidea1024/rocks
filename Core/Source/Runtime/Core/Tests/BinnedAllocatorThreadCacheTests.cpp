#include <Core/Memory/BinnedAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Core/Private/Memory/BinnedAllocatorTestAccess.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <TestSupport/OsThread.h>
#include <TestSupport/Test.h>

// BinnedAllocator의 스레드 캐시·이전 칸·스레드 종료 정리다(Docs/Design/GarbageCollectionScale.ko.md
// 18.4절, 검토 F3·F7). 순서는 명시 gate로 만든다(시간에 기대지 않는다).
using namespace rocks;
namespace binned_allocator_thread_cache_tests_file {

using rocks::detail::BinnedAllocatorTestAccess;

namespace {

constexpr usize SlabBytes = BinnedAllocator::SlabBytes;
constexpr uint32 WorkerTimeoutMs = 60000;
// 묶음 한도가 64개인 등급의 요청 크기다.
constexpr usize SmallRequest = 64;

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
        platform::Fatal("Binned cache test worker failed to start");
    }
}

// Join은 스레드 종료 정리(캐시 반납)까지 끝난 뒤에 돌아온다.
void JoinWorker(platform::Thread& thread) noexcept {
    if (thread.WaitForCompletion(WorkerTimeoutMs) != platform::WaitResult::Signaled ||
        thread.Join() != platform::ThreadStatus::Success) {
        platform::Fatal("Binned cache test worker failed to finish");
    }
}

uint32 ClassOfSmall(usize bytes) noexcept {
    return BinnedAllocatorTestAccess::ClassOf(bytes, 16);
}

// count개를 할당해 blocks에 담는다. 실패 수를 돌려준다.
uint32 AllocateMany(BinnedAllocator& allocator, usize bytes, void** blocks, uint32 count) noexcept {
    uint32 failures = 0;
    for (uint32 index = 0; index < count; ++index) {
        auto result = allocator.Allocate(bytes, 16);
        if (!result || result.Value() == nullptr) {
            blocks[index] = nullptr;
            ++failures;
            continue;
        }

        blocks[index] = result.Value();
        *static_cast<uint32*>(blocks[index]) = index;
    }

    return failures;
}

uint32 FreeMany(BinnedAllocator& allocator, void** blocks, uint32 count) noexcept {
    uint32 failures = 0;
    for (uint32 index = 0; index < count; ++index) {
        if (blocks[index] == nullptr || *static_cast<uint32*>(blocks[index]) != index ||
            !allocator.Deallocate(blocks[index])) {
            ++failures;
        }
    }

    return failures;
}

constexpr uint32 MaxWorkBlocks = 1024;

// worker 하나의 일이다: allocateCount개를 할당하고, freeCount개(앞에서부터)를 반환한 뒤, gate가
// 있으면 단계 1을 알리고 단계 2를 기다린 다음 끝난다.
struct CacheWork {
    BinnedAllocator* allocator = nullptr;
    PhaseGate* gate = nullptr;
    usize bytes = SmallRequest;
    uint32 allocateCount = 0;
    uint32 freeCount = 0;
    void* blocks[MaxWorkBlocks]{};
    uint32 failures = 0;
    bool hadCache = false;
};

void RunCacheWork(void* context) noexcept {
    auto& work = *static_cast<CacheWork*>(context);
    work.failures += AllocateMany(*work.allocator, work.bytes, work.blocks, work.allocateCount);
    work.failures += FreeMany(*work.allocator, work.blocks, work.freeCount);
    work.hadCache = BinnedAllocatorTestAccess::HasCurrentThreadCache(*work.allocator);
    if (work.gate != nullptr) {
        work.gate->Advance();
        work.gate->WaitFor(2);
    }
}

// 한 스레드가 할당한 묶음을 다른 스레드가 반환한다(칸 하나짜리 인계 — 명시 handoff).
struct Handoff {
    platform::Mutex mutex;
    platform::ConditionVariable changed;
    void** batch = nullptr;
    bool done = false;
};

constexpr uint32 HandoffBatch = 96;
constexpr uint32 HandoffRounds = 40;

struct ProducerWork {
    BinnedAllocator* allocator = nullptr;
    Handoff* handoff = nullptr;
    // 인계가 비기를 기다리는 동안 다음 묶음을 채우지 않도록 round마다 다른 배열을 쓴다.
    void* batches[HandoffRounds][HandoffBatch]{};
    uint32 failures = 0;
};

void RunProducer(void* context) noexcept {
    auto& work = *static_cast<ProducerWork*>(context);
    for (uint32 round = 0; round < HandoffRounds; ++round) {
        work.failures +=
            AllocateMany(*work.allocator, SmallRequest, work.batches[round], HandoffBatch);
        platform::LockGuard lock(work.handoff->mutex);
        while (work.handoff->batch != nullptr) {
            (void)work.handoff->changed.Wait(work.handoff->mutex);
        }

        work.handoff->batch = work.batches[round];
        work.handoff->changed.NotifyAll();
    }

    platform::LockGuard lock(work.handoff->mutex);
    while (work.handoff->batch != nullptr) {
        (void)work.handoff->changed.Wait(work.handoff->mutex);
    }

    work.handoff->done = true;
    work.handoff->changed.NotifyAll();
}

struct ConsumerWork {
    BinnedAllocator* allocator;
    Handoff* handoff;
    uint32 batches = 0;
    uint32 failures = 0;
};

void RunConsumer(void* context) noexcept {
    auto& work = *static_cast<ConsumerWork*>(context);
    for (;;) {
        void** batch = nullptr;
        {
            platform::LockGuard lock(work.handoff->mutex);
            while (work.handoff->batch == nullptr && !work.handoff->done) {
                (void)work.handoff->changed.Wait(work.handoff->mutex);
            }

            if (work.handoff->batch == nullptr) {
                return;
            }

            batch = work.handoff->batch;
        }

        work.failures += FreeMany(*work.allocator, batch, HandoffBatch);
        ++work.batches;
        platform::LockGuard lock(work.handoff->mutex);
        work.handoff->batch = nullptr;
        work.handoff->changed.NotifyAll();
    }
}

// 종료 정리가 지나간 스레드의 일이다: 캐시로 조금 쓰고, 종료 정리를 돌린 뒤에 더 쓴다.
struct ExitedThreadWork {
    BinnedAllocator* allocator = nullptr;
    uint32 failures = 0;
    bool cachedBefore = false;
    bool cachedAfter = true;
    bool exitedBefore = true;
    bool exitedAfter = false;
    void* kept = nullptr;
};

constexpr uint32 ExitedThreadBlocks = 100;

void RunExitedThreadWork(void* context) noexcept {
    auto& work = *static_cast<ExitedThreadWork*>(context);
    void* blocks[ExitedThreadBlocks]{};
    work.failures += AllocateMany(*work.allocator, SmallRequest, blocks, ExitedThreadBlocks);
    work.failures += FreeMany(*work.allocator, blocks, ExitedThreadBlocks);
    work.cachedBefore = BinnedAllocatorTestAccess::HasCurrentThreadCache(*work.allocator);
    work.exitedBefore = BinnedAllocatorTestAccess::IsCurrentThreadExited();
    BinnedAllocatorTestAccess::RunCurrentThreadExitHook();
    work.exitedAfter = BinnedAllocatorTestAccess::IsCurrentThreadExited();
    // 종료 정리 뒤에도 할당·반환이 된다(등급 잠금 경로 — 캐시를 다시 만들지 않는다).
    work.failures += AllocateMany(*work.allocator, SmallRequest, blocks, ExitedThreadBlocks);
    work.failures += FreeMany(*work.allocator, blocks, ExitedThreadBlocks - 1);
    work.kept = blocks[ExitedThreadBlocks - 1];
    work.cachedAfter = BinnedAllocatorTestAccess::HasCurrentThreadCache(*work.allocator);
}

// 종료 칸에 걸지 못한 스레드의 일이다: 캐시 없이 등급 잠금 경로로만 간다.
struct UnhookedThreadWork {
    BinnedAllocator* allocator = nullptr;
    BinnedAllocator* other = nullptr;
    uint32 failures = 0;
    bool unhookedBefore = true;
    bool unhookedAfter = false;
    bool cached = true;
    bool cachedOther = true;
    uint32 cacheCount = 1;
};

constexpr uint32 UnhookedRounds = 3;

void RunUnhookedThreadWork(void* context) noexcept {
    auto& work = *static_cast<UnhookedThreadWork*>(context);
    void* blocks[ExitedThreadBlocks]{};
    work.unhookedBefore = BinnedAllocatorTestAccess::IsCurrentThreadUnhooked();
    BinnedAllocatorTestAccess::FailNextThreadExitHook();
    // 첫 호출이 기록을 걸지 못한다. 그 뒤의 호출은 다시 시도하지 않는다(다른 인스턴스도 같다).
    for (uint32 round = 0; round < UnhookedRounds; ++round) {
        work.failures += AllocateMany(*work.allocator, SmallRequest, blocks, ExitedThreadBlocks);
        work.failures += FreeMany(*work.allocator, blocks, ExitedThreadBlocks);
        work.failures += AllocateMany(*work.other, SmallRequest, blocks, ExitedThreadBlocks);
        work.failures += FreeMany(*work.other, blocks, ExitedThreadBlocks);
    }

    work.unhookedAfter = BinnedAllocatorTestAccess::IsCurrentThreadUnhooked();
    work.cached = BinnedAllocatorTestAccess::HasCurrentThreadCache(*work.allocator);
    work.cachedOther = BinnedAllocatorTestAccess::HasCurrentThreadCache(*work.other);
    work.cacheCount = BinnedAllocatorTestAccess::ThreadCacheCount(*work.allocator) +
                      BinnedAllocatorTestAccess::ThreadCacheCount(*work.other);
}

// thread_local 포인터를 잃은 스레드의 일이다(스레드가 끝나는 동안 thread_local 저장소가 먼저
// 해제되는 플랫폼의 상태): 기록은 종료 칸에 살아 있다.
struct LostPointerWork {
    BinnedAllocator* allocator = nullptr;
    BinnedAllocator* other = nullptr;
    uint32 failures = 0;
    const void* recordBefore = nullptr;
    // 지운 직후의 포인터다(null이어야 한다 — 초기값은 null이 아닌 아무 주소다).
    const void* recordForgotten = &failures;
    const void* recordAfterFree = nullptr;
    const void* recordAfterOther = nullptr;
    uint32 cachesAfter = 0;
    uint32 otherCachesAfter = 0;
    uint32 entriesAfter = 0;
    bool cachedAfter = false;
};

void RunLostPointerWork(void* context) noexcept {
    auto& work = *static_cast<LostPointerWork*>(context);
    void* blocks[ExitedThreadBlocks]{};
    work.failures += AllocateMany(*work.allocator, SmallRequest, blocks, ExitedThreadBlocks);
    work.recordBefore = BinnedAllocatorTestAccess::CurrentThreadRecordPointer();
    BinnedAllocatorTestAccess::ForgetCurrentThreadRecord();
    work.recordForgotten = BinnedAllocatorTestAccess::CurrentThreadRecordPointer();
    // 포인터를 잃은 뒤의 첫 호출이 반환이다: 칸의 기록을 다시 가리키고 같은 캐시에 넣는다.
    work.failures += FreeMany(*work.allocator, blocks, ExitedThreadBlocks);
    work.recordAfterFree = BinnedAllocatorTestAccess::CurrentThreadRecordPointer();
    // 다시 잃은 뒤의 첫 호출이 다른 인스턴스의 할당이다: 그 캐시도 같은 기록에 든다.
    BinnedAllocatorTestAccess::ForgetCurrentThreadRecord();
    work.failures += AllocateMany(*work.other, SmallRequest, blocks, ExitedThreadBlocks);
    work.failures += FreeMany(*work.other, blocks, ExitedThreadBlocks);
    work.recordAfterOther = BinnedAllocatorTestAccess::CurrentThreadRecordPointer();
    work.cachesAfter = BinnedAllocatorTestAccess::ThreadCacheCount(*work.allocator);
    work.otherCachesAfter = BinnedAllocatorTestAccess::ThreadCacheCount(*work.other);
    work.entriesAfter = BinnedAllocatorTestAccess::CurrentThreadRecordEntries();
    work.cachedAfter = BinnedAllocatorTestAccess::HasCurrentThreadCache(*work.allocator);
}

// 다른 종료 정리가 allocator를 쓰는 경우다: 이 칸의 정리 함수가 스레드가 끝나는 동안(할당자의 종료
// hook 앞이든 뒤든) 할당·반환을 한다.
struct ExitUse {
    BinnedAllocator* allocator = nullptr;
    uint32 calls = 0;
    uint32 failures = 0;
};

void UseAllocatorAtThreadExit(void* value) noexcept {
    auto& use = *static_cast<ExitUse*>(value);
    void* blocks[ExitedThreadBlocks]{};
    use.failures += AllocateMany(*use.allocator, SmallRequest, blocks, ExitedThreadBlocks);
    use.failures += FreeMany(*use.allocator, blocks, ExitedThreadBlocks);
    ++use.calls;
}

// 프로세스가 시작할 때 만든다(정적 초기화): 할당자의 종료 칸(인스턴스를 처음 만들 때 생긴다)보다
// 먼저 생겨, 칸을 번호 순서로 도는 플랫폼에서는 이 정리 함수가 할당자의 종료 hook보다 먼저 불린다.
// macOS에서는 "thread_local 저장소가 해제된 뒤·hook 앞에 allocator를 쓰는" 실제 순서가 된다. 시험의
// 단언은 순서에 기대지 않는다. 칸은 프로세스 수명 동안 유지한다(OS 칸을 풀지 않는다).
platform::ThreadExitSlot exitUseSlot(&UseAllocatorAtThreadExit);

struct ExitUseWork {
    ExitUse* use = nullptr;
    uint32 failures = 0;
    bool hadCache = false;
    bool slotSet = false;
};

void RunExitUseWork(void* context) noexcept {
    auto& work = *static_cast<ExitUseWork*>(context);
    void* blocks[ExitedThreadBlocks]{};
    work.failures += AllocateMany(*work.use->allocator, SmallRequest, blocks, ExitedThreadBlocks);
    work.failures += FreeMany(*work.use->allocator, blocks, ExitedThreadBlocks);
    work.hadCache = BinnedAllocatorTestAccess::HasCurrentThreadCache(*work.use->allocator);
    work.slotSet = exitUseSlot.Set(work.use);
}

// 캐시를 한도 넘게 채우려 하고 쉬는 스레드의 일이다: 등급마다 묶음 넷만큼 할당하고 셋만큼 반환한
// 뒤(캐시에는 둘만 남고 하나는 이전 칸으로 밀려난다) 단계 1을 알리고 단계 2를 기다린다. 그 뒤 남은
// 블록을 반환하고 끝난다.
constexpr uint32 IdleCacheBundles = 4;
constexpr uint32 IdleCacheFreedBundles = 3;
// 스레드 캐시의 등급 하나에 남는 묶음 수다(부분·가득).
constexpr uint32 IdleCacheHeldBundles = 2;
constexpr uint32 MaxClasses = 32;
constexpr uint32 MaxBundle = 64;

struct IdleCacheWork {
    BinnedAllocator* allocator = nullptr;
    PhaseGate* gate = nullptr;
    void* blocks[MaxClasses][IdleCacheBundles * MaxBundle]{};
    uint32 failures = 0;
    // 등급마다 반환한 직후의 캐시 바이트가 상한을 넘은 횟수다.
    uint32 overLimit = 0;
};

void RunIdleCacheWork(void* context) noexcept {
    auto& work = *static_cast<IdleCacheWork*>(context);
    const uint32 classes = BinnedAllocatorTestAccess::ClassCount();
    for (uint32 sizeClass = 0; sizeClass < classes; ++sizeClass) {
        const usize bytes = BinnedAllocatorTestAccess::ClassBlockBytes(sizeClass);
        const uint32 bundle = BinnedAllocatorTestAccess::BundleBlocks(sizeClass);
        work.failures +=
            AllocateMany(*work.allocator, bytes, work.blocks[sizeClass], IdleCacheBundles * bundle);
        work.failures +=
            FreeMany(*work.allocator, work.blocks[sizeClass], IdleCacheFreedBundles * bundle);
        // 이 스레드만 이 인스턴스를 쓰는 동안이라 캐시를 읽어도 된다.
        if (BinnedAllocatorTestAccess::ThreadCacheBlockBytes(*work.allocator) >
            BinnedAllocator::MaxThreadCacheBytes) {
            ++work.overLimit;
        }
    }

    work.gate->Advance();
    work.gate->WaitFor(2);
    for (uint32 sizeClass = 0; sizeClass < classes; ++sizeClass) {
        const uint32 bundle = BinnedAllocatorTestAccess::BundleBlocks(sizeClass);
        const uint32 freed = IdleCacheFreedBundles * bundle;
        for (uint32 index = freed; index < IdleCacheBundles * bundle; ++index) {
            if (!work.allocator->Deallocate(work.blocks[sizeClass][index])) {
                ++work.failures;
            }
        }
    }
}

// 같은 주소를 두 스레드가 함께 반환한다. round마다 주 스레드가 주소를 걸고 번호를 올리면 두
// worker가 그 번호를 보고 곧바로 반환한다(스레드를 round마다 만들지 않아 두 반환이 가깝게 겹친다).
constexpr uint32 DuplicateRounds = 4096;
constexpr uint32 DuplicateWorkers = 2;

struct DuplicateRace {
    BinnedAllocator* allocator = nullptr;
    // 지금 round 번호(1부터)와 그 round에 반환할 주소다.
    uint32 round = 0;
    void* address = nullptr;
    // round를 끝낸 worker 수다.
    uint32 finished = 0;
    uint32 successes[DuplicateWorkers]{};
    uint32 notFound[DuplicateWorkers]{};
    uint32 other[DuplicateWorkers]{};
};

struct DuplicateRaceWork {
    DuplicateRace* race = nullptr;
    uint32 worker = 0;
};

void RunDuplicateRace(void* context) noexcept {
    auto& work = *static_cast<DuplicateRaceWork*>(context);
    DuplicateRace& race = *work.race;
    for (uint32 round = 1; round <= DuplicateRounds; ++round) {
        while (platform::AtomicLoad<platform::MemoryOrder::Acquire>(&race.round) != round) {
            platform::YieldThread();
        }

        const ErrorCode result = race.allocator->Deallocate(race.address).Error();
        if (result == ErrorCode::None) {
            ++race.successes[work.worker];
        } else if (result == ErrorCode::NotFound) {
            ++race.notFound[work.worker];
        } else {
            ++race.other[work.worker];
        }

        platform::AtomicFetchAdd<platform::MemoryOrder::AcquireRelease>(&race.finished, 1);
    }
}

} // namespace

// 캐시는 처음 쓸 때 생기고, 묶음 단위로 slab에서 떼어 온다: 떼었지만 내준 적 없는 블록(표지)과 아직
// 뗀 적 없는 블록(뗀 블록 수)은 모두 소유가 아니다. 캐시를 비우면 slab이 돌아간다.
TEST(CoreBinnedCacheRefillsByBundle) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        const uint32 sizeClass = ClassOfSmall(SmallRequest);
        const uint32 limit = BinnedAllocatorTestAccess::BundleBlocks(sizeClass);
        const usize block = BinnedAllocatorTestAccess::ClassBlockBytes(sizeClass);
        constexpr uint32 FullBundle = 64;
        REQUIRE(limit == FullBundle);
        // 묶음 한도는 64개 또는 64KB 가운데 작은 쪽이다.
        CHECK(BinnedAllocatorTestAccess::BundleBlocks(ClassOfSmall(1024)) == FullBundle);
        CHECK(BinnedAllocatorTestAccess::BundleBlocks(ClassOfSmall(2048)) == 32);
        CHECK(BinnedAllocatorTestAccess::BundleBlocks(ClassOfSmall(8192)) == 8);
        CHECK(!BinnedAllocatorTestAccess::HasCurrentThreadCache(allocator));
        CHECK(BinnedAllocatorTestAccess::ThreadCacheCount(allocator) == 0);

        auto first = allocator.Allocate(SmallRequest, 16);
        REQUIRE(first);
        CHECK(BinnedAllocatorTestAccess::HasCurrentThreadCache(allocator));
        CHECK(BinnedAllocatorTestAccess::ThreadCacheCount(allocator) == 1);
        const AllocationStats before = allocator.Stats();
        CHECK(before.liveAllocations == 1);
        // 묶음으로 떼어 캐시에 든 블록(표지가 서 있다)과 그 뒤의 뗀 적 없는 블록이다. 떼는 순서는
        // 주소가 높은 쪽부터 내주므로 첫 블록 양옆을 모두 본다.
        const uptr slabStart = reinterpret_cast<uptr>(first.Value()) & ~(SlabBytes - 1);
        constexpr usize SlabHeaderBytes = 64;
        uint32 rejected = 0;
        for (uint32 index = 0; index < 2 * limit; ++index) {
            void* candidate = reinterpret_cast<void*>(slabStart + SlabHeaderBytes + index * block);
            if (candidate == first.Value()) {
                continue;
            }

            if (allocator.Deallocate(candidate).Error() == ErrorCode::NotFound) {
                ++rejected;
            }
        }

        CHECK(rejected == 2 * limit - 1);
        const AllocationStats after = allocator.Stats();
        CHECK(after.liveAllocations == before.liveAllocations);
        CHECK(after.totalAllocations == before.totalAllocations);

        // 이어지는 할당이 서로 다른 주소를 받고 같은 slab의 첫 묶음 안이다.
        void* blocks[FullBundle - 1]{};
        for (uint32 index = 0; index < limit - 1; ++index) {
            auto result = allocator.Allocate(SmallRequest, 16);
            REQUIRE(result);
            blocks[index] = result.Value();
            const uptr value = reinterpret_cast<uptr>(blocks[index]);
            CHECK(value >= slabStart + SlabHeaderBytes);
            CHECK(value < slabStart + SlabHeaderBytes + limit * block);
            CHECK(blocks[index] != first.Value());
            for (uint32 earlier = 0; earlier < index; ++earlier) {
                REQUIRE(blocks[index] != blocks[earlier]);
            }
        }

        for (uint32 index = 0; index < limit - 1; ++index) {
            REQUIRE(allocator.Deallocate(blocks[index]));
        }

        REQUIRE(allocator.Deallocate(first.Value()));
        CHECK(allocator.Stats().liveAllocations == 0);
        // 반환한 블록은 캐시에 있다: slab은 아직 쓰는 중이다.
        CHECK(allocator.Usage().committedBytes == SlabBytes);
        CHECK(allocator.Usage().retainedSlabs == 0);
        CHECK(allocator.Deallocate(first.Value()).Error() == ErrorCode::NotFound);
        BinnedAllocatorTestAccess::FlushCurrentThreadCache(allocator);
        CHECK(allocator.Usage().retainedSlabs == 1);
        CHECK(BinnedAllocatorTestAccess::SlabTableEntry(allocator, first.Value()) == 0);
        allocator.Trim();
        CHECK(allocator.Usage().committedBytes == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 이전 칸(검토 F7): 반환이 쌓이면 가득 찬 묶음이 이전 칸으로 가고(여덟 칸까지, 넘으면 slab으로),
// 이전 칸의 묶음은 언제나 그 등급의 한도만큼이다. 할당은 이전 칸을 slab보다 먼저 쓴다.
TEST(CoreBinnedTransferSlotsHoldOnlyFullBundles) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        constexpr usize Requests[] = {SmallRequest, 2048, 8192};
        for (usize request : Requests) {
            const uint32 sizeClass = ClassOfSmall(request);
            const uint32 limit = BinnedAllocatorTestAccess::BundleBlocks(sizeClass);
            constexpr uint32 Bundles = 12;
            constexpr uint32 TransferSlots = 8;
            const uint32 count = limit * Bundles;
            REQUIRE(count <= MaxWorkBlocks);
            void* blocks[MaxWorkBlocks]{};
            REQUIRE(AllocateMany(allocator, request, blocks, count) == 0);
            CHECK(BinnedAllocatorTestAccess::TransferBundleCount(allocator, sizeClass) == 0);
            REQUIRE(FreeMany(allocator, blocks, count) == 0);
            // 캐시에 묶음 둘(부분·가득), 이전 칸에 여덟, 나머지 둘은 slab으로 돌아갔다.
            CHECK(BinnedAllocatorTestAccess::TransferBundleCount(allocator, sizeClass) ==
                  TransferSlots);
            CHECK(BinnedAllocatorTestAccess::TransferChainLength(allocator, sizeClass) == limit);
            CHECK(allocator.Stats().liveAllocations == 0);

            // 다시 할당하면 캐시의 두 묶음 다음에 이전 칸을 모두 쓴다.
            const uint32 fromCaches = limit * (2 + TransferSlots);
            REQUIRE(AllocateMany(allocator, request, blocks, fromCaches) == 0);
            CHECK(BinnedAllocatorTestAccess::TransferBundleCount(allocator, sizeClass) == 0);
            for (uint32 index = 0; index < fromCaches; ++index) {
                for (uint32 earlier = 0; earlier < index && earlier < 4; ++earlier) {
                    REQUIRE(blocks[index] != blocks[earlier]);
                }
            }

            REQUIRE(FreeMany(allocator, blocks, fromCaches) == 0);
            CHECK(allocator.Stats().liveAllocations == 0);
            // Trim은 호출 스레드의 캐시와 이전 칸을 slab으로 돌려주고 모두 decommit한다.
            allocator.Trim();
            CHECK(BinnedAllocatorTestAccess::TransferBundleCount(allocator, sizeClass) == 0);
            CHECK(allocator.Usage().committedBytes == 0);
        }
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 부분 묶음은 이전 칸에 가지 않는다(검토 F7): 스레드가 끝날 때와 Trim 때 부분 묶음은 slab으로 바로
// 돌아가고, 가득 찬 예비 묶음만 이전 칸으로 간다.
TEST(CoreBinnedPartialBundlesReturnToSlabsAtThreadExitAndTrim) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        const uint32 sizeClass = ClassOfSmall(SmallRequest);
        const uint32 limit = BinnedAllocatorTestAccess::BundleBlocks(sizeClass);
        platform::Thread thread;
        {
            // 부분 묶음만 가진 스레드: 열 개를 받고 돌려준다(예비 묶음은 비어 있다).
            CacheWork work;
            work.allocator = &allocator;
            work.allocateCount = 10;
            work.freeCount = 10;
            StartWorker(thread, RunCacheWork, &work);
            JoinWorker(thread);
            CHECK(work.failures == 0);
            CHECK(work.hadCache);
            CHECK(BinnedAllocatorTestAccess::ThreadCacheCount(allocator) == 0);
            CHECK(BinnedAllocatorTestAccess::TransferBundleCount(allocator, sizeClass) == 0);
            // 묶음이 slab으로 돌아가 slab이 비었다.
            CHECK(allocator.Usage().retainedSlabs == 1);
            CHECK(allocator.Stats().liveAllocations == 0);
            CHECK(allocator.Stats().totalAllocations == 10);
        }

        {
            // 예비 묶음까지 찬 스레드: 한도 + 36개를 받고 모두 돌려주면 부분·예비가 모두
            // 한도만큼이다.
            CacheWork work;
            work.allocator = &allocator;
            work.allocateCount = limit + 36;
            work.freeCount = limit + 36;
            StartWorker(thread, RunCacheWork, &work);
            JoinWorker(thread);
            CHECK(work.failures == 0);
            // 예비 묶음 하나만 이전 칸으로 갔다(부분 묶음은 개수가 같아도 slab으로 간다).
            CHECK(BinnedAllocatorTestAccess::TransferBundleCount(allocator, sizeClass) == 1);
            CHECK(BinnedAllocatorTestAccess::TransferChainLength(allocator, sizeClass) == limit);
            CHECK(allocator.Stats().liveAllocations == 0);
            CHECK(allocator.Stats().totalAllocations == 10 + limit + 36);
        }

        // 이 스레드의 부분 묶음도 Trim이 slab으로 돌려준다.
        void* blocks[10]{};
        REQUIRE(AllocateMany(allocator, SmallRequest, blocks, 10) == 0);
        REQUIRE(FreeMany(allocator, blocks, 10) == 0);
        allocator.Trim();
        CHECK(BinnedAllocatorTestAccess::TransferBundleCount(allocator, sizeClass) == 0);
        CHECK(allocator.Usage().committedBytes == 0);
        CHECK(allocator.Stats().liveAllocations == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 스레드가 끝나면 그 캐시의 블록과 계수가 인스턴스로 돌아간다. 끝난 스레드가 할당한 블록은 살아
// 있고 다른 스레드가 반환할 수 있다. Trim은 다른 스레드의 캐시를 건드리지 않는다.
TEST(CoreBinnedThreadExitReturnsCacheAndKeepsLiveBlocks) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        constexpr uint32 Count = 200;
        constexpr uint32 Freed = 50;
        PhaseGate gate;
        CacheWork work;
        work.allocator = &allocator;
        work.gate = &gate;
        work.allocateCount = Count;
        work.freeCount = Freed;
        platform::Thread thread;
        StartWorker(thread, RunCacheWork, &work);
        gate.WaitFor(1);
        // worker는 문 앞에 서 있다(계수가 멈춰 있어 통계가 정확하다).
        CHECK(work.failures == 0);
        CHECK(BinnedAllocatorTestAccess::ThreadCacheCount(allocator) == 1);
        CHECK(allocator.Stats().liveAllocations == Count - Freed);
        CHECK(allocator.Stats().totalAllocations == Count);
        const usize committed = allocator.Usage().committedBytes;
        CHECK(committed != 0);
        // 다른 스레드의 캐시는 Trim이 비우지 않는다: 살아 있는 블록과 캐시의 블록이 slab을 붙든다.
        allocator.Trim();
        CHECK(allocator.Usage().committedBytes == committed);
        gate.Advance();
        JoinWorker(thread);
        CHECK(BinnedAllocatorTestAccess::ThreadCacheCount(allocator) == 0);
        CHECK(allocator.Stats().liveAllocations == Count - Freed);
        CHECK(allocator.Stats().totalAllocations == Count);
        // 끝난 스레드가 남긴 블록의 내용이 그대로이고 이 스레드가 반환한다.
        for (uint32 index = Freed; index < Count; ++index) {
            CHECK(*static_cast<uint32*>(work.blocks[index]) == index);
            REQUIRE(allocator.Deallocate(work.blocks[index]));
        }

        // 이미 반환된 블록은 끝난 스레드가 돌려줬어도 중복 반환이다.
        CHECK(allocator.Deallocate(work.blocks[Freed]).Error() == ErrorCode::NotFound);
        CHECK(allocator.Stats().liveAllocations == 0);
        allocator.Trim();
        CHECK(allocator.Usage().committedBytes == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// platform::Thread가 아닌 OS 스레드의 캐시도 스레드가 끝날 때 돌아간다.
TEST(CoreBinnedRawOsThreadCacheIsReturned) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        CacheWork work;
        work.allocator = &allocator;
        work.allocateCount = 300;
        work.freeCount = 300;
        REQUIRE(test::RunOnOsThread(RunCacheWork, &work));
        CHECK(work.failures == 0);
        CHECK(work.hadCache);
        CHECK(BinnedAllocatorTestAccess::ThreadCacheCount(allocator) == 0);
        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(allocator.Stats().totalAllocations == 300);
        allocator.Trim();
        CHECK(allocator.Usage().committedBytes == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 인스턴스가 스레드보다 먼저 파괴돼도 된다: 파괴가 그 스레드의 캐시까지 해제하고, 뒤에 끝나는
// 스레드의 종료 정리는 죽은 인스턴스를 건너뛴다. 그 스레드는 다른 인스턴스를 계속 쓸 수 있다.
TEST(CoreBinnedInstanceDestroyedBeforeItsThreadsExit) {
    SystemAllocator system;
    {
        alignas(BinnedAllocator) unsigned char storage[sizeof(BinnedAllocator)];
        struct TwoInstanceWork {
            BinnedAllocator* first;
            BinnedAllocator** second;
            PhaseGate* gate;
            bool useSecond;
            uint32 failures = 0;
            uint32 entriesBefore = 0;
            uint32 entriesAfter = 0;
        };

        const auto run = [](void* context) noexcept {
            auto& work = *static_cast<TwoInstanceWork*>(context);
            void* blocks[100]{};
            work.failures += AllocateMany(*work.first, SmallRequest, blocks, 100);
            work.failures += FreeMany(*work.first, blocks, 100);
            work.entriesBefore = BinnedAllocatorTestAccess::CurrentThreadRecordEntries();
            work.gate->Advance();
            // 여기서 주 스레드가 first를 파괴하고 같은 자리에 second를 만든다.
            work.gate->WaitFor(2);
            if (work.useSecond) {
                BinnedAllocator& second = **work.second;
                work.failures += AllocateMany(second, SmallRequest, blocks, 100);
                work.failures += FreeMany(second, blocks, 100);
            }

            work.entriesAfter = BinnedAllocatorTestAccess::CurrentThreadRecordEntries();
        };

        for (uint32 round = 0; round < 2; ++round) {
            BinnedAllocator* first = ConstructAt<BinnedAllocator>(storage, system);
            BinnedAllocator* second = nullptr;
            PhaseGate gate;
            TwoInstanceWork work{first, &second, &gate, round == 0};
            platform::Thread thread;
            StartWorker(thread, run, &work);
            gate.WaitFor(1);
            CHECK(BinnedAllocatorTestAccess::ThreadCacheCount(*first) == 1);
            CHECK(first->Stats().liveAllocations == 0);
            // worker는 살아 있지만 이 인스턴스를 더 부르지 않는다.
            DestroyAt(first);
            // 같은 주소에 다시 만든 인스턴스는 번호가 달라 옛 캐시와 섞이지 않는다.
            second = ConstructAt<BinnedAllocator>(storage, system);
            CHECK(BinnedAllocatorTestAccess::ThreadCacheCount(*second) == 0);
            auto block = second->Allocate(SmallRequest, 16);
            REQUIRE(block);
            gate.Advance();
            JoinWorker(thread);
            CHECK(work.failures == 0);
            // 죽은 인스턴스의 항목은 그 스레드가 다음 캐시를 만들 때 버려진다(기록이 자라지
            // 않는다). 캐시를 더 만들지 않은 스레드의 기록에는 죽은 항목이 그대로 남는다.
            CHECK(work.entriesBefore == 1);
            CHECK(work.entriesAfter == 1);
            // 끝난 스레드의 캐시는 돌아갔고 이 스레드의 캐시만 남았다.
            CHECK(BinnedAllocatorTestAccess::ThreadCacheCount(*second) == 1);
            CHECK(second->Stats().liveAllocations == 1);
            CHECK(second->Stats().totalAllocations == (round == 0 ? 101u : 1u));
            REQUIRE(second->Deallocate(block.Value()));
            second->Trim();
            CHECK(second->Usage().committedBytes == 0);
            DestroyAt(second);
        }
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 캐시를 만들지 못한 호출은 등급 잠금 경로로 가고, 다음 호출이 캐시를 만든다. 두 경로의 블록을 섞어
// 반환해도 된다. 캐시를 끈 인스턴스는 캐시를 만들지 않는다.
TEST(CoreBinnedCacheCreationFailureUsesLockedPath) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        const usize block = BinnedAllocatorTestAccess::ClassBlockBytes(ClassOfSmall(SmallRequest));
        BinnedAllocatorTestAccess::FailNextThreadCacheCreation();
        auto direct = allocator.Allocate(SmallRequest, 16);
        REQUIRE(direct);
        CHECK(!BinnedAllocatorTestAccess::HasCurrentThreadCache(allocator));
        // 등급 잠금 경로는 하나만 뗀다: 바로 다음 블록은 아직 뗀 적이 없다.
        CHECK(allocator.Deallocate(static_cast<uint8*>(direct.Value()) + block).Error() ==
              ErrorCode::NotFound);
        auto cached = allocator.Allocate(SmallRequest, 16);
        REQUIRE(cached);
        CHECK(BinnedAllocatorTestAccess::HasCurrentThreadCache(allocator));
        CHECK(cached.Value() != direct.Value());
        CHECK(allocator.Stats().liveAllocations == 2);
        REQUIRE(allocator.Deallocate(direct.Value()));
        REQUIRE(allocator.Deallocate(cached.Value()));
        CHECK(allocator.Deallocate(direct.Value()).Error() == ErrorCode::NotFound);
        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(allocator.Stats().totalAllocations == 2);
        allocator.Trim();
        CHECK(allocator.Usage().committedBytes == 0);

        BinnedAllocatorConfig config;
        config.threadCaches = false;
        BinnedAllocator uncached(system, config);
        auto plain = uncached.Allocate(SmallRequest, 16);
        REQUIRE(plain);
        CHECK(!BinnedAllocatorTestAccess::HasCurrentThreadCache(uncached));
        CHECK(BinnedAllocatorTestAccess::ThreadCacheCount(uncached) == 0);
        REQUIRE(uncached.Deallocate(plain.Value()));
        // 캐시가 없으면 반환이 곧바로 slab으로 간다.
        CHECK(uncached.Usage().retainedSlabs == 1);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 한 스레드가 할당하고 다른 스레드가 반환한다(명시 인계). 반환 쪽 캐시가 차면 이전 칸을 거쳐 할당
// 쪽으로 돌아간다. 끝나면 모든 블록이 돌아와 있다.
TEST(CoreBinnedProducerConsumerHandoff) {
    SystemAllocator system;
    SystemAllocator workMemory;
    {
        BinnedAllocator allocator(system);
        Handoff handoff;
        auto producerStorage = workMemory.Allocate(sizeof(ProducerWork), alignof(ProducerWork));
        REQUIRE(producerStorage);
        auto* producer = InitializeAt<ProducerWork>(producerStorage.Value());
        producer->allocator = &allocator;
        producer->handoff = &handoff;
        ConsumerWork consumer{&allocator, &handoff};
        platform::Thread threads[2];
        StartWorker(threads[0], RunProducer, producer);
        StartWorker(threads[1], RunConsumer, &consumer);
        JoinWorker(threads[0]);
        JoinWorker(threads[1]);
        CHECK(producer->failures == 0);
        CHECK(consumer.failures == 0);
        CHECK(consumer.batches == HandoffRounds);
        CHECK(BinnedAllocatorTestAccess::ThreadCacheCount(allocator) == 0);
        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(allocator.Stats().totalAllocations == uint64{HandoffRounds} * HandoffBatch);
        allocator.Trim();
        CHECK(allocator.Usage().committedBytes == 0);
        REQUIRE(workMemory.Deallocate(producer));
    }

    CHECK(system.Stats().liveAllocations == 0);
    CHECK(workMemory.Stats().liveAllocations == 0);
}

// 종료 정리가 지나간 스레드(그 뒤에 도는 다른 종료 정리가 allocator를 쓰는 경우)는 캐시를 다시
// 만들지 않고 등급 잠금 경로로 간다. 그 스레드가 남긴 블록은 살아 있다.
TEST(CoreBinnedExitedThreadUsesLockedPath) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        ExitedThreadWork work;
        work.allocator = &allocator;
        platform::Thread thread;
        StartWorker(thread, RunExitedThreadWork, &work);
        JoinWorker(thread);
        CHECK(work.failures == 0);
        CHECK(work.cachedBefore);
        CHECK(!work.exitedBefore);
        CHECK(work.exitedAfter);
        CHECK(!work.cachedAfter);
        CHECK(BinnedAllocatorTestAccess::ThreadCacheCount(allocator) == 0);
        CHECK(allocator.Stats().liveAllocations == 1);
        CHECK(allocator.Stats().totalAllocations == 2 * ExitedThreadBlocks);
        REQUIRE(work.kept != nullptr);
        CHECK(*static_cast<uint32*>(work.kept) == ExitedThreadBlocks - 1);
        REQUIRE(allocator.Deallocate(work.kept));
        CHECK(allocator.Stats().liveAllocations == 0);
        allocator.Trim();
        CHECK(allocator.Usage().committedBytes == 0);
        // 이 스레드는 종료 정리를 돌리지 않았다.
        CHECK(!BinnedAllocatorTestAccess::IsCurrentThreadExited());
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 같은 주소를 두 스레드가 함께 반환하면 round마다 정확히 한쪽만 성공한다(캐시 켬·끔 모두 — 표지를
// 비교 교환으로 세운다).
TEST(CoreBinnedConcurrentDuplicateReturnHasOneWinner) {
    SystemAllocator system;
    for (uint32 mode = 0; mode < 2; ++mode) {
        BinnedAllocatorConfig config;
        config.threadCaches = mode != 0;
        BinnedAllocator allocator(system, config);
        // 같은 slab에 살아 있는 블록을 남겨 둔다(중복 반환이 slab 풀기와 겹치지 않게 — 18.4절 한계
        // 2).
        auto anchor = allocator.Allocate(SmallRequest, 16);
        REQUIRE(anchor);
        DuplicateRace race;
        race.allocator = &allocator;
        DuplicateRaceWork work[DuplicateWorkers]{{&race, 0}, {&race, 1}};
        platform::Thread threads[DuplicateWorkers];
        for (uint32 index = 0; index < DuplicateWorkers; ++index) {
            StartWorker(threads[index], RunDuplicateRace, &work[index]);
        }

        uint32 allocationFailures = 0;
        for (uint32 round = 1; round <= DuplicateRounds; ++round) {
            auto block = allocator.Allocate(SmallRequest, 16);
            if (!block) {
                ++allocationFailures;
            }

            race.address = block ? block.Value() : nullptr;
            platform::AtomicStore<platform::MemoryOrder::Release>(&race.finished, 0);
            platform::AtomicStore<platform::MemoryOrder::Release>(&race.round, round);
            while (platform::AtomicLoad<platform::MemoryOrder::Acquire>(&race.finished) !=
                   DuplicateWorkers) {
                platform::YieldThread();
            }
        }

        for (uint32 index = 0; index < DuplicateWorkers; ++index) {
            JoinWorker(threads[index]);
        }

        CHECK(allocationFailures == 0);
        CHECK(race.successes[0] + race.successes[1] == DuplicateRounds);
        CHECK(race.notFound[0] + race.notFound[1] == DuplicateRounds);
        CHECK(race.other[0] + race.other[1] == 0);
        CHECK(allocator.Stats().liveAllocations == 1);
        CHECK(allocator.Stats().totalAllocations == DuplicateRounds + 1);
        REQUIRE(allocator.Deallocate(anchor.Value()));
        allocator.Trim();
        CHECK(allocator.Usage().committedBytes == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 기록을 스레드 종료 칸에 걸지 못한 스레드는 캐시 없이 등급 잠금 경로로 가고 다시 시도하지 않는다
// (호출마다 기록을 할당했다가 버리지 않는다). 다른 스레드는 영향이 없다.
TEST(CoreBinnedThreadWithoutExitHookStaysUncached) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        BinnedAllocator other(system);
        UnhookedThreadWork work;
        work.allocator = &allocator;
        work.other = &other;
        platform::Thread thread;
        StartWorker(thread, RunUnhookedThreadWork, &work);
        JoinWorker(thread);
        CHECK(work.failures == 0);
        CHECK(!work.unhookedBefore);
        CHECK(work.unhookedAfter);
        CHECK(!work.cached);
        CHECK(!work.cachedOther);
        CHECK(work.cacheCount == 0);
        constexpr uint64 Total = uint64{UnhookedRounds} * ExitedThreadBlocks;
        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(allocator.Stats().totalAllocations == Total);
        CHECK(other.Stats().liveAllocations == 0);
        CHECK(other.Stats().totalAllocations == Total);
        // 반환이 곧바로 slab으로 갔으므로 그 스레드의 캐시 없이도 Trim이 모두 푼다.
        allocator.Trim();
        other.Trim();
        CHECK(allocator.Usage().committedBytes == 0);
        CHECK(other.Usage().committedBytes == 0);
        // 이 스레드는 그대로 캐시를 쓴다.
        CHECK(!BinnedAllocatorTestAccess::IsCurrentThreadUnhooked());
        auto block = allocator.Allocate(SmallRequest, 16);
        REQUIRE(block);
        CHECK(BinnedAllocatorTestAccess::HasCurrentThreadCache(allocator));
        REQUIRE(allocator.Deallocate(block.Value()));
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 스레드의 기록은 종료 칸의 값이 기준이다: thread_local 포인터를 잃어도(스레드가 끝나는 동안
// thread_local 저장소가 먼저 해제되는 플랫폼) 칸의 기록을 다시 찾아 쓰고 새 기록을 만들지 않는다.
// 스레드가 끝나면 그 기록의 캐시가 모두 돌아온다.
TEST(CoreBinnedThreadRecordIsFoundAgainFromExitSlot) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        BinnedAllocator other(system);
        LostPointerWork work;
        work.allocator = &allocator;
        work.other = &other;
        platform::Thread thread;
        StartWorker(thread, RunLostPointerWork, &work);
        JoinWorker(thread);
        CHECK(work.failures == 0);
        CHECK(work.recordBefore != nullptr);
        CHECK(work.recordForgotten == nullptr);
        // 기록은 하나다: 두 번 모두 같은 기록을 다시 가리킨다.
        CHECK(work.recordAfterFree == work.recordBefore);
        CHECK(work.recordAfterOther == work.recordBefore);
        CHECK(work.cachesAfter == 1);
        CHECK(work.otherCachesAfter == 1);
        CHECK(work.entriesAfter == 2);
        CHECK(work.cachedAfter);
        // 종료 hook이 그 기록의 캐시를 모두 돌려줬다.
        CHECK(BinnedAllocatorTestAccess::ThreadCacheCount(allocator) == 0);
        CHECK(BinnedAllocatorTestAccess::ThreadCacheCount(other) == 0);
        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(allocator.Stats().totalAllocations == ExitedThreadBlocks);
        CHECK(other.Stats().liveAllocations == 0);
        CHECK(other.Stats().totalAllocations == ExitedThreadBlocks);
        allocator.Trim();
        other.Trim();
        CHECK(allocator.Usage().committedBytes == 0);
        CHECK(other.Usage().committedBytes == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 다른 스레드 종료 정리(다른 칸의 정리 함수)가 allocator를 써도 된다: 할당자의 종료 hook 앞에
// 불리면 그 스레드의 캐시를 쓰고, 뒤에 불리면 등급 잠금 경로로 간다(OS가 정하는 순서 — 어느
// 쪽이든 결과가 같다). 스레드가 끝나면 캐시가 남지 않는다.
TEST(CoreBinnedAllocatorUsedByAnotherThreadExitCleanup) {
    REQUIRE(exitUseSlot.IsValid());
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        ExitUse use;
        use.allocator = &allocator;
        ExitUseWork work;
        work.use = &use;
        platform::Thread thread;
        StartWorker(thread, RunExitUseWork, &work);
        JoinWorker(thread);
        CHECK(work.failures == 0);
        CHECK(work.hadCache);
        CHECK(work.slotSet);
        // Join이 돌아왔으면 두 정리 함수가 모두 끝났다.
        CHECK(use.calls == 1);
        CHECK(use.failures == 0);
        CHECK(BinnedAllocatorTestAccess::ThreadCacheCount(allocator) == 0);
        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(allocator.Stats().totalAllocations == uint64{2} * ExitedThreadBlocks);
        allocator.Trim();
        CHECK(allocator.Usage().committedBytes == 0);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 쉬는 스레드의 캐시가 쥐는 블록은 상한(BinnedAllocator::MaxThreadCacheBytes — 등급마다 한도만큼 찬
// 묶음 둘)을 넘지 않는다: 묶음 셋만큼 반환해도 둘만 남고 하나는 이전 칸으로 간다. 모든 등급을
// 그렇게 채우면 정확히 그 값이다. 그 스레드가 쉬는 동안 다른 스레드의 Trim은 이전 칸의 묶음은
// 돌려받지만 캐시의 블록은 돌려받지 못하고(한계 — 설계 18.8절), 스레드가 끝나면 모두 돌아온다.
TEST(CoreBinnedIdleThreadCacheIsBounded) {
    REQUIRE(BinnedAllocatorTestAccess::ClassCount() == MaxClasses);
    usize limit = 0;
    uint64 liveBlocks = 0;
    for (uint32 sizeClass = 0; sizeClass < MaxClasses; ++sizeClass) {
        const uint32 bundle = BinnedAllocatorTestAccess::BundleBlocks(sizeClass);
        REQUIRE(bundle <= MaxBundle);
        limit += usize{IdleCacheHeldBundles} * bundle *
                 BinnedAllocatorTestAccess::ClassBlockBytes(sizeClass);
        liveBlocks += uint64{IdleCacheBundles - IdleCacheFreedBundles} * bundle;
    }

    CHECK(limit == BinnedAllocator::MaxThreadCacheBytes);
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        PhaseGate gate;
        // 블록 배열이 커서 스택에 두지 않는다.
        auto storage = system.Allocate(sizeof(IdleCacheWork), alignof(IdleCacheWork));
        REQUIRE(storage);
        IdleCacheWork* work = ConstructAt<IdleCacheWork>(storage.Value());
        work->allocator = &allocator;
        work->gate = &gate;
        platform::Thread thread;
        StartWorker(thread, RunIdleCacheWork, work);
        gate.WaitFor(1);
        // worker는 문 앞에서 쉰다: 캐시가 상한만큼 차 있다.
        CHECK(work->failures == 0);
        CHECK(work->overLimit == 0);
        CHECK(BinnedAllocatorTestAccess::ThreadCacheBlockBytes(allocator) ==
              BinnedAllocator::MaxThreadCacheBytes);
        CHECK(allocator.Stats().liveAllocations == liveBlocks);
        // 밀려난 묶음은 등급마다 이전 칸에 하나씩 있다.
        uint32 transferBundles = 0;
        for (uint32 sizeClass = 0; sizeClass < MaxClasses; ++sizeClass) {
            transferBundles += BinnedAllocatorTestAccess::TransferBundleCount(allocator, sizeClass);
        }

        CHECK(transferBundles == MaxClasses);
        // 이 스레드의 Trim은 이전 칸을 비우지만 쉬는 스레드의 캐시는 돌려받지 못한다.
        allocator.Trim();
        transferBundles = 0;
        for (uint32 sizeClass = 0; sizeClass < MaxClasses; ++sizeClass) {
            transferBundles += BinnedAllocatorTestAccess::TransferBundleCount(allocator, sizeClass);
        }

        CHECK(transferBundles == 0);
        CHECK(BinnedAllocatorTestAccess::ThreadCacheBlockBytes(allocator) ==
              BinnedAllocator::MaxThreadCacheBytes);
        CHECK(allocator.Usage().committedBytes >= BinnedAllocator::MaxThreadCacheBytes);
        gate.Advance();
        JoinWorker(thread);
        CHECK(work->failures == 0);
        CHECK(BinnedAllocatorTestAccess::ThreadCacheCount(allocator) == 0);
        CHECK(BinnedAllocatorTestAccess::ThreadCacheBlockBytes(allocator) == 0);
        CHECK(allocator.Stats().liveAllocations == 0);
        allocator.Trim();
        CHECK(allocator.Usage().committedBytes == 0);
        // IdleCacheWork는 소멸이 trivial하다(저장소만 돌려준다).
        REQUIRE(system.Deallocate(work));
    }

    CHECK(system.Stats().liveAllocations == 0);
}
} // namespace binned_allocator_thread_cache_tests_file
