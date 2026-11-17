#include "ObjectRegistryInternal.h"

#include <Object/ObjectRegistry.h>

// ObjectRegistry 참조 간선 쓰기다. 단일 참조(SetEdge)와 컨테이너 편집이 함께 쓰는 owner·대상
// 검증(CheckContainerOwnerLocked·CheckTargetLocked)을 둔다. ObjectRefArray 편집은
// ObjectRegistryArrayEdges.cpp, StructRefArray 원소 편집은 ObjectRegistryStructElements.cpp다.
// 쓰기는 삽입 장벽(ShadeLocked — ObjectRegistryGc.cpp)과 관찰자 기록 계획·예약
// (ObjectRegistryObserver.cpp)을 거친다.
namespace rocks {

using namespace registry_detail;

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
                // 클러스터 장부(13.4절 — live 쓰기만, staged는 Commit이 센다). shade 실패는
                // 아래에서 반환하므로 장부는 게시가 확정된 뒤에 고친다.
                const ObjectHandle previous = field;
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

                if (transaction == 0) {
                    LedgerEditLocked(*FindSlot(owner), previous, target);
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

} // namespace rocks
