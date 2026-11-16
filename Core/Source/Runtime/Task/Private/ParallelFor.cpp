#include <Task/ParallelFor.h>
#include <Platform/Atomics.h>

namespace rocks::detail {
namespace {

// 호출 스레드 하나에 helper가 최대 worker 수(64)만큼 붙는다. 그 수의 handle만 스택에 둔다.
constexpr uint32 MaxHelpers = 64;

// worker 하나당 batch 수의 목표다. 너무 작으면 batch 크기 차이로 마지막에 기다리는 시간이 늘고,
// 너무 크면 가져가기 비용이 는다(UE ParallelFor도 참여자 수의 몇 배로 나눈다).
constexpr usize BatchesPerParticipant = 4;

// 호출 스레드와 helper가 공유하는 분배 상태다. 호출 스레드의 스택에 있고, 모든 helper가 terminal이
// 된 뒤에만 사라진다.
struct ParallelForShared {
    void* body = nullptr;
    ParallelForInvoke invoke = nullptr;
    usize count = 0;
    usize batchSize = 1;
    usize batchCount = 0;
    usize nextBatch = 0;
    uint32 stopped = 0;
    platform::Mutex failureGate;
    usize failedBatch = IndexNone;
    ErrorCode failure = ErrorCode::None;

    void Participate(const WorkerTaskContext* context) noexcept {
        for (;;) {
            if (platform::AtomicLoad<platform::MemoryOrder::Acquire>(&stopped) != 0) {
                return;
            }

            // 취소된 helper는 더 가져가지 않는다. 남은 batch는 호출 스레드가 실행한다.
            if (context != nullptr && context->IsCancellationRequested()) {
                return;
            }

            const usize batch = platform::AtomicFetchAdd(&nextBatch, usize{1});
            if (batch >= batchCount) {
                return;
            }

            const usize begin = batch * batchSize;
            const usize end = count - begin < batchSize ? count : begin + batchSize;
            const ErrorCode error = invoke(body, begin, end);
            if (error != ErrorCode::None) {
                {
                    platform::LockGuard lock(failureGate);
                    if (batch < failedBatch) {
                        failedBatch = batch;
                        failure = error;
                    }
                }

                platform::AtomicStore<platform::MemoryOrder::Release>(&stopped, 1u);
            }
        }
    }
};

} // namespace

Result<void> RunParallelFor(WorkerScheduler& scheduler, Allocator& scratch, usize count, void* body,
                            ParallelForInvoke invoke, const ParallelForOptions& options) noexcept {
    if (count == 0) {
        return Result<void>::Success();
    }

    // worker 안에서는 helper를 기다릴 수 없으므로(WorkerTaskHandle::Wait가 WrongThread) 혼자
    // 실행한다.
    uint64 helpers = 0;
    if (!scheduler.IsCurrentWorker()) {
        helpers = scheduler.Stats().workerCount;
        helpers = helpers < options.maxHelpers ? helpers : options.maxHelpers;
        helpers = helpers < MaxHelpers ? helpers : MaxHelpers;
    }

    const usize minimum = options.minBatchSize == 0 ? 1 : options.minBatchSize;
    const usize participants = static_cast<usize>(helpers) + 1;
    const usize targetBatches = participants * BatchesPerParticipant;
    usize batchSize = count / targetBatches + (count % targetBatches != 0 ? 1 : 0);
    batchSize = batchSize < minimum ? minimum : batchSize;

    ParallelForShared shared;
    shared.body = body;
    shared.invoke = invoke;
    shared.count = count;
    shared.batchSize = batchSize;
    shared.batchCount = count / batchSize + (count % batchSize != 0 ? 1 : 0);
    if (helpers > shared.batchCount - 1) {
        helpers = shared.batchCount - 1;
    }

    // helper는 호출 스레드의 lane 표지를 물려받는다. None·Game은 물려주지 않는다(Game lane 전용
    // 검사가 worker에서 통과하지 않게).
    WorkerSubmitOptions submitOptions;
    const TaskLane lane = CurrentTaskLane();
    submitOptions.lane = lane == TaskLane::None || lane == TaskLane::Game ? TaskLane::Worker : lane;

    WorkerTaskHandle handles[MaxHelpers];
    uint32 submitted = 0;
    while (submitted < helpers) {
        auto helper = Function<void(WorkerTaskContext&)>::TryCreate(
            scratch,
            [&shared](WorkerTaskContext& context) noexcept { shared.Participate(&context); });
        if (!helper) {
            break;
        }

        auto handle = scheduler.Submit(Move(helper.Value()), {}, submitOptions);
        if (!handle) {
            break;
        }

        handles[submitted++] = Move(handle.Value());
    }

    shared.Participate(nullptr);

    // helper 본문이 shared(이 스택)를 참조하므로 모든 helper가 terminal이 된 뒤 반환한다. 아직
    // 꺼내지 않은 helper는 회수하고(worker가 바빠도 끝난다) 실행 중인 helper만 기다린다. 호출
    // 스레드가 worker가 아님을 앞에서 확인했으므로 Wait는 WrongThread가 아니다.
    for (uint32 index = 0; index < submitted; ++index) {
        auto revoked = handles[index].TryRevoke();
        ROCKS_ASSERT(revoked.IsOk());
        if (!revoked.Value()) {
            auto waited = handles[index].Wait(platform::InfiniteTimeout);
            ROCKS_ASSERT(waited.IsOk() && waited.Value() == platform::WaitResult::Signaled);
        }
    }

    if (shared.failedBatch != IndexNone) {
        return Result<void>::Failure(shared.failure);
    }

    return Result<void>::Success();
}

} // namespace rocks::detail
