#include "ManagerTestSupport.h"

#include <AssetTestFixtures/TestAssets.generated.h>
#include <Object/ConstructionContext.h>

// AssetManager 단위 그래프 시험이다(Docs/Design/AssetSystem 11.7절 3·4): 순환 탐색의
// 전방·후방 경계(도달 뒤 더 가는 단위, 전방 집합 밖에서 오는 간선), 한 묶음의 여러 새 간선이 같은
// 순환을 이룰 때 한 번만 합치기, Discard 예산이 단위 중간에서 끝날 때, 두 로더 단위가 함께 PostLoad
// 중일 때 서로를 가리키는 간선의 넘겨받기 순서.
using namespace rocks;
namespace manager_graph_tests_file {

using namespace rocks::asset_test;

// 순환 탐색 경계: 11 → 12 → 13 → 14, 10 → 12가 있을 때 13 → 11이 생기면 {11, 12, 13}만 합친다 —
// 전방 탐색은 11에 닿은 뒤에도 14까지 가고, 후방 탐색은 전방 집합 밖의 10에서 오는 간선을 거른다.
// 합친 단위의 보유는 10에서 오는 간선 하나다.
TEST(AssetManager_CycleSearchBoundaries) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    for (uint64 id = 10; id <= 14; ++id) {
        rig.scene.Add(id);
    }

    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    LoadTicket tickets[5];
    for (uint64 id = 10; id <= 14; ++id) {
        tickets[id - 10] = rig.Request(id);
    }

    REQUIRE(rig.Settle());
    REQUIRE(rig.SetHard(11, 12));
    REQUIRE(rig.SetHard(12, 13));
    REQUIRE(rig.SetHard(13, 14));
    REQUIRE(rig.AddHardListEntry(10, 12));
    REQUIRE(rig.Pump());
    CHECK(rig.UnitCount() == 5);
    REQUIRE(rig.AddHardListEntry(13, 11));
    REQUIRE(rig.Pump());
    CHECK(rig.UnitCount() == 3);
    const auto merged = rig.UnitOf(12);
    CHECK(merged.assets == 3 && merged.info.inboundEdges == 1 && merged.info.tickets == 3);
    CHECK(rig.UnitOf(14).assets == 1 && rig.UnitOf(14).info.inboundEdges == 1);
    CHECK(rig.UnitOf(10).assets == 1 && rig.UnitOf(10).info.inboundEdges == 0);

    for (LoadTicket& ticket : tickets) {
        ticket = LoadTicket();
    }

    // 10 → 합친 단위 → 14 사슬이 앞에서부터 하나씩 Discard된다.
    for (uint32 round = 0; round < 3; ++round) {
        REQUIRE(rig.Pump());
    }

    CHECK(rig.UnitCount() == 0);
}

// 한 묶음의 여러 새 간선(11.7절 3 — 묶음 전체를 적용한 뒤 순환을 본다): 1 → 2, 2 → 1, 1 → 3, 3 →
// 1을 Pump 하나에 몰아 반영하면 첫 순환 병합이 지운 단위의 남은 후보는 건너뛰고 셋이 한 단위가
// 된다.
TEST(AssetManager_BatchedEdgesMergeOnce) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    rig.scene.Add(3);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    {
        LoadTicket one = rig.Request(1);
        LoadTicket two = rig.Request(2);
        LoadTicket three = rig.Request(3);
        REQUIRE(rig.Settle());
        REQUIRE(rig.SetHard(1, 2));
        REQUIRE(rig.SetHard(2, 1));
        REQUIRE(rig.AddHardListEntry(1, 3));
        REQUIRE(rig.SetHard(3, 1));
        REQUIRE(rig.Pump());
        CHECK(rig.UnitCount() == 1);
        const auto merged = rig.UnitOf(3);
        CHECK(merged.assets == 3 && merged.info.inboundEdges == 0 && merged.info.tickets == 3);
    }

    REQUIRE(rig.Pump());
    CHECK(rig.UnitCount() == 0);
}

// Discard 예산(11.7절 3·4): 순환 단위 {1, 2}를 놓고 maxDiscards 1로 Pump하면 첫 패키지만 Discard해
// Discarding으로 남고(로더에서는 둘 다 퇴출, RootRef는 아직 쥔다 — releasedPackages 0), 다음 Pump가
// 나머지를 Discard하고 RootRef를 놓아(releasedPackages 2) 단위가 끝난다.
TEST(AssetManager_DiscardBudgetStopsMidUnit) {
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
    }

    AssetManagerPumpBudget one;
    one.maxDiscards = 1;
    auto pumped = rig.manager->Pump(one);
    REQUIRE(pumped);
    CHECK(pumped.Value().discardedPackages == 1);
    // RootRef는 단위 전부를 Discard한 뒤에 놓는다 — 아직 0이다.
    CHECK(pumped.Value().releasedPackages == 0);
    REQUIRE(rig.UnitOf(1).found);
    CHECK(rig.UnitOf(1).info.state == ResidentUnitState::Discarding);
    CHECK(!rig.IsLoaded(1) && !rig.IsLoaded(2));
    pumped = rig.manager->Pump(one);
    REQUIRE(pumped);
    CHECK(pumped.Value().discardedPackages == 1);
    CHECK(pumped.Value().releasedPackages == 2);
    CHECK(rig.UnitCount() == 0);
}

// 함께 PostLoad 중인 두 단위(11.7절 2·3): PostLoad 예산 1로 순환 단위 A {1, 2, 3}과 B {6, 7}을 같은
// Pump에 공개한 뒤 1 ↔ 6 간선을 만들면, 먼저 넘어오는 단위는 아직 상주가 아닌 상대 패키지로 가는
// 간선과 그에서 오는 간선을 보고(보유로 세지만 순환 후보는 아니다), 나중 단위가 넘어올 때 둘을
// 합친다. 그 사이 상주한 단위 사이의 순환 탐색은 상주가 아닌 패키지로 가는 간선과 그에서 오는
// 간선을 건너뛴다.
TEST(AssetManager_ConcurrentPostLoadUnitsMergeOnIngest) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    const uint64 ids[] = {1, 2, 3, 6, 7, 8, 9};
    for (uint64 id : ids) {
        rig.scene.Add(id);
    }

    rig.scene.Hard(1, 2);
    rig.scene.Hard(2, 1);
    rig.scene.Hard(2, 3);
    rig.scene.Hard(3, 2);
    rig.scene.Hard(6, 7);
    rig.scene.Hard(7, 6);
    REQUIRE(rig.Cook());
    RecordingPostLoad postLoad(env.allocator);
    rig.config.loader.postLoad = &postLoad;
    REQUIRE(rig.Start());
    LoadTicket eight = rig.Request(8);
    LoadTicket nine = rig.Request(9);
    REQUIRE(rig.Settle());
    postLoad.order.Clear();

    AssetManagerPumpBudget slow;
    slow.loader.maxPostLoads = 1;
    LoadTicket first = rig.Request(1);
    LoadTicket second = rig.Request(6);
    bool committed = false;
    constexpr uint32 PumpLimit = 64;
    for (uint32 round = 0; round < PumpLimit && !committed; ++round) {
        rig.io.Complete();
        rig.worker.RunAll();
        REQUIRE(rig.Pump(slow));
        committed = rig.IsLoaded(1) && rig.IsLoaded(6);
    }

    REQUIRE(committed);
    REQUIRE(postLoad.order.Size() == 1);
    REQUIRE(rig.UnitCount() == 2);
    // 상주한 8 ↔ 9 순환 탐색이 상주가 아닌 1로 가는 간선(8 → 1)과 1에서 오는 간선(1 → 9)을 지난다.
    REQUIRE(rig.SetHard(1, 6));
    REQUIRE(rig.SetHard(6, 1));
    REQUIRE(rig.AddHardListEntry(1, 9));
    REQUIRE(rig.SetHard(8, 9));
    REQUIRE(rig.AddHardListEntry(8, 1));
    REQUIRE(rig.SetHard(9, 8));
    REQUIRE(rig.Pump(slow));
    CHECK(rig.UnitCount() == 1);
    CHECK(rig.UnitOf(8).assets == 2 && rig.UnitOf(8).info.inboundEdges == 1);

    bool ingested = false;
    for (uint32 round = 0; round < PumpLimit && !ingested; ++round) {
        REQUIRE(rig.Pump(slow));
        ingested = rig.UnitOf(1).found && rig.UnitOf(6).found;
    }

    REQUIRE(ingested);
    // 1 → 9, 8 → 1이 A와 {8, 9}를 잇고 1 ↔ 6이 A와 B를 이어 모두 한 단위다.
    CHECK(rig.UnitCount() == 1);
    const auto merged = rig.UnitOf(7);
    CHECK(merged.assets == 7 && merged.info.inboundEdges == 0 && merged.info.tickets == 4);
    first = LoadTicket();
    second = LoadTicket();
    eight = LoadTicket();
    nine = LoadTicket();
    REQUIRE(rig.Pump());
    CHECK(rig.UnitCount() == 0);
}

// 한 묶음 안 같은 쌍의 여러 항목(11.7절 3): 관찰자 키는 원인·트랜잭션까지 나누므로 "1의 대표 에셋
// X → 2 설정(Edit +1), 1 안 새 객체 Y → 2 Commit(Commit +1), X → 2 지움, Y → 2 지움"은 한 묶음에
// [Edit −1, Commit +1]로 온다. 쌍별로 합쳐 판정하므로 다중도는 0이고(Fatal 없음) 보유가 바뀌지
// 않는다. 변형: 0인 쌍만(묶음 끝에서 뺀다), 0인 쌍 뒤에 다른 쌍 2 → 1(가운데서 뺀다), Y → 2를
// 남김(순 변화 +1이 보유 — 2 → 1과 순환이라 한 단위).
enum class BatchShape : uint8 {
    ZeroOnly,
    ZeroThenOther,
    KeepCommitted
};

TEST(AssetManager_EdgeBatchCoalescesPerPair) {
    AssetEnv env;
    REQUIRE(env.Ready());
    const BatchShape shapes[] = {BatchShape::ZeroOnly, BatchShape::ZeroThenOther,
                                 BatchShape::KeepCommitted};
    for (const BatchShape shape : shapes) {
        const bool keepCommitted = shape == BatchShape::KeepCommitted;
        ManagerRig rig(env);
        rig.scene.Add(1);
        rig.scene.Add(2);
        REQUIRE(rig.Cook());
        REQUIRE(rig.Start());
        LoadTicket one = rig.Request(1);
        LoadTicket two = rig.Request(2);
        REQUIRE(rig.Settle());
        ObjectRegistry& registry = rig.manager->Registry();
        const ObjectAccessContext access = registry.OwnerAccess();
        const ObjectHandle target = rig.AssetObject(2);

        REQUIRE(rig.SetHard(1, 2));
        ConstructionContext transaction(registry, access);
        auto made = transaction.Create<TestAsset>(TestAssetType(), rig.AssetObject(1), "Y");
        REQUIRE(made);
        const ObjectHandle created = made.Value();
        auto staged = transaction.GetForWrite<TestAsset>(created);
        REQUIRE(staged);
        REQUIRE(transaction.SetReference(created, staged.Value()->hard, target));
        auto committed = transaction.Commit();
        REQUIRE(committed);
        REQUIRE(rig.SetHard(1, 0));
        if (!keepCommitted) {
            auto borrow = registry.ResolveForWrite<TestAsset>(access, created);
            REQUIRE(borrow);
            REQUIRE(registry.SetReference(access, created, borrow.Value().Get().Value()->hard,
                                          ObjectHandle{}));
        }

        // 같은 묶음의 다른 쌍(2 → 1): 패키지 칸 순서상 합이 0인 쌍(1 → 2) 뒤에 온다 — 합친 뒤 0인
        // 쌍을 빼고 다음 쌍을 그 자리에 둔다.
        REQUIRE(rig.manager->Loader().FindPackage(FixedId(1)).Value().slot <
                rig.manager->Loader().FindPackage(FixedId(2)).Value().slot);
        if (shape != BatchShape::ZeroOnly) {
            REQUIRE(rig.SetHard(2, 1));
        }

        REQUIRE(rig.Pump());
        if (!keepCommitted) {
            // 1 → 2의 순 변화가 0이라 2는 보유가 없고, 2 → 1만 1의 보유다.
            const uint32 expected = shape == BatchShape::ZeroThenOther ? 1 : 0;
            CHECK(rig.UnitOf(2).info.inboundEdges == 0 &&
                  rig.UnitOf(1).info.inboundEdges == expected);
            CHECK(rig.UnitCount() == 2);
        } else {
            // 남은 Y → 2와 2 → 1이 순환이라 한 단위로 합쳐진다(내부 간선은 보유가 아니다).
            CHECK(rig.UnitCount() == 1 && rig.UnitOf(2).info.inboundEdges == 0);
        }

        committed.Value().Reset();
    }
}

} // namespace manager_graph_tests_file
