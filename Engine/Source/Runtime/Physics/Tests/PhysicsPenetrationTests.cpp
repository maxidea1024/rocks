// Physics 모듈 PH4d 시험이다(Docs/Design/PhysicsModule 5.2절, 7.1.13절): PrimitiveComponent의
// ComputePenetration(MTD)·GetCollisionShape(scale·부풀리기), CollisionShape::Inflate,
// MovementComponent의 GetPenetrationAdjustment·ResolvePenetration·SafeMoveUpdatedComponent(밀어내기
// teleport·막히면 sweep·박히지 않은 보통 sweep·Primitive가 아닌 대상). World·컴포넌트 공개 API만
// 쓰므로 Modular 시험에서도 돈다.
#include "PhysicsBodyFixture.h"

#include <Collision/CollisionShape.h>
#include <Platform/PlatformMath.h>
#include <World/Movement/MovementComponent.generated.h>
#include <World/Movement/MovementComponent.h>
#include <World/Physics/MtdResult.h>

using namespace rocks;
namespace physics_penetration_tests_file {

using namespace physics_test_support;

namespace {

constexpr double BoxHalf = 10.0;
constexpr double ProbeRadius = 20.0;
// 상자(반폭 10)에 구(반경 20)가 5 cm 박히는 중심 x다.
constexpr double OverlapX = 25.0;
constexpr double OverlapDepth = BoxHalf + ProbeRadius - OverlapX;
// 거의 닿는 자리다(정확히 닿는 float 경계를 피해 아주 조금 겹친다).
constexpr double NearTouchSlack = 1.0e-3;
constexpr double TouchX = BoxHalf + ProbeRadius - NearTouchSlack;
constexpr double ApartX = 40.0;
// Jolt 깊이(EPA float) 허용 오차(cm)다.
constexpr double DepthTolerance = 0.05;
constexpr double DirectionTolerance = 1.0e-3;

// 벽(반폭 10, 원점)에 5 cm 박히는 이동 상자 중심 x다.
constexpr double StuckX = 15.0;
constexpr double StuckDepth = 2.0 * BoxHalf - StuckX;
constexpr double SideMove = 50.0;
// 밀어낸 자리를 막는 두 번째 벽(반폭 1, 중심 x 31 — 30–32)이다.
constexpr double SecondWallX = 31.0;
constexpr double SecondWallHalf = 1.0;
constexpr double SecondWallSize = 50.0;
// 박히지 않은 상자가 벽 쪽으로 가는 시작 x와 이동량이다.
constexpr double FreeStartX = 50.0;
constexpr double FreeMove = -100.0;
// 보통 sweep이 멈출 자리의 허용 오차(cm — pullback 1 cm + 0.001·D 이하)다.
constexpr double StopTolerance = 1.5;
constexpr double Inflation = 0.1;
constexpr double ShapeScale = 2.0;
constexpr double PositionCheckTolerance = 1.0e-6;
// 회전 시험: Z축 45° 벽의 면 법선 방향으로 구를 놓고, 눕힌 캡슐(반경 5, 반높이 20)을 상자 위에
// 놓는다(세워 두면 훨씬 깊다).
constexpr double CapsuleRadius = 5.0;
constexpr double CapsuleHalfHeight = 20.0;
constexpr double CapsuleCenterZ = BoxHalf + CapsuleRadius - 2.0;
constexpr double CapsuleDepth = BoxHalf + CapsuleRadius - CapsuleCenterZ;
// 해소 전에 남긴 hit의 time 표지(덮어썼는지 본다)다.
constexpr double StaleHitTime = 0.5;

// 벽·이동 상자 World다. 이동 상자 Actor에 MovementComponent를 붙이고 갱신 대상을 상자로 둔다.
struct PenetrationWorld {
    PenetrationWorld() noexcept {
        started = fixture.registered && fixture.world && fixture.world->Initialize() &&
                  fixture.world->BeginPlay();
    }

    ~PenetrationWorld() {
        if (fixture.world) {
            (void)fixture.world->Shutdown();
        }
    }

    // 정적 상자다(반폭 halfExtent).
    ObjectHandle Wall(const Vec3d& at, const Vec3d& halfExtent) noexcept {
        return fixture.SpawnShape<BoxComponent>([&](BoxComponent& box) noexcept {
            box.SetMobility(ComponentMobility::Static);
            (void)box.SetBoxExtent(halfExtent);
            (void)box.SetRelativeLocation(at);
        });
    }

    // 이동 상자와 그 Actor의 MovementComponent를 만든다.
    bool Mover(double x, ObjectHandle& box, ObjectHandle& movement) noexcept {
        World& world = *fixture.world;
        auto actor = world.SpawnActorDeferred<PhysicsTestActor>();
        if (!actor) {
            return false;
        }

        auto made = world.AddComponent<BoxComponent>(actor.Value());
        auto mover = world.AddComponent<MovementComponent>(actor.Value());
        if (!made || !mover) {
            return false;
        }

        (void)world.With<BoxComponent>(made.Value(), [x](BoxComponent& shape) noexcept {
            (void)shape.SetBoxExtent(Vec3d(BoxHalf, BoxHalf, BoxHalf));
            (void)shape.SetRelativeLocation(Vec3d(x, 0.0, 0.0));
        });
        auto set =
            world.With<MovementComponent>(mover.Value(), [&](MovementComponent& value) noexcept {
                value.SetAutoRegisterUpdatedComponent(false);
                return value.SetUpdatedComponent(made.Value());
            });
        if (!set || !world.FinishSpawning(actor.Value())) {
            return false;
        }

        box = made.Value();
        movement = mover.Value();
        return true;
    }

    Result<bool> SafeMove(ObjectHandle movement, const Vec3d& delta, HitResult& hit) noexcept {
        Result<bool> moved = Result<bool>::Failure(ErrorCode::NotFound);
        (void)fixture.world->With<MovementComponent>(
            movement, [&](MovementComponent& value) noexcept {
                moved =
                    value.SafeMoveUpdatedComponent(delta, Quatd(), true, hit, TeleportMode::None);
            });
        return moved;
    }

    Vec3d Location(ObjectHandle component) noexcept {
        return fixture.ComponentPose(component).translation;
    }

    bool Penetration(ObjectHandle component, const CollisionShape& shape, const Vec3d& at,
                     MtdResult& mtd) noexcept {
        auto found = fixture.world->With<PrimitiveComponent>(
            component, [&](PrimitiveComponent& primitive) noexcept {
                return primitive.ComputePenetration(mtd, shape, at, Quatd());
            });
        return found && found.Value();
    }

    BodyWorldFixture fixture;
    bool started = false;
};

} // namespace

// 상자 body와 겹친 구의 MTD는 상자 밖(+X)으로 겹친 깊이만큼이다. 닿기만 하면 거리 0, 떨어지면
// 거짓, body 없는(NoCollision) 컴포넌트는 거짓이다.
TEST(PhysicsPenetration_ComputePenetrationGivesMinimumTranslation) {
    PenetrationWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle box = scene.Wall(Vec3d::Zero(), Vec3d(BoxHalf, BoxHalf, BoxHalf));
    const ObjectHandle ghost =
        scene.fixture.SpawnShape<BoxComponent>([](BoxComponent& shape) noexcept {
            (void)shape.SetCollisionEnabled(CollisionEnabled::NoCollision);
        });
    REQUIRE(!box.IsNull() && !ghost.IsNull());
    const CollisionShape probe = CollisionShape::Sphere(ProbeRadius);

    MtdResult mtd;
    REQUIRE(scene.Penetration(box, probe, Vec3d(OverlapX, 0.0, 0.0), mtd));
    CHECK(Near(mtd.direction.x, 1.0, DirectionTolerance));
    CHECK(Near(mtd.direction.y, 0.0, DirectionTolerance));
    CHECK(Near(mtd.direction.z, 0.0, DirectionTolerance));
    CHECK(Near(mtd.distance, OverlapDepth, DepthTolerance));

    MtdResult touching;
    REQUIRE(scene.Penetration(box, probe, Vec3d(TouchX, 0.0, 0.0), touching));
    CHECK(Near(touching.distance, 0.0, DepthTolerance));

    MtdResult apart;
    apart.distance = -1.0;
    CHECK(!scene.Penetration(box, probe, Vec3d(ApartX, 0.0, 0.0), apart));
    CHECK(apart.distance == -1.0);
    CHECK(!scene.Penetration(ghost, probe, Vec3d::Zero(), apart));
    // Line 형상은 겹침이 없다.
    CHECK(!scene.Penetration(box, CollisionShape{}, Vec3d::Zero(), apart));
}

// 형상 컴포넌트의 충돌 형상은 scale과 부풀리기를 반영하고 0 아래로 줄지 않는다(UE
// GetCollisionShape·FCollisionShape::Inflate).
TEST(PhysicsPenetration_CollisionShapeFollowsScaleAndInflation) {
    const CollisionShape sphere = CollisionShape::Sphere(ProbeRadius);
    CHECK(sphere.Inflate(0.0).radius == ProbeRadius);
    CHECK(sphere.Inflate(Inflation).radius == ProbeRadius + Inflation);
    CHECK(sphere.Inflate(-2.0 * ProbeRadius).radius == 0.0);
    const CollisionShape capsule =
        CollisionShape::Capsule(BoxHalf, ProbeRadius).Inflate(-ProbeRadius);
    CHECK(capsule.radius == 0.0);
    CHECK(capsule.halfHeight == 0.0);
    const CollisionShape box = CollisionShape::Box(Vec3d(1.0, 2.0, 3.0)).Inflate(-1.5);
    CHECK(box.halfExtent == Vec3d(0.0, 0.5, 1.5));
    const CollisionShape line = CollisionShape{}.Inflate(Inflation);
    CHECK(line.kind == CollisionShapeKind::Line);

    PenetrationWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle ball =
        scene.fixture.SpawnShape<SphereComponent>([](SphereComponent& shape) noexcept {
            (void)shape.SetSphereRadius(static_cast<float>(ProbeRadius));
            (void)shape.SetRelativeScale(Vec3d(ShapeScale, ShapeScale, ShapeScale));
        });
    const ObjectHandle crate =
        scene.fixture.SpawnShape<BoxComponent>([](BoxComponent& shape) noexcept {
            (void)shape.SetBoxExtent(Vec3d(BoxHalf, BoxHalf, BoxHalf));
            (void)shape.SetRelativeScale(Vec3d(1.0, ShapeScale, 1.0));
        });
    const ObjectHandle pill =
        scene.fixture.SpawnShape<CapsuleComponent>([](CapsuleComponent& shape) noexcept {
            (void)shape.SetCapsuleSize(static_cast<float>(BoxHalf),
                                       static_cast<float>(ProbeRadius));
        });
    REQUIRE(!ball.IsNull() && !crate.IsNull() && !pill.IsNull());
    auto shapeOf = [&](ObjectHandle component) noexcept {
        CollisionShape shape;
        (void)Apply<PrimitiveComponent>(*scene.fixture.world, component,
                                        [&](PrimitiveComponent& primitive) noexcept {
                                            shape = primitive.GetCollisionShape(Inflation);
                                        });
        return shape;
    };

    const CollisionShape ballShape = shapeOf(ball);
    CHECK(ballShape.kind == CollisionShapeKind::Sphere);
    CHECK(Near(ballShape.radius, ProbeRadius * ShapeScale + Inflation, PositionCheckTolerance));
    const CollisionShape crateShape = shapeOf(crate);
    CHECK(crateShape.kind == CollisionShapeKind::Box);
    CHECK(Near(crateShape.halfExtent.y, BoxHalf * ShapeScale + Inflation, PositionCheckTolerance));
    CHECK(Near(crateShape.halfExtent.x, BoxHalf + Inflation, PositionCheckTolerance));
    const CollisionShape pillShape = shapeOf(pill);
    CHECK(pillShape.kind == CollisionShapeKind::Capsule);
    CHECK(Near(pillShape.radius, BoxHalf + Inflation, PositionCheckTolerance));
    CHECK(Near(pillShape.halfHeight, ProbeRadius + Inflation, PositionCheckTolerance));
}

// 조정량: 시작 겹침이 아니면 0, 깊이가 없으면 기본 깊이, 평면 제약을 적용한다.
TEST(PhysicsPenetration_AdjustmentFollowsUnrealRule) {
    PenetrationWorld scene;
    REQUIRE(scene.started);
    ObjectHandle box;
    ObjectHandle movement;
    REQUIRE(scene.Mover(0.0, box, movement));
    HitResult hit;
    hit.normal = Vec3d(1.0, 0.0, 0.0);
    hit.penetrationDepth = StuckDepth;
    auto adjust = [&]() noexcept {
        Vec3d value = Vec3d(-1.0, -1.0, -1.0);
        (void)Apply<MovementComponent>(*scene.fixture.world, movement,
                                       [&](MovementComponent& mover) noexcept {
                                           value = mover.GetPenetrationAdjustment(hit);
                                       });
        return value;
    };

    CHECK(adjust() == Vec3d::Zero());
    hit.startPenetrating = true;
    CHECK(Near(adjust().x, StuckDepth + MovementComponent::PenetrationPullbackDistance,
               PositionCheckTolerance));
    hit.penetrationDepth = 0.0;
    CHECK(Near(adjust().x,
               MovementComponent::DefaultPenetrationDepth +
                   MovementComponent::PenetrationPullbackDistance,
               PositionCheckTolerance));
    // 평면 제약(법선 X): X 성분이 빠진다.
    hit.normal = Vec3d(0.0, 1.0, 0.0) + Vec3d(1.0, 0.0, 0.0);
    REQUIRE(Apply<MovementComponent>(*scene.fixture.world, movement,
                                     [](MovementComponent& mover) noexcept {
                                         mover.SetConstrainToPlane(true);
                                         (void)mover.SetPlaneConstraintNormal(Vec3d(1.0, 0.0, 0.0));
                                     }));
    const Vec3d constrained = adjust();
    CHECK(Near(constrained.x, 0.0, PositionCheckTolerance));
    CHECK(constrained.y > 0.0);
}

// 벽에 5 cm 박힌 상자를 옆으로 옮기면: 먼저 박혀 못 움직이고(시작 겹침) 밀어낸 자리가 비어 있어
// teleport로 빠진 뒤 원래 이동을 마친다.
TEST(PhysicsPenetration_SafeMoveTeleportsOutThenMoves) {
    PenetrationWorld scene;
    REQUIRE(scene.started);
    REQUIRE(!scene.Wall(Vec3d::Zero(), Vec3d(BoxHalf, BoxHalf, BoxHalf)).IsNull());
    ObjectHandle box;
    ObjectHandle movement;
    REQUIRE(scene.Mover(StuckX, box, movement));
    scene.fixture.Step(1);

    HitResult hit;
    auto moved = scene.SafeMove(movement, Vec3d(0.0, SideMove, 0.0), hit);
    REQUIRE(moved);
    CHECK(moved.Value());
    CHECK(!hit.startPenetrating);
    CHECK(!hit.blockingHit);
    const Vec3d at = scene.Location(box);
    const double pushedX = StuckX + StuckDepth + MovementComponent::PenetrationPullbackDistance;
    CHECK(Near(at.x, pushedX, DepthTolerance));
    CHECK(Near(at.y, SideMove, PositionCheckTolerance));
}

// 밀어낸 자리가 다른 벽에 걸리면(부풀린 겹침 시험) teleport하지 않고 sweep으로 나올 수 있는 만큼
// 나온다(두 번째 벽 앞에서 멈춘다).
TEST(PhysicsPenetration_ResolveSweepsWhenTeleportIsBlocked) {
    PenetrationWorld scene;
    REQUIRE(scene.started);
    REQUIRE(!scene.Wall(Vec3d::Zero(), Vec3d(BoxHalf, BoxHalf, BoxHalf)).IsNull());
    REQUIRE(!scene
                 .Wall(Vec3d(SecondWallX, 0.0, 0.0),
                       Vec3d(SecondWallHalf, SecondWallSize, SecondWallSize))
                 .IsNull());
    ObjectHandle box;
    ObjectHandle movement;
    REQUIRE(scene.Mover(StuckX, box, movement));
    scene.fixture.Step(1);

    HitResult hit;
    auto moved = scene.SafeMove(movement, Vec3d(0.0, SideMove, 0.0), hit);
    REQUIRE(moved);
    const Vec3d at = scene.Location(box);
    // 두 번째 벽(왼쪽 면 30)에 닿기 전에 멈췄고 처음보다 벽 밖으로 나왔다.
    const double secondWallFace = SecondWallX - SecondWallHalf;
    CHECK(at.x + BoxHalf <= secondWallFace);
    CHECK(at.x > StuckX + StuckDepth - StopTolerance);
    // 아직 첫 벽에 pullback만큼 걸쳐 있어 재시도(옆 이동)는 시작 겹침에 막힌다(UE와 같다 — 다음
    // SafeMove가 다시 밀어낸다).
    CHECK(!moved.Value());
    CHECK(hit.startPenetrating);
    CHECK(Near(at.y, 0.0, PositionCheckTolerance));
}

// 두 번째 벽 Actor가 이동 무시 목록에 있으면 밀어낸 자리 겹침 시험에서도 빠져 teleport한다(UE
// InitSweepCollisionParams).
TEST(PhysicsPenetration_OverlapTestSkipsMoveIgnoredActors) {
    PenetrationWorld scene;
    REQUIRE(scene.started);
    REQUIRE(!scene.Wall(Vec3d::Zero(), Vec3d(BoxHalf, BoxHalf, BoxHalf)).IsNull());
    const ObjectHandle second = scene.Wall(Vec3d(SecondWallX, 0.0, 0.0),
                                           Vec3d(SecondWallHalf, SecondWallSize, SecondWallSize));
    REQUIRE(!second.IsNull());
    ObjectHandle box;
    ObjectHandle movement;
    REQUIRE(scene.Mover(StuckX, box, movement));
    auto secondOwner = scene.fixture.world->With<Component>(
        second, [](Component& value) noexcept { return value.GetOwner(); });
    REQUIRE(secondOwner);
    REQUIRE(scene.fixture.world->With<PrimitiveComponent>(
        box, [&](PrimitiveComponent& primitive) noexcept {
            return primitive.IgnoreActorWhenMoving(secondOwner.Value(), true);
        }));
    scene.fixture.Step(1);

    HitResult hit;
    auto moved = scene.SafeMove(movement, Vec3d(0.0, SideMove, 0.0), hit);
    REQUIRE(moved);
    CHECK(moved.Value());
    const Vec3d at = scene.Location(box);
    CHECK(Near(at.x, StuckX + StuckDepth + MovementComponent::PenetrationPullbackDistance,
               DepthTolerance));
    CHECK(Near(at.y, SideMove, PositionCheckTolerance));
}

// 평면 제약으로 조정량이 0이 되면 해소하지 않는다(SafeMove는 첫 이동 결과 그대로).
TEST(PhysicsPenetration_PlaneConstraintCanBlockResolution) {
    PenetrationWorld scene;
    REQUIRE(scene.started);
    REQUIRE(!scene.Wall(Vec3d::Zero(), Vec3d(BoxHalf, BoxHalf, BoxHalf)).IsNull());
    ObjectHandle box;
    ObjectHandle movement;
    REQUIRE(scene.Mover(StuckX, box, movement));
    REQUIRE(Apply<MovementComponent>(*scene.fixture.world, movement,
                                     [](MovementComponent& mover) noexcept {
                                         mover.SetConstrainToPlane(true);
                                         (void)mover.SetPlaneConstraintNormal(Vec3d(1.0, 0.0, 0.0));
                                     }));
    scene.fixture.Step(1);

    HitResult hit;
    auto moved = scene.SafeMove(movement, Vec3d(0.0, SideMove, 0.0), hit);
    REQUIRE(moved);
    CHECK(!moved.Value());
    CHECK(hit.startPenetrating);
    CHECK(Near(scene.Location(box).x, StuckX, PositionCheckTolerance));
}

// 갱신 대상이 없으면 SafeMove는 hit를 비우고(time 1) 거짓이다.
TEST(PhysicsPenetration_SafeMoveWithoutTargetResetsHit) {
    PenetrationWorld scene;
    REQUIRE(scene.started);
    World& world = *scene.fixture.world;
    auto actor = world.SpawnActorDeferred<PhysicsTestActor>();
    REQUIRE(actor);
    auto mover = world.AddComponent<MovementComponent>(actor.Value());
    REQUIRE(mover);
    REQUIRE(Apply<MovementComponent>(world, mover.Value(), [](MovementComponent& value) noexcept {
        value.SetAutoRegisterUpdatedComponent(false);
    }));
    REQUIRE(world.FinishSpawning(actor.Value()));

    HitResult hit;
    hit.time = StaleHitTime;
    hit.blockingHit = true;
    auto moved = scene.SafeMove(mover.Value(), Vec3d(0.0, SideMove, 0.0), hit);
    REQUIRE(moved);
    CHECK(!moved.Value());
    CHECK(hit.time == 1.0);
    CHECK(!hit.blockingHit);
}

// 회전한 body와 회전한 질의 형상의 MTD다: Z축 45° 벽은 면 법선 방향으로, 눕힌 캡슐은 위(+Z)로 겹친
// 깊이만큼이다(세운 캡슐이면 훨씬 깊다 — 질의 회전이 들어간다).
TEST(PhysicsPenetration_ComputePenetrationFollowsRotation) {
    PenetrationWorld scene;
    REQUIRE(scene.started);
    const Quatd yaw = Quatd::FromUnitAxisAngle(Vec3d::Up(), math::Pi<double> / 4.0);
    const ObjectHandle turned =
        scene.fixture.SpawnShape<BoxComponent>([&](BoxComponent& box) noexcept {
            box.SetMobility(ComponentMobility::Static);
            (void)box.SetBoxExtent(Vec3d(BoxHalf, BoxHalf, BoxHalf));
            (void)box.SetRelativeRotation(yaw);
        });
    REQUIRE(!turned.IsNull());
    const Vec3d faceNormal = yaw.RotateVector(Vec3d(1.0, 0.0, 0.0));
    MtdResult mtd;
    REQUIRE(
        scene.Penetration(turned, CollisionShape::Sphere(ProbeRadius), faceNormal * OverlapX, mtd));
    CHECK(Near(mtd.direction.x, faceNormal.x, DirectionTolerance));
    CHECK(Near(mtd.direction.y, faceNormal.y, DirectionTolerance));
    CHECK(Near(mtd.distance, OverlapDepth, DepthTolerance));

    const ObjectHandle block = scene.Wall(Vec3d(0.0, 0.0, 0.0), Vec3d(BoxHalf, BoxHalf, BoxHalf));
    REQUIRE(!block.IsNull());
    const Quatd lying = Quatd::FromUnitAxisAngle(Vec3d(0.0, 1.0, 0.0), math::HalfPi<double>);
    MtdResult capsule;
    bool found = false;
    REQUIRE(Apply<PrimitiveComponent>(
        *scene.fixture.world, block, [&](PrimitiveComponent& primitive) noexcept {
            found = primitive.ComputePenetration(
                capsule, CollisionShape::Capsule(CapsuleRadius, CapsuleHalfHeight),
                Vec3d(0.0, 0.0, CapsuleCenterZ), lying);
        }));
    REQUIRE(found);
    CHECK(Near(capsule.direction.z, 1.0, DirectionTolerance));
    CHECK(Near(capsule.distance, CapsuleDepth, DepthTolerance));
}

// 장면 접점의 오류 경로는 거짓이고 out을 건드리지 않는다: 옛 handle, 0 회전, 비유한 위치,
// Cylinder(CY2 전).
TEST(PhysicsPenetration_SceneRejectsInvalidQueries) {
    PenetrationWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle box = scene.Wall(Vec3d::Zero(), Vec3d(BoxHalf, BoxHalf, BoxHalf));
    REQUIRE(!box.IsNull());
    const PhysicsBodyHandle body = scene.fixture.Body(box);
    REQUIRE(!body.IsNull());
    PhysicsScene& physics = *scene.fixture.world->GetPhysicsScene();
    const CollisionShape probe = CollisionShape::Sphere(ProbeRadius);
    const Vec3d inside(OverlapX, 0.0, 0.0);
    MtdResult out;
    out.distance = -1.0;
    REQUIRE(physics.ComputePenetration(body, probe, inside, Quatd(), out));
    out.distance = -1.0;
    CHECK(!physics.ComputePenetration(PhysicsBodyHandle{body.index, body.generation + 1}, probe,
                                      inside, Quatd(), out));
    CHECK(!physics.ComputePenetration(body, probe, inside, Quatd(0.0, 0.0, 0.0, 0.0), out));
    const double infinity = platform::PlatformMath::Infinity<double>();
    CHECK(!physics.ComputePenetration(body, probe, Vec3d(infinity, 0.0, 0.0), Quatd(), out));
    CHECK(!physics.ComputePenetration(body, CollisionShape::Cylinder(ProbeRadius, ProbeRadius),
                                      inside, Quatd(), out));
    CHECK(out.distance == -1.0);
}

// 박히지 않았으면 보통 sweep이다: 벽 앞에서 막히고 hit는 시작 겹침이 아니다.
TEST(PhysicsPenetration_SafeMoveWithoutPenetrationSweeps) {
    PenetrationWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle wall = scene.Wall(Vec3d::Zero(), Vec3d(BoxHalf, BoxHalf, BoxHalf));
    REQUIRE(!wall.IsNull());
    ObjectHandle box;
    ObjectHandle movement;
    REQUIRE(scene.Mover(FreeStartX, box, movement));
    scene.fixture.Step(1);

    HitResult hit;
    auto moved = scene.SafeMove(movement, Vec3d(FreeMove, 0.0, 0.0), hit);
    REQUIRE(moved);
    CHECK(moved.Value());
    CHECK(hit.blockingHit);
    CHECK(!hit.startPenetrating);
    CHECK(hit.component == wall);
    CHECK(Near(scene.Location(box).x, 2.0 * BoxHalf, StopTolerance));
    CHECK(scene.Location(box).x >= 2.0 * BoxHalf);
}

// Primitive가 아닌 갱신 대상은 sweep 없이 옮기고(벽을 지나간다), 해소할 것이 없다.
TEST(PhysicsPenetration_NonPrimitiveTargetMovesWithoutSweep) {
    PenetrationWorld scene;
    REQUIRE(scene.started);
    REQUIRE(!scene.Wall(Vec3d::Zero(), Vec3d(BoxHalf, BoxHalf, BoxHalf)).IsNull());
    World& world = *scene.fixture.world;
    auto actor = world.SpawnActorDeferred<PhysicsTestActor>();
    REQUIRE(actor);
    auto pivot = world.AddComponent<SceneComponent>(actor.Value());
    auto mover = world.AddComponent<MovementComponent>(actor.Value());
    REQUIRE(pivot && mover);
    REQUIRE(Apply<SceneComponent>(world, pivot.Value(), [](SceneComponent& scene) noexcept {
        (void)scene.SetRelativeLocation(Vec3d(FreeStartX, 0.0, 0.0));
    }));
    REQUIRE(world.With<MovementComponent>(mover.Value(), [&](MovementComponent& value) noexcept {
        value.SetAutoRegisterUpdatedComponent(false);
        return value.SetUpdatedComponent(pivot.Value());
    }));
    REQUIRE(world.FinishSpawning(actor.Value()));

    HitResult hit;
    Result<bool> moved = Result<bool>::Failure(ErrorCode::NotFound);
    Result<bool> resolved = Result<bool>::Failure(ErrorCode::NotFound);
    REQUIRE(Apply<MovementComponent>(world, mover.Value(), [&](MovementComponent& value) noexcept {
        moved = value.MoveUpdatedComponent(Vec3d(FreeMove, 0.0, 0.0), Quatd(), true, &hit,
                                           TeleportMode::None);
        HitResult stuck;
        stuck.startPenetrating = true;
        stuck.normal = Vec3d(1.0, 0.0, 0.0);
        resolved = value.ResolvePenetration(value.GetPenetrationAdjustment(stuck), stuck, Quatd());
    }));
    REQUIRE(moved);
    CHECK(moved.Value());
    CHECK(!hit.blockingHit);
    CHECK(hit.traceStart == Vec3d(FreeStartX, 0.0, 0.0));
    CHECK(hit.traceEnd == Vec3d(FreeStartX + FreeMove, 0.0, 0.0));
    REQUIRE(resolved);
    CHECK(!resolved.Value());
    CHECK(Near(world
                   .With<SceneComponent>(
                       pivot.Value(),
                       [](SceneComponent& scene) noexcept { return scene.WorldLocation().x; })
                   .Value(),
               FreeStartX + FreeMove, PositionCheckTolerance));
}

} // namespace physics_penetration_tests_file
