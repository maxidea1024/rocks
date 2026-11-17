#include "ObjectRegistryInternal.h"

#include <Object/ObjectRegistry.h>

// ObjectRegistry의 ObjectRefMap 간선 편집이다(리플렉션 B9f4 —
// Docs/Design/ReflectionExtensions.ko.md 7.4절). live 변경(SetMapEdge·EraseMapEdge·ClearMapEdges)과
// 생성 트랜잭션의 staged 지정 (AssignMapEdges)을 둔다. 키 형식별 조회·준비·게시는
// detail::ObjectRefMapEdit(Object/ObjectRefMap.h)이 맡고, owner·대상 검증은
// ObjectRegistryEdges.cpp(CheckContainerOwnerLocked·CheckTargetLocked), 관찰자 계획·예약은
// ObjectRegistryObserver.cpp다. 규칙은 ObjectRefArray 편집 (ObjectRegistryArrayEdges.cpp)과 같다:
// allocator·관찰자 예약 콜백은 잠금 밖, 다시 잠그면 owner· 대상·컨테이너 상태를 다시 판단하고,
// 게시는 잠금 안에서 무할당·무실패다. 항목 포인터는 잠금 하나 안에서만 쓰고(Erase의 압축이 모든
// 포인터를 무효화한다) 다시 잠그면 키로 다시 찾는다. 해제할 것(옛 저장소·지운 키·옛 항목)은 edit가
// 쥐었다가 호출이 돌아온 뒤 반환한다.
namespace rocks {

using namespace registry_detail;

Result<void> ObjectRegistry::SetMapEdge(const ObjectAccessContext& access, ObjectHandle owner,
                                        detail::ObjectRefMapEdit& edit, ObjectHandle target,
                                        const void* nativeType, MapReferenceWrite* write) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    if (target.IsNull()) {
        target = ObjectHandle{};
    }

    // 첫 시도에서 본 구조 변경 계수다. 잠금 밖 준비(키 사본·저장소·관찰자 예약) 동안 콜백이 같은
    // 컨테이너의 구조를 바꾸면 준비가 맞지 않으므로 다시 잠근 뒤 Busy다.
    uint64 revision = 0;
    bool prepared = false;
    for (uint32 attempt = 0;; ++attempt) {
        usize reserve = 0;
        bool insert = false;
        {
            platform::LockGuard lock(gate_);
            if (processing_ || closed_) {
                return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
            }

            auto checked =
                CheckContainerOwnerLocked(owner, edit.Storage(), edit.StorageBytes(), 0, false);
            if (!checked) {
                return checked;
            }

            if (edit.IsIterating() || (attempt != 0 && edit.Revision() != revision)) {
                return Result<void>::Failure(ErrorCode::Busy);
            }

            revision = edit.Revision();
            if (!target.IsNull()) {
                auto targetChecked = CheckTargetLocked(target, 0, nativeType);
                if (!targetChecked) {
                    return targetChecked;
                }
            }

            // 새 키의 항목 수 상한(MapReferenceWrite::maxCount)은 첫 판정과 게시 직전 재검증
            // 모두에서 본다(그 사이 길이를 바꾸는 재진입은 계수로도 Busy지만 상한 판정을 그것에
            // 기대지 않는다).
            ObjectHandle* slot = edit.Find();
            if (!slot && write && edit.Count() >= write->maxCount) {
                return Result<void>::Failure(ErrorCode::CapacityExceeded);
            }

            const EdgePlan plan =
                PlanEditLocked(*FindSlot(owner), slot ? *slot : ObjectHandle{}, target);
            const usize missing = plan.count != 0 ? MissingKeysLocked(plan) : 0;
            const bool keysReady = missing <= ObserverFreeLocked();
            if (keysReady && (slot || prepared)) {
                if (!target.IsNull()) {
                    const ErrorCode shaded = ShadeLocked(target);
                    if (shaded != ErrorCode::None) {
                        return Result<void>::Failure(shaded);
                    }
                }

                // 출력은 이 게시 잠금 안에서 실제로 덮어쓴 옛 값이다(호출 전에 읽은 값이 아니다).
                const ObjectHandle previous = slot ? *slot : ObjectHandle{};
                if (slot) {
                    *slot = target;
                } else {
                    edit.CommitInsert(target);
                }

                LedgerEditLocked(*FindSlot(owner), previous, target);

                ApplyPlanLocked(plan);
                if (write) {
                    write->previous = previous;
                    write->replaced = slot != nullptr;
                }

                return Result<void>::Success();
            }

            if (attempt != 0) {
                // 콜백의 기록이 예약한 관찰자 공간을 다시 써 버렸다.
                return Result<void>::Failure(ErrorCode::Busy);
            }

            reserve = keysReady ? 0 : missing;
            insert = slot == nullptr;
        }

        // allocator·관찰자 콜백은 Registry 잠금 밖에서 실행하며 이 구간에는 할당만 한다(컨테이너는
        // 그대로다). 레지스트리 전체 처리 표시(processing_)는 세우지 않는다 — 흔한 런타임 편집이
        // 다른 스레드를 막지 않게. owner Borrow가 수집·회수를 막으므로 owner와 컨테이너는 살아
        // 있다.
        if (reserve != 0) {
            if (auto reserved = options_.referenceObserver->Reserve(reserve); !reserved) {
                return reserved;
            }
        }

        if (insert) {
            if (auto ready = edit.PrepareInsert(); !ready) {
                return ready;
            }

            prepared = true;
        }
    }
}

Result<void> ObjectRegistry::EraseMapEdge(const ObjectAccessContext& access, ObjectHandle owner,
                                          detail::ObjectRefMapEdit& edit,
                                          ObjectHandle* removed) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    uint64 revision = 0;
    // 관찰자 공간이 모자라면 잠금 밖에서 예약한 뒤 한 번 더 한다(RemoveArrayEdge와 같은 규칙).
    for (uint32 attempt = 0;; ++attempt) {
        usize reserve = 0;
        {
            platform::LockGuard lock(gate_);
            if (processing_ || closed_) {
                return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
            }

            auto checked =
                CheckContainerOwnerLocked(owner, edit.Storage(), edit.StorageBytes(), 0, true);
            if (!checked) {
                return checked;
            }

            if (edit.IsIterating() || (attempt != 0 && edit.Revision() != revision)) {
                return Result<void>::Failure(ErrorCode::Busy);
            }

            revision = edit.Revision();
            const ObjectHandle* slot = edit.Find();
            if (!slot) {
                // 재시도에서는 예약 콜백이 그 항목을 지운 것이다(계수가 같으면 일어나지 않는다).
                return Result<void>::Failure(attempt == 0 ? ErrorCode::NotFound : ErrorCode::Busy);
            }

            // DestroyRequested owner의 제거는 기록하지 않는다(PlanEditLocked — Discard가 이미
            // 반납했다).
            const EdgePlan plan = PlanEditLocked(*FindSlot(owner), *slot, ObjectHandle{});
            const usize missing = plan.count != 0 ? MissingKeysLocked(plan) : 0;
            if (missing <= ObserverFreeLocked()) {
                // 삽입 장벽 GC에서 간선 삭제는 장벽이 필요 없다. 키는 edit가 넘겨받아 잠금을 푼 뒤
                // 반환한다.
                const ObjectHandle erased = *slot;
                LedgerEditLocked(*FindSlot(owner), erased, ObjectHandle{});
                edit.Erase();
                ApplyPlanLocked(plan);
                if (removed) {
                    *removed = erased;
                }

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

void ObjectRegistry::ClearMapLedgerLocked(const Slot& owner,
                                          detail::ObjectRefMapEdit& edit) noexcept {
    if (!IsClusterMemberLocked(owner)) {
        return;
    }

    // 컨테이너 자체의 값 순회다(사용자 코드가 아니다) — 잠금 안에서 부른다.
    MarkState state{this, nullptr};
    state.ledgerOwner = owner.index;
    TraceVisitor visitor(&state, &ObjectRegistry::LedgerRemoveVisited);
    edit.VisitValues(visitor);
}

Result<void> ObjectRegistry::ClearMapEdges(const ObjectAccessContext& access, ObjectHandle owner,
                                           detail::ObjectRefMapEdit& edit) noexcept {
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

        auto checked =
            CheckContainerOwnerLocked(owner, edit.Storage(), edit.StorageBytes(), 0, true);
        if (!checked) {
            return checked;
        }

        if (edit.IsIterating()) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        const Slot& source = *FindSlot(owner);
        if (!ObservesOwnerLocked(source) || edit.Count() == 0) {
            // 클러스터 장부는 관찰 대상이 아니어도 지워지는 값을 모두 센다(13.4절).
            ClearMapLedgerLocked(source, edit);
            edit.Detach();
            return Result<void>::Success();
        }

        // 다중 간선 지점(Docs/Design/ObjectFoundations.ko.md 3절): 처리 표시 아래 잠금 밖에서 떼어
        // 낼 값의 쌍을 합친 뒤 예약하고, 한 잠금 안에서 비우고 기록한다. 처리 표시 동안 다른 변경은
        // Busy라 컨테이너·owner를 다시 검증할 필요가 없다.
        package = HandleOf(*source.outermost);
        processing_ = true;
    }

    EdgeTally tally(allocator_);
    MarkState state{this, nullptr};
    state.tally = &tally;
    state.ownerPackage = package;
    state.delta = -1;
    TraceVisitor visitor(&state, &ObjectRegistry::TallyEdge);
    edit.VisitValues(visitor);
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

    ClearMapLedgerLocked(*FindSlot(owner), edit);
    edit.Detach();
    ApplyTallyLocked(tally, ReferenceChangeCause::Edit, 0);
    return Result<void>::Success();
}

Result<void> ObjectRegistry::AssignMapEdges(const ObjectAccessContext& access, ObjectHandle owner,
                                            detail::ObjectRefMapEdit& edit,
                                            Span<const ObjectHandle> targets, uint64 transaction,
                                            const void* nativeType) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    // owner와 모든 대상을 검증한다(잠금 보유). 하나라도 틀리면 불변이다.
    auto validateLocked = [&]() noexcept -> Result<void> {
        auto checked = CheckContainerOwnerLocked(owner, edit.Storage(), edit.StorageBytes(),
                                                 transaction, false);
        if (!checked) {
            return checked;
        }

        if (edit.IsIterating()) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        for (const ObjectHandle target : targets) {
            if (!target.IsNull()) {
                auto targetChecked = CheckTargetLocked(target, transaction, nativeType);
                if (!targetChecked) {
                    return targetChecked;
                }
            }
        }

        return Result<void>::Success();
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

        // 생성 트랜잭션 안의 잠금 밖 할당은 CreatePending과 같은 규칙이다: allocator 재진입은 Busy,
        // Rollback 재진입은 Fatal.
        processing_ = true;
    }

    // 새 컨테이너(키 사본·저장소)를 잠금 밖에서 만든다. 실패하면 컨테이너는 그대로이고 만든 것은
    // edit가 소멸할 때 반환한다.
    if (auto ready = edit.PrepareAssign(targets); !ready) {
        return EndProcessing(Move(ready));
    }

    platform::LockGuard lock(gate_);
    processing_ = false;
    if (auto checked = validateLocked(); !checked) {
        return checked;
    }

    // 생존 대상만 shade한다(ShadeLocked는 Reserved에 StaleHandle을 내므로 Reserved 대상은 Commit이
    // shade한다). 실패하면 게시하지 않는다.
    for (const ObjectHandle target : targets) {
        if (!target.IsNull() && FindSlot(target)->state == SlotState::Alive) {
            const ErrorCode shaded = ShadeLocked(target);
            if (shaded != ErrorCode::None) {
                return Result<void>::Failure(shaded);
            }
        }
    }

    edit.CommitAssign();
    return Result<void>::Success();
}

} // namespace rocks
