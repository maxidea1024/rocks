#include "AssetManagerInternal.h"

#include <Core/Text/NameComponent.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Thread.h>

// AssetManager의 PrimaryAsset·bundle 로드다(Docs/Design/AssetSystem.ko.md 11.7절 10 — UE
// UAssetManager::LoadPrimaryAsset·FAssetBundleData 대응).
namespace rocks {

namespace {

// bundle 이름 비교 키 builder의 내장 용량이다(이름은 경로 구성 요소 규칙 — 짧은 이름은 할당 없이).
constexpr usize BundleKeyInlineBytes = 64;

} // namespace

// asset 항목이 PrimaryAsset인지 보고 이름마다 bundle을 찾아 out에 대표 + 구성원(중복 없이 AssetId
// 순)을 채운다.
Result<void> AssetManager::Impl::ResolvePrimary(AssetId asset, Span<const StringView> bundles,
                                                Array<AssetId>& out,
                                                AssetDiagnostic* failure) const noexcept {
    auto fail = [failure, asset](ErrorCode error, AssetDiagnosticCode code) noexcept {
        if (failure != nullptr) {
            *failure = AssetDiagnostic{};
            failure->code = code;
            failure->error = error;
            failure->asset = asset;
        }

        return Result<void>::Failure(error);
    };
    const CookedManifestEntry* entry = loaderConfig.manifest->Find(asset);
    if (entry == nullptr) {
        return fail(ErrorCode::NotFound, AssetDiagnosticCode::MissingDependency);
    }

    if ((entry->flags & static_cast<uint32>(CookedManifestFlags::PrimaryAsset)) == 0) {
        return fail(ErrorCode::InvalidArgument, AssetDiagnosticCode::None);
    }

    // 구성원을 먼저 모아 AssetId 순으로 정렬·중복 제거한 뒤 대표 뒤에 붙인다(구성원은 대표 자신이
    // 아니다 — 10.1절 5).
    out.Clear();
    for (StringView name : bundles) {
        StringBuilder<BundleKeyInlineBytes> wanted(allocator);
        if (auto made = TryMakeNameComponentKey(name, wanted); !made) {
            return fail(made.Error() == ErrorCode::InvalidArgument ? ErrorCode::NotFound
                                                                   : made.Error(),
                        AssetDiagnosticCode::None);
        }

        const CookedManifestBundle* found = nullptr;
        for (const CookedManifestBundle& bundle : entry->bundles) {
            StringBuilder<BundleKeyInlineBytes> key(allocator);
            if (auto made = TryMakeNameComponentKey(bundle.name.View(), key); !made) {
                return fail(made.Error(), AssetDiagnosticCode::None);
            }

            if (key.View() == wanted.View()) {
                found = &bundle;
                break;
            }
        }

        if (found == nullptr) {
            return fail(ErrorCode::NotFound, AssetDiagnosticCode::None);
        }

        if (auto appended = out.TryAppend(found->members.View()); !appended) {
            return fail(appended.Error(), AssetDiagnosticCode::None);
        }
    }

    out.Sort([](const AssetId& left, const AssetId& right) noexcept {
        return GuidLess(left.value, right.value);
    });
    usize kept = 0;
    for (usize index = 0; index < out.Size(); ++index) {
        if (kept == 0 || !(out[kept - 1] == out[index])) {
            out[kept++] = out[index];
        }
    }

    (void)out.TryResize(kept);
    if (auto inserted = out.TryInsert(asset, 0); !inserted) {
        return fail(inserted.Error(), AssetDiagnosticCode::None);
    }

    return Result<void>::Success();
}

Result<uint32> AssetManager::Impl::TakePrimary() noexcept {
    if (freePrimary == ManagerNoIndex) {
        // 전달 목록 용량은 TakeTicket과 같은 규칙이다.
        if (auto reserved = deliver.TryReserve(primaries.Size() + 1); !reserved) {
            return Result<uint32>::Failure(reserved.Error());
        }

        if (auto added = primaries.TryEmplaceBack(allocator); !added) {
            return Result<uint32>::Failure(added.Error());
        }

        freePrimary = static_cast<uint32>(primaries.Size() - 1);
        primaries[freePrimary].nextFree = ManagerNoIndex;
    }

    const uint32 index = freePrimary;
    ManagerPrimary& record = primaries[index];
    freePrimary = record.nextFree;
    record.used = true;
    record.nextFree = ManagerNoIndex;
    record.slot = ManagerNoIndex;
    record.scope = ManagerNoIndex;
    record.tickets.Clear();
    record.callback = PrimaryReadyCallback();
    record.callbackRegistered = false;
    record.callbackDelivered = false;
    record.released = false;
    record.stop = ErrorCode::None;
    return Result<uint32>::Success(index);
}

void AssetManager::Impl::FreePrimary(uint32 index) noexcept {
    ManagerPrimary& record = primaries[index];
    for (uint32 ticket : record.tickets) {
        EndTicket(ticket, ErrorCode::None);
        FreeTicket(ticket);
    }

    record.tickets.Clear();
    record.callback = PrimaryReadyCallback();
    record.used = false;
    record.nextFree = freePrimary;
    freePrimary = index;
}

// 묶음을 앞당겨 끝낸다(취소·scope 회수·종료): 구성 요청과 관심을 놓고 아직 부르지 않은 callback이
// 받을 결과를 reason으로 정한다.
void AssetManager::Impl::EndPrimary(uint32 index, ErrorCode reason) noexcept {
    ManagerPrimary& record = primaries[index];
    for (uint32 ticket : record.tickets) {
        EndTicket(ticket, reason);
    }

    // 묶음 하나는 한 번만 앞당겨 끝난다(놓기는 끝낸 묶음 칸을 비우고, 회수는 칸을 회수 표시한다).
    record.stop = reason;
}

// 묶음 티켓 객체가 소멸했다(해제 큐): 끝나기 전이면 취소, 끝났으면 관심을 놓는다.
void AssetManager::Impl::ReleasePrimary(uint32 index) noexcept {
    ManagerPrimary& record = primaries[index];
    record.released = true;
    record.slot = ManagerNoIndex;
    bool done = true;
    for (uint32 ticket : record.tickets) {
        done = done && tickets[ticket].done;
    }

    if (!done || (record.callbackRegistered && !record.callbackDelivered)) {
        EndPrimary(index, ErrorCode::Cancelled);
    } else {
        for (uint32 ticket : record.tickets) {
            ReleaseTicketHold(ticket);
        }
    }

    if (!(record.callbackRegistered && !record.callbackDelivered)) {
        FreePrimary(index);
    }
}

// 끝난 묶음의 callback을 부른다(11.7절 4 단계 8). 처음 실패는 대표 → 구성원 순이다.
uint32 AssetManager::Impl::DeliverPrimary() noexcept {
    deliver.Clear();
    for (usize index = 0; index < primaries.Size(); ++index) {
        const ManagerPrimary& record = primaries[index];
        if (!record.used || !record.callbackRegistered || record.callbackDelivered) {
            continue;
        }

        bool done = true;
        for (uint32 ticket : record.tickets) {
            done = done && tickets[ticket].done;
        }

        // 앞당겨 끝낸 묶음(EndPrimary)도 구성 티켓이 모두 끝나 있다.
        if (done) {
            // 용량은 묶음 표가 자랄 때 잡았다(TakePrimary).
            (void)deliver.TryPushBack(static_cast<uint32>(index));
        }
    }

    uint32 delivered = 0;
    for (uint32 index : deliver) {
        PrimaryReadyCallback callback = Move(primaries[index].callback);
        primaries[index].callbackDelivered = true;
        AssetDiagnostic diagnostic;
        ErrorCode error = primaries[index].stop;
        for (usize position = 0;
             error == ErrorCode::None && position < primaries[index].tickets.Size(); ++position) {
            const ManagerTicket& ticket = tickets[primaries[index].tickets[position]];
            if (ticket.error != ErrorCode::None) {
                error = ticket.error;
                diagnostic = ticket.diagnostic;
            }
        }

        Result<PrimaryAssetLease> result = Result<PrimaryAssetLease>::Failure(error);
        if (error == ErrorCode::None) {
            PrimaryAssetLease lease(allocator);
            if (auto reserved = lease.leases_.TryReserve(primaries[index].tickets.Size());
                !reserved) {
                result = Result<PrimaryAssetLease>::Failure(reserved.Error());
            } else {
                ErrorCode made = ErrorCode::None;
                for (uint32 ticket : primaries[index].tickets) {
                    auto one = MakeLease(tickets[ticket].heldPackage, primaries[index].scope);
                    if (!one) {
                        made = one.Error();
                        break;
                    }

                    (void)lease.leases_.TryPushBack(Move(one.Value()));
                }

                result = made == ErrorCode::None ? Result<PrimaryAssetLease>::Success(Move(lease))
                                                 : Result<PrimaryAssetLease>::Failure(made);
            }
        }

        inCallback = true;
        callback(Move(result), diagnostic);
        inCallback = false;
        ++delivered;
        if (primaries[index].released) {
            FreePrimary(index);
        }
    }

    return delivered;
}

Result<PrimaryAssetTicket> AssetManager::LoadPrimaryAsset(AssetId asset,
                                                          Span<const StringView> bundles,
                                                          LoadPriority priority, AssetScope* scope,
                                                          AssetDiagnostic* failure) noexcept {
    if (impl_ == nullptr) {
        return Result<PrimaryAssetTicket>::Failure(ErrorCode::InvalidArgument);
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<PrimaryAssetTicket>::Failure(ErrorCode::WrongThread);
    }

    if (impl.shutDown) {
        return Result<PrimaryAssetTicket>::Failure(ErrorCode::ShuttingDown);
    }

    if (scope != nullptr && !scope->IsValid()) {
        return Result<PrimaryAssetTicket>::Failure(ErrorCode::InvalidArgument);
    }

    if (scope != nullptr && !impl.ScopeOpen(scope->index_)) {
        return Result<PrimaryAssetTicket>::Failure(ErrorCode::ScopeClosed);
    }

    Array<AssetId> assets(impl.allocator);
    if (auto resolved = impl.ResolvePrimary(asset, bundles, assets, failure); !resolved) {
        return Result<PrimaryAssetTicket>::Failure(resolved.Error());
    }

    auto record = impl.TakePrimary();
    if (!record) {
        return Result<PrimaryAssetTicket>::Failure(record.Error());
    }

    const uint32 index = record.Value();
    auto undo = [&impl, index]() noexcept { impl.FreePrimary(index); };
    if (auto reserved = impl.primaries[index].tickets.TryReserve(assets.Size()); !reserved) {
        undo();
        return Result<PrimaryAssetTicket>::Failure(reserved.Error());
    }

    auto slot = impl.TakeSlot(ManagerSlotKind::PrimaryTicket);
    if (!slot) {
        undo();
        return Result<PrimaryAssetTicket>::Failure(slot.Error());
    }

    for (AssetId member : assets) {
        AssetRequest request;
        request.id = member;
        request.priority = priority;
        request.scope = scope;
        auto ticket = impl.StartTicket(request, failure);
        if (!ticket) {
            impl.Slot(slot.Value()).objectAlive = false;
            platform::AtomicFetchSub<platform::MemoryOrder::Relaxed>(&impl.liveObjects, 1u);
            impl.FreeSlot(slot.Value());
            undo();
            return Result<PrimaryAssetTicket>::Failure(ticket.Error());
        }

        (void)impl.primaries[index].tickets.TryPushBack(ticket.Value());
    }

    ManagerPrimary& made = impl.primaries[index];
    made.slot = slot.Value();
    made.scope = scope == nullptr ? ManagerNoIndex : scope->index_;
    ManagerSlot& handle = impl.Slot(slot.Value());
    handle.record = index;
    handle.scope = made.scope;
    return Result<PrimaryAssetTicket>::Success(
        PrimaryAssetTicket(this, slot.Value(), handle.generation));
}

Result<void> AssetManager::OnReady(PrimaryAssetTicket& ticket, TaskLane lane,
                                   PrimaryReadyCallback&& callback) noexcept {
    if (impl_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (ticket.manager_ != this ||
        !impl.ValidSlot(ticket.slot_, ticket.generation_, ManagerSlotKind::PrimaryTicket)) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    ManagerPrimary& record = impl.primaries[impl.Slot(ticket.slot_).record];
    if (record.callbackRegistered) {
        return Result<void>::Failure(ErrorCode::AlreadyRegistered);
    }

    if (lane != TaskLane::Game) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (!callback) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    record.callback = Move(callback);
    record.callbackRegistered = true;
    return Result<void>::Success();
}

Result<AssetFootprint>
AssetManager::EstimatePrimaryAsset(AssetId asset, Span<const StringView> bundles) const noexcept {
    if (impl_ == nullptr) {
        return Result<AssetFootprint>::Failure(ErrorCode::InvalidArgument);
    }

    const Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<AssetFootprint>::Failure(ErrorCode::WrongThread);
    }

    Array<AssetId> starts(impl.allocator);
    if (auto resolved = impl.ResolvePrimary(asset, bundles, starts, nullptr); !resolved) {
        return Result<AssetFootprint>::Failure(resolved.Error());
    }

    const CookedManifest& manifest = *impl.loaderConfig.manifest;
    Array<uint8> visited(impl.allocator);
    Array<uint32> stack(impl.allocator);
    if (auto sized = visited.TryResize(manifest.entries.Size()); !sized) {
        return Result<AssetFootprint>::Failure(sized.Error());
    }

    if (auto reserved = stack.TryReserve(manifest.entries.Size()); !reserved) {
        return Result<AssetFootprint>::Failure(reserved.Error());
    }

    for (usize index = 0; index < visited.Size(); ++index) {
        visited[index] = 0;
    }

    AssetFootprint footprint;
    auto visit = [&](AssetId id) noexcept {
        const CookedManifestEntry* entry = manifest.Find(id);
        if (entry == nullptr) {
            return;
        }

        const auto index = static_cast<uint32>(entry - manifest.entries.Data());
        if (visited[index] == 0) {
            visited[index] = 1;
            // 항목마다 한 번만 넣으므로 용량(항목 수)을 넘지 않는다.
            (void)stack.TryPushBack(index);
        }
    };
    for (AssetId start : starts) {
        visit(start);
    }

    while (!stack.IsEmpty()) {
        const CookedManifestEntry& entry = manifest.entries[stack.TryPopBack().Value()];
        ++footprint.packages;
        footprint.packageBytes += entry.packageBytes;
        footprint.bulkRawBytes += entry.bulkRawBytes;
        for (const CookedManifestDependency& dependency : entry.dependencies) {
            if (dependency.kind == PackageDependencyKind::Hard) {
                visit(dependency.asset);
            }
        }
    }

    return Result<AssetFootprint>::Success(footprint);
}

PrimaryAssetTicket::PrimaryAssetTicket(PrimaryAssetTicket&& other) noexcept
    : manager_(other.manager_), slot_(other.slot_), generation_(other.generation_) {
    other.manager_ = nullptr;
}

PrimaryAssetTicket& PrimaryAssetTicket::operator=(PrimaryAssetTicket&& other) noexcept {
    if (this != &other) {
        Drop();
        manager_ = other.manager_;
        slot_ = other.slot_;
        generation_ = other.generation_;
        other.manager_ = nullptr;
    }

    return *this;
}

PrimaryAssetTicket::~PrimaryAssetTicket() {
    Drop();
}

void PrimaryAssetTicket::Drop() noexcept {
    if (manager_ != nullptr) {
        manager_->impl_->PostRelease(slot_);
        manager_ = nullptr;
    }
}

} // namespace rocks
