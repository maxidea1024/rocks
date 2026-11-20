#include "AssetManagerInternal.h"

#include <Core/Move.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>

// AssetManager 공개 API와 handle 클래스다(Docs/Design/AssetSystem.ko.md 11.7절).
namespace rocks {

AssetManager::Impl::Impl(AssetManager& owner, const AssetManagerConfig& config) noexcept
    : owner(owner), config(config), allocator(*config.loader.allocator),
      loaderConfig(config.loader), tickets(allocator), deferred(allocator), scopes(allocator),
      primaries(allocator), packages(allocator), entryPackage(allocator), packageOf(allocator),
      units(allocator), edges(allocator), edgeNodeOf(allocator), edgeNodes(allocator),
      freeEdgeNodes(allocator), published(allocator), changes(allocator), workStack(allocator),
      mergeSet(allocator), cycleOwners(allocator), cycleTargets(allocator),
      scratchAssets(allocator), scratchSlots(allocator), deliver(allocator) {
}

AssetManager::Impl::~Impl() {
    // 티켓·보류 명령의 Function과 표를 먼저 비운 뒤 로더(공개 패키지 RootRef) → Registry → 관찰자
    // 순으로 놓는다(Registry가 관찰자보다 먼저 사라져야 한다).
    tickets.Clear();
    deferred.Clear();
    primaries.Clear();
    loader.Reset();
    registry.Reset();
    observer.Reset();
    for (uint32 index = 0; index < blockCount; ++index) {
        blocks[index]->~ManagerSlotBlock();
        (void)allocator.Deallocate(blocks[index]);
    }
}

AssetManager::AssetManager(const AssetManagerConfig& config) noexcept : config_(config) {
}

AssetManager::~AssetManager() {
    if (impl_ == nullptr) {
        return;
    }

    if (!impl_->OnOwnerThread()) {
        platform::Fatal("AssetManager destroyed outside its owner thread");
    }

    if (impl_->inCallback) {
        platform::Fatal("AssetManager destroyed inside its own callback");
    }

    (void)Shutdown();
    // 종료 순서(World → lease·scope → AssetManager — 11.7절 6) 위반은 조용히 넘기지 않는다: 큐가
    // 사라진 뒤의 소멸은 해제된 메모리를 쓴다.
    if (platform::AtomicLoad<platform::MemoryOrder::Acquire>(&impl_->liveObjects) != 0) {
        platform::Fatal("AssetManager destroyed while leases, tickets or commands are alive");
    }

    Allocator& allocator = *config_.loader.allocator;
    impl_->~Impl();
    (void)allocator.Deallocate(impl_);
    impl_ = nullptr;
}

Result<void> AssetManager::Initialize(AssetDiagnostic* failure) noexcept {
    auto fail = [failure](ErrorCode error) noexcept {
        if (failure != nullptr) {
            *failure = AssetDiagnostic{};
            failure->error = error;
        }

        return Result<void>::Failure(error);
    };
    const AssetLoaderConfig& loader = config_.loader;
    if (impl_ != nullptr || loader.allocator == nullptr || loader.types == nullptr ||
        loader.manifest == nullptr || loader.registry != nullptr || config_.observerMaxKeys == 0 ||
        config_.maxDeferredCommands == 0) {
        return fail(ErrorCode::InvalidArgument);
    }

    Allocator& allocator = *loader.allocator;
    auto memory = allocator.Allocate(sizeof(Impl), alignof(Impl));
    if (!memory) {
        return fail(memory.Error());
    }

    impl_ = InitializeAt<Impl>(memory.Value(), *this, config_);
    Impl& impl = *impl_;
    impl.ownerThread = platform::CurrentThreadId();
    auto discard = [this, &allocator]() noexcept {
        impl_->~Impl();
        (void)allocator.Deallocate(impl_);
        impl_ = nullptr;
    };
    auto observer =
        UniquePtr<ReferenceObserver>::TryMake(allocator, allocator, config_.observerMaxKeys);
    if (!observer) {
        discard();
        return fail(observer.Error());
    }

    impl.observer = Move(observer.Value());
    RegistryOptions options = config_.registryOptions;
    options.policy.packageNativeType = NativeTypeKey<Package>();
    options.policy.rejectWorldOnly = true;
    options.referenceObserver = impl.observer.Get();
    auto registry = UniquePtr<ObjectRegistry>::TryMake(allocator, allocator, *loader.types,
                                                       config_.registryContext, options);
    if (!registry) {
        discard();
        return fail(registry.Error());
    }

    impl.registry = Move(registry.Value());
    impl.access = impl.registry->OwnerAccess();
    impl.loaderConfig.registry = impl.registry.Get();
    auto made = UniquePtr<AssetLoader>::TryMake(allocator, impl.loaderConfig);
    if (!made) {
        discard();
        return fail(made.Error());
    }

    impl.loader = Move(made.Value());
    if (auto initialized = impl.loader->Initialize(failure); !initialized) {
        discard();
        return initialized;
    }

    const usize entries = loader.manifest->entries.Size();
    if (auto sized = impl.entryPackage.TryResize(entries); !sized) {
        discard();
        return fail(sized.Error());
    }

    for (usize index = 0; index < entries; ++index) {
        impl.entryPackage[index] = ManagerNoIndex;
    }

    return Result<void>::Success();
}

ObjectRegistry& AssetManager::Registry() noexcept {
    return *impl_->registry;
}

AssetLoader& AssetManager::Loader() noexcept {
    return *impl_->loader;
}

Result<AssetScope> AssetManager::CreateScope() noexcept {
    if (impl_ == nullptr) {
        return Result<AssetScope>::Failure(ErrorCode::InvalidArgument);
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<AssetScope>::Failure(ErrorCode::WrongThread);
    }

    if (impl.shutDown) {
        return Result<AssetScope>::Failure(ErrorCode::ShuttingDown);
    }

    if (impl.freeScope == ManagerNoIndex) {
        if (auto added = impl.scopes.TryEmplaceBack(); !added) {
            return Result<AssetScope>::Failure(added.Error());
        }

        impl.freeScope = static_cast<uint32>(impl.scopes.Size() - 1);
        impl.scopes[impl.freeScope].nextFree = ManagerNoIndex;
    }

    const uint32 index = impl.freeScope;
    ManagerScope& scope = impl.scopes[index];
    impl.freeScope = scope.nextFree;
    scope.used = true;
    scope.revoked = false;
    return Result<AssetScope>::Success(AssetScope(this, index, scope.generation));
}

namespace {

// 요청의 공통 확인이다(RequestAsset·LoadBlocking).
ErrorCode CheckRequest(const AssetRequest& request, const AssetManager::Impl& impl) noexcept {
    if (impl.shutDown) {
        return ErrorCode::ShuttingDown;
    }

    if (request.scope != nullptr && !request.scope->IsValid()) {
        return ErrorCode::InvalidArgument;
    }

    constexpr uint8 known =
        static_cast<uint8>(ReadyMask::Cpu) | static_cast<uint8>(ReadyMask::Render);
    if (request.required == 0 || (request.required & ~known) != 0) {
        return ErrorCode::InvalidArgument;
    }

    if ((request.required & static_cast<uint8>(ReadyMask::Render)) != 0) {
        return ErrorCode::UnsupportedCapability;
    }

    return ErrorCode::None;
}

} // namespace

Result<LoadTicket> AssetManager::RequestAsset(const AssetRequest& request,
                                              AssetDiagnostic* failure) noexcept {
    if (impl_ == nullptr) {
        return Result<LoadTicket>::Failure(ErrorCode::InvalidArgument);
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<LoadTicket>::Failure(ErrorCode::WrongThread);
    }

    if (const ErrorCode error = CheckRequest(request, impl); error != ErrorCode::None) {
        return Result<LoadTicket>::Failure(error);
    }

    if (request.scope != nullptr && !impl.ScopeOpen(request.scope->index_)) {
        return Result<LoadTicket>::Failure(ErrorCode::ScopeClosed);
    }

    auto slot = impl.TakeSlot(ManagerSlotKind::Ticket);
    if (!slot) {
        return Result<LoadTicket>::Failure(slot.Error());
    }

    auto started = impl.StartTicket(request, failure);
    if (!started) {
        impl.Slot(slot.Value()).objectAlive = false;
        platform::AtomicFetchSub<platform::MemoryOrder::Relaxed>(&impl.liveObjects, 1u);
        impl.FreeSlot(slot.Value());
        return Result<LoadTicket>::Failure(started.Error());
    }

    ManagerSlot& made = impl.Slot(slot.Value());
    made.record = started.Value();
    made.scope = impl.tickets[started.Value()].scope;
    impl.tickets[started.Value()].slot = slot.Value();
    return Result<LoadTicket>::Success(LoadTicket(this, slot.Value(), made.generation));
}

Result<void> AssetManager::OnReady(LoadTicket& ticket, TaskLane lane,
                                   AssetReadyCallback&& callback) noexcept {
    if (impl_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (ticket.manager_ != this ||
        !impl.ValidSlot(ticket.slot_, ticket.generation_, ManagerSlotKind::Ticket)) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    ManagerTicket& record = impl.tickets[impl.Slot(ticket.slot_).record];
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

Result<AssetLease> AssetManager::LoadBlocking(const AssetRequest& request, const char* site,
                                              AssetDiagnostic* failure) noexcept {
    if (impl_ == nullptr) {
        return Result<AssetLease>::Failure(ErrorCode::InvalidArgument);
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<AssetLease>::Failure(ErrorCode::WrongThread);
    }

    const AssetLoaderConfig& loader = config_.loader;
    if (impl.inCallback || (loader.inFrame != nullptr && loader.inFrame(loader.inFrameContext))) {
        return Result<AssetLease>::Failure(ErrorCode::WrongContext);
    }

    if (const ErrorCode error = CheckRequest(request, impl); error != ErrorCode::None) {
        return Result<AssetLease>::Failure(error);
    }

    if (request.scope != nullptr && !impl.ScopeOpen(request.scope->index_)) {
        return Result<AssetLease>::Failure(ErrorCode::ScopeClosed);
    }

    auto started = impl.StartTicket(request, failure);
    if (!started) {
        return Result<AssetLease>::Failure(started.Error());
    }

    const uint32 index = started.Value();
    if (!impl.tickets[index].done) {
        auto waited = impl.loader->WaitFor(impl.tickets[index].loaderHandle, site);
        if (!waited) {
            impl.EndTicket(index, waited.Error());
        } else {
            impl.FinishTicketResult(index, waited.Value());
        }
    }

    ManagerTicket& ticket = impl.tickets[index];
    Result<AssetLease> result = Result<AssetLease>::Failure(ticket.error);
    if (ticket.error == ErrorCode::None) {
        result = impl.MakeLease(ticket.heldPackage, ticket.scope);
    } else if (failure != nullptr) {
        *failure = ticket.diagnostic;
    }

    impl.EndTicket(index, ErrorCode::None);
    impl.FreeTicket(index);
    return result;
}

Result<DeferredCommand> AssetManager::RunWhenReady(AssetId asset, NotReadyPolicy policy,
                                                   AssetScope* scope, DeferredRun&& run,
                                                   DeferredFail&& fail) noexcept {
    if (impl_ == nullptr) {
        return Result<DeferredCommand>::Failure(ErrorCode::InvalidArgument);
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<DeferredCommand>::Failure(ErrorCode::WrongThread);
    }

    if (impl.shutDown) {
        return Result<DeferredCommand>::Failure(ErrorCode::ShuttingDown);
    }

    if (!run || !fail ||
        static_cast<uint32>(policy) > static_cast<uint32>(NotReadyPolicy::PromoteAndDefer) ||
        (scope != nullptr && !scope->IsValid())) {
        return Result<DeferredCommand>::Failure(ErrorCode::InvalidArgument);
    }

    if (scope != nullptr && !impl.ScopeOpen(scope->index_)) {
        return Result<DeferredCommand>::Failure(ErrorCode::ScopeClosed);
    }

    if (impl.activeDeferred >= config_.maxDeferredCommands) {
        return Result<DeferredCommand>::Failure(ErrorCode::CapacityExceeded);
    }

    const CookedManifestEntry* entry = config_.loader.manifest->Find(asset);
    if (entry == nullptr) {
        return Result<DeferredCommand>::Failure(ErrorCode::NotFound);
    }

    if (policy == NotReadyPolicy::Fail) {
        const auto index = static_cast<uint32>(entry - config_.loader.manifest->entries.Data());
        if (impl.entryPackage[index] == ManagerNoIndex) {
            return Result<DeferredCommand>::Failure(ErrorCode::NotReady);
        }
    }

    if (impl.freeDeferred == ManagerNoIndex) {
        if (auto added = impl.deferred.TryEmplaceBack(); !added) {
            return Result<DeferredCommand>::Failure(added.Error());
        }

        impl.freeDeferred = static_cast<uint32>(impl.deferred.Size() - 1);
        impl.deferred[impl.freeDeferred].nextFree = ManagerNoIndex;
    }

    auto slot = impl.TakeSlot(ManagerSlotKind::Deferred);
    if (!slot) {
        return Result<DeferredCommand>::Failure(slot.Error());
    }

    AssetRequest request;
    request.id = asset;
    request.priority =
        policy == NotReadyPolicy::PromoteAndDefer ? LoadPriority::High : LoadPriority::Normal;
    request.scope = scope;
    auto ticket = impl.StartTicket(request, nullptr);
    if (!ticket) {
        impl.Slot(slot.Value()).objectAlive = false;
        platform::AtomicFetchSub<platform::MemoryOrder::Relaxed>(&impl.liveObjects, 1u);
        impl.FreeSlot(slot.Value());
        return Result<DeferredCommand>::Failure(ticket.Error());
    }

    const uint32 index = impl.freeDeferred;
    ManagerDeferred& command = impl.deferred[index];
    impl.freeDeferred = command.nextFree;
    command = ManagerDeferred{};
    command.used = true;
    command.slot = slot.Value();
    command.ticket = ticket.Value();
    command.scope = scope == nullptr ? ManagerNoIndex : scope->index_;
    command.run = Move(run);
    command.fail = Move(fail);
    ++impl.activeDeferred;
    ManagerSlot& made = impl.Slot(slot.Value());
    made.record = index;
    made.scope = command.scope;
    return Result<DeferredCommand>::Success(DeferredCommand(this, slot.Value(), made.generation));
}

Result<void> AssetManager::Cancel(DeferredCommand& command) noexcept {
    if (impl_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (command.manager_ != this ||
        !impl.ValidSlot(command.slot_, command.generation_, ManagerSlotKind::Deferred)) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    ManagerDeferred& record = impl.deferred[impl.Slot(command.slot_).record];
    if (!record.done && record.stop == ErrorCode::None) {
        record.stop = ErrorCode::Cancelled;
    }

    return Result<void>::Success();
}

Result<ObjectAccessContext> AssetManager::BeginWith(const AssetLease& lease) const noexcept {
    if (impl_ == nullptr) {
        return Result<ObjectAccessContext>::Failure(ErrorCode::InvalidArgument);
    }

    if (!impl_->OnOwnerThread()) {
        return Result<ObjectAccessContext>::Failure(ErrorCode::WrongThread);
    }

    if (lease.manager_ != this ||
        !impl_->ValidSlot(lease.slot_, lease.generation_, ManagerSlotKind::Lease) ||
        impl_->Slot(lease.slot_).revoked) {
        return Result<ObjectAccessContext>::Failure(ErrorCode::StaleHandle);
    }

    return Result<ObjectAccessContext>::Success(impl_->access);
}

Result<AssetManagerPumpResult> AssetManager::Pump(const AssetManagerPumpBudget& budget) noexcept {
    if (impl_ == nullptr) {
        return Result<AssetManagerPumpResult>::Failure(ErrorCode::InvalidArgument);
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<AssetManagerPumpResult>::Failure(ErrorCode::WrongThread);
    }

    if (impl.inCallback) {
        return Result<AssetManagerPumpResult>::Failure(ErrorCode::WrongContext);
    }

    if (impl.shutDown) {
        return Result<AssetManagerPumpResult>::Failure(ErrorCode::ShuttingDown);
    }

    AssetManagerPumpResult result;
    // (1) 해제 큐 → (2) 로더 → (3) 넘겨받기 → (4) 간선 → (5) 티켓 → (6) 판정 → (7)
    // Closing·Discarding → (8) callback·보류 명령(11.7절 4).
    impl.DrainReleases();
    auto pumped = impl.loader->Pump(budget.loader);
    if (!pumped) {
        return Result<AssetManagerPumpResult>::Failure(pumped.Error());
    }

    result.loader = pumped.Value();
    impl.createdUnits = 0;
    (void)impl.IngestPublished();
    result.residentUnitsCreated = impl.createdUnits;
    const bool edgesApplied = static_cast<bool>(impl.ApplyEdgeChanges());
    impl.CompleteTickets();
    if (edgesApplied && impl.scratchAssets.TryReserve(impl.packages.Size()) &&
        impl.scratchSlots.TryReserve(impl.packages.Size())) {
        impl.JudgeClosing();
        result.discardedPackages = impl.ProcessClosing(budget.maxDiscards);
    }

    if (impl.deliver.TryReserve(impl.tickets.Size())) {
        result.callbacks = impl.DeliverCallbacks();
    }

    if (impl.deliver.TryReserve(impl.primaries.Size())) {
        result.callbacks += impl.DeliverPrimary();
    }

    result.deferredCommands = impl.RunDeferred();
    return Result<AssetManagerPumpResult>::Success(result);
}

Result<void> AssetManager::Describe(AssetManagerSnapshot& out) const noexcept {
    if (impl_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (auto described = impl.loader->Describe(out.loader); !described) {
        return described;
    }

    out.units.Clear();
    out.unitAssets.Clear();
    for (usize index = 0; index < impl.units.Size(); ++index) {
        const ManagerUnit& unit = impl.units[index];
        if (!unit.used) {
            continue;
        }

        AssetManagerUnitInfo info;
        info.firstAsset = static_cast<uint32>(out.unitAssets.Size());
        info.assetCount = unit.packageCount;
        info.state = unit.state;
        info.tickets = unit.tickets;
        info.leases = unit.leases;
        info.deferred = unit.deferred;
        info.loaderHolds = impl.LoaderHolds(static_cast<uint32>(index));
        info.inboundEdges = unit.inbound;
        for (uint32 member = unit.firstPackage; member != ManagerNoIndex;
             member = impl.packages[member].nextInUnit) {
            if (auto pushed = out.unitAssets.TryPushBack(impl.packages[member].asset); !pushed) {
                return pushed;
            }
        }

        if (auto pushed = out.units.TryPushBack(info); !pushed) {
            return pushed;
        }
    }

    out.liveTickets = 0;
    out.liveLeases = 0;
    for (uint32 index = 0; index < impl.blockCount * ManagerSlotsPerBlock; ++index) {
        const ManagerSlot& slot = impl.Slot(index);
        if (slot.objectAlive && slot.kind == ManagerSlotKind::Ticket) {
            ++out.liveTickets;
        } else if (slot.objectAlive && slot.kind == ManagerSlotKind::Lease && !slot.revoked) {
            ++out.liveLeases;
        }
    }

    out.deferredCommands = impl.activeDeferred;
    out.lateEdgesIntoDiscarding = impl.lateEdgesIntoDiscarding;
    return Result<void>::Success();
}

Result<void> AssetManager::Shutdown() noexcept {
    if (impl_ == nullptr) {
        return Result<void>::Success();
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (impl.inCallback) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    if (impl.shutDown) {
        return Result<void>::Success();
    }

    // 모든 티켓·lease·보류 명령을 회수하고 아직 부르지 않은 callback·fail에 ShuttingDown을 한 번
    // 전달한다(11.7절 12). 그 뒤 해제 큐를 비운다(이미 소멸한 handle의 슬롯).
    impl.RevokeScope(ManagerNoIndex, ErrorCode::ShuttingDown);
    if (impl.deliver.TryReserve(impl.tickets.Size())) {
        (void)impl.DeliverCallbacks();
    }

    if (impl.deliver.TryReserve(impl.primaries.Size())) {
        (void)impl.DeliverPrimary();
    }

    (void)impl.RunDeferred();
    impl.DrainReleases();
    impl.shutDown = true;
    (void)impl.loader->Shutdown();
    return Result<void>::Success();
}

// ---- handle 클래스 ----

AssetLease::AssetLease(AssetLease&& other) noexcept
    : manager_(other.manager_), slot_(other.slot_), generation_(other.generation_) {
    other.manager_ = nullptr;
}

AssetLease& AssetLease::operator=(AssetLease&& other) noexcept {
    if (this != &other) {
        Drop();
        manager_ = other.manager_;
        slot_ = other.slot_;
        generation_ = other.generation_;
        other.manager_ = nullptr;
    }

    return *this;
}

AssetLease::~AssetLease() {
    Drop();
}

void AssetLease::Drop() noexcept {
    if (manager_ != nullptr) {
        manager_->impl_->PostRelease(slot_);
        manager_ = nullptr;
    }
}

bool AssetLease::IsValid() const noexcept {
    return manager_ != nullptr &&
           manager_->impl_->ValidSlot(slot_, generation_, ManagerSlotKind::Lease) &&
           !manager_->impl_->Slot(slot_).revoked;
}

AssetId AssetLease::Asset() const noexcept {
    return IsValid() ? manager_->impl_->Slot(slot_).asset : AssetId{};
}

ObjectHandle AssetLease::Object() const noexcept {
    return IsValid() ? manager_->impl_->Slot(slot_).assetObject : ObjectHandle{};
}

ObjectHandle AssetLease::Package() const noexcept {
    return IsValid() ? manager_->impl_->Slot(slot_).packageHandle : ObjectHandle{};
}

LoadTicket::LoadTicket(LoadTicket&& other) noexcept
    : manager_(other.manager_), slot_(other.slot_), generation_(other.generation_) {
    other.manager_ = nullptr;
}

LoadTicket& LoadTicket::operator=(LoadTicket&& other) noexcept {
    if (this != &other) {
        Drop();
        manager_ = other.manager_;
        slot_ = other.slot_;
        generation_ = other.generation_;
        other.manager_ = nullptr;
    }

    return *this;
}

LoadTicket::~LoadTicket() {
    Drop();
}

void LoadTicket::Drop() noexcept {
    if (manager_ != nullptr) {
        manager_->impl_->PostRelease(slot_);
        manager_ = nullptr;
    }
}

DeferredCommand::DeferredCommand(DeferredCommand&& other) noexcept
    : manager_(other.manager_), slot_(other.slot_), generation_(other.generation_) {
    other.manager_ = nullptr;
}

DeferredCommand& DeferredCommand::operator=(DeferredCommand&& other) noexcept {
    if (this != &other) {
        Drop();
        manager_ = other.manager_;
        slot_ = other.slot_;
        generation_ = other.generation_;
        other.manager_ = nullptr;
    }

    return *this;
}

DeferredCommand::~DeferredCommand() {
    Drop();
}

void DeferredCommand::Drop() noexcept {
    if (manager_ != nullptr) {
        manager_->impl_->PostRelease(slot_);
        manager_ = nullptr;
    }
}

AssetScope::AssetScope(AssetScope&& other) noexcept
    : manager_(other.manager_), index_(other.index_), generation_(other.generation_) {
    other.manager_ = nullptr;
}

AssetScope& AssetScope::operator=(AssetScope&& other) noexcept {
    if (this != &other) {
        Close();
        manager_ = other.manager_;
        index_ = other.index_;
        generation_ = other.generation_;
        other.manager_ = nullptr;
    }

    return *this;
}

AssetScope::~AssetScope() {
    Close();
}

void AssetScope::Close() noexcept {
    if (manager_ == nullptr) {
        return;
    }

    AssetManager::Impl& impl = *manager_->impl_;
    if (!impl.OnOwnerThread()) {
        platform::Fatal("AssetScope destroyed outside the AssetManager owner thread");
    }

    // 소멸은 Revoke와 같고, 칸은 그 뒤 다시 쓴다(세대를 올린다).
    if (!impl.scopes[index_].revoked && !impl.shutDown) {
        impl.RevokeScope(index_, ErrorCode::ScopeClosed);
    }

    ManagerScope& scope = impl.scopes[index_];
    scope.used = false;
    scope.revoked = false;
    scope.generation = scope.generation + 1 == 0 ? 1 : scope.generation + 1;
    scope.nextFree = impl.freeScope;
    impl.freeScope = index_;
    manager_ = nullptr;
}

Result<void> AssetScope::Revoke() noexcept {
    if (manager_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    AssetManager::Impl& impl = *manager_->impl_;
    if (!impl.OnOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (!impl.scopes[index_].revoked && !impl.shutDown) {
        impl.RevokeScope(index_, ErrorCode::ScopeClosed);
    }

    return Result<void>::Success();
}

} // namespace rocks
