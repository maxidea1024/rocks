#include "LooseOctreeTestSupport.h"

#include <Core/Memory/SystemAllocator.h>
#include <Platform/Memory.h>
#include <TestSupport/NFailAllocator.h>

using namespace rocks;
using namespace rocks::spatial;
using namespace rocks::spatial::oracle;

// LooseOctree 구조 변경 시험이다(Docs/Design/SpatialIndex.ko.md 8절 LO06–LO10). 모든 시나리오는
// 전수 검색 대조와 Validate()로 정확성을 함께 확인한다.

namespace {

Box<double> MakeCube(const Vec3d& center, double half) noexcept {
    return Box<double>(center - Vec3d::Splat(half), center + Vec3d::Splat(half));
}

Box<double> MakePoint(const Vec3d& point) noexcept {
    return Box<double>(point, point);
}

uint32 RootDirectCount(const LooseOctree& tree) noexcept {
    SpatialReadScope read(tree);
    uint32 direct = 0;
    read.Reader().VisitNodes([&](const SpatialNodeInfo& info) noexcept {
        if (info.depth == 1) {
            direct = info.directCount;
        }
    });
    return direct;
}

// 넓은 질의 몇 개로 전수 검색과 대조한다.
bool QueriesMatch(OracleTree& tree, const LooseOctreeConfig& config, Allocator& allocator,
                  uint64 seed) noexcept {
    const double tolerance = ToleranceFor(tree.Tree(), config);
    SpatialReadScope read(tree.Tree());
    const LooseOctreeReader reader = read.Reader();
    RandomStream random(seed);
    bool ok = true;
    for (uint32 query = 0; query < 24; ++query) {
        const Box<double> box = MakeQuery(random, 60.0);
        ok = ok && CheckBoxQuery(reader, tree.Proxies(), box, 0xFFu, tolerance, allocator).ok;
    }
    return ok;
}

// 각 팔분 공간에 점 하나씩과 (10, 10, 10)의 점을 넣어 루트를 한 level 분할시킨다.
void AddOctantPoints(OracleTree& tree) noexcept {
    for (uint32 octant = 0; octant < 8; ++octant) {
        const Vec3d point((octant & 1u) != 0 ? 10.0 : -10.0, (octant & 2u) != 0 ? 10.0 : -10.0,
                          (octant & 4u) != 0 ? 10.0 : -10.0);
        (void)tree.Add(MakePoint(point + Vec3d(0.25, 0.5, 0.75)), 1);
    }
}

} // namespace

// LO06: 경계 왕복은 제자리 갱신으로 끝나고, 순간 이동·크기 확대는 재삽입하며, 축소로 자식에 들어갈
// 수 있게 된 프록시는 재배치 대기로 표시된 뒤 유지보수가 내린다.
TEST(LooseOctree_LO06_MovementPaths) {
    SystemAllocator allocator;
    const LooseOctreeConfig config = SmallConfig();
    auto created = LooseOctree::TryCreate(config, allocator);
    REQUIRE(created);
    OracleTree tree(Move(created.Value()), allocator);

    // 잎 루트 하나에서 자식 경계(루트 중심 x = 0)를 오가는 이동은 구조를 바꾸지 않는다.
    const usize mover = tree.Add(MakeCube(Vec3d(0.1, 0, 0), 0.05), 1);
    REQUIRE(mover != IndexNone);
    for (uint32 step = 0; step < 100; ++step) {
        const double x = step % 2 == 0 ? -0.2 : 0.2;
        REQUIRE(tree.Update(mover, MakeCube(Vec3d(x, 0.3, -0.1), 0.05)));
    }
    OctreeStats stats = StatsOf(tree.Tree());
    CHECK(stats.inPlaceUpdates == 100 && stats.reinserts == 0);

    // 순간 이동: 루트 확장 경계 밖으로 가면 루트를 키워 다시 넣는다.
    REQUIRE(tree.Update(mover, MakeCube(Vec3d(500, -300, 20), 0.05)));
    stats = StatsOf(tree.Tree());
    CHECK(stats.reinserts == 1 && stats.rootExpansions >= 1);
    REQUIRE(tree.Update(mover, MakeCube(Vec3d(0.5, 0.5, 0.5), 0.05)));
    CHECK(ValidateTree(tree.Tree()));
    CHECK(QueriesMatch(tree, config, allocator, 0x601u));

    // 루트를 한 level 분할시킨 뒤, (10, 10, 10)의 작은 프록시를 키우면 자식 확장 경계를 벗어나
    // 루트로 재삽입되고, 다시 줄이면 루트에서 제자리 갱신되며 재배치 대기로 표시된다.
    auto fresh = LooseOctree::TryCreate(config, allocator);
    REQUIRE(fresh);
    OracleTree split(Move(fresh.Value()), allocator);
    AddOctantPoints(split);
    const usize probe = split.Add(MakeCube(Vec3d(10, 10, 10), 0.1), 1);
    REQUIRE(probe != IndexNone);
    stats = StatsOf(split.Tree());
    REQUIRE(stats.splits >= 1);
    CHECK(RootDirectCount(split.Tree()) == 0);

    REQUIRE(split.Update(probe, MakeCube(Vec3d(10, 10, 10), 40)));
    stats = StatsOf(split.Tree());
    CHECK(stats.reinserts == 1 && RootDirectCount(split.Tree()) == 1);

    REQUIRE(split.Update(probe, MakeCube(Vec3d(10, 10, 10), 0.1)));
    stats = StatsOf(split.Tree());
    CHECK(stats.reinserts == 1 && stats.inPlaceUpdates == 1);
    CHECK(stats.pendingRelocations == 1 && RootDirectCount(split.Tree()) == 1);
    CHECK(ValidateTree(split.Tree()));
    CHECK(QueriesMatch(split, config, allocator, 0x602u));

    const MaintenanceReport report = split.Tree().RunMaintenance(MaintenanceBudget::All());
    CHECK(report.relocatedProxies == 1 && report.remaining == 0);
    CHECK(RootDirectCount(split.Tree()) == 0);
    CHECK(ValidateTree(split.Tree()));
    CHECK(QueriesMatch(split, config, allocator, 0x603u));
}

// LO07: 루트 확장(8방향), 확장 전후 기존 노드 경계의 비트 동일, 수용 판정 실패 시 루트 불변과 범위
// 밖 목록, 범위 밖에서의 복귀, 축소 이력(4회 연속), 빈 트리 재설정과 재고정.
TEST(LooseOctree_LO07_RootExpansionAndShrink) {
    SystemAllocator allocator;
    const LooseOctreeConfig config = SmallConfig();
    for (uint32 direction = 0; direction < 8; ++direction) {
        auto created = LooseOctree::TryCreate(config, allocator);
        REQUIRE(created);
        OracleTree tree(Move(created.Value()), allocator);
        RandomStream random(0x700u + direction);
        for (uint32 index = 0; index < 40; ++index) {
            REQUIRE(tree.Add(MakeCube(Vec3d(Uniform(random, -30, 30), Uniform(random, -30, 30),
                                            Uniform(random, -30, 30)),
                                      Uniform(random, 0, 0.5)),
                             1) != IndexNone);
        }
        // 확장 전 노드 경계(셀·확장 경계·level)를 기록한다.
        Array<SpatialNodeInfo> before(allocator);
        {
            SpatialReadScope read(tree.Tree());
            read.Reader().VisitNodes(
                [&](const SpatialNodeInfo& info) noexcept { (void)before.TryPushBack(info); });
        }
        const uint32 levelBefore = StatsOf(tree.Tree()).rootLevel;
        const Vec3d far((direction & 1u) != 0 ? 150.0 : -150.0,
                        (direction & 2u) != 0 ? 150.0 : -150.0,
                        (direction & 4u) != 0 ? 150.0 : -150.0);
        REQUIRE(tree.Add(MakeCube(far, 1), 1) != IndexNone);
        const OctreeStats stats = StatsOf(tree.Tree());
        CHECK(stats.rootLevel > levelBefore && stats.outOfRangeCount == 0);
        CHECK(ValidateTree(tree.Tree()));
        CHECK(QueriesMatch(tree, config, allocator, 0x710u + direction));

        // 기존 노드의 월드 경계는 비트 단위로 같아야 한다(노드 번호는 바뀔 수 있다).
        SpatialReadScope read(tree.Tree());
        uint32 matched = 0;
        for (const SpatialNodeInfo& old : before) {
            bool found = false;
            read.Reader().VisitNodes([&](const SpatialNodeInfo& info) noexcept {
                found =
                    found || (info.level == old.level &&
                              platform::MemoryEquals(&info.cell, &old.cell, sizeof(Box<double>)) &&
                              platform::MemoryEquals(&info.loose, &old.loose, sizeof(Box<double>)));
            });
            matched += found ? 1u : 0u;
        }
        CHECK(matched == before.Size());
    }

    // 수용 판정: 확장 한도 밖이면 루트를 바꾸지 않고 범위 밖 목록에 넣는다. 갱신으로 돌아오면
    // 트리에 넣는다. 원점 근처 프록시를 병합 임계보다 많이(10개) 두어 루트 축소가 병합보다 먼저
    // 막히지 않게 한다.
    LooseOctreeConfig limited = config;
    limited.maxRootLevel = 8;
    auto created = LooseOctree::TryCreate(limited, allocator);
    REQUIRE(created);
    OracleTree tree(Move(created.Value()), allocator);
    AddOctantPoints(tree);
    REQUIRE(tree.Add(MakeCube(Vec3d(1, 2, 3), 1), 1) != IndexNone);
    REQUIRE(tree.Add(MakeCube(Vec3d(-3, 2, -1), 1), 1) != IndexNone);
    const uint32 levelBefore = StatsOf(tree.Tree()).rootLevel;
    const usize outside = tree.Add(MakeCube(Vec3d(1.0e6, 0, 0), 1), 1);
    REQUIRE(outside != IndexNone);
    OctreeStats stats = StatsOf(tree.Tree());
    CHECK(stats.rootLevel == levelBefore && stats.outOfRangeCount == 1 &&
          stats.rootExpansions == 0);
    // 범위 밖에서 범위 밖으로의 갱신은 목록 안에서 끝난다.
    REQUIRE(tree.Update(outside, MakeCube(Vec3d(-1.0e6, 0, 0), 2)));
    CHECK(StatsOf(tree.Tree()).outOfRangeCount == 1);
    REQUIRE(tree.Update(outside, MakeCube(Vec3d(100, 0, 0), 1)));
    stats = StatsOf(tree.Tree());
    CHECK(stats.outOfRangeCount == 0 && stats.rootLevel > levelBefore);
    CHECK(ValidateTree(tree.Tree()));
    CHECK(QueriesMatch(tree, limited, allocator, 0x720u));

    // 축소 이력: 먼 프록시를 지우면 루트 축소 조건(직접 저장 0, 점유 자식 하나)이 서지만 유지보수
    // 4회 연속 관측 뒤에야 한 단계 줄인다.
    REQUIRE(tree.Remove(outside));
    const uint32 grownLevel = StatsOf(tree.Tree()).rootLevel;
    for (uint32 run = 0; run < 3; ++run) {
        const MaintenanceReport report = tree.Tree().RunMaintenance(MaintenanceBudget::All());
        CHECK(report.rootShrinks == 0);
    }
    CHECK(StatsOf(tree.Tree()).rootLevel == grownLevel);
    const MaintenanceReport fourth = tree.Tree().RunMaintenance(MaintenanceBudget::All());
    CHECK(fourth.rootShrinks == 1);
    CHECK(StatsOf(tree.Tree()).rootLevel == grownLevel - 1);
    CHECK(ValidateTree(tree.Tree()));
    // 예산 0인 유지보수는 루트 확인도 하지 않는다(관측 수가 늘지 않는다).
    for (uint32 run = 0; run < 8; ++run) {
        CHECK(tree.Tree().RunMaintenance(MaintenanceBudget{}).rootShrinks == 0);
    }
    CHECK(StatsOf(tree.Tree()).rootLevel == grownLevel - 1);
    for (uint32 run = 0; run < 64; ++run) {
        (void)tree.Tree().RunMaintenance(MaintenanceBudget::All());
    }
    // 축소는 초기 level에서 멈춘다.
    CHECK(StatsOf(tree.Tree()).rootLevel == limited.initialRootLevel);
    CHECK(ValidateTree(tree.Tree()));
    CHECK(QueriesMatch(tree, limited, allocator, 0x721u));

    // 빈 트리 재설정과 재고정: 모두 지우면 유지보수가 루트를 초기 상태로 되돌리고, 다음 삽입은 그
    // 중심을 포함하는 초기 level 격자 셀로 루트를 다시 잡는다(먼 곳이어도 다단 확장을 하지 않는다).
    REQUIRE(tree.Update(0, MakeCube(Vec3d(200, 0, 0), 1)));
    for (usize index = 0; index < tree.Proxies().Size(); ++index) {
        if (tree.Proxies()[index].live) {
            REQUIRE(tree.Remove(index));
        }
    }
    const MaintenanceReport emptied = tree.Tree().RunMaintenance(MaintenanceBudget::Operations(1));
    CHECK(emptied.emptyReset);
    stats = StatsOf(tree.Tree());
    CHECK(stats.rootLevel == limited.initialRootLevel && stats.nodeCount == 1);
    const uint64 expansions = stats.rootExpansions;
    REQUIRE(tree.Add(MakeCube(Vec3d(300, -200, 100), 1), 1) != IndexNone);
    stats = StatsOf(tree.Tree());
    CHECK(stats.rootLevel == limited.initialRootLevel && stats.rootExpansions == expansions);
    CHECK(stats.outOfRangeCount == 0);
    CHECK(ValidateTree(tree.Tree()));
    CHECK(QueriesMatch(tree, limited, allocator, 0x722u));
}

// LO08: 분리 가능한 군집은 실제 분리 level까지 체인으로 한 번에 분할하고, 같은 AABB 집합은 보류하며
// 재평가 간격을 두 배로 늘린다. 임계 부근의 삽입·삭제 반복은 분할·병합 횟수를 변경 수에 비례해
// 묶는다.
TEST(LooseOctree_LO08_SplitLevelsHoldAndHysteresis) {
    SystemAllocator allocator;
    const LooseOctreeConfig config = SmallConfig();

    // 루트 level 6 셀 [−32, 32]. 정수 셀 좌표(원점 −32 기준)로 x = 42와 45는 XOR 0b111이라 ℓ_sep =
    // 2, 목표 T = level 3이다. 체인 level 5·4와 T의 자식 블록까지 블록 4개(노드 33개), 최대 깊이
    // 5가 된다.
    {
        auto created = LooseOctree::TryCreate(config, allocator);
        REQUIRE(created);
        OracleTree tree(Move(created.Value()), allocator);
        for (uint32 index = 0; index < 9; ++index) {
            const double x = index < 5 ? 10.5 : 13.5;
            REQUIRE(tree.Add(MakePoint(Vec3d(x, 10.5, 10.5)), 1) != IndexNone);
        }
        const OctreeStats stats = StatsOf(tree.Tree());
        CHECK(stats.splits == 1 && stats.chainSplits == 1 && stats.holds == 0);
        CHECK(stats.nodeCount == 33 && stats.maxDepth == 5 && stats.interiorDirectCount == 0);
        CHECK(ValidateTree(tree.Tree()));
        CHECK(QueriesMatch(tree, config, allocator, 0x801u));
    }

    // 같은 AABB 집합: 9번째 삽입에서 보류(n0 = 9, 간격 4). 13·21·37번째에 재평가하고 매번 간격이 두
    // 배가 된다(8, 16, 32).
    {
        auto created = LooseOctree::TryCreate(config, allocator);
        REQUIRE(created);
        OracleTree tree(Move(created.Value()), allocator);
        const Box<double> same = MakeCube(Vec3d(3, 3, 3), 0.5);
        for (uint32 index = 0; index < 40; ++index) {
            REQUIRE(tree.Add(same, 1) != IndexNone);
        }
        OctreeStats stats = StatsOf(tree.Tree());
        CHECK(stats.splitEvaluations == 4 && stats.holds == 4 && stats.reevaluations == 3);
        CHECK(stats.heldNodeCount == 1 && stats.splits == 0);
        CHECK(ValidateTree(tree.Tree()));
        // 직접 저장 수가 splitThreshold 이하로 줄면 보류를 해제한다.
        RandomStream random(0x802u);
        for (uint32 index = 0; index < 32; ++index) {
            REQUIRE(tree.Remove(tree.PickLive(random)));
        }
        stats = StatsOf(tree.Tree());
        CHECK(stats.heldNodeCount == 0);
        CHECK(ValidateTree(tree.Tree()));
    }

    // 임계 부근 반복: 하나씩 넣고 빼는 것은 병합 임계(4)에 닿지 않으므로 분할 1번으로 끝난다. 5개씩
    // 빼고(병합) 넣기(분할)를 반복하면 한 주기에 변경 10번마다 구조 변경 2번이다.
    {
        auto created = LooseOctree::TryCreate(config, allocator);
        REQUIRE(created);
        OracleTree tree(Move(created.Value()), allocator);
        AddOctantPoints(tree);
        REQUIRE(tree.Add(MakePoint(Vec3d(-10, 10, -10)), 1) != IndexNone);
        for (uint32 step = 0; step < 100; ++step) {
            const usize extra = tree.Add(MakePoint(Vec3d(10.5, -10.5, 10.5)), 1);
            REQUIRE(extra != IndexNone);
            REQUIRE(tree.Remove(extra));
            (void)tree.Tree().RunMaintenance(MaintenanceBudget::All());
        }
        OctreeStats stats = StatsOf(tree.Tree());
        CHECK(stats.splits == 1 && stats.merges == 0);

        RandomStream random(0x803u);
        uint32 changes = 0;
        constexpr uint32 Cycles = 20;
        for (uint32 cycle = 0; cycle < Cycles; ++cycle) {
            usize removed[5];
            for (usize& index : removed) {
                index = tree.PickLive(random);
                REQUIRE(tree.Remove(index));
                ++changes;
            }
            (void)tree.Tree().RunMaintenance(MaintenanceBudget::All());
            for (const usize index : removed) {
                REQUIRE(tree.Add(tree.Proxies()[index].bounds, 1) != IndexNone);
                ++changes;
            }
            (void)tree.Tree().RunMaintenance(MaintenanceBudget::All());
        }
        stats = StatsOf(tree.Tree());
        CHECK(stats.merges >= 1 && stats.merges <= Cycles && stats.splits <= Cycles + 1);
        // 구조 변경 한 번에 최소 splitThreshold / 2번의 변경이 필요하다(6.6절 진동 방지).
        CHECK((stats.splits - 1 + stats.merges) * (config.splitThreshold / 2) <= changes);
        CHECK(ValidateTree(tree.Tree()));
        CHECK(QueriesMatch(tree, config, allocator, 0x804u));
    }
}

// LO09: 유지보수. 예산 0으로 미뤄도 검색이 정확하고, 대기량은 한도를 넘지 않으며(강제 처리), 병합은
// 아래에서 위로 진행하고, 블록 해제는 대기 항목을 남기지 않는다. 전체 삭제 뒤 재삽입도 정확하다.
TEST(LooseOctree_LO09_DeferredMaintenance) {
    SystemAllocator allocator;
    LooseOctreeConfig config = SmallConfig();
    config.pendingLimitFloor = 8;
    auto created = LooseOctree::TryCreate(config, allocator);
    REQUIRE(created);
    OracleTree tree(Move(created.Value()), allocator);
    RandomStream random(0x900u);
    for (uint32 index = 0; index < 600; ++index) {
        REQUIRE(tree.Add(MakeBox(Distribution::Uniform, random, 60.0), 1) != IndexNone);
    }
    const OctreeStats grown = StatsOf(tree.Tree());
    REQUIRE(grown.splits > 10);

    bool boundHeld = true;
    uint32 maxPending = 0;
    for (uint32 index = 0; index < 560; ++index) {
        REQUIRE(tree.Remove(tree.PickLive(random)));
        const MaintenanceReport report = tree.Tree().RunMaintenance(MaintenanceBudget{});
        const OctreeStats stats = StatsOf(tree.Tree());
        const uint32 limit = stats.nodeCount / 4 > config.pendingLimitFloor
                                 ? stats.nodeCount / 4
                                 : config.pendingLimitFloor;
        boundHeld = boundHeld && stats.pendingTotal <= limit && report.processed == 0 &&
                    report.remaining == stats.pendingTotal;
        maxPending = stats.pendingTotal > maxPending ? stats.pendingTotal : maxPending;
        if (index % 80 == 0) {
            CHECK(QueriesMatch(tree, config, allocator, 0x901u + index));
            CHECK(ValidateTree(tree.Tree()));
        }
    }
    CHECK(boundHeld);
    CHECK(maxPending > 0);
    OctreeStats stats = StatsOf(tree.Tree());
    CHECK(QueriesMatch(tree, config, allocator, 0x902u));

    // 한 항목씩 처리해도(블록 해제가 대기 중인 자식 항목을 지운다) 불변 조건 9가 성립한다.
    for (uint32 run = 0; run < 40; ++run) {
        (void)tree.Tree().RunMaintenance(MaintenanceBudget::Operations(1));
        OctreeValidationReport report;
        CHECK(ValidateTree(tree.Tree(), &report));
        CHECK(report.invariant == OctreeInvariantNone);
    }
    const MaintenanceReport drained = tree.Tree().RunMaintenance(MaintenanceBudget::All());
    CHECK(drained.remaining == 0);
    stats = StatsOf(tree.Tree());
    CHECK(stats.merges + stats.reclaims > 0);
    // 다 처리한 뒤에는 서브트리가 빈 내부 노드가 없고(회수) 노드 수가 크게 줄었다(상향식 병합).
    {
        SpatialReadScope read(tree.Tree());
        bool emptyInterior = false;
        read.Reader().VisitNodes([&](const SpatialNodeInfo& info) noexcept {
            emptyInterior = emptyInterior || (!info.leaf && info.subtreeCount == 0);
        });
        CHECK(!emptyInterior);
        CHECK(read.Reader().Stats().nodeCount * 2 < grown.nodeCount);
    }
    CHECK(ValidateTree(tree.Tree()));
    CHECK(QueriesMatch(tree, config, allocator, 0x903u));

    // 시간 예산과 축소 요청.
    const MaintenanceReport timed = tree.Tree().RunMaintenance(MaintenanceBudget::Time(1000000));
    CHECK(timed.remaining == 0);
    MaintenanceBudget shrink = MaintenanceBudget::Operations(1);
    shrink.shrink = true;
    CHECK(tree.Tree().RunMaintenance(shrink).shrunk);
    CHECK(ValidateTree(tree.Tree()));

    // 전체 삭제 뒤 재삽입.
    for (usize index = 0; index < tree.Proxies().Size(); ++index) {
        if (tree.Proxies()[index].live) {
            REQUIRE(tree.Remove(index));
        }
    }
    const MaintenanceReport emptied = tree.Tree().RunMaintenance(MaintenanceBudget::All());
    CHECK(emptied.emptyReset);
    CHECK(StatsOf(tree.Tree()).nodeCount == 1);
    RandomStream again(0x900u);
    for (uint32 index = 0; index < 300; ++index) {
        REQUIRE(tree.Add(MakeBox(Distribution::Uniform, again, 60.0), 1) != IndexNone);
    }
    CHECK(ValidateTree(tree.Tree()));
    CHECK(QueriesMatch(tree, config, allocator, 0x904u));
}

// LO09: 누적 방지. 보류된 잎 8개가 모두 재평가 대기가 되면 대기량(8)이 한도 max(1, 노드 수 / 4 =
// 2)를 넘으므로 변경 호출이 그 자리에서 초과분을 처리한다.
TEST(LooseOctree_LO09_PendingLimitForcesProcessing) {
    SystemAllocator allocator;
    LooseOctreeConfig config = SmallConfig();
    config.pendingLimitFloor = 1;
    auto created = LooseOctree::TryCreate(config, allocator);
    REQUIRE(created);
    OracleTree tree(Move(created.Value()), allocator);
    // 팔분 공간마다 같은 box 20개: 루트는 한 level 분할되고 자식 8개는 분리할 수 없어 보류된다(n0 =
    // 13, 간격 8, 마지막 재평가 뒤 변경 7번).
    usize firsts[8] = {};
    for (uint32 round = 0; round < 20; ++round) {
        for (uint32 octant = 0; octant < 8; ++octant) {
            const Vec3d center((octant & 1u) != 0 ? 10.0 : -10.0, (octant & 2u) != 0 ? 10.0 : -10.0,
                               (octant & 4u) != 0 ? 10.0 : -10.0);
            const usize index = tree.Add(MakeCube(center, 0.5), 1);
            REQUIRE(index != IndexNone);
            if (round == 0) {
                firsts[octant] = index;
            }
        }
    }
    OctreeStats stats = StatsOf(tree.Tree());
    REQUIRE(stats.nodeCount == 9 && stats.heldNodeCount == 8 && stats.pendingTotal == 0);
    // 잎마다 하나씩 지우면 변경 수가 간격(8)에 닿아 재평가 대기가 된다.
    for (const usize index : firsts) {
        REQUIRE(tree.Remove(index));
        stats = StatsOf(tree.Tree());
        CHECK(stats.pendingTotal <= 2);
    }
    CHECK(stats.forcedMaintenanceRuns > 0 && stats.forcedMaintenanceItems > 0);
    CHECK(stats.reevaluations > 0);
    CHECK(ValidateTree(tree.Tree()));
    CHECK(QueriesMatch(tree, config, allocator, 0x905u));
}

// LO10: 저장. 풀 확장과 슬롯 재사용, maxProxyGeneration을 낮춘 세대 retire, N번째 할당 실패 주입.
// 실패한 변경은 OutOfMemory이고 논리 상태를 바꾸지 않으며 불변 조건이 성립한다.
TEST(LooseOctree_LO10_StorageReuseRetireAndFailureInjection) {
    SystemAllocator system;
    {
        // 슬롯 재사용과 retire: 세대 상한 2이면 slot 하나를 두 번 쓰고 폐기한다.
        LooseOctreeConfig config = SmallConfig();
        config.maxProxyGeneration = 2;
        auto created = LooseOctree::TryCreate(config, system);
        REQUIRE(created);
        LooseOctree octree = Move(created.Value());
        SpatialProxyDesc desc;
        desc.bounds = MakeCube(Vec3d(1, 1, 1), 1);
        desc.filterMask = 1;
        auto first = octree.TryAdd(desc);
        REQUIRE(first);
        REQUIRE(octree.TryRemove(first.Value()));
        auto second = octree.TryAdd(desc);
        REQUIRE(second);
        CHECK(second.Value().slot.index == first.Value().slot.index &&
              second.Value().slot.generation == 2);
        REQUIRE(octree.TryRemove(second.Value()));
        auto third = octree.TryAdd(desc);
        REQUIRE(third);
        CHECK(third.Value().slot.index != first.Value().slot.index);
        CHECK(octree.TryRemove(first.Value()).Error() == ErrorCode::StaleHandle);
        CHECK(octree.TryRemove(second.Value()).Error() == ErrorCode::StaleHandle);
        CHECK(ValidateTree(octree));

        // 풀 확장 뒤 전부 지우고 다시 넣으면 slot·청크·블록을 재사용한다.
        LooseOctreeConfig reuseConfig = SmallConfig();
        auto reuseCreated = LooseOctree::TryCreate(reuseConfig, system);
        REQUIRE(reuseCreated);
        OracleTree tree(Move(reuseCreated.Value()), system);
        RandomStream random(0xA00u);
        for (uint32 index = 0; index < 800; ++index) {
            REQUIRE(tree.Add(MakeBox(Distribution::Clustered, random, 80.0), 1) != IndexNone);
        }
        const OctreeStats full = StatsOf(tree.Tree());
        for (usize index = 0; index < tree.Proxies().Size(); ++index) {
            REQUIRE(tree.Remove(index));
        }
        (void)tree.Tree().RunMaintenance(MaintenanceBudget::All());
        RandomStream again(0xA00u);
        for (uint32 index = 0; index < 800; ++index) {
            const usize added = tree.Add(MakeBox(Distribution::Clustered, again, 80.0), 1);
            REQUIRE(added != IndexNone);
            CHECK(tree.Proxies()[added].handle.slot.index < 800);
        }
        // 레코드 저장소는 늘지 않는다(slot 재사용). 루트 재고정 위치가 처음과 다를 수 있으므로
        // 구조는 비교하지 않는다.
        const OctreeStats refilled = StatsOf(tree.Tree());
        CHECK(refilled.recordBytesReserved == full.recordBytesReserved);
        CHECK(refilled.proxyCount == 800);
        CHECK(ValidateTree(tree.Tree()));
    }

    // N번째 할당 실패: 변경마다 실패 지점을 1부터 늘려 가며, 실패하면 논리 상태 불변·불변 조건
    // 성립을 확인하고 같은 변경을 다시 시도한다. 분할의 확보 실패는 오류가 아니라 보류다.
    test::NFailAllocator allocator(system);
    const LooseOctreeConfig config = SmallConfig();
    auto created = LooseOctree::TryCreate(config, allocator);
    REQUIRE(created);
    OracleTree tree(Move(created.Value()), system);
    RandomStream random(0xA01u);
    uint32 injected = 0;
    bool consistent = true;
    for (uint32 operation = 0; operation < 260; ++operation) {
        const bool update = operation >= 160 && tree.LiveCount() != 0;
        const usize target = update ? tree.PickLive(random) : IndexNone;
        const Distribution distribution =
            operation % 3 == 0 ? Distribution::WideSparse : Distribution::Uniform;
        const Box<double> bounds = MakeBox(distribution, random, 100.0);
        for (uint64 attempt = 1;; ++attempt) {
            allocator.FailOnAttempt(attempt);
            bool succeeded = false;
            ErrorCode error = ErrorCode::None;
            if (update) {
                auto updated = tree.Tree().TryUpdateBounds(tree.Proxies()[target].handle, bounds);
                succeeded = static_cast<bool>(updated);
                error = updated.Error();
            } else {
                SpatialProxyDesc desc;
                desc.bounds = bounds;
                desc.filterMask = 1;
                desc.userValue = tree.Proxies().Size();
                auto added = tree.Tree().TryAdd(desc);
                succeeded = static_cast<bool>(added);
                error = added.Error();
                if (succeeded) {
                    allocator.DisableFailure();
                    // 모형에도 같은 프록시를 넣는다(옥트리에는 이미 들어갔다).
                    REQUIRE(tree.Record(added.Value(), bounds, 1) != IndexNone);
                }
            }
            const uint64 attempts = allocator.Attempts();
            allocator.DisableFailure();
            if (succeeded) {
                if (update) {
                    tree.RecordBounds(target, bounds);
                }
                break;
            }
            ++injected;
            consistent = consistent && error == ErrorCode::OutOfMemory && attempts >= attempt &&
                         LogicalStateMatches(tree.Tree(), tree.Proxies()) &&
                         ValidateTree(tree.Tree());
            REQUIRE(attempt < 64);
        }
        if (operation % 20 == 0) {
            allocator.FailOnAttempt(1 + operation % 3);
            (void)tree.Tree().RunMaintenance(MaintenanceBudget::All());
            allocator.DisableFailure();
            consistent = consistent && ValidateTree(tree.Tree());
        }
    }
    CHECK(consistent);
    CHECK(injected > 10);
    CHECK(LogicalStateMatches(tree.Tree(), tree.Proxies()));
    CHECK(QueriesMatch(tree, config, system, 0xA02u));
}

// 6.4절: 경계 갱신(재삽입)은 분할을 평가하지 않고 재평가 대기만 표시한다. 대기는 유지보수(변형 0)나
// 다음 삽입(변형 1)이 처리한다.
TEST(LooseOctree_LO08_UpdatesDeferSplitEvaluation) {
    SystemAllocator allocator;
    const LooseOctreeConfig config = SmallConfig();
    for (uint32 variant = 0; variant < 2; ++variant) {
        auto created = LooseOctree::TryCreate(config, allocator);
        REQUIRE(created);
        OracleTree tree(Move(created.Value()), allocator);
        AddOctantPoints(tree);
        REQUIRE(tree.Add(MakePoint(Vec3d(-10, 10, -10)), 1) != IndexNone);
        REQUIRE(StatsOf(tree.Tree()).splits == 1);
        // 이동할 프록시 8개를 팔분 공간 1–7에 흩어 둔다(어느 자식도 임계를 넘지 않는다).
        usize movers[8];
        for (uint32 index = 0; index < 8; ++index) {
            const uint32 octant = 1 + index % 7;
            const double offset = 0.3 * index;
            const Vec3d point((octant & 1u) != 0 ? 20.0 + offset : -20.0 - offset,
                              (octant & 2u) != 0 ? 20.0 + offset : -20.0 - offset,
                              (octant & 4u) != 0 ? 20.0 + offset : -20.0 - offset);
            movers[index] = tree.Add(MakePoint(point), 1);
            REQUIRE(movers[index] != IndexNone);
        }
        REQUIRE(StatsOf(tree.Tree()).splits == 1);
        // 모두 자식 0(셀 [−32, 0]^3)의 축마다 −16보다 작은 곳으로 옮긴다. 다른 자식의 확장 경계(k =
        // 2라 이웃 셀 반 변만큼 넓다)를 벗어나므로 제자리 갱신이 아니라 재삽입이고, 자식 0의 직접
        // 저장 수가 9가 되어 임계(8)를 넘는다.
        for (uint32 index = 0; index < 8; ++index) {
            const Vec3d point(-17.5 - 1.5 * index, -18.0 - 1.2 * index, -30.0 + 1.5 * index);
            REQUIRE(tree.Update(movers[index], MakePoint(point)));
        }
        OctreeStats stats = StatsOf(tree.Tree());
        CHECK(stats.splits == 1 && stats.pendingReevaluations == 1 && stats.nodeCount == 9);
        CHECK(ValidateTree(tree.Tree()));
        CHECK(QueriesMatch(tree, config, allocator, 0x810u + variant));
        if (variant == 0) {
            const MaintenanceReport report = tree.Tree().RunMaintenance(MaintenanceBudget::All());
            CHECK(report.reevaluations == 1);
        } else {
            REQUIRE(tree.Add(MakePoint(Vec3d(-2, -2, -2)), 1) != IndexNone);
        }
        stats = StatsOf(tree.Tree());
        CHECK(stats.splits == 2 && stats.pendingReevaluations == 0);
        CHECK(ValidateTree(tree.Tree()));
        CHECK(QueriesMatch(tree, config, allocator, 0x812u + variant));
    }
}

namespace {

// 시험 쪽의 독립 계수기다. 옥트리의 Stats 할당 계수와 대조한다.
class TallyAllocator final : public Allocator {
public:
    explicit TallyAllocator(Allocator& backing) noexcept : backing_(backing) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        auto allocated = backing_.Allocate(bytes, alignment, tag);
        if (allocated && bytes != 0) {
            ++allocations;
            ++live;
        }
        return allocated;
    }

    Result<void> Deallocate(void* address) noexcept override {
        auto released = backing_.Deallocate(address);
        if (released && address != nullptr) {
            ++deallocations;
            --live;
        }
        return released;
    }

    uint64 allocations = 0;
    uint64 deallocations = 0;
    uint64 live = 0;

private:
    Allocator& backing_;
};

} // namespace

// 6.10절: 할당·해제 횟수와 살아 있는·피크 byte, 배열별 예약 용량 피크. 옥트리의 계수 adapter
// 자체(생성 때 하나)를 뺀 할당 수가 바깥 계수기와 같아야 한다. 이동하면 계수가 함께 옮겨 가고
// 이동된 원본은 다음 추가에서 새 adapter를 만든다.
TEST(LooseOctree_LO10_StatsCountAllocations) {
    SystemAllocator system;
    TallyAllocator tally(system);
    {
        auto created = LooseOctree::TryCreate(SmallConfig(), tally);
        REQUIRE(created);
        CHECK(tally.allocations == 1);
        OracleTree tree(Move(created.Value()), system);
        RandomStream random(0xA10u);
        for (uint32 index = 0; index < 600; ++index) {
            REQUIRE(tree.Add(MakeBox(Distribution::Uniform, random, 60.0), 1) != IndexNone);
        }
        OctreeStats stats = StatsOf(tree.Tree());
        CHECK(stats.allocationCount == tally.allocations - 1);
        CHECK(stats.deallocationCount == tally.deallocations);
        CHECK(stats.allocationCount > 0 && stats.deallocationCount > 0);
        CHECK(stats.liveAllocatedBytes > 0 && stats.liveAllocatedBytes <= stats.peakAllocatedBytes);
        // 살아 있는 byte는 예약 용량(원소 크기 × 용량)의 합보다 작을 수 없다.
        CHECK(stats.liveAllocatedBytes >=
              stats.recordBytesReserved + stats.chunkBytesReserved + stats.nodeBytesReserved);
        CHECK(stats.peakChunkBytesReserved >= stats.chunkBytesReserved &&
              stats.peakNodeBytesReserved >= stats.nodeBytesReserved &&
              stats.peakRecordBytesReserved >= stats.recordBytesReserved);
        const usize peakBefore = stats.peakAllocatedBytes;

        // 모두 지우고 축소하면 해제가 늘고 살아 있는 byte는 줄지만 피크는 그대로다.
        for (usize index = 0; index < tree.Proxies().Size(); ++index) {
            REQUIRE(tree.Remove(index));
        }
        MaintenanceBudget shrink = MaintenanceBudget::All();
        shrink.shrink = true;
        CHECK(tree.Tree().RunMaintenance(shrink).shrunk);
        stats = StatsOf(tree.Tree());
        CHECK(stats.allocationCount == tally.allocations - 1);
        CHECK(stats.deallocationCount == tally.deallocations);
        CHECK(stats.peakAllocatedBytes == peakBefore && stats.liveAllocatedBytes < peakBefore);
        CHECK(stats.chunkBytesReserved == 0 && stats.peakChunkBytesReserved > 0);

        // 이동: 계수는 대상으로 옮겨 가고 원본은 0이다. 원본에 다시 넣으면 새 adapter를 만든다.
        LooseOctree moved = Move(tree.Tree());
        CHECK(StatsOf(moved).allocationCount == tally.allocations - 1);
        CHECK(StatsOf(tree.Tree()).allocationCount == 0);
        const uint64 before = tally.allocations;
        SpatialProxyDesc desc;
        desc.bounds = MakeCube(Vec3d(1, 1, 1), 1);
        desc.filterMask = 1;
        REQUIRE(tree.Tree().TryAdd(desc));
        stats = StatsOf(tree.Tree());
        CHECK(stats.allocationCount > 0 && stats.allocationCount == tally.allocations - before - 1);
        CHECK(ValidateTree(tree.Tree()) && ValidateTree(moved));
    }
    // 두 트리(이동 대상과 원본)가 모두 끝나면 남는 할당이 없다.
    CHECK(tally.live == 0);
}

// LO10 확대: 범위 밖 ↔ 트리 전이와, 삭제가 일으킨 강제 유지보수 중의 할당 실패. 변경 호출은
// 실패하면 OutOfMemory로 논리 상태를 보존하고, 삭제는 할당 실패와 무관하게 성공한다(강제 처리의
// 청크 확보 실패는 항목을 다시 대기시킬 뿐이다).
TEST(LooseOctree_LO10_FailureInjectionAcrossOutOfRangeAndForcedMaintenance) {
    SystemAllocator system;
    test::NFailAllocator allocator(system);
    LooseOctreeConfig config = SmallConfig();
    config.maxRootLevel = 8;
    config.pendingLimitFloor = 1;
    auto created = LooseOctree::TryCreate(config, allocator);
    REQUIRE(created);
    OracleTree tree(Move(created.Value()), system);
    // 원점의 프록시는 지우지 않아 루트가 원점에 고정된다(빈 트리 재고정 없음).
    REQUIRE(tree.Add(MakeCube(Vec3d(0, 0, 0), 1), 1) != IndexNone);
    RandomStream random(0xA11u);
    for (uint32 index = 0; index < 200; ++index) {
        REQUIRE(tree.Add(MakeBox(Distribution::Uniform, random, 60.0), 1) != IndexNone);
    }

    uint32 injected = 0;
    uint32 transitions = 0;
    bool consistent = true;
    for (uint32 step = 0; step < 120; ++step) {
        const usize target =
            1 + random.NextUInt32Below(static_cast<uint32>(tree.Proxies().Size() - 1));
        // 트리 → 범위 밖(10^6 cm) 또는 범위 밖 → 트리로 번갈아 옮긴다.
        const bool outside = tree.Proxies()[target].bounds.min.x > 1.0e5;
        const Box<double> bounds = outside ? MakeBox(Distribution::Uniform, random, 60.0)
                                           : MakeCube(Vec3d(1.0e6, Uniform(random, -10, 10), 0), 1);
        for (uint64 attempt = 1;; ++attempt) {
            allocator.FailOnAttempt(attempt);
            auto updated = tree.Tree().TryUpdateBounds(tree.Proxies()[target].handle, bounds);
            const uint64 attempts = allocator.Attempts();
            allocator.DisableFailure();
            if (updated) {
                tree.RecordBounds(target, bounds);
                ++transitions;
                break;
            }
            ++injected;
            consistent = consistent && updated.Error() == ErrorCode::OutOfMemory &&
                         attempts >= attempt && LogicalStateMatches(tree.Tree(), tree.Proxies()) &&
                         ValidateTree(tree.Tree());
            REQUIRE(attempt < 64);
        }
    }

    // 삭제마다 첫 할당을 실패시킨다. 삭제는 할당 실패와 무관하게 성공하고 상태가 맞아야 한다.
    for (usize index = 1; index < tree.Proxies().Size(); ++index) {
        allocator.FailOnAttempt(1);
        const bool removed = tree.Remove(index);
        allocator.DisableFailure();
        consistent = consistent && removed;
        if (index % 16 == 0) {
            consistent = consistent && LogicalStateMatches(tree.Tree(), tree.Proxies()) &&
                         ValidateTree(tree.Tree());
        }
    }
    CHECK(consistent);
    CHECK(injected > 0 && transitions > 20);
    CHECK(LogicalStateMatches(tree.Tree(), tree.Proxies()));
    (void)tree.Tree().RunMaintenance(MaintenanceBudget::All());
    CHECK(ValidateTree(tree.Tree()));
    CHECK(QueriesMatch(tree, config, system, 0xA12u));

    // 삭제가 일으킨 강제 유지보수: 보류된 잎 7개와 분할된 자식 하나(노드 17개, 한도 max(1, 4) =
    // 4)에서 분할된 자식을 병합 후보로 만들고 보류 잎마다 하나씩 지워 재평가 대기를 쌓으면 대기량이
    // 한도를 넘어 삭제 호출 안에서 처리된다. 그 동안 할당을 실패시킨다(병합 청크 확보가 실패하면
    // 항목을 다시 대기시킨다).
    auto forcedCreated = LooseOctree::TryCreate(config, allocator);
    REQUIRE(forcedCreated);
    OracleTree forced(Move(forcedCreated.Value()), system);
    usize firsts[8] = {};
    for (uint32 round = 0; round < 20; ++round) {
        for (uint32 octant = 1; octant < 8; ++octant) {
            const Vec3d center((octant & 1u) != 0 ? 10.0 : -10.0, (octant & 2u) != 0 ? 10.0 : -10.0,
                               (octant & 4u) != 0 ? 10.0 : -10.0);
            const usize index = forced.Add(MakeCube(center, 0.5), 1);
            REQUIRE(index != IndexNone);
            if (round == 0) {
                firsts[octant] = index;
            }
        }
    }
    usize separable[9];
    for (uint32 index = 0; index < 9; ++index) {
        const Vec3d point(-4.0 - 3.0 * index, -6.0 - 2.5 * index, -25.0 + 3.0 * index);
        separable[index] = forced.Add(MakePoint(point), 1);
        REQUIRE(separable[index] != IndexNone);
    }
    OctreeStats forcedStats = StatsOf(forced.Tree());
    REQUIRE(forcedStats.nodeCount == 17 && forcedStats.heldNodeCount == 7);
    bool forcedConsistent = true;
    const auto removeWithFailure = [&](usize index) noexcept {
        allocator.FailOnAttempt(1);
        const bool removed = forced.Remove(index);
        allocator.DisableFailure();
        forcedConsistent = forcedConsistent && removed &&
                           LogicalStateMatches(forced.Tree(), forced.Proxies()) &&
                           ValidateTree(forced.Tree());
    };
    for (uint32 index = 0; index < 5; ++index) {
        removeWithFailure(separable[index]);
    }
    for (uint32 octant = 1; octant < 8; ++octant) {
        removeWithFailure(firsts[octant]);
    }
    forcedStats = StatsOf(forced.Tree());
    CHECK(forcedConsistent);
    CHECK(forcedStats.forcedMaintenanceRuns > 0 && forcedStats.forcedMaintenanceItems > 0);
    (void)forced.Tree().RunMaintenance(MaintenanceBudget::All());
    CHECK(ValidateTree(forced.Tree()));
    CHECK(QueriesMatch(forced, config, system, 0xA13u));
}

namespace {

// 경계 갱신 품질 대조용 입력이다. 중심은 초기 루트 기본 셀([−32, 32]^3) 안, 반 변은 0.02–0.25다.
Box<double> RandomSmallBox(RandomStream& random) noexcept {
    const Vec3d center(random.NextDouble01() * 62.0 - 31.0, random.NextDouble01() * 62.0 - 31.0,
                       random.NextDouble01() * 62.0 - 31.0);
    return MakeCube(center, 0.02 + random.NextDouble01() * 0.23);
}

// 작은 박스 질의 수백 개의 공간 판정 프록시 수 합이다(구조 품질 지표).
uint64 SmallQueryTestedProxies(const LooseOctree& tree, uint64 seed) noexcept {
    SpatialReadScope read(tree);
    const LooseOctreeReader reader = read.Reader();
    RandomStream random(seed);
    QueryStats stats;
    for (uint32 query = 0; query < 256; ++query) {
        const Box<double> box = MakeCube(RandomSmallBox(random).Center(), 0.5);
        (void)reader.QueryBox(
            box, 0xFFu, [](const SpatialProxyHit&) noexcept { return QueryControl::Continue; },
            &stats);
    }
    return stats.testedProxies;
}

} // namespace

// LO06(구조 품질): 같은 시드의 순간 이동 시퀀스를 TryUpdateBounds로 적용한 트리와 TryRemove +
// TryAdd로 적용한 트리가 유지보수 뒤 같은 수준의 구조가 된다. 경계 갱신의 재삽입이 새 AABB의 중심을
// 기본 셀 밖(확장 띠)에 둔 조상에 프록시를 남기면, 그 프록시는 어느 자식에도 들어가지 못해
// 유지보수로도 내려가지 않는다(내부 노드 직접 저장이 쌓여 작은 질의의 판정 수가 늘어난다).
TEST(LooseOctree_LO06_UpdateReinsertMatchesAddQuality) {
    SystemAllocator allocator;
    const LooseOctreeConfig config = SmallConfig();
    constexpr uint32 Count = 2000;
    constexpr uint32 Rounds = 3;
    constexpr uint32 MovesPerRound = 600;
    auto createdUpdated = LooseOctree::TryCreate(config, allocator);
    auto createdReplaced = LooseOctree::TryCreate(config, allocator);
    REQUIRE(createdUpdated && createdReplaced);
    OracleTree updated(Move(createdUpdated.Value()), allocator);
    LooseOctree replaced = Move(createdReplaced.Value());
    Array<SpatialProxyHandle> handles(allocator);
    RandomStream random(0x10C6u);
    for (uint32 index = 0; index < Count; ++index) {
        const Box<double> box = RandomSmallBox(random);
        REQUIRE(updated.Add(box, 1) == index);
        SpatialProxyDesc desc;
        desc.bounds = box;
        desc.filterMask = 1;
        desc.userValue = index;
        auto added = replaced.TryAdd(desc);
        REQUIRE(added);
        REQUIRE(handles.TryPushBack(added.Value()));
    }
    const OctreeStats built = StatsOf(updated.Tree());
    CHECK(built.interiorDirectCount == StatsOf(replaced).interiorDirectCount);

    for (uint32 round = 0; round < Rounds; ++round) {
        for (uint32 move = 0; move < MovesPerRound; ++move) {
            const uint32 index = random.NextUInt32Below(Count);
            const Box<double> box = RandomSmallBox(random);
            REQUIRE(updated.Update(index, box));
            REQUIRE(replaced.TryRemove(handles[index]));
            SpatialProxyDesc desc;
            desc.bounds = box;
            desc.filterMask = 1;
            desc.userValue = index;
            auto added = replaced.TryAdd(desc);
            REQUIRE(added);
            handles[index] = added.Value();
        }
    }
    const OctreeStats movedUpdated = StatsOf(updated.Tree());
    const OctreeStats movedReplaced = StatsOf(replaced);
    (void)updated.Tree().RunMaintenance(MaintenanceBudget::All());
    (void)replaced.RunMaintenance(MaintenanceBudget::All());
    const OctreeStats maintainedUpdated = StatsOf(updated.Tree());
    const OctreeStats maintainedReplaced = StatsOf(replaced);
    const uint64 testedUpdated = SmallQueryTestedProxies(updated.Tree(), 0x10C7u);
    const uint64 testedReplaced = SmallQueryTestedProxies(replaced, 0x10C7u);
    CHECK(ValidateTree(updated.Tree()));
    CHECK(ValidateTree(replaced));
    CHECK(QueriesMatch(updated, config, allocator, 0x10C8u));
    // 허용 범위: 두 트리는 같은 프록시 집합이지만 구조가 비트 단위로 같을 필요는 없다. 경계 갱신은
    // 분할 평가를 유지보수로 미루고(6.4절), 확장 경계 안에 떨어진 순간 이동은 제자리 갱신으로
    // 노드를 지키며, 삭제 후 재삽입 쪽은 삭제가 병합 대기를 만든다. 그래서 노드 수와 질의 판정 수는
    // 조금 다르다. 결함(조상의 확장 띠에 남는 재삽입)은 갱신 직후 내부 노드 직접 저장 수백 개,
    // 유지보수 뒤에도 질의 판정 수 4배 이상으로 나타났다. 기준은 그보다 한 자릿수 엄격하다: 내부
    // 직접 저장 차이는 프록시의 1% 이하(갱신 직후·유지보수 뒤 모두), 질의 판정 수는 25% + 질의당
    // 1개 이하, 노드 수는 0.5–2배, 유지보수 뒤 노드당 최대 직접 저장은 분할 임계 이하(보류 노드
    // 없음).
    CHECK(movedUpdated.interiorDirectCount <= movedReplaced.interiorDirectCount + Count / 100);
    CHECK(maintainedUpdated.interiorDirectCount <=
          maintainedReplaced.interiorDirectCount + Count / 100);
    CHECK(testedUpdated * 4 <= testedReplaced * 5 + 4 * 256);
    CHECK(maintainedUpdated.nodeCount * 2 >= maintainedReplaced.nodeCount &&
          maintainedUpdated.nodeCount <= maintainedReplaced.nodeCount * 2);
    CHECK(maintainedUpdated.heldNodeCount == 0 &&
          maintainedUpdated.maxDirectCount <= config.splitThreshold);
    CHECK(maintainedUpdated.reinserts + maintainedUpdated.inPlaceUpdates == Rounds * MovesPerRound);
}
