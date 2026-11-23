#include <Core/Math/MathUtility.h>
#include <World/TickScheduler.h>

namespace rocks {

namespace {

constexpr uint32 NoIndex = NumericLimits<uint32>::Max();

// interval 판정의 상대 허용오차다. 1/60초를 여러 번 더한 값이 0.1초에 아주 조금 못 미쳐 한 프레임
// 늦게 실행되는 일을 막는다(GameClock의 고정 스텝 판정과 같은 값).
constexpr double IntervalTolerance = 1.0e-9;

bool IsFixedPhase(TickPhase phase) noexcept {
    return phase == TickPhase::Physics;
}

} // namespace

const char* TickPhaseName(TickPhase phase) noexcept {
    switch (phase) {
    case TickPhase::PrePhysics:
        return "PrePhysics";
    case TickPhase::Physics:
        return "Physics";
    case TickPhase::PostPhysics:
        return "PostPhysics";
    case TickPhase::LateUpdate:
        return "LateUpdate";
    case TickPhase::PreRender:
        return "PreRender";
    case TickPhase::Count:
        break;
    }

    return "Unknown";
}

TickScheduler::Entry::Entry(Allocator& allocator, const TickRegistration& value,
                            uint64 order) noexcept
    : registration(value), prerequisites(allocator), sequence(order), effective(value.phase) {
}

TickScheduler::TickScheduler(Allocator& allocator) noexcept
    : allocator_(allocator), entries_(allocator),
      order_{Array<TickHandle>(allocator), Array<TickHandle>(allocator),
             Array<TickHandle>(allocator), Array<TickHandle>(allocator),
             Array<TickHandle>(allocator)},
      pendingRemoved_(allocator), scratch_(allocator), localIndex_(allocator), indegree_(allocator),
      dependentOffsets_(allocator), dependents_(allocator), heap_(allocator),
      phaseScratch_(allocator), marks_(allocator), stack_(allocator) {
}

Result<TickHandle> TickScheduler::Register(const TickRegistration& registration) noexcept {
    if (closed_) {
        return Result<TickHandle>::Failure(ErrorCode::ShuttingDown);
    }

    const uint32 phase = static_cast<uint32>(registration.phase);
    if (phase >= TickPhaseCount || !math::IsFinite(registration.intervalSeconds) ||
        registration.intervalSeconds < 0.0) {
        return Result<TickHandle>::Failure(ErrorCode::InvalidArgument);
    }

    // 실행 중 해제가 할당하지 않도록 미룬 해제 목록의 용량을 등록 수만큼 확보한다.
    auto reserved = pendingRemoved_.TryEnsureCapacity(entries_.Size() + 1);
    if (!reserved) {
        return Result<TickHandle>::Failure(reserved.Error());
    }

    auto added = entries_.TryEmplace(allocator_, registration, nextSequence_);
    if (!added) {
        return Result<TickHandle>::Failure(added.Error());
    }

    ++nextSequence_;
    // 새 항목은 등록 순서가 가장 늦고 선행이 없다. priority가 순서 배열의 최댓값 이상이면
    // 재구성해도 맨 끝에 오므로 끝에 붙인다(실행 중에는 배열을 바꾸지 않는다 — 다음 경계의 재구성).
    Array<TickHandle>& order = order_[phase];
    if (!running_ && !dirty_[phase] &&
        (order.IsEmpty() || registration.priority >= maxPriority_[phase]) &&
        order.TryPushBack(TickHandle{added.Value()})) {
        entries_.Find(added.Value())->orderPosition = static_cast<uint32>(order.Size() - 1);
        maxPriority_[phase] = order.Size() == 1 || registration.priority > maxPriority_[phase]
                                  ? registration.priority
                                  : maxPriority_[phase];
    } else {
        dirty_[phase] = true;
    }

    return Result<TickHandle>::Success(TickHandle{added.Value()});
}

Result<void> TickScheduler::Unregister(TickHandle handle) noexcept {
    Entry* entry = entries_.Find(handle.slot);
    if (entry == nullptr || entry->removed) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (running_) {
        // 실행 중에는 저장소를 옮기지 않도록 표시만 하고 단계 경계에서 지운다(할당 없음 — 용량은
        // 등록 때 확보했다).
        entry->removed = true;
        ++pendingRemovals_;
        auto queued = pendingRemoved_.TryPushBack(handle);
        (void)queued;
        return Result<void>::Success();
    }

    const uint32 phase = static_cast<uint32>(entry->effective);
    const bool ordered = entry->orderPosition != NoOrder;
    const bool reorder = ReleaseDependencies(*entry);
    if (entry->effective != entry->registration.phase && demoted_ != 0) {
        --demoted_;
    }
    auto removed = entries_.TryRemove(handle.slot);
    if (!removed) {
        return removed;
    }

    // 이 항목에 의존하던 항목은 이 항목 때문에 늦춰졌을 수 있다(늦춰진 항목이 있을 때만).
    phasesDirty_ = phasesDirty_ || (reorder && demoted_ != 0);

    NoteRemovedLocked(phase, ordered);
    dirty_[phase] = dirty_[phase] || reorder;
    return Result<void>::Success();
}

Result<void> TickScheduler::SetEnabled(TickHandle handle, bool enabled) noexcept {
    Entry* entry = entries_.Find(handle.slot);
    if (entry == nullptr || entry->removed) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (enabled && !entry->registration.enabled) {
        // 다시 켠 항목은 꺼져 있던 동안의 시간을 한꺼번에 넘기지 않는다.
        entry->accumulated = 0.0;
        entry->sinceLastRun = 0.0;
    }

    entry->registration.enabled = enabled;
    return Result<void>::Success();
}

Result<void> TickScheduler::SetPhase(TickHandle handle, TickPhase phase) noexcept {
    Entry* entry = entries_.Find(handle.slot);
    if (entry == nullptr || entry->removed) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (static_cast<uint32>(phase) >= TickPhaseCount ||
        IsFixedPhase(phase) != IsFixedPhase(entry->registration.phase)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (entry->registration.phase != phase) {
        entry->registration.phase = phase;
        phasesDirty_ = true;
    }

    return Result<void>::Success();
}

Result<void> TickScheduler::SetPriority(TickHandle handle, int32 priority) noexcept {
    Entry* entry = entries_.Find(handle.slot);
    if (entry == nullptr || entry->removed) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (entry->registration.priority != priority) {
        entry->registration.priority = priority;
        dirty_[static_cast<uint32>(entry->effective)] = true;
    }

    return Result<void>::Success();
}

Result<void> TickScheduler::SetInterval(TickHandle handle, double intervalSeconds) noexcept {
    Entry* entry = entries_.Find(handle.slot);
    if (entry == nullptr || entry->removed) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (!math::IsFinite(intervalSeconds) || intervalSeconds < 0.0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    entry->registration.intervalSeconds = intervalSeconds;
    entry->accumulated = 0.0;
    entry->sinceLastRun = 0.0;
    return Result<void>::Success();
}

Result<bool> TickScheduler::Reaches(TickHandle from, TickHandle to) noexcept {
    // 늘어난 칸은 0(방문 안 함)이다. 번호가 한 바퀴 돌 때만 전체를 비운다.
    const usize previous = marks_.Size();
    auto resized = marks_.TryResize(entries_.SlotCount());
    if (!resized) {
        return Result<bool>::Failure(resized.Error());
    }

    for (usize at = previous; at < marks_.Size(); ++at) {
        marks_[at] = 0;
    }

    if (++markEpoch_ == 0) {
        for (uint32& mark : marks_) {
            mark = 0;
        }

        markEpoch_ = 1;
    }

    const uint32 epoch = markEpoch_;

    stack_.Clear();
    auto pushed = stack_.TryPushBack(from);
    if (!pushed) {
        return Result<bool>::Failure(pushed.Error());
    }

    // 각 항목을 한 번만 펼친다(다이아몬드 모양 의존에서도 선형).
    while (stack_.Size() != 0) {
        auto popped = stack_.TryPopBack();
        const TickHandle current = popped.Value();
        if (current == to) {
            return Result<bool>::Success(true);
        }
        const Entry* entry = entries_.Find(current.slot);
        if (entry == nullptr || entry->removed || marks_[current.slot.index] == epoch) {
            continue;
        }
        marks_[current.slot.index] = epoch;
        for (const TickHandle& prerequisite : entry->prerequisites) {
            const Entry* before = entries_.Find(prerequisite.slot);
            if (before == nullptr || before->removed || marks_[prerequisite.slot.index] == epoch) {
                continue;
            }
            auto next = stack_.TryPushBack(prerequisite);
            if (!next) {
                return Result<bool>::Failure(next.Error());
            }
        }
    }

    return Result<bool>::Success(false);
}

Result<void> TickScheduler::AddPrerequisite(TickHandle dependent,
                                            TickHandle prerequisite) noexcept {
    Entry* after = entries_.Find(dependent.slot);
    Entry* before = entries_.Find(prerequisite.slot);
    if (after == nullptr || after->removed || before == nullptr || before->removed) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    // 고정 스텝 등록과 가변 등록은 실행 횟수가 달라 순서가 뜻이 없다.
    if (dependent == prerequisite ||
        IsFixedPhase(after->registration.phase) != IsFixedPhase(before->registration.phase)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 해제된 선행 handle을 여기서도 지운다. 단계를 넘는 간선은 선행이 해제돼도 dependent의 단계가
    // 재구성되지 않을 수 있어(재구성만 지우면) 선행을 붙였다 떼기를 반복할 때 목록이 계속 자란다.
    for (usize index = after->prerequisites.Size(); index > 0; --index) {
        const Entry* existing = entries_.Find(after->prerequisites[index - 1].slot);
        if (existing == nullptr || existing->removed) {
            auto pruned = after->prerequisites.TryRemoveAt(index - 1);
            (void)pruned;
        }
    }
    for (const TickHandle& existing : after->prerequisites) {
        if (existing == prerequisite) {
            return Result<void>::Success();
        }
    }

    // prerequisite가 이미 dependent를 (간접) 선행으로 가지면 새 간선은 사이클을 만든다.
    auto cycle = Reaches(prerequisite, dependent);
    if (!cycle) {
        return Result<void>::Failure(cycle.Error());
    }
    if (cycle.Value()) {
        return Result<void>::Failure(ErrorCode::CycleDetected);
    }

    after = entries_.Find(dependent.slot);
    auto added = after->prerequisites.TryPushBack(prerequisite);
    if (!added) {
        return added;
    }

    before = entries_.Find(prerequisite.slot);
    ++before->dependents;

    // 선행이 더 늦은 단계면 dependent를 늦춘다(다음 프레임 첫 RunPhase 전에 반영). 더 이른 단계면
    // 단계 순서가 이미 보장한다(간선은 실효 단계 계산을 위해 남긴다).
    if (before->effective > after->effective) {
        phasesDirty_ = true;
        return Result<void>::Success();
    }
    if (before->effective < after->effective) {
        return Result<void>::Success();
    }

    // 같은 단계: 지금 순서에서 이미 선행이 앞이면 새 간선은 순서를 바꾸지 않는다(재구성해도 같은
    // 순서 — 이 간선이 막는 선택은 원래 순서에서도 일어나지 않았다).
    const uint32 phase = static_cast<uint32>(after->effective);
    if (dirty_[phase] || before->orderPosition == NoOrder || after->orderPosition == NoOrder ||
        before->orderPosition >= after->orderPosition) {
        dirty_[phase] = true;
    }

    return Result<void>::Success();
}

Result<void> TickScheduler::RemovePrerequisite(TickHandle dependent,
                                               TickHandle prerequisite) noexcept {
    Entry* after = entries_.Find(dependent.slot);
    if (after == nullptr || after->removed) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    for (usize index = 0; index < after->prerequisites.Size(); ++index) {
        if (after->prerequisites[index] == prerequisite) {
            auto removed = after->prerequisites.TryRemoveAt(index);
            if (!removed) {
                return removed;
            }

            Entry* before = entries_.Find(prerequisite.slot);
            if (before != nullptr && before->dependents != 0) {
                --before->dependents;
            }

            if (before != nullptr && before->effective == after->effective) {
                dirty_[static_cast<uint32>(after->effective)] = true;
            }
            // 이 간선 때문에 늦춰졌을 수 있다.
            if (after->effective != after->registration.phase) {
                phasesDirty_ = true;
            }

            return Result<void>::Success();
        }
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

bool TickScheduler::IsRegistered(TickHandle handle) const noexcept {
    const Entry* entry = entries_.Find(handle.slot);
    return entry != nullptr && !entry->removed;
}

Result<TickPhase> TickScheduler::EffectivePhase(TickHandle handle) const noexcept {
    const Entry* entry = entries_.Find(handle.slot);
    if (entry == nullptr || entry->removed) {
        return Result<TickPhase>::Failure(ErrorCode::StaleHandle);
    }

    return Result<TickPhase>::Success(entry->effective);
}

void TickScheduler::BeginFrame() noexcept {
    executedThisFrame_ = 0;
}

Result<void> TickScheduler::RecomputeEffectivePhases() noexcept {
    // 1) 살아 있는 항목을 모은다(scratch_ 위치 = 지역 번호). 해제된 선행 handle도 여기서
    // 지운다(단계를
    //    넘는 간선은 그 단계의 재구성이 지우지 못할 수 있다).
    scratch_.Clear();
    for (auto iterator = entries_.begin(); iterator != entries_.end(); ++iterator) {
        Entry& entry = *iterator;
        if (entry.removed) {
            continue;
        }
        for (usize index = entry.prerequisites.Size(); index > 0; --index) {
            const Entry* before = entries_.Find(entry.prerequisites[index - 1].slot);
            if (before == nullptr || before->removed) {
                auto pruned = entry.prerequisites.TryRemoveAt(index - 1);
                (void)pruned;
            }
        }
        auto pushed = scratch_.TryPushBack(TickHandle{iterator.Handle()});
        if (!pushed) {
            return pushed;
        }
    }

    const usize count = scratch_.Size();
    if (!localIndex_.TryResize(entries_.SlotCount()) || !indegree_.TryResize(count) ||
        !dependentOffsets_.TryResize(count + 1) || !heap_.TryReserve(count) ||
        !phaseScratch_.TryResize(count)) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }
    for (uint32& value : localIndex_) {
        value = NoIndex;
    }
    for (usize index = 0; index < count; ++index) {
        localIndex_[scratch_[index].slot.index] = static_cast<uint32>(index);
        indegree_[index] = 0;
        phaseScratch_[index] = entries_.Find(scratch_[index].slot)->registration.phase;
    }
    for (uint32& value : dependentOffsets_) {
        value = 0;
    }

    // 2) 단계와 무관한 전체 그래프의 진입 차수와 역방향(선행 → 의존) 인접 목록(CSR)이다. 지역
    // 번호는
    //    칸 번호로 찾으므로 세대가 다른(해제 뒤 재사용된) 선행은 handle 비교로 거른다.
    uint32 edges = 0;
    for (usize index = 0; index < count; ++index) {
        const Entry& entry = *entries_.Find(scratch_[index].slot);
        for (const TickHandle& prerequisite : entry.prerequisites) {
            const uint32 local = localIndex_[prerequisite.slot.index];
            if (local == NoIndex || !(scratch_[local] == prerequisite)) {
                continue;
            }
            ++indegree_[index];
            ++dependentOffsets_[local + 1];
            ++edges;
        }
    }
    for (usize index = 0; index < count; ++index) {
        dependentOffsets_[index + 1] += dependentOffsets_[index];
    }
    if (!dependents_.TryResize(edges)) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }
    // 채우기 위치로 heap_을 잠시 쓴다.
    heap_.Clear();
    for (usize index = 0; index < count; ++index) {
        auto pushed = heap_.TryPushBack(dependentOffsets_[index]);
        (void)pushed;
    }
    for (usize index = 0; index < count; ++index) {
        const Entry& entry = *entries_.Find(scratch_[index].slot);
        for (const TickHandle& prerequisite : entry.prerequisites) {
            const uint32 local = localIndex_[prerequisite.slot.index];
            if (local != NoIndex && scratch_[local] == prerequisite) {
                dependents_[heap_[local]++] = static_cast<uint32>(index);
            }
        }
    }

    // 3) 위상 순서로 실효 단계 = max(요청 단계, 선행의 실효 단계)를 계산한다(phaseScratch_ — 다
    // 계산한
    //    뒤 반영하므로 실패해도 상태가 바뀌지 않는다). 처리 순서는 결과에 영향이 없다(최댓값).
    heap_.Clear();
    for (usize index = 0; index < count; ++index) {
        if (indegree_[index] == 0) {
            auto pushed = heap_.TryPushBack(static_cast<uint32>(index));
            (void)pushed;
        }
    }
    usize processed = 0;
    while (heap_.Size() != 0) {
        const uint32 local = heap_.TryPopBack().Value();
        ++processed;
        for (uint32 edge = dependentOffsets_[local]; edge < dependentOffsets_[local + 1]; ++edge) {
            const uint32 next = dependents_[edge];
            if (phaseScratch_[local] > phaseScratch_[next]) {
                phaseScratch_[next] = phaseScratch_[local];
            }
            if (--indegree_[next] == 0) {
                auto pushed = heap_.TryPushBack(next);
                (void)pushed;
            }
        }
    }

    // 사이클은 추가 시점에 거부하므로 모든 항목이 처리되어야 한다.
    if (processed != count) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 4) 바뀐 항목을 옮긴다. Physics는 늦춤 대상이 아니다(Physics 등록과 다른 등록 사이 간선은 추가
    // 때
    //    거부하므로 가변 등록의 결과가 Physics일 수 없지만, 그래도 PostPhysics로 민다 — UE도
    //    Start/EndPhysics를 건너뛴다).
    uint32 demoted = 0;
    for (usize index = 0; index < count; ++index) {
        Entry& entry = *entries_.Find(scratch_[index].slot);
        TickPhase phase = phaseScratch_[index];
        if (IsFixedPhase(phase) && !IsFixedPhase(entry.registration.phase)) {
            phase = TickPhase::PostPhysics;
        }
        if (phase != entry.effective) {
            dirty_[static_cast<uint32>(entry.effective)] = true;
            dirty_[static_cast<uint32>(phase)] = true;
            entry.effective = phase;
            entry.orderPosition = NoOrder;
        }
        demoted += phase != entry.registration.phase ? 1u : 0u;
    }

    demoted_ = demoted;
    phasesDirty_ = false;
    return Result<void>::Success();
}

void TickScheduler::ApplyPendingRemovals() noexcept {
    if (pendingRemovals_ == 0) {
        return;
    }

    // 미룬 해제만 지운다(전체 항목을 훑지 않는다).
    for (const TickHandle& handle : pendingRemoved_) {
        const Entry* entry = entries_.Find(handle.slot);
        if (entry == nullptr) {
            continue;
        }

        const uint32 phase = static_cast<uint32>(entry->effective);
        const bool ordered = entry->orderPosition != NoOrder;
        const bool reorder = ReleaseDependencies(*entry);
        if (entry->effective != entry->registration.phase && demoted_ != 0) {
            --demoted_;
        }
        auto removed = entries_.TryRemove(handle.slot);
        (void)removed;
        NoteRemovedLocked(phase, ordered);
        dirty_[phase] = dirty_[phase] || reorder;
        phasesDirty_ = phasesDirty_ || (reorder && demoted_ != 0);
    }

    pendingRemoved_.Clear();
    pendingRemovals_ = 0;
}

bool TickScheduler::ReleaseDependencies(const Entry& entry) noexcept {
    // 이 항목이 선행으로 둔 항목의 의존 계수를 내린다(해제된 의존은 순서를 바꾸지 않는다 — 의존하던
    // 쪽이 사라질 뿐이다). 이 항목에 의존하던 항목이 있으면 그쪽 순서가 풀린다.
    for (const TickHandle& prerequisite : entry.prerequisites) {
        Entry* before = entries_.Find(prerequisite.slot);
        if (before != nullptr && before->dependents != 0) {
            --before->dependents;
        }
    }

    return entry.dependents != 0;
}

void TickScheduler::NoteRemovedLocked(uint32 phase, bool ordered) noexcept {
    // 해제는 남은 항목의 순서를 바꾸지 않는다. 순서 배열에 남은 handle은 실행이 건너뛰고(세대가
    // 다르다), 그런 칸이 배열의 절반을 넘으면 다시 만든다.
    if (!ordered) {
        return;
    }

    if (++staleInOrder_[phase] * 2 > order_[phase].Size()) {
        dirty_[phase] = true;
    }
}

bool TickScheduler::Before(uint32 left, uint32 right) const noexcept {
    const Entry& a = *entries_.Find(scratch_[left].slot);
    const Entry& b = *entries_.Find(scratch_[right].slot);
    if (a.registration.priority != b.registration.priority) {
        return a.registration.priority < b.registration.priority;
    }
    return a.sequence < b.sequence;
}

void TickScheduler::HeapPush(uint32 local) noexcept {
    // 용량은 RebuildOrder가 미리 확보한다.
    auto pushed = heap_.TryPushBack(local);
    (void)pushed;
    usize child = heap_.Size() - 1;
    while (child > 0) {
        const usize parent = (child - 1) / 2;
        if (!Before(heap_[child], heap_[parent])) {
            break;
        }
        const uint32 swap = heap_[child];
        heap_[child] = heap_[parent];
        heap_[parent] = swap;
        child = parent;
    }
}

uint32 TickScheduler::HeapPop() noexcept {
    const uint32 top = heap_[0];
    const uint32 last = heap_.TryPopBack().Value();
    if (heap_.Size() == 0) {
        return top;
    }

    heap_[0] = last;
    usize parent = 0;
    for (;;) {
        const usize left = parent * 2 + 1;
        const usize right = left + 1;
        usize smallest = parent;
        if (left < heap_.Size() && Before(heap_[left], heap_[smallest])) {
            smallest = left;
        }
        if (right < heap_.Size() && Before(heap_[right], heap_[smallest])) {
            smallest = right;
        }
        if (smallest == parent) {
            break;
        }
        const uint32 swap = heap_[parent];
        heap_[parent] = heap_[smallest];
        heap_[smallest] = swap;
        parent = smallest;
    }

    return top;
}

Result<void> TickScheduler::RebuildOrder(uint32 phase) noexcept {
    // 1) 이 단계의 살아 있는 항목을 모으고, 해제된 선행 handle을 의존 목록에서 지운다.
    scratch_.Clear();
    for (auto iterator = entries_.begin(); iterator != entries_.end(); ++iterator) {
        Entry& entry = *iterator;
        if (entry.removed || static_cast<uint32>(entry.effective) != phase) {
            continue;
        }
        for (usize index = entry.prerequisites.Size(); index > 0; --index) {
            const Entry* before = entries_.Find(entry.prerequisites[index - 1].slot);
            if (before == nullptr || before->removed) {
                auto pruned = entry.prerequisites.TryRemoveAt(index - 1);
                (void)pruned;
            }
        }
        auto pushed = scratch_.TryPushBack(TickHandle{iterator.Handle()});
        if (!pushed) {
            return pushed;
        }
    }

    const usize count = scratch_.Size();
    if (!localIndex_.TryResize(entries_.SlotCount()) || !indegree_.TryResize(count) ||
        !dependentOffsets_.TryResize(count + 1) || !heap_.TryReserve(count)) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }
    for (uint32& value : localIndex_) {
        value = NoIndex;
    }
    for (usize index = 0; index < count; ++index) {
        localIndex_[scratch_[index].slot.index] = static_cast<uint32>(index);
        indegree_[index] = 0;
    }
    for (uint32& value : dependentOffsets_) {
        value = 0;
    }

    // 2) 진입 차수와 역방향(선행 → 의존) 인접 목록(CSR)을 만든다.
    uint32 edges = 0;
    for (usize index = 0; index < count; ++index) {
        const Entry& entry = *entries_.Find(scratch_[index].slot);
        for (const TickHandle& prerequisite : entry.prerequisites) {
            const uint32 local = localIndex_[prerequisite.slot.index];
            if (local == NoIndex) {
                continue;
            }
            ++indegree_[index];
            ++dependentOffsets_[local + 1];
            ++edges;
        }
    }
    for (usize index = 0; index < count; ++index) {
        dependentOffsets_[index + 1] += dependentOffsets_[index];
    }
    if (!dependents_.TryResize(edges)) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }
    // 채우기 위치로 heap_을 잠시 쓴다(아래 정렬 전에 비운다).
    heap_.Clear();
    for (usize index = 0; index < count; ++index) {
        auto pushed = heap_.TryPushBack(dependentOffsets_[index]);
        (void)pushed;
    }
    for (usize index = 0; index < count; ++index) {
        const Entry& entry = *entries_.Find(scratch_[index].slot);
        for (const TickHandle& prerequisite : entry.prerequisites) {
            const uint32 local = localIndex_[prerequisite.slot.index];
            if (local != NoIndex) {
                dependents_[heap_[local]++] = static_cast<uint32>(index);
            }
        }
    }

    // 3) 준비된 항목 중 (priority, 등록 순서) 최소부터 내보낸다.
    Array<TickHandle>& order = order_[phase];
    order.Clear();
    auto reserved = order.TryReserve(count);
    if (!reserved) {
        return reserved;
    }
    heap_.Clear();
    for (usize index = 0; index < count; ++index) {
        if (indegree_[index] == 0) {
            HeapPush(static_cast<uint32>(index));
        }
    }
    while (heap_.Size() != 0) {
        const uint32 local = HeapPop();
        auto emitted = order.TryPushBack(scratch_[local]);
        (void)emitted;
        for (uint32 edge = dependentOffsets_[local]; edge < dependentOffsets_[local + 1]; ++edge) {
            const uint32 next = dependents_[edge];
            if (--indegree_[next] == 0) {
                HeapPush(next);
            }
        }
    }

    // 사이클은 추가 시점에 거부하므로 모든 항목이 나와야 한다.
    if (order.Size() != count) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    int32 maxPriority = 0;
    for (usize position = 0; position < count; ++position) {
        Entry& entry = *entries_.Find(order[position].slot);
        entry.orderPosition = static_cast<uint32>(position);
        maxPriority = position == 0 || entry.registration.priority > maxPriority
                          ? entry.registration.priority
                          : maxPriority;
    }

    maxPriority_[phase] = maxPriority;
    staleInOrder_[phase] = 0;
    dirty_[phase] = false;
    return Result<void>::Success();
}

Result<void> TickScheduler::RunPhase(TickPhase phase, const TickPhaseFrame& frame,
                                     TickExecutor& executor) noexcept {
    const uint32 index = static_cast<uint32>(phase);
    if (index >= TickPhaseCount) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (running_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    ApplyPendingRemovals();
    // 실효 단계 이동은 새 프레임의 첫 단계 전에만 한다(이미 실행한 항목을 다른 단계로 옮기면 한
    // 프레임에 두 번 실행될 수 있다). 할당이 실패하면 이전 실효 단계로 실행하고 다음 프레임에 다시
    // 계산한다(실패해도 상태는 바뀌지 않는다 — 프레임 전체를 멈추지 않는다).
    if (frame.frameIndex != lastFrameIndex_) {
        lastFrameIndex_ = frame.frameIndex;
        if (phasesDirty_ && !RecomputeEffectivePhases()) {
            ++recomputeFailures_;
        }
    }
    if (dirty_[index]) {
        auto rebuilt = RebuildOrder(index);
        if (!rebuilt) {
            return rebuilt;
        }
    }

    running_ = true;
    const Array<TickHandle>& order = order_[index];
    // 실행 중 등록이 추가되어도 이 배열은 단계가 끝날 때까지 바뀌지 않는다(재정렬은 다음 경계).
    for (usize position = 0; position < order.Size(); ++position) {
        Entry* entry = entries_.Find(order[position].slot);
        if (entry == nullptr || entry->removed || !entry->registration.enabled ||
            static_cast<uint32>(entry->effective) != index) {
            continue;
        }
        if (frame.paused && !entry->registration.tickWhenPaused) {
            continue;
        }

        const double delta = frame.paused ? frame.unscaledDeltaSeconds : frame.deltaSeconds;
        double executedDelta = delta;
        const double interval = entry->registration.intervalSeconds;
        if (interval > 0.0) {
            entry->accumulated += delta;
            entry->sinceLastRun += delta;
            if (entry->accumulated < interval * (1.0 - IntervalTolerance)) {
                continue;
            }
            executedDelta = entry->sinceLastRun;
            entry->sinceLastRun = 0.0;
            // 남은 누적을 다음 주기로 넘기되 한 번에 여러 주기를 따라잡지 않는다.
            entry->accumulated -= interval;
            if (!(entry->accumulated > 0.0) || entry->accumulated >= interval) {
                entry->accumulated = 0.0;
            }
        }

        TickContext context;
        context.phase = phase;
        context.deltaSeconds = executedDelta;
        context.unscaledDeltaSeconds = frame.unscaledDeltaSeconds;
        context.frameIndex = frame.frameIndex;
        context.fixedStepIndex = frame.fixedStepIndex;
        context.paused = frame.paused;
        context.handle = order[position];
        const TickTarget target = entry->registration.target;
        ++executedThisFrame_;
        // 실행기가 등록을 바꿔 저장소가 재배치될 수 있으므로 이후 entry 포인터를 쓰지 않는다.
        executor.ExecuteTick(target, context);
    }

    running_ = false;
    ApplyPendingRemovals();
    return Result<void>::Success();
}

TickStats TickScheduler::Stats() const noexcept {
    TickStats stats;
    for (const Entry& entry : entries_) {
        if (entry.removed) {
            continue;
        }
        ++stats.registered;
        if (entry.registration.enabled) {
            ++stats.enabled;
        }
    }

    stats.executedThisFrame = executedThisFrame_;
    stats.pendingChanges = pendingRemovals_ + (phasesDirty_ ? 1u : 0u);
    stats.phaseRecomputeFailures = recomputeFailures_;
    for (bool dirty : dirty_) {
        stats.pendingChanges += dirty ? 1u : 0u;
    }

    return stats;
}

} // namespace rocks
