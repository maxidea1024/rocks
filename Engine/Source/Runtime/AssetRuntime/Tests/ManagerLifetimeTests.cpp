#include "ManagerTestSupport.h"

#include <AssetTestFixtures/TestAssets.generated.h>

// AssetManager 상주 수명 시험이다(Docs/Design/AssetSystem 11.4·11.7절 3·4, 시나리오
// 16·23·28): 해제 뒤 Discard·같은 id 새 로드, Closing 되살리기, 하드 순환 단위 해제, 의존 보유,
// 로드 중 의존 보유(로더 공개 보유), 동적 하드 간선 계수와 다중도, 동적 순환 병합. IO·worker는
// 시험이 내보낸다.
using namespace rocks;
namespace manager_lifetime_tests_file {

using namespace rocks::asset_test;

namespace {

AssetManagerPumpBudget NoDiscards() noexcept {
    AssetManagerPumpBudget budget;
    budget.maxDiscards = 0;
    return budget;
}

} // namespace

// 수명 기본(시나리오 16·23): 티켓·callback lease가 상주를 붙잡고, 둘 다 놓으면 다음 Pump가
// Discard해 로더가 패키지를 놓는다. 같은 id를 다시 요청하면 GC 전이어도 새 로드로 다른 패키지가
// 생긴다.
TEST(AssetManager_ReleaseDiscardsAndReloads) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    ReadyRecord ready(env.allocator);
    ObjectHandle first;
    {
        LoadTicket ticket = rig.Request(1);
        REQUIRE(ticket.IsValid());
        REQUIRE(rig.manager->OnReady(ticket, TaskLane::Game, RecordReady(env.allocator, ready)));
        REQUIRE(rig.Settle());
        REQUIRE(ready.calls == 1);
        CHECK(ready.error == ErrorCode::None && ready.leases.Size() == 1);
        CHECK(ready.leases[0].IsValid() && ready.leases[0].Asset() == FixedId(1));
        CHECK(ready.leases[0].Object() == rig.AssetObject(1));
        first = ready.leases[0].Package();
        const auto unit = rig.UnitOf(1);
        REQUIRE(unit.found);
        CHECK(unit.info.state == ResidentUnitState::Resident && unit.info.tickets == 1 &&
              unit.info.leases == 1 && unit.assets == 1);
    }

    // 티켓을 놓아도 lease가 붙잡는다.
    REQUIRE(rig.Pump());
    CHECK(rig.IsLoaded(1));
    CHECK(rig.UnitOf(1).info.tickets == 0 && rig.UnitOf(1).info.leases == 1);
    ready.leases.Clear();
    REQUIRE(rig.Pump());
    CHECK(!rig.IsLoaded(1));
    CHECK(!rig.UnitOf(1).found && rig.UnitCount() == 0);

    // 같은 id의 새 요청은 새 로드다(옛 패키지는 Discard돼 GC를 기다린다).
    LoadTicket again = rig.Request(1);
    REQUIRE(again.IsValid());
    REQUIRE(rig.Settle());
    REQUIRE(rig.IsLoaded(1));
    CHECK(!(rig.manager->Loader().FindPackage(FixedId(1)).Value() == first));
    CHECK(rig.UnitOf(1).info.tickets == 1);
}

// Closing 되살리기(11.7절 3·4): 보유가 0이 된 단위는 Closing이고(Discard 예산 0으로 멈춤), 그 사이
// 같은 id 요청은 그 자리에서 보유를 1 올려 다음 판정에서 Resident로 돌아온다(새 로드가 아니다).
TEST(AssetManager_ClosingUnitRevivesOnRequest) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    ObjectHandle package;
    {
        LoadTicket ticket = rig.Request(1);
        REQUIRE(rig.Settle());
        package = rig.manager->Loader().FindPackage(FixedId(1)).Value();
    }

    REQUIRE(rig.Pump(NoDiscards()));
    REQUIRE(rig.UnitOf(1).found);
    CHECK(rig.UnitOf(1).info.state == ResidentUnitState::Closing);
    CHECK(rig.IsLoaded(1));

    LoadTicket revive = rig.Request(1);
    REQUIRE(revive.IsValid());
    // 로더가 바로 Succeeded로 답해 보유가 그 자리에서 걸린다.
    CHECK(rig.UnitOf(1).info.tickets == 1);
    REQUIRE(rig.Pump());
    CHECK(rig.UnitOf(1).info.state == ResidentUnitState::Resident);
    CHECK(rig.manager->Loader().FindPackage(FixedId(1)).Value() == package);
    CHECK(rig.io.begun.Size() == 1);
}

// 하드 순환 단위(11.3·11.4절): 1 ↔ 2는 한 상주 단위이고 내부 간선은 보유가 아니다 — 요청을 놓으면
// 둘 다 같은 Pump에서 Discard된다.
TEST(AssetManager_HardCycleUnitReleasesTogether) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    rig.scene.Hard(1, 2);
    rig.scene.Hard(2, 1);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    {
        LoadTicket ticket = rig.Request(1);
        REQUIRE(rig.Settle());
        const auto unit = rig.UnitOf(1);
        REQUIRE(unit.found);
        CHECK(unit.assets == 2 && unit.info.inboundEdges == 0 && unit.info.tickets == 1);
        CHECK(rig.UnitOf(2).info.firstAsset == unit.info.firstAsset);
    }

    REQUIRE(rig.Pump());
    CHECK(!rig.IsLoaded(1) && !rig.IsLoaded(2));
    CHECK(rig.UnitCount() == 0);
}

// 의존 보유(11.4절 시나리오 16 "의존 retention"): 1 → 2에서 2는 별도 단위이고 1에서 오는 간선
// 다중도 1이 보유다. 1을 놓으면 1이 먼저 Discard되고, 그 Discard가 보고한 음수 간선이 다음 Pump에
// 반영돼 2가 Discard된다(연쇄 — 결정적).
TEST(AssetManager_DependencyHeldByEdgeUntilReferrerDiscarded) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    rig.scene.Hard(1, 2);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    {
        LoadTicket ticket = rig.Request(1);
        REQUIRE(rig.Settle());
        REQUIRE(rig.UnitCount() == 2);
        CHECK(rig.UnitOf(2).info.inboundEdges == 1 && rig.UnitOf(2).info.tickets == 0);
        CHECK(rig.UnitOf(1).info.inboundEdges == 0);
    }

    REQUIRE(rig.Pump());
    CHECK(!rig.IsLoaded(1));
    CHECK(rig.IsLoaded(2));
    REQUIRE(rig.Pump());
    CHECK(!rig.IsLoaded(2));
    CHECK(rig.UnitCount() == 0);
}

// 로드 중 의존 보유(11.4절, 11.7절 2): 상주한 2에 Hard로 기대는 1이 IO 중이면 2의 티켓을 놓아도
// 로더 공개 보유가 2를 붙잡는다. 1이 공개되면 그 Commit 간선이 이어서 붙잡는다.
TEST(AssetManager_LoadingDependentHoldsResidentDependency) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    rig.scene.Hard(1, 2);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    LoadTicket dependent;
    {
        LoadTicket dependency = rig.Request(2);
        REQUIRE(rig.Settle());
        dependent = rig.Request(1);
        REQUIRE(dependent.IsValid());
        // 허가·IO 시작까지(완료는 붙잡는다).
        REQUIRE(rig.Pump());
        REQUIRE(!rig.io.held.IsEmpty() || rig.io.active != 0);
    }

    REQUIRE(rig.Pump());
    REQUIRE(rig.UnitOf(2).found);
    CHECK(rig.IsLoaded(2));
    CHECK(rig.UnitOf(2).info.state == ResidentUnitState::Resident);
    CHECK(rig.UnitOf(2).info.loaderHolds == 1 && rig.UnitOf(2).info.tickets == 0);
    REQUIRE(rig.Settle());
    REQUIRE(rig.IsLoaded(1));
    CHECK(rig.IsLoaded(2));
    CHECK(rig.UnitOf(2).info.loaderHolds == 0 && rig.UnitOf(2).info.inboundEdges == 1);
}

// 동적 하드 간선과 다중도(6.1절, 시나리오 23·28): 공개된 1의 필드 둘이 2를 가리키면 다중도 2가
// 보유다. 하나를 지워도 1이 남아 2는 상주이고, 1이 Discard되면 2도 이어서 Discard된다.
TEST(AssetManager_DynamicEdgeMultiplicityHolds) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    LoadTicket owner = rig.Request(1);
    {
        LoadTicket target = rig.Request(2);
        REQUIRE(rig.Settle());
        REQUIRE(rig.SetHard(1, 2));
        REQUIRE(rig.AddHardListEntry(1, 2));
        REQUIRE(rig.Pump());
        CHECK(rig.UnitOf(2).info.inboundEdges == 2);
    }

    REQUIRE(rig.Pump());
    CHECK(rig.IsLoaded(2));
    REQUIRE(rig.SetHard(1, 0));
    REQUIRE(rig.Pump());
    CHECK(rig.IsLoaded(2));
    CHECK(rig.UnitOf(2).info.inboundEdges == 1);

    owner = LoadTicket();
    REQUIRE(rig.Pump());
    CHECK(!rig.IsLoaded(1) && rig.IsLoaded(2));
    REQUIRE(rig.Pump());
    CHECK(!rig.IsLoaded(2));
}

// 동적 순환 병합(6.1절 union-find, 시나리오 28): 따로 상주한 1·2 사이에 1 → 2, 2 → 1 간선이 생기면
// 한 단위로 합쳐지고 내부 간선은 보유가 아니다 — 두 티켓을 놓으면 둘 다 같은 Pump에서 Discard된다.
// 간선을 지워도 나누지 않는다(보수 규칙).
TEST(AssetManager_DynamicCycleMergesUnits) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    {
        LoadTicket first = rig.Request(1);
        LoadTicket second = rig.Request(2);
        REQUIRE(rig.Settle());
        REQUIRE(rig.UnitCount() == 2);
        REQUIRE(rig.SetHard(1, 2));
        REQUIRE(rig.Pump());
        CHECK(rig.UnitCount() == 2);
        REQUIRE(rig.SetHard(2, 1));
        REQUIRE(rig.Pump());
        CHECK(rig.UnitCount() == 1);
        const auto merged = rig.UnitOf(1);
        CHECK(merged.assets == 2 && merged.info.inboundEdges == 0 && merged.info.tickets == 2);
        REQUIRE(rig.SetHard(2, 0));
        REQUIRE(rig.Pump());
        CHECK(rig.UnitCount() == 1);
    }

    REQUIRE(rig.Pump());
    CHECK(!rig.IsLoaded(1) && !rig.IsLoaded(2));
    CHECK(rig.UnitCount() == 0);
}

} // namespace manager_lifetime_tests_file
