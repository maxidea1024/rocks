#include "ObjectRegistryInternal.h"

#include <Object/ObjectBatchPublish.h>

// ObjectRegistry 묶음 게시 구간이다(리플렉션 B9i2 — Docs/Design/ReflectionExtensions.ko.md 10.2절).
// MutationStamp, BeginBatchPublish(검사·관찰자 예약 — 실패는 모두 여기서), 구간 안 게시(무할당·
// 무실패 — 계약 위반은 Fatal), End(관찰자 기록·표지 증가), PreparedReferenceArray 순서다.
namespace rocks {

using namespace registry_detail;

namespace {

constexpr uint32 BatchTallyTag = 0x42544c59u; // 'BTLY'

// 간선 검사합의 항이다(null은 0 — 그대로 남는 원소는 더하고 빼져 0이 된다).
uint64 EdgeTerm(ObjectHandle handle) noexcept {
    return handle.IsNull() ? 0 : Hash<ObjectHandle>{}(handle);
}

// ObjectRefArray 옛 원소에 value가 있는지다(PublishBatchTarget의 retained — 재확인에 실패한
// 원소만 찾는다).
bool ArrayHolds(const void* storage, ObjectHandle value) noexcept {
    const auto& old = *static_cast<const detail::ObjectRefArrayStorage*>(storage);
    for (uint32 index = 0; index < old.size; ++index) {
        if (old.data[index] == value) {
            return true;
        }
    }

    return false;
}

} // namespace

Result<uint64> ObjectRegistry::MutationStamp(const ObjectAccessContext& access,
                                             ObjectHandle owner) const noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<uint64>::Failure(valid.Error());
    }

    platform::LockGuard lock(gate_);
    if (auto open = OpenState(closed_, processing_); !open) {
        return Result<uint64>::Failure(open.Error());
    }

    Slot* slot = FindSlot(owner);
    if (auto live = CheckLive(slot, nullptr); !live) {
        return Result<uint64>::Failure(live.Error());
    }

    return Result<uint64>::Success(slot->mutations);
}

Result<ObjectBatchPublish>
ObjectRegistry::BeginBatchPublish(const ObjectAccessContext& access, ObjectHandle owner,
                                  uint64 expectedStamp, Span<const BatchTargetCheck> targets,
                                  Span<const BatchEdgeChange> edges) noexcept {
    using Outcome = Result<ObjectBatchPublish>;
    auto valid = CheckAccess(access);
    if (!valid) {
        return Outcome::Failure(valid.Error());
    }

    bool observe = false;
    {
        platform::LockGuard lock(gate_);
        if (auto open = OpenState(closed_, processing_); !open) {
            return Outcome::Failure(open.Error());
        }

        Slot* source = FindSlot(owner);
        if (!source || !source->object || IsCutoff(source) || source->state != SlotState::Alive) {
            return Outcome::Failure(ErrorCode::StaleHandle);
        }

        // 게시는 owner 저장소를 바꾸므로 호출자가 Borrow를 쥐어 수집·회수를 막고 있어야 한다.
        if (source->borrows == 0) {
            return Outcome::Failure(ErrorCode::InvalidArgument);
        }

        if (source->encodePins != 0 || source->mutations != expectedStamp) {
            return Outcome::Failure(ErrorCode::Busy);
        }

        uint64 declared = 0;
        for (const BatchEdgeChange& edge : edges) {
            if (!edge.removed.IsNull()) {
                const Slot* removed = FindSlot(edge.removed);
                if (!removed || !(removed->state == SlotState::Alive ||
                                  removed->state == SlotState::DestroyRequested)) {
                    return Outcome::Failure(ErrorCode::InvalidArgument);
                }
            }

            declared += EdgeTerm(edge.added) - EdgeTerm(edge.removed);
        }

        for (const BatchTargetCheck& check : targets) {
            if (check.target.IsNull()) {
                return Outcome::Failure(ErrorCode::InvalidArgument);
            }

            if (auto checked = CheckTargetLocked(check.target, 0, check.nativeType); !checked) {
                return Outcome::Failure(checked.Error());
            }
        }

        for (const BatchEdgeChange& edge : edges) {
            if (!edge.added.IsNull()) {
                if (auto checked = CheckTargetLocked(edge.added, 0, nullptr); !checked) {
                    return Outcome::Failure(checked.Error());
                }
            }
        }

        // 여기부터 처리 표시 아래다: 다른 변경·GC 단계·Discard가 Busy라 위 판정이 유지된다.
        observe = ObservesOwnerLocked(*source) && !edges.IsEmpty();
        processing_ = true;
        batchActive_ = true;
        batchPublished_ = false;
        batchOwner_ = owner;
        batchDeclared_ = declared;
        batchApplied_ = 0;
    }

    if (observe) {
        ErrorCode failure = ErrorCode::None;
        auto memory = allocator_.Allocate(sizeof(EdgeTally), alignof(EdgeTally), BatchTallyTag);
        if (!memory) {
            failure = memory.Error();
        } else {
            batchTally_ = ::new (memory.Value()) EdgeTally(allocator_);
            // 쌍의 패키지는 잠금 안에서, 합치기(할당)는 잠금 밖에서 한다.
            for (const BatchEdgeChange& edge : edges) {
                EdgePlan plan;
                {
                    platform::LockGuard lock(gate_);
                    plan = PlanEditLocked(*FindSlot(owner), edge.removed, edge.added);
                }

                for (uint32 index = 0; index < plan.count && failure == ErrorCode::None; ++index) {
                    const EdgePlan::Change& change = plan.changes[index];
                    if (auto added = batchTally_->Add(change.owner, change.target, change.delta);
                        !added) {
                        failure = added.Error();
                    }
                }

                if (failure != ErrorCode::None) {
                    break;
                }
            }

            if (failure == ErrorCode::None) {
                if (auto reserved = ReserveTally(*batchTally_, ReferenceChangeCause::Edit, 0);
                    !reserved) {
                    failure = reserved.Error();
                } else {
                    platform::LockGuard lock(gate_);
                    if (MissingKeysLocked(*batchTally_, ReferenceChangeCause::Edit, 0) >
                        ObserverFreeLocked()) {
                        failure = ErrorCode::Busy;
                    }
                }
            }
        }

        if (failure != ErrorCode::None) {
            // 처리 표시 아래에서 집계를 반환한 뒤 구간을 거둔다(아무것도 바뀌지 않았다).
            if (batchTally_ != nullptr) {
                batchTally_->~EdgeTally();
                ReleaseRegistryMemory(allocator_, batchTally_);
                batchTally_ = nullptr;
            }

            platform::LockGuard lock(gate_);
            batchActive_ = false;
            batchOwner_ = {};
            processing_ = false;
            return Outcome::Failure(failure);
        }
    }

    return Outcome::Success(ObjectBatchPublish(this, owner));
}

void ObjectRegistry::CheckBatchContainer(const void* storage, usize bytes) const noexcept {
    platform::LockGuard lock(gate_);
    const Slot* source = FindSlot(batchOwner_);
    if (!batchActive_ || source == nullptr) {
        platform::Fatal("Batch publish used outside an open batch");
    }

    const uptr start = StorageStartOf(*source);
    const uptr location = reinterpret_cast<uptr>(storage);
    const usize size = source->type.Ops().size;
    if (size < bytes || location < start || location - start > size - bytes) {
        platform::Fatal("Batch publish location is outside the owner storage");
    }
}

void ObjectRegistry::NoteBatchRemoved(ObjectHandle removed) noexcept {
    batchApplied_ -= EdgeTerm(removed);
}

void ObjectRegistry::PublishBatchTarget(ObjectHandle& value, const void* nativeType,
                                        bool (*retained)(const void*, ObjectHandle) noexcept,
                                        const void* state) noexcept {
    if (value.IsNull()) {
        // 비교·Required 검사·encode가 정규 null만 보게 한다.
        value = ObjectHandle{};
        return;
    }

    platform::LockGuard lock(gate_);
    if (!batchActive_) {
        platform::Fatal("Batch publish target was not revalidated by BeginBatchPublish");
    }

    if (!CheckTargetLocked(value, 0, nativeType)) {
        // 컨테이너 교체에서 옛 내용에 있던 대상은 이미 게시된 간선이라 다시 확인하지 않는다
        // (리플렉션 B9i5 — Discard된 대상이 남아도 다른 원소의 변경을 막지 않는다). 새로 들어오는
        // 대상만 BeginBatchPublish가 재확인했어야 한다. 장벽도 필요 없다(이미 owner의 간선이다).
        if (retained == nullptr || !retained(state, value)) {
            platform::Fatal("Batch publish target was not revalidated by BeginBatchPublish");
        }

        batchApplied_ += EdgeTerm(value);
        batchPublished_ = true;
        return;
    }

    // 삽입 장벽: Marking 중이면 미리 잡은 GC 작업 용량으로 shade한다(모자라면 불변식 위반).
    if (ShadeLocked(value) != ErrorCode::None) {
        platform::Fatal("Batch publish barrier exceeded the reserved GC work capacity");
    }

    batchApplied_ += EdgeTerm(value);
    batchPublished_ = true;
}

void ObjectRegistry::PublishBatchEdge(ObjectHandle& field, ObjectHandle target,
                                      const void* nativeType,
                                      const detail::StructElementLocation* element) noexcept {
    const uptr location = reinterpret_cast<uptr>(&field);
    if (element != nullptr) {
        const detail::StructRefArrayStorage* storage = element->storage;
        CheckBatchContainer(storage, sizeof(*storage));
        bool inside = element->index < storage->size;
        if (inside) {
            const uptr block = reinterpret_cast<uptr>(storage->blocks[element->index]);
            const usize bytes = storage->element->size;
            inside = bytes >= sizeof(ObjectHandle) && location >= block &&
                     location - block <= bytes - sizeof(ObjectHandle);
        }

        if (!inside) {
            platform::Fatal("Batch publish element field is outside the element block");
        }
    } else {
        CheckBatchContainer(&field, sizeof(field));
    }

    NoteBatchRemoved(field);
    ObjectHandle value = target;
    PublishBatchTarget(value, nativeType);
    batchPublished_ = true;
    field = value;
}

void ObjectRegistry::PublishBatchValue(void* location, usize bytes,
                                       const detail::StructElementLocation* element) noexcept {
    if (element != nullptr) {
        const detail::StructRefArrayStorage* storage = element->storage;
        CheckBatchContainer(storage, sizeof(*storage));
        bool inside = element->index < storage->size;
        if (inside) {
            const uptr block = reinterpret_cast<uptr>(storage->blocks[element->index]);
            const uptr start = reinterpret_cast<uptr>(location);
            const usize size = storage->element->size;
            inside = bytes <= size && start >= block && start - block <= size - bytes;
        }

        if (!inside) {
            platform::Fatal("Batch publish element value is outside the element block");
        }
    } else {
        CheckBatchContainer(location, bytes);
    }

    batchPublished_ = true;
}

void ObjectRegistry::PublishBatchArray(detail::ObjectRefArrayStorage& storage,
                                       detail::ObjectRefArrayStorage& prepared,
                                       const void* nativeType) noexcept {
    CheckBatchContainer(&storage, sizeof(storage));
    if (storage.allocator != prepared.allocator) {
        platform::Fatal("Batch array replacement needs the array allocator");
    }

    for (uint32 index = 0; index < storage.size; ++index) {
        NoteBatchRemoved(storage.data[index]);
    }

    // 옛 원소는 맞바꾸기 전까지 storage에 그대로 있다.
    for (uint32 index = 0; index < prepared.size; ++index) {
        PublishBatchTarget(prepared.data[index], nativeType, &ArrayHolds, &storage);
    }

    batchPublished_ = true;
    const detail::ObjectRefArrayStorage previous = storage;
    storage.data = prepared.data;
    storage.size = prepared.size;
    storage.capacity = prepared.capacity;
    prepared.data = previous.data;
    prepared.size = previous.size;
    prepared.capacity = previous.capacity;
}

void ObjectRegistry::EndBatchPublish(ObjectHandle owner) noexcept {
    EdgeTally* tally = nullptr;
    {
        platform::LockGuard lock(gate_);
        if (!batchActive_ || !(batchOwner_ == owner)) {
            platform::Fatal("Batch publish closed twice or by another owner");
        }

        tally = batchTally_;
        if (batchPublished_) {
            // 선언한 간선 변경과 게시한 간선 변경이 같아야 관찰자 예약·기록이 맞다.
            if (batchApplied_ != batchDeclared_) {
                platform::Fatal("Batch publish edges differ from the declared edge changes");
            }

            if (tally != nullptr) {
                ApplyTallyLocked(*tally, ReferenceChangeCause::Edit, 0);
            }

            ++FindSlot(owner)->mutations;
        }

        batchTally_ = nullptr;
    }

    // 집계 반환은 잠금 밖·처리 표시 아래다(allocator 콜백의 재진입은 Busy).
    if (tally != nullptr) {
        tally->~EdgeTally();
        ReleaseRegistryMemory(allocator_, tally);
    }

    platform::LockGuard lock(gate_);
    batchActive_ = false;
    batchPublished_ = false;
    batchOwner_ = {};
    processing_ = false;
}

void ObjectBatchPublish::End() noexcept {
    if (registry_ != nullptr) {
        ObjectRegistry* registry = registry_;
        registry_ = nullptr;
        registry->EndBatchPublish(owner_);
        owner_ = {};
    }
}

ObjectRegistry& ObjectBatchPublish::Registry() const noexcept {
    if (registry_ == nullptr) {
        platform::Fatal("Batch publish used after End");
    }

    return *registry_;
}

PreparedReferenceArray::~PreparedReferenceArray() {
    ReleaseContainerMemory(*storage_.allocator, storage_.data);
}

Result<void> PreparedReferenceArray::TryAssign(Span<const ObjectHandle> elements) noexcept {
    if (elements.Size() > NumericLimits<uint32>::Max()) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    const auto count = static_cast<uint32>(elements.Size());
    ObjectHandle* data = nullptr;
    if (count != 0) {
        auto memory =
            storage_.allocator->Allocate(sizeof(ObjectHandle) * count, alignof(ObjectHandle));
        if (!memory) {
            return Result<void>::Failure(memory.Error());
        }

        data = static_cast<ObjectHandle*>(memory.Value());
        for (uint32 index = 0; index < count; ++index) {
            data[index] = elements[index].IsNull() ? ObjectHandle{} : elements[index];
        }
    }

    ReleaseContainerMemory(*storage_.allocator, storage_.data);
    storage_.data = data;
    storage_.size = count;
    storage_.capacity = count;
    return Result<void>::Success();
}

} // namespace rocks
