#include <Task/DedicatedLane.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>

namespace rocks {

struct DedicatedLane::Node {
    explicit Node(Function<void()>&& body) noexcept : task(Move(body)) {
    }

    Function<void()> task;
    Node* next = nullptr;
};

namespace {

// 이 스레드가 지금 실행 중인 lane 작업의 사슬이다. Inline 모드에서 lane A의 작업이 lane B의 Post를
// 불러 B를 그 자리에서 비우면 B 칸이 A 칸 위에 쌓인다. 가장 안쪽 칸만 "실행 중"으로 보고
// (IsExecutingTask), 자기 자신을 기다리는 Flush·Shutdown은 사슬 전체를 보고 거부한다. 할당 없이
// 스택 객체로만 쌓는다.
struct LaneFrame {
    explicit LaneFrame(const DedicatedLane* owner) noexcept : lane(owner), previous(current) {
        current = this;
    }

    ~LaneFrame() noexcept {
        current = previous;
    }

    LaneFrame(const LaneFrame&) = delete;
    LaneFrame& operator=(const LaneFrame&) = delete;

    const DedicatedLane* lane;
    LaneFrame* previous;

    static thread_local LaneFrame* current;
};

thread_local LaneFrame* LaneFrame::current = nullptr;

ErrorCode ToErrorCode(platform::ThreadStatus status) noexcept {
    // 옵션 검사 실패만 호출자 잘못이다. 나머지(OS 거부)는 스레드·스택 자원 부족으로 본다.
    return status == platform::ThreadStatus::InvalidArgument ? ErrorCode::InvalidArgument
                                                             : ErrorCode::OutOfMemory;
}

} // namespace

DedicatedLane::DedicatedLane(Allocator& allocator) noexcept
    : allocator_(allocator), owner_(platform::CurrentThreadId()) {
}

DedicatedLane::~DedicatedLane() noexcept {
    bool running;
    {
        platform::LockGuard lock(gate_);
        running = running_;
    }

    if (running) {
        platform::Fatal("DedicatedLane destroyed without Shutdown");
    }
}

Result<void> DedicatedLane::Start(const DedicatedLaneConfig& config) noexcept {
    if (platform::CurrentThreadId() != owner_) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if ((config.mode != DedicatedLaneMode::Thread && config.mode != DedicatedLaneMode::Inline) ||
        !IsValidTaskLane(config.lane)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    {
        platform::LockGuard lock(gate_);
        if (running_) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        // Start·Shutdown은 생성 스레드 전용이라 이 사이에 다른 수명 전이가 끼어들지 않는다. mode_·
        // lane_·수락은 스레드 시작이 끝난 뒤에 바꾼다(실패하면 아무것도 바꾸지 않고, 이미 받은
        // 작업을 되돌릴 일도 없다).
        ROCKS_ASSERT(head_ == nullptr && !draining_ && !stopping_);
    }

    if (config.mode == DedicatedLaneMode::Thread) {
        started_.Reset();
        stopped_.Reset();
        const platform::ThreadStatus status =
            config.launch != nullptr
                ? config.launch(config.launchContext, thread_, RunThread, this, config.thread)
                : thread_.Start(RunThread, this, config.thread);
        if (status != platform::ThreadStatus::Success || !thread_.IsJoinable()) {
            StopUnstartedThread();
            return Result<void>::Failure(status == platform::ThreadStatus::Success
                                             ? ErrorCode::OutOfMemory
                                             : ToErrorCode(status));
        }

        // 시작 handshake: lane 스레드가 진입해 이름·우선순위 적용을 마쳤음을 알린 뒤에 연다.
        ROCKS_ASSERT(started_.Wait() == platform::WaitResult::Signaled);
    }

    platform::LockGuard lock(gate_);
    mode_ = config.mode;
    lane_ = config.lane;
    running_ = true;
    accepting_ = true;
    return Result<void>::Success();
}

void DedicatedLane::StopUnstartedThread() noexcept {
    // launcher가 실패를 돌려주면서도 스레드를 시작했으면(계약 위반) 그 스레드를 빈 큐로 끝내고
    // 회수한다. 수락을 열기 전이라 큐는 비어 있고, lane 스레드는 stopping_을 보고 바로 루프를
    // 끝낸다.
    if (!thread_.IsJoinable()) {
        return;
    }

    {
        platform::LockGuard lock(gate_);
        stopping_ = true;
        changed_.NotifyAll();
    }

    ROCKS_ASSERT(stopped_.Wait() == platform::WaitResult::Signaled);
    ROCKS_ASSERT(thread_.Join() == platform::ThreadStatus::Success);
    platform::LockGuard lock(gate_);
    stopping_ = false;
}

Result<void> DedicatedLane::Post(Function<void()>&& task) noexcept {
    if (!task) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 큐 칸은 잠금 밖에서 할당한다(allocator 콜백이 이 lane을 다시 불러도 교착하지 않는다). 수락이
    // 확정된 뒤에만 task를 옮기므로 거부·할당 실패 때 호출자의 task는 그대로다.
    auto memory = allocator_.Allocate(sizeof(Node), alignof(Node));
    if (!memory) {
        return Result<void>::Failure(memory.Error());
    }

    bool accepted = false;
    bool drain = false;
    {
        platform::LockGuard lock(gate_);
        const bool laneContinuation = running_ && stopping_ && IsInsideLane();
        if (accepting_ || laneContinuation) {
            Node* node = ConstructAt<Node>(memory.Value(), Move(task));
            if (tail_ != nullptr) {
                tail_->next = node;
            } else {
                head_ = node;
            }

            tail_ = node;
            ++posted_;
            accepted = true;
            if (mode_ == DedicatedLaneMode::Inline && !draining_) {
                draining_ = true;
                drain = true;
            }

            // 알림도 잠금 안에서 한다. 잠금을 놓은 뒤 알리면 그 사이 owner가 Shutdown을 마치고
            // lane을 파괴해 해제된 조건 변수를 건드릴 수 있다.
            changed_.NotifyAll();
        }
    }

    if (!accepted) {
        ROCKS_ASSERT(allocator_.Deallocate(memory.Value()).IsOk());
        return Result<void>::Failure(ErrorCode::ShuttingDown);
    }

    if (drain) {
        DrainInline();
    }

    return Result<void>::Success();
}

Result<void> DedicatedLane::Flush() noexcept {
    if (IsInsideLane()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    platform::LockGuard lock(gate_);
    if (!running_) {
        return Result<void>::Failure(ErrorCode::ShuttingDown);
    }

    // 완료 수는 수락 순서대로 늘어나므로(FIFO, 실행 흐름 하나) 지금까지 받은 수에 닿으면 그 전의
    // 작업이 모두 끝났다.
    const uint64 target = posted_;
    while (completed_ < target) {
        ROCKS_ASSERT(changed_.Wait(gate_) != platform::WaitResult::Failed);
    }

    return Result<void>::Success();
}

Result<void> DedicatedLane::Shutdown() noexcept {
    if (platform::CurrentThreadId() != owner_ || IsInsideLane()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    DedicatedLaneMode mode;
    {
        platform::LockGuard lock(gate_);
        if (!running_) {
            return Result<void>::Success();
        }

        accepting_ = false;
        stopping_ = true;
        mode = mode_;
        changed_.NotifyAll();
    }

    if (mode == DedicatedLaneMode::Thread) {
        // 종료 handshake: lane 스레드는 큐가 비고 stopping_을 본 뒤에만 루프를 끝내고 stopped_를
        // 켠다.
        ROCKS_ASSERT(stopped_.Wait() == platform::WaitResult::Signaled);
        ROCKS_ASSERT(thread_.Join() == platform::ThreadStatus::Success);
    } else {
        // Inline 모드는 지금 큐를 비우는 다른 실행 흐름이 끝나기를 기다린다. 비우는 흐름이 없으면
        // 큐도 비어 있다(Post가 비우는 흐름 없이 넣으면 그 Post가 비운다).
        platform::LockGuard lock(gate_);
        while (draining_ || head_ != nullptr) {
            ROCKS_ASSERT(changed_.Wait(gate_) != platform::WaitResult::Failed);
        }
    }

    platform::LockGuard lock(gate_);
    ROCKS_ASSERT(head_ == nullptr && completed_ == posted_);
    running_ = false;
    stopping_ = false;
    changed_.NotifyAll();
    return Result<void>::Success();
}

bool DedicatedLane::IsExecutingTask() const noexcept {
    return LaneFrame::current != nullptr && LaneFrame::current->lane == this;
}

DedicatedLaneStats DedicatedLane::Stats() const noexcept {
    platform::LockGuard lock(gate_);
    DedicatedLaneStats stats;
    stats.posted = posted_;
    stats.completed = completed_;
    stats.running = running_;
    stats.accepting = accepting_;
    stats.mode = mode_;
    return stats;
}

DedicatedLane::Node* DedicatedLane::PopLocked() noexcept {
    Node* node = head_;
    if (node != nullptr) {
        head_ = node->next;
        if (head_ == nullptr) {
            tail_ = nullptr;
        }
    }

    return node;
}

void DedicatedLane::RunNode(Node* node) noexcept {
    {
        // 실행 표지는 작업 본문과 capture 소멸까지 덮는다. capture 소멸자가 lane 전용 자원을 놓을
        // 수 있고, 그 안에서 자기 lane을 기다리면 교착이기 때문이다.
        LaneFrame frame(this);
        TaskLaneScope scope(lane_);
        node->task();
        node->task.Reset();
    }

    DestroyAt(node);
    ROCKS_ASSERT(allocator_.Deallocate(node).IsOk());
    platform::LockGuard lock(gate_);
    ++completed_;
    changed_.NotifyAll();
}

void DedicatedLane::DrainInline() noexcept {
    for (;;) {
        Node* node;
        {
            platform::LockGuard lock(gate_);
            node = PopLocked();
            if (node == nullptr) {
                // 비었음을 본 것과 draining_을 내리는 것이 같은 잠금 안이라, 그 사이 들어온 Post는
                // 이 흐름이 실행하거나(먼저 잠금을 얻음) 스스로 비운다(나중에 잠금을 얻음). 알림도
                // 잠금 안에서 한다. Inline Shutdown은 draining_이 내려가기를 기다렸다가 반환하고
                // owner는 곧 lane을 파괴할 수 있으므로, 잠금을 놓은 뒤에는 lane을 건드리지 않는다.
                draining_ = false;
                changed_.NotifyAll();
                return;
            }
        }

        RunNode(node);
    }
}

bool DedicatedLane::IsInsideLane() const noexcept {
    for (const LaneFrame* frame = LaneFrame::current; frame != nullptr; frame = frame->previous) {
        if (frame->lane == this) {
            return true;
        }
    }

    return false;
}

void DedicatedLane::RunThread(void* raw) noexcept {
    auto& lane = *static_cast<DedicatedLane*>(raw);
    lane.started_.Signal();
    for (;;) {
        Node* node;
        {
            platform::LockGuard lock(lane.gate_);
            for (;;) {
                node = lane.PopLocked();
                if (node != nullptr || lane.stopping_) {
                    break;
                }

                ROCKS_ASSERT(lane.changed_.Wait(lane.gate_) != platform::WaitResult::Failed);
            }
        }

        if (node == nullptr) {
            // 큐가 비었고 종료 요청이 있다. 이 시점에는 실행 중인 lane 작업이 없으므로 lane 작업
            // 안의 후속 Post도 더 오지 않고, 일반 Post는 accepting_이 닫혀 거부된다(Drain 완료).
            break;
        }

        lane.RunNode(node);
    }

    // Shutdown은 이 신호 뒤 Join으로 스레드 종료까지 기다리므로 이 Signal은 lane 파괴 전에 끝난다.
    lane.stopped_.Signal();
}

} // namespace rocks
