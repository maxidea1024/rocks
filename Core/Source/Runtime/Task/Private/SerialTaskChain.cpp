#include <Task/SerialTaskChain.h>

namespace rocks {
namespace {

bool IsTerminal(const WorkerTaskHandle& handle) noexcept {
    auto status = handle.Status();
    if (!status) {
        return true;
    }
    const WorkerTaskState state = status.Value().state;
    return state == WorkerTaskState::Succeeded || state == WorkerTaskState::Failed ||
           state == WorkerTaskState::Cancelled;
}

} // namespace

SerialTaskChain::SerialTaskChain(WorkerScheduler& scheduler, TaskLane lane) noexcept
    : scheduler_(scheduler), lane_(lane) {
}

Result<WorkerTaskHandle>
SerialTaskChain::Submit(Function<void(WorkerTaskContext&)>&& body,
                        Span<const WorkerTaskHandle> dependencies) noexcept {
    platform::LockGuard lock(gate_);
    // 이미 끝난 직전 작업은 선행으로 걸 필요가 없다. 놓아 두면 결과 record를 오래 붙잡고,
    // scheduler가 다시 시작된 뒤에는 이전 세대 handle이라 WrongContext가 된다(Shutdown은 모든
    // 작업을 끝낸다).
    if (last_.IsValid() && IsTerminal(last_)) {
        last_.Reset();
    }
    WorkerSubmitOptions options;
    options.lane = lane_;
    if (last_.IsValid()) {
        options.orderDependencies = Span<const WorkerTaskHandle>(&last_, 1);
    }
    auto submitted = scheduler_.Submit(Move(body), dependencies, options);
    if (!submitted) {
        return submitted;
    }
    // Clone은 참조 계수 포화(uint64)에서만 실패한다. 수락된 작업을 되돌릴 수 없으므로 계약 위반으로
    // 본다.
    auto retained = submitted.Value().Clone();
    ROCKS_ASSERT(retained.IsOk());
    last_ = Move(retained.Value());
    return submitted;
}

Result<WorkerTaskHandle> SerialTaskChain::Last() const noexcept {
    platform::LockGuard lock(gate_);
    if (!last_.IsValid()) {
        return Result<WorkerTaskHandle>::Success(WorkerTaskHandle());
    }
    return last_.Clone();
}

Result<platform::WaitResult> SerialTaskChain::Wait(uint32 timeoutMs) const noexcept {
    if (scheduler_.IsCurrentWorker()) {
        return Result<platform::WaitResult>::Failure(ErrorCode::WrongThread);
    }
    auto last = Last();
    if (!last) {
        return Result<platform::WaitResult>::Failure(last.Error());
    }
    if (!last.Value().IsValid()) {
        return Result<platform::WaitResult>::Success(platform::WaitResult::Signaled);
    }
    return last.Value().Wait(timeoutMs);
}

} // namespace rocks
