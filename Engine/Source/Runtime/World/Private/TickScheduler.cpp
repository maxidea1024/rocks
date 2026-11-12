#include <Core/Math/MathUtility.h>
#include <World/TickScheduler.h>

namespace rocks {

const char* TickPhaseName(TickPhase phase) noexcept {
    switch (phase) {
    case TickPhase::PrePhysics:
        return "PrePhysics";
    case TickPhase::Physics:
        return "Physics";
    case TickPhase::PostPhysics:
        return "PostPhysics";
    case TickPhase::PreRender:
        return "PreRender";
    case TickPhase::LateUpdate:
        return "LateUpdate";
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
      scratch_(allocator), marks_(allocator), stack_(allocator), pendingRemovals_(allocator) {
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

    const uint32 phase = static_cast<uint32>(entry->registration.phase);
    if (running_) {
        // 실행 중에는 저장소를 옮기지 않도록 표시만 하고 단계 경계에서 지운다.
        auto queued = pendingRemovals_.TryPushBack(handle);
        if (!queued) {
            return queued;
        }
        entry->removed = true;
        return Result<void>::Success();
    }

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
    }

    entry->registration.enabled = enabled;
    return Result<void>::Success();
}

Result<bool> TickScheduler::Reaches(TickHandle from, TickHandle to) noexcept {
    stack_.Clear();
    auto pushed = stack_.TryPushBack(from);
    if (!pushed) {
        return Result<bool>::Failure(pushed.Error());
    }

    // 의존 그래프는 사이클이 없으므로 방문 표시 없이도 끝난다. 다이아몬드 모양에서 중복 방문이 생길
    // 수 있지만 등록 시점에만 실행한다.
    while (stack_.Size() != 0) {
        auto popped = stack_.TryPopBack();
        const TickHandle current = popped.Value();
        if (current == to) {
            return Result<bool>::Success(true);
        }
        const Entry* entry = entries_.Find(current.slot);
        if (entry == nullptr || entry->removed) {
            continue;
        }
        for (const TickHandle& prerequisite : entry->prerequisites) {
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
    for (const TickHandle& handle : pendingRemovals_) {
        const Entry* entry = entries_.Find(handle.slot);
        if (entry == nullptr) {
            continue;
        }
        dirty_[static_cast<uint32>(entry->registration.phase)] = true;
        // 표시된 항목은 유효하므로 제거가 실패하지 않는다.
        auto removed = entries_.TryRemove(handle.slot);
        (void)removed;
    }

    pendingRemovals_.Clear();
}

Result<void> TickScheduler::RebuildOrder(uint32 phase) noexcept {
    // 1) 이 단계의 살아 있는 항목을 priority → 등록 순서로 정렬한다.
    scratch_.Clear();
    for (auto iterator = entries_.begin(); iterator != entries_.end(); ++iterator) {
        const Entry& entry = *iterator;
        if (entry.removed || static_cast<uint32>(entry.registration.phase) != phase) {
            continue;
        }
        auto pushed = scratch_.TryPushBack(TickHandle{iterator.Handle()});
        if (!pushed) {
            return pushed;
        }
    }

    scratch_.Sort([this](const TickHandle& left, const TickHandle& right) noexcept {
        const Entry& a = *entries_.Find(left.slot);
        const Entry& b = *entries_.Find(right.slot);
        if (a.registration.priority != b.registration.priority) {
            return a.registration.priority < b.registration.priority;
        }
        return a.sequence < b.sequence;
    });

    // 2) 정렬 순서대로 깊이 우선 후위 방문해 선행 의존을 먼저 내보낸다. 의존이 없는 항목끼리는
    //    정렬 순서가 유지되고, 선행 항목은 priority와 무관하게 의존 항목 앞으로 당겨진다.
    //    표시: 0 미방문, 1 방문 중, 2 완료.
    auto resized = marks_.TryResize(entries_.SlotCount());
    if (!resized) {
        return resized;
    }
    for (uint8& mark : marks_) {
        mark = 0;
    }

    Array<TickHandle>& order = order_[phase];
    order.Clear();
    auto reserved = order.TryReserve(scratch_.Size());
    if (!reserved) {
        return reserved;
    }

    for (const TickHandle& root : scratch_) {
        if (marks_[root.slot.index] != 0) {
            continue;
        }
        stack_.Clear();
        auto pushed = stack_.TryPushBack(root);
        if (!pushed) {
            return pushed;
        }
        while (stack_.Size() != 0) {
            const TickHandle current = stack_[stack_.Size() - 1];
            uint8& mark = marks_[current.slot.index];
            if (mark == 2) {
                auto popped = stack_.TryPopBack();
                (void)popped;
                continue;
            }
            if (mark == 1) {
                // 모든 선행을 내보낸 뒤 돌아왔다.
                mark = 2;
                auto popped = stack_.TryPopBack();
                (void)popped;
                auto emitted = order.TryPushBack(current);
                if (!emitted) {
                    return emitted;
                }
                continue;
            }
            mark = 1;
            const Entry& entry = *entries_.Find(current.slot);
            for (usize index = entry.prerequisites.Size(); index > 0; --index) {
                const TickHandle prerequisite = entry.prerequisites[index - 1];
                const Entry* before = entries_.Find(prerequisite.slot);
                // 해제된 선행과 이미 방문한 선행은 건너뛴다. 사이클은 추가 시점에 거부했다.
                if (before == nullptr || before->removed || marks_[prerequisite.slot.index] != 0) {
                    continue;
                }
                auto next = stack_.TryPushBack(prerequisite);
                if (!next) {
                    return next;
                }
            }
        }
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
        if (entry->registration.intervalSeconds > 0.0) {
            entry->accumulated += delta;
            if (entry->accumulated < entry->registration.intervalSeconds) {
                continue;
            }
            executedDelta = entry->accumulated;
            entry->accumulated = 0.0;
        }

        TickContext context;
        context.phase = phase;
        context.deltaSeconds = executedDelta;
        context.unscaledDeltaSeconds = frame.unscaledDeltaSeconds;
        context.frameIndex = frame.frameIndex;
        context.fixedStepIndex = frame.fixedStepIndex;
        context.paused = frame.paused;
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
    stats.pendingChanges = static_cast<uint32>(pendingRemovals_.Size());
    for (bool dirty : dirty_) {
        stats.pendingChanges += dirty ? 1u : 0u;
    }

    return stats;
}

} // namespace rocks
