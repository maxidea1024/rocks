#include "AssetLoaderInternal.h"

// 로더 공개(트랜잭션)·PostLoad·Pump·종료다(11.6절 10·16·14).
namespace rocks {

uint32 AssetLoader::Impl::PickReady() const noexcept {
    uint32 best = readyUnits[0];
    for (uint32 candidate : readyUnits) {
        if (Before(candidate, best)) {
            best = candidate;
        }
    }

    return best;
}

uint32 AssetLoader::Impl::FailedMember(const LoaderUnit& unit, AssetId asset) const noexcept {
    uint32 failed = unit.members[0];
    for (uint32 member : unit.members) {
        failed = EntryAsset(member) == asset ? member : failed;
    }

    return failed;
}

void AssetLoader::Impl::InstantiateUnit(uint32 unit) noexcept {
    RemoveValue(readyUnits, unit);
    const usize count = units[unit].members.Size();
    Array<PackageLoadState*> states(allocator);
    Array<ObjectHandle> handles(allocator);
    Array<RootRef<Object>> roots(allocator);
    AssetDiagnostic diagnostic;
    diagnostic.asset = EntryAsset(units[unit].members[0]);
    ErrorCode step = ErrorCode::None;
    LoaderKeepFirst(step, states.TryReserve(count));
    LoaderKeepFirst(step, handles.TryResize(count));
    LoaderKeepFirst(step, roots.TryResize(count));
    LoaderKeepFirst(step, loadedRoots.TryReserve(loadedRoots.Size() + count));
    LoaderKeepFirst(step, loadedHandles.TryReserve(loadedHandles.Size() + count));
    if (step != ErrorCode::None) {
        diagnostic.error = step;
        FailUnit(unit, units[unit].members[0], step, diagnostic);
        return;
    }

    for (LoaderPackageJob* job : units[unit].jobs) {
        (void)states.TryPushBack(job->state.Get());
    }

    // 트랜잭션은 생성부터 소멸(실패면 롤백)까지 사용자 코드(클래스 생성자·소멸자, 필드 codec·
    // Validate hook·Required 검사, Registry allocator)를 부르므로 그 전체를 콜백 중 표시 안에
    // 둔다(11.6절 18). 실패 처리는 표시를 푼 뒤에 한다.
    AssetDiagnostic made;
    ErrorCode instantiateError = ErrorCode::None;
    ErrorCode commitError = ErrorCode::None;
    {
        LoaderCallbackScope scope(inCallback);
        ConstructionContext transaction(*config.registry, access);
        if (auto instantiated = InstantiatePackages(
                transaction, Span<PackageLoadState* const>(states.Data(), states.Size()), &owner,
                handles.View(), &made);
            !instantiated) {
            instantiateError = instantiated.Error();
        } else if (auto committed = transaction.Commit(
                       Span<const ObjectHandle>(handles.Data(), handles.Size()), roots.View());
                   !committed) {
            commitError = committed.Error();
        }
    }

    if (instantiateError != ErrorCode::None) {
        FailUnit(unit, FailedMember(units[unit], made.asset), instantiateError, made);
        return;
    }

    if (commitError != ErrorCode::None) {
        diagnostic.error = commitError;
        FailUnit(unit, units[unit].members[0], commitError, diagnostic);
        return;
    }

    LoaderUnit& target = units[unit];
    // jobs와 members는 같은 순서다(허가 때 members 순서로 만들었다).
    for (usize index = 0; index < count; ++index) {
        const uint32 member = target.jobs[index]->entry;
        entryLoaded[member] = static_cast<uint32>(loadedRoots.Size());
        (void)loadedRoots.TryPushBack(Move(roots[index]));
        (void)loadedHandles.TryPushBack(handles[index]);
    }

    // 기대는 단위는 공개를 기다리지 않게 되지만(공개 대기 수), PostLoad는 이 단위가 끝날
    // 때까지 기다린다(의존 목록에 남긴다).
    for (uint32 other : target.dependents) {
        --units[other].waitingDependencies;
        MarkReadyIfDone(other);
    }

    for (LoaderPackageJob* job : target.jobs) {
        DestroyJob(job);
    }

    target.jobs.Clear();
    target.aliveJobs = 0;
    target.phase = AssetLoaderUnitPhase::PostLoading;
    target.postLoadNext = 0;
    if (config.postLoad == nullptr) {
        FinishUnit(unit);
        return;
    }

    (void)postLoadUnits.TryPushBack(unit);
}

void AssetLoader::Impl::FinishUnit(uint32 unit) noexcept {
    RemoveValue(postLoadUnits, unit);
    LoaderUnit& target = units[unit];
    for (uint32 index : target.requests) {
        FinishRequest(index, AssetLoadState::Succeeded, ErrorCode::None, AssetDiagnostic{},
                      loadedHandles[entryLoaded[requests[index].entry]]);
    }

    target.requests.Clear();
    for (uint32 other : target.dependents) {
        RemoveValue(units[other].dependencies, unit);
    }

    for (uint32 member : target.members) {
        entryUnit[member] = LoaderNoIndex;
    }

    FreeUnitSlot(unit);
}

uint32 AssetLoader::Impl::PickPostLoad() const noexcept {
    uint32 best = LoaderNoIndex;
    for (uint32 candidate : postLoadUnits) {
        if (units[candidate].dependencies.IsEmpty() &&
            (best == LoaderNoIndex || Before(candidate, best))) {
            best = candidate;
        }
    }

    return best;
}

void AssetLoader::Impl::PostLoadNext(uint32 unit) noexcept {
    const uint32 member = units[unit].members[units[unit].postLoadNext++];
    {
        LoaderCallbackScope scope(inCallback);
        config.postLoad->PostLoad(loadedHandles[entryLoaded[member]], EntryAsset(member));
    }

    if (units[unit].postLoadNext == units[unit].members.Size()) {
        FinishUnit(unit);
    }
}

AssetLoaderPumpResult AssetLoader::Impl::PumpOnce(const AssetLoaderPumpBudget& budget) noexcept {
    AssetLoaderPumpResult result;
    result.completions = DrainCompletions();
    CheckCancellations();
    result.admittedUnits = Admit();
    // 시계는 공개·PostLoad 구간에서만 읽는다(단위·호출 사이 검사 — 11.6절 10·16).
    if (readyUnits.IsEmpty() && PickPostLoad() == LoaderNoIndex) {
        return result;
    }

    const uint64 start = Now();
    while (!readyUnits.IsEmpty()) {
        InstantiateUnit(PickReady());
        ProcessRetire();
        ++result.instantiatedUnits;
        if (readyUnits.IsEmpty()) {
            break;
        }

        if (result.instantiatedUnits >= budget.maxUnits || Now() - start >= budget.maxNanoseconds) {
            result.budgetExhausted = true;
            break;
        }
    }

    // 공개가 예산을 다 써도 준비된 PostLoad 하나는 부른다(굶주림 방지 — 11.6절 16).
    if (result.budgetExhausted) {
        if (const uint32 unit = PickPostLoad(); unit != LoaderNoIndex) {
            PostLoadNext(unit);
            ++result.postLoads;
        }

        return result;
    }

    // PostLoad는 공개를 다 한 뒤에 한다(공개가 예산을 다 쓰지 않았으면). 준비된 것이 있으면
    // 적어도 하나를 부른다.
    for (uint32 unit = PickPostLoad(); unit != LoaderNoIndex; unit = PickPostLoad()) {
        const bool first = result.postLoads == 0;
        if (!first &&
            (result.postLoads >= budget.maxPostLoads || Now() - start >= budget.maxNanoseconds)) {
            result.budgetExhausted = true;
            break;
        }

        PostLoadNext(unit);
        ++result.postLoads;
    }

    return result;
}

void AssetLoader::Impl::ShutdownAll() noexcept {
    for (usize index = 0; index < requests.Size(); ++index) {
        if (requests[index].used && requests[index].state == AssetLoadState::Pending) {
            CancelRequest(static_cast<uint32>(index), ErrorCode::ShuttingDown);
        }
    }

    for (usize index = 0; index < units.Size(); ++index) {
        if (units[index].used && units[index].phase != AssetLoaderUnitPhase::Failed &&
            units[index].phase != AssetLoaderUnitPhase::Abandoned) {
            QueueRetire(static_cast<uint32>(index), LoaderRetire::Abandon);
        }
    }

    ProcessRetire();
    while (asyncInFlight > 0) {
        WaitCompletions();
        (void)DrainCompletions();
    }

    // 공개된 채 PostLoad를 기다리던 단위는 남은 PostLoad를 부르지 않고 놓는다(11.6절 16).
    for (usize index = 0; index < units.Size(); ++index) {
        if (units[index].used) {
            RemoveValue(postLoadUnits, static_cast<uint32>(index));
            FreeUnitSlot(static_cast<uint32>(index));
        }
    }

    loadedRoots.Clear();
    loadedHandles.Clear();
    for (usize index = 0; index < entryLoaded.Size(); ++index) {
        entryLoaded[index] = LoaderNoIndex;
    }
}

void AssetLoader::Impl::WaitCompletions() noexcept {
    queue.mutex.Lock();
    while (queue.head == nullptr && asyncInFlight > 0) {
        if (config.waitEntered != nullptr) {
            LoaderCallbackScope scope(inCallback);
            config.waitEntered(config.waitEnteredContext);
        }

        (void)queue.changed.Wait(queue.mutex);
    }

    queue.mutex.Unlock();
}

} // namespace rocks
