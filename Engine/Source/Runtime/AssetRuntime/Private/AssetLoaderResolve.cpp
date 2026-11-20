#include "AssetLoaderInternal.h"

// 로더 초기화·단위 표·ResolveManifest(Hard 폐포·Tarjan SCC)·요청이다(11.6절 1·2).
namespace rocks {

uint64 AssetLoader::Impl::Now() const noexcept {
    if (config.clock == nullptr) {
        return LoaderDefaultClock(nullptr);
    }

    LoaderCallbackScope scope(inCallback);
    return config.clock(config.clockContext);
}

bool AssetLoader::Impl::OnOwnerThread() const noexcept {
    return platform::CurrentThreadId() == ownerThread;
}

uint32 AssetLoader::Impl::EntryIndex(const CookedManifestEntry* entry) const noexcept {
    return static_cast<uint32>(entry - config.manifest->entries.Data());
}

AssetId AssetLoader::Impl::EntryAsset(uint32 entry) const noexcept {
    return config.manifest->entries[entry].asset;
}

LoadPriority AssetLoader::Impl::Effective(const LoaderUnit& unit) const noexcept {
    const uint64 waited = admissions - unit.queuedAt;
    const uint64 bonus = waited / config.limits.agingAdmissions;
    const uint64 raised = static_cast<uint64>(unit.priority) + bonus;
    const uint64 top = static_cast<uint64>(LoadPriority::Critical);
    return static_cast<LoadPriority>(raised < top ? raised : top);
}

bool AssetLoader::Impl::Before(uint32 a, uint32 b) const noexcept {
    const auto pa = static_cast<uint32>(Effective(units[a]));
    const auto pb = static_cast<uint32>(Effective(units[b]));
    if (pa != pb) {
        return pa > pb;
    }

    return units[a].sequence < units[b].sequence;
}

Result<void> AssetLoader::Impl::BuildPathIndex() noexcept {
    const CookedManifest& manifest = *config.manifest;
    const usize count = manifest.entries.Size();
    if (auto reserved = keyOffsets.TryReserve(count + 1); !reserved) {
        return reserved;
    }

    if (auto reserved = keyOrder.TryReserve(count); !reserved) {
        return reserved;
    }

    (void)keyOffsets.TryPushBack(0);
    for (usize index = 0; index < count; ++index) {
        StringBuilder<LoaderKeyInlineBytes> key(allocator);
        if (auto made = package_detail::CheckCookedPathHint(manifest.entries[index].pathHint.View(),
                                                            allocator, key);
            !made) {
            // 매니페스트는 검증을 마쳤다 — 남는 실패는 할당뿐이다.
            return made;
        }

        const StringView view = key.View();
        if (auto appended = keyBytes.TryAppend(Span<const char>(view.Data(), view.ByteCount()));
            !appended) {
            return appended;
        }

        (void)keyOffsets.TryPushBack(static_cast<uint64>(keyBytes.Size()));
        (void)keyOrder.TryPushBack(static_cast<uint32>(index));
    }

    keyOrder.Sort([this](uint32 left, uint32 right) noexcept {
        return package_detail::Utf8ByteLess(KeyOf(left), KeyOf(right));
    });
    return Result<void>::Success();
}

StringView AssetLoader::Impl::KeyOf(uint32 entry) const noexcept {
    const uint64 begin = keyOffsets[entry];
    const uint64 end = keyOffsets[entry + 1];
    return StringView(keyBytes.Data() + begin, static_cast<usize>(end - begin));
}

Result<void> AssetLoader::Impl::Initialize(AssetDiagnostic* failure) noexcept {
    const usize count = config.manifest->entries.Size();
    // 검증하지 않은 모델의 빠진 Hard 대상은 해석에서 범위 밖 색인이 된다(11.6절 1).
    if (auto valid = ValidateCookedManifest(*config.manifest, allocator, failure); !valid) {
        return valid;
    }

    if (auto checked = CheckCookedManifestProfile(*config.manifest, config.runtimeProfile, failure);
        !checked) {
        return checked;
    }

    auto resource = [failure](Result<void> result) noexcept {
        if (!result && failure != nullptr) {
            *failure = AssetDiagnostic{};
            failure->error = result.Error();
        }

        return result;
    };
    if (auto copied = root.TryAppend(config.cookedRoot); !copied) {
        return resource(Move(copied));
    }

    ErrorCode sized = ErrorCode::None;
    LoaderKeepFirst(sized, entryUnit.TryResize(count));
    LoaderKeepFirst(sized, entryLoaded.TryResize(count));
    LoaderKeepFirst(sized, publishedHolds.TryResize(count));
    LoaderKeepFirst(sized, tarjanIndex.TryResize(count));
    LoaderKeepFirst(sized, tarjanLow.TryResize(count));
    LoaderKeepFirst(sized, tarjanOnStack.TryResize(count));
    if (sized != ErrorCode::None) {
        return resource(Result<void>::Failure(sized));
    }

    for (usize index = 0; index < count; ++index) {
        entryUnit[index] = LoaderNoIndex;
        entryLoaded[index] = LoaderNoIndex;
        publishedHolds[index] = 0;
        tarjanIndex[index] = LoaderNoIndex;
    }

    return resource(BuildPathIndex());
}

uint32 AssetLoader::Impl::TakeUnitSlot() noexcept {
    uint32 index = freeUnit;
    if (index != LoaderNoIndex) {
        freeUnit = units[index].nextFree;
    } else {
        index = static_cast<uint32>(units.Size());
        (void)units.TryEmplaceBack(allocator);
    }

    LoaderUnit& unit = units[index];
    unit.used = true;
    unit.nextFree = LoaderNoIndex;
    ++usedUnits;
    return index;
}

bool AssetLoader::Impl::IsBeforePublish(AssetLoaderUnitPhase phase) noexcept {
    return phase == AssetLoaderUnitPhase::Queued || phase == AssetLoaderUnitPhase::Loading ||
           phase == AssetLoaderUnitPhase::Ready;
}

void AssetLoader::Impl::ReleasePublishedHolds(LoaderUnit& unit) noexcept {
    if (unit.publishedHeld) {
        for (uint32 entry : unit.publishedDeps) {
            --publishedHolds[entry];
        }
    }

    unit.publishedDeps.Clear();
    unit.publishedHeld = false;
}

void AssetLoader::Impl::FreeUnitSlot(uint32 index) noexcept {
    LoaderUnit& unit = units[index];
    ReleasePublishedHolds(unit);
    unit.used = false;
    unit.members.Clear();
    unit.jobs.Clear();
    unit.dependencies.Clear();
    unit.dependents.Clear();
    unit.requests.Clear();
    unit.unadmittedDependencies = 0;
    unit.waitingDependencies = 0;
    unit.interest = 0;
    unit.preparedPackages = 0;
    unit.aliveJobs = 0;
    unit.postLoadNext = 0;
    unit.tempBytes = 0;
    unit.retire = LoaderRetire::None;
    unit.nextRetire = LoaderNoIndex;
    unit.failedEntry = LoaderNoIndex;
    unit.failError = ErrorCode::None;
    unit.failDiagnostic = AssetDiagnostic{};
    unit.phase = AssetLoaderUnitPhase::Queued;
    unit.nextFree = freeUnit;
    freeUnit = index;
    --usedUnits;
}

void AssetLoader::Impl::RemoveValue(Array<uint32>& values, uint32 value) noexcept {
    const usize at = values.Find(value);
    if (at < values.Size()) {
        (void)values.TryRemoveAtSwap(at);
    }
}

void AssetLoader::Impl::ResetTarjan() noexcept {
    for (uint32 entry : tarjanVisited) {
        tarjanIndex[entry] = LoaderNoIndex;
        tarjanOnStack[entry] = 0;
    }

    tarjanVisited.Clear();
    tarjanFrames.Clear();
    tarjanStack.Clear();
}

uint32 AssetLoader::Impl::HardTarget(uint32 entry, uint32 position) const noexcept {
    const CookedManifestDependency& dependency =
        config.manifest->entries[entry].dependencies[position];
    if (dependency.kind != PackageDependencyKind::Hard) {
        return LoaderNoIndex;
    }

    // 매니페스트가 Hard 대상의 존재를 보장한다(AS 10.1절 6).
    return EntryIndex(config.manifest->Find(dependency.asset));
}

bool AssetLoader::Impl::IsNew(uint32 entry) const noexcept {
    return entryLoaded[entry] == LoaderNoIndex && entryUnit[entry] == LoaderNoIndex;
}

Result<void> AssetLoader::Impl::Visit(uint32 entry, uint32& counter) noexcept {
    if (auto pushed = tarjanVisited.TryPushBack(entry); !pushed) {
        return pushed;
    }

    if (auto pushed = tarjanFrames.TryPushBack(LoaderTarjanFrame{entry, 0}); !pushed) {
        (void)tarjanVisited.TryPopBack();
        return pushed;
    }

    if (auto pushed = tarjanStack.TryPushBack(entry); !pushed) {
        return pushed;
    }

    tarjanIndex[entry] = counter;
    tarjanLow[entry] = counter;
    tarjanOnStack[entry] = 1;
    ++counter;
    return Result<void>::Success();
}

Result<void> AssetLoader::Impl::FindComponents(uint32 rootEntry) noexcept {
    sccMembers.Clear();
    sccStarts.Clear();
    uint32 counter = 0;
    if (auto visited = Visit(rootEntry, counter); !visited) {
        return visited;
    }

    while (!tarjanFrames.IsEmpty()) {
        LoaderTarjanFrame& frame = tarjanFrames[tarjanFrames.Size() - 1];
        const uint32 entry = frame.entry;
        const auto count = static_cast<uint32>(config.manifest->entries[entry].dependencies.Size());
        bool descended = false;
        while (frame.position < count) {
            const uint32 target = HardTarget(entry, frame.position);
            ++frame.position;
            if (target == LoaderNoIndex || !IsNew(target)) {
                continue;
            }

            if (tarjanIndex[target] == LoaderNoIndex) {
                if (auto visited = Visit(target, counter); !visited) {
                    return visited;
                }

                descended = true;
                break;
            }

            if (tarjanOnStack[target] != 0 && tarjanIndex[target] < tarjanLow[entry]) {
                tarjanLow[entry] = tarjanIndex[target];
            }
        }

        if (descended) {
            continue;
        }

        (void)tarjanFrames.TryPopBack();
        if (!tarjanFrames.IsEmpty()) {
            const uint32 parent = tarjanFrames[tarjanFrames.Size() - 1].entry;
            if (tarjanLow[entry] < tarjanLow[parent]) {
                tarjanLow[parent] = tarjanLow[entry];
            }
        }

        if (tarjanLow[entry] != tarjanIndex[entry]) {
            continue;
        }

        if (auto pushed = sccStarts.TryPushBack(static_cast<uint32>(sccMembers.Size())); !pushed) {
            return pushed;
        }

        uint32 member = LoaderNoIndex;
        do {
            member = tarjanStack[tarjanStack.Size() - 1];
            (void)tarjanStack.TryPopBack();
            tarjanOnStack[member] = 0;
            if (auto pushed = sccMembers.TryPushBack(member); !pushed) {
                return pushed;
            }
        } while (member != entry);
    }

    return Result<void>::Success();
}

void AssetLoader::Impl::DiscardNewUnits() noexcept {
    for (uint32 unit : newUnits) {
        FreeUnitSlot(unit);
    }

    newUnits.Clear();
}

Result<uint32> AssetLoader::Impl::Resolve(uint32 rootEntry) noexcept {
    auto fail = [this](ErrorCode error) noexcept {
        ResetTarjan();
        DiscardNewUnits();
        return Result<uint32>::Failure(error);
    };
    newUnits.Clear();
    if (auto found = FindComponents(rootEntry); !found) {
        return fail(found.Error());
    }

    const auto components = static_cast<uint32>(sccStarts.Size());
    if (auto reserved = units.TryReserve(units.Size() + components); !reserved) {
        return fail(reserved.Error());
    }

    if (auto reserved = newUnits.TryReserve(components); !reserved) {
        return fail(reserved.Error());
    }

    for (uint32 component = 0; component < components; ++component) {
        const uint32 begin = sccStarts[component];
        const uint32 end = component + 1 < components ? sccStarts[component + 1]
                                                      : static_cast<uint32>(sccMembers.Size());
        const uint32 index = TakeUnitSlot();
        (void)newUnits.TryPushBack(index);
        LoaderUnit& unit = units[index];
        if (auto copied =
                unit.members.TryAppend(Span<const uint32>(sccMembers.Data() + begin, end - begin));
            !copied) {
            return fail(copied.Error());
        }

        unit.members.Sort();
    }

    // 새 단위의 구성 항목에 임시로 단위 번호를 적어 간선을 찾는다(실패하면 되돌린다).
    for (uint32 unit : newUnits) {
        for (uint32 member : units[unit].members) {
            entryUnit[member] = unit;
        }
    }

    auto unmark = [this]() noexcept {
        for (uint32 unit : newUnits) {
            for (uint32 member : units[unit].members) {
                entryUnit[member] = LoaderNoIndex;
            }
        }
    };
    for (uint32 unit : newUnits) {
        for (uint32 member : units[unit].members) {
            const auto count =
                static_cast<uint32>(config.manifest->entries[member].dependencies.Size());
            for (uint32 position = 0; position < count; ++position) {
                // PostLoad를 마친 패키지는 단위가 없다(entryUnit 없음). PostLoading 단위는
                // 공개됐어도 이 단위의 PostLoad가 기다리는 의존이다.
                const uint32 target = HardTarget(member, position);
                // PostLoad까지 마친 공개 패키지는 단위가 없다 — 공개 보유로 붙잡는다(11.7절 2).
                if (target != LoaderNoIndex && entryUnit[target] == LoaderNoIndex &&
                    entryLoaded[target] != LoaderNoIndex) {
                    if (!units[unit].publishedDeps.Contains(target)) {
                        if (auto pushed = units[unit].publishedDeps.TryPushBack(target); !pushed) {
                            unmark();
                            return fail(pushed.Error());
                        }
                    }

                    continue;
                }

                const uint32 other = target == LoaderNoIndex ? LoaderNoIndex : entryUnit[target];
                if (other == LoaderNoIndex || other == unit ||
                    units[unit].dependencies.Contains(other)) {
                    continue;
                }

                if (auto pushed = units[unit].dependencies.TryPushBack(other); !pushed) {
                    unmark();
                    return fail(pushed.Error());
                }
            }
        }
    }

    // 대상 단위의 dependents·목록 용량을 미리 잡는다(아래 잇기는 할당하지 않는다). 의존 단위가
    // PostLoad를 마치면 그 구성 패키지가 공개 보유로 넘어오므로(FinishUnit) 그 용량도 잡는다.
    ErrorCode reserved = ErrorCode::None;
    for (uint32 unit : newUnits) {
        usize transferable = units[unit].publishedDeps.Size();
        for (uint32 other : units[unit].dependencies) {
            LoaderUnit& target = units[other];
            LoaderKeepFirst(
                reserved, target.dependents.TryReserve(target.dependents.Size() + newUnits.Size()));
            transferable += target.members.Size();
        }

        LoaderKeepFirst(reserved, units[unit].publishedDeps.TryReserve(transferable));
    }

    LoaderKeepFirst(reserved, admitCandidates.TryReserve(units.Size()));
    LoaderKeepFirst(reserved, readyUnits.TryReserve(units.Size()));
    LoaderKeepFirst(reserved, postLoadUnits.TryReserve(units.Size()));
    LoaderKeepFirst(reserved, workStack.TryReserve(units.Size()));
    LoaderKeepFirst(reserved, units[entryUnit[rootEntry]].requests.TryReserve(1));
    if (reserved != ErrorCode::None) {
        unmark();
        return fail(reserved);
    }

    // 잇기(할당 없음): 생성 순서 = Tarjan 완료 순서(의존 먼저).
    for (uint32 unit : newUnits) {
        LoaderUnit& made = units[unit];
        made.phase = AssetLoaderUnitPhase::Queued;
        // 우선순위는 요청이 Boost로 올린다(새 단위는 모두 root에서 닿는다).
        made.priority = LoadPriority::Low;
        made.sequence = nextSequence++;
        made.queuedAt = admissions;
        for (uint32 entry : made.publishedDeps) {
            ++publishedHolds[entry];
        }

        made.publishedHeld = true;
        for (uint32 other : made.dependencies) {
            LoaderUnit& target = units[other];
            (void)target.dependents.TryPushBack(unit);
            ++target.interest;
            if (target.phase != AssetLoaderUnitPhase::PostLoading) {
                ++made.waitingDependencies;
            }

            if (target.phase == AssetLoaderUnitPhase::Queued) {
                ++made.unadmittedDependencies;
            }
        }
    }

    for (uint32 unit : newUnits) {
        if (units[unit].unadmittedDependencies == 0) {
            (void)admitCandidates.TryPushBack(unit);
        }
    }

    const uint32 target = entryUnit[rootEntry];
    ResetTarjan();
    newUnits.Clear();
    return Result<uint32>::Success(target);
}

void AssetLoader::Impl::Boost(uint32 start, LoadPriority priority) noexcept {
    if (static_cast<uint32>(units[start].priority) >= static_cast<uint32>(priority)) {
        return;
    }

    workStack.Clear();
    units[start].priority = priority;
    (void)workStack.TryPushBack(start);
    while (!workStack.IsEmpty()) {
        const uint32 unit = workStack[workStack.Size() - 1];
        (void)workStack.TryPopBack();
        for (uint32 other : units[unit].dependencies) {
            if (static_cast<uint32>(units[other].priority) < static_cast<uint32>(priority)) {
                units[other].priority = priority;
                (void)workStack.TryPushBack(other);
            }
        }
    }
}

bool AssetLoader::Impl::ValidHandle(AssetLoadHandle handle) const noexcept {
    return handle.index < requests.Size() && requests[handle.index].used &&
           requests[handle.index].generation == handle.generation;
}

void AssetLoader::Impl::FinishRequest(uint32 index, AssetLoadState state, ErrorCode error,
                                      const AssetDiagnostic& diagnostic,
                                      ObjectHandle package) noexcept {
    LoaderRequest& request = requests[index];
    request.state = state;
    request.error = error;
    request.diagnostic = diagnostic;
    request.package = package;
    request.unit = LoaderNoIndex;
    request.cancel = CancelToken();
}

void AssetLoader::Impl::FreeRequest(uint32 index) noexcept {
    LoaderRequest& request = requests[index];
    request.used = false;
    ++request.generation;
    request.cancel = CancelToken();
    request.nextFree = freeRequest;
    freeRequest = index;
}

} // namespace rocks
