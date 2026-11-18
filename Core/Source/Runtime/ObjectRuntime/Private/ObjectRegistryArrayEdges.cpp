#include "ObjectRegistryInternal.h"

#include <Object/ObjectRegistry.h>

// ObjectRegistry의 ObjectRefArray 간선 편집이다. live 변경(InsertArrayEdge·ReplaceArrayEdge·
// RemoveArrayEdge·ClearArrayEdges)과 생성 트랜잭션의 staged 지정(AssignArrayEdges)을 둔다. owner·
// 대상 검증은 ObjectRegistryEdges.cpp, 버퍼 용량·반환 도우미는 ObjectRegistryInternal.h다.
namespace rocks {

using namespace registry_detail;

namespace {

constexpr uint32 ObjectRefArrayTag = 0x4f524146u; // 'ORAF'

} // namespace

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
        if (target.IsNull()) {
            return Result<void>::Success();
        }

        auto checked = CheckTargetLocked(target, 0, nativeType);
        if (!checked) {
            return checked;
        }

        return CheckPermanentEdgeLocked(*FindSlot(owner), target);
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
        LedgerEditLocked(*FindSlot(owner), ObjectHandle{}, target);
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
                if (targetChecked) {
                    targetChecked = CheckPermanentEdgeLocked(*FindSlot(owner), target);
                }

                if (!targetChecked) {
                    return targetChecked;
                }
            }

            const EdgePlan plan = PlanEditLocked(*FindSlot(owner), storage.data[index], target);
            const usize missing = plan.count != 0 ? MissingKeysLocked(plan) : 0;
            if (missing <= ObserverFreeLocked()) {
                const ObjectHandle previous = storage.data[index];
                if (target.IsNull()) {
                    storage.data[index] = ObjectHandle{};
                } else {
                    const ErrorCode shaded = ShadeLocked(target);
                    if (shaded != ErrorCode::None) {
                        return Result<void>::Failure(shaded);
                    }

                    storage.data[index] = target;
                }

                LedgerEditLocked(*FindSlot(owner), previous, target);

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
                LedgerEditLocked(*FindSlot(owner), storage.data[position], ObjectHandle{});
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
            // 클러스터 장부는 관찰 대상이 아니어도 지워지는 값을 모두 센다(13.4절).
            LedgerClearLocked(source, storage.data, storage.size);
            storage.size = 0;
            return Result<void>::Success();
        }

        // 다중 간선 지점(Docs/Design/ObjectFoundations.ko.md 3절): 처리 표시 아래 잠금 밖에서
        // 떼어 낼 값의 쌍을 합친 뒤 예약하고, 한 잠금 안에서 비우고 기록한다. 처리 표시 동안 다른
        // 변경은 Busy라 배열·owner를 다시 검증할 필요가 없다.
        package = HandleOf(*ColdOf(source).outermost);
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

    LedgerClearLocked(*FindSlot(owner), storage.data, storage.size);
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

} // namespace rocks
