#include <Core/Math/MathUtility.h>
#include <SparksVm/VmSession.h>

// async 실행의 구현이다(Docs/Design/SparksRuntime.ko.md 2.4·2.5절, LC6c): AWAIT 정지(frame stack
// 전체 — R9, 또는 CALLW 경계부터 — R14), 대기 표(Delay·NextTick·NextFixedTick·All·Race·이벤트·하위
// 실행 — 번호를 돌려받아 다시 쓴다), 시계 진행과 재개 순서, 취소와 실패 전파.

namespace rocks::sparks {

namespace {

constexpr uint8 VmGameClock = 0;
// handle의 번호(아래 32비트)와 세대(그 위 31비트 — 표지 비트 아래)다.
constexpr uint32 VmWaitIndexBits = 32;
constexpr uint64 VmWaitIndexMask = (uint64(1) << VmWaitIndexBits) - 1;
constexpr uint32 VmWaitGenerationMask = (uint32(1) << 31) - 1;

uint64 VmWaitHandle(uint32 index, uint32 generation) noexcept {
    return VmWaitTag | (uint64(generation & VmWaitGenerationMask) << VmWaitIndexBits) | index;
}

} // namespace

uint32 VmSession::SuspendedCount() const noexcept {
    uint32 count = 0;
    for (const Suspended& entry : suspended_) {
        count += entry.live ? 1 : 0;
    }

    return count;
}

uint32 VmSession::WaitCount() const noexcept {
    uint32 count = 0;
    for (const Wait& wait : waits_) {
        count += wait.used ? 1 : 0;
    }

    return count;
}

bool VmSession::DecodeWait(uint64 handle, uint32& index) const noexcept {
    if ((handle & VmWaitTag) == 0) {
        return false;
    }

    const uint64 at = handle & VmWaitIndexMask;
    const uint32 generation = static_cast<uint32>((handle & ~VmWaitTag) >> VmWaitIndexBits);
    if (at >= waits_.Size() || !waits_[at].used ||
        (waits_[at].generation & VmWaitGenerationMask) != generation) {
        return false;
    }

    index = static_cast<uint32>(at);
    return true;
}

uint32 VmSession::WaitRoot(uint32 index) const noexcept {
    // 부모는 자식보다 늦게 만들어지므로 사슬에 순환이 없다.
    while (waits_[index].parent != NoBytecodeIndex) {
        index = waits_[index].parent;
    }

    return index;
}

void VmSession::FreeWaitTree(uint32 root) noexcept {
    // 첫 자식부터 잎을 하나씩 돌려받는다(부모 포인터로 오르내린다 — 재귀·추가 저장소 없음). 아직
    // 정지해 있는 하위 실행은 취소한다(부모 묶음을 돌려받는 길 — 부모에 알리지 않는다).
    uint32 node = root;
    for (;;) {
        while (waits_[node].firstChild != NoBytecodeIndex) {
            node = waits_[node].firstChild;
        }

        if (waits_[node].kind == WaitKind::Child && waits_[node].child != NoBytecodeIndex) {
            const uint32 child = waits_[node].child;
            waits_[node].child = NoBytecodeIndex;
            if (child < suspended_.Size() && suspended_[child].live) {
                CancelSuspended(child, false);
            }
        }

        const uint32 parent = waits_[node].parent;
        const uint32 next = waits_[node].nextSibling;
        Wait& wait = waits_[node];
        const uint32 generation = wait.generation + 1;
        wait = Wait();
        wait.generation = generation;
        // 목록에 넣지 못하면 번호를 잃는다(할당 실패 — 표가 조금 자랄 뿐 안전하다).
        static_cast<void>(freeWaits_.TryPushBack(node));
        if (node == root) {
            if (parent != NoBytecodeIndex && waits_[parent].firstChild == node) {
                waits_[parent].firstChild = next;
            }

            return;
        }

        waits_[parent].firstChild = next;
        node = parent;
    }
}

void VmSession::CancelWaitTree(uint32 root) noexcept {
    // 끝나지 않은 대기를 취소 표시한다(하위 실행은 취소한다 — Race의 진 인자).
    uint32 node = root;
    for (;;) {
        Wait& wait = waits_[node];
        if (!wait.done) {
            wait.cancelled = true;
            if (wait.kind == WaitKind::Child && wait.child != NoBytecodeIndex) {
                const uint32 child = wait.child;
                wait.child = NoBytecodeIndex;
                if (child < suspended_.Size() && suspended_[child].live) {
                    CancelSuspended(child, false);
                }
            }
        }

        if (waits_[node].firstChild != NoBytecodeIndex) {
            node = waits_[node].firstChild;
            continue;
        }

        while (node != root && waits_[node].nextSibling == NoBytecodeIndex) {
            node = waits_[node].parent;
        }

        if (node == root) {
            return;
        }

        node = waits_[node].nextSibling;
    }
}

uint64 VmSession::NewWait(WaitKind kind, uint8 clock, double seconds, Span<const uint64> children,
                          ErrorCode& error) noexcept {
    uint32 index = NoBytecodeIndex;
    if (!freeWaits_.IsEmpty()) {
        index = freeWaits_[freeWaits_.Size() - 1];
        static_cast<void>(freeWaits_.TryResize(freeWaits_.Size() - 1));
    } else {
        Wait fresh;
        if (!waits_.TryPushBack(fresh)) {
            error = ErrorCode::OutOfMemory;
            return 0;
        }

        index = static_cast<uint32>(waits_.Size() - 1);
    }

    // 조각 끝(SettleWaits)이 기다리지 않은 대기를 돌려받도록 기록한다.
    if (!fragmentWaits_.TryPushBack(VmWaitHandle(index, waits_[index].generation))) {
        static_cast<void>(freeWaits_.TryPushBack(index));
        error = ErrorCode::OutOfMemory;
        return 0;
    }

    Wait& wait = waits_[index];
    wait.used = true;
    wait.kind = kind;
    wait.clock = clock;
    wait.frame = frame_;
    wait.fixedStep = fixedStep_;
    wait.instance = currentInstance_;
    wait.order = nextWaitOrder_++;
    // 기한: 등록한 프레임의 그 시계 값 + seconds(0 이하는 다음 프레임 — SL 6.1절).
    wait.deadline = (clock == VmGameClock ? gameTime_ : uiTime_) + (seconds > 0.0 ? seconds : 0.0);
    // 자식: 살아 있고, 취소되지 않았고, 기다린 적 없고, 다른 묶음의 인자가 아니어야 한다(중복
    // 인자는 첫 번째가 부모를 얻으므로 두 번째에서 걸린다 — 언어가 막는다, 안전장치). 이미 끝난
    // 자식(동기로 끝난 하위 실행)은 받는다 — 묶음은 인자를 모두 시작한 뒤 판정한다(SL 6.1절).
    uint32 last = NoBytecodeIndex;
    bool nullChild = false;
    for (const uint64 child : children) {
        // null 인자는 바로 끝난 인자다(`a?.F()`의 a가 null — SL 6.1절, LC6c2 리뷰 B5).
        if (child == 0) {
            nullChild = true;
            continue;
        }

        uint32 childIndex = NoBytecodeIndex;
        const bool valid = DecodeWait(child, childIndex) && childIndex != index &&
                           waits_[childIndex].parent == NoBytecodeIndex &&
                           !waits_[childIndex].cancelled && !waits_[childIndex].awaited;
        if (!valid) {
            // 이어 붙인 자식을 풀고 이 대기는 조각 끝(SettleWaits)이 돌려받는다.
            for (uint32 at = waits_[index].firstChild; at != NoBytecodeIndex;) {
                const uint32 next = waits_[at].nextSibling;
                waits_[at].parent = NoBytecodeIndex;
                waits_[at].nextSibling = NoBytecodeIndex;
                at = next;
            }

            waits_[index].firstChild = NoBytecodeIndex;
            return 0;
        }

        waits_[childIndex].parent = index;
        if (last == NoBytecodeIndex) {
            waits_[index].firstChild = childIndex;
        } else {
            waits_[last].nextSibling = childIndex;
        }

        last = childIndex;
    }

    // 이미 끝난 자식으로 판정한다: All은 모두, Race는 첫 끝난 인자(나머지 취소).
    if (kind == WaitKind::All || kind == WaitKind::Race) {
        bool all = true;
        bool any = nullChild;
        for (uint32 at = waits_[index].firstChild; at != NoBytecodeIndex;
             at = waits_[at].nextSibling) {
            all = all && waits_[at].done;
            any = any || waits_[at].done;
        }

        if (kind == WaitKind::All ? all : any) {
            waits_[index].done = true;
            for (uint32 at = waits_[index].firstChild;
                 kind == WaitKind::Race && at != NoBytecodeIndex; at = waits_[at].nextSibling) {
                if (!waits_[at].done) {
                    CancelWaitTree(at);
                }
            }
        }
    }

    return VmWaitHandle(index, waits_[index].generation);
}

uint64 VmSession::NewEventWait(uint64 publisher, const VmEventKey& key, ErrorCode& error) noexcept {
    const uint64 handle = NewWait(WaitKind::Event, 0, 0.0, Span<const uint64>(), error);
    uint32 index = NoBytecodeIndex;
    if (handle == 0 || !DecodeWait(handle, index)) {
        return 0;
    }

    Wait& wait = waits_[index];
    wait.publisher = publisher;
    wait.key = key;
    wait.connectionOrder = nextOrder_++;
    wait.afterSequence = itemSequence_;
    return handle;
}

uint64 VmSession::NewChildDone(ErrorCode& error) noexcept {
    const uint64 handle = NewWait(WaitKind::Child, 0, 0.0, Span<const uint64>(), error);
    uint32 index = NoBytecodeIndex;
    if (handle != 0 && DecodeWait(handle, index)) {
        waits_[index].done = true;
    }

    return handle;
}

void VmSession::SettleWaits(const Result<VmCallResult>& called) noexcept {
    uint32 awaited = NoBytecodeIndex;
    if (called && called.Value().outcome == VmOutcome::Suspended) {
        static_cast<void>(DecodeWait(called.Value().wait, awaited));
    }

    for (usize at = 0; at < fragmentWaits_.Size(); ++at) {
        uint32 index = NoBytecodeIndex;
        if (!DecodeWait(fragmentWaits_[at], index)) {
            continue; // 이미 돌려받았다(같은 묶음의 다른 자식과 함께)
        }

        // 하위 정지 실행이 기다리는 묶음은 그 실행의 것이다(조각이 정지한 실행과 같다).
        const uint32 root = WaitRoot(index);
        const uint32 owner = waits_[root].suspension;
        const bool held = root == awaited || (owner != NoBytecodeIndex &&
                                              owner < suspended_.Size() && suspended_[owner].live);
        if (!held) {
            FreeWaitTree(root);
        }
    }

    fragmentWaits_.Clear();
}

VmFaultKind VmSession::Suspend(uint64 wait, uint32 resultRegister, uint32 resultBank,
                               uint32 resultSlots, uint32& suspension, ErrorCode& error) noexcept {
    // 한 대기는 한 번만 기다린다(동시·순차 모두 — 이미 끝난 대기는 다음 재개 단계에 재개한다).
    uint32 index = NoBytecodeIndex;
    if (!DecodeWait(wait, index) || waits_[index].awaited ||
        waits_[index].parent != NoBytecodeIndex || waits_[index].cancelled) {
        return VmFaultKind::ContractViolation;
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
    // 복사에 실패하면 항목을 비운다(Buf 사본은 참조를 갖지 않는다 — 실행 문맥이 아직 쥔다).
    auto failed = [&]() noexcept -> VmFaultKind {
        entry.frames.Clear();
        entry.generations.Clear();
        entry.held.Clear();
        entry.scalars.Clear();
        entry.refs.Clear();
        entry.buffers.Clear();
        error = ErrorCode::OutOfMemory;
        return VmFaultKind::None;
    };
    entry.frames.Clear();
    entry.generations.Clear();
    entry.held.Clear();
    entry.scalars.Clear();
    entry.refs.Clear();
    entry.buffers.Clear();
    if (!entry.frames.TryAppend(execution_.frames_.View()) ||
        !entry.scalars.TryAppend(execution_.scalars_.View()) ||
        !entry.refs.TryAppend(execution_.refs_.View()) ||
        !entry.buffers.TryAppend(execution_.buffers_.View())) {
        return failed();
    }

    for (const VmExecution::Frame& frame : execution_.frames_) {
        const BytecodeFunction& function = frame.link->Module().functions[frame.function];
        if (!entry.generations.TryPushBack(frame.link->generation_) ||
            !entry.held.TryPushBack((uint32(function.heldFirst) << 16) | function.heldCount)) {
            return failed();
        }
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
    entry.serial = currentInstance_ < instances_.Size() ? instances_[currentInstance_].serial : 0;
    entry.publisher = currentPublisher_;
    entry.handlerModule = currentHandlerModule_;
    entry.handler = currentHandler_;
    entry.order = waits_[index].order;
    entry.random = execution_.random_;
    // 재개 중인 하위 실행이 다시 정지하면 부모 묶음의 하위 대기를 이 자리로 잇는다.
    entry.childWait = currentChildWait_;
    entry.depthOffset = execution_.depthBase_;
    uint32 childIndex = NoBytecodeIndex;
    if (currentChildWait_ != 0 && DecodeWait(currentChildWait_, childIndex)) {
        waits_[childIndex].child = number;
    }

    entry.live = true;
    waits_[index].awaited = true;
    waits_[index].suspension = number;
    suspension = number;
    return VmFaultKind::None;
}

VmFaultKind VmSession::SuspendChild(usize boundary, uint64 wait, uint32 resultRegister,
                                    uint32 resultBank, uint32 resultSlots, uint64& child,
                                    ErrorCode& error) noexcept {
    uint32 index = NoBytecodeIndex;
    if (!DecodeWait(wait, index) || waits_[index].awaited ||
        waits_[index].parent != NoBytecodeIndex || waits_[index].cancelled) {
        return VmFaultKind::ContractViolation;
    }

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
    auto failed = [&]() noexcept -> VmFaultKind {
        entry.frames.Clear();
        entry.generations.Clear();
        entry.held.Clear();
        entry.scalars.Clear();
        entry.refs.Clear();
        entry.buffers.Clear();
        error = ErrorCode::OutOfMemory;
        return VmFaultKind::None;
    };
    // 경계 frame부터 맨 위까지와 그 레지스터 구간(경계 frame의 base부터 stack 끝)을 base 0으로
    // 옮긴다.
    const VmExecution::Frame first = execution_.frames_[boundary];
    entry.frames.Clear();
    entry.generations.Clear();
    entry.held.Clear();
    entry.scalars.Clear();
    entry.refs.Clear();
    entry.buffers.Clear();
    const usize count = execution_.frames_.Size() - boundary;
    if (!entry.frames.TryAppend(
            Span<const VmExecution::Frame>(execution_.frames_.Data() + boundary, count)) ||
        !entry.scalars.TryAppend(Span<const uint64>(execution_.scalars_.Data() + first.base[0],
                                                    execution_.scalars_.Size() - first.base[0])) ||
        !entry.refs.TryAppend(Span<const uint64>(execution_.refs_.Data() + first.base[1],
                                                 execution_.refs_.Size() - first.base[1])) ||
        !entry.buffers.TryAppend(
            Span<VmBuffer* const>(execution_.buffers_.Data() + first.base[2],
                                  execution_.buffers_.Size() - first.base[2]))) {
        return failed();
    }

    for (VmExecution::Frame& frame : entry.frames) {
        for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
            frame.base[bank] -= first.base[bank];
        }

        const BytecodeFunction& function = frame.link->Module().functions[frame.function];
        if (!entry.generations.TryPushBack(frame.link->generation_) ||
            !entry.held.TryPushBack((uint32(function.heldFirst) << 16) | function.heldCount)) {
            return failed();
        }
    }

    entry.frames[0].child = false; // 이제 그 자체가 실행이다(경계는 하위 대기가 맡는다)
    ErrorCode waitError = ErrorCode::None;
    const uint64 handle = NewWait(WaitKind::Child, 0, 0.0, Span<const uint64>(), waitError);
    uint32 childIndex = NoBytecodeIndex;
    if (handle == 0 || !DecodeWait(handle, childIndex)) {
        return failed();
    }

    // Buf 참조는 옮긴다(실행 문맥 쪽 자리는 null).
    for (usize at = first.base[2]; at < execution_.buffers_.Size(); ++at) {
        execution_.buffers_[at] = nullptr;
    }

    waits_[childIndex].child = number;
    entry.resultRegister = resultRegister;
    entry.resultBank = resultBank;
    entry.resultSlots = resultSlots;
    entry.wait = wait;
    entry.instance = currentInstance_;
    entry.serial = currentInstance_ < instances_.Size() ? instances_[currentInstance_].serial : 0;
    entry.publisher = 0; // 하위 실행은 handler 실행이 아니다(재진입 판정 밖)
    entry.handlerModule = nullptr;
    entry.handler = NoBytecodeIndex;
    entry.order = waits_[index].order;
    entry.random = execution_.random_;
    entry.childWait = handle;
    entry.depthOffset = execution_.depthBase_ + static_cast<uint32>(boundary);
    entry.live = true;
    waits_[index].awaited = true;
    waits_[index].suspension = number;
    child = handle;
    return VmFaultKind::None;
}

void VmSession::ReleaseSuspended(Suspended& entry) noexcept {
    // 취소: 맨 위 frame부터 held 구간을 뒤에서부터 멈춘다(정지 때 보관한 구간 — 재링크와 무관).
    for (usize frame = entry.frames.Size(); frame > 0 && entry.live; --frame) {
        const uint32 range = frame - 1 < entry.held.Size() ? entry.held[frame - 1] : 0;
        const usize base = entry.frames[frame - 1].base[static_cast<uint32>(BytecodeBank::Ref)];
        for (uint32 at = range & 0xFFFF; at > 0; --at) {
            const usize slot = base + (range >> 16) + at - 1;
            if (slot < entry.refs.Size() && entry.refs[slot] != 0) {
                if (host_ != nullptr) {
                    host_->ReleaseHeld(entry.refs[slot]);
                }

                entry.refs[slot] = 0;
            }
        }
    }

    for (VmBuffer*& buffer : entry.buffers) {
        execution_.Release(buffer);
        buffer = nullptr;
    }

    entry.frames.Empty();
    entry.generations.Empty();
    entry.held.Empty();
    entry.scalars.Empty();
    entry.refs.Empty();
    entry.buffers.Empty();
    entry.childWait = 0;
    entry.live = false;
}

void VmSession::CancelExecutions(uint32 instance, bool frames) noexcept {
    // instance의 정지 실행을 취소한다: frame의 Buf를 놓고 대기를 돌려받는다(SL 6.1절 — 취소는 await
    // 지점에서만, 유지 자원 해제는 LC6d). frames면 그 instance를 this(frame의 Ref 0번)로 쓰는
    // frame이 있는 실행도 취소한다(다른 instance가 부른 이 instance의 async 메서드 안의 정지).
    const uint64 handle = instance < instances_.Size() ? instances_[instance].handle : 0;
    for (uint32 at = 0; at < suspended_.Size(); ++at) {
        const Suspended& entry = suspended_[at];
        if (!entry.live) {
            continue;
        }

        bool owned = entry.instance == instance;
        for (usize frame = 0; frames && !owned && frame < entry.frames.Size(); ++frame) {
            const usize base = entry.frames[frame].base[static_cast<uint32>(BytecodeBank::Ref)];
            owned = handle != 0 && base < entry.refs.Size() && entry.refs[base] == handle;
        }

        if (owned) {
            CancelSuspended(at);
        }
    }
}

void VmSession::CancelSuspended(uint32 suspended, bool propagate) noexcept {
    const uint64 childWait = suspended_[suspended].childWait;
    uint32 index = NoBytecodeIndex;
    if (DecodeWait(suspended_[suspended].wait, index)) {
        FreeWaitTree(WaitRoot(index));
    }

    ReleaseSuspended(suspended_[suspended]);
    // 하위 실행이 완료되지 않은 채 끝났다: 부모 묶음에 알린다(Race는 남은 인자, All은 전체 —
    // SparksRuntime 2.5절).
    uint32 childIndex = NoBytecodeIndex;
    if (childWait != 0 && DecodeWait(childWait, childIndex)) {
        waits_[childIndex].child = NoBytecodeIndex;
        if (propagate) {
            FailWait(childIndex);
        }
    }
}

void VmSession::FailWait(uint32 node) noexcept {
    for (;;) {
        const Wait& wait = waits_[node];
        if (!wait.used || wait.done) {
            return;
        }

        const uint32 parent = wait.parent;
        if (parent == NoBytecodeIndex) {
            // 최상위: 기다리는 실행을 취소한다(하위 실행이면 그 부모 묶음으로 이어진다).
            const uint32 owner = wait.suspension;
            if (owner != NoBytecodeIndex && owner < suspended_.Size() && suspended_[owner].live) {
                CancelSuspended(owner, true);
            }

            return;
        }

        if (waits_[parent].done || waits_[parent].cancelled) {
            return;
        }

        if (waits_[parent].kind == WaitKind::Race) {
            CancelWaitTree(node);
            bool alive = false;
            for (uint32 at = waits_[parent].firstChild; at != NoBytecodeIndex;
                 at = waits_[at].nextSibling) {
                alive = alive || !waits_[at].cancelled;
            }

            if (alive) {
                return; // 남은 인자를 기다린다
            }
        }

        node = parent; // All(또는 인자가 모두 끝난 Race)은 묶음 전체가 끝난다
    }
}

void VmSession::EndEventWaits(uint64 publisher) noexcept {
    for (uint32 index = 0; index < waits_.Size(); ++index) {
        const Wait& wait = waits_[index];
        if (wait.used && wait.kind == WaitKind::Event && wait.publisher == publisher &&
            !wait.done && !wait.cancelled) {
            FailWait(index);
        }
    }
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
    // done을 위로 전파한다: All은 자식이 모두, Race는 첫 자식이 끝나면 끝나고 Race는 나머지(와 그
    // 하위 묶음)를 취소한다(같은 처리 시점의 여럿은 인자 순 — 호출 순서가 그 순서다).
    for (;;) {
        Wait& wait = waits_[index];
        if (wait.done || wait.cancelled) {
            return;
        }

        wait.done = true;
        const uint32 parent = wait.parent;
        if (parent == NoBytecodeIndex || waits_[parent].done || waits_[parent].cancelled) {
            return;
        }

        if (waits_[parent].kind == WaitKind::Race) {
            for (uint32 at = waits_[parent].firstChild; at != NoBytecodeIndex;
                 at = waits_[at].nextSibling) {
                if (!waits_[at].done) {
                    CancelWaitTree(at);
                }
            }

            index = parent;
            continue;
        }

        bool all = true;
        for (uint32 at = waits_[parent].firstChild; at != NoBytecodeIndex;
             at = waits_[at].nextSibling) {
            all = all && waits_[at].done;
        }

        if (!all) {
            return;
        }

        index = parent;
    }
}

Result<void> VmSession::ResumeSuspended(uint32 chosen, uint64 wait, uint32 depth, uint64 root,
                                        bool inDrain) noexcept {
    Suspended& entry = suspended_[chosen];
    if (!entry.live || entry.wait != wait) {
        return Result<void>::Success(); // 앞서 재개한 조각이 취소했거나(@restart) 다시 정지했다
    }

    // 파괴 적용된 instance의 실행은 재개하지 않는다(프레임 끝 정리가 취소한다 — LC6d2 리뷰 B4).
    if (entry.instance < instances_.Size() && instances_[entry.instance].pendingDestroy) {
        return Result<void>::Success();
    }

    // 정지 뒤 frame의 모듈을 다시 링크했으면 이어 갈 수 없다: 취소한다(SL 12.4절 — 호환되지
    // 않는 reload).
    bool current = true;
    for (usize frame = 0; frame < entry.frames.Size(); ++frame) {
        const VmLinkedModule* link = entry.frames[frame].link;
        current = current && link->Linked() && link->ImportsLinked() &&
                  link->generation_ == entry.generations[frame];
    }

    if (!current) {
        CancelSuspended(chosen);
        return Result<void>::Success();
    }

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

    // 결과는 없다(기본 대기는 값이 없다 — 0·null로 둔다).
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
    const uint64 childWait = entry.childWait;
    uint32 waitIndex = NoBytecodeIndex;
    if (DecodeWait(entry.wait, waitIndex)) {
        FreeWaitTree(WaitRoot(waitIndex)); // 기다린 대기는 끝났다
    }

    execution_.random_ = entry.random;
    currentPublisher_ = entry.publisher;
    currentHandlerModule_ = entry.handlerModule;
    currentHandler_ = entry.handler;
    currentChildWait_ = childWait;
    entry.frames.Empty();
    entry.generations.Empty();
    entry.held.Empty();
    entry.scalars.Empty();
    entry.refs.Empty();
    entry.buffers.Empty();
    entry.childWait = 0;
    entry.live = false;
    uint32 childIndex = NoBytecodeIndex;
    if (childWait != 0 && DecodeWait(childWait, childIndex)) {
        waits_[childIndex].child = NoBytecodeIndex; // 실행 중(다시 정지하면 Suspend가 잇는다)
    }

    const uint32 depthOffset = entry.depthOffset;
    currentInstance_ = instance;
    currentDepth_ = depth;
    currentRoot_ = root;
    execution_.depthBase_ = depthOffset;
    auto continued = execution_.Continue(VmLimits());
    execution_.depthBase_ = 0;
    currentInstance_ = NoBytecodeIndex;
    currentPublisher_ = 0;
    currentHandlerModule_ = nullptr;
    currentHandler_ = NoBytecodeIndex;
    currentChildWait_ = 0;
    SettleWaits(continued);
    if (!continued) {
        // 하위 실행이었으면 끝나지 않은 채 끝났다: 부모 묶음에 알린다(LC6c2 리뷰 S2).
        uint32 lost = NoBytecodeIndex;
        if (childWait != 0 && DecodeWait(childWait, lost)) {
            FailWait(lost);
        }

        return Result<void>::Failure(continued.Error());
    }

    Delivery delivery;
    delivery.subscriber = instance < instances_.Size() ? instances_[instance].handle : uint64(0);
    delivery.module = bottom.link;
    delivery.function = bottom.function;
    delivery.result = continued.Value();
    delivery.resumed = true;
    execution_.ReleaseReturned();
    ErrorCode failure = ErrorCode::None;
    if (!deliveries_.TryPushBack(delivery)) {
        failure = ErrorCode::OutOfMemory;
    }

    // 조각 끝 정리 → 하위 실행의 끝을 부모 묶음에(곧바로 — 하위 실행이 낸 발행보다 먼저, LC6c2
    // 리뷰 B2) → 전달 순회 밖이면 큐를 비운다. 앞 단계가 할당에 실패해도 부모 묶음 처리는 한다.
    currentDepth_ = depth;
    currentRoot_ = root;
    if (auto ended = EndFragment(); !ended && failure == ErrorCode::None) {
        failure = ended.Error();
    }

    currentDepth_ = 0;
    currentRoot_ = 0;
    if (childWait == 0 && continued.Value().outcome == VmOutcome::Faulted) {
        NoteFault(instance, continued.Value(), true); // 실행 트리의 뿌리 fault(하위는 부모가 센다)
    }

    if (childWait != 0 && continued.Value().outcome != VmOutcome::Suspended) {
        if (auto finished = FinishChild(childWait, continued.Value(), depth, root, inDrain);
            !finished && failure == ErrorCode::None) {
            failure = finished.Error();
        }
    }

    if (failure != ErrorCode::None) {
        return Result<void>::Failure(failure);
    }

    return inDrain ? Result<void>::Success() : Drain();
}

Result<void> VmSession::FinishChild(uint64 childWait, const VmCallResult& result, uint32 depth,
                                    uint64 root, bool inDrain) noexcept {
    uint32 node = NoBytecodeIndex;
    if (!DecodeWait(childWait, node)) {
        return Result<void>::Success(); // 부모 묶음이 이미 끝났다(Race 패배·취소)
    }

    waits_[node].child = NoBytecodeIndex;
    if (result.outcome == VmOutcome::Faulted) {
        // 하위 작업의 fault는 부모 실행의 fault다(R15): 부모를 그 fault로 끝내고(형제는 묶음과 함께
        // 취소) 부모가 하위 실행이면 위로 잇는다. 기록에 실패해도 취소는 끝까지 한다(LC6c2 리뷰
        // S2).
        ErrorCode failure = ErrorCode::None;
        for (;;) {
            const uint32 root = WaitRoot(node);
            const uint32 owner = waits_[root].suspension;
            if (owner == NoBytecodeIndex || owner >= suspended_.Size() || !suspended_[owner].live) {
                return failure == ErrorCode::None ? Result<void>::Success()
                                                  : Result<void>::Failure(failure);
            }

            const Suspended& parent = suspended_[owner];
            Delivery delivery;
            delivery.subscriber =
                parent.instance < instances_.Size() ? instances_[parent.instance].handle : 0;
            delivery.module = parent.frames[0].link;
            delivery.function = parent.frames[0].function;
            delivery.result = result;
            delivery.resumed = true;
            if (!deliveries_.TryPushBack(delivery)) {
                failure = ErrorCode::OutOfMemory;
            }

            const uint64 up = parent.childWait;
            const uint32 parentInstance = parent.instance;
            CancelSuspended(owner, false);
            if (up == 0) {
                NoteFault(parentInstance, result, true); // 실행 트리 하나는 한 번 센다
            }

            if (up == 0 || !DecodeWait(up, node)) {
                return failure == ErrorCode::None ? Result<void>::Success()
                                                  : Result<void>::Failure(failure);
            }

            waits_[node].child = NoBytecodeIndex;
        }
    }

    // 반환·ENDEXEC: 하위 대기가 완료됐다. 그것으로 부모 묶음이 끝났으면 부모를 곧바로 재개한다.
    CompleteWait(node);
    const uint32 top = WaitRoot(node);
    const uint32 owner = waits_[top].suspension;
    if (waits_[top].done && owner != NoBytecodeIndex && owner < suspended_.Size() &&
        suspended_[owner].live) {
        // 부모 재개는 그 하위 실행의 조각과 같은 연쇄 깊이·루트다(LC6c2 리뷰 B3).
        return ResumeSuspended(owner, suspended_[owner].wait, depth, root, inDrain);
    }

    return Result<void>::Success();
}

Result<void> VmSession::ResumeDone() noexcept {
    // 단계 시작 때 끝난 최상위 대기의 정지 실행을 (등록 프레임, instance 생성 번호, 등록 순번)
    // 순으로 재개한다(SL 6.1절). 재개된 조각이 이미 끝난 대기를 다시 기다리면 다음 단계에
    // 재개한다(같은 단계에서 다시 고르지 않는다 — 끝나지 않는 반복 방지).
    resumeOrder_.Clear();
    resumeWaits_.Clear();
    for (uint32 at = 0; at < suspended_.Size(); ++at) {
        uint32 index = NoBytecodeIndex;
        if (suspended_[at].live && DecodeWait(suspended_[at].wait, index) && waits_[index].done) {
            if (!resumeOrder_.TryPushBack(at)) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }
        }
    }

    auto earlier = [&](uint32 left, uint32 right) noexcept {
        const Suspended& a = suspended_[left];
        const Suspended& b = suspended_[right];
        uint32 aw = NoBytecodeIndex;
        uint32 bw = NoBytecodeIndex;
        static_cast<void>(DecodeWait(a.wait, aw));
        static_cast<void>(DecodeWait(b.wait, bw));
        return waits_[aw].frame != waits_[bw].frame ? waits_[aw].frame < waits_[bw].frame
               : a.serial != b.serial               ? a.serial < b.serial
                                                    : a.order < b.order;
    };
    for (usize at = 1; at < resumeOrder_.Size(); ++at) {
        const uint32 moving = resumeOrder_[at];
        usize to = at;
        while (to > 0 && earlier(moving, resumeOrder_[to - 1])) {
            resumeOrder_[to] = resumeOrder_[to - 1];
            --to;
        }

        resumeOrder_[to] = moving;
    }

    for (const uint32 slot : resumeOrder_) {
        if (!resumeWaits_.TryPushBack(suspended_[slot].wait)) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }
    }

    for (usize position = 0; position < resumeOrder_.Size(); ++position) {
        if (auto resumed =
                ResumeSuspended(resumeOrder_[position], resumeWaits_[position], 0, 0, false);
            !resumed) {
            return resumed;
        }
    }

    return Result<void>::Success();
}

Result<void> VmSession::Advance(double gameDelta, double uiDelta, bool paused) noexcept {
    if (closed_ || execution_.running_ || !math::IsFinite(gameDelta) || !math::IsFinite(uiDelta) ||
        gameDelta < 0.0 || uiDelta < 0.0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 프레임 +1, 시계 진행, 가변 재개 단계(새 스크립트 단계 — 이월 항목도 전달): 기한이 된 Delay와
    // 앞 프레임의 NextTick(SL 5.4·6.1절). 일시 정지면 Game 시계 대기와 NextTick은 둔다.
    BeginEntry();
    execution_.keepLogs_ = true;
    deliveredInPhase_ = 0;
    for (usize at = queueHead_; at < queue_.Size(); ++at) {
        queue_[at].carried = true;
    }

    // 이 단계의 fault는 새 활성 프레임에 센다(이월 전달 포함 — LC6d3 리뷰).
    activeFrame_ += paused ? 0 : 1;
    // 이월 항목을 먼저 전달하고(같은 단계의 전달이 파괴보다 먼저 — SD13, LC6c2 리뷰 S1) 엔진 객체
    // 발행자가 사라졌으면 그 이벤트 대기를 끝낸다.
    if (auto carried = Drain(); !carried) {
        execution_.keepLogs_ = false;
        return carried;
    }

    for (uint32 index = 0; host_ != nullptr && index < waits_.Size(); ++index) {
        const Wait& wait = waits_[index];
        if (wait.used && wait.kind == WaitKind::Event && !wait.done && !wait.cancelled &&
            !host_->IsAlive(wait.publisher)) {
            FailWait(index);
        }
    }

    ++frame_;
    gameTime_ += gameDelta;
    uiTime_ += uiDelta;
    for (uint32 index = 0; index < waits_.Size(); ++index) {
        const Wait& wait = waits_[index];
        if (!wait.used || wait.done || wait.cancelled) {
            continue;
        }

        const bool game = wait.clock == VmGameClock;
        const bool due = (wait.kind == WaitKind::NextTick && !paused && frame_ > wait.frame) ||
                         (wait.kind == WaitKind::Delay && frame_ > wait.frame &&
                          !(paused && game) && (game ? gameTime_ : uiTime_) >= wait.deadline);
        if (due) {
            CompleteWait(index);
        }
    }

    Result<void> resumed = ResumeDone();
    if (resumed) {
        resumed = Drain();
    }

    execution_.keepLogs_ = false;
    return resumed;
}

Result<void> VmSession::FixedStep() noexcept {
    if (closed_ || execution_.running_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    BeginEntry();
    execution_.keepLogs_ = true;
    deliveredInPhase_ = 0;
    for (usize at = queueHead_; at < queue_.Size(); ++at) {
        queue_[at].carried = true;
    }

    if (auto carried = Drain(); !carried) {
        execution_.keepLogs_ = false;
        return carried;
    }

    for (uint32 index = 0; host_ != nullptr && index < waits_.Size(); ++index) {
        const Wait& wait = waits_[index];
        if (wait.used && wait.kind == WaitKind::Event && !wait.done && !wait.cancelled &&
            !host_->IsAlive(wait.publisher)) {
            FailWait(index);
        }
    }

    ++fixedStep_;
    for (uint32 index = 0; index < waits_.Size(); ++index) {
        const Wait& wait = waits_[index];
        if (wait.used && !wait.done && !wait.cancelled && wait.kind == WaitKind::NextFixedTick &&
            fixedStep_ > wait.fixedStep) {
            CompleteWait(index);
        }
    }

    Result<void> resumed = ResumeDone();
    if (resumed) {
        resumed = Drain();
    }

    execution_.keepLogs_ = false;
    return resumed;
}

} // namespace rocks::sparks
