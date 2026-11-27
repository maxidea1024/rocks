// Physics 모듈 PH3c2 시험이다(Docs/Design/PhysicsModule 4장, 7.1.9절): overlap의 Blocking·Any·All,
// ObjectTypes·회전·잘못된 입력, 컴포넌트 자기 형상 sweep·overlap(자기 Actor·자기 컴포넌트 빼기,
// 컴포넌트 채널·응답, 크기 0 컴포넌트의 line trace). World·컴포넌트 공개 API만 쓰므로 Modular
// 시험에서도 돈다.
#include "PhysicsBodyFixture.h"

#include <Core/Math/Quaternion.h>
#include <World/Physics/WorldCollision.h>

using namespace rocks;
namespace physics_overlap_tests_file {

using namespace physics_test_support;

namespace {

constexpr double BoxHalf = 10.0;
constexpr double ProbeRadius = 20.0;
// 질의 구가 원점에서 상자와 겹치는 깊이(cm)다.
constexpr double OverlapDepth = 5.0;
constexpr double FarX = 200.0;
constexpr double SweepReach = 500.0;
constexpr double WallX = 300.0;
constexpr double SweepTolerance = 0.05; // cm
constexpr double YawDegrees = 45.0;
// 옆 상자 앞면의 거리 배율이다: 회전하지 않은 상자(반폭 BoxHalf)는 닿지 않고 yaw 45° 상자의
// 꼭짓점은 닿는 1 < k < √2 사이 값이다(상자 body는 convex radius로 꼭짓점이 약 2 cm 안쪽이라 작게
// 둔다 — 7.1.9절 한계).
constexpr double TargetFaceScale = 1.1;

const QueryFilter Visibility = QueryFilter::ByChannel(CollisionChannel::Visibility);

bool StartWorld(BodyWorldFixture& fixture) noexcept {
    return fixture.registered && fixture.world && fixture.world->Initialize() &&
           fixture.world->BeginPlay();
}

// 원점의 구(반경 ProbeRadius)와 OverlapDepth만큼 겹치는 상자 중심 거리다.
constexpr double OverlappingOffset() noexcept {
    return ProbeRadius + BoxHalf - OverlapDepth;
}

// position에 반폭 BoxHalf 상자를 둔다(channel 응답 response).
ObjectHandle SpawnBoxAt(BodyWorldFixture& fixture, const Vec3d& position,
                        CollisionResponse response = CollisionResponse::Block,
                        CollisionChannel channel = CollisionChannel::Visibility) noexcept {
    return fixture.SpawnShape<BoxComponent>(
        [position, response, channel](BoxComponent& box) noexcept {
            (void)box.SetBoxExtent(Vec3d(BoxHalf, BoxHalf, BoxHalf));
            (void)box.SetRelativeLocation(position);
            (void)box.SetCollisionResponseToChannel(channel, response);
        });
}

bool Contains(const Array<OverlapResult>& overlaps, ObjectHandle component) noexcept {
    for (const OverlapResult& overlap : overlaps) {
        if (overlap.component == component) {
            return true;
        }
    }

    return false;
}

Result<void> SetResponse(World& world, ObjectHandle component, CollisionChannel channel,
                         CollisionResponse response) noexcept {
    return world.With<PrimitiveComponent>(component, [&](PrimitiveComponent& primitive) noexcept {
        return primitive.SetCollisionResponseToChannel(channel, response);
    });
}

} // namespace

// Blocking·Any·All(UE OverlapBlockingTest·OverlapAnyTest·OverlapMulti): 막는 상자·겹치는 상자·
// Ignore 상자·떨어진 상자. All은 body handle 순이고 반환 값은 막는 겹침이 있는지다.
TEST(PhysicsOverlap_ModesDiffer) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const double offset = OverlappingOffset();
    const ObjectHandle block = SpawnBoxAt(fixture, Vec3d(offset, 0.0, 0.0));
    const ObjectHandle touch =
        SpawnBoxAt(fixture, Vec3d(-offset, 0.0, 0.0), CollisionResponse::Overlap);
    const ObjectHandle ignored =
        SpawnBoxAt(fixture, Vec3d(0.0, offset, 0.0), CollisionResponse::Ignore);
    const ObjectHandle far = SpawnBoxAt(fixture, Vec3d(FarX, 0.0, 0.0));
    REQUIRE(!block.IsNull() && !touch.IsNull() && !ignored.IsNull() && !far.IsNull());
    const CollisionShape probe = CollisionShape::Sphere(ProbeRadius);

    CHECK(OverlapBlocking(world, Vec3d::Zero(), Quatd(), probe, Visibility, {}));
    CHECK(OverlapAny(world, Vec3d::Zero(), Quatd(), probe, Visibility, {}));
    Array<OverlapResult> overlaps(fixture.allocator);
    auto all = OverlapAll(world, Vec3d::Zero(), Quatd(), probe, Visibility, {}, overlaps);
    REQUIRE(all);
    CHECK(all.Value());
    REQUIRE(overlaps.Size() == 2u);
    CHECK(overlaps[0].component == block);
    CHECK(overlaps[0].blockingHit);
    CHECK(overlaps[1].component == touch);
    CHECK(!overlaps[1].blockingHit);
    CHECK(!overlaps[0].actor.IsNull());

    // 막는 상자를 빼면 Blocking은 거짓, Any는 참, All은 겹침 하나와 거짓이다.
    const ObjectHandle skipBlock[] = {block};
    QueryParams params;
    params.ignoreComponents = skipBlock;
    CHECK(!OverlapBlocking(world, Vec3d::Zero(), Quatd(), probe, Visibility, params));
    CHECK(OverlapAny(world, Vec3d::Zero(), Quatd(), probe, Visibility, params));
    all = OverlapAll(world, Vec3d::Zero(), Quatd(), probe, Visibility, params, overlaps);
    REQUIRE(all);
    CHECK(!all.Value());
    REQUIRE(overlaps.Size() == 1u);
    CHECK(overlaps[0].component == touch);
}

// ObjectTypes(4.4절 3, UE GeomOverlapMultiImp): 채널 응답과 무관하게 type이 맞으면 모두 막음이다.
// All은 겹침으로 모으므로 ignoreTouches면 비어 있다. Blocking은 일치를 겹침으로 바꿔 버리므로 늘
// 거짓이다(UE). Any는 막음으로 두므로 ignoreBlocks가 버리고 ignoreTouches는 상관없다.
TEST(PhysicsOverlap_ObjectTypes) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const double offset = OverlappingOffset();
    REQUIRE(!SpawnBoxAt(fixture, Vec3d(offset, 0.0, 0.0)).IsNull());
    REQUIRE(!SpawnBoxAt(fixture, Vec3d(-offset, 0.0, 0.0), CollisionResponse::Overlap).IsNull());
    REQUIRE(!SpawnBoxAt(fixture, Vec3d(0.0, offset, 0.0), CollisionResponse::Ignore).IsNull());
    const QueryFilter statics =
        QueryFilter::ByObjectTypes(CollisionObjectTypeBit(CollisionChannel::WorldStatic));
    const CollisionShape probe = CollisionShape::Sphere(ProbeRadius);

    Array<OverlapResult> overlaps(fixture.allocator);
    auto all = OverlapAll(world, Vec3d::Zero(), Quatd(), probe, statics, {}, overlaps);
    REQUIRE(all);
    CHECK(all.Value());
    REQUIRE(overlaps.Size() == 3u);
    CHECK(overlaps[0].blockingHit && overlaps[1].blockingHit && overlaps[2].blockingHit);
    CHECK(!OverlapBlocking(world, Vec3d::Zero(), Quatd(), probe, statics, {}));
    CHECK(OverlapAny(world, Vec3d::Zero(), Quatd(), probe, statics, {}));

    QueryParams touchesOff;
    touchesOff.ignoreTouches = true;
    all = OverlapAll(world, Vec3d::Zero(), Quatd(), probe, statics, touchesOff, overlaps);
    REQUIRE(all);
    CHECK(!all.Value());
    CHECK(overlaps.IsEmpty());
    CHECK(OverlapAny(world, Vec3d::Zero(), Quatd(), probe, statics, touchesOff));
    QueryParams blocksOff;
    blocksOff.ignoreBlocks = true;
    CHECK(!OverlapAny(world, Vec3d::Zero(), Quatd(), probe, statics, blocksOff));
}

// 회전: yaw 45° 상자만 꼭짓점이 옆 상자에 닿는다. 맞닿기만 한 형상(깊이 0)도 겹침이다.
// Line·Cylinder·0 quaternion·장면 없는 World는 겹침이 없다.
TEST(PhysicsOverlap_RotationAndDegenerateInputs) {
    constexpr double TargetFace = BoxHalf * TargetFaceScale;
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    REQUIRE(!SpawnBoxAt(fixture, Vec3d(TargetFace + BoxHalf, 0.0, 0.0)).IsNull());
    const CollisionShape cube = CollisionShape::Box(Vec3d(BoxHalf, BoxHalf, BoxHalf));
    const Quatd yaw =
        Quatd::FromUnitAxisAngle(Vec3d(0.0, 0.0, 1.0), math::DegreesToRadians(YawDegrees));
    CHECK(!OverlapAny(world, Vec3d::Zero(), Quatd(), cube, Visibility, {}));
    CHECK(OverlapAny(world, Vec3d::Zero(), yaw, cube, Visibility, {}));
    // 질의 상자 앞면이 옆 상자 앞면에 정확히 닿는다.
    CHECK(OverlapAny(world, Vec3d(TargetFace - BoxHalf, 0.0, 0.0), Quatd(), cube, Visibility, {}));

    CHECK(!OverlapAny(world, Vec3d(TargetFace + BoxHalf, 0.0, 0.0), Quatd(), CollisionShape(),
                      Visibility, {}));
    CHECK(!OverlapAny(world, Vec3d::Zero(), yaw, CollisionShape::Cylinder(ProbeRadius, ProbeRadius),
                      Visibility, {}));
    CHECK(!OverlapAny(world, Vec3d::Zero(), Quatd(0.0, 0.0, 0.0, 0.0), cube, Visibility, {}));

    BodyWorldFixture plain(false);
    REQUIRE(StartWorld(plain));
    Array<OverlapResult> overlaps(plain.allocator);
    auto all = OverlapAll(*plain.world, Vec3d::Zero(), Quatd(), cube, Visibility, {}, overlaps);
    REQUIRE(all);
    CHECK(!all.Value());
    CHECK(overlaps.IsEmpty());
}

// 컴포넌트 sweep(UE ComponentSweepMulti): 자기 Actor의 자식은 지나가고 다른 Actor의 벽에 맞는다.
// 채널은 컴포넌트 object type, 응답은 컴포넌트 응답 묶음이다.
TEST(PhysicsOverlap_ComponentTraceUsesOwnShapeAndChannel) {
    constexpr double ChildX = 100.0;
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    ObjectHandle mover;
    ObjectHandle child;
    REQUIRE((fixture.SpawnAttached<SphereComponent, BoxComponent>(
        mover, child,
        [](SphereComponent& sphere) noexcept {
            (void)sphere.SetSphereRadius(static_cast<float>(ProbeRadius));
            (void)sphere.SetCollisionObjectType(CollisionChannel::Pawn);
        },
        [](BoxComponent& box) noexcept {
            (void)box.SetBoxExtent(Vec3d(BoxHalf, BoxHalf, BoxHalf));
            (void)box.SetRelativeLocation(Vec3d(ChildX, 0.0, 0.0));
        })));
    const ObjectHandle wall = SpawnBoxAt(fixture, Vec3d(WallX, 0.0, 0.0));
    REQUIRE(!wall.IsNull());
    const Vec3d end(SweepReach, 0.0, 0.0);

    Array<HitResult> hits(fixture.allocator);
    auto traced = ComponentTraceAll(world, mover, Vec3d::Zero(), end, Quatd(), {}, hits);
    REQUIRE(traced);
    CHECK(traced.Value());
    REQUIRE(!hits.IsEmpty());
    for (const HitResult& hit : hits) {
        CHECK(!(hit.component == child));
        CHECK(!(hit.component == mover));
    }

    CHECK(hits[hits.Size() - 1].component == wall);
    CHECK(Near(hits[hits.Size() - 1].location.x, WallX - BoxHalf - ProbeRadius, SweepTolerance));

    // 벽이 컴포넌트 채널(Pawn)을 Ignore하면 맞지 않는다.
    REQUIRE(SetResponse(world, wall, CollisionChannel::Pawn, CollisionResponse::Ignore));
    traced = ComponentTraceAll(world, mover, Vec3d::Zero(), end, Quatd(), {}, hits);
    REQUIRE(traced);
    CHECK(!traced.Value());
    CHECK(hits.IsEmpty());
    // 컴포넌트가 벽 type(WorldStatic)을 Ignore해도 맞지 않는다.
    REQUIRE(SetResponse(world, wall, CollisionChannel::Pawn, CollisionResponse::Block));
    REQUIRE(SetResponse(world, mover, CollisionChannel::WorldStatic, CollisionResponse::Ignore));
    traced = ComponentTraceAll(world, mover, Vec3d::Zero(), end, Quatd(), {}, hits);
    REQUIRE(traced);
    CHECK(!traced.Value());
    CHECK(hits.IsEmpty());
}

// 컴포넌트 질의는 인자의 위치·회전에 body 형상을 놓는다(컴포넌트의 지금 자리가 아니다): 상자
// 컴포넌트를 yaw 45°로 쓸면 꼭짓점이 먼저 닿고, 떨어진 자리로 overlap하면 그 자리의 상자와 겹친다.
TEST(PhysicsOverlap_ComponentQueriesUseGivenPose) {
    constexpr double ApartY = 500.0;
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle cube = fixture.SpawnShape<BoxComponent>([](BoxComponent& box) noexcept {
        (void)box.SetBoxExtent(Vec3d(BoxHalf, BoxHalf, BoxHalf));
    });
    const ObjectHandle wall = SpawnBoxAt(fixture, Vec3d(WallX, 0.0, 0.0));
    const double targetFace = BoxHalf * TargetFaceScale;
    const ObjectHandle side = SpawnBoxAt(fixture, Vec3d(targetFace + BoxHalf, ApartY, 0.0));
    REQUIRE(!cube.IsNull() && !wall.IsNull() && !side.IsNull());
    const Quatd yaw =
        Quatd::FromUnitAxisAngle(Vec3d(0.0, 0.0, 1.0), math::DegreesToRadians(YawDegrees));
    const Vec3d end(SweepReach, 0.0, 0.0);

    Array<HitResult> straight(fixture.allocator);
    Array<HitResult> turned(fixture.allocator);
    REQUIRE(ComponentTraceAll(world, cube, Vec3d::Zero(), end, Quatd(), {}, straight));
    REQUIRE(ComponentTraceAll(world, cube, Vec3d::Zero(), end, yaw, {}, turned));
    REQUIRE(!straight.IsEmpty() && !turned.IsEmpty());
    const HitResult& face = straight[straight.Size() - 1];
    const HitResult& corner = turned[turned.Size() - 1];
    CHECK(face.component == wall);
    CHECK(corner.component == wall);
    CHECK(Near(face.location.x, WallX - BoxHalf * 2.0, SweepTolerance));
    // 둥근 꼭짓점(7.1.9절 한계)도 면보다 이만큼은 먼저 닿는다.
    constexpr double CornerLead = 1.0; // cm
    CHECK(corner.location.x < face.location.x - CornerLead);

    Array<OverlapResult> overlaps(fixture.allocator);
    const Vec3d apart(0.0, ApartY, 0.0);
    auto overlapped = ComponentOverlapAll(world, cube, apart, Quatd(), {}, overlaps);
    REQUIRE(overlapped);
    CHECK(!overlapped.Value());
    overlapped = ComponentOverlapAll(world, cube, apart, yaw, {}, overlaps);
    REQUIRE(overlapped);
    CHECK(overlapped.Value());
    REQUIRE(overlaps.Size() == 1u);
    CHECK(overlaps[0].component == side);
}

// 크기 0 컴포넌트(UE IsZeroExtent)는 같은 채널·응답의 line trace다(구 반경만큼 앞에서 멈추지
// 않는다). UE처럼 자기 Actor를 빼지 않아 같은 Actor의 자식에 맞는다(크기가 있으면 지나간다). body가
// 없는 컴포넌트는 hit 없음이다.
TEST(PhysicsOverlap_ComponentTraceZeroExtentAndNoBody) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle point = fixture.SpawnShape<SphereComponent>(
        [](SphereComponent& sphere) noexcept { (void)sphere.SetSphereRadius(0.0f); });
    const ObjectHandle quiet =
        fixture.SpawnShape<SphereComponent>([](SphereComponent& sphere) noexcept {
            (void)sphere.SetSphereRadius(static_cast<float>(ProbeRadius));
            (void)sphere.SetCollisionEnabled(CollisionEnabled::NoCollision);
        });
    const ObjectHandle wall = SpawnBoxAt(fixture, Vec3d(WallX, 0.0, 0.0));
    REQUIRE(!point.IsNull() && !quiet.IsNull() && !wall.IsNull());
    const Vec3d end(SweepReach, 0.0, 0.0);
    Array<HitResult> hits(fixture.allocator);
    auto traced = ComponentTraceAll(world, point, Vec3d::Zero(), end, Quatd(), {}, hits);
    REQUIRE(traced);
    CHECK(traced.Value());
    REQUIRE(!hits.IsEmpty());
    const HitResult& hit = hits[hits.Size() - 1];
    CHECK(hit.component == wall);
    CHECK(Near(hit.location.x, WallX - BoxHalf, SweepTolerance));

    traced = ComponentTraceAll(world, quiet, Vec3d::Zero(), end, Quatd(), {}, hits);
    REQUIRE(traced);
    CHECK(!traced.Value());
    CHECK(hits.IsEmpty());

    constexpr double ChildX = 100.0;
    constexpr double PointY = 200.0;
    ObjectHandle zero;
    ObjectHandle child;
    REQUIRE((fixture.SpawnAttached<SphereComponent, BoxComponent>(
        zero, child,
        [](SphereComponent& sphere) noexcept {
            (void)sphere.SetSphereRadius(0.0f);
            (void)sphere.SetRelativeLocation(Vec3d(0.0, PointY, 0.0));
        },
        [](BoxComponent& box) noexcept {
            (void)box.SetBoxExtent(Vec3d(BoxHalf, BoxHalf, BoxHalf));
            (void)box.SetRelativeLocation(Vec3d(ChildX, 0.0, 0.0));
        })));
    traced = ComponentTraceAll(world, zero, Vec3d(0.0, PointY, 0.0), Vec3d(SweepReach, PointY, 0.0),
                               Quatd(), {}, hits);
    REQUIRE(traced);
    CHECK(traced.Value());
    REQUIRE(!hits.IsEmpty());
    CHECK(hits[hits.Size() - 1].component == child);
    Array<OverlapResult> overlaps(fixture.allocator);
    auto overlapped = ComponentOverlapAll(world, quiet, Vec3d::Zero(), Quatd(), {}, overlaps);
    REQUIRE(overlapped);
    CHECK(!overlapped.Value());
}

// 컴포넌트 overlap(UE ComponentOverlapMulti): 자기 컴포넌트만 빼고 같은 Actor의 자식은 맞는다.
// 반환 값은 겹침이 하나라도 있는지다(막음이 아니어도 참).
TEST(PhysicsOverlap_ComponentOverlapExcludesSelfOnly) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const double offset = OverlappingOffset();
    ObjectHandle self;
    ObjectHandle child;
    REQUIRE((fixture.SpawnAttached<SphereComponent, BoxComponent>(
        self, child,
        [](SphereComponent& sphere) noexcept {
            (void)sphere.SetSphereRadius(static_cast<float>(ProbeRadius));
        },
        [offset](BoxComponent& box) noexcept {
            (void)box.SetBoxExtent(Vec3d(BoxHalf, BoxHalf, BoxHalf));
            (void)box.SetRelativeLocation(Vec3d(offset, 0.0, 0.0));
        })));
    const ObjectHandle other = SpawnBoxAt(fixture, Vec3d(-offset, 0.0, 0.0));
    REQUIRE(!other.IsNull());

    Array<OverlapResult> overlaps(fixture.allocator);
    auto overlapped = ComponentOverlapAll(world, self, Vec3d::Zero(), Quatd(), {}, overlaps);
    REQUIRE(overlapped);
    CHECK(overlapped.Value());
    CHECK(overlaps.Size() == 2u);
    CHECK(Contains(overlaps, child));
    CHECK(Contains(overlaps, other));
    CHECK(!Contains(overlaps, self));

    // 겹침만(막음 없음) 있어도 참이다: 두 상자가 컴포넌트 채널(WorldStatic)을 Overlap으로 응답한다.
    REQUIRE(SetResponse(world, child, CollisionChannel::WorldStatic, CollisionResponse::Overlap));
    REQUIRE(SetResponse(world, other, CollisionChannel::WorldStatic, CollisionResponse::Overlap));
    overlapped = ComponentOverlapAll(world, self, Vec3d::Zero(), Quatd(), {}, overlaps);
    REQUIRE(overlapped);
    CHECK(overlapped.Value());
    REQUIRE(overlaps.Size() == 2u);
    CHECK(!overlaps[0].blockingHit && !overlaps[1].blockingHit);
}

} // namespace physics_overlap_tests_file
