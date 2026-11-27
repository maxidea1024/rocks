// Physics 모듈 PH4a 시험이다(Docs/Design/PhysicsModule 5.1절, 7.1.10절): PrimitiveComponent::
// MoveComponent의 sweep 이동(pullback·최소 이동·시작 겹침·이동 무시 목록·NoCollision·Static),
// sweep hit 발행 순서(자기 Actor → 상대 Actor, 뒤집은 법선, 시작 겹침 발행 끄기, 발행 중 파괴),
// teleport의 body 이동. World·컴포넌트 공개 API만 쓰므로 Modular 시험에서도 돈다.
#include "PhysicsBodyFixture.h"

#include <Base/InitializerList.h>
#include <World/Physics/MoveOptions.h>
#include <World/Physics/WorldCollision.h>

using namespace rocks;
namespace physics_move_tests_file {

using namespace physics_test_support;

namespace {

constexpr double BoxHalf = 10.0;
constexpr double WallX = 100.0;
constexpr double MoveDistance = 200.0;
constexpr double MoveTolerance = 0.05; // cm
// 한 World에 여러 이동 상자를 둘 때 서로 겹치지 않게 벌리는 z 간격과 그 줄을 모두 막는 벽 반높이다.
constexpr double LaneSpacing = BoxHalf * 3.0;
constexpr double TallWallHalf = LaneSpacing * 2.0;

// OnHit 기록 하나다.
struct HitRecord {
    ObjectHandle actor;
    ObjectHandle myComponent;
    ObjectHandle otherActor;
    ObjectHandle otherComponent;
    bool selfMoved = false;
    Vec3d impactNormal;
};

constexpr int MaxRecords = 8;

// OnHit을 기록하는 Actor다(기록은 시험 하나 안에서만 쓴다).
class HitRecordingActor final : public Actor {
    ROCKS_NATIVE_TYPE(HitRecordingActor, Actor, 0x6c2e91f04b7a3d58ull, 0x1f8a7c3e5d906b24ull)

public:
    explicit HitRecordingActor(Allocator& allocator) noexcept : Actor(allocator) {
    }

    void OnHit(ObjectHandle myComponent, ObjectHandle otherActor, ObjectHandle otherComponent,
               bool selfMoved, const Vec3d&, const HitResult& hit) noexcept override {
        if (count < MaxRecords) {
            records[count++] = {Handle(),       myComponent, otherActor,
                                otherComponent, selfMoved,   hit.impactNormal};
        }

        // 발행 중 상대 컴포넌트를 파괴한다(UE — 상대 쪽 발행이 없어야 한다).
        if (destroyOtherOnHit && selfMoved) {
            (void)GetWorld().DestroyComponent(otherComponent);
        }
    }

    static inline HitRecord records[MaxRecords];
    static inline int count = 0;
    static inline bool destroyOtherOnHit = false;

    static void Reset() noexcept {
        count = 0;
        destroyOtherOnHit = false;
    }
};

// 고정 장치에 기록 Actor 클래스를 등록한다.
struct MoveWorld {
    MoveWorld() noexcept {
        auto registeredToken = RegisterNativeClass<HitRecordingActor>(fixture.types);
        registered = registeredToken.IsOk();
        if (registeredToken) {
            token = registeredToken.Value();
        }

        HitRecordingActor::Reset();
        started = fixture.registered && registered && fixture.world &&
                  fixture.world->Initialize() && fixture.world->BeginPlay();
    }

    ~MoveWorld() {
        if (fixture.world) {
            (void)fixture.world->Shutdown();
        }

        if (registered) {
            CHECK(fixture.types.Unregister(token).IsOk());
        }
    }

    // x에 반폭 BoxHalf 상자를 기록 Actor로 둔다.
    template <class Configure>
    ObjectHandle Box(double x, Configure&& configure) noexcept {
        return fixture.SpawnShapeOn<HitRecordingActor, BoxComponent>(
            [&](BoxComponent& box) noexcept {
                (void)box.SetBoxExtent(Vec3d(BoxHalf, BoxHalf, BoxHalf));
                (void)box.SetRelativeLocation(Vec3d(x, 0.0, 0.0));
                configure(box);
            });
    }

    ObjectHandle Box(double x) noexcept {
        return Box(x, [](BoxComponent&) noexcept {});
    }

    // 여러 줄(z = −LaneSpacing·0·+LaneSpacing)을 모두 막는 벽이다.
    ObjectHandle TallWall() noexcept {
        return Box(WallX, [](BoxComponent& box) noexcept {
            (void)box.SetBoxExtent(Vec3d(BoxHalf, BoxHalf, TallWallHalf));
        });
    }

    // z 줄에 반폭 BoxHalf 상자를 둔다.
    template <class Configure>
    ObjectHandle LaneBox(double x, double lane, Configure&& configure) noexcept {
        return Box(x, [&](BoxComponent& box) noexcept {
            (void)box.SetRelativeLocation(Vec3d(x, 0.0, lane * LaneSpacing));
            configure(box);
        });
    }

    ObjectHandle LaneBox(double x, double lane) noexcept {
        return LaneBox(x, lane, [](BoxComponent&) noexcept {});
    }

    Result<bool> Move(ObjectHandle component, const Vec3d& delta, bool sweep, HitResult* hit,
                      const MoveOptions& options = {}) noexcept {
        Result<bool> moved = Result<bool>::Failure(ErrorCode::NotFound);
        (void)fixture.world->With<PrimitiveComponent>(
            component, [&](PrimitiveComponent& primitive) noexcept {
                moved = primitive.MoveComponent(delta, primitive.WorldQuaternion(), sweep, hit,
                                                options);
                return Result<void>::Success();
            });
        return moved;
    }

    double X(ObjectHandle component) noexcept {
        return fixture.ComponentPose(component).translation.x;
    }

    ObjectHandle Owner(ObjectHandle component) noexcept {
        ObjectHandle owner;
        (void)fixture.world->With<Component>(component, [&](Component& value) noexcept {
            owner = value.GetOwner();
            return Result<void>::Success();
        });
        return owner;
    }

    BodyWorldFixture fixture;
    TypeRegistrationToken token;
    bool registered = false;
    bool started = false;
};

// UE PullBackHit: D > 10 cm면 time에서 1/D + 0.001을 뺀다.
double PulledBackTime(double rawTime, double distance) noexcept {
    return rawTime - (1.0 / distance + 0.001);
}

} // namespace

// sweep은 벽 앞 pullback 자리에 멈추고 막는 hit를 돌려준다. sweep 없는 이동은 벽을 지나간다.
TEST(PhysicsMove_SweepStopsBeforeWall) {
    MoveWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle mover = scene.Box(0.0);
    const ObjectHandle wall = scene.Box(WallX);
    REQUIRE(!mover.IsNull() && !wall.IsNull());

    HitResult hit;
    auto moved = scene.Move(mover, Vec3d(MoveDistance, 0.0, 0.0), true, &hit);
    REQUIRE(moved);
    CHECK(moved.Value());
    // 맞닿는 중심 x = 벽 앞면 − 반폭, raw time = 그 거리 / 이동 거리.
    const double rawTime = (WallX - BoxHalf * 2.0) / MoveDistance;
    const double time = PulledBackTime(rawTime, MoveDistance);
    CHECK(hit.blockingHit);
    CHECK(hit.component == wall);
    CHECK(Near(hit.time, time, 1.0e-4));
    CHECK(Near(scene.X(mover), MoveDistance * time, MoveTolerance));

    // sweep 없는 이동은 벽을 지나간다.
    REQUIRE(scene.Move(mover, Vec3d(MoveDistance, 0.0, 0.0), false, &hit));
    CHECK(!hit.blockingHit);
    CHECK(Near(scene.X(mover), MoveDistance * (time + 1.0), MoveTolerance));
}

// 최소 이동(4·KindaSmallNumber 이하)과 같은 회전은 움직이지 않고 참이다. outHit는 빈 hit다.
TEST(PhysicsMove_TinyMoveDoesNothing) {
    MoveWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle mover = scene.Box(0.0);
    REQUIRE(!mover.IsNull());
    const Vec3d tiny(math::KindaSmallNumber<double>, 0.0, 0.0);
    HitResult hit;
    auto moved = scene.Move(mover, tiny, true, &hit);
    REQUIRE(moved);
    CHECK(moved.Value());
    CHECK(scene.X(mover) == 0.0);
    CHECK(!hit.blockingHit);
    CHECK(hit.traceEnd == tiny);
}

// 시작 겹침(UE ShouldIgnoreHitResult): 빠져나가는 이동은 무시되어 끝까지 가고, 들어가는 이동은
// 시작에 머문다. neverIgnoreBlockingOverlaps면 빠져나가는 이동도 막힌다.
TEST(PhysicsMove_InitialOverlapDirection) {
    constexpr double Overlapping = WallX - BoxHalf * 2.0 + 5.0;
    MoveWorld scene;
    REQUIRE(scene.started);
    REQUIRE(!scene.TallWall().IsNull());
    const ObjectHandle leaving = scene.LaneBox(Overlapping, 0.0);
    const ObjectHandle entering = scene.LaneBox(Overlapping, 1.0);
    const ObjectHandle stuck = scene.LaneBox(Overlapping, -1.0);
    REQUIRE(!leaving.IsNull() && !entering.IsNull() && !stuck.IsNull());
    HitResult hit;
    REQUIRE(scene.Move(leaving, Vec3d(-MoveDistance, 0.0, 0.0), true, &hit));
    CHECK(!hit.blockingHit);
    CHECK(Near(scene.X(leaving), Overlapping - MoveDistance, MoveTolerance));

    auto moved = scene.Move(entering, Vec3d(MoveDistance, 0.0, 0.0), true, &hit);
    REQUIRE(moved);
    CHECK(!moved.Value());
    CHECK(hit.blockingHit && hit.startPenetrating);
    CHECK(hit.time == 0.0);
    CHECK(scene.X(entering) == Overlapping);

    MoveOptions strict;
    strict.neverIgnoreBlockingOverlaps = true;
    REQUIRE(scene.Move(stuck, Vec3d(-MoveDistance, 0.0, 0.0), true, &hit, strict));
    CHECK(hit.blockingHit && hit.startPenetrating);
    CHECK(scene.X(stuck) == Overlapping);
}

// hit 발행(UE DispatchBlockingHit): 자기 Actor가 먼저(selfMoved 참, 맞은 면 법선), 그다음 상대
// Actor(selfMoved 거짓, 뒤집은 법선)다.
TEST(PhysicsMove_HitDispatchOrder) {
    MoveWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle mover = scene.Box(0.0);
    const ObjectHandle wall = scene.Box(WallX);
    REQUIRE(!mover.IsNull() && !wall.IsNull());
    REQUIRE(scene.Move(mover, Vec3d(MoveDistance, 0.0, 0.0), true, nullptr));
    REQUIRE(HitRecordingActor::count == 2);
    const HitRecord& self = HitRecordingActor::records[0];
    const HitRecord& other = HitRecordingActor::records[1];
    CHECK(self.actor == scene.Owner(mover));
    CHECK(self.myComponent == mover);
    CHECK(self.otherActor == scene.Owner(wall));
    CHECK(self.otherComponent == wall);
    CHECK(self.selfMoved);
    CHECK(Near(self.impactNormal.x, -1.0, 1.0e-4));
    CHECK(other.actor == scene.Owner(wall));
    CHECK(other.myComponent == wall);
    CHECK(other.otherActor == scene.Owner(mover));
    CHECK(other.otherComponent == mover);
    CHECK(!other.selfMoved);
    CHECK(Near(other.impactNormal.x, 1.0, 1.0e-4));
}

// 시작 겹침 hit는 disableBlockingOverlapDispatch면 발행하지 않는다. 발행 중 상대 컴포넌트를
// 파괴하면 상대 쪽 발행이 없다.
TEST(PhysicsMove_HitDispatchOptionsAndDestruction) {
    constexpr double Overlapping = WallX - BoxHalf * 2.0 + 5.0;
    MoveWorld scene;
    REQUIRE(scene.started);
    REQUIRE(!scene.Box(WallX).IsNull());
    const ObjectHandle inside = scene.Box(Overlapping);
    REQUIRE(!inside.IsNull());
    MoveOptions quiet;
    quiet.disableBlockingOverlapDispatch = true;
    HitResult hit;
    REQUIRE(scene.Move(inside, Vec3d(MoveDistance, 0.0, 0.0), true, &hit, quiet));
    CHECK(hit.startPenetrating);
    CHECK(HitRecordingActor::count == 0);
    REQUIRE(scene.Move(inside, Vec3d(MoveDistance, 0.0, 0.0), true, &hit));
    CHECK(HitRecordingActor::count == 2);

    HitRecordingActor::Reset();
    HitRecordingActor::destroyOtherOnHit = true;
    REQUIRE(scene.Move(inside, Vec3d(MoveDistance, 0.0, 0.0), true, &hit));
    CHECK(HitRecordingActor::count == 1);
    CHECK(HitRecordingActor::records[0].selfMoved);
}

// 이동 무시 목록(Actor·컴포넌트)과 질의 충돌이 꺼진(PhysicsOnly) 컴포넌트는 벽을 지나간다.
TEST(PhysicsMove_IgnoreListsAndNoCollision) {
    MoveWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle wall = scene.TallWall();
    const ObjectHandle byActor = scene.LaneBox(0.0, 0.0);
    const ObjectHandle byComponent = scene.LaneBox(0.0, 1.0);
    // body는 있고 질의만 없는 컴포넌트다(NoCollision은 body가 없어 질의 충돌 검사를 시험하지
    // 못한다).
    const ObjectHandle quiet = scene.LaneBox(0.0, -1.0, [](BoxComponent& box) noexcept {
        (void)box.SetCollisionEnabled(CollisionEnabled::PhysicsOnly);
    });
    REQUIRE(!wall.IsNull() && !byActor.IsNull() && !byComponent.IsNull() && !quiet.IsNull());
    const ObjectHandle wallActor = scene.Owner(wall);
    World& world = *scene.fixture.world;
    REQUIRE(world.With<PrimitiveComponent>(byActor, [&](PrimitiveComponent& primitive) noexcept {
        return primitive.IgnoreActorWhenMoving(wallActor, true);
    }));
    REQUIRE(
        world.With<PrimitiveComponent>(byComponent, [&](PrimitiveComponent& primitive) noexcept {
            return primitive.IgnoreComponentWhenMoving(wall, true);
        }));
    const Vec3d delta(MoveDistance, 0.0, 0.0);
    HitResult hit;
    for (const ObjectHandle mover : {byActor, byComponent, quiet}) {
        REQUIRE(scene.Move(mover, delta, true, &hit));
        CHECK(!hit.blockingHit);
        CHECK(Near(scene.X(mover), MoveDistance, MoveTolerance));
    }

    // 목록에서 빼면 다시 막힌다.
    REQUIRE(world.With<PrimitiveComponent>(byActor, [&](PrimitiveComponent& primitive) noexcept {
        return primitive.IgnoreActorWhenMoving(wallActor, false);
    }));
    REQUIRE(scene.Move(byActor, Vec3d(-MoveDistance, 0.0, 0.0), false, nullptr));
    REQUIRE(scene.Move(byActor, delta, true, &hit));
    CHECK(hit.blockingHit);
    CHECK(HitRecordingActor::count >= 1);
}

// teleport(7.1.10절): TeleportPhysics 이동은 다음 step 속도가 0이고 None은 이동량/dt다. 부착된
// 자식 body도 같은 방식이다. Static 컴포넌트 이동은 오류다.
TEST(PhysicsMove_TeleportAppliesToChildrenAndStaticFails) {
    constexpr double Shift = 50.0;
    constexpr double ChildX = 300.0;
    MoveWorld scene;
    REQUIRE(scene.started);
    BodyWorldFixture& fixture = scene.fixture;
    ObjectHandle parent;
    ObjectHandle child;
    REQUIRE((fixture.SpawnAttached<BoxComponent, BoxComponent>(
        parent, child, [](BoxComponent&) noexcept {},
        [](BoxComponent& box) noexcept {
            (void)box.SetRelativeLocation(Vec3d(ChildX, 0.0, 0.0));
        })));
    const Vec3d delta(Shift, 0.0, 0.0);
    MoveOptions teleport;
    teleport.teleport = TeleportMode::TeleportPhysics;
    REQUIRE(scene.Move(parent, delta, false, nullptr, teleport));
    fixture.Step(1);
    CHECK(fixture.Kinematics(parent).linearVelocity == Vec3d::Zero());
    CHECK(fixture.Kinematics(child).linearVelocity == Vec3d::Zero());

    REQUIRE(scene.Move(parent, delta, false, nullptr));
    fixture.Step(1);
    CHECK(
        Near(fixture.Kinematics(parent).linearVelocity.x, Shift / StepSeconds, VelocityTolerance));
    CHECK(Near(fixture.Kinematics(child).linearVelocity.x, Shift / StepSeconds, VelocityTolerance));

    const ObjectHandle pinned = scene.Box(
        -WallX, [](BoxComponent& box) noexcept { box.SetMobility(ComponentMobility::Static); });
    REQUIRE(!pinned.IsNull());
    auto moved = scene.Move(pinned, delta, true, nullptr);
    REQUIRE(!moved);
    CHECK(moved.Error() == ErrorCode::InvalidArgument);
    CHECK(scene.X(pinned) == -WallX);
}

// skipPhysicsMove(UE MOVECOMP_SkipPhysicsMove): 자기 body는 옮기지 않고 자식 body는 따라간다.
TEST(PhysicsMove_SkipPhysicsMoveKeepsOwnBody) {
    constexpr double Shift = 50.0;
    constexpr double ChildX = 300.0;
    MoveWorld scene;
    REQUIRE(scene.started);
    BodyWorldFixture& fixture = scene.fixture;
    ObjectHandle parent;
    ObjectHandle child;
    REQUIRE((fixture.SpawnAttached<BoxComponent, BoxComponent>(
        parent, child, [](BoxComponent&) noexcept {},
        [](BoxComponent& box) noexcept {
            (void)box.SetRelativeLocation(Vec3d(ChildX, 0.0, 0.0));
        })));
    MoveOptions skip;
    skip.skipPhysicsMove = true;
    REQUIRE(scene.Move(parent, Vec3d(Shift, 0.0, 0.0), false, nullptr, skip));
    CHECK(Near(scene.X(parent), Shift, MoveTolerance));
    CHECK(Near(fixture.Kinematics(parent).location.x, 0.0, MoveTolerance));
    CHECK(Near(fixture.Kinematics(child).location.x, ChildX + Shift, MoveTolerance));
}

// 회전만 바뀌는 sweep은 질의 없이 돌고 참이다. 벽이 pullback 거리보다 가까우면 time이 0으로 잘려
// 시작에 머문다(시작 겹침이 아닌 막음 — 도착 위치 검사).
TEST(PhysicsMove_RotationOnlyAndCloseWall) {
    constexpr double YawDegrees = 30.0;
    constexpr double Gap = 0.5; // cm — 이동 200 cm의 pullback(1.2 cm)보다 작다
    MoveWorld scene;
    REQUIRE(scene.started);
    const ObjectHandle wall = scene.Box(WallX);
    const ObjectHandle close = scene.Box(WallX - BoxHalf * 2.0 - Gap);
    REQUIRE(!wall.IsNull() && !close.IsNull());
    World& world = *scene.fixture.world;
    const Quatd yaw =
        Quatd::FromUnitAxisAngle(Vec3d(0.0, 0.0, 1.0), math::DegreesToRadians(YawDegrees));
    HitResult hit;
    Result<bool> turned = Result<bool>::Failure(ErrorCode::NotFound);
    REQUIRE(world.With<PrimitiveComponent>(close, [&](PrimitiveComponent& primitive) noexcept {
        turned = primitive.MoveComponent(Vec3d::Zero(), yaw, true, &hit, {});
        return Result<void>::Success();
    }));
    REQUIRE(turned);
    CHECK(turned.Value());
    CHECK(!hit.blockingHit);
    CHECK(AngularDistance(scene.fixture.ComponentPose(close).rotation, yaw) < 1.0e-6);
    // 회전을 되돌리고 벽 쪽으로 옮긴다.
    REQUIRE(world.With<PrimitiveComponent>(close, [](PrimitiveComponent& primitive) noexcept {
        return primitive.SetWorldLocationAndRotation(primitive.WorldLocation(), Quatd());
    }));
    const double before = scene.X(close);
    auto moved = scene.Move(close, Vec3d(MoveDistance, 0.0, 0.0), true, &hit);
    REQUIRE(moved);
    CHECK(!moved.Value());
    CHECK(hit.blockingHit);
    CHECK(!hit.startPenetrating);
    CHECK(hit.time == 0.0);
    CHECK(hit.component == wall);
    CHECK(scene.X(close) == before);
}

} // namespace physics_move_tests_file
