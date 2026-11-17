#include "TextAssignInternal.h"

#include <Core/Containers/HashMap.h>
#include <Object/ObjectBatchPublish.h>

// 게시된 객체 대상의 묶음 게시 구간에 넘길 새 대상 재확인 목록과 간선 변경 목록이다(리플렉션
// B9i3c2–c4·B9i5 — Docs/Design/ReflectionExtensions.ko.md 10.2절).
namespace rocks::reflection_text::detail {

namespace {

// 컨테이너 하나의 옛 내용(previous)과 새 내용(current)의 handle 다중집합 차이만 선언한다: 새로
// 들어오는 대상은 재확인과 {null, 대상}, 빠지는 대상은 {대상, null}이다. 그대로 남는 대상(자리가
// 바뀐 것 포함)은 이미 게시된 간선이라 선언·재확인하지 않는다 — 구간 안 PublishBatchArray·
// PublishBatchMap도 옛 내용에 있던 대상은 재확인하지 않으므로(ObjectRegistry::PublishBatchTarget의
// retained) 다른 원소가 Discard된 대상을 가리켜도 게시가 StaleHandle로 막히지 않는다(리플렉션 B9i5,
// B9i 리뷰 L1). counts는 호출 사이에 다시 쓰는 작업 표다.
Result<void> AppendContainerChanges(const PreparedReferences& stage,
                                    HashMap<ObjectHandle, usize>& counts,
                                    Array<BatchTargetCheck>& targets,
                                    Array<BatchEdgeChange>& edges) noexcept {
    counts.Clear();
    for (const ObjectHandle previous : stage.Previous()) {
        if (!previous.IsNull()) {
            auto found = counts.FindOrAdd(previous, usize{0});
            if (!found) {
                return Result<void>::Failure(found.Error());
            }

            ++*found.Value().value;
        }
    }

    for (const ObjectHandle current : stage.Current()) {
        if (current.IsNull()) {
            continue;
        }

        usize* remaining = counts.Find(current);
        if (remaining != nullptr && *remaining != 0) {
            --*remaining;
            continue;
        }

        (void)targets.TryPushBack(BatchTargetCheck{current, stage.TargetType()});
        (void)edges.TryPushBack(BatchEdgeChange{ObjectHandle{}, current});
    }

    // 짝을 찾지 못하고 남은 옛 대상이 빠지는 간선이다.
    for (const ObjectHandle previous : stage.Previous()) {
        usize* remaining = previous.IsNull() ? nullptr : counts.Find(previous);
        if (remaining != nullptr && *remaining != 0) {
            --*remaining;
            (void)edges.TryPushBack(BatchEdgeChange{previous, ObjectHandle{}});
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<void> PlanBatchPublish(AssignmentState& state, Array<BatchTargetCheck>& targets,
                              Array<BatchEdgeChange>& edges) noexcept {
    usize targetCount = state.pins.Size() + state.references.Size();
    usize edgeCount = state.references.Size();
    for (usize index = 0; index < state.containers.Size(); ++index) {
        const PreparedReferences& stage = state.containers[index]->stage;
        targetCount += stage.Current().Size();
        edgeCount += stage.Previous().Size() + stage.Current().Size();
    }

    for (usize index = 0; index < state.elements.Size(); ++index) {
        const PreparedStructElements& stage = state.elements[index]->stage;
        targetCount += stage.AddedReferences().Size();
        edgeCount += stage.RemovedReferences().Size() + stage.AddedReferences().Size();
    }

    if (auto reserved = targets.TryReserve(targetCount); !reserved) {
        return reserved;
    }

    if (auto reserved = edges.TryReserve(edgeCount); !reserved) {
        return reserved;
    }

    HashMap<ObjectHandle, usize> counts(state.allocator);
    for (usize index = 0; index < state.containers.Size(); ++index) {
        if (auto appended =
                AppendContainerChanges(state.containers[index]->stage, counts, targets, edges);
            !appended) {
            return appended;
        }
    }

    // 원소 지우기는 지운 원소 안 참조마다 {참조, null}, 원소 추가는 남는 추가 원소 안 참조마다
    // 재확인과 {null, 참조}다(기대 타입은 쓸 때 정적 대조했다 — 여기서는 생존만 본다).
    for (usize index = 0; index < state.elements.Size(); ++index) {
        const PreparedStructElements& stage = state.elements[index]->stage;
        for (const ObjectHandle removed : stage.RemovedReferences()) {
            (void)edges.TryPushBack(BatchEdgeChange{removed, ObjectHandle{}});
        }

        for (const ObjectHandle added : stage.AddedReferences()) {
            (void)targets.TryPushBack(BatchTargetCheck{added, nullptr});
            (void)edges.TryPushBack(BatchEdgeChange{ObjectHandle{}, added});
        }
    }

    // Weak 고정은 생존만, 참조 단위의 새 대상은 자리의 C++ 대상 타입까지 본다. 컨테이너 단위의
    // 고정은 GC만 막는다 — 새로 들어오는 원소는 위에서 검사하고, 사본에서 다시 빠진 대상(
    // `group=/a` 뒤 `group=`)은 검사하지 않는다(리플렉션 B9i 리뷰 반영).
    for (usize index = 0; index < state.pins.Size(); ++index) {
        bool container = false;
        for (usize unit = 0; unit < state.containers.Size() && !container; ++unit) {
            container = state.pinOwners[index] == state.containers[unit].Get();
        }

        if (!container) {
            (void)targets.TryPushBack(BatchTargetCheck{state.pins[index].Handle(), nullptr});
        }
    }

    for (usize index = 0; index < state.references.Size(); ++index) {
        const ReferenceUnit& unit = *state.references[index];
        if (!unit.target.IsNull()) {
            (void)targets.TryPushBack(BatchTargetCheck{unit.target, unit.prepared.TargetType()});
        }

        (void)edges.TryPushBack(BatchEdgeChange{unit.previous, unit.target});
    }

    return Result<void>::Success();
}

} // namespace rocks::reflection_text::detail
