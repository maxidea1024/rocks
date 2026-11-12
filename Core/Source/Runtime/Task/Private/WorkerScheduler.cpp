#include <Task/WorkerScheduler.h>
#include <Base/Checked.h>
#include <Platform/Clock.h>

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

bool IsTerminal(WorkerTaskState state) noexcept {
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
    if (record_->references == static_cast<uint64>(-1)) {
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
        if (!IsTerminal(record_->status.state)) {
            record_->status.cancellationRequested = true;
        }
    }
    state.changed.NotifyAll();
    return Result<void>::Success();
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
    platform::LockGuard lock(state.gate);
    while (!IsTerminal(record_->status.state)) {
        const uint32 remaining = Remaining(timeoutMs, start);
        if (remaining == 0) {
            return Result<platform::WaitResult>::Success(platform::WaitResult::TimedOut);
        }
        if (state.changed.Wait(state.gate, remaining) == platform::WaitResult::Failed) {
            return Result<platform::WaitResult>::Success(platform::WaitResult::Failed);
        }
    }
    return Result<platform::WaitResult>::Success(platform::WaitResult::Signaled);
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
        state = ConstructAt<WorkerState>(memory.Value(), allocator_);
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
        } else if (state->generation == static_cast<uint64>(-1)) {
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
        ConstructAt<WorkerSlot>(&workers[index]);
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

Result<WorkerTaskHandle>
WorkerScheduler::Submit(Function<void(WorkerTaskContext&)>&& body,
                        Span<const WorkerTaskHandle> dependencies) noexcept {
    if (!body) {
        return Result<WorkerTaskHandle>::Failure(ErrorCode::InvalidArgument);
    }
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
        if (dependencies.Size() > state->maxDependencies) {
            return Result<WorkerTaskHandle>::Failure(ErrorCode::CapacityExceeded);
        }
        if (state->inFlight + state->reservations >= state->capacity) {
            return Result<WorkerTaskHandle>::Failure(ErrorCode::CapacityExceeded);
        }
        if (state->references == static_cast<uint64>(-1)) {
            return Result<WorkerTaskHandle>::Failure(ErrorCode::Overflow);
        }
        for (usize index = 0; index < dependencies.Size(); ++index) {
            WorkerRecord* dependency = dependencies[index].record_;
            if (dependency == nullptr) {
                return Result<WorkerTaskHandle>::Failure(ErrorCode::InvalidArgument);
            }
            if (dependency->state != state || dependency->generation != state->generation) {
                return Result<WorkerTaskHandle>::Failure(ErrorCode::WrongContext);
            }
            if (dependency->references == static_cast<uint64>(-1)) {
                return Result<WorkerTaskHandle>::Failure(ErrorCode::Overflow);
            }
            for (usize earlier = 0; earlier < index; ++earlier) {
                if (retained[earlier] == dependency) {
                    return Result<WorkerTaskHandle>::Failure(ErrorCode::InvalidArgument);
                }
            }
            retained[index] = dependency;
        }
        count = static_cast<uint32>(dependencies.Size());
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
    WorkerRecord* record = ConstructAt<WorkerRecord>(memory.Value(), *state);
    record->dependencyCount = count;
    for (uint32 index = 0; index < count; ++index) {
        record->dependencies[index] = retained[index];
    }
    bool accepted = false;
    {
        platform::LockGuard lock(state->gate);
        if (state->accepting) {
            for (uint32 index = 0; index < state->capacity; ++index) {
                if (state->records[index] == nullptr) {
                    record->generation = state->generation;
                    record->body = Move(body); // 빈 목적지이며 capture 소멸자를 호출하지 않는다.
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
        ClearDependencies(*record);
        ReleaseRecord(record);
        EndReservation(*state);
        return Result<WorkerTaskHandle>::Failure(ErrorCode::ShuttingDown);
    }
    return Result<WorkerTaskHandle>::Success(WorkerTaskHandle(record));
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
                        bool ready = true;
                        bool failed = false;
                        for (uint32 dependency = 0; dependency < record->dependencyCount;
                             ++dependency) {
                            const auto status = record->dependencies[dependency]->status.state;
                            if (!IsTerminal(status)) {
                                ready = false;
                            } else if (status != WorkerTaskState::Succeeded) {
                                failed = true;
                            }
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
                ROCKS_ASSERT(state.changed.Wait(state.gate) != platform::WaitResult::Failed);
            }
        }
        if (run) {
            WorkerTaskContext context(selected);
            body(context);
            error = context.Error();
            {
                platform::LockGuard lock(state.gate);
                selected->status.state = WorkerTaskState::Completing;
            }
        }
        // 사용자 코드와 allocator 콜백은 모두 gate 밖이다. 재진입 Submit/Cancel/Stats가 가능하다.
        body.Reset();
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
