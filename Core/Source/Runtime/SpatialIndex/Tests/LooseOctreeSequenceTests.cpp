#include "LooseOctreeTestSupport.h"

#include <Core/Memory/SystemAllocator.h>

using namespace rocks;
using namespace rocks::spatial;
using namespace rocks::spatial::oracle;

// LO05: 고정 시드의 삽입·이동·삭제·필터 변경·유지보수·검색 시퀀스를 정답 모형과 함께 돌리며 일정
// 간격으로 Validate()와 전수 대조를 한다(Docs/Design/SpatialIndex.ko.md 8절).
//
// 연산 수는 기본 실행이 수 초 안에 끝나도록 SequenceOperations로 조절한다. 문서의 목표(10^6회
// 이상)는 이 상수를 올려 별도로 돌린다. 시드는 실행기의 --seed와 섞으므로 다른 시드로도 반복할 수
// 있다.
namespace {

constexpr uint32 SequenceOperations = 120000;
constexpr uint32 ValidateInterval = 4000;
constexpr uint32 QueryInterval = 97;

// 분포를 바꿔 가며 프록시를 만든다. 일부는 범위 밖 목록에 들어갈 만큼 멀거나 크다.
Box<double> NextBox(RandomStream& random, uint32 step) noexcept {
    const uint32 phase = (step / 20000) % 8;
    if (random.NextUInt32Below(200) == 0) {
        const double far = random.NextUInt32Below(2) == 0 ? 2.0e5 : -2.0e5;
        const Vec3d center(far, Uniform(random, -10, 10), Uniform(random, -10, 10));
        return Box<double>(center - Vec3d::Splat(1.0), center + Vec3d::Splat(1.0));
    }
    return MakeBox(AllDistributions[phase], random, 80.0);
}

// 작은 이동(제자리 갱신이 많다), 경계 통과, 순간 이동, 크기 변경을 섞는다.
Box<double> MovedBox(RandomStream& random, const Box<double>& current, uint32 step) noexcept {
    const uint32 kind = random.NextUInt32Below(4);
    if (kind == 0) {
        const Vec3d delta(Uniform(random, -0.5, 0.5), Uniform(random, -0.5, 0.5),
                          Uniform(random, -0.5, 0.5));
        return Box<double>(current.min + delta, current.max + delta);
    }
    if (kind == 1) {
        const Vec3d delta(Uniform(random, -8, 8), Uniform(random, -8, 8), Uniform(random, -8, 8));
        return Box<double>(current.min + delta, current.max + delta);
    }
    if (kind == 2) {
        return NextBox(random, step);
    }
    const Vec3d center = current.Center();
    const double half = Uniform(random, 0.0, 6.0);
    return Box<double>(center - Vec3d::Splat(half), center + Vec3d::Splat(half));
}

} // namespace

TEST(LooseOctree_LO05_FixedSeedSequenceMatchesOracle) {
    SystemAllocator allocator;
    LooseOctreeConfig config = SmallConfig();
    config.maxRootLevel = 14;
    config.pendingLimitFloor = 16;
    auto created = LooseOctree::TryCreate(config, allocator);
    REQUIRE(created);
    OracleTree tree(Move(created.Value()), allocator);
    RandomStream random(0x105EEDu ^ rocks::test::CurrentSeed());

    uint32 violations = 0;
    uint32 queryFailures = 0;
    uint64 queries = 0;
    for (uint32 step = 0; step < SequenceOperations; ++step) {
        const uint32 action = random.NextUInt32Below(100);
        // 살아 있는 수가 대략 1500 근처를 오가도록 삽입·삭제 비율을 바꾼다.
        const uint32 addBelow = tree.LiveCount() < 1500 ? 40 : 25;
        if (action < addBelow || tree.LiveCount() == 0) {
            if (tree.Add(NextBox(random, step), 1u << random.NextUInt32Below(4)) == IndexNone) {
                ++violations;
            }
        } else if (action < 50) {
            if (!tree.Remove(tree.PickLive(random))) {
                ++violations;
            }
        } else if (action < 92) {
            const usize index = tree.PickLive(random);
            if (!tree.Update(index, MovedBox(random, tree.Proxies()[index].bounds, step))) {
                ++violations;
            }
        } else if (action < 95) {
            if (!tree.SetFilter(tree.PickLive(random), random.NextUInt32Below(16))) {
                ++violations;
            }
        } else {
            (void)tree.Tree().RunMaintenance(
                MaintenanceBudget::Operations(random.NextUInt32Below(16)));
        }

        if (step % QueryInterval == 0) {
            const double tolerance = ToleranceFor(tree.Tree(), config);
            SpatialReadScope read(tree.Tree());
            const Box<double> box = MakeQuery(random, 80.0);
            const QueryCheck check =
                CheckBoxQuery(read.Reader(), tree.Proxies(), box, 1u + random.NextUInt32Below(15),
                              tolerance, allocator);
            queryFailures += check.ok ? 0u : 1u;
            ++queries;
        }
        if (step % ValidateInterval == ValidateInterval - 1) {
            ValidationFailure failure;
            if (!ValidateTree(tree.Tree(), &failure) ||
                !LogicalStateMatches(tree.Tree(), tree.Proxies())) {
                ++violations;
            }
        }
    }
    CHECK(violations == 0);
    CHECK(queryFailures == 0);
    CHECK(queries > 1000);

    // 끝에 유지보수를 모두 처리해도 결과가 같다.
    (void)tree.Tree().RunMaintenance(MaintenanceBudget::All());
    CHECK(ValidateTree(tree.Tree()));
    const double tolerance = ToleranceFor(tree.Tree(), config);
    SpatialReadScope read(tree.Tree());
    for (uint32 query = 0; query < 50; ++query) {
        const Box<double> box = MakeQuery(random, 80.0);
        CHECK(CheckBoxQuery(read.Reader(), tree.Proxies(), box, 0xFu, tolerance, allocator).ok);
    }
    const OctreeStats stats = read.Reader().Stats();
    // 시퀀스가 실제로 각 경로를 지났는지 확인한다.
    CHECK(stats.splits > 0 && stats.merges + stats.reclaims > 0);
    CHECK(stats.inPlaceUpdates > 0 && stats.reinserts > 0 && stats.rootExpansions > 0);
    CHECK(stats.outOfRangeInsertions > 0);
}
