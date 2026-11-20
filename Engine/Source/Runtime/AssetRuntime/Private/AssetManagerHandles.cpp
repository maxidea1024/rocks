#include "AssetManagerInternal.h"

#include <Core/Move.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>

// AssetManager의 handle 슬롯·해제 큐·티켓·callback·scope·보류
// 명령이다(Docs/Design/AssetSystem.ko.md 11.7절 5–9).
namespace rocks {

bool AssetManager::Impl::OnOwnerThread() const noexcept {
    return platform::CurrentThreadId() == ownerThread;
}

ManagerSlot& AssetManager::Impl::Slot(uint32 index) noexcept {
    return blocks[index / ManagerSlotsPerBlock]->slots[index % ManagerSlotsPerBlock];
}

const ManagerSlot& AssetManager::Impl::Slot(uint32 index) const noexcept {
    return blocks[index / ManagerSlotsPerBlock]->slots[index % ManagerSlotsPerBlock];
}

bool AssetManager::Impl::ValidSlot(uint32 index, uint32 generation,
                                   ManagerSlotKind kind) const noexcept {
    if (index >= blockCount * ManagerSlotsPerBlock) {
        return false;
    }

    const ManagerSlot& slot = Slot(index);
    return slot.generation == generation && slot.kind == kind && slot.objectAlive;
}

// 슬롯을 하나 꺼낸다. 블록이 모자라면 새 블록을 만든다(블록 표는 고정 크기 — 11.7절 6).
Result<uint32> AssetManager::Impl::TakeSlot(ManagerSlotKind kind) noexcept {
    if (freeSlot == ManagerNoIndex) {
        if (blockCount == ManagerMaxSlotBlocks) {
            return Result<uint32>::Failure(ErrorCode::CapacityExceeded);
        }

        auto memory = allocator.Allocate(sizeof(ManagerSlotBlock), alignof(ManagerSlotBlock));
        if (!memory) {
            return Result<uint32>::Failure(memory.Error());
        }

        ManagerSlotBlock* block = InitializeAt<ManagerSlotBlock>(memory.Value());
        const uint32 base = blockCount * ManagerSlotsPerBlock;
        for (uint32 index = 0; index < ManagerSlotsPerBlock; ++index) {
            block->slots[index].self = base + index;
            block->slots[index].nextFree =
                index + 1 < ManagerSlotsPerBlock ? base + index + 1 : ManagerNoIndex;
        }

        blocks[blockCount++] = block;
        freeSlot = base;
    }

    const uint32 index = freeSlot;
    ManagerSlot& slot = Slot(index);
    freeSlot = slot.nextFree;
    slot.kind = kind;
    slot.objectAlive = true;
    slot.revoked = false;
    slot.record = ManagerNoIndex;
    slot.package = ManagerNoIndex;
    slot.scope = ManagerNoIndex;
    slot.asset = AssetId{};
    slot.assetObject = ObjectHandle{};
    slot.packageHandle = ObjectHandle{};
    platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&liveObjects, 1u);
    return Result<uint32>::Success(index);
}

void AssetManager::Impl::FreeSlot(uint32 index) noexcept {
    ManagerSlot& slot = Slot(index);
    slot.kind = ManagerSlotKind::Free;
    slot.objectAlive = false;
    // 세대 0은 쓰지 않는다(기본 handle과 구분).
    slot.generation = slot.generation + 1 == 0 ? 1 : slot.generation + 1;
    slot.nextFree = freeSlot;
    freeSlot = index;
}

// handle 객체의 소멸이 부른다(어느 스레드 — 할당 없음). 슬롯은 Pump가 처리할 때까지 재사용되지
// 않는다.
void AssetManager::Impl::PostRelease(uint32 index) noexcept {
    ManagerSlot& slot = Slot(index);
    releases.mutex.Lock();
    slot.nextReleased = releases.head;
    releases.head = &slot;
    releases.mutex.Unlock();
    platform::AtomicFetchSub<platform::MemoryOrder::Release>(&liveObjects, 1u);
}

// 해제 큐를 비운다(11.7절 4 단계 1). 넣은 순서대로 처리한다.
void AssetManager::Impl::DrainReleases() noexcept {
    releases.mutex.Lock();
    ManagerSlot* head = releases.head;
    releases.head = nullptr;
    releases.mutex.Unlock();
    ManagerSlot* ordered = nullptr;
    while (head != nullptr) {
        ManagerSlot* next = head->nextReleased;
        head->nextReleased = ordered;
        ordered = head;
        head = next;
    }

    while (ordered != nullptr) {
        ManagerSlot& slot = *ordered;
        ordered = slot.nextReleased;
        slot.nextReleased = nullptr;
        slot.objectAlive = false;
        switch (slot.kind) {
        case ManagerSlotKind::Ticket: {
            ManagerTicket& ticket = tickets[slot.record];
            ticket.released = true;
            ticket.slot = ManagerNoIndex;
            // 끝나기 전이거나 callback을 아직 부르지 않았으면 Cancelled로 끝낸다(11.7절 5 — 끝난
            // 티켓도 전달 전이면 그 결과 대신 Cancelled를 받는다).
            if (!slot.revoked) {
                if (!ticket.done || (ticket.callbackRegistered && !ticket.callbackDelivered)) {
                    EndTicket(slot.record, ErrorCode::Cancelled);
                } else {
                    ReleaseTicketHold(slot.record);
                }
            }

            if (!(ticket.callbackRegistered && !ticket.callbackDelivered)) {
                FreeTicket(slot.record);
            }

            break;
        }
        case ManagerSlotKind::Lease:
            if (!slot.revoked) {
                DropHold(slot.package, &ManagerUnit::leases);
            }

            break;
        case ManagerSlotKind::Deferred:
            deferred[slot.record].slot = ManagerNoIndex;
            if (deferred[slot.record].done) {
                FreeDeferred(slot.record);
            }

            break;
        case ManagerSlotKind::PrimaryTicket:
            if (!slot.revoked) {
                ReleasePrimary(slot.record);
            } else {
                ManagerPrimary& primary = primaries[slot.record];
                primary.released = true;
                primary.slot = ManagerNoIndex;
                if (!(primary.callbackRegistered && !primary.callbackDelivered)) {
                    FreePrimary(slot.record);
                }
            }

            break;
        case ManagerSlotKind::Free:
            platform::Fatal("AssetManager released a free handle slot");
        }

        FreeSlot(slot.self);
    }
}

Result<uint32> AssetManager::Impl::TakeTicket() noexcept {
    if (freeTicket == ManagerNoIndex) {
        // 전달 목록은 티켓·묶음 표 크기만큼 늘 잡아 둔다(Pump·Shutdown의 전달은 할당하지 않는다).
        if (auto reserved = deliver.TryReserve(tickets.Size() + 1); !reserved) {
            return Result<uint32>::Failure(reserved.Error());
        }

        if (auto added = tickets.TryEmplaceBack(); !added) {
            return Result<uint32>::Failure(added.Error());
        }

        freeTicket = static_cast<uint32>(tickets.Size() - 1);
        tickets[freeTicket].nextFree = ManagerNoIndex;
    }

    const uint32 index = freeTicket;
    freeTicket = tickets[index].nextFree;
    tickets[index] = ManagerTicket{};
    tickets[index].used = true;
    return Result<uint32>::Success(index);
}

void AssetManager::Impl::FreeTicket(uint32 index) noexcept {
    tickets[index] = ManagerTicket{};
    tickets[index].nextFree = freeTicket;
    freeTicket = index;
}

// 로더 요청을 내고 티켓 기록을 만든다. 로더가 바로 Succeeded로 답하면(상주) 그 자리에서 보유를
// 건다(11.7절 4 — revive).
Result<uint32> AssetManager::Impl::StartTicket(const AssetRequest& request,
                                               AssetDiagnostic* failure) noexcept {
    auto taken = TakeTicket();
    if (!taken) {
        return taken;
    }

    const uint32 index = taken.Value();
    auto requested =
        loader->Request(AssetLoadRequest{request.id, request.priority, request.cancel}, failure);
    if (!requested) {
        FreeTicket(index);
        return Result<uint32>::Failure(requested.Error());
    }

    ManagerTicket& ticket = tickets[index];
    const CookedManifestEntry* entry = loaderConfig.manifest->Find(request.id);
    ticket.entry = static_cast<uint32>(entry - loaderConfig.manifest->entries.Data());
    ticket.asset = request.id;
    ticket.expectedType = request.expectedType;
    ticket.scope = request.scope == nullptr ? ManagerNoIndex : request.scope->index_;
    ticket.loaderHandle = requested.Value();
    ticket.loaderHeld = true;
    CompleteTicket(index);
    return Result<uint32>::Success(index);
}

void AssetManager::Impl::CompleteTicket(uint32 index) noexcept {
    ManagerTicket& ticket = tickets[index];
    // 끝나지 않은 티켓은 로더 요청을 쥐고 있다(끝낼 때만 놓는다 — EndTicket·FinishTicketResult).
    if (ticket.done) {
        return;
    }

    auto status = loader->Status(ticket.loaderHandle);
    if (status && status.Value().state != AssetLoadState::Pending) {
        FinishTicketResult(index, status.Value());
    }
}

// 끝난 로더 결과를 티켓에 옮기고 로더 기록을 돌려준다. 성공이면 대상 단위에 티켓 관심을 건다.
void AssetManager::Impl::FinishTicketResult(uint32 index, AssetLoadStatus status) noexcept {
    ManagerTicket& ticket = tickets[index];
    (void)loader->Release(ticket.loaderHandle);
    ticket.loaderHeld = false;
    ticket.done = true;
    if (status.state != AssetLoadState::Succeeded) {
        ticket.error = status.error;
        ticket.diagnostic = status.diagnostic;
        return;
    }

    // 로더가 이번 단계에서 끝낸 단위는 아직 넘겨받지 않았을 수 있다.
    if (entryPackage[ticket.entry] == ManagerNoIndex) {
        (void)IngestPublished();
    }

    const uint32 package = entryPackage[ticket.entry];
    if (package == ManagerNoIndex) {
        // 넘겨받기가 할당 실패로 미뤄졌다 — 그 실패를 이 티켓의 결과로 준다.
        ticket.error = ErrorCode::OutOfMemory;
        return;
    }

    if (ticket.expectedType != TypeId{}) {
        auto type = registry->AcquireType(access, packages[package].assetObject);
        if (!type || !type.Value().IsA(ticket.expectedType)) {
            ticket.error = ErrorCode::TypeMismatch;
            ticket.diagnostic = AssetDiagnostic{};
            ticket.diagnostic.code = AssetDiagnosticCode::TypeMismatch;
            ticket.diagnostic.error = ErrorCode::TypeMismatch;
            ticket.diagnostic.asset = ticket.asset;
            return;
        }
    }

    AddHold(package, &ManagerUnit::tickets);
    ticket.heldPackage = package;
}

void AssetManager::Impl::ReleaseTicketHold(uint32 index) noexcept {
    ManagerTicket& ticket = tickets[index];
    if (ticket.heldPackage != ManagerNoIndex) {
        DropHold(ticket.heldPackage, &ManagerUnit::tickets);
        ticket.heldPackage = ManagerNoIndex;
    }
}

// 티켓을 끝낸다(취소·scope 회수·종료): 로더 요청과 관심을 놓고 아직 부르지 않은 callback이 받을
// 결과를 error로 정한다.
void AssetManager::Impl::EndTicket(uint32 index, ErrorCode error) noexcept {
    ManagerTicket& ticket = tickets[index];
    if (ticket.loaderHeld) {
        (void)loader->Release(ticket.loaderHandle);
        ticket.loaderHeld = false;
    }

    ReleaseTicketHold(index);
    ticket.done = true;
    ticket.error = error;
    ticket.diagnostic = AssetDiagnostic{};
}

void AssetManager::Impl::CompleteTickets() noexcept {
    for (usize index = 0; index < tickets.Size(); ++index) {
        if (tickets[index].used) {
            CompleteTicket(static_cast<uint32>(index));
        }
    }
}

Result<AssetLease> AssetManager::Impl::MakeLease(uint32 package, uint32 scope) noexcept {
    auto taken = TakeSlot(ManagerSlotKind::Lease);
    if (!taken) {
        return Result<AssetLease>::Failure(taken.Error());
    }

    ManagerSlot& slot = Slot(taken.Value());
    slot.package = package;
    slot.scope = scope;
    slot.asset = packages[package].asset;
    slot.assetObject = packages[package].assetObject;
    slot.packageHandle = packages[package].handle;
    AddHold(package, &ManagerUnit::leases);
    return Result<AssetLease>::Success(AssetLease(&owner, taken.Value(), slot.generation));
}

// 끝난 티켓의 callback을 부른다(11.7절 4 단계 8 — 내부 순회가 끝난 뒤 모아 둔 목록으로). callback이
// 표를 늘릴 수 있으므로 매번 번호로 다시 찾는다.
uint32 AssetManager::Impl::DeliverCallbacks() noexcept {
    deliver.Clear();
    for (usize index = 0; index < tickets.Size(); ++index) {
        const ManagerTicket& ticket = tickets[index];
        if (ticket.used && ticket.done && ticket.callbackRegistered && !ticket.callbackDelivered) {
            // 용량은 티켓 표가 자랄 때 잡았다(TakeTicket).
            (void)deliver.TryPushBack(static_cast<uint32>(index));
        }
    }

    // callback 안의 요청이 전달 목록을 다시 잡을 수 있으므로 색인으로 돈다(11.7절 5 재진입).
    uint32 delivered = 0;
    const usize count = deliver.Size();
    for (usize position = 0; position < count; ++position) {
        const uint32 index = deliver[position];
        AssetReadyCallback callback = Move(tickets[index].callback);
        tickets[index].callbackDelivered = true;
        const AssetDiagnostic diagnostic = tickets[index].diagnostic;
        Result<AssetLease> result = Result<AssetLease>::Failure(tickets[index].error);
        if (tickets[index].error == ErrorCode::None) {
            result = MakeLease(tickets[index].heldPackage, tickets[index].scope);
        }

        inCallback = true;
        callback(Move(result), diagnostic);
        inCallback = false;
        ++delivered;
        if (tickets[index].released) {
            FreeTicket(index);
        }
    }

    return delivered;
}

void AssetManager::Impl::FreeDeferred(uint32 index) noexcept {
    deferred[index] = ManagerDeferred{};
    deferred[index].nextFree = freeDeferred;
    freeDeferred = index;
}

// 끝난(또는 멈춘) 보류 명령의 run·fail을 부른다(11.7절 9). 이번 Pump에 새로 생긴 명령은 다음
// Pump에서 실행한다(처음 크기까지만 본다).
uint32 AssetManager::Impl::RunDeferred() noexcept {
    uint32 ran = 0;
    const usize count = deferred.Size();
    for (usize position = 0; position < count; ++position) {
        const auto index = static_cast<uint32>(position);
        if (!deferred[index].used || deferred[index].done) {
            continue;
        }

        // scope 회수는 그 scope의 명령에 stop을 먼저 적는다(RevokeScope) — 닫힌 scope는 stop으로
        // 온다.
        const uint32 ticketIndex = deferred[index].ticket;
        const ErrorCode stop = deferred[index].stop;
        if (stop == ErrorCode::None && !tickets[ticketIndex].done) {
            continue;
        }

        DeferredRun run = Move(deferred[index].run);
        DeferredFail fail = Move(deferred[index].fail);
        deferred[index].done = true;
        --activeDeferred;
        const ManagerTicket& ticket = tickets[ticketIndex];
        if (stop != ErrorCode::None) {
            inCallback = true;
            fail(stop, AssetDiagnostic{});
            inCallback = false;
        } else if (ticket.error != ErrorCode::None) {
            const AssetDiagnostic diagnostic = ticket.diagnostic;
            const ErrorCode error = ticket.error;
            inCallback = true;
            fail(error, diagnostic);
            inCallback = false;
        } else if (auto lease = MakeLease(ticket.heldPackage, deferred[index].scope); !lease) {
            inCallback = true;
            fail(lease.Error(), AssetDiagnostic{});
            inCallback = false;
        } else {
            inCallback = true;
            run(lease.Value());
            inCallback = false;
        }

        // 내부 티켓의 관심은 실행이 끝나면 놓는다(lease는 run이 쥐지 않으면 다음 Pump에 놓인다).
        EndTicket(ticketIndex, ErrorCode::None);
        FreeTicket(ticketIndex);
        deferred[index].ticket = ManagerNoIndex;
        ++ran;
        if (deferred[index].slot == ManagerNoIndex) {
            FreeDeferred(index);
        }
    }

    return ran;
}

bool AssetManager::Impl::ScopeOpen(uint32 scope) const noexcept {
    return !scopes[scope].revoked;
}

// scope(NoIndex면 전부 — Shutdown)의 티켓·lease·보류 명령을 회수한다(11.7절 7).
void AssetManager::Impl::RevokeScope(uint32 scope, ErrorCode reason) noexcept {
    const bool all = scope == ManagerNoIndex;
    if (!all) {
        scopes[scope].revoked = true;
    }

    for (uint32 index = 0; index < blockCount * ManagerSlotsPerBlock; ++index) {
        // 소멸했지만 해제 큐에 남은 lease도 여기서 회수한다(objectAlive는 큐를 처리할 때 칸과 함께
        // 지운다) — 큐 처리는 회수된 lease의 보유를 다시 빼지 않는다.
        ManagerSlot& slot = Slot(index);
        if (slot.kind != ManagerSlotKind::Lease || slot.revoked || (!all && slot.scope != scope)) {
            continue;
        }

        slot.revoked = true;
        DropHold(slot.package, &ManagerUnit::leases);
    }

    for (usize index = 0; index < tickets.Size(); ++index) {
        ManagerTicket& ticket = tickets[index];
        // 내부 티켓(보류 명령·LoadBlocking)은 주인이 처리한다.
        if (!ticket.used || ticket.slot == ManagerNoIndex || (!all && ticket.scope != scope)) {
            continue;
        }

        ManagerSlot& slot = Slot(ticket.slot);
        if (slot.revoked) {
            continue;
        }

        slot.revoked = true;
        if (!ticket.callbackDelivered) {
            EndTicket(static_cast<uint32>(index), reason);
        } else {
            ReleaseTicketHold(static_cast<uint32>(index));
        }
    }

    for (ManagerDeferred& command : deferred) {
        if (command.used && !command.done && command.stop == ErrorCode::None &&
            (all || command.scope == scope)) {
            command.stop = reason;
        }
    }

    for (usize index = 0; index < primaries.Size(); ++index) {
        ManagerPrimary& primary = primaries[index];
        if (!primary.used || primary.slot == ManagerNoIndex || (!all && primary.scope != scope)) {
            continue;
        }

        ManagerSlot& slot = Slot(primary.slot);
        if (slot.revoked) {
            continue;
        }

        slot.revoked = true;
        if (!primary.callbackDelivered) {
            EndPrimary(static_cast<uint32>(index), reason);
        } else {
            for (uint32 ticket : primary.tickets) {
                ReleaseTicketHold(ticket);
            }
        }
    }
}

} // namespace rocks
