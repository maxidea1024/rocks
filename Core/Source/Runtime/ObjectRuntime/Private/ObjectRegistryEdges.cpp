#include "ObjectRegistryInternal.h"

#include <Object/ObjectRegistry.h>
#include <Platform/Fatal.h>

// ObjectRegistry 참조 간선 쓰기다. 단일 참조(SetEdge)와 ObjectRefArray·StructRefArray 컨테이너
// 편집, owner·대상 검증을 둔다. 쓰기는 삽입 장벽(ShadeLocked — ObjectRegistryGc.cpp)과 관찰자
// 기록 계획·예약(ObjectRegistryObserver.cpp)을 거친다.
namespace rocks {

using namespace registry_detail;

namespace {

constexpr uint32 ObjectRefArrayTag = 0x4f524146u;   // 'ORAF'
constexpr uint32 StructRefElementTag = 0x53524145u; // 'SRAE'
constexpr uint32 StructRefIndexTag = 0x53524149u;   // 'SRAI'

// 컨테이너 버퍼의 다음 용량이다(4부터 두 배, uint32 상한에서 포화).
uint32 GrownCapacity(uint32 capacity) noexcept {
    constexpr uint32 Maximum = NumericLimits<uint32>::Max();
    return capacity < 4u ? 4u : capacity > Maximum / 2 ? Maximum : capacity * 2u;
}

void ReleaseContainerMemory(Allocator& allocator, void* memory) noexcept {
    if (memory != nullptr && !allocator.Deallocate(memory)) {
        platform::Fatal("Registry container deallocation failed");
    }
}

// StructRefArray 원소 하나를 소멸하고 블록을 반환한다.
void DestroyStructElement(Allocator& allocator, const detail::StructElementOps& ops,
                          void* block) noexcept {
    ops.destroy(block);
    ReleaseContainerMemory(allocator, block);
}

} // namespace

Result<void> ObjectRegistry::CheckContainerOwnerLocked(ObjectHandle owner, const void* storage,
                                                       usize storageBytes, uint64 transaction,
                                                       bool allowDestroyRequested) noexcept {
    Slot* source = FindSlot(owner);
    if (!source || !source->object || IsCutoff(source)) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (transaction != 0) {
        // staged: 같은 생성 트랜잭션의 Reserved 객체다(Reserved는 Borrow를 얻을 수 없다).
        if (source->state != SlotState::Reserved || source->transaction != transaction) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }
    } else {
        if (!(source->state == SlotState::Alive ||
              (allowDestroyRequested && source->state == SlotState::DestroyRequested))) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }

        // 컨테이너는 owner 객체 안의 멤버이므로 호출자는 owner의 Borrow를 쥔 채 호출해야 한다.
        // Borrow가 있는 동안은 수집이 Busy이고 회수도 일어나지 않으므로, 잠금 밖 할당·원소 생성 중
        // allocator·생성자가 Registry에 재진입해도 owner 저장소가 사라지지 않는다.
        if (source->borrows == 0) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    // 컨테이너 객체는 owner 저장소 안에 직접 포함되어야 한다(SetEdge의 필드 위치 검사와 같다).
    const uptr start = reinterpret_cast<uptr>(source->storage);
    const uptr location = reinterpret_cast<uptr>(storage);
    const usize size = source->type.Ops().size;
    if (size < storageBytes || location < start || location - start > size - storageBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // encode 중인 owner의 버퍼를 바꾸지 않는다(PinForEncode). 호출 오류(InvalidArgument)를 가리지
    // 않게 마지막에 본다. Reserved 객체는 고정할 수 없다.
    if (source->encodePins != 0) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    return Result<void>::Success();
}

Result<void> ObjectRegistry::CheckTargetLocked(ObjectHandle target, uint64 transaction,
                                               const void* nativeType) const noexcept {
    Slot* destination = FindSlot(target);
    if (!destination || IsCutoff(destination) ||
        !(destination->state == SlotState::Alive ||
          (transaction && destination->state == SlotState::Reserved &&
           destination->transaction == transaction))) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (nativeType && !destination->type.IsNativeA(nativeType)) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    return Result<void>::Success();
}

Result<void> ObjectRegistry::SetEdge(const ObjectAccessContext& access, ObjectHandle owner,
                                     ObjectHandle& field, ObjectHandle target, uint64 transaction,
                                     const void* nativeType,
                                     const detail::StructElementLocation* element) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    const uptr location = reinterpret_cast<uptr>(&field);
    // 원소 모드에서 첫 시도 때 본 컨테이너 변경 계수다. 예약 콜백이 원소를 지우고 다시 만들면 같은
    // 블록 주소가 재사용되어 위치 검사를 통과할 수 있으므로 재시도는 계수로 변경을 가려 Busy다.
    uint32 revision = 0;
    // 검증 → (live·관찰자) 기록 계획 → 공간이 있으면 shade·게시·기록이다. 관찰자 공간이 모자라면
    // 잠금을 풀고 예약한 뒤 처음부터 한 번 더 한다(그래도 모자라면 Busy — 단일 간선 지점의 컨테이너
    // 성장 규칙, Docs/Design/ObjectFoundations.ko.md 3절).
    for (uint32 attempt = 0;; ++attempt) {
        usize reserve = 0;
        {
            platform::LockGuard lock(gate_);
            if (processing_ || closed_) {
                return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
            }

            if (element) {
                // 원소 모드: 컨테이너가 owner 저장소 안, index < size, field가 그 원소 블록
                // 안(O(1)). 컨테이너 위치가 owner 저장소 안임을 확인하기 전에는 storage를
                // 역참조하지 않는다.
                const detail::StructRefArrayStorage* storage = element->storage;
                auto checked =
                    CheckContainerOwnerLocked(owner, storage, sizeof(*storage), transaction, false);
                if (!checked) {
                    return checked;
                }

                if (attempt != 0 && storage->revision != revision) {
                    return Result<void>::Failure(ErrorCode::Busy);
                }

                revision = storage->revision;
                if (element->index >= storage->size) {
                    return Result<void>::Failure(attempt == 0 ? ErrorCode::InvalidArgument
                                                              : ErrorCode::Busy);
                }

                const uptr block = reinterpret_cast<uptr>(storage->blocks[element->index]);
                const usize bytes = storage->element->size;
                if (bytes < sizeof(ObjectHandle) || location < block ||
                    location - block > bytes - sizeof(ObjectHandle)) {
                    return Result<void>::Failure(attempt == 0 ? ErrorCode::InvalidArgument
                                                              : ErrorCode::Busy);
                }
            } else {
                Slot* source = FindSlot(owner);
                if (!source || !source->object || IsCutoff(source) ||
                    (transaction ? source->state != SlotState::Reserved ||
                                       source->transaction != transaction
                                 : source->state != SlotState::Alive)) {
                    return Result<void>::Failure(ErrorCode::StaleHandle);
                }

                const uptr start = reinterpret_cast<uptr>(source->storage);
                const usize size = source->type.Ops().size;
                if (location < start || location - start > size - sizeof(ObjectHandle)) {
                    return Result<void>::Failure(ErrorCode::InvalidArgument);
                }

                // 위치 오류를 가리지 않게 마지막에 본다(PinForEncode).
                if (source->encodePins != 0) {
                    return Result<void>::Failure(ErrorCode::Busy);
                }
            }

            if (!target.IsNull()) {
                auto targetChecked = CheckTargetLocked(target, transaction, nativeType);
                if (!targetChecked) {
                    return targetChecked;
                }
            }

            // staged 쓰기는 기록하지 않는다(Commit이 한꺼번에 보고한다).
            const EdgePlan plan =
                transaction == 0 ? PlanEditLocked(*FindSlot(owner), field, target) : EdgePlan{};
            const usize missing = plan.count != 0 ? MissingKeysLocked(plan) : 0;
            if (missing <= ObserverFreeLocked()) {
                if (target.IsNull()) {
                    // 비교·Required 검사·encode가 정규 null만 보게 한다.
                    field = ObjectHandle{};
                } else {
                    // staged 내부 간선은 Commit에서 전체 게시·shade한다. 이미 게시된 대상은 즉시
                    // 보존한다.
                    if (FindSlot(target)->state == SlotState::Alive) {
                        const ErrorCode shaded = ShadeLocked(target);
                        if (shaded != ErrorCode::None) {
                            return Result<void>::Failure(shaded);
                        }
                    }

                    field = target;
                }

                ApplyPlanLocked(plan);
                return Result<void>::Success();
            }

            if (attempt != 0) {
                return Result<void>::Failure(ErrorCode::Busy);
            }

            reserve = missing;
        }

        // 관찰자 allocator 콜백은 잠금 밖에서 실행한다. 콜백이 같은 field·컨테이너를 바꿀 수 있어
        // 다시 잠근 뒤 검증부터 다시 한다(owner Borrow가 수집·회수를 막는다 — 원소 모드).
        if (auto reserved = options_.referenceObserver->Reserve(reserve); !reserved) {
            return reserved;
        }
    }
}

Result<void> ObjectRegistry::InsertArrayEdge(const ObjectAccessContext& access, ObjectHandle owner,
                                             detail::ObjectRefArrayStorage& storage, usize index,
                                             bool append, ObjectHandle target,
                                             const void* nativeType) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    if (target.IsNull()) {
        target = ObjectHandle{};
    }

    // owner 검증 뒤에만 부른다(배열이 owner 저장소 안임을 확인한 뒤 size를 읽는다).
    auto positionLocked = [&]() noexcept -> usize { return append ? storage.size : index; };
    Allocator& allocator = *storage.allocator;
    // owner·대상 검증(잠금 보유). 할당 전과 할당 뒤에 같은 검사를 하고, 그 사이 index를 본다.
    auto ownerLocked = [&]() noexcept -> Result<void> {
        if (processing_ || closed_) {
            return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
        }

        return CheckContainerOwnerLocked(owner, &storage, sizeof(storage), 0, false);
    };
    auto targetLocked = [&]() noexcept -> Result<void> {
        return target.IsNull() ? Result<void>::Success() : CheckTargetLocked(target, 0, nativeType);
    };
    // position에 target을 넣어 게시한다(잠금 보유, 검증 뒤). buffer는 size + 1을 담는 버퍼(기존
    // 또는 새 것)이고, 새 버퍼로 바꾸면 잠금 밖에서 반환할 옛 버퍼를 돌려준다. shade 실패면
    // 불변이다.
    auto publishLocked = [&](usize position, ObjectHandle* buffer,
                             uint32 capacity) noexcept -> Result<ObjectHandle*> {
        if (!target.IsNull()) {
            const ErrorCode shaded = ShadeLocked(target);
            if (shaded != ErrorCode::None) {
                return Result<ObjectHandle*>::Failure(shaded);
            }
        }

        ObjectHandle* previous = nullptr;
        if (buffer != storage.data) {
            for (usize at = 0; at < position; ++at) {
                buffer[at] = storage.data[at];
            }

            for (usize at = storage.size; at > position; --at) {
                buffer[at] = storage.data[at - 1];
            }

            previous = storage.data;
            storage.data = buffer;
            storage.capacity = capacity;
        } else {
            for (usize at = storage.size; at > position; --at) {
                buffer[at] = buffer[at - 1];
            }
        }

        storage.data[position] = target;
        ++storage.size;
        return Result<ObjectHandle*>::Success(previous);
    };

    uint32 grown = 0;
    usize reserve = 0;
    {
        platform::LockGuard lock(gate_);
        // 용량을 늘리기 전에 검증해 잘못된 호출이 할당을 일으키지 않게 한다.
        auto checked = ownerLocked();
        if (!checked) {
            return checked;
        }

        const usize position = positionLocked();
        if (position > storage.size) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        checked = targetLocked();
        if (!checked) {
            return checked;
        }

        if (storage.size == NumericLimits<uint32>::Max()) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        // 관찰자 기록(새 키 ≤ 1)은 버퍼와 같은 규칙이다: 공간이 모자라면 잠금 밖에서 예약한다.
        const EdgePlan plan = PlanEditLocked(*FindSlot(owner), ObjectHandle{}, target);
        const usize missing = plan.count != 0 ? MissingKeysLocked(plan) : 0;
        const bool keysReady = missing <= ObserverFreeLocked();
        if (storage.size < storage.capacity && keysReady) {
            auto published = publishLocked(position, storage.data, storage.capacity);
            if (!published) {
                return Result<void>::Failure(published.Error());
            }

            ApplyPlanLocked(plan);
            return Result<void>::Success();
        }

        grown = storage.size < storage.capacity ? 0 : GrownCapacity(storage.capacity);
        reserve = keysReady ? 0 : missing;
    }

    // allocator callback은 Registry 잠금 밖에서 실행하며, 이 구간에는 할당(관찰자 예약 포함)만
    // 한다. 콜백이 같은 배열을 바꿀 수 있으므로 복사·삽입·게시는 다시 잠근 뒤 그때의 size·index로
    // 한다. 레지스트리 전체 처리 표시(processing_)는 세우지 않는다(흔한 런타임 목록 변경이 다른
    // 스레드를 막지 않게). owner Borrow가 수집·회수를 막으므로 owner와 배열은 살아 있다.
    if (reserve != 0) {
        if (auto reserved = options_.referenceObserver->Reserve(reserve); !reserved) {
            return reserved;
        }
    }

    ObjectHandle* fresh = nullptr;
    if (grown != 0) {
        auto memory = allocator.Allocate(sizeof(ObjectHandle) * grown, alignof(ObjectHandle),
                                         ObjectRefArrayTag);
        if (!memory) {
            return Result<void>::Failure(memory.Error());
        }

        fresh = static_cast<ObjectHandle*>(memory.Value());
    }

    ObjectHandle* release = fresh;
    Result<void> result = Result<void>::Success();
    {
        platform::LockGuard lock(gate_);
        result = ownerLocked();
        const usize position = result ? positionLocked() : 0;
        if (result && position > storage.size) {
            // 콜백이 원소를 지워 index가 지금 size를 넘는다.
            result = Result<void>::Failure(ErrorCode::Busy);
        }

        if (result) {
            result = targetLocked();
        }

        EdgePlan plan;
        if (result) {
            plan = PlanEditLocked(*FindSlot(owner), ObjectHandle{}, target);
            if (plan.count != 0 && MissingKeysLocked(plan) > ObserverFreeLocked()) {
                // 콜백의 기록이 예약한 공간을 다시 써 버렸다.
                result = Result<void>::Failure(ErrorCode::Busy);
            }
        }

        if (result) {
            if (storage.size == NumericLimits<uint32>::Max()) {
                result = Result<void>::Failure(ErrorCode::Overflow);
            } else if (storage.size < storage.capacity) {
                // 그 사이 용량이 충분해졌다(또는 관찰자 공간만 모자랐다). 기존 버퍼에 넣고 새
                // 버퍼는 반환한다.
                auto published = publishLocked(position, storage.data, storage.capacity);
                if (!published) {
                    result = Result<void>::Failure(published.Error());
                }
            } else if (fresh != nullptr && grown > storage.size) {
                auto published = publishLocked(position, fresh, grown);
                if (published) {
                    release = published.Value();
                } else {
                    result = Result<void>::Failure(published.Error());
                }
            } else {
                // 콜백이 원소를 늘려 새 버퍼가 size + 1을 담지 못한다(또는 새 버퍼가 없다).
                result = Result<void>::Failure(ErrorCode::Busy);
            }
        }

        if (result) {
            ApplyPlanLocked(plan);
        }
    }

    ReleaseContainerMemory(allocator, release);
    return result;
}

Result<void> ObjectRegistry::ReplaceArrayEdge(const ObjectAccessContext& access, ObjectHandle owner,
                                              detail::ObjectRefArrayStorage& storage, usize index,
                                              ObjectHandle target,
                                              const void* nativeType) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    // 관찰자 공간이 모자라면 잠금 밖에서 예약한 뒤 한 번 더 한다(SetEdge와 같은 규칙).
    for (uint32 attempt = 0;; ++attempt) {
        usize reserve = 0;
        {
            platform::LockGuard lock(gate_);
            if (processing_ || closed_) {
                return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
            }

            auto checked = CheckContainerOwnerLocked(owner, &storage, sizeof(storage), 0, false);
            if (!checked) {
                return checked;
            }

            if (index >= storage.size) {
                // 재시도에서는 예약 콜백이 원소를 지운 것이다.
                return Result<void>::Failure(attempt == 0 ? ErrorCode::InvalidArgument
                                                          : ErrorCode::Busy);
            }

            if (!target.IsNull()) {
                auto targetChecked = CheckTargetLocked(target, 0, nativeType);
                if (!targetChecked) {
                    return targetChecked;
                }
            }

            const EdgePlan plan = PlanEditLocked(*FindSlot(owner), storage.data[index], target);
            const usize missing = plan.count != 0 ? MissingKeysLocked(plan) : 0;
            if (missing <= ObserverFreeLocked()) {
                if (target.IsNull()) {
                    storage.data[index] = ObjectHandle{};
                } else {
                    const ErrorCode shaded = ShadeLocked(target);
                    if (shaded != ErrorCode::None) {
                        return Result<void>::Failure(shaded);
                    }

                    storage.data[index] = target;
                }

                ApplyPlanLocked(plan);
                return Result<void>::Success();
            }

            if (attempt != 0) {
                return Result<void>::Failure(ErrorCode::Busy);
            }

            reserve = missing;
        }

        if (auto reserved = options_.referenceObserver->Reserve(reserve); !reserved) {
            return reserved;
        }
    }
}

Result<void> ObjectRegistry::RemoveArrayEdge(const ObjectAccessContext& access, ObjectHandle owner,
                                             detail::ObjectRefArrayStorage& storage, usize index,
                                             ObjectHandle target, bool byHandle) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    // 원소 null은 정규 값이므로 비정규 null 인자도 첫 null 원소를 찾는다.
    if (byHandle && target.IsNull()) {
        target = ObjectHandle{};
    }

    // 관찰자 공간이 모자라면 잠금 밖에서 예약한 뒤 한 번 더 한다(SetEdge와 같은 규칙).
    for (uint32 attempt = 0;; ++attempt) {
        usize reserve = 0;
        {
            platform::LockGuard lock(gate_);
            if (processing_ || closed_) {
                return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
            }

            auto checked = CheckContainerOwnerLocked(owner, &storage, sizeof(storage), 0, true);
            if (!checked) {
                return checked;
            }

            usize position = index;
            if (byHandle) {
                position = storage.size;
                for (uint32 at = 0; at < storage.size; ++at) {
                    if (storage.data[at] == target) {
                        position = at;
                        break;
                    }
                }

                if (position == storage.size) {
                    // 재시도에서는 예약 콜백이 그 원소를 지운 것이다.
                    return Result<void>::Failure(attempt == 0 ? ErrorCode::NotFound
                                                              : ErrorCode::Busy);
                }
            } else if (position >= storage.size) {
                return Result<void>::Failure(attempt == 0 ? ErrorCode::InvalidArgument
                                                          : ErrorCode::Busy);
            }

            // DestroyRequested owner의 제거는 기록하지 않는다(PlanEditLocked — Discard가 이미
            // 반납했다).
            const EdgePlan plan =
                PlanEditLocked(*FindSlot(owner), storage.data[position], ObjectHandle{});
            const usize missing = plan.count != 0 ? MissingKeysLocked(plan) : 0;
            if (missing <= ObserverFreeLocked()) {
                // 삽입 장벽 GC에서 간선 삭제는 장벽이 필요 없다. 지운 대상은 다른 경로로 도달하지
                // 않으면 회수된다.
                for (usize at = position + 1; at < storage.size; ++at) {
                    storage.data[at - 1] = storage.data[at];
                }

                --storage.size;
                ApplyPlanLocked(plan);
                return Result<void>::Success();
            }

            if (attempt != 0) {
                return Result<void>::Failure(ErrorCode::Busy);
            }

            reserve = missing;
        }

        if (auto reserved = options_.referenceObserver->Reserve(reserve); !reserved) {
            return reserved;
        }
    }
}

Result<void> ObjectRegistry::ClearArrayEdges(const ObjectAccessContext& access, ObjectHandle owner,
                                             detail::ObjectRefArrayStorage& storage) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    ObjectHandle package;
    {
        platform::LockGuard lock(gate_);
        if (processing_ || closed_) {
            return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
        }

        auto checked = CheckContainerOwnerLocked(owner, &storage, sizeof(storage), 0, true);
        if (!checked) {
            return checked;
        }

        const Slot& source = *FindSlot(owner);
        if (!ObservesOwnerLocked(source) || storage.size == 0) {
            storage.size = 0;
            return Result<void>::Success();
        }

        // 다중 간선 지점(Docs/Design/ObjectFoundations.ko.md 3절): 처리 표시 아래 잠금 밖에서
        // 떼어 낼 값의 쌍을 합친 뒤 예약하고, 한 잠금 안에서 비우고 기록한다. 처리 표시 동안 다른
        // 변경은 Busy라 배열·owner를 다시 검증할 필요가 없다.
        package = HandleOf(*source.outermost);
        processing_ = true;
    }

    EdgeTally tally(allocator_);
    MarkState state{this, nullptr};
    state.tally = &tally;
    state.ownerPackage = package;
    state.delta = -1;
    for (uint32 position = 0; position < storage.size && state.error == ErrorCode::None;
         ++position) {
        TallyEdge(&state, storage.data[position]);
    }

    if (state.error != ErrorCode::None) {
        return EndProcessing(Result<void>::Failure(state.error));
    }

    if (auto reserved = ReserveTally(tally, ReferenceChangeCause::Edit, 0); !reserved) {
        return EndProcessing(Move(reserved));
    }

    platform::LockGuard lock(gate_);
    processing_ = false;
    if (MissingKeysLocked(tally, ReferenceChangeCause::Edit, 0) > ObserverFreeLocked()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    storage.size = 0;
    ApplyTallyLocked(tally, ReferenceChangeCause::Edit, 0);
    return Result<void>::Success();
}

Result<void> ObjectRegistry::AssignArrayEdges(const ObjectAccessContext& access, ObjectHandle owner,
                                              detail::ObjectRefArrayStorage& storage,
                                              Span<const ObjectHandle> targets, uint64 transaction,
                                              const void* nativeType) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    if (targets.Size() > NumericLimits<uint32>::Max()) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    const auto count = static_cast<uint32>(targets.Size());
    Allocator& allocator = *storage.allocator;
    // 모든 원소를 검증한다(잠금 보유). 하나라도 틀리면 불변이다.
    auto validateLocked = [&]() noexcept -> Result<void> {
        auto checked =
            CheckContainerOwnerLocked(owner, &storage, sizeof(storage), transaction, false);
        if (!checked) {
            return checked;
        }

        for (uint32 index = 0; index < count; ++index) {
            if (!targets[index].IsNull()) {
                auto targetChecked = CheckTargetLocked(targets[index], transaction, nativeType);
                if (!targetChecked) {
                    return targetChecked;
                }
            }
        }

        return Result<void>::Success();
    };
    // 생존 대상만 shade한 뒤(ShadeLocked는 Reserved에 StaleHandle을 내므로 Reserved 대상은 Commit이
    // shade한다) buffer에 정규화해 복사하고 게시한다. 새 버퍼로 바꾸면 옛 버퍼를 돌려준다.
    auto publishLocked = [&](ObjectHandle* buffer,
                             uint32 capacity) noexcept -> Result<ObjectHandle*> {
        for (uint32 index = 0; index < count; ++index) {
            const ObjectHandle target = targets[index];
            if (!target.IsNull() && FindSlot(target)->state == SlotState::Alive) {
                const ErrorCode shaded = ShadeLocked(target);
                if (shaded != ErrorCode::None) {
                    return Result<ObjectHandle*>::Failure(shaded);
                }
            }
        }

        for (uint32 index = 0; index < count; ++index) {
            buffer[index] = targets[index].IsNull() ? ObjectHandle{} : targets[index];
        }

        ObjectHandle* previous = buffer != storage.data ? storage.data : nullptr;
        storage.data = buffer;
        storage.capacity = capacity;
        storage.size = count;
        return Result<ObjectHandle*>::Success(previous);
    };

    {
        platform::LockGuard lock(gate_);
        if (closed_ || processing_ || transaction == 0) {
            return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
        }

        auto checked = validateLocked();
        if (!checked) {
            return checked;
        }

        if (count <= storage.capacity) {
            auto published = publishLocked(storage.data, storage.capacity);
            return published ? Result<void>::Success() : Result<void>::Failure(published.Error());
        }

        // 생성 트랜잭션 안의 잠금 밖 할당은 CreatePending과 같은 규칙이다: allocator 재진입은 Busy,
        // Rollback 재진입은 Fatal.
        processing_ = true;
    }

    auto finish = [this]() noexcept {
        platform::LockGuard lock(gate_);
        processing_ = false;
    };
    auto memory =
        allocator.Allocate(sizeof(ObjectHandle) * count, alignof(ObjectHandle), ObjectRefArrayTag);
    if (!memory) {
        finish();
        return Result<void>::Failure(memory.Error());
    }

    auto* fresh = static_cast<ObjectHandle*>(memory.Value());
    ObjectHandle* release = fresh;
    Result<void> result = Result<void>::Success();
    {
        platform::LockGuard lock(gate_);
        result = validateLocked();
        if (result) {
            auto published = publishLocked(fresh, count);
            if (published) {
                release = published.Value();
            } else {
                result = Result<void>::Failure(published.Error());
            }
        }
    }

    // 옛(또는 쓰이지 않은) 버퍼는 처리 표시 아래 잠금 밖에서 반환한다.
    ReleaseContainerMemory(allocator, release);
    finish();
    return result;
}

Result<usize> ObjectRegistry::InsertStructElement(const ObjectAccessContext& access,
                                                  ObjectHandle owner,
                                                  detail::StructRefArrayStorage& storage,
                                                  usize index, bool append) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<usize>::Failure(valid.Error());
    }

    Allocator& allocator = *storage.allocator;
    const detail::StructElementOps& ops = *storage.element;
    auto validateLocked = [&]() noexcept -> Result<void> {
        if (processing_ || closed_) {
            return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
        }

        return CheckContainerOwnerLocked(owner, &storage, sizeof(storage), 0, false);
    };

    uint32 grown = 0;
    {
        platform::LockGuard lock(gate_);
        auto checked = validateLocked();
        if (!checked) {
            return Result<usize>::Failure(checked.Error());
        }

        const usize position = append ? storage.size : index;
        if (position > storage.size) {
            return Result<usize>::Failure(ErrorCode::InvalidArgument);
        }

        if (storage.size == NumericLimits<uint32>::Max()) {
            return Result<usize>::Failure(ErrorCode::Overflow);
        }

        if (storage.size == storage.capacity) {
            grown = GrownCapacity(storage.capacity);
        }
    }

    // 잠금 밖: 원소 블록 할당·기본 생성(참조는 null이라 장벽이 필요 없다)·필요하면 새 색인 할당.
    // 생성자·allocator 콜백이 같은 배열을 바꿀 수 있으므로 게시는 다시 잠근 뒤 그때 상태로 한다.
    auto memory = allocator.Allocate(ops.size, ops.alignment, StructRefElementTag);
    if (!memory) {
        return Result<usize>::Failure(memory.Error());
    }

    void* block = memory.Value();
    ops.construct(block, allocator);
    void** fresh = nullptr;
    if (grown != 0) {
        auto table = allocator.Allocate(sizeof(void*) * grown, alignof(void*), StructRefIndexTag);
        if (!table) {
            DestroyStructElement(allocator, ops, block);
            return Result<usize>::Failure(table.Error());
        }

        fresh = static_cast<void**>(table.Value());
    }

    void** release = fresh;
    bool published = false;
    usize position = 0;
    Result<void> result = Result<void>::Success();
    {
        platform::LockGuard lock(gate_);
        result = validateLocked();
        if (result) {
            position = append ? storage.size : index;
            void** target = nullptr;
            if (position > storage.size) {
                result = Result<void>::Failure(ErrorCode::Busy);
            } else if (storage.size == NumericLimits<uint32>::Max()) {
                result = Result<void>::Failure(ErrorCode::Overflow);
            } else if (storage.size < storage.capacity) {
                target = storage.blocks;
            } else if (fresh != nullptr && grown > storage.size) {
                target = fresh;
            } else {
                // 새 색인이 없거나(그 사이 용량이 찼다) 모자라다.
                result = Result<void>::Failure(ErrorCode::Busy);
            }

            if (target != nullptr) {
                if (target != storage.blocks) {
                    for (usize at = 0; at < position; ++at) {
                        target[at] = storage.blocks[at];
                    }

                    for (usize at = storage.size; at > position; --at) {
                        target[at] = storage.blocks[at - 1];
                    }

                    release = storage.blocks;
                    storage.blocks = target;
                    storage.capacity = grown;
                } else {
                    for (usize at = storage.size; at > position; --at) {
                        target[at] = target[at - 1];
                    }
                }

                target[position] = block;
                ++storage.size;
                ++storage.revision;
                published = true;
            }
        }
    }

    ReleaseContainerMemory(allocator, release);
    if (!published) {
        DestroyStructElement(allocator, ops, block);
        return Result<usize>::Failure(result.Error());
    }

    return Result<usize>::Success(position);
}

Result<void> ObjectRegistry::RemoveStructElement(const ObjectAccessContext& access,
                                                 ObjectHandle owner,
                                                 detail::StructRefArrayStorage& storage,
                                                 usize index) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    Allocator& allocator = *storage.allocator;
    const detail::StructElementOps& ops = *storage.element;
    void* block = nullptr;
    // 삭제 장벽은 필요 없다(삽입 장벽과 Sweeping cutoff가 지킨다). 색인에서 먼저 떼므로 소멸 중
    // 재진입이 이 원소를 다시 건드릴 수 없다.
    auto detachLocked = [&]() noexcept {
        block = storage.blocks[index];
        for (usize position = index + 1; position < storage.size; ++position) {
            storage.blocks[position - 1] = storage.blocks[position];
        }

        --storage.size;
        ++storage.revision;
    };
    ObjectHandle package;
    {
        platform::LockGuard lock(gate_);
        if (processing_ || closed_) {
            return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
        }

        auto checked = CheckContainerOwnerLocked(owner, &storage, sizeof(storage), 0, true);
        if (!checked) {
            return checked;
        }

        if (index >= storage.size) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const Slot& source = *FindSlot(owner);
        if (!ObservesOwnerLocked(source)) {
            detachLocked();
        } else {
            // 다중 간선 지점: 떼어 낼 원소만 처리 표시 아래 잠금 밖에서 trace해 합친다.
            package = HandleOf(*source.outermost);
            processing_ = true;
        }
    }

    if (block == nullptr) {
        EdgeTally tally(allocator_);
        void* const target = storage.blocks[index];
        if (auto tallied = TallyStructBlocks(ops, &target, 1, package, tally); !tallied) {
            return EndProcessing(Move(tallied));
        }

        if (auto reserved = ReserveTally(tally, ReferenceChangeCause::Edit, 0); !reserved) {
            return EndProcessing(Move(reserved));
        }

        platform::LockGuard lock(gate_);
        processing_ = false;
        if (MissingKeysLocked(tally, ReferenceChangeCause::Edit, 0) > ObserverFreeLocked()) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        detachLocked();
        ApplyTallyLocked(tally, ReferenceChangeCause::Edit, 0);
    }

    DestroyStructElement(allocator, ops, block);
    return Result<void>::Success();
}

Result<void> ObjectRegistry::ClearStructElements(const ObjectAccessContext& access,
                                                 ObjectHandle owner,
                                                 detail::StructRefArrayStorage& storage) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    Allocator& allocator = *storage.allocator;
    const detail::StructElementOps& ops = *storage.element;
    void** blocks = nullptr;
    uint32 size = 0;
    // 색인째 떼어 낸다(잠금 밖에서 소멸할 원소를 담으려고 할당하지 않는다).
    auto detachLocked = [&]() noexcept {
        blocks = storage.blocks;
        size = storage.size;
        storage.blocks = nullptr;
        storage.size = 0;
        ++storage.revision;
        storage.capacity = 0;
    };
    bool observe = false;
    ObjectHandle package;
    {
        platform::LockGuard lock(gate_);
        if (processing_ || closed_) {
            return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
        }

        auto checked = CheckContainerOwnerLocked(owner, &storage, sizeof(storage), 0, true);
        if (!checked) {
            return checked;
        }

        const Slot& source = *FindSlot(owner);
        observe = ObservesOwnerLocked(source) && storage.size != 0;
        if (!observe) {
            detachLocked();
        } else {
            // 다중 간선 지점: 떼어 낼 원소를 처리 표시 아래 잠금 밖에서 trace해 합친다.
            package = HandleOf(*source.outermost);
            processing_ = true;
        }
    }

    if (observe) {
        EdgeTally tally(allocator_);
        if (auto tallied = TallyStructBlocks(ops, storage.blocks, storage.size, package, tally);
            !tallied) {
            return EndProcessing(Move(tallied));
        }

        if (auto reserved = ReserveTally(tally, ReferenceChangeCause::Edit, 0); !reserved) {
            return EndProcessing(Move(reserved));
        }

        platform::LockGuard lock(gate_);
        processing_ = false;
        if (MissingKeysLocked(tally, ReferenceChangeCause::Edit, 0) > ObserverFreeLocked()) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        detachLocked();
        ApplyTallyLocked(tally, ReferenceChangeCause::Edit, 0);
    }

    for (uint32 remaining = size; remaining != 0; --remaining) {
        DestroyStructElement(allocator, ops, blocks[remaining - 1]);
    }

    ReleaseContainerMemory(allocator, blocks);
    return Result<void>::Success();
}

Result<void> ObjectRegistry::AssignStructElements(const ObjectAccessContext& access,
                                                  ObjectHandle owner,
                                                  detail::StructRefArrayStorage& storage,
                                                  usize count, uint64 transaction) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    if (count > NumericLimits<uint32>::Max()) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    Allocator& allocator = *storage.allocator;
    const detail::StructElementOps& ops = *storage.element;
    {
        platform::LockGuard lock(gate_);
        if (closed_ || processing_ || transaction == 0) {
            return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
        }

        auto checked =
            CheckContainerOwnerLocked(owner, &storage, sizeof(storage), transaction, false);
        if (!checked) {
            return checked;
        }

        // 블록 할당·원소 생성·옛 원소 소멸은 처리 표시 아래 잠금 밖에서 한다(CreatePending과 같은
        // 규칙 — 그동안의 Registry 재진입은 Busy, Rollback 재진입은 Fatal).
        processing_ = true;
    }

    auto finish = [this]() noexcept {
        platform::LockGuard lock(gate_);
        processing_ = false;
    };
    void** fresh = nullptr;
    usize built = 0;
    auto discard = [&]() noexcept {
        for (usize remaining = built; remaining != 0; --remaining) {
            DestroyStructElement(allocator, ops, fresh[remaining - 1]);
        }

        ReleaseContainerMemory(allocator, fresh);
    };
    if (count != 0) {
        auto table = allocator.Allocate(sizeof(void*) * count, alignof(void*), StructRefIndexTag);
        if (!table) {
            finish();
            return Result<void>::Failure(table.Error());
        }

        fresh = static_cast<void**>(table.Value());
    }

    for (; built < count; ++built) {
        auto memory = allocator.Allocate(ops.size, ops.alignment, StructRefElementTag);
        if (!memory) {
            discard();
            finish();
            return Result<void>::Failure(memory.Error());
        }

        ops.construct(memory.Value(), allocator);
        fresh[built] = memory.Value();
    }

    void** previous = nullptr;
    uint32 previousSize = 0;
    Result<void> result = Result<void>::Success();
    {
        platform::LockGuard lock(gate_);
        result = CheckContainerOwnerLocked(owner, &storage, sizeof(storage), transaction, false);
        if (result) {
            previous = storage.blocks;
            previousSize = storage.size;
            storage.blocks = fresh;
            storage.size = static_cast<uint32>(count);
            ++storage.revision;
            storage.capacity = static_cast<uint32>(count);
        }
    }

    if (!result) {
        discard();
        finish();
        return result;
    }

    // 옛 원소는 게시 뒤 역순으로 소멸한다.
    for (uint32 remaining = previousSize; remaining != 0; --remaining) {
        DestroyStructElement(allocator, ops, previous[remaining - 1]);
    }

    ReleaseContainerMemory(allocator, previous);
    finish();
    return Result<void>::Success();
}

} // namespace rocks
