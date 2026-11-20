#include "AllocatorMicroBench.h"

#include <Core/Memory/BinnedAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <Task/WorkerScheduler.h>

#include <stdio.h>

namespace gc_bench {

namespace {

using namespace rocks;

// 쌍 측정의 블록 크기와 반복 수다.
constexpr usize PairSizes[] = {16, 64, 256, 1024, 8192};
constexpr uint32 PairCount = 1000000;
// 묶음·스레드 측정의 블록이다.
constexpr usize BlockBytes = 64;
constexpr usize BlockAlignment = 16;
constexpr uint32 BatchBlocks = 4096;
constexpr uint32 BatchRounds = 64;
// 원장을 선형 검색하는 느린 할당자도 끝나도록 묶음 측정은 이 시간을 넘기면 멈춘다(끝낸 round만으로
// 평균을 낸다 — 벤치의 반복 수 조절일 뿐 판정에 쓰지 않는다).
constexpr uint64 BatchTimeLimitNanoseconds = 2000000000;
constexpr uint32 HandoffBlocks = 256;
constexpr uint32 HandoffRounds = 2000;
constexpr uint32 ConcurrentThreads = 8;
constexpr uint32 ConcurrentPairs = 500000;
constexpr uint32 ConcurrentWindow = 64;
constexpr double NanosecondsPerSecond = 1e9;
constexpr double PairsPerMillion = 1e6;

double PerOperation(uint64 nanoseconds, uint64 operations) noexcept {
    return static_cast<double>(nanoseconds) / static_cast<double>(operations);
}

// 내준 메모리를 실제로 건드린다(첫 byte 쓰기).
void Touch(void* block, uint32 value) noexcept {
    *static_cast<uint8*>(block) = static_cast<uint8>(value);
}

bool MeasurePairs(Allocator& allocator, usize bytes, double& nanoseconds) noexcept {
    const uint64 started = platform::MonotonicNanoseconds();
    for (uint32 index = 0; index < PairCount; ++index) {
        auto block = allocator.Allocate(bytes, BlockAlignment);
        if (!block) {
            return false;
        }

        Touch(block.Value(), index);
        if (!allocator.Deallocate(block.Value())) {
            return false;
        }
    }

    nanoseconds = PerOperation(platform::MonotonicNanoseconds() - started, PairCount);
    return true;
}

// blocks에 count개를 할당한다. 실패하면 이미 받은 것을 반환하고 거짓이다.
bool AllocateBlocks(Allocator& allocator, void** blocks, uint32 count) noexcept {
    for (uint32 index = 0; index < count; ++index) {
        auto block = allocator.Allocate(BlockBytes, BlockAlignment);
        if (!block) {
            for (uint32 undo = 0; undo < index; ++undo) {
                (void)allocator.Deallocate(blocks[undo]);
            }

            return false;
        }

        Touch(block.Value(), index);
        blocks[index] = block.Value();
    }

    return true;
}

bool DeallocateBlocks(Allocator& allocator, void** blocks, uint32 count) noexcept {
    bool ok = true;
    for (uint32 index = 0; index < count; ++index) {
        ok = static_cast<bool>(allocator.Deallocate(blocks[index])) && ok;
    }

    return ok;
}

bool MeasureBatch(Allocator& allocator, double& nanoseconds, uint32& rounds) noexcept {
    static void* blocks[BatchBlocks];
    const uint64 started = platform::MonotonicNanoseconds();
    uint64 elapsed = 0;
    rounds = 0;
    while (rounds < BatchRounds && elapsed < BatchTimeLimitNanoseconds) {
        if (!AllocateBlocks(allocator, blocks, BatchBlocks) ||
            !DeallocateBlocks(allocator, blocks, BatchBlocks)) {
            return false;
        }

        ++rounds;
        elapsed = platform::MonotonicNanoseconds() - started;
    }

    nanoseconds = PerOperation(elapsed, uint64{rounds} * BatchBlocks * 2);
    return true;
}

// 한 스레드가 채운 묶음을 다른 스레드에 넘기는 칸이다. full이 순서를 정한다(채움 → 비움 → 채움).
struct Handoff {
    platform::Mutex mutex;
    platform::ConditionVariable changed;
    Allocator* allocator = nullptr;
    void* blocks[HandoffBlocks] = {};
    bool full = false;
    bool finished = false;
    bool failed = false;
};

void ConsumeHandoff(void* context) noexcept {
    auto& handoff = *static_cast<Handoff*>(context);
    void* local[HandoffBlocks];
    for (;;) {
        {
            platform::LockGuard lock(handoff.mutex);
            while (!handoff.full && !handoff.finished) {
                (void)handoff.changed.Wait(handoff.mutex);
            }

            if (!handoff.full) {
                return;
            }

            for (uint32 index = 0; index < HandoffBlocks; ++index) {
                local[index] = handoff.blocks[index];
            }
        }

        const bool ok = DeallocateBlocks(*handoff.allocator, local, HandoffBlocks);
        {
            platform::LockGuard lock(handoff.mutex);
            handoff.full = false;
            handoff.failed = handoff.failed || !ok;
            handoff.changed.NotifyAll();
        }
    }
}

bool MeasureHandoff(Allocator& allocator, double& crossNanoseconds,
                    double& sameNanoseconds) noexcept {
    void* local[HandoffBlocks];
    // 같은 흐름(256개 할당 뒤 같은 순서로 반환)을 한 스레드에서 한 값이다.
    uint64 started = platform::MonotonicNanoseconds();
    for (uint32 round = 0; round < HandoffRounds; ++round) {
        if (!AllocateBlocks(allocator, local, HandoffBlocks) ||
            !DeallocateBlocks(allocator, local, HandoffBlocks)) {
            return false;
        }
    }

    sameNanoseconds = PerOperation(platform::MonotonicNanoseconds() - started,
                                   uint64{HandoffRounds} * HandoffBlocks);
    Handoff handoff;
    handoff.allocator = &allocator;
    platform::Thread consumer;
    if (consumer.Start(&ConsumeHandoff, &handoff) != platform::ThreadStatus::Success) {
        return false;
    }

    bool ok = true;
    started = platform::MonotonicNanoseconds();
    for (uint32 round = 0; round < HandoffRounds && ok; ++round) {
        ok = AllocateBlocks(allocator, local, HandoffBlocks);
        if (!ok) {
            break;
        }

        platform::LockGuard lock(handoff.mutex);
        for (uint32 index = 0; index < HandoffBlocks; ++index) {
            handoff.blocks[index] = local[index];
        }

        handoff.full = true;
        handoff.changed.NotifyAll();
        while (handoff.full) {
            (void)handoff.changed.Wait(handoff.mutex);
        }
    }

    const uint64 elapsed = platform::MonotonicNanoseconds() - started;
    {
        platform::LockGuard lock(handoff.mutex);
        handoff.finished = true;
        handoff.changed.NotifyAll();
    }

    if (consumer.Join() != platform::ThreadStatus::Success) {
        return false;
    }

    crossNanoseconds = PerOperation(elapsed, uint64{HandoffRounds} * HandoffBlocks);
    return ok && !handoff.failed;
}

// 동시 측정의 시작 gate다. 모든 스레드가 gate에 선 뒤(ready) 한꺼번에 연다.
struct StartGate {
    platform::Mutex mutex;
    platform::ConditionVariable changed;
    uint32 ready = 0;
    bool open = false;
};

struct Worker {
    platform::Thread thread;
    Allocator* allocator = nullptr;
    StartGate* gate = nullptr;
    bool failed = false;
};

void RunWorker(void* context) noexcept {
    auto& worker = *static_cast<Worker*>(context);
    {
        platform::LockGuard lock(worker.gate->mutex);
        ++worker.gate->ready;
        worker.gate->changed.NotifyAll();
        while (!worker.gate->open) {
            (void)worker.gate->changed.Wait(worker.gate->mutex);
        }
    }

    void* window[ConcurrentWindow] = {};
    for (uint32 index = 0; index < ConcurrentPairs; ++index) {
        void*& slot = window[index % ConcurrentWindow];
        if (slot != nullptr && !worker.allocator->Deallocate(slot)) {
            worker.failed = true;
        }

        slot = nullptr;
        auto block = worker.allocator->Allocate(BlockBytes, BlockAlignment);
        if (!block) {
            worker.failed = true;
            continue;
        }

        Touch(block.Value(), index);
        slot = block.Value();
    }

    for (void* block : window) {
        if (block != nullptr && !worker.allocator->Deallocate(block)) {
            worker.failed = true;
        }
    }
}

bool MeasureConcurrent(Allocator& allocator, uint32 threads,
                       double& millionPairsPerSecond) noexcept {
    StartGate gate;
    Worker workers[ConcurrentThreads];
    uint32 started = 0;
    for (; started < threads; ++started) {
        workers[started].allocator = &allocator;
        workers[started].gate = &gate;
        if (workers[started].thread.Start(&RunWorker, &workers[started]) !=
            platform::ThreadStatus::Success) {
            break;
        }
    }

    uint64 opened = 0;
    {
        platform::LockGuard lock(gate.mutex);
        while (gate.ready < started) {
            (void)gate.changed.Wait(gate.mutex);
        }

        opened = platform::MonotonicNanoseconds();
        gate.open = true;
        gate.changed.NotifyAll();
    }

    bool ok = started == threads;
    for (uint32 index = 0; index < started; ++index) {
        ok = workers[index].thread.Join() == platform::ThreadStatus::Success && ok;
        ok = ok && !workers[index].failed;
    }

    const uint64 elapsed = platform::MonotonicNanoseconds() - opened;
    const double pairs = static_cast<double>(threads) * ConcurrentPairs;
    millionPairsPerSecond =
        pairs / (static_cast<double>(elapsed) / NanosecondsPerSecond) / PairsPerMillion;
    return ok;
}

// ---- 쉬는 worker의 캐시와 TrimMemory ----

// worker가 캐시를 채울 때 쓰는 요청 크기와, 크기마다 할당했다가 반환하는 블록 수다.
constexpr usize IdleTrimSizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
constexpr uint32 IdleTrimBlocksPerSize = 256;
constexpr uint32 IdleTrimWaitMs = 60000;
constexpr double BytesPerKiB = 1024.0;
constexpr double NanosecondsPerMicrosecond = 1000.0;
constexpr uint32 IdleTrimMaxWorkers = 64;

// 지금 쉬고 있는 worker 수를 따라가는 통과 allocator다. forwardMarks가 거짓이면 쉬는 표시를 상위로
// 넘기지 않는다(대조).
class IdleTrackingAllocator final : public Allocator {
public:
    IdleTrackingAllocator(Allocator& backing, bool forwardMarks) noexcept
        : backing_(backing), forwardMarks_(forwardMarks) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return backing_.Deallocate(address);
    }

    void TrimMemory() noexcept override {
        backing_.TrimMemory();
    }

    void MarkThreadIdle() noexcept override {
        if (forwardMarks_) {
            backing_.MarkThreadIdle();
        }

        platform::LockGuard lock(mutex_);
        ++idleNow_;
        changed_.NotifyAll();
    }

    void MarkThreadActive() noexcept override {
        {
            platform::LockGuard lock(mutex_);
            --idleNow_;
        }

        if (forwardMarks_) {
            backing_.MarkThreadActive();
        }
    }

    // 쉬는 worker가 count개가 될 때까지 기다린다.
    void WaitForIdle(uint32 count) noexcept {
        platform::LockGuard lock(mutex_);
        while (idleNow_ != count) {
            (void)changed_.Wait(mutex_);
        }
    }

private:
    Allocator& backing_;
    bool forwardMarks_;
    platform::Mutex mutex_;
    platform::ConditionVariable changed_;
    uint32 idleNow_ = 0;
};

// worker 작업들이 함께 쓰는 상태다.
struct IdleTrimShared {
    Allocator* allocator = nullptr;
    platform::Semaphore* entered = nullptr;
    platform::ManualResetEvent* release = nullptr;
    // 실패 수다(원자 접근).
    uint32 failures = 0;
};

// 크기마다 블록을 할당했다가 모두 반환해 이 스레드의 캐시를 채운다.
void FillThreadCache(IdleTrimShared& shared) noexcept {
    void* blocks[IdleTrimBlocksPerSize];
    uint32 failures = 0;
    for (usize bytes : IdleTrimSizes) {
        for (uint32 index = 0; index < IdleTrimBlocksPerSize; ++index) {
            auto block = shared.allocator->Allocate(bytes, BlockAlignment);
            blocks[index] = block ? block.Value() : nullptr;
            if (blocks[index] == nullptr) {
                ++failures;
                continue;
            }

            Touch(blocks[index], index);
        }

        for (uint32 index = 0; index < IdleTrimBlocksPerSize; ++index) {
            if (blocks[index] != nullptr && !shared.allocator->Deallocate(blocks[index])) {
                ++failures;
            }
        }
    }

    if (failures != 0) {
        (void)platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&shared.failures, failures);
    }
}

struct IdleTrimResult {
    usize committedBefore = 0;
    usize committedAfter = 0;
    uint64 trimNanoseconds = 0;
};

bool MeasureIdleTrim(uint32 workers, bool forwardMarks, IdleTrimResult& result) noexcept {
    SystemAllocator system;
    BinnedAllocator binned(system);
    IdleTrackingAllocator tracked(binned, forwardMarks);
    bool ok = true;
    {
        platform::Semaphore entered(0, workers);
        platform::ManualResetEvent release;
        IdleTrimShared shared;
        shared.allocator = &tracked;
        shared.entered = &entered;
        shared.release = &release;
        WorkerScheduler scheduler(tracked);
        WorkerSchedulerConfig config;
        config.workerCount = workers;
        if (!scheduler.Start(config)) {
            return false;
        }

        // worker마다 작업 하나를 잡게 한다: 모든 작업이 캐시를 채우고 들어왔다고 알릴 때까지
        // 붙든다.
        WorkerTaskHandle handles[IdleTrimMaxWorkers];
        for (uint32 index = 0; index < workers && ok; ++index) {
            auto body = Function<void(WorkerTaskContext&)>::TryCreate(
                tracked, [&shared](WorkerTaskContext&) noexcept {
                    FillThreadCache(shared);
                    (void)shared.entered->Release();
                    (void)shared.release->Wait(IdleTrimWaitMs);
                });
            if (!body) {
                ok = false;
                break;
            }

            auto submitted = scheduler.Submit(Move(body.Value()));
            if (!submitted) {
                ok = false;
                break;
            }

            handles[index] = Move(submitted.Value());
        }

        for (uint32 index = 0; index < workers && ok; ++index) {
            ok = entered.Wait(IdleTrimWaitMs) == platform::WaitResult::Signaled;
        }

        release.Signal();
        for (uint32 index = 0; index < workers && ok; ++index) {
            auto waited = handles[index].Wait(IdleTrimWaitMs);
            ok = waited && waited.Value() == platform::WaitResult::Signaled;
        }

        if (ok) {
            // 모든 worker가 대기에 들어간 뒤에 잰다.
            tracked.WaitForIdle(workers);
            result.committedBefore = binned.Usage().committedBytes;
            const uint64 opened = platform::MonotonicNanoseconds();
            tracked.TrimMemory();
            result.trimNanoseconds = platform::MonotonicNanoseconds() - opened;
            result.committedAfter = binned.Usage().committedBytes;
        }

        ok = scheduler.Shutdown() && ok;
        ok = ok && platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&shared.failures) == 0;
    }

    return ok && binned.Stats().liveAllocations == 0;
}

} // namespace

int RunIdleTrimBench(uint32 workers) noexcept {
    if (workers == 0 || workers > IdleTrimMaxWorkers) {
        printf("allocator idle trim bench: workers must be 1..%u\n", IdleTrimMaxWorkers);
        return 9;
    }

    const bool modes[] = {true, false};
    for (const bool forwardMarks : modes) {
        IdleTrimResult result;
        if (!MeasureIdleTrim(workers, forwardMarks, result)) {
            printf("allocator idle trim bench failed\n");
            return 9;
        }

        printf("  idle trim x%u workers, idle marks %s: committed %.0f KiB -> %.0f KiB, "
               "TrimMemory %.0f us\n",
               workers, forwardMarks ? "forwarded" : "dropped (control)",
               static_cast<double>(result.committedBefore) / BytesPerKiB,
               static_cast<double>(result.committedAfter) / BytesPerKiB,
               static_cast<double>(result.trimNanoseconds) / NanosecondsPerMicrosecond);
        fflush(stdout);
    }

    return 0;
}

int RunAllocatorMicroBench(Allocator& allocator, const char* name) noexcept {
    printf("  %-16s pairs ns:", name);
    for (usize bytes : PairSizes) {
        double nanoseconds = 0.0;
        if (!MeasurePairs(allocator, bytes, nanoseconds)) {
            printf("\nallocator micro bench failed (pairs %llu)\n",
                   static_cast<unsigned long long>(bytes));
            return 9;
        }

        printf(" %lluB %.1f", static_cast<unsigned long long>(bytes), nanoseconds);
    }

    printf("\n");
    fflush(stdout);
    double batch = 0.0;
    uint32 rounds = 0;
    if (!MeasureBatch(allocator, batch, rounds)) {
        printf("allocator micro bench failed (batch)\n");
        return 9;
    }

    printf("  %-16s batch %ux%lluB: %.1f ns/op (%u rounds)\n", name, BatchBlocks,
           static_cast<unsigned long long>(BlockBytes), batch, rounds);
    fflush(stdout);
    double cross = 0.0;
    double same = 0.0;
    if (!MeasureHandoff(allocator, cross, same)) {
        printf("allocator micro bench failed (handoff)\n");
        return 9;
    }

    printf("  %-16s handoff %ux%lluB: cross-thread %.1f ns/pair, same-thread %.1f ns/pair "
           "(x%.2f)\n",
           name, HandoffBlocks, static_cast<unsigned long long>(BlockBytes), cross, same,
           same > 0.0 ? cross / same : 0.0);
    fflush(stdout);
    double single = 0.0;
    double parallel = 0.0;
    if (!MeasureConcurrent(allocator, 1, single) ||
        !MeasureConcurrent(allocator, ConcurrentThreads, parallel)) {
        printf("allocator micro bench failed (concurrent)\n");
        return 9;
    }

    printf("  %-16s concurrent %lluB: x1 %.2f Mpairs/s, x%u %.2f Mpairs/s (scaling x%.2f)\n", name,
           static_cast<unsigned long long>(BlockBytes), single, ConcurrentThreads, parallel,
           single > 0.0 ? parallel / single : 0.0);
    fflush(stdout);
    return 0;
}

} // namespace gc_bench
