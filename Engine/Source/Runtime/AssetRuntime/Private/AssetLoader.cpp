#include "AssetLoaderInternal.h"

// AssetLoader 공개 API와 이름 함수다(Docs/Design/AssetSystem 11.6절).
namespace rocks {

Result<void> WorkerSchedulerLoaderWorker::Run(Job job, void* context) noexcept {
    auto body = Function<void(WorkerTaskContext&)>::TryCreate(
        allocator_, [job, context](WorkerTaskContext&) noexcept { job(context); });
    if (!body) {
        return Result<void>::Failure(body.Error());
    }

    auto submitted = scheduler_.Submit(Move(body.Value()));
    if (!submitted) {
        return Result<void>::Failure(submitted.Error());
    }

    return Result<void>::Success();
}

const char* LoadPriorityName(LoadPriority priority) noexcept {
    switch (priority) {
    case LoadPriority::Low:
        return "Low";
    case LoadPriority::Normal:
        return "Normal";
    case LoadPriority::High:
        return "High";
    case LoadPriority::Critical:
        return "Critical";
    }

    return "Unknown";
}

const char* AssetLoadStateName(AssetLoadState state) noexcept {
    switch (state) {
    case AssetLoadState::Pending:
        return "Pending";
    case AssetLoadState::Succeeded:
        return "Succeeded";
    case AssetLoadState::Failed:
        return "Failed";
    case AssetLoadState::Cancelled:
        return "Cancelled";
    }

    return "Unknown";
}

const char* AssetLoaderUnitPhaseName(AssetLoaderUnitPhase phase) noexcept {
    switch (phase) {
    case AssetLoaderUnitPhase::Queued:
        return "Queued";
    case AssetLoaderUnitPhase::Loading:
        return "Loading";
    case AssetLoaderUnitPhase::Ready:
        return "Ready";
    case AssetLoaderUnitPhase::Abandoned:
        return "Abandoned";
    case AssetLoaderUnitPhase::Failed:
        return "Failed";
    case AssetLoaderUnitPhase::PostLoading:
        return "PostLoading";
    }

    return "Unknown";
}

const char* AssetLoaderPackagePhaseName(AssetLoaderPackagePhase phase) noexcept {
    switch (phase) {
    case AssetLoaderPackagePhase::Queued:
        return "Queued";
    case AssetLoaderPackagePhase::Reading:
        return "Reading";
    case AssetLoaderPackagePhase::Preparing:
        return "Preparing";
    case AssetLoaderPackagePhase::Prepared:
        return "Prepared";
    case AssetLoaderPackagePhase::Count:
        break;
    }

    return "Unknown";
}

const char* AssetLoaderWaitName(AssetLoaderWait wait) noexcept {
    switch (wait) {
    case AssetLoaderWait::None:
        return "None";
    case AssetLoaderWait::Admission:
        return "Admission";
    case AssetLoaderWait::DependencyAdmission:
        return "DependencyAdmission";
    case AssetLoaderWait::Dependency:
        return "Dependency";
    case AssetLoaderWait::Io:
        return "Io";
    case AssetLoaderWait::Worker:
        return "Worker";
    case AssetLoaderWait::Pump:
        return "Pump";
    case AssetLoaderWait::PostLoadDependency:
        return "PostLoadDependency";
    case AssetLoaderWait::PostLoad:
        return "PostLoad";
    }

    return "Unknown";
}

AssetLoader::AssetLoader(const AssetLoaderConfig& config) noexcept : config_(config) {
}

AssetLoader::~AssetLoader() {
    if (impl_ == nullptr) {
        return;
    }

    if (!impl_->OnOwnerThread()) {
        platform::Fatal("AssetLoader destroyed outside its owner thread");
    }

    // 콜백 안의 파괴는 돌아온 뒤 해제된 상태를 읽는다(11.6절 18).
    if (impl_->inCallback) {
        platform::Fatal("AssetLoader destroyed inside its own callback");
    }

    (void)Shutdown();
    Allocator& allocator = *config_.allocator;
    impl_->~Impl();
    (void)allocator.Deallocate(impl_);
    impl_ = nullptr;
}

Result<void> AssetLoader::Initialize(AssetDiagnostic* failure) noexcept {
    const AssetLoaderConfig& config = config_;
    auto invalid = [failure]() noexcept {
        if (failure != nullptr) {
            *failure = AssetDiagnostic{};
            failure->error = ErrorCode::InvalidArgument;
        }

        return Result<void>::Failure(ErrorCode::InvalidArgument);
    };
    if (impl_ != nullptr || config.allocator == nullptr || config.registry == nullptr ||
        config.types == nullptr || config.manifest == nullptr || config.io == nullptr ||
        config.worker == nullptr || config.limits.maxPackagesInFlight == 0 ||
        config.limits.maxTempBytes == 0 || config.limits.agingAdmissions == 0 ||
        !config.registry->UsesTypeRegistry(*config.types)) {
        return invalid();
    }

    auto memory = config.allocator->Allocate(sizeof(Impl), alignof(Impl));
    if (!memory) {
        if (failure != nullptr) {
            *failure = AssetDiagnostic{};
            failure->error = memory.Error();
        }

        return Result<void>::Failure(memory.Error());
    }

    impl_ = InitializeAt<Impl>(memory.Value(), *this, config_);
    impl_->ownerThread = platform::CurrentThreadId();
    if (auto initialized = impl_->Initialize(failure); !initialized) {
        Allocator& allocator = *config_.allocator;
        impl_->~Impl();
        (void)allocator.Deallocate(impl_);
        impl_ = nullptr;
        return initialized;
    }

    return Result<void>::Success();
}

Result<AssetLoadHandle> AssetLoader::Request(const AssetLoadRequest& request,
                                             AssetDiagnostic* failure) noexcept {
    using Out = AssetLoadHandle;
    auto fail = [failure](ErrorCode error, AssetDiagnosticCode code, AssetId asset) noexcept {
        if (failure != nullptr) {
            *failure = AssetDiagnostic{};
            failure->code = code;
            failure->error = error;
            failure->asset = asset;
        }

        return Result<Out>::Failure(error);
    };
    if (impl_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, AssetDiagnosticCode::None, request.id);
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return fail(ErrorCode::WrongThread, AssetDiagnosticCode::None, request.id);
    }

    if (impl.inCallback) {
        return fail(ErrorCode::WrongContext, AssetDiagnosticCode::None, request.id);
    }

    if (impl.shutDown) {
        return fail(ErrorCode::ShuttingDown, AssetDiagnosticCode::None, request.id);
    }

    if (!LoaderValidPriority(request.priority)) {
        return fail(ErrorCode::InvalidArgument, AssetDiagnosticCode::None, request.id);
    }

    const CookedManifestEntry* found = config_.manifest->Find(request.id);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, AssetDiagnosticCode::MissingDependency, request.id);
    }

    const uint32 entry = impl.EntryIndex(found);
    // 요청 칸을 먼저 잡는다(되돌림이 쉽다).
    uint32 index = impl.freeRequest;
    if (index == LoaderNoIndex) {
        if (auto added = impl.requests.TryEmplaceBack(); !added) {
            return fail(added.Error(), AssetDiagnosticCode::None, request.id);
        }

        index = static_cast<uint32>(impl.requests.Size() - 1);
    } else {
        impl.freeRequest = impl.requests[index].nextFree;
    }

    auto release = [&impl, index]() noexcept {
        impl.requests[index].nextFree = impl.freeRequest;
        impl.freeRequest = index;
    };
    LoaderRequest& slot = impl.requests[index];
    slot.used = true;
    slot.entry = entry;
    slot.priority = request.priority;
    slot.state = AssetLoadState::Pending;
    slot.error = ErrorCode::None;
    slot.diagnostic = AssetDiagnostic{};
    slot.package = ObjectHandle{};
    slot.unit = LoaderNoIndex;
    const AssetLoadHandle handle{index, slot.generation};
    // 공개됐어도 PostLoad를 기다리는 단위면 합류한다(PostLoad가 끝나야 Succeeded).
    uint32 unit = impl.entryUnit[entry];
    if (unit == LoaderNoIndex && impl.entryLoaded[entry] != LoaderNoIndex) {
        slot.state = AssetLoadState::Succeeded;
        slot.package = impl.loadedHandles[impl.entryLoaded[entry]];
        return Result<Out>::Success(handle);
    }

    if (unit == LoaderNoIndex) {
        auto resolved = impl.Resolve(entry);
        if (!resolved) {
            impl.requests[index].used = false;
            release();
            return fail(resolved.Error(), AssetDiagnosticCode::None, request.id);
        }

        unit = resolved.Value();
    } else {
        ErrorCode reserved = ErrorCode::None;
        LoaderKeepFirst(reserved,
                        impl.units[unit].requests.TryReserve(impl.units[unit].requests.Size() + 1));
        LoaderKeepFirst(reserved, impl.workStack.TryReserve(impl.units.Size()));
        if (reserved != ErrorCode::None) {
            impl.requests[index].used = false;
            release();
            return fail(reserved, AssetDiagnosticCode::None, request.id);
        }
    }

    LoaderRequest& pending = impl.requests[index];
    pending.unit = unit;
    pending.cancel = request.cancel;
    (void)impl.units[unit].requests.TryPushBack(index);
    ++impl.units[unit].interest;
    impl.Boost(unit, request.priority);
    return Result<Out>::Success(handle);
}

Result<AssetId> AssetLoader::FindByPath(StringView path) const noexcept {
    if (impl_ == nullptr) {
        return Result<AssetId>::Failure(ErrorCode::InvalidArgument);
    }

    const Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<AssetId>::Failure(ErrorCode::WrongThread);
    }

    StringBuilder<LoaderKeyInlineBytes> key(impl.allocator);
    if (auto made = package_detail::CheckCookedPathHint(path, impl.allocator, key); !made) {
        return Result<AssetId>::Failure(made.Error());
    }

    const StringView wanted = key.View();
    usize low = 0;
    usize high = impl.keyOrder.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (package_detail::Utf8ByteLess(impl.KeyOf(impl.keyOrder[middle]), wanted)) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    if (low == impl.keyOrder.Size() || !(impl.KeyOf(impl.keyOrder[low]) == wanted)) {
        return Result<AssetId>::Failure(ErrorCode::NotFound);
    }

    return Result<AssetId>::Success(impl.EntryAsset(impl.keyOrder[low]));
}

Result<AssetLoadStatus> AssetLoader::Status(AssetLoadHandle handle) const noexcept {
    if (impl_ == nullptr) {
        return Result<AssetLoadStatus>::Failure(ErrorCode::InvalidArgument);
    }

    const Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<AssetLoadStatus>::Failure(ErrorCode::WrongThread);
    }

    if (!impl.ValidHandle(handle)) {
        return Result<AssetLoadStatus>::Failure(ErrorCode::StaleHandle);
    }

    const LoaderRequest& request = impl.requests[handle.index];
    AssetLoadStatus status;
    status.state = request.state;
    status.error = request.error;
    status.diagnostic = request.diagnostic;
    status.package = request.package;
    return Result<AssetLoadStatus>::Success(status);
}

Result<void> AssetLoader::SetPriority(AssetLoadHandle handle, LoadPriority priority) noexcept {
    if (impl_ == nullptr || !LoaderValidPriority(priority)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (impl.inCallback) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    if (!impl.ValidHandle(handle)) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    LoaderRequest& request = impl.requests[handle.index];
    if (request.state != AssetLoadState::Pending) {
        return Result<void>::Success();
    }

    request.priority = priority;
    // Boost의 작업 목록 용량은 요청·해석 때 단위 표 크기 이상으로 잡았다.
    impl.Boost(request.unit, priority);
    return Result<void>::Success();
}

Result<void> AssetLoader::Release(AssetLoadHandle handle) noexcept {
    if (impl_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (impl.inCallback) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    if (!impl.ValidHandle(handle)) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (impl.requests[handle.index].state == AssetLoadState::Pending) {
        impl.CancelRequest(handle.index, ErrorCode::Cancelled);
        impl.ProcessRetire();
    }

    impl.FreeRequest(handle.index);
    return Result<void>::Success();
}

Result<AssetLoaderPumpResult> AssetLoader::Pump(const AssetLoaderPumpBudget& budget) noexcept {
    using Out = AssetLoaderPumpResult;
    if (impl_ == nullptr) {
        return Result<Out>::Failure(ErrorCode::InvalidArgument);
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<Out>::Failure(ErrorCode::WrongThread);
    }

    if (impl.inCallback) {
        return Result<Out>::Failure(ErrorCode::WrongContext);
    }

    if (impl.shutDown) {
        return Result<Out>::Failure(ErrorCode::ShuttingDown);
    }

    return Result<Out>::Success(impl.PumpOnce(budget));
}

Result<void> AssetLoader::WaitForCompletions() noexcept {
    if (impl_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!impl_->OnOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (impl_->inCallback) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    impl_->WaitCompletions();
    return Result<void>::Success();
}

Result<AssetLoadStatus> AssetLoader::WaitFor(AssetLoadHandle handle, const char* site) noexcept {
    using Out = AssetLoadStatus;
    if (impl_ == nullptr) {
        return Result<Out>::Failure(ErrorCode::InvalidArgument);
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<Out>::Failure(ErrorCode::WrongThread);
    }

    if (impl.inCallback) {
        return Result<Out>::Failure(ErrorCode::WrongContext);
    }

    bool framing = false;
    if (config_.inFrame != nullptr) {
        LoaderCallbackScope scope(impl.inCallback);
        framing = config_.inFrame(config_.inFrameContext);
    }

    if (framing) {
        return Result<Out>::Failure(ErrorCode::WrongContext);
    }

    if (!impl.ValidHandle(handle)) {
        return Result<Out>::Failure(ErrorCode::StaleHandle);
    }

    if (impl.shutDown) {
        return Result<Out>::Failure(ErrorCode::ShuttingDown);
    }

    const uint64 start = impl.Now();
    while (impl.requests[handle.index].state == AssetLoadState::Pending) {
        (void)impl.PumpOnce(AssetLoaderPumpBudget{});
        if (impl.requests[handle.index].state != AssetLoadState::Pending) {
            break;
        }

        impl.WaitCompletions();
    }

    const uint64 elapsed = impl.Now() - start;
    AssetLoaderSyncWait wait;
    wait.site = site;
    wait.asset = impl.EntryAsset(impl.requests[handle.index].entry);
    wait.nanoseconds = elapsed;
    impl.recentWaits[impl.syncWaits % AssetLoaderSnapshot::RecentSyncWaitCapacity] = wait;
    ++impl.syncWaits;
    impl.syncWaitNanoseconds += elapsed;
    if (config_.observer != nullptr) {
        LoaderCallbackScope scope(impl.inCallback);
        config_.observer->OnSyncWait(wait);
    }

    return Status(handle);
}

Result<void> AssetLoader::Describe(AssetLoaderSnapshot& out) const noexcept {
    if (impl_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    out.requests.Clear();
    out.units.Clear();
    out.packages.Clear();
    const uint64 now = impl.Now();
    for (usize index = 0; index < impl.requests.Size(); ++index) {
        const LoaderRequest& request = impl.requests[index];
        if (!request.used) {
            continue;
        }

        AssetLoaderRequestInfo info;
        info.handle = AssetLoadHandle{static_cast<uint32>(index), request.generation};
        info.asset = impl.EntryAsset(request.entry);
        info.priority = request.priority;
        info.state = request.state;
        info.error = request.error;
        info.diagnostic = request.diagnostic;
        info.unit = request.unit;
        if (auto pushed = out.requests.TryPushBack(info); !pushed) {
            return pushed;
        }
    }

    for (usize index = 0; index < impl.units.Size(); ++index) {
        const LoaderUnit& unit = impl.units[index];
        if (!unit.used) {
            continue;
        }

        AssetLoaderUnitInfo info;
        info.unit = static_cast<uint32>(index);
        info.asset = impl.EntryAsset(unit.members[0]);
        info.phase = unit.phase;
        info.priority = unit.priority;
        info.effectivePriority = impl.Effective(unit);
        info.packages = static_cast<uint32>(unit.members.Size());
        info.preparedPackages = unit.preparedPackages;
        info.interest = unit.interest;
        for (const LoaderPackageJob* job : unit.jobs) {
            info.tempBytes = LoaderSaturatingAdd(info.tempBytes, job->tempBytes);
        }

        if (unit.phase == AssetLoaderUnitPhase::Queued) {
            info.waiting = AssetLoaderWait::Admission;
            for (uint32 other : unit.dependencies) {
                if (impl.units[other].phase == AssetLoaderUnitPhase::Queued) {
                    info.waiting = AssetLoaderWait::DependencyAdmission;
                    info.waitingOn = impl.EntryAsset(impl.units[other].members[0]);
                    break;
                }
            }
        } else if (unit.phase == AssetLoaderUnitPhase::Ready) {
            info.waiting = AssetLoaderWait::Pump;
        } else if (unit.phase == AssetLoaderUnitPhase::PostLoading) {
            info.waiting = AssetLoaderWait::PostLoad;
            if (!unit.dependencies.IsEmpty()) {
                info.waiting = AssetLoaderWait::PostLoadDependency;
                info.waitingOn = impl.EntryAsset(impl.units[unit.dependencies[0]].members[0]);
            }
        } else if (unit.phase == AssetLoaderUnitPhase::Failed) {
            info.failedAsset = impl.EntryAsset(unit.failedEntry);
            info.failError = unit.failError;
            info.failDiagnostic = unit.failDiagnostic;
        } else if (unit.phase == AssetLoaderUnitPhase::Loading) {
            for (const LoaderPackageJob* job : unit.jobs) {
                if (job->phase == AssetLoaderPackagePhase::Reading) {
                    info.waiting = AssetLoaderWait::Io;
                    break;
                }

                if (job->phase == AssetLoaderPackagePhase::Preparing) {
                    info.waiting = AssetLoaderWait::Worker;
                }
            }

            // IO·worker에 없는 Loading 단위는 모든 패키지가 Prepared이고 공개 안 된 의존이 있다
            // (아니면 Ready다 — 그래서 아래 탐색은 목록 안에서 멈춘다). 의존 목록에는 공개된 채
            // PostLoad 중인 단위도 있으므로 공개 안 된 것을 고른다.
            if (info.waiting == AssetLoaderWait::None) {
                info.waiting = AssetLoaderWait::Dependency;
                usize at = 0;
                while (impl.units[unit.dependencies[at]].phase ==
                       AssetLoaderUnitPhase::PostLoading) {
                    ++at;
                }

                info.waitingOn = impl.EntryAsset(impl.units[unit.dependencies[at]].members[0]);
            }
        }

        if (auto pushed = out.units.TryPushBack(info); !pushed) {
            return pushed;
        }

        for (const LoaderPackageJob* job : unit.jobs) {
            AssetLoaderPackageInfo package;
            package.asset = impl.EntryAsset(job->entry);
            package.unit = static_cast<uint32>(index);
            package.phase = job->phase;
            package.tempBytes = job->tempBytes;
            for (uint32 phase = 0; phase < AssetLoaderPackagePhaseCount; ++phase) {
                package.phaseNanoseconds[phase] = job->phaseNanoseconds[phase];
            }

            package.phaseNanoseconds[static_cast<uint32>(job->phase)] += now - job->phaseStart;
            if (auto pushed = out.packages.TryPushBack(package); !pushed) {
                return pushed;
            }
        }
    }

    out.loadedPackages = static_cast<uint32>(impl.loadedRoots.Size() - impl.freeLoadedSlots.Size());
    out.packagesInFlight = impl.packagesInFlight;
    out.tempBytesInFlight = impl.tempBytesInFlight;
    out.syncWaits = impl.syncWaits;
    out.syncWaitNanoseconds = impl.syncWaitNanoseconds;
    // 고리에서 오래된 것부터 옮긴다.
    constexpr uint64 Capacity = AssetLoaderSnapshot::RecentSyncWaitCapacity;
    const uint64 kept = impl.syncWaits < Capacity ? impl.syncWaits : Capacity;
    out.recentSyncWaitCount = static_cast<uint32>(kept);
    for (uint64 index = 0; index < kept; ++index) {
        out.recentSyncWaits[index] = impl.recentWaits[(impl.syncWaits - kept + index) % Capacity];
    }
    return Result<void>::Success();
}

Result<ObjectHandle> AssetLoader::FindPackage(AssetId asset) noexcept {
    if (impl_ == nullptr) {
        return Result<ObjectHandle>::Failure(ErrorCode::NotFound);
    }

    if (!impl_->OnOwnerThread()) {
        return Result<ObjectHandle>::Failure(ErrorCode::WrongThread);
    }

    const CookedManifestEntry* found = config_.manifest->Find(asset);
    if (found == nullptr) {
        return Result<ObjectHandle>::Failure(ErrorCode::NotFound);
    }

    const uint32 loaded = impl_->entryLoaded[impl_->EntryIndex(found)];
    if (loaded == LoaderNoIndex) {
        return Result<ObjectHandle>::Failure(ErrorCode::NotFound);
    }

    return Result<ObjectHandle>::Success(impl_->loadedHandles[loaded]);
}

Result<void> AssetLoader::TakePublished(Array<AssetLoaderPublished>& out) noexcept {
    if (impl_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (impl.inCallback) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    if (auto appended = out.TryAppend(impl.published.View()); !appended) {
        return appended;
    }

    impl.published.Clear();
    return Result<void>::Success();
}

Result<uint32> AssetLoader::PublishedHolds(AssetId asset) const noexcept {
    if (impl_ == nullptr) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    if (!impl_->OnOwnerThread()) {
        return Result<uint32>::Failure(ErrorCode::WrongThread);
    }

    const CookedManifestEntry* found = config_.manifest->Find(asset);
    if (found == nullptr) {
        return Result<uint32>::Failure(ErrorCode::NotFound);
    }

    return Result<uint32>::Success(impl_->publishedHolds[impl_->EntryIndex(found)]);
}

Result<void> AssetLoader::RetirePublished(Span<const AssetId> assets) noexcept {
    if (impl_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (impl.inCallback) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    if (impl.shutDown) {
        return Result<void>::Failure(ErrorCode::ShuttingDown);
    }

    // 모두 확인한 뒤에만 바꾼다: 매니페스트에 있고, PostLoad까지 마친 공개 패키지이고(단위 없음),
    // 공개 보유가 없고, 같은 id가 두 번 나오지 않는다. 바꾸기는 할당하지 않는다(칸을 퇴출로 표시).
    for (usize index = 0; index < assets.Size(); ++index) {
        const CookedManifestEntry* found = config_.manifest->Find(assets[index]);
        if (found == nullptr) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        const uint32 entry = impl.EntryIndex(found);
        if (impl.entryLoaded[entry] == LoaderNoIndex || impl.entryUnit[entry] != LoaderNoIndex ||
            impl.publishedHolds[entry] != 0) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        for (usize earlier = 0; earlier < index; ++earlier) {
            if (assets[earlier] == assets[index]) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    for (AssetId asset : assets) {
        const uint32 entry = impl.EntryIndex(config_.manifest->Find(asset));
        impl.slotStates[impl.entryLoaded[entry]] = static_cast<uint8>(LoaderSlot::Retired);
        impl.entryLoaded[entry] = LoaderNoIndex;
    }

    return Result<void>::Success();
}

Result<void> AssetLoader::ReleaseRetired(Span<const uint32> slots) noexcept {
    if (impl_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (impl.inCallback) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    for (usize index = 0; index < slots.Size(); ++index) {
        const uint32 slot = slots[index];
        if (slot >= impl.slotStates.Size() ||
            impl.slotStates[slot] != static_cast<uint8>(LoaderSlot::Retired)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        for (usize earlier = 0; earlier < index; ++earlier) {
            if (slots[earlier] == slot) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    if (auto reserved = impl.freeLoadedSlots.TryReserve(impl.freeLoadedSlots.Size() + slots.Size());
        !reserved) {
        return reserved;
    }

    for (uint32 slot : slots) {
        impl.loadedRoots[slot].Reset();
        impl.loadedHandles[slot] = ObjectHandle{};
        impl.slotStates[slot] = static_cast<uint8>(LoaderSlot::Free);
        (void)impl.freeLoadedSlots.TryPushBack(slot);
    }

    return Result<void>::Success();
}

Result<void> AssetLoader::Shutdown() noexcept {
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

    impl.shutDown = true;
    impl.ShutdownAll();
    return Result<void>::Success();
}

} // namespace rocks
