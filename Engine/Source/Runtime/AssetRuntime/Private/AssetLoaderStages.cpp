#include "AssetLoaderInternal.h"

// 로더 정리(실패·버림)·허가·완료 처리다(11.6절 6·7·9·3).
namespace rocks {

void AssetLoader::Impl::QueueRetire(uint32 unit, LoaderRetire kind) noexcept {
    // 이미 정리 중인(Failed·Abandoned) 단위는 어느 목록에도 남지 않아 여기 오지 않는다. 공개된
    // 단위(PostLoading)는 관심이 0이어도 PostLoad를 마친다(버리지 않는다).
    LoaderUnit& target = units[unit];
    if (target.retire != LoaderRetire::None || target.phase == AssetLoaderUnitPhase::PostLoading) {
        return;
    }

    target.retire = kind;
    target.nextRetire = LoaderNoIndex;
    if (retireTail == LoaderNoIndex) {
        retireHead = unit;
    } else {
        units[retireTail].nextRetire = unit;
    }

    retireTail = unit;
}

void AssetLoader::Impl::DropInterest(uint32 unit) noexcept {
    // 관심 수는 요청·기대는 단위마다 정확히 하나씩 걸고 놓는다.
    LoaderUnit& target = units[unit];
    --target.interest;
    if (target.interest == 0) {
        QueueRetire(unit, LoaderRetire::Abandon);
    }
}

void AssetLoader::Impl::FailUnit(uint32 unit, uint32 failedEntry, ErrorCode error,
                                 const AssetDiagnostic& diagnostic) noexcept {
    // 같은 정리 묶음에서 먼저 실패(또는 버림)로 들어간 단위는 그 원인을 지킨다.
    LoaderUnit& target = units[unit];
    if (target.retire != LoaderRetire::None) {
        return;
    }

    target.failedEntry = failedEntry;
    target.failError = error;
    target.failDiagnostic = diagnostic;
    QueueRetire(unit, LoaderRetire::Fail);
}

void AssetLoader::Impl::DestroyJob(LoaderPackageJob* job) noexcept {
    --packagesInFlight;
    tempBytesInFlight -= job->accountedBytes;
    Allocator& owner = allocator;
    job->~LoaderPackageJob();
    (void)owner.Deallocate(job);
}

void AssetLoader::Impl::RemoveFromLists(uint32 unit) noexcept {
    RemoveValue(admitCandidates, unit);
    RemoveValue(readyUnits, unit);
}

void AssetLoader::Impl::ProcessRetire() noexcept {
    while (retireHead != LoaderNoIndex) {
        const uint32 unit = retireHead;
        retireHead = units[unit].nextRetire;
        if (retireHead == LoaderNoIndex) {
            retireTail = LoaderNoIndex;
        }

        LoaderUnit& target = units[unit];
        const LoaderRetire kind = target.retire;
        target.retire = LoaderRetire::None;
        target.nextRetire = LoaderNoIndex;
        RemoveFromLists(unit);
        if (kind == LoaderRetire::Fail) {
            const AssetId failedAsset = EntryAsset(target.failedEntry);
            for (uint32 index : target.requests) {
                LoaderRequest& request = requests[index];
                if (request.entry == target.failedEntry) {
                    FinishRequest(index, AssetLoadState::Failed, target.failError,
                                  target.failDiagnostic, ObjectHandle{});
                    continue;
                }

                // 할당 실패는 내용 결함이 아니다: 기대는 요청도 OutOfMemory(진단 None — 11.6절
                // 6·13번). "의존 없음"으로 보고하지 않는다.
                if (target.failError == ErrorCode::OutOfMemory) {
                    FinishRequest(index, AssetLoadState::Failed, ErrorCode::OutOfMemory,
                                  AssetDiagnostic{}, ObjectHandle{});
                    continue;
                }

                AssetDiagnostic dependency;
                dependency.code = AssetDiagnosticCode::MissingDependency;
                dependency.error = ErrorCode::DependencyFailed;
                dependency.asset = EntryAsset(request.entry);
                dependency.related = failedAsset;
                FinishRequest(index, AssetLoadState::Failed, ErrorCode::DependencyFailed,
                              dependency, ObjectHandle{});
            }

            // 기대는 단위는 같은 원인으로 실패한다(요청은 DependencyFailed).
            for (uint32 other : target.dependents) {
                FailUnit(other, target.failedEntry, target.failError, target.failDiagnostic);
            }
        }

        target.requests.Clear();
        target.phase = kind == LoaderRetire::Fail ? AssetLoaderUnitPhase::Failed
                                                  : AssetLoaderUnitPhase::Abandoned;
        for (uint32 other : target.dependencies) {
            RemoveValue(units[other].dependents, unit);
            DropInterest(other);
        }

        for (uint32 other : target.dependents) {
            RemoveValue(units[other].dependencies, unit);
        }

        target.dependencies.Clear();
        target.dependents.Clear();
        for (uint32 member : target.members) {
            entryUnit[member] = LoaderNoIndex;
        }

        usize kept = 0;
        for (LoaderPackageJob* job : target.jobs) {
            if (job->asyncPending) {
                platform::AtomicStore<platform::MemoryOrder::Release>(&job->abandoned, 1u);
                target.jobs[kept++] = job;
                continue;
            }

            DestroyJob(job);
        }

        (void)target.jobs.TryResize(kept);
        target.aliveJobs = static_cast<uint32>(kept);
        if (kept == 0) {
            FreeUnitSlot(unit);
        }
    }
}

uint64 AssetLoader::Impl::UnitBytes(const LoaderUnit& unit) const noexcept {
    uint64 bytes = 0;
    for (uint32 member : unit.members) {
        const CookedManifestEntry& entry = config.manifest->entries[member];
        bytes =
            LoaderSaturatingAdd(bytes, LoaderSaturatingAdd(entry.packageBytes, entry.bulkRawBytes));
    }

    return bytes;
}

void AssetLoader::Impl::SetPhase(LoaderPackageJob& job, AssetLoaderPackagePhase phase) noexcept {
    const uint64 now = Now();
    job.phaseNanoseconds[static_cast<uint32>(job.phase)] += now - job.phaseStart;
    job.phase = phase;
    job.phaseStart = now;
}

Result<void> AssetLoader::Impl::BeginPackage(uint32 unit, uint32 member,
                                             AssetDiagnostic& diagnostic) noexcept {
    LoaderUnit& target = units[unit];
    const CookedManifestEntry& entry = config.manifest->entries[member];
    diagnostic = AssetDiagnostic{};
    diagnostic.asset = entry.asset;
    auto memory = allocator.Allocate(sizeof(LoaderPackageJob), alignof(LoaderPackageJob));
    if (!memory) {
        return Result<void>::Failure(memory.Error());
    }

    auto* job = InitializeAt<LoaderPackageJob>(memory.Value(), allocator);
    // jobs 용량은 허가 전에 구성 수만큼 잡았다.
    (void)target.jobs.TryPushBack(job);
    ++target.aliveJobs;
    ++packagesInFlight;
    job->tempBytes = LoaderSaturatingAdd(entry.packageBytes, entry.bulkRawBytes);
    job->accountedBytes =
        LoaderSaturatingAdd(tempBytesInFlight, job->tempBytes) - tempBytesInFlight;
    tempBytesInFlight += job->accountedBytes;
    job->queue = &queue;
    job->manifest = config.manifest;
    job->entry = member;
    job->unit = unit;
    job->phaseStart = Now();
    PackageLoadOptions options = config.packageOptions;
    options.read.expectedKind = PackageKind::Cooked;
    options.maxBulkRawBytes = entry.bulkRawBytes;
    auto state = UniquePtr<PackageLoadState>::TryMake(allocator, allocator, *config.registry,
                                                      *config.types, access, options);
    if (!state) {
        return Result<void>::Failure(state.Error());
    }

    job->state = Move(state.Value());
    if (auto sized = job->file.TryResize(static_cast<usize>(entry.packageBytes)); !sized) {
        return sized;
    }

    ErrorCode built = ErrorCode::None;
    LoaderKeepFirst(built, job->path.TryAppend(root.View()));
    LoaderKeepFirst(built, job->path.TryAppendAscii('/'));
    LoaderKeepFirst(built, AppendCookedPackagePath(job->path, entry.asset, entry.packageSha256));
    if (built != ErrorCode::None) {
        return Result<void>::Failure(built);
    }

    CookedPackageRead& read = job->read;
    read.path = job->path.CString();
    read.pathBytes = job->path.View().ByteCount();
    read.expectedBytes = entry.packageBytes;
    read.destination = job->file.Data();
    read.complete = OnLoaderReadComplete;
    read.owner = job;
    read.backendState = nullptr;
    SetPhase(*job, AssetLoaderPackagePhase::Reading);
    // complete가 Begin 안에서 불릴 수 있으므로 진행 표시를 먼저 한다.
    job->asyncPending = true;
    job->readBegun = true;
    ++asyncInFlight;
    Result<void> begun = Result<void>::Success();
    {
        LoaderCallbackScope scope(inCallback);
        begun = config.io->Begin(read);
    }

    if (!begun) {
        job->asyncPending = false;
        job->readBegun = false;
        --asyncInFlight;
        return begun;
    }

    return Result<void>::Success();
}

void AssetLoader::Impl::AdmitUnit(uint32 unit) noexcept {
    LoaderUnit& target = units[unit];
    target.phase = AssetLoaderUnitPhase::Loading;
    RemoveValue(admitCandidates, unit);
    ++admissions;
    // 기대는 단위는 모두 이 단위가 Queued일 때 생겼다(허가 전 단위만 미허가 의존으로 센다) —
    // 그래서
    // 이 단위를 세고 있고, 의존보다 먼저 허가될 수 없어 아직 Queued다.
    for (uint32 other : target.dependents) {
        if (--units[other].unadmittedDependencies == 0) {
            (void)admitCandidates.TryPushBack(other);
        }
    }

    AssetDiagnostic diagnostic;
    if (auto reserved = target.jobs.TryReserve(target.members.Size()); !reserved) {
        diagnostic.asset = EntryAsset(target.members[0]);
        diagnostic.error = reserved.Error();
        FailUnit(unit, target.members[0], reserved.Error(), diagnostic);
        return;
    }

    for (uint32 member : target.members) {
        if (auto begun = BeginPackage(unit, member, diagnostic); !begun) {
            diagnostic.error = begun.Error();
            FailUnit(unit, member, begun.Error(), diagnostic);
            return;
        }
    }
}

uint32 AssetLoader::Impl::Admit() noexcept {
    uint32 admitted = 0;
    while (!admitCandidates.IsEmpty()) {
        uint32 best = admitCandidates[0];
        for (uint32 candidate : admitCandidates) {
            if (Before(candidate, best)) {
                best = candidate;
            }
        }

        const LoaderUnit& unit = units[best];
        const auto count = static_cast<uint32>(unit.members.Size());
        const uint64 bytes = UnitBytes(unit);
        const bool overCount = packagesInFlight + count > config.limits.maxPackagesInFlight;
        const bool overBytes =
            LoaderSaturatingAdd(tempBytesInFlight, bytes) > config.limits.maxTempBytes;
        if (packagesInFlight > 0 && (overCount || overBytes)) {
            break;
        }

        AdmitUnit(best);
        ProcessRetire();
        ++admitted;
    }

    return admitted;
}

void AssetLoader::Impl::MarkReadyIfDone(uint32 unit) noexcept {
    LoaderUnit& target = units[unit];
    if (target.phase == AssetLoaderUnitPhase::Loading && target.waitingDependencies == 0 &&
        target.preparedPackages == target.members.Size()) {
        target.phase = AssetLoaderUnitPhase::Ready;
        (void)readyUnits.TryPushBack(unit);
    }
}

void AssetLoader::Impl::DrainRetiredJob(LoaderPackageJob* job) noexcept {
    const uint32 unit = job->unit;
    LoaderUnit& target = units[unit];
    // 버린 단위의 jobs에는 진행 중이던 작업만 남아 있다.
    (void)target.jobs.TryRemoveAtSwap(target.jobs.Find(job));
    DestroyJob(job);
    if (--target.aliveJobs == 0) {
        FreeUnitSlot(unit);
    }
}

void AssetLoader::Impl::HandleCompletion(LoaderPackageJob* job) noexcept {
    job->asyncPending = false;
    --asyncInFlight;
    const LoaderCompletion completion = job->completion;
    // IO 완료만 readBegun이 남아 있다(worker 완료 때는 이미 놓았다).
    if (job->readBegun) {
        {
            LoaderCallbackScope scope(inCallback);
            config.io->Release(job->read);
        }

        job->readBegun = false;
    }

    const uint32 unit = job->unit;
    const AssetLoaderUnitPhase phase = units[unit].phase;
    if (phase == AssetLoaderUnitPhase::Failed || phase == AssetLoaderUnitPhase::Abandoned) {
        DrainRetiredJob(job);
        return;
    }

    const CookedManifestEntry& entry = config.manifest->entries[job->entry];
    AssetDiagnostic diagnostic;
    diagnostic.asset = entry.asset;
    if (completion == LoaderCompletion::Io) {
        const CookedPackageReadResult result = job->ioResult;
        if (result.error != ErrorCode::None) {
            diagnostic.error = result.error;
            diagnostic.code = result.error == ErrorCode::NotFound ? AssetDiagnosticCode::MissingFile
                                                                  : AssetDiagnosticCode::None;
            FailUnit(unit, job->entry, result.error, diagnostic);
            return;
        }

        if (result.fileBytes != entry.packageBytes) {
            diagnostic.code = AssetDiagnosticCode::CorruptPackage;
            diagnostic.error = ErrorCode::CorruptData;
            diagnostic.section = static_cast<uint32>(CookedManifestField::PackageBytes);
            diagnostic.detail = CookedManifestNoEntry;
            FailUnit(unit, job->entry, ErrorCode::CorruptData, diagnostic);
            return;
        }

        SetPhase(*job, AssetLoaderPackagePhase::Preparing);
        job->asyncPending = true;
        ++asyncInFlight;
        Result<void> run = Result<void>::Success();
        {
            LoaderCallbackScope scope(inCallback);
            run = config.worker->Run(RunLoaderPrepare, job);
        }

        if (!run) {
            job->asyncPending = false;
            --asyncInFlight;
            diagnostic.error = run.Error();
            FailUnit(unit, job->entry, run.Error(), diagnostic);
        }

        return;
    }

    if (job->workerError != ErrorCode::None) {
        FailUnit(unit, job->entry, job->workerError, job->workerDiagnostic);
        return;
    }

    SetPhase(*job, AssetLoaderPackagePhase::Prepared);
    ++units[unit].preparedPackages;
    MarkReadyIfDone(unit);
}

uint32 AssetLoader::Impl::DrainCompletions() noexcept {
    queue.mutex.Lock();
    LoaderPackageJob* head = queue.head;
    queue.head = nullptr;
    queue.tail = nullptr;
    queue.mutex.Unlock();
    uint32 count = 0;
    while (head != nullptr) {
        LoaderPackageJob* job = head;
        head = head->nextCompletion;
        HandleCompletion(job);
        ProcessRetire();
        ++count;
    }

    return count;
}

void AssetLoader::Impl::CheckCancellations() noexcept {
    for (usize index = 0; index < requests.Size(); ++index) {
        LoaderRequest& request = requests[index];
        if (!request.used || request.state != AssetLoadState::Pending ||
            !request.cancel.IsCancellationRequested()) {
            continue;
        }

        CancelRequest(static_cast<uint32>(index), ErrorCode::Cancelled);
    }

    ProcessRetire();
}

void AssetLoader::Impl::CancelRequest(uint32 index, ErrorCode error) noexcept {
    const uint32 unit = requests[index].unit;
    AssetDiagnostic diagnostic;
    diagnostic.error = error;
    diagnostic.asset = EntryAsset(requests[index].entry);
    FinishRequest(
        index, error == ErrorCode::Cancelled ? AssetLoadState::Cancelled : AssetLoadState::Failed,
        error, diagnostic, ObjectHandle{});
    // Pending 요청은 언제나 단위에 붙어 있다.
    RemoveValue(units[unit].requests, index);
    DropInterest(unit);
}

} // namespace rocks
