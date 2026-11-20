#pragma once

#include "LoaderTestSupport.h"

#include <AssetRuntime/AssetManager.h>

// AssetManager 시험 도구다(Docs/Design/AssetSystem.ko.md 11.7절): 로더 시험의 쿠킹 장면·gate
// IO·손으로 돌리는 worker 위에 AssetManager 하나를 띄우고, 상태 조회·동적 하드 간선·callback 기록을
// 돕는다. 순서는 모두 시험이 정한다(시간·경쟁에 기대지 않는다).
namespace rocks::asset_test {

// 준비 callback의 결과를 기록한다(받은 lease는 keep이 참이면 쥔다).
struct ReadyRecord {
    explicit ReadyRecord(Allocator& allocator) noexcept : leases(allocator) {
    }

    uint32 calls = 0;
    ErrorCode error = ErrorCode::None;
    AssetDiagnostic diagnostic;
    bool keep = true;
    Array<AssetLease> leases;
};

inline AssetReadyCallback RecordReady(Allocator& allocator, ReadyRecord& record) noexcept {
    auto made = AssetReadyCallback::TryCreate(
        allocator, [&record](Result<AssetLease> lease, const AssetDiagnostic& diagnostic) noexcept {
            ++record.calls;
            record.error = lease ? ErrorCode::None : lease.Error();
            record.diagnostic = diagnostic;
            if (lease && record.keep) {
                (void)record.leases.TryPushBack(Move(lease.Value()));
            }
        });
    return made ? Move(made.Value()) : AssetReadyCallback();
}

struct ManagerRig {
    explicit ManagerRig(AssetEnv& env) noexcept
        : env(env), scene(env), io(env.allocator), worker(env.allocator) {
    }

    // 장면을 쿠킹하고 IO에 등록한 뒤 설정을 채운다(AssetManager는 Start가 만든다).
    bool Cook() noexcept {
        if (!scene.Cook() || !io.AddScene(scene)) {
            return false;
        }

        constexpr uint64 ManagerContextHigh = 0x4d414e41474552ull;
        config.loader.allocator = &env.allocator;
        config.loader.types = &env.types;
        config.loader.manifest = &scene.manifest;
        config.loader.cookedRoot = scene.root;
        config.loader.runtimeProfile = "Windows";
        config.loader.io = &io;
        config.loader.worker = &worker;
        config.registryContext = ContextId{ManagerContextHigh, 1};
        config.registryOptions.names = &env.names;
        return true;
    }

    bool Start() noexcept {
        auto made = UniquePtr<AssetManager>::TryMake(env.allocator, config);
        if (!made) {
            return false;
        }

        manager = Move(made.Value());
        return manager->Initialize().IsOk();
    }

    LoadTicket Request(uint64 id, AssetScope* scope = nullptr,
                       LoadPriority priority = LoadPriority::Normal) noexcept {
        AssetRequest request;
        request.id = FixedId(id);
        request.scope = scope;
        request.priority = priority;
        auto ticket = manager->RequestAsset(request);
        return ticket ? Move(ticket.Value()) : LoadTicket();
    }

    bool Pump(const AssetManagerPumpBudget& budget = {}) noexcept {
        return manager->Pump(budget).IsOk();
    }

    // IO·worker를 내보내며 Pump를 로더 단위가 없어질 때까지 되풀이한다(횟수 상한 — 넘으면 false).
    bool Settle(uint32 limit = 1000) noexcept {
        for (uint32 round = 0; round < limit; ++round) {
            io.Complete();
            worker.RunAll();
            if (!Pump()) {
                return false;
            }

            AssetManagerSnapshot snapshot(env.allocator);
            if (!manager->Describe(snapshot)) {
                return false;
            }

            if (snapshot.loader.units.IsEmpty() && io.held.IsEmpty() && worker.pending.IsEmpty() &&
                io.active == 0) {
                return true;
            }
        }

        return false;
    }

    bool IsLoaded(uint64 id) noexcept {
        return manager->Loader().FindPackage(FixedId(id)).IsOk();
    }

    // id가 속한 상주 단위의 정보다(없으면 found false).
    struct UnitView {
        bool found = false;
        AssetManagerUnitInfo info;
        uint32 assets = 0;
    };

    UnitView UnitOf(uint64 id) noexcept {
        UnitView view;
        AssetManagerSnapshot snapshot(env.allocator);
        if (!manager->Describe(snapshot)) {
            return view;
        }

        for (const AssetManagerUnitInfo& info : snapshot.units) {
            for (uint32 index = 0; index < info.assetCount; ++index) {
                if (snapshot.unitAssets[info.firstAsset + index] == FixedId(id)) {
                    view.found = true;
                    view.info = info;
                    view.assets = info.assetCount;
                    return view;
                }
            }
        }

        return view;
    }

    usize UnitCount() noexcept {
        AssetManagerSnapshot snapshot(env.allocator);
        return manager->Describe(snapshot) ? snapshot.units.Size() : 0;
    }

    // 공개된 owner 에셋의 hard 필드를 target 에셋으로 바꾼다(target 0이면 비운다 — 동적 하드 간선).
    bool SetHard(uint64 owner, uint64 target) noexcept {
        ObjectRegistry& registry = manager->Registry();
        const ObjectAccessContext access = registry.OwnerAccess();
        const ObjectHandle ownerObject = AssetObject(owner);
        auto borrow = registry.ResolveForWrite<TestAsset>(access, ownerObject);
        if (!borrow) {
            return false;
        }

        const ObjectHandle targetObject = target == 0 ? ObjectHandle{} : AssetObject(target);
        return registry
            .SetReference(access, ownerObject, borrow.Value().Get().Value()->hard, targetObject)
            .IsOk();
    }

    // owner의 hardList 끝에 target을 더한다(같은 쌍의 두 번째 간선 — 다중도).
    bool AddHardListEntry(uint64 owner, uint64 target) noexcept {
        ObjectRegistry& registry = manager->Registry();
        const ObjectAccessContext access = registry.OwnerAccess();
        const ObjectHandle ownerObject = AssetObject(owner);
        auto borrow = registry.ResolveForWrite<TestAsset>(access, ownerObject);
        if (!borrow) {
            return false;
        }

        ObjectRefArray<Object>& list = borrow.Value().Get().Value()->hardList;
        return registry
            .InsertReferenceAt(access, ownerObject, list, list.Size(), AssetObject(target))
            .IsOk();
    }

    ObjectHandle AssetObject(uint64 id) noexcept {
        ObjectRegistry& registry = manager->Registry();
        auto package = manager->Loader().FindPackage(FixedId(id));
        if (!package) {
            return ObjectHandle{};
        }

        auto found = registry.FindObject(registry.OwnerAccess(), package.Value(), "");
        return found ? found.Value() : ObjectHandle{};
    }

    AssetEnv& env;
    LoaderScene scene;
    TestPackageIo io;
    TestLoaderWorker worker;
    AssetManagerConfig config;
    UniquePtr<AssetManager> manager;
};

} // namespace rocks::asset_test
