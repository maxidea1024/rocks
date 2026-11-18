#include "LooseOctreeTestSupport.h"

#include <Core/Memory/SystemAllocator.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Platform/PlatformMath.h>
#include <Platform/Thread.h>

using namespace rocks;
using namespace rocks::spatial;
using namespace rocks::spatial::oracle;

// LooseOctree OT3 시험이다(Docs/Design/SpatialIndex.ko.md 8절 LO01·LO02·LO04·LO12·LO13·LO14와 노드
// 묶음): 구·볼록 영역(프러스텀)·ray 질의, 최근접 ray, 노드 묶음. 정답은 전수 검색이며 형상 술어는
// Core(Intersection.h)의 Overlap·Raycast를 정확한 double AABB에 적용한다.

namespace {

Box<double> MakeCube(const Vec3d& center, double half) noexcept {
    return Box<double>(center - Vec3d::Splat(half), center + Vec3d::Splat(half));
}

double Infinity() noexcept {
    return platform::PlatformMath::Infinity<double>();
}

double NaN() noexcept {
    return platform::PlatformMath::QuietNaN<double>();
}

// 분포 하나로 채우고 일부를 지우고 옮긴 트리다.
void Populate(OracleTree& tree, Distribution distribution, RandomStream& random, double extent,
              uint32 count) noexcept {
    for (uint32 index = 0; index < count; ++index) {
        (void)tree.Add(MakeBox(distribution, random, extent), 1u << random.NextUInt32Below(4));
    }
    for (uint32 index = 0; index < count / 8; ++index) {
        (void)tree.Remove(tree.PickLive(random));
        const usize moved = tree.PickLive(random);
        (void)tree.Update(moved, MakeBox(distribution, random, extent));
    }
}

ShapeQuery RayQuery(const Vec3d& origin, const Vec3d& direction, double maxDistance) noexcept {
    ShapeQuery query;
    query.kind = ShapeKind::Ray;
    auto ray = Ray<double>::TryMake(origin, direction);
    if (ray) {
        query.ray = ray.Value();
    }
    query.maxDistance = maxDistance;
    return query;
}

} // namespace

// LO01(전체): 8개 분포 × 박스·구·볼록 영역·ray, 필터 조합과 mask 0. 구·볼록 영역·ray는 후보가 정답
// 술어와 정확히 같다(누락·초과 0건).
TEST(LooseOctree_LO01_AllShapesMatchExhaustiveSearch) {
    SystemAllocator allocator;
    const LooseOctreeConfig config = SmallConfig();
    uint64 exact[4] = {};
    for (const Distribution distribution : AllDistributions) {
        RandomStream random(0x5EED0101u + static_cast<uint32>(distribution));
        auto created = LooseOctree::TryCreate(config, allocator);
        REQUIRE(created);
        OracleTree tree(Move(created.Value()), allocator);
        Populate(tree, distribution, random, 200.0, 1200);
        REQUIRE(ValidateTree(tree.Tree()));
        const double tolerance = ToleranceFor(tree.Tree(), config);
        SpatialReadScope read(tree.Tree());
        for (uint32 query = 0; query < 160; ++query) {
            const ShapeKind kind = AllShapeKinds[query % 4];
            const ShapeQuery shape = MakeShapeQuery(kind, random, 200.0);
            const uint32 mask = query % 13 == 0 ? 0u : 1u + random.NextUInt32Below(15);
            const QueryCheck check =
                CheckShapeQuery(read.Reader(), tree.Proxies(), shape, mask, tolerance, allocator);
            CHECK(check.ok);
            if (mask == 0) {
                CHECK(check.results == 0);
            }
            exact[static_cast<uint32>(kind)] += check.exact;
        }
    }
    // ray는 가늘어 후보가 적다.
    CHECK(exact[0] > 500 && exact[1] > 500 && exact[2] > 500 && exact[3] > 20);
}

// LO02: 보수성 한계. 박스의 초과 후보는 루트 level의 float 상대 경계 폭(δ, ToleranceFor) 안이고,
// 구·볼록 영역·ray는 δ = 0(정확한 double 술어)이다. 원점에서 10^9 cm 떨어진 곳에서도 같다.
TEST(LooseOctree_LO02_ConservativenessBoundsPerShape) {
    SystemAllocator allocator;
    LooseOctreeConfig config;
    config.parentWarnCount = 100000;
    const Vec3d centers[] = {Vec3d(0, 0, 0), Vec3d(1.0e9, -1.0e9, 1.0e9)};
    for (const Vec3d& center : centers) {
        auto created = LooseOctree::TryCreate(config, allocator);
        REQUIRE(created);
        OracleTree tree(Move(created.Value()), allocator);
        RandomStream random(0x10200u);
        // 원점 근처에 하나를 고정해 두면 먼 곳이 루트 확장으로 들어간다(빈 트리 재고정 없음).
        REQUIRE(tree.Add(MakeCube(Vec3d(0, 0, 0), 10), 1) != IndexNone);
        for (uint32 index = 0; index < 600; ++index) {
            const Vec3d offset(Uniform(random, -5000, 5000), Uniform(random, -5000, 5000),
                               Uniform(random, -5000, 5000));
            REQUIRE(tree.Add(MakeCube(center + offset, Uniform(random, 0, 200)), 1) != IndexNone);
        }
        const double tolerance = ToleranceFor(tree.Tree(), config);
        SpatialReadScope read(tree.Tree());
        uint64 boxExtra = 0;
        for (uint32 query = 0; query < 120; ++query) {
            const ShapeKind kind = AllShapeKinds[query % 4];
            const ShapeQuery shape = MakeShapeQuery(kind, random, 5000.0, center);
            const QueryCheck check =
                CheckShapeQuery(read.Reader(), tree.Proxies(), shape, 1, tolerance, allocator);
            CHECK(check.ok);
            if (kind == ShapeKind::Box) {
                boxExtra += check.extra;
            } else {
                CHECK(check.extra == 0);
            }
        }
        // 박스의 초과 후보 수 자체는 보고만 한다(0이어도 된다).
        (void)boxExtra;
    }
}

// LO04(OT3): 잘못된 질의 형상은 InvalidArgument이며 아무것도 방문하지 않는다. maxDistance는 +Inf를
// 허용하고 음수·NaN은 거부한다.
TEST(LooseOctree_LO04_InvalidShapeQueries) {
    SystemAllocator allocator;
    auto created = LooseOctree::TryCreate(SmallConfig(), allocator);
    REQUIRE(created);
    OracleTree tree(Move(created.Value()), allocator);
    REQUIRE(tree.Add(MakeCube(Vec3d(1, 1, 1), 1), 1) != IndexNone);
    SpatialReadScope read(tree.Tree());
    const LooseOctreeReader reader = read.Reader();
    uint32 calls = 0;
    const auto visitor = [&](const SpatialProxyHit&) noexcept {
        ++calls;
        return QueryControl::Continue;
    };
    const Sphere<double> badSpheres[] = {Sphere<double>(Vec3d(0, 0, 0), -1.0),
                                         Sphere<double>(Vec3d(NaN(), 0, 0), 1.0),
                                         Sphere<double>(Vec3d(0, 0, 0), Infinity())};
    for (const Sphere<double>& sphere : badSpheres) {
        CHECK(reader.QuerySphere(sphere, 1, visitor).Error() == ErrorCode::InvalidArgument);
        CHECK(reader.CountInSphere(sphere, 1).Error() == ErrorCode::InvalidArgument);
        CHECK(reader.AnyInSphere(sphere, 1).Error() == ErrorCode::InvalidArgument);
        CHECK(reader.QuerySphereInto(sphere, 1, Span<SpatialProxyHit>()).Error() ==
              ErrorCode::InvalidArgument);
    }
    const ConvexVolume<double> empty;
    ConvexVolume<double> badPlane;
    badPlane.planeCount = 1;
    badPlane.planes[0] = Plane<double>(Vec3d(0, 0, 3), 0);
    const ConvexVolume<double>* badVolumes[] = {&empty, &badPlane};
    for (const ConvexVolume<double>* volume : badVolumes) {
        CHECK(reader.QueryFrustum(*volume, 1, visitor).Error() == ErrorCode::InvalidArgument);
        CHECK(reader.CountInFrustum(*volume, 1).Error() == ErrorCode::InvalidArgument);
        SpatialNodeBatch batch;
        CHECK(reader.CollectNodeBatches(*volume, Span<SpatialNodeBatch>(&batch, 1)).Error() ==
              ErrorCode::InvalidArgument);
    }
    const Ray<double> notUnit(Vec3d(0, 0, 0), Vec3d(0, 0, 2));
    const Ray<double> good(Vec3d(-10, 1, 1), Vec3d(1, 0, 0));
    CHECK(reader.QueryRay(notUnit, 100.0, 1, visitor).Error() == ErrorCode::InvalidArgument);
    CHECK(reader.QueryRay(good, -1.0, 1, visitor).Error() == ErrorCode::InvalidArgument);
    CHECK(reader.QueryRay(good, NaN(), 1, visitor).Error() == ErrorCode::InvalidArgument);
    CHECK(reader.CountOnRay(good, NaN(), 1).Error() == ErrorCode::InvalidArgument);
    CHECK(
        reader
            .QueryRayNearest(good, -1.0, 1,
                             [](const SpatialProxyHit&, double) noexcept { return RayHitResult{}; })
            .Error() == ErrorCode::InvalidArgument);
    CHECK(calls == 0);
    // +Inf는 허용한다.
    auto infinite = reader.CountOnRay(good, Infinity(), 1);
    REQUIRE(infinite);
    CHECK(infinite.Value() == 1);
}

// LO12(전 형상): Stop은 즉시 끝나고, Into는 용량이 모자라도 끝까지 센다. 범위 밖 목록 항목도 같다.
TEST(LooseOctree_LO12_StopAndBuffersForAllShapes) {
    SystemAllocator allocator;
    auto created = LooseOctree::TryCreate(SmallConfig(), allocator);
    REQUIRE(created);
    OracleTree tree(Move(created.Value()), allocator);
    for (uint32 index = 0; index < 60; ++index) {
        const double x = -30.0 + index;
        REQUIRE(tree.Add(MakeCube(Vec3d(x, 0.25, -0.25), 0.4), 1) != IndexNone);
    }
    // 범위 밖 목록(확장 한도 2^16 cm보다 큰 box)도 모든 질의에 걸린다.
    REQUIRE(tree.Add(MakeCube(Vec3d(0, 0, 0), 1.0e6), 1) != IndexNone);
    SpatialReadScope read(tree.Tree());
    const LooseOctreeReader reader = read.Reader();
    ShapeQuery shapes[4];
    shapes[0].kind = ShapeKind::Box;
    shapes[0].box = Box<double>(Vec3d(-40, -1, -1), Vec3d(40, 1, 1));
    shapes[1].kind = ShapeKind::Sphere;
    shapes[1].sphere = Sphere<double>(Vec3d(0, 0, 0), 40.0);
    shapes[2].kind = ShapeKind::Volume;
    const Plane<double> slab[] = {Plane<double>(Vec3d(0, 1, 0), -1),
                                  Plane<double>(Vec3d(0, -1, 0), -1)};
    auto volume = ConvexVolume<double>::TryMake(slab);
    REQUIRE(volume);
    shapes[2].volume = volume.Value();
    shapes[3] = RayQuery(Vec3d(-100, 0, 0), Vec3d(1, 0, 0), Infinity());
    for (const ShapeQuery& shape : shapes) {
        uint32 calls = 0;
        auto stopped = RunShape(reader, shape, 1, [&](const SpatialProxyHit&) noexcept {
            ++calls;
            return calls == 5 ? QueryControl::Stop : QueryControl::Continue;
        });
        REQUIRE(stopped);
        CHECK(stopped.Value().stopped && stopped.Value().visited == 5 && calls == 5);
        SpatialProxyHit buffer[8];
        auto into = IntoShape(reader, shape, 1, Span<SpatialProxyHit>(buffer, 8));
        REQUIRE(into);
        CHECK(into.Value().written == 8 && into.Value().total == 61 && into.Value().truncated);
        auto count = CountShape(reader, shape, 1);
        auto any = AnyShape(reader, shape, 1);
        auto none = AnyShape(reader, shape, 2);
        REQUIRE(count && any && none);
        CHECK(count.Value() == 61 && any.Value() && !none.Value());
    }
}

// LO13: 최근접 ray. hitTest 거리 = AABB 진입 거리 + 프록시마다 정한 0 이상의 덧셈(동률 포함)이고
// 일부는 빗나간다. 전수 최근접과 거리가 같아야 하고, 빗나가는 가까운 AABB가 최단을 줄이지 않아야
// 하며, 가지치기로 hitTest 호출이 후보보다 적어야 한다.
TEST(LooseOctree_LO13_NearestRayMatchesExhaustiveSearch) {
    SystemAllocator allocator;
    auto created = LooseOctree::TryCreate(SmallConfig(), allocator);
    REQUIRE(created);
    OracleTree tree(Move(created.Value()), allocator);
    RandomStream random(0x1313u);
    Populate(tree, Distribution::Uniform, random, 100.0, 1500);
    // 근접 동률: 같은 AABB 여러 개(덧셈도 같으면 거리가 정확히 같다).
    for (uint32 index = 0; index < 20; ++index) {
        REQUIRE(tree.Add(MakeCube(Vec3d(5, 5, 5), 2), 1) != IndexNone);
    }
    // 줄지어 선 상자 101개(x = −100 … 100, 필터 0x10)다.
    for (int32 index = 0; index <= 100; ++index) {
        REQUIRE(tree.Add(MakeCube(Vec3d(-100.0 + 2.0 * index, 150, 150), 0.5), 0x10) != IndexNone);
    }
    // 프록시마다 덧셈과 빗나감을 userValue로 정한다(같은 상자 20개는 덧셈 0으로 동률).
    const auto extra = [](uint64 userValue) noexcept {
        return userValue >= 1500 ? 0.0 : static_cast<double>(userValue % 7) * 0.5;
    };
    const auto misses = [](uint64 userValue) noexcept {
        return userValue < 1500 && userValue % 5 == 0;
    };
    SpatialReadScope read(tree.Tree());
    const LooseOctreeReader reader = read.Reader();
    uint64 hitTests = 0;
    uint64 candidates = 0;
    for (uint32 query = 0; query < 150; ++query) {
        ShapeQuery shape = MakeShapeQuery(ShapeKind::Ray, random, 100.0);
        if (query % 10 == 0) {
            shape = RayQuery(Vec3d(5, 5, -200), Vec3d(0, 0, 1), Infinity());
        }
        const auto hitTest = [&](const SpatialProxyHit& hit, double currentMax) noexcept {
            const OracleProxy& proxy = tree.Proxies()[hit.userValue];
            const RayHit<double> aabb = Raycast(shape.ray, proxy.bounds, shape.maxDistance);
            RayHitResult result;
            result.hit = aabb.IsHit() && !misses(hit.userValue);
            result.distance = aabb.distance + extra(hit.userValue);
            (void)currentMax;
            return result;
        };
        QueryStats stats;
        auto nearest = reader.QueryRayNearest(shape.ray, shape.maxDistance, 0xF, hitTest, &stats);
        REQUIRE(nearest);
        // 전수 최근접.
        bool found = false;
        double best = 0.0;
        for (usize index = 0; index < tree.Proxies().Size(); ++index) {
            const OracleProxy& proxy = tree.Proxies()[index];
            if (!proxy.live || (proxy.filter & 0xFu) == 0 || !ExactHit(shape, proxy.bounds) ||
                misses(index)) {
                continue;
            }
            const double distance =
                Raycast(shape.ray, proxy.bounds, shape.maxDistance).distance + extra(index);
            if (distance <= shape.maxDistance && (!found || distance < best)) {
                found = true;
                best = distance;
            }
        }
        CHECK(nearest.Value().found == found);
        if (found && nearest.Value().found) {
            CHECK(nearest.Value().distance == best);
            const uint64 chosen = nearest.Value().proxy.userValue;
            CHECK(!misses(chosen));
            CHECK(Raycast(shape.ray, tree.Proxies()[chosen].bounds, shape.maxDistance).distance +
                      extra(chosen) ==
                  best);
        }
        hitTests += stats.results;
        auto counted = reader.CountOnRay(shape.ray, shape.maxDistance, 0xF);
        REQUIRE(counted);
        candidates += counted.Value();
    }
    CHECK(hitTests <= candidates);

    // 가까운 쪽부터 방문하고 진입 거리로 가지를 치므로, 줄지어 선 상자 101개를 꿰는 ray는 hitTest를
    // 몇 번만 부른다(앞쪽 상자가 최단을 정한 뒤 뒤쪽 노드는 진입 거리가 더 커서 버려진다).
    const Ray<double> line(Vec3d(-140, 150, 150), Vec3d(1, 0, 0));
    QueryStats lineStats;
    auto lineNearest = reader.QueryRayNearest(
        line, Infinity(), 0x10,
        [&](const SpatialProxyHit& hit, double) noexcept {
            const RayHit<double> aabb =
                Raycast(line, tree.Proxies()[hit.userValue].bounds, Infinity());
            return RayHitResult{aabb.IsHit(), aabb.distance};
        },
        &lineStats);
    REQUIRE(lineNearest && lineNearest.Value().found);
    CHECK(lineNearest.Value().distance == 39.5);
    auto lineCount = reader.CountOnRay(line, Infinity(), 0x10);
    REQUIRE(lineCount);
    CHECK(lineCount.Value() == 101 && lineStats.results * 4 < lineCount.Value());
}

// LO14: ray 동률 방문 순서. 원점이 모든 자식의 확장 경계 안이면 진입 거리가 모두 0이라 자식 번호
// 오름차순이고, 바깥에서 들어오면 진입 거리 오름차순(같으면 번호 오름차순)이다. visitor 안 재검색도
// 허용된다.
TEST(LooseOctree_LO14_RayVisitOrderAndNestedQueries) {
    SystemAllocator allocator;
    auto created = LooseOctree::TryCreate(SmallConfig(), allocator);
    REQUIRE(created);
    OracleTree tree(Move(created.Value()), allocator);
    // 자식 i(비트 0 = x, 1 = y, 2 = z가 양)에 0 축을 넘는 box를 하나씩 둔다. userValue가 자식
    // 번호다.
    for (uint32 child = 0; child < 8; ++child) {
        const Vec3d center((child & 1u) != 0 ? 8.0 : -8.0, (child & 2u) != 0 ? 8.0 : -8.0,
                           (child & 4u) != 0 ? 8.0 : -8.0);
        REQUIRE(tree.Add(MakeCube(center, 8.5), 1) == child);
    }
    REQUIRE(tree.Add(MakeCube(Vec3d(20, 20, 20), 0.1), 2) != IndexNone);
    REQUIRE(StatsOf(tree.Tree()).splits == 1);

    SpatialReadScope read(tree.Tree());
    const LooseOctreeReader reader = read.Reader();
    const auto order = [&](const Ray<double>& ray, uint64(&visited)[8]) noexcept {
        uint32 count = 0;
        auto queried =
            reader.QueryRay(ray, Infinity(), 1, [&](const SpatialProxyHit& hit) noexcept {
                if (count < 8) {
                    visited[count] = hit.userValue;
                }
                ++count;
                return QueryControl::Continue;
            });
        return queried && count == 8;
    };
    uint64 visited[8] = {};
    REQUIRE(order(Ray<double>(Vec3d(0, 0, 0), Vec3d(1, 0, 0)), visited));
    for (uint32 index = 0; index < 8; ++index) {
        CHECK(visited[index] == index);
    }
    // −x 쪽 자식(확장 경계 x 최솟값 −48)이 +x 쪽(−16)보다 먼저 닿는다.
    REQUIRE(order(Ray<double>(Vec3d(-100, 0, 0), Vec3d(1, 0, 0)), visited));
    const uint64 expected[8] = {0, 2, 4, 6, 1, 3, 5, 7};
    for (uint32 index = 0; index < 8; ++index) {
        CHECK(visited[index] == expected[index]);
    }

    // visitor 안에서 같은 트리를 다른 형상으로 다시 검색한다.
    uint64 nested = 0;
    auto outer = reader.QueryRay(Ray<double>(Vec3d(0, 0, 0), Vec3d(0, 0, 1)), 100.0, 1,
                                 [&](const SpatialProxyHit&) noexcept {
                                     auto inner = reader.CountInSphere(
                                         Sphere<double>(Vec3d(20, 20, 20), 1.0), 3);
                                     nested += inner ? inner.Value() : 100;
                                     return QueryControl::Continue;
                                 });
    REQUIRE(outer);
    CHECK(outer.Value().visited == 8 && nested == 8);
}

namespace {

struct BatchWork {
    const LooseOctreeReader* reader = nullptr;
    const SpatialNodeBatch* batches = nullptr;
    usize first = 0;
    usize last = 0;
    const ConvexVolume<double>* volume = nullptr;
    uint64 results[4096] = {};
    usize count = 0;
    bool ok = true;
};

void ProcessBatches(void* context) noexcept {
    BatchWork& work = *static_cast<BatchWork*>(context);
    for (usize index = work.first; index < work.last; ++index) {
        auto queried = work.reader->QueryBatch(work.batches[index], *work.volume, 0xF,
                                               [&](const SpatialProxyHit& hit) noexcept {
                                                   if (work.count < 4096) {
                                                       work.results[work.count++] = hit.userValue;
                                                   } else {
                                                       work.ok = false;
                                                   }
                                                   return QueryControl::Continue;
                                               });
        work.ok = work.ok && static_cast<bool>(queried);
    }
}

} // namespace

// 노드 묶음: 묶음별 결과를 순서대로 이으면 단일 질의와 같고(중복 없음), 완전 포함 묶음은 공간
// 판정을 생략한다. 여러 스레드로 나눠 처리해도 합집합이 같다(스레드는 Join으로 끝을 기다린다). 변경
// 뒤에는 낡은 묶음, 다른 종류의 형상은 오류다.
TEST(LooseOctree_NodeBatchesMatchSingleQuery) {
    SystemAllocator allocator;
    auto created = LooseOctree::TryCreate(SmallConfig(), allocator);
    REQUIRE(created);
    OracleTree tree(Move(created.Value()), allocator);
    RandomStream random(0xBA7Cu);
    Populate(tree, Distribution::Uniform, random, 100.0, 2000);
    REQUIRE(tree.Add(MakeCube(Vec3d(0, 0, 0), 1.0e6), 1) != IndexNone);

    const Plane<double> planes[] = {
        Plane<double>(Vec3d(1, 0, 0), -60), Plane<double>(Vec3d(-1, 0, 0), -60),
        Plane<double>(Vec3d(0, 1, 0), -60), Plane<double>(Vec3d(0, -1, 0), -60),
        Plane<double>(Vec3d(0, 0, 1), -60), Plane<double>(Vec3d(0, 0, -1), -10),
    };
    auto volume = ConvexVolume<double>::TryMake(planes);
    REQUIRE(volume);
    {
        SpatialReadScope read(tree.Tree());
        const LooseOctreeReader reader = read.Reader();
        SpatialNodeBatch batches[1024];
        auto collected = reader.CollectNodeBatches(volume.Value(), Span<SpatialNodeBatch>(batches));
        REQUIRE(collected && !collected.Value().truncated);
        const usize batchCount = collected.Value().written;
        REQUIRE(batchCount > 8);

        Array<uint64> single(allocator);
        QueryStats singleStats;
        REQUIRE(reader.QueryFrustum(
            volume.Value(), 0xF,
            [&](const SpatialProxyHit& hit) noexcept {
                (void)single.TryPushBack(hit.userValue);
                return QueryControl::Continue;
            },
            &singleStats));
        CHECK(singleStats.containedProxies > 0);

        Array<uint64> joined(allocator);
        uint32 containedBatches = 0;
        QueryStats batchStats;
        for (usize index = 0; index < batchCount; ++index) {
            containedBatches += batches[index].contained ? 1u : 0u;
            REQUIRE(reader.QueryBatch(
                batches[index], volume.Value(), 0xF,
                [&](const SpatialProxyHit& hit) noexcept {
                    (void)joined.TryPushBack(hit.userValue);
                    return QueryControl::Continue;
                },
                &batchStats));
        }
        CHECK(containedBatches > 0 && batchStats.containedProxies == singleStats.containedProxies);
        REQUIRE(joined.Size() == single.Size());
        for (usize index = 0; index < single.Size(); ++index) {
            CHECK(joined[index] == single[index]);
        }

        // 세 스레드로 나눠 처리한다. 결과는 스레드별 배열에 모으고 Join 뒤에 합친다.
        BatchWork work[3];
        platform::Thread threads[3];
        for (usize part = 0; part < 3; ++part) {
            work[part].reader = &reader;
            work[part].batches = batches;
            work[part].first = batchCount * part / 3;
            work[part].last = batchCount * (part + 1) / 3;
            work[part].volume = &volume.Value();
            REQUIRE(threads[part].Start(&ProcessBatches, &work[part]) ==
                    platform::ThreadStatus::Success);
        }
        for (platform::Thread& thread : threads) {
            REQUIRE(thread.Join() == platform::ThreadStatus::Success);
        }
        Array<uint8> seen(allocator);
        REQUIRE(seen.TryResize(tree.Proxies().Size()));
        usize total = 0;
        bool unique = true;
        for (const BatchWork& part : work) {
            CHECK(part.ok);
            for (usize index = 0; index < part.count; ++index) {
                unique = unique && seen[part.results[index]] == 0;
                seen[part.results[index]] = 1;
                ++total;
            }
        }
        CHECK(unique && total == single.Size());
        for (const uint64 value : single) {
            CHECK(seen[value] == 1);
        }

        // 박스·구 묶음도 같은 성질이다.
        const Box<double> box(Vec3d(-50, -20, -40), Vec3d(30, 60, 10));
        const Sphere<double> sphere(Vec3d(10, -10, 5), 45.0);
        auto boxBatches = reader.CollectNodeBatches(box, Span<SpatialNodeBatch>(batches));
        REQUIRE(boxBatches);
        uint64 boxTotal = 0;
        for (usize index = 0; index < boxBatches.Value().written; ++index) {
            auto queried =
                reader.QueryBatch(batches[index], box, 0xF, [&](const SpatialProxyHit&) noexcept {
                    ++boxTotal;
                    return QueryControl::Continue;
                });
            REQUIRE(queried);
        }
        CHECK(boxTotal == reader.CountInBox(box, 0xF).Value());
        // 종류가 다른 형상으로 부르면 오류다.
        CHECK(
            reader
                .QueryBatch(batches[0], sphere, 0xF,
                            [](const SpatialProxyHit&) noexcept { return QueryControl::Continue; })
                .Error() == ErrorCode::InvalidArgument);
        auto sphereBatches = reader.CollectNodeBatches(sphere, Span<SpatialNodeBatch>(batches));
        REQUIRE(sphereBatches);
        uint64 sphereTotal = 0;
        for (usize index = 0; index < sphereBatches.Value().written; ++index) {
            REQUIRE(reader.QueryBatch(batches[index], sphere, 0xF,
                                      [&](const SpatialProxyHit&) noexcept {
                                          ++sphereTotal;
                                          return QueryControl::Continue;
                                      }));
        }
        CHECK(sphereTotal == reader.CountInSphere(sphere, 0xF).Value());
    }

    // 모은 뒤 변경이 있으면 낡은 묶음이다.
    SpatialNodeBatch stale;
    {
        SpatialReadScope read(tree.Tree());
        auto collected =
            read.Reader().CollectNodeBatches(volume.Value(), Span<SpatialNodeBatch>(&stale, 1));
        REQUIRE(collected && collected.Value().written == 1);
    }
    REQUIRE(tree.Add(MakeCube(Vec3d(1, 2, 3), 1), 1) != IndexNone);
    SpatialReadScope read(tree.Tree());
    CHECK(read.Reader()
              .QueryBatch(stale, volume.Value(), 0xF,
                          [](const SpatialProxyHit&) noexcept { return QueryControl::Continue; })
              .Error() == ErrorCode::StaleHandle);
}

// FTZ·DAZ에서도 새 질의가 정답(기본 환경에서 계산)을 놓치지 않는다. 좌표가 비정규 값 근처인
// 프록시를 섞는다.
TEST(LooseOctree_ShapeQueriesStayConservativeUnderFlushToZero) {
    SystemAllocator allocator;
    LooseOctreeConfig config = SmallConfig();
    auto created = LooseOctree::TryCreate(config, allocator);
    REQUIRE(created);
    OracleTree tree(Move(created.Value()), allocator);
    RandomStream random(0xF7Au);
    for (uint32 index = 0; index < 300; ++index) {
        const double tiny = index % 3 == 0 ? 1.0e-310 * static_cast<double>(index) : 0.0;
        const Vec3d center(Uniform(random, -20, 20) + tiny, tiny, Uniform(random, -20, 20));
        REQUIRE(tree.Add(MakeCube(center, index % 4 == 0 ? 0.0 : Uniform(random, 0, 2)), 1) !=
                IndexNone);
    }
    ShapeQuery shapes[64];
    for (uint32 index = 0; index < 64; ++index) {
        shapes[index] = MakeShapeQuery(AllShapeKinds[index % 4], random, 20.0);
    }
    // FTZ·DAZ를 켠 채 후보 집합을 모은다(프록시 300개, 질의 64개).
    static uint8 seen[64][300];
    for (auto& row : seen) {
        for (uint8& flag : row) {
            flag = 0;
        }
    }
    const uint64 original = platform::detail::ReadFloatingPointControl();
    platform::detail::WriteFloatingPointControl(original | platform::detail::FlushToZeroBit() |
                                                platform::detail::DenormalsAreZeroBit());
    bool ok = true;
    {
        SpatialReadScope read(tree.Tree());
        for (uint32 index = 0; index < 64; ++index) {
            auto queried =
                RunShape(read.Reader(), shapes[index], 1, [&](const SpatialProxyHit& hit) noexcept {
                    if (hit.userValue < 300) {
                        seen[index][hit.userValue] = 1;
                    }
                    return QueryControl::Continue;
                });
            ok = ok && static_cast<bool>(queried);
        }
    }
    platform::detail::WriteFloatingPointControl(original);
    CHECK(ok);
    // 기본 환경에서 계산한 정답이 모두 후보에 있어야 한다.
    uint32 missing = 0;
    uint64 exactTotal = 0;
    for (uint32 index = 0; index < 64; ++index) {
        for (usize proxy = 0; proxy < tree.Proxies().Size(); ++proxy) {
            if (ExactHit(shapes[index], tree.Proxies()[proxy].bounds)) {
                ++exactTotal;
                missing += seen[index][proxy] == 0 ? 1u : 0u;
            }
        }
    }
    CHECK(missing == 0 && exactTotal > 0);
}
