// Physics 모듈 PH3c1 시험이다(Docs/Design/PhysicsModule 4장, 7.1.8절): shape trace의 결과 값·PA08
// 법선, 회전·캡슐 형상, 시작 겹침(MTD·버리기·가장 깊은 것), All의 순서, 작은 형상의 line 전환, 길이
// 0 sweep. World·컴포넌트 공개 API만 쓰므로 Modular 시험에서도 돈다.
#include "PhysicsBodyFixture.h"

#include <Core/Math/Quaternion.h>
#include <World/Physics/WorldCollision.h>

using namespace rocks;
namespace physics_sweep_tests_file {

using namespace physics_test_support;

namespace {

// shape cast 결과 위치 허용 오차다(Jolt GJK·EPA 허용 오차 1e-4 m 수준 + float 좌표).
constexpr double SweepTolerance = 0.05; // cm
constexpr double DepthTolerance = 0.1;  // cm
constexpr double NormalTolerance = 1.0e-4;
constexpr double BoxHalf = 10.0;
constexpr double SweepRadius = 20.0;
constexpr double SweepReach = 500.0;
// x축 sweep의 벽: 반폭 BoxHalf 상자의 중심이다(앞면 x = WallX − BoxHalf).
constexpr double WallX = 100.0;
// 시작 겹침 깊이(cm)다(얕은 것·깊은 것).
constexpr double ShallowDepth = 5.0;
constexpr double DeepDepth = 10.0;

// 구(반경 SweepRadius)가 원점에서 x 방향 상자(반폭 BoxHalf)에 depth만큼 겹치는 상자 중심 x다.
constexpr double OverlappingBoxX(double depth) noexcept {
    return SweepRadius + BoxHalf - depth;
}

const QueryFilter Visibility = QueryFilter::ByChannel(CollisionChannel::Visibility);
const Vec3d XStart(-SweepReach, 0.0, 0.0);
const Vec3d XEnd(SweepReach, 0.0, 0.0);

bool StartWorld(BodyWorldFixture& fixture) noexcept {
    return fixture.registered && fixture.world && fixture.world->Initialize() &&
           fixture.world->BeginPlay();
}

bool NearVector(const Vec3d& value, const Vec3d& expected, double tolerance) noexcept {
    return Near(value.x, expected.x, tolerance) && Near(value.y, expected.y, tolerance) &&
           Near(value.z, expected.z, tolerance);
}

// x에 상자(x 반폭 BoxHalf, y·z 반폭 side)를 둔다(Visibility 응답 response).
ObjectHandle SpawnBoxAt(BodyWorldFixture& fixture, double x,
                        CollisionResponse response = CollisionResponse::Block,
                        double side = BoxHalf) noexcept {
    return fixture.SpawnShape<BoxComponent>([x, response, side](BoxComponent& box) noexcept {
        (void)box.SetBoxExtent(Vec3d(BoxHalf, side, side));
        (void)box.SetRelativeLocation(Vec3d(x, 0.0, 0.0));
        (void)box.SetCollisionResponseToChannel(CollisionChannel::Visibility, response);
    });
}

// x축 sweep에서 형상 중심이 멈추는 x(앞면 − 형상의 +x 반폭)의 time이다.
double StopTime(double stopX) noexcept {
    return (stopX - XStart.x) / (XEnd.x - XStart.x);
}

} // namespace

// PA08 sweep: 구를 바닥(윗면 z = 0)으로 쓸면 normal·impactNormal이 +Z이고 구 중심은 z = 반경에
// 멈춘다.
TEST(PhysicsSweep_SphereOntoFloor) {
    constexpr double Height = 200.0;
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle floor = fixture.SpawnFloor();
    REQUIRE(!floor.IsNull());
    const Vec3d start(0.0, 0.0, Height);
    const Vec3d end(0.0, 0.0, -Height);
    const CollisionShape sphere = CollisionShape::Sphere(SweepRadius);
    CHECK(ShapeTraceTest(world, start, end, Quatd(), sphere, Visibility, {}));
    HitResult hit;
    REQUIRE(ShapeTraceFirst(world, start, end, Quatd(), sphere, Visibility, {}, hit));
    CHECK(hit.component == floor);
    CHECK(hit.blockingHit);
    CHECK(!hit.startPenetrating);
    const double length = Height * 2.0;
    CHECK(Near(hit.time, (Height - SweepRadius) / length, NormalTolerance));
    CHECK(Near(hit.distance, Height - SweepRadius, SweepTolerance));
    CHECK(NearVector(hit.location, Vec3d(0.0, 0.0, SweepRadius), SweepTolerance));
    CHECK(NearVector(hit.impactPoint, Vec3d::Zero(), SweepTolerance));
    CHECK(NearVector(hit.impactNormal, Vec3d(0.0, 0.0, 1.0), NormalTolerance));
    CHECK(NearVector(hit.normal, Vec3d(0.0, 0.0, 1.0), NormalTolerance));
    CHECK(hit.penetrationDepth == 0.0);

    // 무시 목록은 sweep에도 적용된다.
    const ObjectHandle skipFloor[] = {floor};
    QueryParams params;
    params.ignoreComponents = skipFloor;
    CHECK(!ShapeTraceTest(world, start, end, Quatd(), sphere, Visibility, params));
}

// 형상과 회전: yaw 45° 상자는 모서리(중심에서 반폭·√2)가 벽면에 먼저 닿고, 캡슐(축 Z)은 반경만큼
// 앞에서 멈춘다. 질의 상자는 모서리를 둥글리지 않는다.
TEST(PhysicsSweep_RotatedBoxAndCapsule) {
    constexpr double CapsuleRadius = 15.0;
    constexpr double CapsuleHalfHeight = 40.0;
    constexpr double YawDegrees = 45.0;
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    // 접촉 모서리(질의 상자의 세로 모서리)가 벽면 안쪽에 오도록 벽을 넓게 둔다.
    constexpr double WallSide = 100.0;
    const ObjectHandle wall = SpawnBoxAt(fixture, WallX, CollisionResponse::Block, WallSide);
    REQUIRE(!wall.IsNull());
    const double face = WallX - BoxHalf;

    const Quatd yaw =
        Quatd::FromUnitAxisAngle(Vec3d(0.0, 0.0, 1.0), math::DegreesToRadians(YawDegrees));
    HitResult hit;
    REQUIRE(ShapeTraceFirst(world, XStart, XEnd, yaw,
                            CollisionShape::Box(Vec3d(BoxHalf, BoxHalf, BoxHalf)), Visibility, {},
                            hit));
    CHECK(hit.component == wall);
    // yaw 45° 상자의 +x 꼭짓점은 중심에서 반폭·√2 앞이다.
    const double corner = math::Sqrt(BoxHalf * BoxHalf * 2.0);
    CHECK(Near(hit.location.x, face - corner, SweepTolerance));
    CHECK(Near(hit.time, StopTime(face - corner), NormalTolerance));
    CHECK(NearVector(hit.impactNormal, Vec3d(-1.0, 0.0, 0.0), NormalTolerance));
    CHECK(NearVector(hit.normal, Vec3d(-1.0, 0.0, 0.0), NormalTolerance));
    CHECK(Near(hit.impactPoint.x, face, SweepTolerance));

    REQUIRE(ShapeTraceFirst(world, XStart, XEnd, Quatd(),
                            CollisionShape::Capsule(CapsuleRadius, CapsuleHalfHeight), Visibility,
                            {}, hit));
    CHECK(hit.component == wall);
    CHECK(Near(hit.location.x, face - CapsuleRadius, SweepTolerance));
    // CapsuleFromExtent: 반경 = max(x, y), 반높이 = z(UE SetCapsule).
    const CollisionShape fromExtent =
        CollisionShape::CapsuleFromExtent(Vec3d(ShallowDepth, CapsuleRadius, CapsuleHalfHeight));
    CHECK(fromExtent.radius == CapsuleRadius);
    CHECK(fromExtent.halfHeight == CapsuleHalfHeight);
}

// 시작 겹침(UE ConvertOverlappedShapeToImpactHit): time 0, location = 시작, normal = 밀어낼 방향,
// penetrationDepth = MTD 깊이. findInitialOverlaps가 거짓이면 버린다.
TEST(PhysicsSweep_InitialOverlapReportsMtd) {
    constexpr double Sunk = DeepDepth;
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle floor = fixture.SpawnFloor();
    REQUIRE(!floor.IsNull());
    const Vec3d start(0.0, 0.0, SweepRadius - Sunk);
    const Vec3d end(0.0, 0.0, SweepRadius - Sunk + SweepReach);
    const CollisionShape sphere = CollisionShape::Sphere(SweepRadius);
    HitResult hit;
    REQUIRE(ShapeTraceFirst(world, start, end, Quatd(), sphere, Visibility, {}, hit));
    CHECK(hit.component == floor);
    CHECK(hit.startPenetrating);
    CHECK(hit.blockingHit);
    CHECK(!hit.IsValidBlockingHit());
    CHECK(hit.time == 0.0);
    CHECK(hit.distance == 0.0);
    CHECK(hit.location == start);
    CHECK(Near(hit.penetrationDepth, Sunk, DepthTolerance));
    CHECK(NearVector(hit.normal, Vec3d(0.0, 0.0, 1.0), NormalTolerance));
    CHECK(hit.impactNormal == hit.normal);
    CHECK(Near(hit.impactPoint.z, 0.0, DepthTolerance));

    QueryParams discard;
    discard.findInitialOverlaps = false;
    CHECK(!ShapeTraceFirst(world, start, end, Quatd(), sphere, Visibility, discard, hit));
    CHECK(!ShapeTraceTest(world, start, end, Quatd(), sphere, Visibility, discard));
    // Test도 막는 시작 겹침을 찾는다.
    CHECK(ShapeTraceTest(world, start, end, Quatd(), sphere, Visibility, {}));

    // 시작 겹침만 있는 All도 막는 hit가 있다(보고된 막음 — UE bHadBlockingHit).
    Array<HitResult> hits(fixture.allocator);
    auto all = ShapeTraceAll(world, start, end, Quatd(), sphere, Visibility, {}, hits);
    REQUIRE(all);
    CHECK(all.Value());
    REQUIRE(hits.Size() == 1u);
    CHECK(hits[0].startPenetrating && hits[0].blockingHit);
}

// First는 시작 겹침 중 가장 깊은 것을 고른다(UE — MTD 거리가 더 음수인 쪽). 칸 순서와 반대로
// 둔다.
TEST(PhysicsSweep_FirstPicksDeepestInitialOverlap) {
    constexpr double ShallowX = -OverlappingBoxX(ShallowDepth);
    constexpr double DeepX = OverlappingBoxX(DeepDepth);
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle shallow = SpawnBoxAt(fixture, ShallowX);
    const ObjectHandle deep = SpawnBoxAt(fixture, DeepX);
    REQUIRE(!shallow.IsNull() && !deep.IsNull());
    HitResult hit;
    REQUIRE(ShapeTraceFirst(world, Vec3d::Zero(), Vec3d(0.0, 0.0, SweepReach), Quatd(),
                            CollisionShape::Sphere(SweepRadius), Visibility, {}, hit));
    CHECK(hit.component == deep);
    CHECK(hit.startPenetrating);
    CHECK(Near(hit.penetrationDepth, DeepDepth, DepthTolerance));
    CHECK(NearVector(hit.normal, Vec3d(-1.0, 0.0, 0.0), NormalTolerance));
}

// All(7.1.8절): 막는 시작 겹침은 자르지 않고 모은 뒤 막음으로 보고한다. 순서는 시작 겹침 → 겹침 →
// 시작 겹침이 아닌 가장 가까운 막음이고, 그 뒤 겹침은 버린다. First는 시작 겹침을 고른다.
TEST(PhysicsSweep_AllKeepsInitialBlocksAndCuts) {
    constexpr double InitialX = OverlappingBoxX(ShallowDepth);
    constexpr double OverlapX = 200.0;
    constexpr double BlockX = 300.0;
    constexpr double LateOverlapX = 400.0;
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle initial = SpawnBoxAt(fixture, InitialX);
    const ObjectHandle overlap = SpawnBoxAt(fixture, OverlapX, CollisionResponse::Overlap);
    const ObjectHandle block = SpawnBoxAt(fixture, BlockX);
    const ObjectHandle late = SpawnBoxAt(fixture, LateOverlapX, CollisionResponse::Overlap);
    REQUIRE(!initial.IsNull() && !overlap.IsNull() && !block.IsNull() && !late.IsNull());
    const CollisionShape sphere = CollisionShape::Sphere(SweepRadius);
    const Vec3d start = Vec3d::Zero();
    const Vec3d end(SweepReach, 0.0, 0.0);

    Array<HitResult> hits(fixture.allocator);
    auto all = ShapeTraceAll(world, start, end, Quatd(), sphere, Visibility, {}, hits);
    REQUIRE(all);
    CHECK(all.Value());
    REQUIRE(hits.Size() == 3u);
    CHECK(hits[0].component == initial);
    CHECK(hits[0].startPenetrating && hits[0].blockingHit);
    CHECK(hits[1].component == overlap);
    CHECK(!hits[1].blockingHit && !hits[1].startPenetrating);
    CHECK(hits[2].component == block);
    CHECK(hits[2].blockingHit && !hits[2].startPenetrating);
    CHECK(Near(hits[2].location.x, BlockX - BoxHalf - SweepRadius, SweepTolerance));

    HitResult hit;
    REQUIRE(ShapeTraceFirst(world, start, end, Quatd(), sphere, Visibility, {}, hit));
    CHECK(hit.component == initial);

    // 시작 겹침을 버리면 겹침과 막음만 남는다.
    QueryParams discard;
    discard.findInitialOverlaps = false;
    REQUIRE(ShapeTraceAll(world, start, end, Quatd(), sphere, Visibility, discard, hits));
    REQUIRE(hits.Size() == 2u);
    CHECK(hits[0].component == overlap);
    CHECK(hits[1].component == block);

    // ignoreTouches는 막는 시작 겹침을 버리지 않는다(UE — 시작 겹침의 겹침 취급은 PreFilter 뒤다).
    QueryParams touchesOff;
    touchesOff.ignoreTouches = true;
    REQUIRE(ShapeTraceAll(world, start, end, Quatd(), sphere, Visibility, touchesOff, hits));
    REQUIRE(hits.Size() == 2u);
    CHECK(hits[0].component == initial);
    CHECK(hits[1].component == block);

    // ObjectTypes All은 자르지 않는다: 시작 겹침을 포함해 WorldStatic 상자 넷이 모두 막음이다.
    const QueryFilter statics =
        QueryFilter::ByObjectTypes(CollisionObjectTypeBit(CollisionChannel::WorldStatic));
    all = ShapeTraceAll(world, start, end, Quatd(), sphere, statics, {}, hits);
    REQUIRE(all);
    CHECK(all.Value());
    REQUIRE(hits.Size() == 4u);
    CHECK(hits[0].component == initial);
    CHECK(hits[0].startPenetrating);
    CHECK(hits[3].component == late);
    CHECK(hits[0].blockingHit && hits[1].blockingHit && hits[2].blockingHit && hits[3].blockingHit);
}

// impactNormal(UE FindGeomOpposingNormal): 구가 상자의 세로 모서리를 맞히면 normal은 모서리에서 구
// 중심 쪽(비스듬)이고, impactNormal은 normal 성분이 있는 면 가운데 sweep 방향에 가장 맞서는 면(−X)
// 이다. 상자 질의는 impactNormal = normal이다.
TEST(PhysicsSweep_ImpactNormalOpposesSweep) {
    // 구 중심이 상자 +Y 면보다 이만큼 바깥을 지나간다(모서리에 비스듬히 닿는다).
    constexpr double EdgeOffset = SweepRadius * 0.6;
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle wall = SpawnBoxAt(fixture, WallX);
    REQUIRE(!wall.IsNull());
    const Vec3d start(-SweepReach, BoxHalf + EdgeOffset, 0.0);
    const Vec3d end(SweepReach, BoxHalf + EdgeOffset, 0.0);
    HitResult hit;
    REQUIRE(ShapeTraceFirst(world, start, end, Quatd(), CollisionShape::Sphere(SweepRadius),
                            Visibility, {}, hit));
    CHECK(hit.component == wall);
    CHECK(hit.normal.x < 0.0);
    CHECK(hit.normal.y > 0.0);
    CHECK(NearVector(hit.impactNormal, Vec3d(-1.0, 0.0, 0.0), NormalTolerance));

    REQUIRE(ShapeTraceFirst(world, start, end, Quatd(),
                            CollisionShape::Box(Vec3d(BoxHalf, BoxHalf + EdgeOffset, BoxHalf)),
                            Visibility, {}, hit));
    CHECK(hit.component == wall);
    CHECK(hit.impactNormal == hit.normal);
}

// 반경 0 구는 line trace다(UE WorldCollision.cpp:151-155): 같은 hit 값이다. 길이 0 sweep은 겹치면
// 시작 겹침, 아니면 hit 없음이다.
TEST(PhysicsSweep_DegenerateShapesAndLengths) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle wall = SpawnBoxAt(fixture, WallX);
    REQUIRE(!wall.IsNull());
    HitResult line;
    HitResult point;
    REQUIRE(LineTraceFirst(world, XStart, XEnd, Visibility, {}, line));
    REQUIRE(ShapeTraceFirst(world, XStart, XEnd, Quatd(), CollisionShape::Sphere(0.0), Visibility,
                            {}, point));
    CHECK(point.component == wall);
    CHECK(point.time == line.time);
    CHECK(point.impactPoint == line.impactPoint);
    CHECK(point.impactNormal == line.impactNormal);

    const CollisionShape sphere = CollisionShape::Sphere(SweepRadius);
    const Vec3d touching(WallX - BoxHalf - SweepRadius + ShallowDepth, 0.0, 0.0);
    HitResult hit;
    REQUIRE(ShapeTraceFirst(world, touching, touching, Quatd(), sphere, Visibility, {}, hit));
    CHECK(hit.component == wall);
    CHECK(hit.startPenetrating);
    CHECK(Near(hit.penetrationDepth, ShallowDepth, DepthTolerance));
    CHECK(hit.location == touching);
    CHECK(NearVector(hit.normal, Vec3d(-1.0, 0.0, 0.0), NormalTolerance));
    CHECK(Near(hit.impactPoint.x, WallX - BoxHalf, DepthTolerance));
    QueryParams discard;
    discard.findInitialOverlaps = false;
    CHECK(!ShapeTraceTest(world, touching, touching, Quatd(), sphere, Visibility, discard));
    Array<HitResult> hits(fixture.allocator);
    REQUIRE(ShapeTraceAll(world, touching, touching, Quatd(), sphere, Visibility, {}, hits));
    REQUIRE(hits.Size() == 1u);
    CHECK(hits[0].startPenetrating);
    const Vec3d apart(WallX - BoxHalf - SweepRadius - DeepDepth, 0.0, 0.0);
    CHECK(!ShapeTraceTest(world, apart, apart, Quatd(), sphere, Visibility, {}));

    // Cylinder 질의는 CY2 전에는 hit 없음이다(All도 오류가 아니다).
    const CollisionShape cylinder = CollisionShape::Cylinder(SweepRadius, SweepRadius);
    CHECK(!ShapeTraceTest(world, XStart, XEnd, Quatd(), cylinder, Visibility, {}));
    auto cylinderAll = ShapeTraceAll(world, XStart, XEnd, Quatd(), cylinder, Visibility, {}, hits);
    REQUIRE(cylinderAll);
    CHECK(!cylinderAll.Value());
    // 0 quaternion은 hit 없음이다(정규화할 수 없다). 정규화되지 않은 회전은 정규화해 쓴다.
    CHECK(!ShapeTraceTest(world, XStart, XEnd, Quatd(0.0, 0.0, 0.0, 0.0), sphere, Visibility, {}));
    CHECK(ShapeTraceTest(world, XStart, XEnd, Quatd(0.0, 0.0, 0.0, DeepDepth), sphere, Visibility,
                         {}));
}

} // namespace physics_sweep_tests_file
