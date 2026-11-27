#include "ObjectRegistryInternal.h"

#include <Object/FunctionCall.h>

// 실행 중 필드 변경 통지다(Docs/Design/ChangeNotification.ko.md 3.1절 — CN1): 객체별 지연 상태
// (notifyStates_ — 슬롯 index → NotifyState, 슬롯 hot 레코드의 notified bit), 구독·해제, 동기 통지
// (깊이 상한·발행 중 정리 지연), 파괴 요청·회수 때 정리. 상태와 bit는 소유 스레드만 읽고 바꾸므로
// 통지 빠른 경로는 잠금을 쥐지 않는다(CN-D12 — 슬롯·chunk 구조와 그 flag byte도 소유 스레드만
// 바꾼다). 계수(구독 객체 수·버린 통지 수)는 Stats가 다른 스레드에서 읽으므로 잠금 안에서 바꾼다.
//
// 재진입: delegate를 지우거나 비우면 포획한 값의 소멸자가 돈다(Core MulticastDelegate가 제거 도중에
// 소멸한다). 그 소멸자가 같은 객체의 다른 구독을 해제할 수 있으므로, 상태를 바꾸는 동안 active를
// 올려 안쪽 정리가 목록을 옮기거나 상태를 반환하지 않게 하고, 지울 목록은 상태 갱신이 끝난 뒤
// 지역 배열에서 파괴한다.
//
// 쓰기 경로의 통지(CN1b — CN-D10): Registry 참조·컨테이너 API가 성공한 뒤 받은 멤버 주소를 담은
// Notify 필드를 찾아 알리고(NotifyMember — 구독자가 있는 owner만 storage thunk로 잰다), 통지 묶음
// (NotifyGroup)이 열려 있으면 (객체, 번호)마다 한 번으로 모았다가 가장 바깥 묶음을 닫을 때 낸다.
namespace rocks {

using namespace registry_detail;

namespace {

constexpr uint32 NotifyBitsPerWord = 64;

bool TestNotifyBit(const Array<uint64>& bits, uint32 index) noexcept {
    const uint32 word = index / NotifyBitsPerWord;
    return word < bits.Size() && (bits[word] & (uint64{1} << (index % NotifyBitsPerWord))) != 0;
}

void SetNotifyBit(Array<uint64>& bits, uint32 index, bool on) noexcept {
    const uint32 word = index / NotifyBitsPerWord;
    const uint64 mask = uint64{1} << (index % NotifyBitsPerWord);
    bits[word] = on ? (bits[word] | mask) : (bits[word] & ~mask);
}

} // namespace

Result<ObjectRegistry::NotifyState*> ObjectRegistry::EnsureNotifyState(Slot& slot) noexcept {
    if (UniquePtr<NotifyState>* found = notifyStates_.Find(slot.index)) {
        return Result<NotifyState*>::Success(found->Get());
    }

    auto made = UniquePtr<NotifyState>::TryMake(allocator_, allocator_);
    if (!made) {
        return Result<NotifyState*>::Failure(made.Error());
    }

    const uint32 words = (slot.type.NotifyCount() + NotifyBitsPerWord - 1) / NotifyBitsPerWord;
    if (auto sized = made.Value()->bits.TryResize(words); !sized) {
        return Result<NotifyState*>::Failure(sized.Error());
    }

    for (uint64& word : made.Value()->bits) {
        word = 0;
    }

    made.Value()->allSerial = ++notifySerial_;
    NotifyState* state = made.Value().Get();
    if (auto inserted = notifyStates_.Insert(slot.index, Move(made.Value())); !inserted) {
        return Result<NotifyState*>::Failure(inserted.Error());
    }

    slot.notified = true;
    platform::LockGuard lock(gate_);
    ++notifiedObjects_;
    return Result<NotifyState*>::Success(state);
}

void ObjectRegistry::EraseNotifyState(uint32 slotIndex) noexcept {
    UniquePtr<NotifyState> removed;
    if (UniquePtr<NotifyState>* found = notifyStates_.Find(slotIndex)) {
        removed = Move(*found);
        (void)notifyStates_.Erase(slotIndex);
    }

    if (!removed) {
        return;
    }

    if (Slot* slot = SlotAt(slotIndex)) {
        slot->notified = false;
    }

    {
        platform::LockGuard lock(gate_);
        --notifiedObjects_;
    }

    // delegate 소멸(포획한 값의 소멸자)은 잠금 밖에서, 표에서 뗀 뒤에 한다 — 그 소멸자의 재진입
    // 호출은 이 객체의 상태를 찾지 못한다.
    removed.Reset();
}

void ObjectRegistry::ClearNotifyLists(NotifyState& state) noexcept {
    ++state.active;
    for (NotifyState::List& list : state.lists) {
        list.delegate->Clear();
    }

    state.all.Clear();
    --state.active;
}

void ObjectRegistry::ReleaseNotifyState(uint32 slotIndex) noexcept {
    UniquePtr<NotifyState>* found = notifyStates_.Find(slotIndex);
    if (!found) {
        return;
    }

    NotifyState& state = **found;
    if (state.active != 0) {
        // 발행·정리 중인 delegate는 옮기거나 파괴할 수 없다 — 비우기만 하고 반환은 가장 바깥 통지
        // 뒤로(파괴 경로만 released를 세운다 — CN-D11).
        ClearNotifyLists(state);
        state.released = true;
        return;
    }

    EraseNotifyState(slotIndex);
}

void ObjectRegistry::TidyNotifyState(uint32 slotIndex, NotifyState& state) noexcept {
    if (state.active != 0) {
        return;
    }

    if (state.released) {
        EraseNotifyState(slotIndex);
        return;
    }

    // 빈 목록을 지역 배열로 옮기고 그 번호의 bit를 내린다(순서 보존). 옮길 공간이 없으면 이번에는
    // 지우지 않는다(다음 정리가 한다 — 빈 목록은 통지에 해가 없다).
    // 빈 목록이 없으면 예약하지 않는다(통지마다 할당하지 않게 — CN-D12 bench). 생존 판정은 다른
    // 스레드에서 바뀔 수 있으므로(약한 대상) 보는 단계와 옮기는 단계 사이에 빈 목록이 늘 수 있다 —
    // 있으면 목록 수만큼 예약한다.
    bool anyDead = false;
    for (const NotifyState::List& list : state.lists) {
        if (!list.delegate->IsBound()) {
            anyDead = true;
            break;
        }
    }

    Array<NotifyState::List> dead(allocator_);
    if (anyDead && dead.TryReserve(state.lists.Size())) {
        usize kept = 0;
        for (usize index = 0; index < state.lists.Size(); ++index) {
            NotifyState::List& list = state.lists[index];
            if (list.delegate->IsBound()) {
                if (kept != index) {
                    state.lists[kept] = Move(list);
                }

                ++kept;
                continue;
            }

            SetNotifyBit(state.bits, list.index, false);
            if (!dead.TryPushBack(Move(list))) {
                platform::Fatal("Reserved notify list push failed");
            }
        }

        // 줄이기는 실패하지 않는다.
        (void)state.lists.TryResize(kept);
    }

    if (state.lists.IsEmpty() && !state.all.IsBound()) {
        EraseNotifyState(slotIndex);
    }

    // dead의 소멸(남은 죽은 항목의 소멸자)은 상태를 다 바꾼 뒤다 — 재진입 호출이 일관된 상태를
    // 본다.
}

Result<ObjectRegistry::Slot*> ObjectRegistry::NotifySlot(const ObjectAccessContext& access,
                                                         ObjectHandle object,
                                                         bool requireAlive) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<Slot*>::Failure(valid.Error());
    }

    if (closed_) {
        return Result<Slot*>::Failure(ErrorCode::ShuttingDown);
    }

    if (processing_ || batchActive_) {
        return Result<Slot*>::Failure(ErrorCode::Busy);
    }

    Slot* slot = FindSlot(object);
    if (requireAlive) {
        if (auto live = CheckLive(slot, nullptr); !live) {
            return Result<Slot*>::Failure(live.Error());
        }
    } else if (!slot || slot->state == SlotState::Free || slot->state == SlotState::Exhausted ||
               slot->state == SlotState::Reserved) {
        return Result<Slot*>::Failure(ErrorCode::StaleHandle);
    }

    return Result<Slot*>::Success(slot);
}

Result<FieldNotifyHandle> ObjectRegistry::TrySubscribe(const ObjectAccessContext& access,
                                                       ObjectHandle object, NotifyTarget target,
                                                       FieldChangeDelegate&& delegate) noexcept {
    auto found = NotifySlot(access, object, true);
    if (!found) {
        return Result<FieldNotifyHandle>::Failure(found.Error());
    }

    Slot& slot = *found.Value();
    const uint32 index = slot.type.NotifyIndex(target.id);
    if (index == NotifyIndexNone) {
        // 타입(기반 체인 포함)의 필드·함수인데 Notify가 아니면 InvalidArgument, 없는 ID면
        // NotFound다.
        const bool member = slot.type.FieldIndex(target.id) != TypeLease::NoField ||
                            types_.FindFunction(slot.type.Id(), target.id).IsOk();
        return Result<FieldNotifyHandle>::Failure(member ? ErrorCode::InvalidArgument
                                                         : ErrorCode::NotFound);
    }

    auto ensured = EnsureNotifyState(slot);
    if (!ensured) {
        return Result<FieldNotifyHandle>::Failure(ensured.Error());
    }

    NotifyState& state = *ensured.Value();
    usize position = 0;
    while (position < state.lists.Size() && state.lists[position].index < index) {
        ++position;
    }

    if (position == state.lists.Size() || state.lists[position].index != index) {
        auto made = UniquePtr<NotifyState::Multicast>::TryMake(allocator_, allocator_);
        if (!made) {
            TidyNotifyState(slot.index, state);
            return Result<FieldNotifyHandle>::Failure(made.Error());
        }

        if (auto inserted = state.lists.TryInsert(
                NotifyState::List{index, ++notifySerial_, Move(made.Value())}, position);
            !inserted) {
            TidyNotifyState(slot.index, state);
            return Result<FieldNotifyHandle>::Failure(inserted.Error());
        }
    }

    NotifyState::List& list = state.lists[position];
    auto added = list.delegate->TryAdd(Move(delegate));
    if (!added) {
        TidyNotifyState(slot.index, state);
        return Result<FieldNotifyHandle>::Failure(added.Error());
    }

    SetNotifyBit(state.bits, index, true);
    return Result<FieldNotifyHandle>::Success(
        FieldNotifyHandle{object, index, added.Value(), list.serial});
}

Result<FieldNotifyHandle> ObjectRegistry::TrySubscribeAll(const ObjectAccessContext& access,
                                                          ObjectHandle object,
                                                          FieldChangeDelegate&& delegate) noexcept {
    auto found = NotifySlot(access, object, true);
    if (!found) {
        return Result<FieldNotifyHandle>::Failure(found.Error());
    }

    Slot& slot = *found.Value();
    if (slot.type.NotifyCount() == 0) {
        return Result<FieldNotifyHandle>::Failure(ErrorCode::InvalidArgument);
    }

    auto ensured = EnsureNotifyState(slot);
    if (!ensured) {
        return Result<FieldNotifyHandle>::Failure(ensured.Error());
    }

    NotifyState& state = *ensured.Value();
    auto added = state.all.TryAdd(Move(delegate));
    if (!added) {
        TidyNotifyState(slot.index, state);
        return Result<FieldNotifyHandle>::Failure(added.Error());
    }

    return Result<FieldNotifyHandle>::Success(
        FieldNotifyHandle{object, FieldNotifyHandle::AllTargets, added.Value(), state.allSerial});
}

bool ObjectRegistry::Unsubscribe(const ObjectAccessContext& access,
                                 const FieldNotifyHandle& handle) noexcept {
    if (!handle.IsValid() || !CheckAccess(access)) {
        return false;
    }

    Slot* slot = FindSlot(handle.object);
    if (!slot || !slot->notified) {
        return false;
    }

    UniquePtr<NotifyState>* found = notifyStates_.Find(slot->index);
    if (!found) {
        return false;
    }

    // 일련번호가 다르면 그 목록(또는 상태)은 handle을 받은 뒤 사라졌다가 다시 생긴 것이다 — 같은
    // DelegateHandle 값이 남의 새 구독일 수 있다(ABA).
    NotifyState& state = **found;
    NotifyState::Multicast* owner = nullptr;
    if (handle.target == FieldNotifyHandle::AllTargets) {
        owner = handle.serial == state.allSerial ? &state.all : nullptr;
    } else {
        for (NotifyState::List& list : state.lists) {
            if (list.index == handle.target) {
                owner = list.serial == handle.serial ? list.delegate.Get() : nullptr;
                break;
            }
        }
    }

    if (owner == nullptr) {
        return false;
    }

    ++state.active;
    const bool removed = owner->Remove(handle.delegate);
    --state.active;
    TidyNotifyState(slot->index, state);
    return removed;
}

Result<usize> ObjectRegistry::UnsubscribeAll(const ObjectAccessContext& access,
                                             ObjectHandle object) noexcept {
    auto found = NotifySlot(access, object, false);
    if (!found) {
        return Result<usize>::Failure(found.Error());
    }

    Slot& slot = *found.Value();
    UniquePtr<NotifyState>* entry = slot.notified ? notifyStates_.Find(slot.index) : nullptr;
    if (!entry) {
        return Result<usize>::Success(0);
    }

    // 살아 있는 객체의 상태는 비우기만 한다(released는 파괴 경로만 — 같은 통지 안의 새 구독이
    // 사라지지 않게). 빈 상태는 정리가 반환한다.
    NotifyState& state = **entry;
    usize removed = state.all.Stats().bound;
    for (NotifyState::List& list : state.lists) {
        removed += list.delegate->Stats().bound;
    }

    ClearNotifyLists(state);
    TidyNotifyState(slot.index, state);
    return Result<usize>::Success(removed);
}

Result<usize> ObjectRegistry::CompactNotifications(const ObjectAccessContext& access,
                                                   ObjectHandle object) noexcept {
    auto found = NotifySlot(access, object, false);
    if (!found) {
        return Result<usize>::Failure(found.Error());
    }

    Slot& slot = *found.Value();
    UniquePtr<NotifyState>* entry = slot.notified ? notifyStates_.Find(slot.index) : nullptr;
    if (!entry) {
        return Result<usize>::Success(0);
    }

    NotifyState& state = **entry;
    if (state.active != 0) {
        return Result<usize>::Failure(ErrorCode::Busy);
    }

    ++state.active;
    usize removed = state.all.CompactDead();
    for (NotifyState::List& list : state.lists) {
        removed += list.delegate->CompactDead();
    }

    --state.active;
    TidyNotifyState(slot.index, state);
    return Result<usize>::Success(removed);
}

Result<void> ObjectRegistry::Notify(const ObjectAccessContext& access, ObjectHandle object,
                                    NotifyTarget target) noexcept {
    return NotifyChecked(access, object, target.id, NotifyIndexNone);
}

Result<void> ObjectRegistry::NotifyIndex(const ObjectAccessContext& access, ObjectHandle object,
                                         uint32 index) noexcept {
    return NotifyChecked(access, object, Guid{}, index);
}

Result<void> ObjectRegistry::NotifyChecked(const ObjectAccessContext& access, ObjectHandle object,
                                           Guid targetId, uint32 index) noexcept {
    auto found = NotifySlot(access, object, false);
    if (!found) {
        return Result<void>::Failure(found.Error());
    }

    // 빠른 경로: 구독자가 없는 객체는 bit 하나로 끝난다(CN-D12).
    Slot& slot = *found.Value();
    if (!slot.notified) {
        return Result<void>::Success();
    }

    if (index == NotifyIndexNone) {
        index = slot.type.NotifyIndex(targetId);
        if (index == NotifyIndexNone) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    } else if (index >= slot.type.NotifyCount()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    UniquePtr<NotifyState>* entry = notifyStates_.Find(slot.index);
    if (!entry) {
        return Result<void>::Success();
    }

    NotifyState& state = **entry;
    NotifyState::Multicast* targeted = nullptr;
    if (TestNotifyBit(state.bits, index)) {
        for (NotifyState::List& list : state.lists) {
            if (list.index == index) {
                targeted = list.delegate.Get();
                break;
            }
        }
    }

    if (targeted == nullptr && !state.all.IsBound()) {
        return Result<void>::Success();
    }

    // 통지 묶음 안이면 모았다가 가장 바깥 묶음을 닫을 때 낸다(CN-D10). 모을 자리를 할당하지 못하면
    // 바로 낸다(중복은 "바뀌었을 수 있음"에 맞다).
    if (notifyGroupDepth_ != 0 && DeferNotify(object, slot.index, index)) {
        return Result<void>::Success();
    }

    // 통지 동안 Borrow로 붙잡아 회수되지 않게 한다(구독자의 Collect는 Busy — CN-D6). 살아 있지 않은
    // 객체(파괴 요청·Discard·도달 불가)는 부르지 않고 성공이다(CN-D11 — 쓰기의 결과가 구독 유무로
    // 갈리지 않게).
    auto borrow = Resolve<Object>(access, object);
    if (!borrow) {
        return borrow.Error() == ErrorCode::Destroyed ? Result<void>::Success()
                                                      : Result<void>::Failure(borrow.Error());
    }

    if (notifyDepth_ >= MaxNotifyDepth) {
        platform::LockGuard lock(gate_);
        ++droppedNotifications_;
        return Result<void>::Success();
    }

    const Guid id = slot.type.NotifyTarget(index);
    ++notifyDepth_;
    ++state.active;
    if (targeted != nullptr) {
        targeted->Broadcast(object, id);
    }

    state.all.Broadcast(object, id);
    --state.active;
    --notifyDepth_;
    TidyNotifyState(slot.index, state);
    return Result<void>::Success();
}

Result<void> ObjectRegistry::NotifyIfTarget(const ObjectAccessContext& access, ObjectHandle object,
                                            Guid id) noexcept {
    auto found = NotifySlot(access, object, false);
    if (!found) {
        return Result<void>::Failure(found.Error());
    }

    Slot& slot = *found.Value();
    if (!slot.notified) {
        return Result<void>::Success();
    }

    const uint32 index = slot.type.NotifyIndex(id);
    if (index == NotifyIndexNone) {
        return Result<void>::Success();
    }

    return NotifyChecked(access, object, Guid{}, index);
}

bool ObjectRegistry::NotifyWatched(ObjectHandle object) const noexcept {
    const Slot* slot = FindSlot(object);
    return slot && slot->notified;
}

void ObjectRegistry::NotifyMember(const ObjectAccessContext& access, ObjectHandle owner,
                                  const void* member) noexcept {
    // 빠른 경로: 구독자가 없는 owner는 bit 하나로 끝난다(CN-D12). 호출한 API가 접근 검사를 마쳤다.
    Slot* slot = FindSlot(owner);
    if (!slot || !slot->notified) {
        return;
    }

    // 살아 있지 않은 owner(제거·비우기는 DestroyRequested도 받는다)는 알리지 않는다(CN-D11).
    auto borrow = Resolve<Object>(access, owner);
    if (!borrow) {
        return;
    }

    auto object = borrow.Value().Get();
    if (!object) {
        return;
    }

    const TypeLease& type = slot->type;
    const auto fields = type.Schema().fields;
    // 서로 다른 객체의 포인터를 관계 비교하지 않도록 주소 값으로 비교한다.
    const uptr at = reinterpret_cast<uptr>(member);
    for (usize index = 0; index < fields.Size(); ++index) {
        if (!HasFlag(fields[index].flags, FieldFlags::Notify)) {
            continue;
        }

        const RuntimeFieldOps* ops = type.FieldOps(index);
        if (!ops || !ops->storage) {
            continue;
        }

        const Span<const uint8> range = ops->storage(*object.Value());
        const uptr begin = reinterpret_cast<uptr>(range.Data());
        if (at >= begin && at - begin < range.Size()) {
            // 쓰기는 이미 성공했다 — 통지의 실패(접근·종료는 호출한 API가 먼저 본다)는 쓰기 결과를
            // 바꾸지 않는다.
            (void)NotifyChecked(access, owner, Guid{}, type.NotifyIndex(fields[index].id));
            return;
        }
    }
}

bool ObjectRegistry::DeferNotify(ObjectHandle object, uint32 slotIndex, uint32 index) noexcept {
    // (슬롯, 번호) 색인으로 묶음이 커도 모으기가 상수 시간이다. 같은 슬롯을 다른 세대가 다시 쓰면
    // (묶음 안의 회수·재사용) 색인을 새 handle로 바꾸고 따로 모은다.
    const uint64 key = (static_cast<uint64>(slotIndex) << 32) | index;
    auto found = notifyPendingIndex_.FindOrAdd(key, object);
    if (!found) {
        return false;
    }

    if (found.Value().outcome == InsertOutcome::AlreadyPresent) {
        if (*found.Value().value == object) {
            return true;
        }

        *found.Value().value = object;
    }

    // 목록에 넣지 못하면 색인을 비워 둔다(다음 같은 통지가 "이미 모음"으로 사라지지 않게).
    ObjectHandle& indexed = *found.Value().value;
    if (!notifyPending_.TryPushBack(PendingNotify{object, index})) {
        indexed = ObjectHandle{};
        return false;
    }

    return true;
}

Result<NotifyGroup> ObjectRegistry::BeginNotifyGroup(const ObjectAccessContext& access) noexcept {
    if (auto valid = CheckAccess(access); !valid) {
        return Result<NotifyGroup>::Failure(valid.Error());
    }

    if (closed_) {
        return Result<NotifyGroup>::Failure(ErrorCode::ShuttingDown);
    }

    if (notifyGroupDepth_ == NumericLimits<uint32>::Max()) {
        return Result<NotifyGroup>::Failure(ErrorCode::Overflow);
    }

    ++notifyGroupDepth_;
    return Result<NotifyGroup>::Success(NotifyGroup(this, access));
}

void ObjectRegistry::EndNotifyGroup(const ObjectAccessContext& access) noexcept {
    if (!CheckAccess(access)) {
        platform::Fatal("NotifyGroup was closed outside its registry owner thread or context");
    }

    if (--notifyGroupDepth_ != 0) {
        return;
    }

    // 처리 표시·묶음 게시 구간 안에서는 통지가 Busy라 모은 통지가 사라진다 — 계약 위반이다(묶음은
    // 그 구간 밖에서 열고 닫는다).
    if (!notifyPending_.IsEmpty() && (processing_ || batchActive_)) {
        platform::Fatal("NotifyGroup was closed inside registry processing or a batch publish");
    }

    notifyPendingIndex_.Clear();
    // 부르는 동안 구독자가 새 묶음을 열고 닫을 수 있으므로 모은 목록을 지역으로 옮긴 뒤 낸다(그
    // 뒤의 통지는 묶음 밖이라 바로 나간다). 끝나면 비운 버퍼를 돌려 다음 묶음이 다시 쓴다.
    Array<PendingNotify> pending(Move(notifyPending_));
    for (const PendingNotify& item : pending) {
        // 모은 뒤 파괴 요청·회수된 객체는 부르지 않는다(Destroyed는 Success, 회수는 StaleHandle).
        (void)NotifyChecked(access, item.object, Guid{}, item.index);
    }

    pending.Clear();
    if (notifyPending_.IsEmpty() && notifyPending_.Capacity() < pending.Capacity()) {
        notifyPending_ = Move(pending);
    }
}

void NotifyGroup::End() noexcept {
    if (!registry_) {
        return;
    }

    ObjectRegistry* registry = registry_;
    registry_ = nullptr;
    registry->EndNotifyGroup(access_);
}

} // namespace rocks
