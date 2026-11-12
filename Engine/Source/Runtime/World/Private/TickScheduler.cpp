#include <Core/Math/MathUtility.h>
#include <World/TickScheduler.h>

namespace rocks {

namespace {

constexpr uint32 NoIndex = 0xffffffffu;

// interval 판정의 상대 허용오차다. 1/60초를 여러 번 더한 값이 0.1초에 아주 조금 못 미쳐 한 프레임
// 늦게 실행되는 일을 막는다(GameClock의 고정 스텝 판정과 같은 값).
constexpr double IntervalTolerance = 1.0e-9;

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
    : registration(value), prerequisites(allocator), sequence(order) {
}

TickScheduler::TickScheduler(Allocator& allocator) noexcept
    : allocator_(allocator), entries_(allocator),
      order_{Array<TickHandle>(allocator), Array<TickHandle>(allocator),
             Array<TickHandle>(allocator), Array<TickHandle>(allocator),
             Array<TickHandle>(allocator)},
      scratch_(allocator), localIndex_(allocator), indegree_(allocator),
      dependentOffsets_(allocator), dependents_(allocator), heap_(allocator), marks_(allocator),
      stack_(allocator) {
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

    auto added = entries_.TryEmplace(allocator_, registration, nextSequence_);
    if (!added) {
        return Result<TickHandle>::Failure(added.Error());
    }

    ++nextSequence_;
    dirty_[phase] = true;
    return Result<TickHandle>::Success(TickHandle{added.Value()});
}

Result<void> TickScheduler::Unregister(TickHandle handle) noexcept {
    Entry* entry = entries_.Find(handle.slot);
    if (entry == nullptr || entry->removed) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (running_) {
        // 실행 중에는 저장소를 옮기지 않도록 표시만 하고 단계 경계에서 지운다(할당 없음).
        entry->removed = true;
        ++pendingRemovals_;
        return Result<void>::Success();
    }

    const uint32 phase = static_cast<uint32>(entry->registration.phase);
    auto removed = entries_.TryRemove(handle.slot);
    if (!removed) {
        return removed;
    }

    dirty_[phase] = true;
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

Result<bool> TickScheduler::Reaches(TickHandle from, TickHandle to) noexcept {
    auto resized = marks_.TryResize(entries_.SlotCount());
    if (!resized) {
        return Result<bool>::Failure(resized.Error());
    }
    for (uint8& mark : marks_) {
        mark = 0;
    }

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
        if (entry == nullptr || entry->removed || marks_[current.slot.index] != 0) {
            continue;
        }
        marks_[current.slot.index] = 1;
        for (const TickHandle& prerequisite : entry->prerequisites) {
            const Entry* before = entries_.Find(prerequisite.slot);
            if (before == nullptr || before->removed || marks_[prerequisite.slot.index] != 0) {
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

    if (dependent == prerequisite || after->registration.phase != before->registration.phase) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
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
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    after = entries_.Find(dependent.slot);
    auto added = after->prerequisites.TryPushBack(prerequisite);
    if (!added) {
        return added;
    }

    dirty_[static_cast<uint32>(after->registration.phase)] = true;
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
            dirty_[static_cast<uint32>(after->registration.phase)] = true;
            return Result<void>::Success();
        }
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

bool TickScheduler::IsRegistered(TickHandle handle) const noexcept {
    const Entry* entry = entries_.Find(handle.slot);
    return entry != nullptr && !entry->removed;
}

void TickScheduler::BeginFrame() noexcept {
    executedThisFrame_ = 0;
}

void TickScheduler::ApplyPendingRemovals() noexcept {
    if (pendingRemovals_ == 0) {
        return;
    }

    // SlotArray는 순회 중 현재 원소 삭제를 허용한다.
    for (auto iterator = entries_.begin(); iterator != entries_.end(); ++iterator) {
        if (!iterator->removed) {
            continue;
        }
        dirty_[static_cast<uint32>(iterator->registration.phase)] = true;
        auto removed = entries_.TryRemove(iterator.Handle());
        (void)removed;
    }

    pendingRemovals_ = 0;
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
        if (entry.removed || static_cast<uint32>(entry.registration.phase) != phase) {
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
        if (entry == nullptr || entry->removed || !entry->registration.enabled) {
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
    stats.pendingChanges = pendingRemovals_;
    for (bool dirty : dirty_) {
        stats.pendingChanges += dirty ? 1u : 0u;
    }

    return stats;
}

} // namespace rocks
