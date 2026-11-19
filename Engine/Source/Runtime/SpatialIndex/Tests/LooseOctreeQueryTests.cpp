#include "LooseOctreeTestSupport.h"

#include <Core/Memory/SystemAllocator.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Platform/PlatformMath.h>

using namespace rocks;
using namespace rocks::spatial;
using namespace rocks::spatial::oracle;

// LooseOctree 질의와 API 계약 시험이다(Docs/Design/SpatialIndex.ko.md 8절 LO01·LO03·LO04·LO12·LO14·
// LO15, 박스 질의 범위). 정답은 전수 검색이다.

namespace {

Box<double> MakeCube(const Vec3d& center, double half) noexcept {
    return Box<double>(center - Vec3d::Splat(half), center + Vec3d::Splat(half));
}

double NaN() noexcept {
    return platform::PlatformMath::QuietNaN<double>();
}

double Infinity() noexcept {
    return platform::PlatformMath::Infinity<double>();
}

// 같은 시드로 같은 연산열을 만들어 두 트리에 적용할 수 있게 하는 작은 작업 부하다.
void RunWorkload(OracleTree& tree, uint64 seed, uint32 operations) noexcept {
    RandomStream random(seed);
    for (uint32 step = 0; step < operations; ++step) {
        const uint32 action = random.NextUInt32Below(10);
        if (action < 5 || tree.LiveCount() == 0) {
            (void)tree.Add(MakeBox(Distribution::Uniform, random, 120.0),
                           1u << random.NextUInt32Below(3));
        } else if (action < 7) {
            (void)tree.Remove(tree.PickLive(random));
        } else {
            const usize index = tree.PickLive(random);
            (void)tree.Update(index, MakeBox(Distribution::Uniform, random, 120.0));
        }
        if (step % 64 == 63) {
            (void)tree.Tree().RunMaintenance(MaintenanceBudget::Operations(8));
        }
    }
}

} // namespace

// LO01(박스): 8개 분포, 필터 조합, 질의 mask 0에서 전수 검색과 대조한다. 누락 0건·중복 0건이고 초과
// 후보는 float 보수성 폭 안이어야 한다. 삭제·이동을 섞어 내부 노드 저장과 범위 밖 목록도 지난다.
TEST(LooseOctree_LO01_BoxQueriesMatchExhaustiveSearch) {
    SystemAllocator allocator;
    const LooseOctreeConfig config = SmallConfig();
    uint64 exactTotal = 0;
    for (const Distribution distribution : AllDistributions) {
        RandomStream random(0x5EED0001u + static_cast<uint32>(distribution));
        auto created = LooseOctree::TryCreate(config, allocator);
        REQUIRE(created);
        OracleTree tree(Move(created.Value()), allocator);
        for (uint32 index = 0; index < 1200; ++index) {
            REQUIRE(tree.Add(MakeBox(distribution, random, 200.0),
                             1u << random.NextUInt32Below(4)) != IndexNone);
        }
        for (uint32 index = 0; index < 200; ++index) {
            REQUIRE(tree.Remove(tree.PickLive(random)));
            REQUIRE(tree.Update(tree.PickLive(random), MakeBox(distribution, random, 200.0)));
        }
        OctreeValidationReport report;
        CHECK(ValidateTree(tree.Tree(), &report));
        CHECK(report.invariant == OctreeInvariantNone);

        const double tolerance = ToleranceFor(tree.Tree(), config);
        SpatialReadScope read(tree.Tree());
        const LooseOctreeReader reader = read.Reader();
        for (uint32 query = 0; query < 120; ++query) {
            const Box<double> box = MakeQuery(random, 200.0);
            // 10번에 한 번은 mask 0: 아무것도 돌려주지 않고 오류도 아니다.
            const uint32 mask = query % 10 == 0 ? 0u : 1u + random.NextUInt32Below(15);
            const QueryCheck check =
                CheckBoxQuery(reader, tree.Proxies(), box, mask, tolerance, allocator);
            CHECK(check.ok);
            if (mask == 0) {
                CHECK(check.results == 0);
            }
            exactTotal += check.exact;
        }
    }
    // 대조가 실제로 후보를 지났는지 확인한다.
    CHECK(exactTotal > 1000);
}

// LO03: 경계 접촉과 크기 0. 닫힌 집합이므로 면·모서리·꼭짓점 접촉은 교차이며 반드시 후보다.
TEST(LooseOctree_LO03_BoundaryContactAndZeroSize) {
    SystemAllocator allocator;
    auto created = LooseOctree::TryCreate(SmallConfig(), allocator);
    REQUIRE(created);
    OracleTree tree(Move(created.Value()), allocator);
    // 정수 격자 위의 점(크기 0)과 단위 cube. 노드 경계·중심과 같은 좌표가 많다.
    for (int32 x = -6; x <= 6; x += 2) {
        for (int32 y = -6; y <= 6; y += 3) {
            for (int32 z = -4; z <= 4; z += 4) {
                const Vec3d point(x, y, z);
                REQUIRE(tree.Add(Box<double>(point, point), 1) != IndexNone);
                REQUIRE(tree.Add(Box<double>(point, point + Vec3d(1, 1, 1)), 2) != IndexNone);
            }
        }
    }
    CHECK(ValidateTree(tree.Tree()));
    SpatialReadScope read(tree.Tree());
    const LooseOctreeReader reader = read.Reader();
    for (int32 x = -7; x <= 7; ++x) {
        // 질의의 면이 점·cube 면과 정확히 같은 좌표다.
        const Box<double> slab(Vec3d(x, -7, -5), Vec3d(x, 7, 5));
        const QueryCheck check = CheckBoxQuery(reader, tree.Proxies(), slab, 3, 0.0, allocator);
        CHECK(check.ok && check.extra == 0);
        const Box<double> corner(Vec3d(x + 1, 7, 5), Vec3d(x + 2, 8, 6));
        CHECK(CheckBoxQuery(reader, tree.Proxies(), corner, 3, 0.0, allocator).ok);
    }
    // 크기 0 질의(점 질의)도 유효하다.
    const QueryCheck point = CheckBoxQuery(
        reader, tree.Proxies(), Box<double>(Vec3d(2, 3, 4), Vec3d(2, 3, 4)), 3, 0.0, allocator);
    CHECK(point.ok && point.exact >= 2);
}

// LO03: 루트보다 큰 객체는 루트를 키우고, 확장 한도보다 큰 객체는 범위 밖 목록에 들어가 항상
// 판정된다.
TEST(LooseOctree_LO03_ObjectsLargerThanRootAndOutOfRange) {
    SystemAllocator allocator;
    LooseOctreeConfig config = SmallConfig();
    config.maxRootLevel = 10;
    auto created = LooseOctree::TryCreate(config, allocator);
    REQUIRE(created);
    OracleTree tree(Move(created.Value()), allocator);
    REQUIRE(tree.Add(MakeCube(Vec3d(0, 0, 0), 1), 1) != IndexNone);
    // 초기 루트(한 변 64 cm)보다 큰 객체.
    REQUIRE(tree.Add(MakeCube(Vec3d(10, 0, 0), 200), 1) != IndexNone);
    {
        SpatialReadScope read(tree.Tree());
        const OctreeStats stats = read.Reader().Stats();
        CHECK(stats.rootLevel > config.initialRootLevel && stats.outOfRangeCount == 0);
    }
    // 확장 한도(level 10, 한 변 1024 cm)를 넘는 객체와 먼 위치의 객체.
    REQUIRE(tree.Add(MakeCube(Vec3d(0, 0, 0), 5000), 1) != IndexNone);
    REQUIRE(tree.Add(MakeCube(Vec3d(1.0e7, -3.0e6, 0), 1), 1) != IndexNone);
    {
        SpatialReadScope read(tree.Tree());
        const OctreeStats stats = read.Reader().Stats();
        CHECK(stats.outOfRangeCount == 2 && stats.outOfRangeInsertions == 2);
        CHECK(stats.rootLevel <= config.maxRootLevel);
    }
    CHECK(ValidateTree(tree.Tree()));
    const double tolerance = ToleranceFor(tree.Tree(), config);
    SpatialReadScope read(tree.Tree());
    const LooseOctreeReader reader = read.Reader();
    const Box<double> queries[] = {
        MakeCube(Vec3d(0, 0, 0), 2), MakeCube(Vec3d(1.0e7, -3.0e6, 0), 3),
        MakeCube(Vec3d(4000, 4000, 4000), 1), MakeCube(Vec3d(-3000, 0, 0), 10)};
    for (const Box<double>& box : queries) {
        CHECK(CheckBoxQuery(reader, tree.Proxies(), box, 1, tolerance, allocator).ok);
    }
}

// LO03: 위치와 AABB가 같은 1만 개는 분리할 수 없으므로 큰 잎(청크 연결)으로 보류된다.
// 검색·삭제·검증이 정확해야 한다.
TEST(LooseOctree_LO03_TenThousandIdenticalBoxes) {
    SystemAllocator allocator;
    LooseOctreeConfig config = SmallConfig();
    config.parentWarnCount = 100000;
    auto created = LooseOctree::TryCreate(config, allocator);
    REQUIRE(created);
    OracleTree tree(Move(created.Value()), allocator);
    const Box<double> same = MakeCube(Vec3d(3.25, -2.5, 1.0), 0.75);
    for (uint32 index = 0; index < 10000; ++index) {
        REQUIRE(tree.Add(same, 1u << (index % 3)) != IndexNone);
    }
    {
        SpatialReadScope read(tree.Tree());
        const OctreeStats stats = read.Reader().Stats();
        CHECK(stats.heldNodeCount == 1 && stats.splits == 0);
        // 보류 중에는 삽입마다 평가를 반복하지 않는다(재평가 간격이 지수로 늘어난다).
        CHECK(stats.splitEvaluations < 64);
        CHECK(stats.maxDirectCount == 10000);
    }
    CHECK(ValidateTree(tree.Tree()));
    RandomStream random(0x1D3A7u);
    for (uint32 index = 0; index < 5000; ++index) {
        REQUIRE(tree.Remove(tree.PickLive(random)));
    }
    CHECK(ValidateTree(tree.Tree()));
    SpatialReadScope read(tree.Tree());
    const LooseOctreeReader reader = read.Reader();
    const QueryCheck check =
        CheckBoxQuery(reader, tree.Proxies(), MakeCube(Vec3d(3, -2, 1), 0.5), 5, 0.0, allocator);
    CHECK(check.ok && check.exact == check.results);
    auto counted = reader.CountInBox(MakeCube(Vec3d(0, 0, 0), 100), 7);
    REQUIRE(counted);
    CHECK(counted.Value() == 5000);
}

// LO03: 음수 좌표, 원점에서 10^9 cm, 루트 여유 띠에 중심이 있는 점 다수. 기본 설정(128 cm 셀, 최대
// 루트 level 24)을 쓴다.
TEST(LooseOctree_LO03_FarAndNegativeCoordinatesAndRootBand) {
    SystemAllocator allocator;
    LooseOctreeConfig config;
    config.parentWarnCount = 100000;
    {
        auto created = LooseOctree::TryCreate(config, allocator);
        REQUIRE(created);
        OracleTree tree(Move(created.Value()), allocator);
        RandomStream random(0xFA4u);
        const Vec3d far(1.0e9, -1.0e9, 1.0e9);
        for (uint32 index = 0; index < 300; ++index) {
            const Vec3d offset(Uniform(random, -5000, 5000), Uniform(random, -5000, 5000),
                               Uniform(random, -5000, 5000));
            REQUIRE(tree.Add(MakeCube(far + offset, Uniform(random, 0, 300)), 1) != IndexNone);
            REQUIRE(tree.Add(MakeCube(-offset * 10.0, Uniform(random, 0, 50)), 2) != IndexNone);
        }
        CHECK(ValidateTree(tree.Tree()));
        const double tolerance = ToleranceFor(tree.Tree(), config);
        SpatialReadScope read(tree.Tree());
        const LooseOctreeReader reader = read.Reader();
        CHECK(reader.Stats().rootLevel <= config.maxRootLevel);
        for (uint32 query = 0; query < 60; ++query) {
            const Vec3d base = query % 2 == 0 ? far : Vec3d(0, 0, 0);
            const Box<double> box =
                MakeCube(base + Vec3d(Uniform(random, -6000, 6000), Uniform(random, -6000, 6000),
                                      Uniform(random, -6000, 6000)),
                         Uniform(random, 1, 4000));
            CHECK(CheckBoxQuery(reader, tree.Proxies(), box, 3, tolerance, allocator).ok);
        }
    }

    // 루트 여유 띠: 초기 루트(level 10, 셀 ±65536 cm, 확장 경계 ±131072 cm)의 셀 밖·확장 경계 안에
    // 중심을 둔 점은 루트에 쌓이지 않도록 루트를 키운다(6.5절 중심 조건).
    auto created = LooseOctree::TryCreate(config, allocator);
    REQUIRE(created);
    OracleTree tree(Move(created.Value()), allocator);
    // 원점의 프록시를 먼저 둔다. 빈 트리의 첫 삽입은 루트를 그 중심의 격자 셀로 다시 잡으므로(6.5절
    // 빈 트리) 띠 시나리오가 되려면 루트가 원점에 고정되어 있어야 한다.
    REQUIRE(tree.Add(MakeCube(Vec3d(0, 0, 0), 10), 1) != IndexNone);
    RandomStream random(0xBA4Du);
    for (uint32 index = 0; index < 200; ++index) {
        const Vec3d point(Uniform(random, 70000, 130000), Uniform(random, -130000, -70000),
                          Uniform(random, -1000, 1000));
        REQUIRE(tree.Add(Box<double>(point, point), 1) != IndexNone);
    }
    CHECK(ValidateTree(tree.Tree()));
    SpatialReadScope read(tree.Tree());
    const LooseOctreeReader reader = read.Reader();
    CHECK(reader.Stats().rootLevel > config.initialRootLevel);
    uint32 rootDirect = 0;
    reader.VisitNodes([&](const SpatialNodeInfo& info) noexcept {
        if (info.depth == 1) {
            rootDirect = info.directCount;
        }
    });
    CHECK(rootDirect == 0);
    CHECK(CheckBoxQuery(reader, tree.Proxies(), MakeCube(Vec3d(100000, -100000, 0), 20000), 1,
                        ToleranceFor(tree.Tree(), config), allocator)
              .ok);
}

// LO04: 입력 검증. 잘못된 AABB·handle·질의는 정해진 오류 코드로 끝나고 상태를 바꾸지 않는다.
TEST(LooseOctree_LO04_InputValidation) {
    SystemAllocator allocator;
    auto created = LooseOctree::TryCreate(SmallConfig(), allocator);
    REQUIRE(created);
    OracleTree tree(Move(created.Value()), allocator);
    const usize kept = tree.Add(MakeCube(Vec3d(1, 2, 3), 1), 1);
    const usize removed = tree.Add(MakeCube(Vec3d(-1, -2, -3), 1), 1);
    REQUIRE(kept != IndexNone && removed != IndexNone);
    const SpatialProxyHandle stale = tree.Proxies()[removed].handle;
    REQUIRE(tree.Remove(removed));

    const Box<double> invalid[] = {
        Box<double>(Vec3d(NaN(), 0, 0), Vec3d(1, 1, 1)),
        Box<double>(Vec3d(0, 0, 0), Vec3d(1, Infinity(), 1)),
        Box<double>(Vec3d(0, -Infinity(), 0), Vec3d(1, 1, 1)),
        Box<double>(Vec3d(2, 0, 0), Vec3d(1, 1, 1)),
    };
    LooseOctree& octree = tree.Tree();
    for (const Box<double>& box : invalid) {
        SpatialProxyDesc desc;
        desc.bounds = box;
        desc.filterMask = 1;
        CHECK(octree.TryAdd(desc).Error() == ErrorCode::InvalidArgument);
        CHECK(octree.TryUpdateBounds(tree.Proxies()[kept].handle, box).Error() ==
              ErrorCode::InvalidArgument);
    }

    const SpatialProxyHandle null;
    CHECK(octree.TryRemove(null).Error() == ErrorCode::InvalidArgument);
    CHECK(octree.TryUpdateBounds(null, MakeCube(Vec3d(0, 0, 0), 1)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(octree.TrySetFilter(null, 1).Error() == ErrorCode::InvalidArgument);
    CHECK(octree.TrySetUserValue(null, 1).Error() == ErrorCode::InvalidArgument);
    // 낡은 handle과 이중 삭제.
    CHECK(octree.TryRemove(stale).Error() == ErrorCode::StaleHandle);
    CHECK(octree.TryUpdateBounds(stale, MakeCube(Vec3d(0, 0, 0), 1)).Error() ==
          ErrorCode::StaleHandle);
    CHECK(octree.TrySetFilter(stale, 1).Error() == ErrorCode::StaleHandle);
    CHECK(octree.TrySetUserValue(stale, 1).Error() == ErrorCode::StaleHandle);

    CHECK(LogicalStateMatches(octree, tree.Proxies()));
    CHECK(ValidateTree(octree));

    SpatialReadScope read(octree);
    const LooseOctreeReader reader = read.Reader();
    CHECK(reader.TryGet(null).Error() == ErrorCode::InvalidArgument);
    CHECK(reader.TryGet(stale).Error() == ErrorCode::StaleHandle);
    CHECK(!reader.IsValid(stale) && !reader.IsValid(null));
    for (const Box<double>& box : invalid) {
        uint32 calls = 0;
        auto queried = reader.QueryBox(box, 1, [&](const SpatialProxyHit&) noexcept {
            ++calls;
            return QueryControl::Continue;
        });
        CHECK(queried.Error() == ErrorCode::InvalidArgument && calls == 0);
        SpatialProxyHit buffer[2];
        CHECK(reader.QueryBoxInto(box, 1, Span<SpatialProxyHit>(buffer, 2)).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(reader.AnyInBox(box, 1).Error() == ErrorCode::InvalidArgument);
        CHECK(reader.CountInBox(box, 1).Error() == ErrorCode::InvalidArgument);
    }
    // mask 0은 오류가 아니라 빈 결과다.
    auto empty = reader.CountInBox(MakeCube(Vec3d(0, 0, 0), 100), 0);
    CHECK(empty && empty.Value() == 0);
}

// LO04: TryCreate의 설정 범위 검사.
TEST(LooseOctree_LO04_CreateRejectsInvalidConfig) {
    SystemAllocator allocator;
    const auto rejects = [&](const LooseOctreeConfig& config) noexcept {
        return LooseOctree::TryCreate(config, allocator).Error() == ErrorCode::InvalidArgument;
    };
    LooseOctreeConfig config;
    CHECK(LooseOctree::TryCreate(config, allocator));

    config = LooseOctreeConfig{};
    config.minCellSize = 100.0; // 2의 거듭제곱이 아니다.
    CHECK(rejects(config));
    config.minCellSize = 0.0;
    CHECK(rejects(config));
    config = LooseOctreeConfig{};
    config.anchor = Vec3d(64.0, 0.0, 0.0); // minCellSize 128의 정수배가 아니다.
    CHECK(rejects(config));
    config.anchor = Vec3d(256.0, -128.0, 0.0);
    CHECK(LooseOctree::TryCreate(config, allocator));
    config = LooseOctreeConfig{};
    config.looseness = 1.0 + 1.0 / 128.0; // 1/64 단위가 아니다.
    CHECK(rejects(config));
    config.looseness = 2.5;
    CHECK(rejects(config));
    config.looseness = 65.0 / 64.0;
    CHECK(LooseOctree::TryCreate(config, allocator));
    config = LooseOctreeConfig{};
    config.initialRootLevel = 0;
    CHECK(rejects(config));
    config.initialRootLevel = 25; // maxRootLevel(24)보다 크다.
    CHECK(rejects(config));
    config = LooseOctreeConfig{};
    config.maxRootLevel = LooseOctreeLevelLimit + 1;
    CHECK(rejects(config));
    config = LooseOctreeConfig{};
    config.splitThreshold = 3;
    CHECK(rejects(config));
    config.splitThreshold = 16;
    config.mergeThreshold = 9; // splitThreshold / 2 = 8보다 크다.
    CHECK(rejects(config));
    config = LooseOctreeConfig{};
    config.maxProxies = 0;
    CHECK(rejects(config));
    config = LooseOctreeConfig{};
    config.maxProxyGeneration = 0;
    CHECK(rejects(config));
    config = LooseOctreeConfig{};
    config.pendingLimitFloor = 0;
    CHECK(rejects(config));
    // 격자 계산의 정확 범위: |anchor| + 2^(maxRootLevel+1) · minCellSize < 2^52 · minCellSize.
    config = LooseOctreeConfig{};
    config.maxRootLevel = 31;
    config.minCellSize = 1.0;
    config.anchor = Vec3d(0x1.0p52 - 0x1.0p31, 0.0, 0.0);
    CHECK(rejects(config));
    // float 범위: k · 2^maxRootLevel · minCellSize가 float 최대값을 넘는다.
    config = LooseOctreeConfig{};
    config.maxRootLevel = 31;
    config.minCellSize = 0x1.0p100;
    CHECK(rejects(config));
}

// LO12: 조기 종료와 버퍼. Stop은 즉시 끝나고, Into는 용량이 모자라도 끝까지 세어 truncated와
// total을 알린다.
TEST(LooseOctree_LO12_EarlyStopAndBuffers) {
    SystemAllocator allocator;
    auto created = LooseOctree::TryCreate(SmallConfig(), allocator);
    REQUIRE(created);
    OracleTree tree(Move(created.Value()), allocator);
    RandomStream random(0x12u);
    for (uint32 index = 0; index < 100; ++index) {
        REQUIRE(tree.Add(MakeCube(Vec3d(Uniform(random, -20, 20), Uniform(random, -20, 20),
                                        Uniform(random, -20, 20)),
                                  0.5),
                         index < 60 ? 1u : 2u) != IndexNone);
    }
    // 범위 밖 목록 항목도 같은 계약을 따른다.
    REQUIRE(tree.Add(MakeCube(Vec3d(0, 0, 0), 1.0e6), 1) != IndexNone);
    SpatialReadScope read(tree.Tree());
    const LooseOctreeReader reader = read.Reader();
    const Box<double> all = MakeCube(Vec3d(0, 0, 0), 30);

    uint32 calls = 0;
    auto stopped = reader.QueryBox(all, 3, [&](const SpatialProxyHit&) noexcept {
        ++calls;
        return calls == 3 ? QueryControl::Stop : QueryControl::Continue;
    });
    REQUIRE(stopped);
    CHECK(stopped.Value().stopped && stopped.Value().visited == 3 && calls == 3);

    auto full = reader.QueryBox(
        all, 3, [](const SpatialProxyHit&) noexcept { return QueryControl::Continue; });
    REQUIRE(full);
    CHECK(!full.Value().stopped && full.Value().visited == 101);

    SpatialProxyHit buffer[16];
    auto truncated = reader.QueryBoxInto(all, 1, Span<SpatialProxyHit>(buffer, 16));
    REQUIRE(truncated);
    CHECK(truncated.Value().written == 16 && truncated.Value().total == 61 &&
          truncated.Value().truncated);
    for (const SpatialProxyHit& hit : buffer) {
        CHECK(hit.filterMask == 1 && reader.IsValid(hit.handle));
    }
    auto exact = reader.QueryBoxInto(all, 2, Span<SpatialProxyHit>(buffer, 16));
    REQUIRE(exact);
    CHECK(exact.Value().written == 16 && exact.Value().total == 40 && exact.Value().truncated);
    auto none = reader.QueryBoxInto(MakeCube(Vec3d(0, 0, 0), 30), 4, Span<SpatialProxyHit>());
    REQUIRE(none);
    CHECK(none.Value().total == 0 && !none.Value().truncated);

    auto count = reader.CountInBox(all, 2);
    auto any = reader.AnyInBox(all, 2);
    auto nothing = reader.AnyInBox(all, 4);
    REQUIRE(count && any && nothing);
    CHECK(count.Value() == 40 && any.Value() && !nothing.Value());

    // 계수: 결과 수는 visitor 호출 수와 같다.
    QueryStats stats;
    auto counted = reader.QueryBox(
        all, 3, [](const SpatialProxyHit&) noexcept { return QueryControl::Continue; }, &stats);
    REQUIRE(counted);
    CHECK(stats.results == 101 && stats.visitedNodes >= 1);
    CHECK(stats.testedProxies + stats.containedProxies >= 101);
}

// LO14: API 계약(필터·사용자 값 변경, 예약, Clear 뒤 낡은 handle, 이동한 인스턴스의 handle, visitor
// 안 재검색).
TEST(LooseOctree_LO14_ApiContracts) {
    SystemAllocator allocator;
    auto created = LooseOctree::TryCreate(SmallConfig(), allocator);
    REQUIRE(created);
    LooseOctree octree = Move(created.Value());
    REQUIRE(octree.TryReserve(500));
    CHECK(octree.TryReserve(SmallConfig().maxProxies + 1).Error() == ErrorCode::CapacityExceeded);

    SpatialProxyHandle handles[200];
    RandomStream random(0x14u);
    for (uint32 index = 0; index < 200; ++index) {
        SpatialProxyDesc desc;
        desc.bounds = MakeCube(
            Vec3d(Uniform(random, -30, 30), Uniform(random, -30, 30), Uniform(random, -30, 30)),
            Uniform(random, 0, 2));
        desc.filterMask = 1;
        desc.userValue = index;
        auto added = octree.TryAdd(desc);
        REQUIRE(added);
        handles[index] = added.Value();
    }
    // 필터·사용자 값 변경은 다음 질의에 바로 보인다.
    REQUIRE(octree.TrySetFilter(handles[7], 0));
    REQUIRE(octree.TrySetFilter(handles[8], 4));
    REQUIRE(octree.TrySetUserValue(handles[9], 9000));
    {
        SpatialReadScope read(octree);
        const LooseOctreeReader reader = read.Reader();
        const Box<double> all = MakeCube(Vec3d(0, 0, 0), 40);
        bool sawSeven = false;
        bool sawNine = false;
        uint32 outer = 0;
        uint32 nestedTotal = 0;
        auto queried = reader.QueryBox(all, 0xFFu, [&](const SpatialProxyHit& hit) noexcept {
            sawSeven = sawSeven || hit.handle == handles[7];
            sawNine = sawNine || (hit.handle == handles[9] && hit.userValue == 9000);
            ++outer;
            // visitor 안 재검색은 허용된다(스택을 하나 더 쓴다).
            if (outer <= 3) {
                auto nested = reader.CountInBox(all, 4);
                nestedTotal += nested ? static_cast<uint32>(nested.Value()) : 1000u;
            }
            return QueryControl::Continue;
        });
        REQUIRE(queried);
        CHECK(!sawSeven && sawNine && outer == 199 && nestedTotal == 3);
        auto got = reader.TryGet(handles[8]);
        REQUIRE(got);
        CHECK(got.Value().filterMask == 4 && got.Value().userValue == 8);
    }

    // 이동: 기존 handle은 이동 대상에서 유효하고 원본은 빈 트리로 다시 쓸 수 있다.
    LooseOctree moved = Move(octree);
    {
        SpatialReadScope read(moved);
        const LooseOctreeReader reader = read.Reader();
        CHECK(reader.Count() == 200 && reader.IsValid(handles[0]) && reader.IsValid(handles[199]));
        CHECK(reader.Validate().Value().IsValid());
    }
    {
        SpatialReadScope read(octree);
        CHECK(read.Reader().Count() == 0);
        CHECK(read.Reader().Validate().Value().IsValid());
    }
    SpatialProxyDesc desc;
    desc.bounds = MakeCube(Vec3d(0, 0, 0), 1);
    desc.filterMask = 1;
    REQUIRE(octree.TryAdd(desc));
    CHECK(ValidateTree(octree));

    // Clear: 모든 프록시가 지워지고 지우기 전 handle은 StaleHandle이다. 세대 번호를 유지하므로 다시
    // 쓰는 slot의 새 handle은 옛 handle과 다르다.
    moved.Clear();
    CHECK(moved.TryRemove(handles[5]).Error() == ErrorCode::StaleHandle);
    auto again = moved.TryAdd(desc);
    REQUIRE(again);
    CHECK(!(again.Value() == handles[0]) && !(again.Value() == handles[199]));
    SpatialReadScope read(moved);
    CHECK(read.Reader().Count() == 1 && !read.Reader().IsValid(handles[5]));
    CHECK(read.Reader().Validate().Value().IsValid());
}

// LO15: 결정성. 같은 연산열과 작업 수 예산에서 결과 순서와 구조가 같고, Stats()의 구조 값이 독립
// 계수와 일치한다.
TEST(LooseOctree_LO15_DeterministicOrderAndIndependentCounts) {
    SystemAllocator allocator;
    auto first = LooseOctree::TryCreate(SmallConfig(), allocator);
    auto second = LooseOctree::TryCreate(SmallConfig(), allocator);
    REQUIRE(first && second);
    OracleTree left(Move(first.Value()), allocator);
    OracleTree right(Move(second.Value()), allocator);
    RunWorkload(left, 0xD37u, 4000);
    RunWorkload(right, 0xD37u, 4000);

    SpatialReadScope leftRead(left.Tree());
    SpatialReadScope rightRead(right.Tree());
    const LooseOctreeReader leftReader = leftRead.Reader();
    const LooseOctreeReader rightReader = rightRead.Reader();
    RandomStream random(0xD38u);
    for (uint32 query = 0; query < 50; ++query) {
        const Box<double> box = MakeQuery(random, 120.0);
        Array<uint64> leftOrder(allocator);
        Array<uint64> rightOrder(allocator);
        auto leftQueried = leftReader.QueryBox(box, 7, [&](const SpatialProxyHit& hit) noexcept {
            (void)leftOrder.TryPushBack(hit.userValue);
            return QueryControl::Continue;
        });
        auto rightQueried = rightReader.QueryBox(box, 7, [&](const SpatialProxyHit& hit) noexcept {
            (void)rightOrder.TryPushBack(hit.userValue);
            return QueryControl::Continue;
        });
        REQUIRE(leftQueried && rightQueried);
        REQUIRE(leftOrder.Size() == rightOrder.Size());
        for (usize index = 0; index < leftOrder.Size(); ++index) {
            CHECK(leftOrder[index] == rightOrder[index]);
        }
    }

    const OctreeStats stats = leftReader.Stats();
    const OctreeStats other = rightReader.Stats();
    CHECK(stats.nodeCount == other.nodeCount && stats.splits == other.splits &&
          stats.merges == other.merges && stats.reinserts == other.reinserts);

    // 독립 계수: 모형과 VisitNodes로 센 값.
    uint32 nodes = 0;
    uint32 leaves = 0;
    uint32 maxDepth = 0;
    uint64 inTree = 0;
    uint32 interior = 0;
    leftReader.VisitNodes([&](const SpatialNodeInfo& info) noexcept {
        ++nodes;
        leaves += info.leaf ? 1u : 0u;
        maxDepth = info.depth > maxDepth ? info.depth : maxDepth;
        inTree += info.directCount;
        interior += info.leaf ? 0u : info.directCount;
    });
    CHECK(stats.proxyCount == left.LiveCount());
    CHECK(stats.nodeCount == nodes && stats.leafCount == leaves && stats.maxDepth == maxDepth);
    CHECK(stats.interiorDirectCount == interior);
    CHECK(inTree + stats.outOfRangeCount == stats.proxyCount);
}

namespace {

// LO15 독립 모형의 범위 판정이다. 루트가 커질 수 없는 설정(maxRootLevel = initialRootLevel = 6,
// k = 2)에서 트리에 들어가려면 AABB가 루트 확장 경계 [−64, 64]^3 안이고 중심이 루트 셀 [−32, 32]^3
// 안이어야 한다(6.5절 루트 조건). 옥트리 내부 구조를 읽지 않고 입력만으로 정한다.
bool FitsFixedRoot(const Box<double>& box) noexcept {
    const Box<double> loose(Vec3d(-64, -64, -64), Vec3d(64, 64, 64));
    const Box<double> cell(Vec3d(-32, -32, -32), Vec3d(32, 32, 32));
    return loose.Contains(box) && cell.Contains(box.Center());
}

// allowBand가 거짓이면 루트 셀 밖·확장 경계 안의 띠를 만들지 않는다. 경계 갱신은 지금 노드의 확장
// 경계 안이면 중심 조건 없이 제자리에서 끝나므로(6.5절) 띠로의 갱신은 노드를 모르는 모형이 예측할
// 수 없다.
Box<double> ModelBox(RandomStream& random, bool allowBand) noexcept {
    const uint32 kind = random.NextUInt32Below(10);
    if (kind < 6 || (!allowBand && kind < 8)) {
        return MakeCube(
            Vec3d(Uniform(random, -30, 30), Uniform(random, -30, 30), Uniform(random, -30, 30)),
            Uniform(random, 0, 0.6));
    }
    if (kind < 8) {
        // 루트 셀 밖·확장 경계 안의 띠: 루트를 키울 수 없으므로 범위 밖이다.
        return MakeCube(Vec3d(Uniform(random, 33, 60), Uniform(random, -30, 30), 0), 0.5);
    }
    return MakeCube(Vec3d(1.0e4 + Uniform(random, -100, 100), 0, 0), 1);
}

} // namespace

// LO15: Stats()의 값을 옥트리 내부 배열(VisitNodes)에 기대지 않는 독립 모형과 대조한다. 모형은
// 입력과 연산 기록만으로 프록시 수, 범위 밖 수, 범위 밖 삽입 수, 경계 갱신 수(제자리 + 재삽입),
// 루트 확장·축소·빈 트리 재설정 수(모두 0)를 계산한다.
TEST(LooseOctree_LO15_StatsMatchIndependentModel) {
    SystemAllocator allocator;
    LooseOctreeConfig config = SmallConfig();
    config.maxRootLevel = config.initialRootLevel;
    auto created = LooseOctree::TryCreate(config, allocator);
    REQUIRE(created);
    OracleTree tree(Move(created.Value()), allocator);
    // 원점의 프록시는 지우지 않아 빈 트리 재고정이 일어나지 않는다.
    REQUIRE(tree.Add(MakeCube(Vec3d(0, 0, 0), 1), 1) != IndexNone);

    uint64 live = 1;
    uint64 outside = 0;
    uint64 outsideInsertions = 0;
    uint64 treeUpdates = 0;
    RandomStream random(0x1515u);
    for (uint32 step = 0; step < 3000; ++step) {
        const uint32 action = random.NextUInt32Below(10);
        if (action < 4 || live < 2) {
            const Box<double> box = ModelBox(random, true);
            REQUIRE(tree.Add(box, 1) != IndexNone);
            ++live;
            if (!FitsFixedRoot(box)) {
                ++outside;
                ++outsideInsertions;
            }
        } else if (action < 6) {
            usize index = tree.PickLive(random);
            if (index == 0) {
                continue;
            }
            const bool wasOutside = !FitsFixedRoot(tree.Proxies()[index].bounds);
            REQUIRE(tree.Remove(index));
            --live;
            outside -= wasOutside ? 1 : 0;
        } else {
            const usize index = tree.PickLive(random);
            const Box<double> box = ModelBox(random, false);
            const bool wasOutside = !FitsFixedRoot(tree.Proxies()[index].bounds);
            const bool nowOutside = !FitsFixedRoot(box);
            REQUIRE(tree.Update(index, box));
            if (!(wasOutside && nowOutside)) {
                // 트리 안 이동(제자리 또는 재삽입), 트리와 범위 밖 사이 이동은 모두 갱신 계수에
                // 든다.
                ++treeUpdates;
            }
            if (!wasOutside && nowOutside) {
                ++outsideInsertions;
            }
            outside = outside + (nowOutside ? 1 : 0) - (wasOutside ? 1 : 0);
        }
        if (step % 50 == 49) {
            (void)tree.Tree().RunMaintenance(MaintenanceBudget::Operations(4));
        }
    }

    const OctreeStats stats = StatsOf(tree.Tree());
    CHECK(stats.proxyCount == live && stats.proxyCount == tree.LiveCount());
    CHECK(stats.outOfRangeCount == outside);
    CHECK(stats.outOfRangeInsertions == outsideInsertions);
    CHECK(stats.inPlaceUpdates + stats.reinserts == treeUpdates);
    CHECK(stats.rootExpansions == 0 && stats.rootShrinks == 0 && stats.emptyResets == 0);
    CHECK(stats.rootLevel == config.initialRootLevel);
    CHECK(outside > 0 && treeUpdates > 0);
    CHECK(ValidateTree(tree.Tree()));
}

// FTZ·DAZ 환경에서도 불변 조건 7을 거짓으로 보고하지 않고 접촉 질의가 후보를 놓치지 않는다. 상대
// 좌표가 정확히 0이거나 float 비정규 범위인 경계는 저장 float가 비정규 값이 된다.
TEST(LooseOctree_ValidateAndQueriesUnderFlushToZero) {
    SystemAllocator allocator;
    LooseOctreeConfig config;
    config.minCellSize = 1.0;
    config.initialRootLevel = 4;
    config.maxRootLevel = 8;
    auto created = LooseOctree::TryCreate(config, allocator);
    REQUIRE(created);
    LooseOctree octree = Move(created.Value());

    const uint64 original = platform::detail::ReadFloatingPointControl();
    platform::detail::WriteFloatingPointControl(original | platform::detail::FlushToZeroBit() |
                                                platform::detail::DenormalsAreZeroBit());
    SpatialProxyDesc desc;
    desc.filterMask = 1;
    // 루트 중심이 원점이므로 min.x의 상대 좌표가 정확히 0, 그리고 float 비정규 값(2e-40)이다.
    desc.bounds = Box<double>(Vec3d(0, 0, 0), Vec3d(1, 1, 1));
    const bool firstAdded = static_cast<bool>(octree.TryAdd(desc));
    desc.bounds = Box<double>(Vec3d(2.0e-40, 0, 0), Vec3d(1, 1, 1));
    const bool secondAdded = static_cast<bool>(octree.TryAdd(desc));
    OctreeValidationReport report;
    const bool valid = ValidateTree(octree, &report);
    uint64 touching = 0;
    {
        SpatialReadScope read(octree);
        auto counted =
            read.Reader().CountInBox(Box<double>(Vec3d(-1, -1, -1), Vec3d(2.0e-40, 0, 0)), 1);
        touching = counted ? counted.Value() : 0;
    }
    platform::detail::WriteFloatingPointControl(original);

    CHECK(firstAdded && secondAdded);
    CHECK(valid && report.invariant == OctreeInvariantNone);
    CHECK(touching == 2);
}
