#include "ObjectRegistryGcInternal.h"

#include <Base/Compiler.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>

// ObjectRegistry GC의 sweep 뒤 처리다. Retire와 회수 대상 목록(Pending·Deferred·Ready), 파괴 요청
// 목록(TakeDestroyRequests), 회수(ReclaimEligible·DrainRetired)를 둔다.
namespace rocks {

using namespace registry_detail;

bool ObjectRegistry::IsReclaimable(const Slot& slot) const noexcept {
    // 고리에 남은 자식이 있으면 부모 슬롯을 재사용하지 않는다. white 부모가 자식보다 먼저
    // Retire되면 자식들이 같은 sweep에서 스스로 떨어질 때까지 기다린다(그 사이 DrainRetired 회수
    // 포함 — rollback은 저널의 슬롯만 회수하고, 그 자식은 같은 저널의 뒤쪽이라 먼저 떨어진다).
    return slot.state == SlotState::Retired && slot.roots == 0 && slot.borrows == 0 &&
           ColdOf(slot).backendHolds == 0 && slot.encodePins == 0 &&
           ColdOf(slot).firstChild == nullptr;
}

ObjectRegistry::Slot* ObjectRegistry::NextReclaimableLocked() noexcept {
    // 지연 목록의 슬롯은 회수할 수 없다는 불변식이 있다(회수 가능해지는 순간 WakeDeferredLocked가
    // 풀린 목록으로 옮긴다). 그래서 지연 슬롯은 다시 걷지 않는다.
    if (retiredReady_.head != nullptr) {
        ++reclaimProbes_;
        return retiredReady_.head;
    }

    while (Slot* slot = retiredPending_.head) {
        ++reclaimProbes_;
        if (IsReclaimable(*slot)) {
            return slot;
        }

        UnlinkRetiredLocked(*slot);
        LinkRetiredLocked(*slot, RetiredQueue::Deferred);
    }

    return nullptr;
}

bool ObjectRegistry::HasReclaimableLocked() noexcept {
    return NextReclaimableLocked() != nullptr;
}

void ObjectRegistry::LinkRetiredLocked(Slot& slot, RetiredQueue queue) noexcept {
    RetiredList& list = queue == RetiredQueue::Pending    ? retiredPending_
                        : queue == RetiredQueue::Deferred ? retiredDeferred_
                                                          : retiredReady_;
    slot.queue = queue;
    ColdOf(slot).nextInList = nullptr;
    ColdOf(slot).prevInList = list.tail;
    if (list.tail != nullptr) {
        ColdOf(*list.tail).nextInList = &slot;
    } else {
        list.head = &slot;
    }

    list.tail = &slot;
}

void ObjectRegistry::UnlinkRetiredLocked(Slot& slot) noexcept {
    RetiredList& list = slot.queue == RetiredQueue::Pending    ? retiredPending_
                        : slot.queue == RetiredQueue::Deferred ? retiredDeferred_
                                                               : retiredReady_;
    if (ColdOf(slot).prevInList != nullptr) {
        ColdOf(*ColdOf(slot).prevInList).nextInList = ColdOf(slot).nextInList;
    } else {
        list.head = ColdOf(slot).nextInList;
    }

    if (ColdOf(slot).nextInList != nullptr) {
        ColdOf(*ColdOf(slot).nextInList).prevInList = ColdOf(slot).prevInList;
    } else {
        list.tail = ColdOf(slot).prevInList;
    }

    slot.queue = RetiredQueue::None;
    ColdOf(slot).nextInList = nullptr;
    ColdOf(slot).prevInList = nullptr;
}

void ObjectRegistry::RetireLocked(Slot& slot) noexcept {
    // 패키지 정책 Registry의 객체는 Discard로만 죽는다(1.7절). 패키지 정책에서는 RequestDestroy가
    // 없으므로 Alive는 Discard되지 않았다는 뜻이다. 종료(closed_) 뒤의 회수는 세지 않는다.
    if (slot.state == SlotState::Alive && options_.policy.packageNativeType != nullptr &&
        !closed_) {
        if (tally_.retiredWithoutDiscard != NumericLimits<uint32>::Max()) {
            ++tally_.retiredWithoutDiscard;
        }

        if constexpr (IsDebugBuild) {
            if (options_.assertRetiredWithoutDiscard) {
                platform::Fatal("Package-policy object retired without Discard");
            }
        }
    }

    if constexpr (IsDebugBuild) {
        // 소속·detached는 sweep 후보가 아니다(소속은 clustered 비트, detached는 그 클러스터 처리가
        // 표식하거나 cutoff 해체가 비운다).
        if (clustersEnabled_ && ClusterStateOf(slot.index) != 0) {
            platform::Fatal("Retiring an object that is still in a cluster ledger");
        }
    }

    if (ColdOf(slot).destroyListed) {
        UnlinkDestroyRequestLocked(slot);
    }

    slot.SetState(tally_, SlotState::Retired);
    BitsOf(slot.index).live &= ~BitOf(slot.index);
    DetachIdentityLocked(slot);
    LinkRetiredLocked(slot, RetiredQueue::Pending);
}

void ObjectRegistry::LinkDestroyRequestLocked(Slot& slot) noexcept {
    ColdOf(slot).prevInList = nullptr;
    ColdOf(slot).nextInList = destroyRequests_;
    if (destroyRequests_ != nullptr) {
        ColdOf(*destroyRequests_).prevInList = &slot;
    }

    destroyRequests_ = &slot;
    ColdOf(slot).destroyListed = true;
}

void ObjectRegistry::UnlinkDestroyRequestLocked(Slot& slot) noexcept {
    if (ColdOf(slot).prevInList != nullptr) {
        ColdOf(*ColdOf(slot).prevInList).nextInList = ColdOf(slot).nextInList;
    } else {
        destroyRequests_ = ColdOf(slot).nextInList;
    }

    if (ColdOf(slot).nextInList != nullptr) {
        ColdOf(*ColdOf(slot).nextInList).prevInList = ColdOf(slot).prevInList;
    }

    ColdOf(slot).nextInList = nullptr;
    ColdOf(slot).prevInList = nullptr;
    ColdOf(slot).destroyListed = false;
}

Result<usize> ObjectRegistry::TakeDestroyRequests(const ObjectAccessContext& access,
                                                  Span<ObjectHandle> out) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<usize>::Failure(valid.Error());
    }

    platform::LockGuard lock(gate_);
    if (closed_ || processing_) {
        return Result<usize>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }

    usize taken = 0;
    while (taken < out.Size() && destroyRequests_ != nullptr) {
        Slot& slot = *destroyRequests_;
        UnlinkDestroyRequestLocked(slot);
        out[taken++] = HandleOf(slot);
    }

    return Result<usize>::Success(taken);
}

void ObjectRegistry::WakeDeferredLocked(Slot& slot) noexcept {
    if (slot.queue == RetiredQueue::Deferred && IsReclaimable(slot)) {
        UnlinkRetiredLocked(slot);
        LinkRetiredLocked(slot, RetiredQueue::Ready);
    }
}

ObjectRegistry::ReclaimWork ObjectRegistry::TakeForReclaimLocked(Slot& slot) noexcept {
    if (slot.indexed || slot.linked) {
        platform::Fatal("Reclaiming an object still in the name index or child ring");
    }

    if (slot.queue != RetiredQueue::None) {
        platform::Fatal("Reclaiming an object still in a retired queue");
    }

    // 저장소를 목록에서 분리한 뒤 잠금을 푼다. 콜백 중 통계 조회는 가능하지만 재회수는 불가하다.
    // 지역 TypeLease가 소멸자 코드까지 살아 있게 하므로 해제 콜백이 끝난 뒤 반환한다. 이름 기록도
    // 잠금 밖에서 반환한다(조회는 잠금 안에서 복사해 두었다).
    slot.SetState(tally_, SlotState::Reclaiming);
    ReclaimWork work;
    work.slot = &slot;
    work.type = Move(slot.type);
    work.object = slot.object;
    work.storage = ColdOf(slot).storage;
    work.record = ColdOf(slot).name;
    ColdOf(slot).name = nullptr;
    return work;
}

void ObjectRegistry::FinishReclaim(ReclaimWork& work) noexcept {
    // User destructors and allocator callbacks never execute under the registry gate.
    work.type.Ops().destroy(work.object);
    auto released = allocator_.Deallocate(work.storage);
    if (!released) {
        platform::Fatal("Object allocation ownership mismatch");
    }

    ReleaseRegistryMemory(allocator_, work.record);
    {
        platform::LockGuard lock(gate_);
        Slot& slot = *work.slot;
        slot.object = nullptr;
        ColdOf(slot).storage = nullptr;
        ColdOf(slot).transaction = 0;
        ColdOf(slot).stableType = {};
        ColdOf(slot).outer = {};
        // GC 밀집 자료를 비운다(live는 Retire 때 이미 꺼졌다).
        HotOf(slot.index) = GcHot{};
        GcBits& bits = BitsOf(slot.index);
        bits.leaf &= ~BitOf(slot.index);
        StoreMark(bits.mark, LoadMark(bits.mark) & ~BitOf(slot.index));
        StoreMark(bits.clusterSeen, LoadMark(bits.clusterSeen) & ~BitOf(slot.index));
        if (clustersEnabled_) {
            ClearClusterSlotLocked(slot.index);
        }

        ColdOf(slot).outermost = nullptr;
        ColdOf(slot).parent = nullptr;
        ColdOf(slot).lastChild = nullptr;
        ColdOf(slot).pathBytes = 0;
        ColdOf(slot).discarded = false;
        // 세대를 0/과거 값으로 순환시키면 오래된 WeakRef가 새 객체를 가리킬 수 있다.
        // 한도에 도달한 슬롯은 재사용하지 않고 Exhausted로 남긴다.
        if (slot.generation >= options_.maxGeneration) {
            slot.SetState(tally_, SlotState::Exhausted);
        } else {
            ++slot.generation;
            slot.SetState(tally_, SlotState::Free);
            ColdOf(slot).nextInList = freeSlots_;
            freeSlots_ = &slot;
        }
    }

    work.type.Reset();
}

uint32 ObjectRegistry::ReclaimEligible(uint32 maximum, uint64 deadline) noexcept {
    // 회수 사이에 잠금을 풀고 사용자 소멸자를 부른다. 그 사이 소멸자(또는 다른 스레드)가 지연
    // 슬롯의 보유 계수를 반환하면 그 슬롯이 풀린 목록으로 옮겨져 다음 차례에 회수된다(목록을
    // 처음부터 다시 걷지 않는다).
    uint32 reclaimed = 0;
    while (reclaimed < maximum && (deadline == 0 || platform::MonotonicNanoseconds() < deadline)) {
        ReclaimWork work;
        {
            platform::LockGuard lock(gate_);
            Slot* selected = NextReclaimableLocked();
            if (selected == nullptr) {
                break;
            }

            UnlinkRetiredLocked(*selected);
            work = TakeForReclaimLocked(*selected);
        }

        FinishReclaim(work);
        ++reclaimed;
    }

    return reclaimed;
}

Result<uint32> ObjectRegistry::DrainRetired(const ObjectAccessContext& access) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<uint32>::Failure(valid.Error());
    }

    {
        platform::LockGuard lock(gate_);
        if (processing_) {
            return Result<uint32>::Failure(ErrorCode::Busy);
        }

        processing_ = true;
    }

    uint32 count = ReclaimEligible();
    {
        platform::LockGuard lock(gate_);
        processing_ = false;
    }

    return Result<uint32>::Success(count);
}

} // namespace rocks
