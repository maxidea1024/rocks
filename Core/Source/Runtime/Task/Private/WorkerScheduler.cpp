#include <Task/WorkerScheduler.h>
#include <Base/Checked.h>
#include <Platform/Clock.h>
#include <TraceCapture/TraceTask.h>

namespace rocks::detail {
struct WorkerSlot {
    WorkerState* state = nullptr;
    platform::Thread thread;
};

struct WorkerState {
    explicit WorkerState(Allocator& source) noexcept : allocator(&source) {
    }

    Allocator* allocator;
    platform::Mutex gate;
    platform::ConditionVariable changed;
    uint64 references = 1; // scheduler owner + 각각의 record storage
    uint64 generation = 0;
    uint32 workerCount = 0;
    uint32 capacity = 0;
    uint32 maxDependencies = 0;
    uint32 inFlight = 0;
    uint32 reservations = 0;
    bool accepting = false;
    bool starting = false;
    bool stopping = false;
    WorkerSlot* workers = nullptr;
    WorkerRecord** records = nullptr;
};

struct WorkerRecord {
    explicit WorkerRecord(WorkerState& owner) noexcept : state(&owner) {
    }

    WorkerState* state;
    uint64 generation = 0;
    uint64 references = 1; // caller handle; 승인 뒤 scheduler 보존분 1개 추가
    WorkerTaskStatus status;
    Function<void(WorkerTaskContext&)> body;
    WorkerRecord* dependencies[64]{};
    uint32 dependencyCount = 0;
    // dependencies 칸 가운데 순서 전용 선행(결과 무관, WorkerSubmitOptions::orderDependencies)의
    // bit다.
    uint64 orderOnlyMask = 0;
    // body·capture 정리 동안의 lane 표지다. 수락 전에 정하고 바꾸지 않는다.
    TaskLane lane = TaskLane::Worker;
    // 외부 완료 이벤트(WorkerTaskEvent)다. worker는 취소 요청이 있을 때만 꺼내 Cancelled로 끝낸다.
    bool external = false;
    // 수락 뒤 terminal 전까지 들어 있는 state.records 칸이다(회수·이벤트 완료가 선형 탐색하지
    // 않는다).
    uint32 slot = 0;
    // trace 신원과 만들 때 저장한 문맥이다. records에 공개하기 전에 채우고 그 뒤 바꾸지 않는다.
    // task 채널이 꺼져 있으면 id가 0이고, 모든 사건 지점이 분기 하나로 끝난다.
    TraceTaskInfo trace;
};
} // namespace rocks::detail

namespace rocks {
namespace {
using detail::WorkerRecord;
using detail::WorkerSlot;
using detail::WorkerState;
thread_local WorkerState* executingState = nullptr;
struct SubmitScope;
thread_local SubmitScope* submittingScope = nullptr;

bool WorkerSchedulerIsTerminal(WorkerTaskState state) noexcept {
    return state == WorkerTaskState::Succeeded || state == WorkerTaskState::Failed ||
           state == WorkerTaskState::Cancelled;
}

void ReleaseState(WorkerState* state) noexcept {
    bool last;
    {
        platform::LockGuard lock(state->gate);
        ROCKS_ASSERT(state->references != 0);
        last = --state->references == 0;
    }
    if (last) {
        ROCKS_ASSERT(state->workers == nullptr && state->records == nullptr);
        Allocator* allocator = state->allocator;
        DestroyAt(state);
        ROCKS_ASSERT(allocator->Deallocate(state));
    }
}

void ReleaseRecord(WorkerRecord* record) noexcept {
    if (record == nullptr) {
        return;
    }
    WorkerState* state = record->state;
    bool last;
    {
        platform::LockGuard lock(state->gate);
        ROCKS_ASSERT(record->references != 0);
        last = --record->references == 0;
    }
    if (last) {
        // 실행 자원은 terminal 게시 전에 정리된다. 결과 handle 해제는 capture를 실행하지 않는다.
        ROCKS_ASSERT(!record->body && record->dependencyCount == 0);
        DestroyAt(record);
        ROCKS_ASSERT(state->allocator->Deallocate(record));
        ReleaseState(state);
    }
}

void ClearDependencies(WorkerRecord& record) noexcept {
    const uint32 count = record.dependencyCount;
    record.dependencyCount = 0;
    for (uint32 index = 0; index < count; ++index) {
        WorkerRecord* dependency = record.dependencies[index];
        record.dependencies[index] = nullptr;
        ReleaseRecord(dependency);
    }
}

struct SubmitScope {
    explicit SubmitScope(WorkerState* owner) noexcept : state(owner), previous(submittingScope) {
        submittingScope = this;
    }

    ~SubmitScope() noexcept {
        ROCKS_ASSERT(submittingScope == this);
        submittingScope = previous;
    }

    SubmitScope(const SubmitScope&) = delete;
    SubmitScope& operator=(const SubmitScope&) = delete;

    WorkerState* state;
    SubmitScope* previous;
};

bool IsSubmitting(WorkerState* state) noexcept {
    // A 할당기 → B.Submit → B 할당기 → A.Shutdown도 A의 예약 반환을 기다리면 교착된다.
    // 가장 안쪽 scheduler뿐 아니라 이 스레드의 모든 활성 제출을 할당 없는 스택으로 검사한다.
    for (const SubmitScope* scope = submittingScope; scope != nullptr; scope = scope->previous) {
        if (scope->state == state) {
            return true;
        }
    }
    return false;
}

// gate 안에서 부른다. 실행(Running)·정리(Completing) 중인 작업이 없으면 취소 요청이 없는 외부
// 이벤트에 취소를 요청하고 하나라도 바꿨으면 true다(호출자는 다시 고른다).
// 순서 전용 선행을 기다리는 작업(취소 요청이 있어도)은 막힘이 아니다. 선행 사슬은 DAG라 끝을
// 따라가면 실행 중인 작업, 고를 수 있는 작업, 또는 이 함수가 취소할 외부 이벤트에 닿는다. 그래서
// 고를 작업이 없고 실행·정리 중인 작업도 없으면 남은 원인은 완료되지 않은 외부 이벤트뿐이다.
bool CancelStalledEvents(WorkerState& state) noexcept {
    for (uint32 index = 0; index < state.capacity; ++index) {
        const WorkerRecord* record = state.records[index];
        if (record != nullptr && (record->status.state == WorkerTaskState::Running ||
                                  record->status.state == WorkerTaskState::Completing)) {
            return false;
        }
    }

    bool changed = false;
    for (uint32 index = 0; index < state.capacity; ++index) {
        WorkerRecord* record = state.records[index];
        if (record != nullptr && record->external && !record->status.cancellationRequested) {
            record->status.cancellationRequested = true;
            changed = true;
        }
    }
    return changed;
}

// gate 안에서 부른다. 끝 상태를 trace 사건으로 남긴다(취소는 Cancelled, 그 밖은 Completed).
void TraceTerminal(const WorkerRecord& record) noexcept {
    if (!record.trace.id.IsTraced()) {
        return;
    }

    if (record.status.state == WorkerTaskState::Cancelled) {
        TraceTaskCancelled(record.trace.id);
    } else {
        TraceTaskCompleted(record.trace.id, record.status.error);
    }
}

// gate 안에서 부른다. 의존이 모두 성공으로 끝났는지다.
bool DependenciesSucceeded(const WorkerRecord& record) noexcept {
    for (uint32 index = 0; index < record.dependencyCount; ++index) {
        if (record.dependencies[index]->status.state != WorkerTaskState::Succeeded) {
            return false;
        }
    }

    return true;
}

// gate 안에서 부른다. completed가 성공으로 끝나 실행 가능해진 작업의 Ready 사건을 낸다.
void TraceDependentsReady(WorkerState& state, const WorkerRecord& completed) noexcept {
    if (!completed.trace.id.IsTraced() || completed.status.state != WorkerTaskState::Succeeded) {
        return;
    }

    for (uint32 index = 0; index < state.capacity; ++index) {
        const WorkerRecord* record = state.records[index];
        if (record == nullptr || record == &completed || !record->trace.id.IsTraced() ||
            record->status.state != WorkerTaskState::Pending) {
            continue;
        }

        bool dependsOnCompleted = false;
        for (uint32 dependency = 0; dependency < record->dependencyCount; ++dependency) {
            dependsOnCompleted =
                dependsOnCompleted || record->dependencies[dependency] == &completed;
        }

        if (dependsOnCompleted && DependenciesSucceeded(*record)) {
            TraceTaskReady(record->trace.id);
        }
    }
}

void EndReservation(WorkerState& state) noexcept {
    {
        platform::LockGuard lock(state.gate);
        ROCKS_ASSERT(state.reservations != 0);
        --state.reservations;
    }
    state.changed.NotifyAll();
}

uint32 Remaining(uint32 timeoutMs, uint64 start) noexcept {
    if (timeoutMs == platform::InfiniteTimeout) {
        return timeoutMs;
    }
    const uint64 elapsed = platform::MonotonicNanoseconds() - start;
    const uint64 duration = static_cast<uint64>(timeoutMs) * 1000000;
    if (elapsed >= duration) {
        return 0;
    }
    const uint64 rest = duration - elapsed;
    return static_cast<uint32>(rest / 1000000 + (rest % 1000000 != 0));
}

void DestroyWorkerStorage(WorkerState& state, WorkerSlot* workers, uint32 count,
                          WorkerRecord** records) noexcept {
    for (uint32 index = 0; index < count; ++index) {
        DestroyAt(&workers[index]);
    }
    if (workers != nullptr) {
        ROCKS_ASSERT(state.allocator->Deallocate(workers));
    }
    if (records != nullptr) {
        ROCKS_ASSERT(state.allocator->Deallocate(records));
    }
}
} // namespace

bool WorkerTaskContext::IsCancellationRequested() const noexcept {
    platform::LockGuard lock(record_->state->gate);
    return record_->status.cancellationRequested;
}

void WorkerTaskContext::Fail(ErrorCode error) noexcept {
    error_ = error == ErrorCode::None ? ErrorCode::InvalidArgument : error;
}

WorkerTaskHandle::WorkerTaskHandle(WorkerTaskHandle&& other) noexcept : record_(other.record_) {
    other.record_ = nullptr;
}

WorkerTaskHandle& WorkerTaskHandle::operator=(WorkerTaskHandle&& other) noexcept {
    if (this != &other) {
        WorkerRecord* old = record_;
        record_ = other.record_;
        other.record_ = nullptr;
        // allocator 해제 콜백이 이 래퍼에 재진입해도 이후 새 상태를 덮어쓰지 않는다.
        ReleaseRecord(old);
    }
    return *this;
}

WorkerTaskHandle::~WorkerTaskHandle() noexcept {
    Reset();
}

void WorkerTaskHandle::Reset() noexcept {
    WorkerRecord* record = record_;
    record_ = nullptr;
    ReleaseRecord(record);
}

Result<WorkerTaskHandle> WorkerTaskHandle::Clone() const noexcept {
    if (record_ == nullptr) {
        return Result<WorkerTaskHandle>::Failure(ErrorCode::StaleHandle);
    }
    platform::LockGuard lock(record_->state->gate);
    if (record_->references == NumericLimits<uint64>::Max()) {
        return Result<WorkerTaskHandle>::Failure(ErrorCode::Overflow);
    }
    ++record_->references;
    return Result<WorkerTaskHandle>::Success(WorkerTaskHandle(record_));
}

Result<WorkerTaskStatus> WorkerTaskHandle::Status() const noexcept {
    if (record_ == nullptr) {
        return Result<WorkerTaskStatus>::Failure(ErrorCode::StaleHandle);
    }
    platform::LockGuard lock(record_->state->gate);
    return Result<WorkerTaskStatus>::Success(record_->status);
}

Result<void> WorkerTaskHandle::RequestCancel() const noexcept {
    if (record_ == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }
    WorkerState& state = *record_->state;
    {
        platform::LockGuard lock(state.gate);
        if (!WorkerSchedulerIsTerminal(record_->status.state)) {
            record_->status.cancellationRequested = true;
        }
    }
    state.changed.NotifyAll();
    return Result<void>::Success();
}

Result<bool> WorkerTaskHandle::TryRevoke() const noexcept {
    if (record_ == nullptr) {
        return Result<bool>::Failure(ErrorCode::StaleHandle);
    }
    WorkerState& state = *record_->state;
    Function<void(WorkerTaskContext&)> body;
    uint32 slot = 0;
    {
        platform::LockGuard lock(state.gate);
        if (record_->status.state != WorkerTaskState::Pending) {
            return Result<bool>::Success(false);
        }

        // 순서 전용 선행이 끝나지 않았으면 회수하지 않는다. 지금 정리하면 그 선행이 실행 중일 때
        // 이 작업의 capture 정리가 겹치고, 이 작업이 먼저 terminal이 되어 뒤 작업이 선행보다 먼저
        // 시작할 수 있다.
        for (uint32 dependency = 0; dependency < record_->dependencyCount; ++dependency) {
            const bool orderOnly = (record_->orderOnlyMask & (uint64{1} << dependency)) != 0;
            if (orderOnly &&
                !WorkerSchedulerIsTerminal(record_->dependencies[dependency]->status.state)) {
                return Result<bool>::Success(false);
            }
        }

        // Pending 작업은 수락 뒤 terminal 전까지 records의 자기 칸에 있다. Completing으로 바꾸면
        // worker가 꺼내지 않는다(worker는 Pending만 고른다).
        slot = record_->slot;
        ROCKS_ASSERT(slot < state.capacity && state.records[slot] == record_);
        record_->status.cancellationRequested = true;
        record_->status.state = WorkerTaskState::Completing;
        body = Move(record_->body);
    }
    // worker 정리와 같은 순서다: capture와 dependency 보존분을 gate 밖에서 정리한 뒤 terminal을
    // 게시한다.
    body.Reset();
    ClearDependencies(*record_);
    {
        platform::LockGuard lock(state.gate);
        record_->status.state = WorkerTaskState::Cancelled;
        record_->status.error = ErrorCode::Cancelled;
        TraceTerminal(*record_);
        state.records[slot] = nullptr;
        --state.inFlight;
    }
    state.changed.NotifyAll();
    ReleaseRecord(record_); // scheduler의 실행 보존분을 반환한다.
    return Result<bool>::Success(true);
}

Result<platform::WaitResult> WorkerTaskHandle::Wait(uint32 timeoutMs) const noexcept {
    if (record_ == nullptr) {
        return Result<platform::WaitResult>::Failure(ErrorCode::StaleHandle);
    }
    WorkerState& state = *record_->state;
    if (executingState == &state) {
        return Result<platform::WaitResult>::Failure(ErrorCode::WrongThread);
    }
    const uint64 start = platform::MonotonicNanoseconds();
    // 기다림 시작·끝 사건이다. trace id는 공개 뒤 바뀌지 않으므로 gate 밖에서 읽는다.
    const TraceTaskId traced = record_->trace.id;
    TraceTaskWaitBegin(traced);
    platform::WaitResult result = platform::WaitResult::Signaled;
    {
        platform::LockGuard lock(state.gate);
        while (!WorkerSchedulerIsTerminal(record_->status.state)) {
            const uint32 remaining = Remaining(timeoutMs, start);
            if (remaining == 0) {
                result = platform::WaitResult::TimedOut;
                break;
            }
            if (state.changed.Wait(state.gate, remaining) == platform::WaitResult::Failed) {
                result = platform::WaitResult::Failed;
                break;
            }
        }
    }

    TraceTaskWaitEnd(traced);
    return Result<platform::WaitResult>::Success(result);
}

namespace {

// 외부 이벤트를 완료한다. Pending일 때만 바꾸며(이미 완료·취소·worker가 취소 처리 중이면 false),
// body·dependency가 없으므로 정리 없이 terminal을 게시하고 scheduler 보존분을 반환한다.
Result<bool> CompleteEvent(WorkerRecord* record, ErrorCode error) noexcept {
    if (record == nullptr) {
        return Result<bool>::Failure(ErrorCode::StaleHandle);
    }
    WorkerState& state = *record->state;
    {
        platform::LockGuard lock(state.gate);
        if (record->status.state != WorkerTaskState::Pending) {
            return Result<bool>::Success(false);
        }
        const uint32 slot = record->slot;
        ROCKS_ASSERT(record->external && slot < state.capacity && state.records[slot] == record);
        record->status.state =
            error == ErrorCode::None ? WorkerTaskState::Succeeded : WorkerTaskState::Failed;
        record->status.error = error;
        TraceTerminal(*record);
        TraceDependentsReady(state, *record);
        state.records[slot] = nullptr;
        --state.inFlight;
    }
    state.changed.NotifyAll();
    ReleaseRecord(record); // scheduler의 보존분을 반환한다.
    return Result<bool>::Success(true);
}

} // namespace

WorkerTaskEvent& WorkerTaskEvent::operator=(WorkerTaskEvent&& other) noexcept {
    if (this != &other) {
        CancelPending();
        handle_ = static_cast<WorkerTaskHandle&&>(other.handle_);
    }
    return *this;
}

WorkerTaskEvent::~WorkerTaskEvent() noexcept {
    CancelPending();
}

void WorkerTaskEvent::CancelPending() noexcept {
    if (!handle_.IsValid()) {
        return;
    }
    auto status = handle_.Status();
    if (status && status.Value().state == WorkerTaskState::Pending) {
        // Trigger 없이 버린 이벤트에 의존한 작업이 영원히 기다리지 않게 한다(worker가 Cancelled로
        // 끝낸다).
        (void)handle_.RequestCancel();
    }
}

Result<bool> WorkerTaskEvent::TryTrigger() const noexcept {
    return CompleteEvent(handle_.record_, ErrorCode::None);
}

Result<bool> WorkerTaskEvent::TryFail(ErrorCode error) const noexcept {
    return CompleteEvent(handle_.record_,
                         error == ErrorCode::None ? ErrorCode::InvalidArgument : error);
}

WorkerScheduler::WorkerScheduler(Allocator& allocator) noexcept
    : allocator_(allocator), owner_(platform::CurrentThreadId()) {
}

WorkerScheduler::~WorkerScheduler() noexcept {
    ROCKS_ASSERT(Shutdown().IsOk());
    WorkerState* state;
    {
        platform::LockGuard lock(lifecycleGate_);
        transitioning_ = true;
        state = state_;
        state_ = nullptr;
    }
    if (state != nullptr) {
        ReleaseState(state);
    }
}

Result<void> WorkerScheduler::Start(WorkerSchedulerConfig config) noexcept {
    if (platform::CurrentThreadId() != owner_) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }
    if (config.workerCount == 0 || config.workerCount > 64 || config.maxInFlight == 0 ||
        config.maxInFlight > 65536 || config.maxDependencies > 64) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    WorkerState* state;
    {
        platform::LockGuard lock(lifecycleGate_);
        if (transitioning_) {
            return Result<void>::Failure(ErrorCode::Busy);
        }
        transitioning_ = true;
        state = state_;
    }
    auto finish = [this](ErrorCode error) noexcept {
        platform::LockGuard lock(lifecycleGate_);
        transitioning_ = false;
        return error == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(error);
    };
    if (state == nullptr) {
        auto memory = allocator_.Allocate(sizeof(WorkerState), alignof(WorkerState));
        if (!memory) {
            return finish(memory.Error());
        }
        state = InitializeAt<WorkerState>(memory.Value(), allocator_);
        {
            platform::LockGuard lock(lifecycleGate_);
            state_ = state;
        }
    }
    ErrorCode validation = ErrorCode::None;
    {
        platform::LockGuard lock(state->gate);
        if (state->workers != nullptr || state->accepting) {
            validation = ErrorCode::AlreadyExists;
        } else if (state->generation == NumericLimits<uint64>::Max()) {
            validation = ErrorCode::Overflow;
        }
    }
    if (validation != ErrorCode::None) {
        return finish(validation);
    }
    usize workerBytes;
    usize queueBytes;
    if (!TryMultiply(config.workerCount, sizeof(WorkerSlot), workerBytes) ||
        !TryMultiply(config.maxInFlight, sizeof(WorkerRecord*), queueBytes)) {
        return finish(ErrorCode::Overflow);
    }
    auto workerMemory = allocator_.Allocate(workerBytes, alignof(WorkerSlot));
    if (!workerMemory) {
        return finish(workerMemory.Error());
    }
    auto* workers = static_cast<WorkerSlot*>(workerMemory.Value());
    for (uint32 index = 0; index < config.workerCount; ++index) {
        InitializeAt<WorkerSlot>(&workers[index]);
        workers[index].state = state;
    }
    auto queueMemory = allocator_.Allocate(queueBytes, alignof(WorkerRecord*));
    if (!queueMemory) {
        DestroyWorkerStorage(*state, workers, config.workerCount, nullptr);
        return finish(queueMemory.Error());
    }
    auto** records = static_cast<WorkerRecord**>(queueMemory.Value());
    for (uint32 index = 0; index < config.maxInFlight; ++index) {
        records[index] = nullptr;
    }
    {
        platform::LockGuard lock(state->gate);
        state->workers = workers;
        state->records = records;
        state->workerCount = config.workerCount;
        state->capacity = config.maxInFlight;
        state->maxDependencies = config.maxDependencies;
        state->starting = true;
        state->stopping = false;
    }
    uint32 started = 0;
    bool launchFailed = false;
    for (uint32 index = 0; index < config.workerCount; ++index) {
        const auto result = config.launch != nullptr
                                ? config.launch(config.launchContext, workers[index].thread,
                                                RunWorker, &workers[index])
                                : workers[index].thread.Start(RunWorker, &workers[index]);
        const bool joinable = workers[index].thread.IsJoinable();
        if (joinable) {
            ++started;
        }
        if (result != platform::ThreadStatus::Success || !joinable) {
            launchFailed = true;
            break;
        }
    }
    {
        platform::LockGuard lock(state->gate);
        state->starting = false;
        state->stopping = launchFailed;
        state->accepting = !launchFailed;
        if (!launchFailed) {
            ++state->generation;
        }
    }
    state->changed.NotifyAll();
    if (launchFailed) {
        for (uint32 index = 0; index < started; ++index) {
            ROCKS_ASSERT(workers[index].thread.Join() == platform::ThreadStatus::Success);
        }
        {
            platform::LockGuard lock(state->gate);
            state->workers = nullptr;
            state->records = nullptr;
            state->workerCount = 0;
        }
        DestroyWorkerStorage(*state, workers, config.workerCount, records);
        return finish(ErrorCode::DependencyFailed);
    }
    return finish(ErrorCode::None);
}

Result<WorkerTaskHandle> WorkerScheduler::Submit(Function<void(WorkerTaskContext&)>&& body,
                                                 Span<const WorkerTaskHandle> dependencies,
                                                 const WorkerSubmitOptions& options) noexcept {
    if (!body) {
        return Result<WorkerTaskHandle>::Failure(ErrorCode::InvalidArgument);
    }
    if (!IsValidTaskLane(options.lane)) {
        return Result<WorkerTaskHandle>::Failure(ErrorCode::InvalidArgument);
    }

    const Span<const WorkerTaskHandle> orderDependencies = options.orderDependencies;
    WorkerState* state;
    WorkerRecord* retained[64]{};
    uint32 count = 0;
    {
        platform::LockGuard ownerLock(lifecycleGate_);
        state = state_;
        if (state == nullptr) {
            return Result<WorkerTaskHandle>::Failure(ErrorCode::ShuttingDown);
        }
        platform::LockGuard lock(state->gate);
        if (!state->accepting) {
            return Result<WorkerTaskHandle>::Failure(ErrorCode::ShuttingDown);
        }
        if (dependencies.Size() > state->maxDependencies ||
            orderDependencies.Size() > state->maxDependencies - dependencies.Size()) {
            return Result<WorkerTaskHandle>::Failure(ErrorCode::CapacityExceeded);
        }
        if (state->inFlight + state->reservations >= state->capacity) {
            return Result<WorkerTaskHandle>::Failure(ErrorCode::CapacityExceeded);
        }
        if (state->references == NumericLimits<uint64>::Max()) {
            return Result<WorkerTaskHandle>::Failure(ErrorCode::Overflow);
        }
        // 일반 선행 뒤에 순서 전용 선행을 이어 한 목록으로 검사한다(두 목록 사이의 중복도 거부).
        const usize total = dependencies.Size() + orderDependencies.Size();
        for (usize index = 0; index < total; ++index) {
            WorkerRecord* dependency = index < dependencies.Size()
                                           ? dependencies[index].record_
                                           : orderDependencies[index - dependencies.Size()].record_;
            if (dependency == nullptr) {
                return Result<WorkerTaskHandle>::Failure(ErrorCode::InvalidArgument);
            }
            if (dependency->state != state || dependency->generation != state->generation) {
                return Result<WorkerTaskHandle>::Failure(ErrorCode::WrongContext);
            }
            if (dependency->references == NumericLimits<uint64>::Max()) {
                return Result<WorkerTaskHandle>::Failure(ErrorCode::Overflow);
            }
            for (usize earlier = 0; earlier < index; ++earlier) {
                if (retained[earlier] == dependency) {
                    return Result<WorkerTaskHandle>::Failure(ErrorCode::InvalidArgument);
                }
            }
            retained[index] = dependency;
        }
        count = static_cast<uint32>(total);
        for (uint32 index = 0; index < count; ++index) {
            ++retained[index]->references;
        }
        ++state->references; // 예약에서 record로 이전할 control 수명
        ++state->reservations;
    }
    SubmitScope scope(state);
    auto memory = state->allocator->Allocate(sizeof(WorkerRecord), alignof(WorkerRecord));
    if (!memory) {
        for (uint32 index = 0; index < count; ++index) {
            ReleaseRecord(retained[index]);
        }
        EndReservation(*state);
        ReleaseState(state);
        return Result<WorkerTaskHandle>::Failure(memory.Error());
    }
    WorkerRecord* record = InitializeAt<WorkerRecord>(memory.Value(), *state);
    record->dependencyCount = count;
    record->lane = options.lane;
    for (uint32 index = 0; index < count; ++index) {
        record->dependencies[index] = retained[index];
        if (index >= dependencies.Size()) {
            record->orderOnlyMask |= uint64{1} << index;
        }
    }
    // trace 신원은 worker가 볼 수 있게 되기(records 공개) 전에 정한다. 채널이 꺼져 있으면 분기
    // 하나다.
    record->trace = TraceCreateTask();
    if (record->trace.id.IsTraced()) {
        for (uint32 index = 0; index < count; ++index) {
            TraceTaskDependency(record->trace.id, retained[index]->trace.id);
        }
    }
    bool accepted = false;
    {
        platform::LockGuard lock(state->gate);
        if (state->accepting) {
            for (uint32 index = 0; index < state->capacity; ++index) {
                if (state->records[index] == nullptr) {
                    record->generation = state->generation;
                    record->body = Move(body); // 빈 목적지이며 capture 소멸자를 호출하지 않는다.
                    record->slot = index;
                    ++record->references;
                    state->records[index] = record;
                    ++state->inFlight;
                    --state->reservations;
                    accepted = true;
                    break;
                }
            }
            ROCKS_ASSERT(accepted); // 예약이 capacity를 이미 확보하였다.
            // 의존이 이미 모두 성공했으면 지금 실행 가능하다. gate 안에서 내야 worker의 Started보다
            // 앞선다.
            if (record->trace.id.IsTraced() && DependenciesSucceeded(*record)) {
                TraceTaskReady(record->trace.id);
            }
        }
    }
    state->changed.NotifyAll();
    if (!accepted) {
        TraceTaskCancelled(record->trace.id);
        ClearDependencies(*record);
        ReleaseRecord(record);
        EndReservation(*state);
        return Result<WorkerTaskHandle>::Failure(ErrorCode::ShuttingDown);
    }
    return Result<WorkerTaskHandle>::Success(WorkerTaskHandle(record));
}

Result<WorkerTaskEvent> WorkerScheduler::TryCreateEvent() noexcept {
    WorkerState* state;
    {
        platform::LockGuard ownerLock(lifecycleGate_);
        state = state_;
        if (state == nullptr) {
            return Result<WorkerTaskEvent>::Failure(ErrorCode::ShuttingDown);
        }
        platform::LockGuard lock(state->gate);
        if (!state->accepting) {
            return Result<WorkerTaskEvent>::Failure(ErrorCode::ShuttingDown);
        }
        if (state->inFlight + state->reservations >= state->capacity) {
            return Result<WorkerTaskEvent>::Failure(ErrorCode::CapacityExceeded);
        }
        if (state->references == NumericLimits<uint64>::Max()) {
            return Result<WorkerTaskEvent>::Failure(ErrorCode::Overflow);
        }
        ++state->references; // 예약에서 record로 이전할 control 수명
        ++state->reservations;
    }
    // Submit과 같은 예약 절차다(할당은 gate 밖, 할당기 재진입 Shutdown 검사 포함).
    SubmitScope scope(state);
    auto memory = state->allocator->Allocate(sizeof(WorkerRecord), alignof(WorkerRecord));
    if (!memory) {
        EndReservation(*state);
        ReleaseState(state);
        return Result<WorkerTaskEvent>::Failure(memory.Error());
    }
    WorkerRecord* record = InitializeAt<WorkerRecord>(memory.Value(), *state);
    record->external = true;
    record->trace = TraceCreateTask();
    bool accepted = false;
    {
        platform::LockGuard lock(state->gate);
        if (state->accepting) {
            for (uint32 index = 0; index < state->capacity; ++index) {
                if (state->records[index] == nullptr) {
                    record->generation = state->generation;
                    record->slot = index;
                    ++record->references;
                    state->records[index] = record;
                    ++state->inFlight;
                    --state->reservations;
                    accepted = true;
                    break;
                }
            }
            ROCKS_ASSERT(accepted); // 예약이 capacity를 이미 확보하였다.
        }
    }
    state->changed.NotifyAll();
    if (!accepted) {
        TraceTaskCancelled(record->trace.id);
        ReleaseRecord(record);
        EndReservation(*state);
        return Result<WorkerTaskEvent>::Failure(ErrorCode::ShuttingDown);
    }
    return Result<WorkerTaskEvent>::Success(WorkerTaskEvent(WorkerTaskHandle(record)));
}

void WorkerScheduler::RunWorker(void* raw) noexcept {
    WorkerState& state = *static_cast<WorkerSlot*>(raw)->state;
    executingState = &state; // body뿐 아니라 모든 capture/dependency cleanup까지 포함한다.
    for (;;) {
        WorkerRecord* selected = nullptr;
        uint32 selectedIndex = 0;
        bool run = false;
        ErrorCode error = ErrorCode::None;
        Function<void(WorkerTaskContext&)> body;
        {
            platform::LockGuard lock(state.gate);
            for (;;) {
                if (!state.starting) {
                    for (uint32 index = 0; index < state.capacity; ++index) {
                        WorkerRecord* record = state.records[index];
                        if (record == nullptr || record->status.state != WorkerTaskState::Pending) {
                            continue;
                        }
                        // 외부 이벤트는 TryTrigger·TryFail이 완료한다. 취소 요청만 worker가
                        // 처리한다.
                        if (record->external && !record->status.cancellationRequested) {
                            continue;
                        }
                        // 순서 전용 선행이 모두 끝나기 전에는 실행·취소·DependencyFailed 어느
                        // 경로로도 고르지 않는다. 그래야 순서 전용 선행과 이 작업의 body·capture
                        // 정리가 겹치지 않고, 이 작업의 terminal이 선행 전체의 terminal을 뜻한다
                        // (SerialTaskChain이 마지막 작업만 보고 체인 전체 완료를 판단한다).
                        bool orderReady = true;
                        bool ready = true;
                        bool failed = false;
                        for (uint32 dependency = 0; dependency < record->dependencyCount;
                             ++dependency) {
                            const auto status = record->dependencies[dependency]->status.state;
                            const bool orderOnly =
                                (record->orderOnlyMask & (uint64{1} << dependency)) != 0;
                            if (!WorkerSchedulerIsTerminal(status)) {
                                if (orderOnly) {
                                    orderReady = false;
                                } else {
                                    ready = false;
                                }
                            } else if (status != WorkerTaskState::Succeeded && !orderOnly) {
                                failed = true;
                            }
                        }

                        if (!orderReady) {
                            continue;
                        }

                        if (!record->status.cancellationRequested && !failed && !ready) {
                            continue;
                        }
                        selected = record;
                        selectedIndex = index;
                        run = !record->status.cancellationRequested && !failed;
                        error = failed ? ErrorCode::DependencyFailed : ErrorCode::None;
                        record->status.state =
                            run ? WorkerTaskState::Running : WorkerTaskState::Completing;
                        body = Move(record->body);
                        break;
                    }
                }
                if (selected != nullptr) {
                    break;
                }
                if (!state.starting && state.stopping && state.inFlight == 0 &&
                    state.reservations == 0) {
                    executingState = nullptr;
                    return;
                }
                // Drain 중 더 진행할 수 없으면(실행·정리 중인 작업이 없고 고를 작업도 없다) 남은
                // 것은 완료되지 않은 외부 이벤트와 그에 막힌 작업뿐이다. 이벤트를 취소해 Drain을
                // 끝낸다. 실행 중인 작업이 곧 Trigger할 이벤트는 그 작업이 끝날 때까지 취소하지
                // 않는다.
                if (!state.starting && state.stopping && state.reservations == 0 &&
                    CancelStalledEvents(state)) {
                    continue;
                }
                ROCKS_ASSERT(state.changed.Wait(state.gate) != platform::WaitResult::Failed);
            }
        }
        {
            // lane 표지는 body와 capture 정리를 함께 덮는다(DedicatedLane과 같은 범위).
            TaskLaneScope laneScope(selected->lane);
            if (run) {
                WorkerTaskContext context(selected);
                // 저장한 상관 id는 늘 복원하고(trace 채널·세션과 무관, 로깅 L15), 기록한 task면
                // 시작·끝 사건과 CPU 구간 "Task.Run"도 남긴다. 기록하지 않고 문맥도 없는 task는
                // 생성·소멸이 분기 하나씩이다(TraceTaskScope).
                TraceTaskScope traced(selected->trace);
                body(context);
                error = context.Error();
                {
                    platform::LockGuard lock(state.gate);
                    selected->status.state = WorkerTaskState::Completing;
                }
            }
            // 사용자 코드와 allocator 콜백은 모두 gate 밖이다. 재진입 Submit/Cancel/Stats가
            // 가능하다.
            body.Reset();
        }
        ClearDependencies(*selected);
        {
            platform::LockGuard lock(state.gate);
            if (error != ErrorCode::None) {
                selected->status.state = WorkerTaskState::Failed;
                selected->status.error = error;
            } else if (selected->status.cancellationRequested) {
                selected->status.state = WorkerTaskState::Cancelled;
                selected->status.error = ErrorCode::Cancelled;
            } else {
                selected->status.state = WorkerTaskState::Succeeded;
            }
            if (selected->trace.id.IsTraced()) {
                TraceTerminal(*selected);
                TraceDependentsReady(state, *selected);
            }
            state.records[selectedIndex] = nullptr;
            --state.inFlight;
        }
        state.changed.NotifyAll();
        ReleaseRecord(selected); // scheduler의 실행 보존분을 반환한다.
    }
}

Result<void> WorkerScheduler::Shutdown(WorkerShutdownMode mode) noexcept {
    if (platform::CurrentThreadId() != owner_) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }
    WorkerState* state;
    {
        platform::LockGuard lock(lifecycleGate_);
        state = state_;
        if (executingState == state && state != nullptr) {
            return Result<void>::Failure(ErrorCode::WrongThread);
        }
        if (transitioning_ || (state != nullptr && IsSubmitting(state))) {
            return Result<void>::Failure(ErrorCode::Busy);
        }
        transitioning_ = true;
    }
    if (state != nullptr) {
        WorkerSlot* workers;
        WorkerRecord** records;
        uint32 count;
        {
            platform::LockGuard lock(state->gate);
            state->accepting = false;
            state->stopping = true;
            if (mode == WorkerShutdownMode::Cancel && state->records != nullptr) {
                for (uint32 index = 0; index < state->capacity; ++index) {
                    if (state->records[index] != nullptr) {
                        state->records[index]->status.cancellationRequested = true;
                    }
                }
            }
            workers = state->workers;
            records = state->records;
            count = state->workerCount;
        }
        state->changed.NotifyAll();
        for (uint32 index = 0; index < count; ++index) {
            ROCKS_ASSERT(workers[index].thread.Join() == platform::ThreadStatus::Success);
        }
        {
            platform::LockGuard lock(state->gate);
            // worker가 없던 실패 Start에서도 예약은 없으며, 정상 drain은 이 합류 전에 끝난다.
            ROCKS_ASSERT(state->inFlight == 0 && state->reservations == 0);
            state->workers = nullptr;
            state->records = nullptr;
            state->workerCount = 0;
        }
        DestroyWorkerStorage(*state, workers, count, records);
    }
    {
        platform::LockGuard lock(lifecycleGate_);
        transitioning_ = false;
    }
    return Result<void>::Success();
}

WorkerSchedulerStats WorkerScheduler::Stats() const noexcept {
    platform::LockGuard ownerLock(lifecycleGate_);
    if (state_ == nullptr) {
        return {};
    }
    platform::LockGuard lock(state_->gate);
    return {state_->workerCount, state_->inFlight, state_->reservations, state_->generation,
            state_->accepting};
}

bool WorkerScheduler::IsCurrentWorker() const noexcept {
    platform::LockGuard lock(lifecycleGate_);
    return state_ != nullptr && executingState == state_;
}
} // namespace rocks
