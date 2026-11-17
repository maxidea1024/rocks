#include "ObjectRegistryInternal.h"

#include <Object/ObjectRegistry.h>
#include <Platform/Fatal.h>

// ObjectRegistry 쪽 ReferenceObserver 기록과 Discard다(Docs/Design/ObjectFoundations.ko.md
// 1.7·3절). 패키지 쌍 기록 계획(EdgePlan)·집계(EdgeTally)·키 예약·적용 도우미와 부분 트리
// Discard를 둔다.
namespace rocks {

using namespace registry_detail;

namespace {

// 관찰자가 없는 Registry의 남은 새 키 공간이다(제한 없음 — 기록하지 않으므로 예약도 없다).
constexpr usize UnlimitedObserverKeys = MaxSize;

} // namespace

ObjectHandle ObjectRegistry::PackageOfLocked(ObjectHandle target) const noexcept {
    // 관찰 대상 간선의 대상은 회수 전이다: 대상을 쓰는 지점이 생존(또는 같은 트랜잭션) 대상만 받고,
    // 소유자가 cutoff 아닌 생존 객체인 동안 그 대상은 표식되어 inner → outer 간선으로 Outer
    // 사슬까지 회수되지 않는다(그래서 outermost 슬롯도 유효하다). 대상의 GC 색은 보지 않는다.
    const Slot* slot = FindSlot(target);
    if (slot == nullptr ||
        !(slot->state == SlotState::Reserved || slot->state == SlotState::Alive ||
          slot->state == SlotState::DestroyRequested) ||
        slot->outermost == nullptr) {
        platform::Fatal("Observed reference target lost its package");
    }

    return HandleOf(*slot->outermost);
}

bool ObjectRegistry::ObservesOwnerLocked(const Slot& owner) const noexcept {
    // 패키지 객체(Outer 없음)가 소유한 간선과 DestroyRequested 소유자의 제거·비우기(Discard가 이미
    // 반납했다)는 기록하지 않는다.
    return options_.referenceObserver != nullptr && owner.state == SlotState::Alive &&
           !owner.outer.IsNull();
}

ObjectRegistry::EdgePlan ObjectRegistry::PlanEditLocked(const Slot& owner, ObjectHandle removed,
                                                        ObjectHandle added) const noexcept {
    EdgePlan plan;
    if (!ObservesOwnerLocked(owner)) {
        return plan;
    }

    const ObjectHandle package = HandleOf(*owner.outermost);
    const bool hasRemoved = !removed.IsNull();
    const bool hasAdded = !added.IsNull();
    const ObjectHandle removedPackage = hasRemoved ? PackageOfLocked(removed) : ObjectHandle{};
    const ObjectHandle addedPackage = hasAdded ? PackageOfLocked(added) : ObjectHandle{};
    if (hasRemoved && hasAdded && removedPackage == addedPackage) {
        return plan;
    }

    if (hasRemoved && !(removedPackage == package)) {
        plan.changes[plan.count++] = {package, removedPackage, -1};
    }

    if (hasAdded && !(addedPackage == package)) {
        plan.changes[plan.count++] = {package, addedPackage, 1};
    }

    return plan;
}

usize ObjectRegistry::MissingKeysLocked(const EdgePlan& plan) const noexcept {
    usize missing = 0;
    for (uint32 index = 0; index < plan.count; ++index) {
        const EdgePlan::Change& change = plan.changes[index];
        if (!options_.referenceObserver->Contains(change.owner, change.target,
                                                  ReferenceChangeCause::Edit, 0)) {
            ++missing;
        }
    }

    return missing;
}

usize ObjectRegistry::MissingKeysLocked(const EdgeTally& tally, ReferenceChangeCause cause,
                                        uint64 transaction) const noexcept {
    usize missing = 0;
    for (const EdgeTally::Pair& pair : tally.pairs) {
        if (!options_.referenceObserver->Contains(pair.owner, pair.target, cause, transaction)) {
            ++missing;
        }
    }

    return missing;
}

usize ObjectRegistry::ObserverFreeLocked() const noexcept {
    return options_.referenceObserver != nullptr ? options_.referenceObserver->FreeKeys()
                                                 : UnlimitedObserverKeys;
}

void ObjectRegistry::ApplyPlanLocked(const EdgePlan& plan) noexcept {
    for (uint32 index = 0; index < plan.count; ++index) {
        const EdgePlan::Change& change = plan.changes[index];
        options_.referenceObserver->Add(change.owner, change.target, change.delta,
                                        ReferenceChangeCause::Edit, 0);
    }
}

void ObjectRegistry::ApplyTallyLocked(const EdgeTally& tally, ReferenceChangeCause cause,
                                      uint64 transaction) noexcept {
    for (const EdgeTally::Pair& pair : tally.pairs) {
        options_.referenceObserver->Add(pair.owner, pair.target, pair.delta, cause, transaction);
    }
}

Result<void> ObjectRegistry::ReserveTally(const EdgeTally& tally, ReferenceChangeCause cause,
                                          uint64 transaction) noexcept {
    if (tally.pairs.IsEmpty()) {
        return Result<void>::Success();
    }

    usize missing = 0;
    usize free = 0;
    {
        platform::LockGuard lock(gate_);
        missing = MissingKeysLocked(tally, cause, transaction);
        free = ObserverFreeLocked();
    }

    return missing <= free ? Result<void>::Success() : options_.referenceObserver->Reserve(missing);
}

void ObjectRegistry::TallyEdge(void* pointer, ObjectHandle handle) noexcept {
    auto& state = *static_cast<MarkState*>(pointer);
    if (handle.IsNull() || state.error != ErrorCode::None) {
        return;
    }

    ObjectHandle package;
    {
        platform::LockGuard lock(state.registry->gate_);
        package = state.registry->PackageOfLocked(handle);
    }

    if (package == state.ownerPackage) {
        return;
    }

    if (auto added = state.tally->Add(state.ownerPackage, package, state.delta); !added) {
        state.error = added.Error();
    }
}

Result<void> ObjectRegistry::TallyHandles(const ObjectHandle* handles, usize count,
                                          ObjectHandle ownerPackage, EdgeTally& tally) noexcept {
    MarkState state{this, nullptr};
    state.tally = &tally;
    state.ownerPackage = ownerPackage;
    state.delta = -1;
    for (usize index = 0; index < count && state.error == ErrorCode::None; ++index) {
        TallyEdge(&state, handles[index]);
    }

    return state.error == ErrorCode::None ? Result<void>::Success()
                                          : Result<void>::Failure(state.error);
}

Result<void> ObjectRegistry::CollectStructEdges(const detail::StructElementOps& element,
                                                void* const* blocks, uint32 count,
                                                Array<ObjectHandle>& out) noexcept {
    // 떼어 낼 원소만 trace한다(소유자 Trace는 부르지 않는다). trace 칸은 원소 표에 없으므로 등록
    // struct의 실행 정보로 얻는다(StructRefArray.h — 생성 헤더 없는 곳의 인스턴스화 문제). 관찰자
    // 집계와 클러스터 장부가 이 결과를 같이 쓴다(원소마다 trace 한 번 — 13.4절: 소속 소유자의
    // 원소를 지울 때도 원소 Trace가 반드시 있어야 한다).
    auto lease = types_.AcquireStructByNative(element.nativeType);
    if (!lease) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    struct Collected {
        Array<ObjectHandle>* out;
        ErrorCode error;
    };

    Collected collected{&out, ErrorCode::None};
    auto visit = [](void* pointer, ObjectHandle handle) noexcept {
        auto& state = *static_cast<Collected*>(pointer);
        if (!handle.IsNull() && state.error == ErrorCode::None) {
            if (auto pushed = state.out->TryPushBack(handle); !pushed) {
                state.error = pushed.Error();
            }
        }
    };
    const auto trace = lease.Value().StructOps().trace;
    TraceVisitor visitor(&collected, +visit);
    for (uint32 index = 0; trace != nullptr && index < count && collected.error == ErrorCode::None;
         ++index) {
        trace(blocks[index], visitor);
    }

    return collected.error == ErrorCode::None ? Result<void>::Success()
                                              : Result<void>::Failure(collected.error);
}

Result<void> ObjectRegistry::EndProcessing(Result<void> result) noexcept {
    platform::LockGuard lock(gate_);
    processing_ = false;
    return result;
}

Result<void> ObjectRegistry::Discard(const ObjectAccessContext& access,
                                     ObjectHandle handle) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    // 부분 트리 전위 순회의 다음 노드다(스택 없이 부모 포인터로 되돌아 오른다). 고리로 닿는 객체만
    // 부분 트리다 — 이미 Discard·Retire된 객체는 고리에서 빠져 있다.
    auto next = [](const Slot* node, const Slot* root) noexcept -> Slot* {
        if (node->firstChild != nullptr) {
            return node->firstChild;
        }

        while (node != root && node->nextSibling == nullptr) {
            node = node->parent;
        }

        return node == root ? nullptr : node->nextSibling;
    };
    Slot* root = nullptr;
    usize count = 0;
    {
        platform::LockGuard lock(gate_);
        if (auto open = OpenState(closed_, processing_); !open) {
            return open;
        }

        root = FindSlot(handle);
        if (!root || root->state == SlotState::Free || root->state == SlotState::Exhausted ||
            root->state == SlotState::Reserved) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }

        // 이미 Discard된 객체는 무동작이다(cutoff·Retire 뒤에도 — RequestDestroy 재요청과 같다).
        if (root->discarded) {
            return Result<void>::Success();
        }

        if (IsCutoff(root) ||
            !(root->state == SlotState::Alive || root->state == SlotState::DestroyRequested)) {
            return Result<void>::Failure(ErrorCode::Destroyed);
        }

        // (1) 부분 트리를 센다. 공개 전 객체가 있으면 그 트랜잭션의 Commit이 Outer 검증으로 실패할
        // 상태를 만들지 않도록 Busy다.
        for (const Slot* node = root; node != nullptr; node = next(node, root)) {
            if (node->state == SlotState::Reserved) {
                return Result<void>::Failure(ErrorCode::Busy);
            }

            // 영구 객체는 Shutdown 때만 해체한다(13.7절).
            if (IsPermanentLocked(node->index)) {
                return Result<void>::Failure(ErrorCode::Unsupported);
            }

            ++count;
        }

        // (2) 처리 표시 아래 잠금 밖에서 할당한다. 그동안 다른 변경은 Busy라 (1)과 (3) 사이에 부분
        // 트리가 바뀌지 않는다.
        processing_ = true;
    }

    Array<Slot*> nodes(allocator_);
    if (auto reserved = nodes.TryReserve(count); !reserved) {
        return EndProcessing(Move(reserved));
    }

    {
        platform::LockGuard lock(gate_);
        for (Slot* node = root; node != nullptr; node = next(node, root)) {
            if (nodes.Size() == count || !nodes.TryPushBack(node)) {
                platform::Fatal("Discard subtree changed while processing");
            }
        }
    }

    // 관찰자: 부분 트리의 Alive·Outer 있는 소유자마다 타입 Trace로 다른 패키지로 나가는 강한 간선을
    // 쌍별로 합친다(목록 순 × trace 순 — handle 해시 순서를 쓰지 않는다). 대상의 GC 색과 무관하다.
    EdgeTally tally(allocator_);
    if (options_.referenceObserver != nullptr) {
        MarkState state{this, nullptr};
        state.tally = &tally;
        state.delta = -1;
        TraceVisitor visitor(&state, &ObjectRegistry::TallyEdge);
        for (usize position = 0; position < nodes.Size() && state.error == ErrorCode::None;
             ++position) {
            const Slot& node = *nodes[position];
            bool observe = false;
            {
                platform::LockGuard lock(gate_);
                observe = ObservesOwnerLocked(node);
                if (observe) {
                    state.ownerPackage = HandleOf(*node.outermost);
                }
            }

            if (observe) {
                node.type.Ops().trace(*node.object, visitor);
            }
        }

        if (state.error != ErrorCode::None) {
            return EndProcessing(Result<void>::Failure(state.error));
        }

        if (auto reserved = ReserveTally(tally, ReferenceChangeCause::Discard, 0); !reserved) {
            return EndProcessing(Move(reserved));
        }
    }

    // (3) 한 잠금 안에서 상태·색인·고리·관찰자 기록을 바꾼다. 부분 트리 안의 순서는 의미가 없다.
    bool leaves = false;
    {
        platform::LockGuard lock(gate_);
        processing_ = false;
        if (options_.referenceObserver != nullptr &&
            MissingKeysLocked(tally, ReferenceChangeCause::Discard, 0) > ObserverFreeLocked()) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        for (Slot* node : nodes) {
            if (node->state == SlotState::Alive) {
                node->SetState(tally_, SlotState::DestroyRequested);
            }

            node->discarded = true;
            DetachIdentityLocked(*node);
        }

        if (options_.referenceObserver != nullptr) {
            ApplyTallyLocked(tally, ReferenceChangeCause::Discard, 0);
        }

        // 클러스터(13.2·13.4절): 부분 트리의 루트는 클러스터를 해체하고, 남은 소속은 하나씩
        // 이탈한다(순서와 무관하게 결과가 같다). 이탈 Trace는 잠금 밖에서 하므로 처리 표시를 다시
        // 세운다(공개된 변경은 이미 끝났다 — 이 뒤로는 실패하지 않는다).
        if (clustersEnabled_) {
            for (Slot* node : nodes) {
                if (IsClusterMemberLocked(*node)) {
                    const uint32 id = CellOf(node->index).state & ClusterIdMask;
                    if (clusters_[id].root == node->index) {
                        DissolveClusterLocked(id);
                    }
                }
            }

            for (Slot* node : nodes) {
                leaves = leaves || IsClusterMemberLocked(*node);
            }

            processing_ = leaves;
        }
    }

    if (leaves) {
        for (Slot* node : nodes) {
            bool member = false;
            {
                platform::LockGuard lock(gate_);
                member = IsClusterMemberLocked(*node);
            }

            if (member) {
                LeaveTrace(*node);
                platform::LockGuard lock(gate_);
                FinishLeaveLocked(*node);
            }
        }

        platform::LockGuard lock(gate_);
        processing_ = false;
    }

    return Result<void>::Success();
}

} // namespace rocks
