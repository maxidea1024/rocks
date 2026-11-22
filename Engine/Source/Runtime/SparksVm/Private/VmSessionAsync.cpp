#include <SparksVm/VmSession.h>

// async 실행의 구현이다(Docs/Design/SparksRuntime.ko.md 2.4절, LC6c): AWAIT 정지(frame stack 전체를
// session으로 옮김 — R9), 대기 표(Delay·NextTick·NextFixedTick·All·Race), 시계 진행과 재개 순서,
// instance 취소.

namespace rocks::sparks {

namespace {

constexpr uint64 VmWaitIndexMask = ~VmWaitTag;
constexpr uint8 VmGameClock = 0;

} // namespace

uint32 VmSession::SuspendedCount() const noexcept {
    uint32 count = 0;
    for (const Suspended& entry : suspended_) {
        count += entry.live ? 1 : 0;
    }

    return count;
}

uint64 VmSession::NewWait(WaitKind kind, uint8 clock, double seconds, Span<const uint64> children,
                          ErrorCode& error) noexcept {
    Wait wait;
    wait.kind = kind;
    wait.clock = clock;
    wait.frame = frame_;
    wait.fixedStep = fixedStep_;
    wait.instance = currentInstance_;
    wait.order = nextWaitOrder_++;
    // 기한: 등록한 프레임의 그 시계 값 + seconds(0 이하는 다음 프레임 — SL 6.1절).
    wait.deadline = (clock == VmGameClock ? gameTime_ : uiTime_) + (seconds > 0.0 ? seconds : 0.0);
    wait.firstChild = static_cast<uint32>(waitChildren_.Size());
    const uint32 index = static_cast<uint32>(waits_.Size());
    for (const uint64 child : children) {
        const uint64 childIndex = child & VmWaitIndexMask;
        if ((child & VmWaitTag) == 0 || childIndex >= waits_.Size() ||
            waits_[childIndex].parent != NoBytecodeIndex) {
            static_cast<void>(waitChildren_.TryResize(wait.firstChild));
            return 0; // 대기가 아니거나 이미 다른 묶음의 인자(언어가 막는다 — 안전장치)
        }

        if (!waitChildren_.TryPushBack(static_cast<uint32>(childIndex))) {
            error = ErrorCode::OutOfMemory;
            static_cast<void>(waitChildren_.TryResize(wait.firstChild));
            return 0;
        }
    }

    wait.childCount = static_cast<uint32>(children.Size());
    if (!waits_.TryPushBack(wait)) {
        error = ErrorCode::OutOfMemory;
        static_cast<void>(waitChildren_.TryResize(wait.firstChild));
        return 0;
    }

    for (uint32 at = wait.firstChild; at < wait.firstChild + wait.childCount; ++at) {
        waits_[waitChildren_[at]].parent = index;
    }

    return VmWaitTag | index;
}

VmFaultKind VmSession::Suspend(uint64 wait, uint32 resultRegister, uint32 resultBank,
                               uint32 resultSlots, uint32& suspension, ErrorCode& error) noexcept {
    const uint64 index = wait & VmWaitIndexMask;
    if (index >= waits_.Size() || waits_[index].suspension != NoBytecodeIndex ||
        waits_[index].parent != NoBytecodeIndex || waits_[index].cancelled) {
        return VmFaultKind::ContractViolation; // 같은 대기를 두 번 기다리거나 묶음 인자를 기다림
    }

    // 빈 자리를 다시 쓴다.
    uint32 number = static_cast<uint32>(suspended_.Size());
    for (uint32 at = 0; at < suspended_.Size(); ++at) {
        if (!suspended_[at].live) {
            number = at;
            break;
        }
    }

    if (number == suspended_.Size() && !suspended_.TryPushBack(Suspended(*allocator_))) {
        error = ErrorCode::OutOfMemory;
        return VmFaultKind::None;
    }

    Suspended& entry = suspended_[number];
    entry.frames.Clear();
    entry.scalars.Clear();
    entry.refs.Clear();
    entry.buffers.Clear();
    if (!entry.frames.TryAppend(execution_.frames_.View()) ||
        !entry.scalars.TryAppend(execution_.scalars_.View()) ||
        !entry.refs.TryAppend(execution_.refs_.View()) ||
        !entry.buffers.TryAppend(execution_.buffers_.View())) {
        error = ErrorCode::OutOfMemory;
        return VmFaultKind::None;
    }

    // Buf 레지스터의 참조는 옮긴다(실행 문맥은 비운다).
    for (VmBuffer*& buffer : execution_.buffers_) {
        buffer = nullptr;
    }

    execution_.frames_.Clear();
    execution_.scalars_.Clear();
    execution_.refs_.Clear();
    execution_.buffers_.Clear();
    entry.resultRegister = resultRegister;
    entry.resultBank = resultBank;
    entry.resultSlots = resultSlots;
    entry.wait = wait;
    entry.instance = currentInstance_;
    entry.publisher = currentPublisher_;
    entry.handlerModule = currentHandlerModule_;
    entry.handler = currentHandler_;
    entry.order = waits_[index].order;
    entry.live = true;
    waits_[index].suspension = number;
    suspension = number;
    return VmFaultKind::None;
}

void VmSession::ReleaseSuspended(Suspended& entry) noexcept {
    for (VmBuffer*& buffer : entry.buffers) {
        execution_.Release(buffer);
        buffer = nullptr;
    }

    entry.frames.Empty();
    entry.scalars.Empty();
    entry.refs.Empty();
    entry.buffers.Empty();
    entry.live = false;
}

void VmSession::CancelExecutions(uint32 instance) noexcept {
    // instance의 정지 실행을 취소한다: frame의 Buf를 놓고 대기를 끝낸다(SL 6.1절 — 취소는 await
    // 지점에서만, 유지 자원 해제는 LC6d).
    for (uint32 at = 0; at < suspended_.Size(); ++at) {
        if (suspended_[at].live && suspended_[at].instance == instance) {
            CancelSuspended(at);
        }
    }
}

void VmSession::CancelSuspended(uint32 suspended) noexcept {
    Suspended& entry = suspended_[suspended];
    const uint64 index = entry.wait & VmWaitIndexMask;
    if (index < waits_.Size()) {
        waits_[index].suspension = NoBytecodeIndex;
        waits_[index].cancelled = true;
    }

    ReleaseSuspended(entry);
}

uint32 VmSession::FindBusy(uint32 instance, const VmLinkedModule* module, uint32 handler,
                           uint64 publisher) const noexcept {
    for (uint32 at = 0; at < suspended_.Size(); ++at) {
        const Suspended& entry = suspended_[at];
        if (entry.live && entry.instance == instance && entry.handlerModule == module &&
            entry.handler == handler && entry.publisher == publisher) {
            return at;
        }
    }

    return NoBytecodeIndex;
}

void VmSession::CompleteWait(uint32 index) noexcept {
    // done을 위로 전파한다: All은 자식이 모두, Race는 첫 자식이 끝나면 끝나고 Race는 나머지를
    // 취소한다(같은 처리 시점의 여럿은 인자 순 — 호출 순서가 그 순서다).
    Wait& wait = waits_[index];
    if (wait.done || wait.cancelled) {
        return;
    }

    wait.done = true;
    const uint32 parent = wait.parent;
    if (parent == NoBytecodeIndex) {
        return;
    }

    Wait& group = waits_[parent];
    if (group.done || group.cancelled) {
        return;
    }

    if (group.kind == WaitKind::Race) {
        for (uint32 at = group.firstChild; at < group.firstChild + group.childCount; ++at) {
            if (!waits_[waitChildren_[at]].done) {
                waits_[waitChildren_[at]].cancelled = true;
            }
        }

        CompleteWait(parent);
        return;
    }

    bool all = true;
    for (uint32 at = group.firstChild; at < group.firstChild + group.childCount; ++at) {
        all = all && waits_[waitChildren_[at]].done;
    }

    if (all) {
        CompleteWait(parent);
    }
}

Result<void> VmSession::ResumeDone() noexcept {
    // 끝난 최상위 대기의 정지 실행을 (등록 프레임, instance, 등록 순번) 순으로 재개한다(SL 6.1절).
    for (;;) {
        uint32 chosen = NoBytecodeIndex;
        for (uint32 at = 0; at < suspended_.Size(); ++at) {
            const Suspended& entry = suspended_[at];
            if (!entry.live) {
                continue;
            }

            const Wait& wait = waits_[entry.wait & VmWaitIndexMask];
            if (!wait.done) {
                continue;
            }

            if (chosen == NoBytecodeIndex) {
                chosen = at;
                continue;
            }

            const Suspended& best = suspended_[chosen];
            const Wait& bestWait = waits_[best.wait & VmWaitIndexMask];
            const bool earlier = wait.frame != bestWait.frame      ? wait.frame < bestWait.frame
                                 : entry.instance != best.instance ? entry.instance < best.instance
                                                                   : entry.order < best.order;
            chosen = earlier ? at : chosen;
        }

        if (chosen == NoBytecodeIndex) {
            return Result<void>::Success();
        }

        // 되돌린다: frame·레지스터를 실행 문맥으로(Buf 참조는 옮긴다). 결과는 없다(기본 대기는 값이
        // 없다 — 0·null로 둔다).
        Suspended& entry = suspended_[chosen];
        // 앞 실행(반환한 조각)이 남긴 frame·레지스터를 비운다(덧붙이면 frame base가 어긋난다).
        execution_.Unwind();
        execution_.scalars_.Clear();
        execution_.refs_.Clear();
        execution_.buffers_.Clear();
        if (!execution_.frames_.TryAppend(entry.frames.View()) ||
            !execution_.scalars_.TryAppend(entry.scalars.View()) ||
            !execution_.refs_.TryAppend(entry.refs.View()) ||
            !execution_.buffers_.TryAppend(entry.buffers.View())) {
            execution_.frames_.Clear();
            execution_.scalars_.Clear();
            execution_.refs_.Clear();
            execution_.buffers_.Clear();
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        for (VmBuffer*& buffer : entry.buffers) {
            buffer = nullptr;
        }

        const VmExecution::Frame& top = entry.frames[entry.frames.Size() - 1];
        for (uint32 slot = 0; slot < entry.resultSlots; ++slot) {
            const usize at = usize(top.base[entry.resultBank]) + entry.resultRegister + slot;
            if (entry.resultBank == 0) {
                execution_.scalars_[at] = 0;
            } else if (entry.resultBank == 1) {
                execution_.refs_[at] = 0;
            } else {
                VmBuffer* previous = execution_.buffers_[at];
                execution_.buffers_[at] = nullptr;
                execution_.Release(previous);
            }
        }

        const VmExecution::Frame bottom = entry.frames[0];
        const uint32 instance = entry.instance;
        currentPublisher_ = entry.publisher;
        currentHandlerModule_ = entry.handlerModule;
        currentHandler_ = entry.handler;
        waits_[entry.wait & VmWaitIndexMask].suspension = NoBytecodeIndex;
        entry.frames.Empty();
        entry.scalars.Empty();
        entry.refs.Empty();
        entry.buffers.Empty();
        entry.live = false;
        currentInstance_ = instance;
        auto continued = execution_.Continue(VmLimits());
        currentInstance_ = NoBytecodeIndex;
        currentPublisher_ = 0;
        currentHandlerModule_ = nullptr;
        currentHandler_ = NoBytecodeIndex;
        if (!continued) {
            return Result<void>::Failure(continued.Error());
        }

        Delivery delivery;
        delivery.subscriber =
            instance < instances_.Size() ? instances_[instance].handle : uint64(0);
        delivery.module = bottom.link;
        delivery.function = bottom.function;
        delivery.result = continued.Value();
        delivery.resumed = true;
        execution_.ReleaseReturned();
        if (!deliveries_.TryPushBack(delivery)) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        if (auto drained = Drain(); !drained) {
            return drained;
        }
    }
}

Result<void> VmSession::Advance(double gameDelta, double uiDelta) noexcept {
    // 프레임 +1, 시계 진행, 가변 재개 단계: 기한이 된 Delay와 앞 프레임의 NextTick(SL 5.4·6.1절).
    BeginEntry();
    execution_.keepLogs_ = true;
    ++frame_;
    gameTime_ += gameDelta;
    uiTime_ += uiDelta;
    for (uint32 index = 0; index < waits_.Size(); ++index) {
        const Wait& wait = waits_[index];
        if (wait.done || wait.cancelled) {
            continue;
        }

        const bool due = (wait.kind == WaitKind::NextTick && frame_ > wait.frame) ||
                         (wait.kind == WaitKind::Delay && frame_ > wait.frame &&
                          (wait.clock == VmGameClock ? gameTime_ : uiTime_) >= wait.deadline);
        if (due) {
            CompleteWait(index);
        }
    }

    auto resumed = ResumeDone();
    execution_.keepLogs_ = false;
    return resumed;
}

Result<void> VmSession::FixedStep() noexcept {
    BeginEntry();
    execution_.keepLogs_ = true;
    ++fixedStep_;
    for (uint32 index = 0; index < waits_.Size(); ++index) {
        const Wait& wait = waits_[index];
        if (!wait.done && !wait.cancelled && wait.kind == WaitKind::NextFixedTick &&
            fixedStep_ > wait.fixedStep) {
            CompleteWait(index);
        }
    }

    auto resumed = ResumeDone();
    execution_.keepLogs_ = false;
    return resumed;
}

} // namespace rocks::sparks
