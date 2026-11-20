#include "AssetManagerInternal.h"

#include <Core/Diagnostics/Log.h>

// AssetManager의 상주 단위·간선 표·병합·Closing·Discarding이다(Docs/Design/AssetSystem.ko.md 11.7절
// 3·4).
namespace rocks {

namespace {

constexpr LogCategory LogAssetManager{"AssetManager"};

} // namespace

Result<uint32> AssetManager::Impl::TakeUnit() noexcept {
    if (freeUnit == ManagerNoIndex) {
        if (auto added = units.TryEmplaceBack(); !added) {
            return Result<uint32>::Failure(added.Error());
        }

        freeUnit = static_cast<uint32>(units.Size() - 1);
        units[freeUnit].nextFree = ManagerNoIndex;
    }

    const uint32 index = freeUnit;
    ManagerUnit& unit = units[index];
    freeUnit = unit.nextFree;
    unit = ManagerUnit{};
    unit.used = true;
    return Result<uint32>::Success(index);
}

void AssetManager::Impl::FreeUnit(uint32 index) noexcept {
    ManagerUnit& unit = units[index];
    unit = ManagerUnit{};
    unit.nextFree = freeUnit;
    freeUnit = index;
}

uint32 AssetManager::Impl::UnitOf(ObjectHandle package) const noexcept {
    const uint32* found = packageOf.Find(package);
    return found == nullptr ? ManagerNoIndex : packages[*found].unit;
}

void AssetManager::Impl::AddHold(uint32 package, uint64 ManagerUnit::* counter) noexcept {
    ++(units[packages[package].unit].*counter);
}

void AssetManager::Impl::DropHold(uint32 package, uint64 ManagerUnit::* counter) noexcept {
    --(units[packages[package].unit].*counter);
}

// 로더가 PostLoad까지 끝낸 단위를 상주 단위로 만든다(11.7절 3). 받은 기록은 published에 쌓고 앞에서
// 부터 처리한다 — 할당이 실패하면 남은 것을 다음 Pump에서 다시 한다.
Result<void> AssetManager::Impl::IngestPublished() noexcept {
    if (auto taken = loader->TakePublished(published); !taken) {
        return taken;
    }

    usize begin = 0;
    ErrorCode failure = ErrorCode::None;
    while (begin < published.Size()) {
        usize end = begin + 1;
        while (end < published.Size() && published[end].group == published[begin].group) {
            ++end;
        }

        const auto count = static_cast<uint32>(end - begin);
        // 할당을 모두 먼저 잡는다(아래 잇기는 실패하지 않는다).
        uint32 freePackages = 0;
        for (uint32 index = freePackage; index != ManagerNoIndex && freePackages < count;
             index = packages[index].nextFree) {
            ++freePackages;
        }

        if (auto reserved = packages.TryReserve(packages.Size() + (count - freePackages));
            !reserved) {
            failure = reserved.Error();
            break;
        }

        if (auto reserved = packageOf.Reserve(packageOf.Count() + count); !reserved) {
            failure = reserved.Error();
            break;
        }

        // 순환 후보(쌓여 있던 간선 수)와 탐색 작업 공간을 잡는다.
        usize candidates = 0;
        for (usize index = begin; index < end; ++index) {
            if (const uint32* node = edgeNodeOf.Find(published[index].package)) {
                candidates += edgeNodes[*node].out.Size() + edgeNodes[*node].in.Size();
            }
        }

        ErrorCode work = ErrorCode::None;
        auto keep = [&work](Result<void>&& result) noexcept {
            if (work == ErrorCode::None && !result) {
                work = result.Error();
            }
        };
        keep(cycleOwners.TryReserve(candidates));
        keep(cycleTargets.TryReserve(candidates));
        keep(workStack.TryReserve(units.Size() + 1));
        keep(mergeSet.TryReserve(units.Size() + 1));
        if (work != ErrorCode::None) {
            failure = work;
            break;
        }

        auto made = TakeUnit();
        if (!made) {
            failure = made.Error();
            break;
        }

        const uint32 unitIndex = made.Value();
        for (usize index = begin; index < end; ++index) {
            const AssetLoaderPublished& record = published[index];
            uint32 slot = freePackage;
            if (slot != ManagerNoIndex) {
                freePackage = packages[slot].nextFree;
            } else {
                (void)packages.TryEmplaceBack();
                slot = static_cast<uint32>(packages.Size() - 1);
            }

            ManagerPackage& package = packages[slot];
            package = ManagerPackage{};
            package.used = true;
            package.asset = record.asset;
            package.handle = record.package;
            package.loaderSlot = record.slot;
            package.unit = unitIndex;
            const CookedManifestEntry* entry = loaderConfig.manifest->Find(record.asset);
            package.entry = static_cast<uint32>(entry - loaderConfig.manifest->entries.Data());
            auto representative = registry->FindObject(access, record.package, "");
            package.assetObject = representative ? representative.Value() : ObjectHandle{};
            (void)packageOf.Insert(record.package, slot);
            entryPackage[package.entry] = slot;
            ManagerUnit& unit = units[unitIndex];
            if (unit.lastPackage == ManagerNoIndex) {
                unit.firstPackage = slot;
            } else {
                packages[unit.lastPackage].nextInUnit = slot;
            }

            unit.lastPackage = slot;
            ++unit.packageCount;
        }

        // 넘어오기 전에 쌓인 간선: 들어오는 간선은 보유다(다른 단위·아직 상주가 아닌 패키지에서).
        // 다른 상주 단위 사이에 새 단위 간선이 생기면 순환을 본다.
        units[unitIndex].inbound = InboundOf(unitIndex);
        cycleOwners.Clear();
        cycleTargets.Clear();
        for (uint32 member = units[unitIndex].firstPackage; member != ManagerNoIndex;
             member = packages[member].nextInUnit) {
            const uint32* node = edgeNodeOf.Find(packages[member].handle);
            if (node == nullptr) {
                continue;
            }

            for (ObjectHandle target : edgeNodes[*node].out) {
                const uint32 other = UnitOf(target);
                if (other != ManagerNoIndex && other != unitIndex) {
                    (void)cycleOwners.TryPushBack(unitIndex);
                    (void)cycleTargets.TryPushBack(other);
                }
            }

            for (ObjectHandle owner : edgeNodes[*node].in) {
                const uint32 other = UnitOf(owner);
                if (other != ManagerNoIndex && other != unitIndex) {
                    (void)cycleOwners.TryPushBack(other);
                    (void)cycleTargets.TryPushBack(unitIndex);
                }
            }
        }

        for (usize index = 0; index < cycleOwners.Size(); ++index) {
            CheckCycle(cycleOwners[index], cycleTargets[index]);
        }

        begin = end;
    }

    (void)published.TryRemoveAt(0, begin);
    return failure == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(failure);
}

// 단위로 들어오는 간선 다중도의 합이다(같은 단위 안의 간선은 세지 않는다).
int64 AssetManager::Impl::InboundOf(uint32 unit) const noexcept {
    int64 inbound = 0;
    for (uint32 member = units[unit].firstPackage; member != ManagerNoIndex;
         member = packages[member].nextInUnit) {
        const ObjectHandle target = packages[member].handle;
        const uint32* node = edgeNodeOf.Find(target);
        if (node == nullptr) {
            continue;
        }

        for (ObjectHandle owner : edgeNodes[*node].in) {
            if (UnitOf(owner) != unit) {
                inbound += *edges.Find(ManagerEdgeKey{owner, target});
            }
        }
    }

    return inbound;
}

void AssetManager::Impl::LinkEdge(ObjectHandle owner, ObjectHandle target) noexcept {
    // 용량은 ApplyEdgeChanges가 미리 잡았다.
    const uint32 ownerNode = *edgeNodeOf.Find(owner);
    const uint32 targetNode = *edgeNodeOf.Find(target);
    (void)edgeNodes[ownerNode].out.TryPushBack(target);
    (void)edgeNodes[targetNode].in.TryPushBack(owner);
}

void AssetManager::Impl::UnlinkEdge(ObjectHandle owner, ObjectHandle target) noexcept {
    auto removeFrom = [this](ObjectHandle node, ObjectHandle value, bool outgoing) noexcept {
        const uint32* index = edgeNodeOf.Find(node);
        Array<ObjectHandle>& list = outgoing ? edgeNodes[*index].out : edgeNodes[*index].in;
        for (usize position = 0; position < list.Size(); ++position) {
            if (list[position] == value) {
                list[position] = list[list.Size() - 1];
                (void)list.TryPopBack();
                break;
            }
        }

        if (edgeNodes[*index].in.IsEmpty() && edgeNodes[*index].out.IsEmpty()) {
            (void)freeEdgeNodes.TryPushBack(*index);
            (void)edgeNodeOf.Erase(node);
        }
    };
    removeFrom(owner, target, true);
    removeFrom(target, owner, false);
}

// 관찰자 묶음을 간선 표에 적용한다(11.7절 3 — 묶음은 키마다 순 변화라 키마다 전후 값으로 판정한다).
// 할당은 모두 먼저 잡고 실패하면 묶음을 쥔 채 다음 Pump에서 다시 한다.
Result<void> AssetManager::Impl::ApplyEdgeChanges() noexcept {
    if (changes.IsEmpty()) {
        if (auto taken = observer->TakeChanges(changes); !taken) {
            return taken;
        }
    }

    usize newKeys = 0;
    usize newNodes = 0;
    for (const ReferenceEdgeDelta& change : changes) {
        const int64* found = edges.Find(ManagerEdgeKey{change.ownerPackage, change.targetPackage});
        const int64 before = found == nullptr ? 0 : *found;
        if (before == 0 && before + change.delta > 0) {
            ++newKeys;
            newNodes += edgeNodeOf.Contains(change.ownerPackage) ? 0 : 1;
            newNodes += edgeNodeOf.Contains(change.targetPackage) ? 0 : 1;
        }
    }

    const usize reusable = freeEdgeNodes.Size() < newNodes ? freeEdgeNodes.Size() : newNodes;
    ErrorCode reserve = ErrorCode::None;
    auto keep = [&reserve](Result<void>&& result) noexcept {
        if (reserve == ErrorCode::None && !result) {
            reserve = result.Error();
        }
    };
    keep(edges.Reserve(edges.Count() + newKeys));
    keep(edgeNodeOf.Reserve(edgeNodeOf.Count() + newNodes));
    keep(edgeNodes.TryReserve(edgeNodes.Size() + (newNodes - reusable)));
    keep(freeEdgeNodes.TryReserve(freeEdgeNodes.Size() + 2 * changes.Size()));
    keep(cycleOwners.TryReserve(changes.Size()));
    keep(cycleTargets.TryReserve(changes.Size()));
    keep(workStack.TryReserve(units.Size()));
    keep(mergeSet.TryReserve(units.Size()));
    if (reserve != ErrorCode::None) {
        return Result<void>::Failure(reserve);
    }

    // 새 이웃 칸을 만들고 그 목록 용량을 잡는다(아래 잇기는 할당하지 않는다).
    for (const ReferenceEdgeDelta& change : changes) {
        const int64* found = edges.Find(ManagerEdgeKey{change.ownerPackage, change.targetPackage});
        const int64 before = found == nullptr ? 0 : *found;
        if (!(before == 0 && before + change.delta > 0)) {
            continue;
        }

        const ObjectHandle ends[2] = {change.ownerPackage, change.targetPackage};
        for (ObjectHandle node : ends) {
            if (edgeNodeOf.Contains(node)) {
                continue;
            }

            uint32 index = 0;
            if (!freeEdgeNodes.IsEmpty()) {
                index = freeEdgeNodes.TryPopBack().Value();
                edgeNodes[index].in.Clear();
                edgeNodes[index].out.Clear();
            } else {
                (void)edgeNodes.TryEmplaceBack(allocator);
                index = static_cast<uint32>(edgeNodes.Size() - 1);
            }

            (void)edgeNodeOf.Insert(node, index);
        }

        // 같은 패키지가 여러 새 키에 나오므로 용량을 하나씩 더해 간다.
        ManagerEdgeNode& owner = edgeNodes[*edgeNodeOf.Find(change.ownerPackage)];
        keep(owner.out.TryReserve(owner.out.Capacity() + 1));
        ManagerEdgeNode& target = edgeNodes[*edgeNodeOf.Find(change.targetPackage)];
        keep(target.in.TryReserve(target.in.Capacity() + 1));
    }

    if (reserve != ErrorCode::None) {
        // 만든 빈 이웃 칸은 남아도 무해하다(목록이 비어 있고 다음 시도가 다시 쓴다).
        return Result<void>::Failure(reserve);
    }

    cycleOwners.Clear();
    cycleTargets.Clear();
    for (const ReferenceEdgeDelta& change : changes) {
        const ManagerEdgeKey key{change.ownerPackage, change.targetPackage};
        int64* found = edges.Find(key);
        const int64 before = found == nullptr ? 0 : *found;
        const int64 after = before + change.delta;
        if (after < 0) {
            platform::Fatal("AssetManager edge multiplicity became negative");
        }

        if (after == 0) {
            if (found != nullptr) {
                (void)edges.Erase(key);
                UnlinkEdge(key.owner, key.target);
            }
        } else if (found != nullptr) {
            *found = after;
        } else {
            (void)edges.Insert(key, after);
            LinkEdge(key.owner, key.target);
        }

        const uint32 ownerUnit = UnitOf(key.owner);
        const uint32 targetUnit = UnitOf(key.target);
        if (targetUnit == ManagerNoIndex || targetUnit == ownerUnit) {
            continue;
        }

        units[targetUnit].inbound += change.delta;
        if (units[targetUnit].state == ResidentUnitState::Discarding && change.delta > 0) {
            lateEdgesIntoDiscarding += static_cast<uint64>(change.delta);
            if (!lateEdgeWarned) {
                lateEdgeWarned = true;
                Log::Warning(LogAssetManager,
                             "A hard reference was set into a package that is being discarded; "
                             "it will point at destroyed objects");
            }
        }

        if (before == 0 && after > 0 && ownerUnit != ManagerNoIndex) {
            (void)cycleOwners.TryPushBack(ownerUnit);
            (void)cycleTargets.TryPushBack(targetUnit);
        }
    }

    changes.Clear();
    for (usize index = 0; index < cycleOwners.Size(); ++index) {
        // 앞의 병합으로 단위가 바뀌었을 수 있다 — 지금의 단위로 다시 본다(CheckCycle이 같은 단위·
        // 지운 단위를 거른다).
        CheckCycle(cycleOwners[index], cycleTargets[index]);
    }

    return Result<void>::Success();
}

// 새 단위 간선 from → to가 to에서 from으로 가는 경로를 만들면 그 경로 위 단위를 합친다(전방 도달 ∩
// 후방 도달 — 11.7절 3). Discarding 단위는 지나지 않는다. 작업 공간 용량은 Pump가 단위 표 크기로
// 잡았다.
void AssetManager::Impl::CheckCycle(uint32 from, uint32 to) noexcept {
    if (from == to || !units[from].used || !units[to].used ||
        units[from].state == ResidentUnitState::Discarding ||
        units[to].state == ResidentUnitState::Discarding) {
        return;
    }

    const uint64 forward = ++stamp;
    workStack.Clear();
    (void)workStack.TryPushBack(to);
    units[to].visitStamp = forward;
    bool reached = false;
    while (!workStack.IsEmpty()) {
        const uint32 current = workStack.TryPopBack().Value();
        for (uint32 member = units[current].firstPackage; member != ManagerNoIndex;
             member = packages[member].nextInUnit) {
            const uint32* node = edgeNodeOf.Find(packages[member].handle);
            if (node == nullptr) {
                continue;
            }

            for (ObjectHandle target : edgeNodes[*node].out) {
                const uint32 next = UnitOf(target);
                if (next == ManagerNoIndex || units[next].visitStamp == forward ||
                    units[next].state == ResidentUnitState::Discarding) {
                    continue;
                }

                units[next].visitStamp = forward;
                reached = reached || next == from;
                (void)workStack.TryPushBack(next);
            }
        }
    }

    if (!reached) {
        return;
    }

    // from에서 거꾸로 가며 전방 도달 집합 안의 단위만 모은다.
    const uint64 backward = ++stamp;
    mergeSet.Clear();
    workStack.Clear();
    (void)workStack.TryPushBack(from);
    units[from].backStamp = backward;
    while (!workStack.IsEmpty()) {
        const uint32 current = workStack.TryPopBack().Value();
        (void)mergeSet.TryPushBack(current);
        for (uint32 member = units[current].firstPackage; member != ManagerNoIndex;
             member = packages[member].nextInUnit) {
            const uint32* node = edgeNodeOf.Find(packages[member].handle);
            if (node == nullptr) {
                continue;
            }

            for (ObjectHandle owner : edgeNodes[*node].in) {
                const uint32 previous = UnitOf(owner);
                if (previous == ManagerNoIndex || units[previous].visitStamp != forward ||
                    units[previous].backStamp == backward) {
                    continue;
                }

                units[previous].backStamp = backward;
                (void)workStack.TryPushBack(previous);
            }
        }
    }

    MergeUnits();
}

// mergeSet의 단위를 가장 작은 번호의 단위로 합친다(목록 이어 붙이기·계수 합산, Closing은 되살리고
// 들어오는 간선은 다시 센다 — 할당 없음).
void AssetManager::Impl::MergeUnits() noexcept {
    uint32 root = mergeSet[0];
    for (uint32 unit : mergeSet) {
        root = unit < root ? unit : root;
    }

    ManagerUnit& target = units[root];
    for (uint32 unit : mergeSet) {
        if (unit == root) {
            continue;
        }

        ManagerUnit& source = units[unit];
        for (uint32 member = source.firstPackage; member != ManagerNoIndex;
             member = packages[member].nextInUnit) {
            packages[member].unit = root;
        }

        packages[target.lastPackage].nextInUnit = source.firstPackage;
        target.lastPackage = source.lastPackage;
        target.packageCount += source.packageCount;
        target.tickets += source.tickets;
        target.leases += source.leases;
        target.deferred += source.deferred;
        FreeUnit(unit);
    }

    target.state = ResidentUnitState::Resident;
    target.inbound = InboundOf(root);
}

uint64 AssetManager::Impl::LoaderHolds(uint32 unit) const noexcept {
    uint64 holds = 0;
    for (uint32 member = units[unit].firstPackage; member != ManagerNoIndex;
         member = packages[member].nextInUnit) {
        auto held = loader->PublishedHolds(packages[member].asset);
        holds += held ? held.Value() : 0;
    }

    return holds;
}

// 보유 전체를 지금 다시 계산해 Resident ↔ Closing을 정한다(11.7절 3·4).
void AssetManager::Impl::JudgeClosing() noexcept {
    for (usize index = 0; index < units.Size(); ++index) {
        ManagerUnit& unit = units[index];
        if (!unit.used || unit.state == ResidentUnitState::Discarding) {
            continue;
        }

        const bool held = unit.tickets + unit.leases + unit.deferred != 0 || unit.inbound != 0 ||
                          LoaderHolds(static_cast<uint32>(index)) != 0;
        unit.state = held ? ResidentUnitState::Resident : ResidentUnitState::Closing;
    }
}

// Closing·Discarding 단위를 진행한다(11.7절 3). 돌려주는 값은 Discard한 패키지 수다. 작업 공간
// (scratchAssets·scratchSlots) 용량은 Pump가 패키지 표 크기로 잡았다.
uint32 AssetManager::Impl::ProcessClosing(uint32 maxDiscards) noexcept {
    uint32 discarded = 0;
    for (usize index = 0; index < units.Size() && discarded < maxDiscards; ++index) {
        ManagerUnit& unit = units[index];
        if (!unit.used || unit.state == ResidentUnitState::Resident) {
            continue;
        }

        if (unit.state == ResidentUnitState::Closing) {
            // 첫 Discard가 성공하면 같은 자리에서 로더에서 퇴출한다(그 사이 요청이 Discard된
            // 패키지를 받지 않게). 실패하면 아무것도 바뀌지 않았으니 Closing으로 남는다.
            if (!registry->Discard(access, packages[unit.firstPackage].handle)) {
                continue;
            }

            ++discarded;
            packages[unit.firstPackage].discarded = true;
            scratchAssets.Clear();
            for (uint32 member = unit.firstPackage; member != ManagerNoIndex;
                 member = packages[member].nextInUnit) {
                (void)scratchAssets.TryPushBack(packages[member].asset);
                entryPackage[packages[member].entry] = ManagerNoIndex;
            }

            if (!loader->RetirePublished(scratchAssets.View())) {
                platform::Fatal("AssetManager could not retire a closing unit from the loader");
            }

            unit.state = ResidentUnitState::Discarding;
            unit.discardNext = packages[unit.firstPackage].nextInUnit;
        }

        while (unit.discardNext != ManagerNoIndex && discarded < maxDiscards) {
            if (!registry->Discard(access, packages[unit.discardNext].handle)) {
                break;
            }

            ++discarded;
            packages[unit.discardNext].discarded = true;
            unit.discardNext = packages[unit.discardNext].nextInUnit;
        }

        if (unit.discardNext == ManagerNoIndex) {
            scratchSlots.Clear();
            for (uint32 member = unit.firstPackage; member != ManagerNoIndex;
                 member = packages[member].nextInUnit) {
                (void)scratchSlots.TryPushBack(packages[member].loaderSlot);
            }

            // RootRef 칸을 돌려주는 할당이 실패하면 다음 Pump에서 다시 한다.
            if (loader->ReleaseRetired(scratchSlots.View())) {
                EndUnit(static_cast<uint32>(index));
            }
        }
    }

    return discarded;
}

void AssetManager::Impl::EndUnit(uint32 index) noexcept {
    uint32 member = units[index].firstPackage;
    while (member != ManagerNoIndex) {
        const uint32 next = packages[member].nextInUnit;
        (void)packageOf.Erase(packages[member].handle);
        packages[member] = ManagerPackage{};
        packages[member].nextFree = freePackage;
        freePackage = member;
        member = next;
    }

    FreeUnit(index);
}

} // namespace rocks
