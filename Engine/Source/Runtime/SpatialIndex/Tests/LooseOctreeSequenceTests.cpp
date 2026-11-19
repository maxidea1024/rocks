#include "LooseOctreeTestSupport.h"

#include <Core/Memory/SystemAllocator.h>
#include <Platform/Fatal.h>

using namespace rocks;
namespace loose_octree_sequence_tests_file {

using namespace rocks::spatial;
using namespace rocks::spatial::oracle;

// LO05: 고정 시드의 삽입·이동·삭제·필터 변경·유지보수·검색 시퀀스를 정답 모형과 함께 돌리며 일정
// 간격으로 Validate()와 전수 대조를 한다(Docs/Design/SpatialIndex.ko.md 8절). 질의의 일부는 범위 밖
// 목록(±2·10^5 cm)까지 닿아 그 분기도 전수 대조한다.
//
// 기본 실행은 SequenceOperations(12만)회로 수 초 안에 끝난다. 시퀀스 상태(트리·모형·난수)는
// 프로세스 안에서 이어지므로 실행기의 --repeat N은 같은 트리에 이어서 N × 12만 회를 돌린다. 문서
// 목표(10^6회 이상)는 기본 시험 시간을 늘리지 않고 다음처럼 돌린다(시드는 --seed로 바꾼다):
//   FoundationTests --filter LooseOctree_LO05 --repeat 9            (1,080,000회, 한 시퀀스)
//   FoundationTests --filter LooseOctree_LO05 --repeat 9 --seed 7
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

// 한 프로세스 안에서 이어지는 시퀀스 상태다. 처음 쓸 때 만들고 프로세스가 끝날 때 트리·모형을 먼저
// 없앤 뒤 allocator를 없앤다(멤버 역순 소멸).
struct SequenceSession {
    SystemAllocator allocator;
    LooseOctreeConfig config;
    OracleTree tree;
    RandomStream random;
    uint64 operations = 0;

    explicit SequenceSession(bool largeProxyList) noexcept
        : config(SessionConfig(largeProxyList)),
          tree(CreateTree(config, allocator), allocator, true),
          random((largeProxyList ? 0x1A26E0u : 0x105EEDu) ^ rocks::test::CurrentSeed()) {
    }

    // largeProxyList이면 큰 객체 목록을 켠다. 기준 반 변 4는 MovedBox의 크기 변경(반 변 0–6)이
    // 기준(4)과 이력 경계(3)를 자주 넘나들게 한다.
    static LooseOctreeConfig SessionConfig(bool largeProxyList) noexcept {
        LooseOctreeConfig session = SmallConfig();
        session.maxRootLevel = 14;
        session.pendingLimitFloor = 16;
        session.largeProxyHalfExtent = largeProxyList ? 4.0 : 0.0;
        return session;
    }

    static LooseOctree CreateTree(const LooseOctreeConfig& session, Allocator& backing) noexcept {
        auto created = LooseOctree::TryCreate(session, backing);
        if (!created) {
            platform::Fatal("LO05 sequence tree creation failed");
        }
        return Move(created.Value());
    }
};

SequenceSession& Session() noexcept {
    static SequenceSession session(false);
    return session;
}

SequenceSession& LargeProxySession() noexcept {
    static SequenceSession session(true);
    return session;
}

// 질의 box다. 다섯 번에 한 번은 범위 밖 프록시가 있는 ±2·10^5 cm 근처를 본다.
Box<double> NextQuery(RandomStream& random, uint64 query) noexcept {
    if (query % 5 != 4) {
        return MakeQuery(random, 80.0);
    }
    const double far = random.NextUInt32Below(2) == 0 ? 2.0e5 : -2.0e5;
    const Vec3d center(far + Uniform(random, -50, 50), Uniform(random, -20, 20),
                       Uniform(random, -20, 20));
    const Vec3d half(Uniform(random, 0, 60), Uniform(random, 0, 30), Uniform(random, 0, 30));
    return Box<double>(center - half, center + half);
}

void RunSequence(SequenceSession& session, uint32 operations) noexcept {
    OracleTree& tree = session.tree;
    RandomStream& random = session.random;
    const LooseOctreeConfig& config = session.config;
    Allocator& allocator = session.allocator;

    uint32 violations = 0;
    uint32 queryFailures = 0;
    uint64 queries = 0;
    uint64 farExact = 0;
    for (uint32 iteration = 0; iteration < operations; ++iteration) {
        const uint32 step = static_cast<uint32>(session.operations++ % 0xFFFFFFu);
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

        if (iteration % QueryInterval == 0) {
            const double tolerance = ToleranceFor(tree.Tree(), config);
            SpatialReadScope read(tree.Tree());
            const Box<double> box = NextQuery(random, queries);
            const QueryCheck check =
                CheckBoxQuery(read.Reader(), tree.Proxies(), box, 1u + random.NextUInt32Below(15),
                              tolerance, allocator);
            queryFailures += check.ok ? 0u : 1u;
            farExact += queries % 5 == 4 ? check.exact : 0;
            ++queries;
        }
        if (iteration % ValidateInterval == ValidateInterval - 1) {
            OctreeValidationReport report;
            if (!ValidateTree(tree.Tree(), &report) ||
                !LogicalStateMatches(tree.Tree(), tree.Proxies())) {
                ++violations;
            }
        }
    }
    CHECK(violations == 0);
    CHECK(queryFailures == 0);
    CHECK(queries >= operations / QueryInterval);
    // 먼 질의가 실제로 범위 밖 프록시를 후보로 냈다.
    CHECK(farExact > 0);

    // 끝에 유지보수를 모두 처리해도 결과가 같다.
    (void)tree.Tree().RunMaintenance(MaintenanceBudget::All());
    CHECK(ValidateTree(tree.Tree()));
    const double tolerance = ToleranceFor(tree.Tree(), config);
    SpatialReadScope read(tree.Tree());
    for (uint32 query = 0; query < 50; ++query) {
        const Box<double> box = NextQuery(random, query);
        CHECK(CheckBoxQuery(read.Reader(), tree.Proxies(), box, 0xFu, tolerance, allocator).ok);
    }
    const OctreeStats stats = read.Reader().Stats();
    // 시퀀스가 실제로 각 경로를 지났는지 확인한다.
    CHECK(stats.splits > 0 && stats.merges + stats.reclaims > 0);
    CHECK(stats.inPlaceUpdates > 0 && stats.reinserts > 0 && stats.rootExpansions > 0);
    CHECK(stats.outOfRangeInsertions > 0);
    CHECK(stats.proxyCount == tree.LiveCount());
    if (config.largeProxyHalfExtent > 0.0) {
        // 큰 객체 목록을 오가는 경로를 지났다.
        CHECK(stats.largeProxyInsertions > 0);
    } else {
        CHECK(stats.largeProxyInsertions == 0 && stats.largeProxyCount == 0);
    }
}

} // namespace

TEST(LooseOctree_LO05_FixedSeedSequenceMatchesOracle) {
    RunSequence(Session(), SequenceOperations);
}

// 결정 8: 큰 객체 목록을 켠 같은 시퀀스다. 크기 변경이 기준 반 변(4)과 이력 경계(3)를 넘나들어
// 트리·범위 밖 목록·큰 객체 목록 사이의 이동을 모두 지난다. 시간 때문에 기본 시퀀스의 1/3이다.
TEST(LooseOctree_LO05_LargeProxyListSequenceMatchesOracle) {
    RunSequence(LargeProxySession(), SequenceOperations / 3);
}
} // namespace loose_octree_sequence_tests_file
