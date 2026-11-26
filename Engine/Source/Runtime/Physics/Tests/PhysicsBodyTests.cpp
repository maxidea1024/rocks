// Physics 모듈 PH2b–PH2d 시험이다(Docs/Design/PhysicsModule 2.3–2.5절, 7.1.3–7.1.5절):
// PrimitiveComponent 물리 시스템 상태의 body 수명, 형상·충돌·장면이 없으면 body 없음,
// Mobility·simulatePhysics → 운동 종류, 떨어지는 body의 운동 읽기, PA10을 CapsuleComponent 경로로
// (반구 포함 반높이), box scale을 형상에 굽기, 컴포넌트 이동이 body를 옮김, 결과 반영·Kinematic,
// 재생성 표(simulate 전환·떼기, 충돌 켬, 형상·scale, Mobility). World·컴포넌트 공개 API만 쓰므로
// Modular 시험에서도 돈다.
#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Physics/PhysicsWorldSubsystem.h>
#include <TestSupport/Test.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Physics/PhysicsScene.h>
#include <World/Primitives/BoxComponent.generated.h>
#include <World/Primitives/BoxComponent.h>
#include <World/Primitives/CapsuleComponent.generated.h>
#include <World/Primitives/CapsuleComponent.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/Primitives/ShapeComponent.generated.h>
#include <World/Primitives/SphereComponent.generated.h>
#include <World/Primitives/SphereComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>

using namespace rocks;
namespace physics_body_tests_file {

namespace {

constexpr double StepSeconds = 1.0 / 60.0;
constexpr int FallSteps = 30;
constexpr int SettleSteps = 180;
constexpr double GravityZ = -980.0; // cm/s²(PhysicsSettings 기본값)

constexpr double VelocityTolerance = 0.05; // cm/s
constexpr double PositionTolerance = 0.01; // cm
// 쌓여 정지한 형상의 높이 허용 오차: Jolt mPenetrationSlop 2 cm + 여유.
constexpr double RestHeightTolerance = 3.0; // cm

constexpr double FloorHalfExtent = 1000.0;
constexpr double FloorHalfThickness = 50.0;
constexpr double SphereRadius = 20.0;
constexpr double DropHeight = 200.0;
// 같은 World에 둔 형상끼리 서로 닿지 않게 벌리는 x 간격(cm)이다.
constexpr double SideOffset = 200.0;
// 고정 스텝 한 번에 떨어질 수 있는 거리보다 넉넉한 위치 허용 오차(cm — g·dt² ≈ 0.27 cm).
constexpr double OneStepFallTolerance = 1.0;

class PhysicsTestActor final : public Actor {
    ROCKS_NATIVE_TYPE(PhysicsTestActor, Actor, 0x4b8e2d6f91c03a57ull, 0x2f6c1e8a05b97d43ull)

public:
    explicit PhysicsTestActor(Allocator& allocator) noexcept : Actor(allocator) {
    }
};

// 물리 World 고정 장치다(설정 문맥 없음 — 충돌 profile 표가 비어 형상은 사용자 지정 값
// QueryAndPhysics·WorldStatic으로 해석된다).
struct BodyWorldFixture {
    explicit BodyWorldFixture(bool withPhysics = true, WorldKind kind = WorldKind::Game,
                              const WorldSubsystemType* after = nullptr) noexcept
        : types(allocator), modules(types, allocator) {
        registered = RegisterWorldTypes(modules).IsOk();
        auto actorToken = RegisterNativeClass<PhysicsTestActor>(types);
        registered = registered && actorToken.IsOk();
        if (actorToken) {
            token = actorToken.Value();
        }

        subsystems[0] = PhysicsWorldSubsystem::Type();
        usize subsystemCount = 1;
        if (after != nullptr) {
            subsystems[subsystemCount++] = *after;
        }

        WorldDesc desc;
        desc.name = "PhysicsBodies";
        desc.context = {0x50485953424f4459ull, 1};
        desc.timeStep = {StepSeconds, 0.25, 8};
        desc.kind = kind;
        if (withPhysics) {
            desc.subsystems = Span<const WorldSubsystemType>(subsystems, subsystemCount);
        }

        auto created = UniquePtr<World>::TryMake(allocator, allocator, types, desc);
        if (created) {
            world = Move(created.Value());
        }
    }

    ~BodyWorldFixture() {
        if (world) {
            (void)world->Shutdown();
        }

        world.Reset();
        if (registered) {
            CHECK(types.Unregister(token).IsOk());
        }

        CHECK(modules.Unregister().IsOk());
    }

    // 형상 컴포넌트 하나를 가진 Actor를 만든다. configure는 등록(body 생성) 전에 부른다.
    template <class T, class Configure>
    ObjectHandle SpawnShape(Configure&& configure) noexcept {
        auto actor = world->SpawnActorDeferred<PhysicsTestActor>();
        if (!actor) {
            return {};
        }

        auto component = world->AddComponent<T>(actor.Value());
        if (!component) {
            return {};
        }

        (void)world->With<T>(component.Value(), [&](T& shape) noexcept {
            configure(shape);
            return Result<void>::Success();
        });
        if (!world->FinishSpawning(actor.Value())) {
            return {};
        }

        return component.Value();
    }

    // 부모(ParentT)와 그에 붙은 자식(ChildT)을 가진 Actor를 만든다. configure는 등록 전에 부른다.
    template <class ParentT, class ChildT, class ConfigureParent, class ConfigureChild>
    bool SpawnAttached(ObjectHandle& parentOut, ObjectHandle& childOut,
                       ConfigureParent&& configureParent,
                       ConfigureChild&& configureChild) noexcept {
        auto actor = world->SpawnActorDeferred<PhysicsTestActor>();
        if (!actor) {
            return false;
        }

        auto parent = world->AddComponent<ParentT>(actor.Value());
        auto child = world->AddComponent<ChildT>(actor.Value());
        if (!parent || !child) {
            return false;
        }

        (void)world->With<ParentT>(parent.Value(), [&](ParentT& shape) noexcept {
            configureParent(shape);
            return Result<void>::Success();
        });
        auto attached = world->With<ChildT>(child.Value(), [&](ChildT& shape) noexcept {
            configureChild(shape);
            return shape.AttachTo(parent.Value());
        });
        if (!attached || !world->FinishSpawning(actor.Value())) {
            return false;
        }

        parentOut = parent.Value();
        childOut = child.Value();
        return true;
    }

    ObjectHandle Parent(ObjectHandle component) noexcept {
        ObjectHandle parent;
        (void)world->With<SceneComponent>(component, [&](SceneComponent& scene) noexcept {
            parent = scene.Parent();
            return Result<void>::Success();
        });
        return parent;
    }

    // 윗면이 z = 0인 정적 바닥(BoxComponent)이다.
    ObjectHandle SpawnFloor() noexcept {
        return SpawnShape<BoxComponent>([](BoxComponent& floor) noexcept {
            floor.SetMobility(ComponentMobility::Static);
            (void)floor.SetBoxExtent(Vec3d(FloorHalfExtent, FloorHalfExtent, FloorHalfThickness));
            (void)floor.SetRelativeLocation(Vec3d(0.0, 0.0, -FloorHalfThickness));
        });
    }

    PhysicsBodyHandle Body(ObjectHandle component) noexcept {
        PhysicsBodyHandle handle;
        (void)world->With<PrimitiveComponent>(component,
                                              [&](PrimitiveComponent& primitive) noexcept {
                                                  handle = primitive.BodyHandle();
                                                  return Result<void>::Success();
                                              });
        return handle;
    }

    BodyKinematics Kinematics(ObjectHandle component) noexcept {
        auto read = world->GetPhysicsScene()->ReadKinematics(Body(component));
        CHECK(read);
        return read ? read.Value() : BodyKinematics{};
    }

    // 컴포넌트 월드 위치·회전이다.
    Transform ComponentPose(ObjectHandle component) noexcept {
        Transform pose;
        (void)world->With<PrimitiveComponent>(component,
                                              [&](PrimitiveComponent& primitive) noexcept {
                                                  pose.translation = primitive.WorldLocation();
                                                  pose.rotation = primitive.WorldQuaternion();
                                                  return Result<void>::Success();
                                              });
        return pose;
    }

    void Step(int count) noexcept {
        for (int index = 0; index < count; ++index) {
            CHECK(world->Tick(StepSeconds));
        }
    }

    uint32 BodyCount() noexcept {
        return world->GetPhysicsScene()->Stats().bodies;
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    TypeRegistrationToken token;
    bool registered = false;
    WorldSubsystemType subsystems[2];
    UniquePtr<World> world;
};

bool Near(double value, double expected, double tolerance) noexcept {
    return math::Abs(value - expected) <= tolerance;
}

// 컴포넌트 하나에 setter를 부른다(결과는 시험이 본다).
template <class T, class Call>
bool Apply(World& world, ObjectHandle component, Call&& call) noexcept {
    return world
        .With<T>(component,
                 [&](T& value) noexcept {
                     call(value);
                     return Result<void>::Success();
                 })
        .IsOk();
}

// UE 기본 선형 감쇠(BodySettings DefaultLinearDamping 0.01/s)를 넣은 Jolt 적분의 n 스텝 뒤 낙하
// 속도다 (MotionProperties.inl:133-143 — 중력을 더한 뒤 v·(1 − c·dt)). Jolt 기본 감쇠 0.05였다면
// 다르다.
double DampedFallVelocity(int steps) noexcept {
    const double keep = 1.0 - static_cast<double>(DefaultLinearDamping) * StepSeconds;
    double velocity = 0.0;
    for (int step = 0; step < steps; ++step) {
        velocity = (velocity + GravityZ * StepSeconds) * keep;
    }

    return velocity;
}

} // namespace

// 등록이 body를 만들고 Actor 파괴가 지운다. NoCollision·장면 없는 World는 body가 없다.
TEST(PhysicsBody_ComponentRegistrationOwnsBody) {
    BodyWorldFixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());

    const ObjectHandle box = fixture.SpawnShape<BoxComponent>([](BoxComponent&) noexcept {});
    REQUIRE(!box.IsNull());
    CHECK(!fixture.Body(box).IsNull());
    CHECK(fixture.BodyCount() == 1u);

    const ObjectHandle quiet =
        fixture.SpawnShape<SphereComponent>([](SphereComponent& shape) noexcept {
            (void)shape.SetCollisionEnabled(CollisionEnabled::NoCollision);
        });
    REQUIRE(!quiet.IsNull());
    CHECK(fixture.Body(quiet).IsNull());
    CHECK(fixture.BodyCount() == 1u);

    ObjectHandle owner;
    REQUIRE(world.With<BoxComponent>(box, [&](BoxComponent& shape) noexcept {
        owner = shape.GetOwner();
        return Result<void>::Success();
    }));
    const PhysicsBodyHandle oldBody = fixture.Body(box);
    REQUIRE(world.DestroyActor(owner));
    fixture.Step(1);
    CHECK(fixture.BodyCount() == 0u);
    CHECK(world.GetPhysicsScene()->ReadKinematics(oldBody).Error() == ErrorCode::StaleHandle);

    BodyWorldFixture plain(false);
    REQUIRE(plain.registered);
    REQUIRE(plain.world);
    REQUIRE(plain.world->Initialize());
    const ObjectHandle lonely = plain.SpawnShape<BoxComponent>([](BoxComponent&) noexcept {});
    REQUIRE(!lonely.IsNull());
    CHECK(plain.Body(lonely).IsNull());
}

// Movable + simulatePhysics는 Dynamic이라 떨어지고(UE 기본 선형 감쇠를 넣은 속도), Movable만이면
// Kinematic, Static mobility는 simulate여도 Static(경고)으로 움직이지 않는다.
TEST(PhysicsBody_MotionFollowsMobilityAndSimulate) {
    BodyWorldFixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());

    const ObjectHandle falling =
        fixture.SpawnShape<SphereComponent>([](SphereComponent& shape) noexcept {
            shape.SetSimulatePhysics(true);
            (void)shape.SetRelativeLocation(Vec3d(0.0, 0.0, DropHeight));
        });
    const ObjectHandle kinematic =
        fixture.SpawnShape<SphereComponent>([](SphereComponent& shape) noexcept {
            (void)shape.SetRelativeLocation(Vec3d(500.0, 0.0, DropHeight));
        });
    const ObjectHandle pinned =
        fixture.SpawnShape<SphereComponent>([](SphereComponent& shape) noexcept {
            shape.SetMobility(ComponentMobility::Static);
            shape.SetSimulatePhysics(true);
            (void)shape.SetRelativeLocation(Vec3d(-500.0, 0.0, DropHeight));
        });
    REQUIRE(!falling.IsNull() && !kinematic.IsNull() && !pinned.IsNull());
    REQUIRE(world.With<PrimitiveComponent>(falling, [](PrimitiveComponent& shape) noexcept {
        CHECK(shape.PhysicsBodyMotion() == BodyMotion::Dynamic);
        return Result<void>::Success();
    }));
    REQUIRE(world.With<PrimitiveComponent>(kinematic, [](PrimitiveComponent& shape) noexcept {
        CHECK(shape.PhysicsBodyMotion() == BodyMotion::Kinematic);
        return Result<void>::Success();
    }));
    REQUIRE(world.With<PrimitiveComponent>(pinned, [](PrimitiveComponent& shape) noexcept {
        CHECK(shape.PhysicsBodyMotion() == BodyMotion::Static);
        return Result<void>::Success();
    }));

    fixture.Step(FallSteps);
    const BodyKinematics fall = fixture.Kinematics(falling);
    CHECK(fall.awake);
    CHECK(Near(fall.linearVelocity.z, DampedFallVelocity(FallSteps), VelocityTolerance));
    CHECK(fall.location.z < DropHeight);
    const BodyKinematics held = fixture.Kinematics(kinematic);
    CHECK(Near(held.location.z, DropHeight, PositionTolerance));
    CHECK(held.linearVelocity == Vec3d::Zero());
    CHECK(Near(fixture.Kinematics(pinned).location.z, DropHeight, PositionTolerance));
}

// PA10(CapsuleComponent 경로): 반경 30 cm·반높이 90 cm(반구 포함)인 캡슐을 세워 떨어뜨리면 중심이
// 바닥 위 90 cm에 서고 쓰러지지 않는다.
TEST(PhysicsBody_CapsuleComponentStandsOnFloor) {
    constexpr float Radius = 30.0f;
    constexpr float HalfHeight = 90.0f;
    BodyWorldFixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world);
    REQUIRE(fixture.world->Initialize());
    REQUIRE(fixture.world->BeginPlay());
    REQUIRE(!fixture.SpawnFloor().IsNull());
    const ObjectHandle capsule =
        fixture.SpawnShape<CapsuleComponent>([](CapsuleComponent& shape) noexcept {
            (void)shape.SetCapsuleSize(Radius, HalfHeight);
            shape.SetSimulatePhysics(true);
            (void)shape.SetRelativeLocation(Vec3d(0.0, 0.0, HalfHeight + 10.0));
        });
    REQUIRE(!capsule.IsNull());
    fixture.Step(SettleSteps);
    const BodyKinematics rest = fixture.Kinematics(capsule);
    CHECK(Near(rest.location.z, HalfHeight, RestHeightTolerance));
    CHECK(rest.rotation.GetUpVector().z > 0.999);
}

// box scale은 형상 크기에 굽는다: 반폭 z 25 cm·scale z 2인 바닥 윗면은 z = 0이라 구가 반경 높이에
// 선다(굽지 않으면 윗면이 −25 cm라 구가 더 낮게 선다).
TEST(PhysicsBody_BoxScaleBakesIntoShape) {
    constexpr double HalfThickness = 25.0;
    constexpr double ScaleZ = 2.0;
    BodyWorldFixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world);
    REQUIRE(fixture.world->Initialize());
    REQUIRE(fixture.world->BeginPlay());
    const ObjectHandle floor = fixture.SpawnShape<BoxComponent>([](BoxComponent& shape) noexcept {
        shape.SetMobility(ComponentMobility::Static);
        (void)shape.SetBoxExtent(Vec3d(FloorHalfExtent, FloorHalfExtent, HalfThickness));
        (void)shape.SetRelativeScale(Vec3d(1.0, 1.0, ScaleZ));
        (void)shape.SetRelativeLocation(Vec3d(0.0, 0.0, -HalfThickness * ScaleZ));
    });
    const ObjectHandle ball =
        fixture.SpawnShape<SphereComponent>([](SphereComponent& shape) noexcept {
            (void)shape.SetSphereRadius(static_cast<float>(SphereRadius));
            shape.SetSimulatePhysics(true);
            (void)shape.SetRelativeLocation(Vec3d(0.0, 0.0, DropHeight));
        });
    REQUIRE(!floor.IsNull() && !ball.IsNull());
    fixture.Step(SettleSteps);
    CHECK(Near(fixture.Kinematics(ball).location.z, SphereRadius, RestHeightTolerance));
}

// 컴포넌트를 옮기면 Kinematic body가 바로 따라온다(OnTransformUpdated → SetBodyTransform — 3.3절).
TEST(PhysicsBody_MovingComponentMovesBody) {
    BodyWorldFixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    const ObjectHandle box = fixture.SpawnShape<BoxComponent>([](BoxComponent&) noexcept {});
    REQUIRE(!box.IsNull());
    const Vec3d target(120.0, -40.0, 300.0);
    const Quatd turn = Quatd::FromUnitAxisAngle(Vec3d::Up(), math::HalfPi<double>);
    REQUIRE(world.With<BoxComponent>(box, [&](BoxComponent& shape) noexcept {
        return shape.SetWorldLocationAndRotation(target, turn);
    }));
    const BodyKinematics moved = fixture.Kinematics(box);
    CHECK(Near(moved.location.x, target.x, PositionTolerance));
    CHECK(Near(moved.location.y, target.y, PositionTolerance));
    CHECK(Near(moved.location.z, target.z, PositionTolerance));
    CHECK(AngularDistance(moved.rotation, turn) < 1.0e-5);
}

// 바닥에 내려앉아 잠든 Dynamic body를 컴포넌트 이동으로 띄우면 깨어난다(UE SetGlobalPose가 잠든
// 입자를 깨운다 — 7.1.3절).
TEST(PhysicsBody_MovedSleepingBodyWakes) {
    constexpr double LiftHeight = 500.0;
    BodyWorldFixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    REQUIRE(!fixture.SpawnFloor().IsNull());
    const ObjectHandle ball =
        fixture.SpawnShape<SphereComponent>([](SphereComponent& shape) noexcept {
            (void)shape.SetSphereRadius(static_cast<float>(SphereRadius));
            shape.SetSimulatePhysics(true);
            (void)shape.SetRelativeLocation(Vec3d(0.0, 0.0, SphereRadius + 1.0));
        });
    REQUIRE(!ball.IsNull());
    fixture.Step(SettleSteps);
    REQUIRE(!fixture.Kinematics(ball).awake);
    REQUIRE(world.With<SphereComponent>(ball, [](SphereComponent& shape) noexcept {
        return shape.SetWorldLocationAndRotation(Vec3d(0.0, 0.0, LiftHeight), Quatd());
    }));
    const BodyKinematics lifted = fixture.Kinematics(ball);
    CHECK(lifted.awake);
    CHECK(Near(lifted.location.z, LiftHeight, PositionTolerance));
}

// 크기 0 형상·scale 0 축·비균일 scale 부모 아래 회전한 자식(행렬로 분해할 수 없다)도 UE처럼 body가
// 생긴다(크기는 clamp, transform은 위치·회전·scale을 따로 쓴다 — 7.1.3절).
TEST(PhysicsBody_DegenerateSizesAndShearStillCreateBodies) {
    BodyWorldFixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    const ObjectHandle point = fixture.SpawnShape<SphereComponent>(
        [](SphereComponent& shape) noexcept { (void)shape.SetSphereRadius(0.0f); });
    const ObjectHandle flat = fixture.SpawnShape<BoxComponent>(
        [](BoxComponent& shape) noexcept { (void)shape.SetRelativeScale(Vec3d(1.0, 1.0, 0.0)); });
    const ObjectHandle thin = fixture.SpawnShape<CapsuleComponent>(
        [](CapsuleComponent& shape) noexcept { (void)shape.SetCapsuleSize(0.0f, 0.0f); });
    REQUIRE(!point.IsNull() && !flat.IsNull() && !thin.IsNull());
    CHECK(!fixture.Body(point).IsNull());
    CHECK(!fixture.Body(flat).IsNull());
    CHECK(!fixture.Body(thin).IsNull());

    // 비균일 scale(2, 1, 1) 부모 아래 45° 돌린 자식은 월드 행렬에 shear가 생긴다.
    auto actor = world.SpawnActorDeferred<PhysicsTestActor>();
    REQUIRE(actor);
    auto parent = world.AddComponent<BoxComponent>(actor.Value());
    auto child = world.AddComponent<SphereComponent>(actor.Value());
    REQUIRE(parent && child);
    REQUIRE(world.With<BoxComponent>(parent.Value(), [](BoxComponent& shape) noexcept {
        return shape.SetRelativeScale(Vec3d(2.0, 1.0, 1.0));
    }));
    const ObjectHandle parentHandle = parent.Value();
    REQUIRE(world.With<SphereComponent>(child.Value(), [&](SphereComponent& shape) noexcept {
        if (auto attached = shape.AttachTo(parentHandle); !attached) {
            return attached;
        }

        return shape.SetRelativeRotation(
            Quatd::FromUnitAxisAngle(Vec3d::Up(), math::Pi<double> / 4.0));
    }));
    REQUIRE(world.FinishSpawning(actor.Value()));
    CHECK(!fixture.Body(child.Value()).IsNull());
}

// 결과 반영(3.4절): 떨어지는 구 컴포넌트의 월드 위치·회전은 매 Tick 끝에 body와 같다(한 Tick에
// step 여러 번이어도, 바닥에서 잠든 뒤에도).
TEST(PhysicsBody_ResultsWriteBackToComponent) {
    constexpr double MultiStepTick = StepSeconds * 3.5;
    constexpr int Ticks = 120;
    constexpr double RotationTolerance = 1.0e-6; // rad
    BodyWorldFixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    REQUIRE(!fixture.SpawnFloor().IsNull());
    const ObjectHandle ball =
        fixture.SpawnShape<SphereComponent>([](SphereComponent& shape) noexcept {
            (void)shape.SetSphereRadius(static_cast<float>(SphereRadius));
            shape.SetSimulatePhysics(true);
            (void)shape.SetRelativeLocation(Vec3d(0.0, 0.0, DropHeight));
        });
    REQUIRE(!ball.IsNull());
    int mismatches = 0;
    for (int tick = 0; tick < Ticks; ++tick) {
        CHECK(world.Tick(tick % 2 == 0 ? StepSeconds : MultiStepTick));
        const BodyKinematics body = fixture.Kinematics(ball);
        const Transform pose = fixture.ComponentPose(ball);
        if (!(pose.translation == body.location) ||
            AngularDistance(pose.rotation, body.rotation) > RotationTolerance) {
            ++mismatches;
        }
    }

    CHECK(mismatches == 0);
    const BodyKinematics rest = fixture.Kinematics(ball);
    CHECK(!rest.awake);
    CHECK(Near(fixture.ComponentPose(ball).translation.z, SphereRadius, RestHeightTolerance));
}

// Kinematic(3.3절): 옮기면 body가 바로 목표에 있고, 다음 step 속도는 이동량/dt, 그다음 step은
// 0이다. 한 Tick에 step이 셋이면 목표는 첫 step이 쓰고 끝 속도는 0이다.
TEST(PhysicsBody_KinematicMoveGivesStepVelocity) {
    constexpr double Offset = 100.0;
    constexpr double SecondOffset = 60.0;
    constexpr double ThreeStepTick = StepSeconds * 3.5;
    BodyWorldFixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    const ObjectHandle box = fixture.SpawnShape<BoxComponent>([](BoxComponent&) noexcept {});
    REQUIRE(!box.IsNull());
    auto move = [&](double x) noexcept {
        REQUIRE(world.With<BoxComponent>(box, [&](BoxComponent& shape) noexcept {
            return shape.SetWorldLocationAndRotation(Vec3d(x, 0.0, 0.0), Quatd());
        }));
    };

    move(Offset);
    CHECK(Near(fixture.Kinematics(box).location.x, Offset, PositionTolerance));
    fixture.Step(1);
    const BodyKinematics moving = fixture.Kinematics(box);
    CHECK(Near(moving.linearVelocity.x, Offset / StepSeconds, VelocityTolerance));
    CHECK(Near(moving.location.x, Offset, PositionTolerance));
    fixture.Step(1);
    CHECK(fixture.Kinematics(box).linearVelocity == Vec3d::Zero());

    move(Offset + SecondOffset);
    CHECK(world.Tick(ThreeStepTick));
    const BodyKinematics stopped = fixture.Kinematics(box);
    CHECK(stopped.linearVelocity == Vec3d::Zero());
    CHECK(Near(stopped.location.x, Offset + SecondOffset, PositionTolerance));
}

// teleport(TeleportPhysics)로 옮긴 Kinematic은 속도가 0이고, step하지 않는 Editor World의 Kinematic
// 이동은 언제나 teleport다(PMD16).
TEST(PhysicsBody_TeleportAndEditorKinematicHaveNoVelocity) {
    constexpr double Target = 250.0;
    BodyWorldFixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    const ObjectHandle box = fixture.SpawnShape<BoxComponent>([](BoxComponent&) noexcept {});
    REQUIRE(!box.IsNull());
    PhysicsScene& scene = *world.GetPhysicsScene();
    const PhysicsBodyHandle body = fixture.Body(box);
    auto at = [](double x) noexcept {
        return Transform(Quatd(), Vec3d(x, 0.0, 0.0), Vec3d::One());
    };
    // 움직이는 중(속도 ≠ 0)인 Kinematic을 teleport하면 바로 속도 0이고 다음 step에도 0이다.
    scene.SetBodyTransform(body, at(Target * 0.5), TeleportMode::None);
    fixture.Step(1);
    REQUIRE(!(fixture.Kinematics(box).linearVelocity == Vec3d::Zero()));
    scene.SetBodyTransform(body, at(Target), TeleportMode::TeleportPhysics);
    CHECK(fixture.Kinematics(box).linearVelocity == Vec3d::Zero());
    fixture.Step(1);
    const BodyKinematics teleported = fixture.Kinematics(box);
    CHECK(teleported.linearVelocity == Vec3d::Zero());
    CHECK(Near(teleported.location.x, Target, PositionTolerance));
    // 같은 프레임에 물리적 이동 뒤 teleport하면 대기 목표가 지워진다(다음 step 속도 0).
    scene.SetBodyTransform(body, at(Target * 2.0), TeleportMode::None);
    scene.SetBodyTransform(body, at(Target * 3.0), TeleportMode::TeleportPhysics);
    fixture.Step(1);
    const BodyKinematics cancelled = fixture.Kinematics(box);
    CHECK(cancelled.linearVelocity == Vec3d::Zero());
    CHECK(Near(cancelled.location.x, Target * 3.0, PositionTolerance));

    BodyWorldFixture editor(true, WorldKind::Editor);
    REQUIRE(editor.registered);
    REQUIRE(editor.world);
    REQUIRE(editor.world->Initialize());
    const ObjectHandle edited = editor.SpawnShape<BoxComponent>([](BoxComponent&) noexcept {});
    REQUIRE(!edited.IsNull());
    REQUIRE(editor.world->With<BoxComponent>(edited, [](BoxComponent& shape) noexcept {
        return shape.SetWorldLocationAndRotation(Vec3d(Target, 0.0, 0.0), Quatd());
    }));
    const BodyKinematics placed = editor.Kinematics(edited);
    CHECK(Near(placed.location.x, Target, PositionTolerance));
    CHECK(placed.linearVelocity == Vec3d::Zero());
}

// 결정성(PC03): 같은 설정·같은 호출 순서의 World 둘에서 떨어지는 구들의 결과가 byte까지 같다.
TEST(PhysicsBody_SameWorldsGiveSameResults) {
    constexpr int Balls = 6;
    constexpr double Spacing = 45.0;
    struct Run {
        Vec3d location[Balls];
        Vec3d velocity[Balls];
        Transform pose[Balls];
        Vec3d pusher;
    };

    auto simulate = [](Run& run) noexcept {
        BodyWorldFixture fixture;
        REQUIRE(fixture.registered);
        REQUIRE(fixture.world);
        REQUIRE(fixture.world->Initialize());
        REQUIRE(fixture.world->BeginPlay());
        REQUIRE(!fixture.SpawnFloor().IsNull());
        ObjectHandle balls[Balls];
        for (int index = 0; index < Balls; ++index) {
            const double offset = static_cast<double>(index) * Spacing;
            balls[index] =
                fixture.SpawnShape<SphereComponent>([offset](SphereComponent& shape) noexcept {
                    (void)shape.SetSphereRadius(static_cast<float>(SphereRadius));
                    shape.SetSimulatePhysics(true);
                    (void)shape.SetRelativeLocation(Vec3d(offset * 0.5, 0.0, DropHeight + offset));
                });
            REQUIRE(!balls[index].IsNull());
        }

        // Kinematic 하나가 구들 사이를 지나간다(반영·Kinematic 경로도 같은 결과여야 한다).
        const ObjectHandle pusher =
            fixture.SpawnShape<BoxComponent>([](BoxComponent& shape) noexcept {
                (void)shape.SetBoxExtent(Vec3d(20.0, 20.0, 20.0));
                (void)shape.SetRelativeLocation(Vec3d(-200.0, 0.0, 20.0));
            });
        REQUIRE(!pusher.IsNull());
        for (int step = 0; step < SettleSteps; ++step) {
            const double x = -200.0 + static_cast<double>(step) * 2.0;
            REQUIRE(fixture.world->With<BoxComponent>(pusher, [x](BoxComponent& shape) noexcept {
                return shape.SetWorldLocationAndRotation(Vec3d(x, 0.0, 20.0), Quatd());
            }));
            fixture.Step(1);
        }

        for (int index = 0; index < Balls; ++index) {
            const BodyKinematics result = fixture.Kinematics(balls[index]);
            run.location[index] = result.location;
            run.velocity[index] = result.linearVelocity;
            run.pose[index] = fixture.ComponentPose(balls[index]);
        }

        run.pusher = fixture.Kinematics(pusher).location;
    };

    Run first;
    Run second;
    simulate(first);
    simulate(second);
    for (int index = 0; index < Balls; ++index) {
        CHECK(first.location[index] == second.location[index]);
        CHECK(first.velocity[index] == second.velocity[index]);
        CHECK(first.pose[index].translation == second.pose[index].translation);
        CHECK(first.pose[index].rotation == second.pose[index].rotation);
    }

    CHECK(first.pusher == second.pusher);
}

// Kinematic이 MoveKinematic 속도로 움직여 바닥에 놓인 Dynamic 구를 민다(teleport였다면 속도가 없어
// 위치 보정으로만 밀린다 — 3.3절의 MoveKinematic 이유).
TEST(PhysicsBody_KinematicPushesDynamicBody) {
    constexpr double PushSpeed = 300.0; // cm/s
    constexpr int PushSteps = 40;
    BodyWorldFixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    REQUIRE(!fixture.SpawnFloor().IsNull());
    const ObjectHandle ball =
        fixture.SpawnShape<SphereComponent>([](SphereComponent& shape) noexcept {
            (void)shape.SetSphereRadius(static_cast<float>(SphereRadius));
            shape.SetSimulatePhysics(true);
            (void)shape.SetRelativeLocation(Vec3d(0.0, 0.0, SphereRadius));
        });
    const ObjectHandle pusher = fixture.SpawnShape<BoxComponent>([](BoxComponent& shape) noexcept {
        (void)shape.SetBoxExtent(Vec3d(10.0, 50.0, 30.0));
        (void)shape.SetRelativeLocation(Vec3d(-60.0, 0.0, 30.0));
    });
    REQUIRE(!ball.IsNull() && !pusher.IsNull());
    for (int step = 0; step < PushSteps; ++step) {
        const double x = -60.0 + PushSpeed * StepSeconds * static_cast<double>(step + 1);
        REQUIRE(world.With<BoxComponent>(pusher, [x](BoxComponent& shape) noexcept {
            return shape.SetWorldLocationAndRotation(Vec3d(x, 0.0, 30.0), Quatd());
        }));
        fixture.Step(1);
    }

    CHECK(fixture.Kinematics(ball).linearVelocity.x > PushSpeed * 0.5);
    CHECK(fixture.ComponentPose(ball).translation.x > 0.0);
}

// 재생성 표(2.3.1절 simulatePhysics): body가 있는 컴포넌트의 simulate 전환은 다시 만들지 않고
// Kinematic ↔ Dynamic을 바꾼다. 켜면 떨어지고, 끄면 그 자리에서 속도 0으로 멈춘다(UE
// SetIsKinematic).
TEST(PhysicsBody_SimulateToggleSwitchesMotionInPlace) {
    BodyWorldFixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    const ObjectHandle box = fixture.SpawnShape<BoxComponent>([](BoxComponent& shape) noexcept {
        (void)shape.SetRelativeLocation(Vec3d(0.0, 0.0, DropHeight));
    });
    REQUIRE(!box.IsNull());
    const PhysicsBodyHandle body = fixture.Body(box);
    fixture.Step(1);
    CHECK(Near(fixture.Kinematics(box).location.z, DropHeight, PositionTolerance));

    REQUIRE(Apply<BoxComponent>(
        world, box, [](BoxComponent& shape) noexcept { shape.SetSimulatePhysics(true); }));
    CHECK(fixture.Body(box) == body);
    fixture.Step(FallSteps);
    const BodyKinematics falling = fixture.Kinematics(box);
    CHECK(falling.linearVelocity.z < 0.0);
    CHECK(falling.location.z < DropHeight);
    CHECK(fixture.ComponentPose(box).translation == falling.location);

    REQUIRE(Apply<BoxComponent>(
        world, box, [](BoxComponent& shape) noexcept { shape.SetSimulatePhysics(false); }));
    CHECK(fixture.Body(box) == body);
    CHECK(fixture.Kinematics(box).linearVelocity == Vec3d::Zero());
    fixture.Step(FallSteps);
    const BodyKinematics stopped = fixture.Kinematics(box);
    CHECK(stopped.linearVelocity == Vec3d::Zero());
    CHECK(Near(stopped.location.z, falling.location.z, PositionTolerance));
    CHECK(Near(fixture.ComponentPose(box).translation.z, falling.location.z, PositionTolerance));
}

// simulate를 켜면 부모에서 떨어진다(월드 transform 유지 — UE SetInstanceSimulatePhysics). 시작 전에
// 켜진 부착 자식은 시작(OnStart — UE BeginPlay)에서 떨어진다. Kinematic 부모는 그대로다.
TEST(PhysicsBody_SimulatingComponentDetachesFromParent) {
    constexpr double ChildOffset = 100.0;
    BodyWorldFixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto configureParent = [](BoxComponent& shape) noexcept {
        (void)shape.SetRelativeLocation(Vec3d(0.0, 0.0, DropHeight));
    };
    ObjectHandle parent;
    ObjectHandle child;
    REQUIRE((fixture.SpawnAttached<BoxComponent, SphereComponent>(
        parent, child, configureParent, [](SphereComponent& shape) noexcept {
            (void)shape.SetRelativeLocation(Vec3d(ChildOffset, 0.0, 0.0));
        })));
    fixture.Step(1);
    REQUIRE(fixture.Parent(child) == parent);
    REQUIRE(Apply<SphereComponent>(
        world, child, [](SphereComponent& shape) noexcept { shape.SetSimulatePhysics(true); }));
    CHECK(fixture.Parent(child).IsNull());
    CHECK(fixture.ComponentPose(child).translation == Vec3d(ChildOffset, 0.0, DropHeight));
    fixture.Step(FallSteps);
    CHECK(fixture.ComponentPose(child).translation.z < DropHeight);
    CHECK(Near(fixture.Kinematics(parent).location.z, DropHeight, PositionTolerance));

    ObjectHandle startParent;
    ObjectHandle startChild;
    REQUIRE((fixture.SpawnAttached<BoxComponent, SphereComponent>(
        startParent, startChild, configureParent, [](SphereComponent& shape) noexcept {
            shape.SetSimulatePhysics(true);
            (void)shape.SetRelativeLocation(Vec3d(-ChildOffset, 0.0, 0.0));
        })));
    fixture.Step(1);
    CHECK(fixture.Parent(startChild).IsNull());
    const Vec3d started = fixture.ComponentPose(startChild).translation;
    CHECK(started.x == -ChildOffset);
    CHECK(started.y == 0.0);
    CHECK(Near(started.z, DropHeight, OneStepFallTolerance));
    fixture.Step(FallSteps);
    CHECK(fixture.ComponentPose(startChild).translation.z < DropHeight);
    CHECK(Near(fixture.Kinematics(startParent).location.z, DropHeight, PositionTolerance));
}

// 충돌 켬(2.3.1절): NoCollision 경계를 넘으면 body를 그 자리에서 지우고 만든다. 그 밖의 변경은 같은
// handle에 필터만 바꾼다 — QueryOnly 바닥은 구를 받치지 않고, 다시 켜면 받친다.
TEST(PhysicsBody_CollisionEnabledChangesBody) {
    BodyWorldFixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    const ObjectHandle floor = fixture.SpawnFloor();
    const ObjectHandle box = fixture.SpawnShape<BoxComponent>([](BoxComponent& shape) noexcept {
        (void)shape.SetRelativeLocation(Vec3d(0.0, 0.0, DropHeight));
    });
    REQUIRE(!floor.IsNull() && !box.IsNull());
    const uint32 bodies = fixture.BodyCount();
    auto setCollision = [&](ObjectHandle component, CollisionEnabled enabled) noexcept {
        return world.With<PrimitiveComponent>(component,
                                              [enabled](PrimitiveComponent& shape) noexcept {
                                                  return shape.SetCollisionEnabled(enabled);
                                              });
    };

    REQUIRE(setCollision(box, CollisionEnabled::NoCollision));
    CHECK(fixture.Body(box).IsNull());
    CHECK(fixture.BodyCount() == bodies - 1);
    REQUIRE(setCollision(box, CollisionEnabled::QueryAndPhysics));
    const PhysicsBodyHandle recreated = fixture.Body(box);
    CHECK(!recreated.IsNull());
    CHECK(fixture.BodyCount() == bodies);
    REQUIRE(setCollision(box, CollisionEnabled::QueryOnly));
    CHECK(fixture.Body(box) == recreated);

    const PhysicsBodyHandle floorBody = fixture.Body(floor);
    REQUIRE(setCollision(floor, CollisionEnabled::QueryOnly));
    CHECK(fixture.Body(floor) == floorBody);
    auto dropBall = [&](double x) noexcept {
        return fixture.SpawnShape<SphereComponent>([x](SphereComponent& shape) noexcept {
            (void)shape.SetSphereRadius(static_cast<float>(SphereRadius));
            shape.SetSimulatePhysics(true);
            (void)shape.SetRelativeLocation(Vec3d(x, 0.0, DropHeight));
        });
    };

    const ObjectHandle through = dropBall(SideOffset);
    REQUIRE(!through.IsNull());
    fixture.Step(SettleSteps);
    CHECK(fixture.Kinematics(through).location.z < -FloorHalfThickness * 2.0);

    REQUIRE(setCollision(floor, CollisionEnabled::QueryAndPhysics));
    CHECK(fixture.Body(floor) == floorBody);
    const ObjectHandle held = dropBall(-SideOffset);
    REQUIRE(!held.IsNull());
    fixture.Step(SettleSteps);
    CHECK(Near(fixture.Kinematics(held).location.z, SphereRadius, RestHeightTolerance));
}

// 형상 값·scale(2.3.1절 — UE UpdateBodyScale): 바닥에 놓인 구의 반경이나 scale을 바꾸면 같은
// handle로 형상이 바뀌어 정지 높이가 새 크기를 따른다(잠든 구도 깨운다).
TEST(PhysicsBody_ShapeAndScaleChangesKeepBody) {
    BodyWorldFixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    REQUIRE(!fixture.SpawnFloor().IsNull());
    const ObjectHandle ball =
        fixture.SpawnShape<SphereComponent>([](SphereComponent& shape) noexcept {
            (void)shape.SetSphereRadius(static_cast<float>(SphereRadius));
            shape.SetSimulatePhysics(true);
            (void)shape.SetRelativeLocation(Vec3d(0.0, 0.0, SphereRadius));
        });
    REQUIRE(!ball.IsNull());
    const PhysicsBodyHandle body = fixture.Body(ball);
    fixture.Step(SettleSteps);
    REQUIRE(!fixture.Kinematics(ball).awake);

    REQUIRE(Apply<SphereComponent>(world, ball, [](SphereComponent& shape) noexcept {
        CHECK(shape.SetSphereRadius(static_cast<float>(SphereRadius * 2.0)));
    }));
    CHECK(fixture.Body(ball) == body);
    CHECK(fixture.Kinematics(ball).awake);
    fixture.Step(SettleSteps);
    CHECK(Near(fixture.Kinematics(ball).location.z, SphereRadius * 2.0, RestHeightTolerance));

    REQUIRE(Apply<SphereComponent>(world, ball, [](SphereComponent& shape) noexcept {
        CHECK(shape.SetRelativeScale(Vec3d(0.5, 0.5, 0.5)));
    }));
    CHECK(fixture.Body(ball) == body);
    fixture.Step(SettleSteps);
    CHECK(Near(fixture.Kinematics(ball).location.z, SphereRadius, RestHeightTolerance));
    CHECK(Near(fixture.ComponentPose(ball).translation.z, SphereRadius, RestHeightTolerance));
}

// Mobility(2.3.1절): Static → Movable은 body를 다시 만든다(새 handle — simulate가 켜져 있어 이제
// Dynamic으로 떨어진다). Movable이 된 부모의 자식도 Movable이다(UE SetMobility).
TEST(PhysicsBody_MobilityChangeRecreatesBody) {
    BodyWorldFixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    ObjectHandle parent;
    ObjectHandle child;
    REQUIRE((fixture.SpawnAttached<SphereComponent, BoxComponent>(
        parent, child,
        [](SphereComponent& shape) noexcept {
            shape.SetMobility(ComponentMobility::Static);
            shape.SetSimulatePhysics(true);
            (void)shape.SetRelativeLocation(Vec3d(0.0, 0.0, DropHeight));
        },
        [](BoxComponent& shape) noexcept {
            shape.SetMobility(ComponentMobility::Static);
            (void)shape.SetRelativeLocation(Vec3d(SideOffset, 0.0, 0.0));
        })));
    const PhysicsBodyHandle staticBody = fixture.Body(parent);
    const PhysicsBodyHandle staticChild = fixture.Body(child);
    const uint32 bodies = fixture.BodyCount();
    fixture.Step(FallSteps);
    CHECK(Near(fixture.Kinematics(parent).location.z, DropHeight, PositionTolerance));

    REQUIRE(Apply<SphereComponent>(world, parent, [](SphereComponent& shape) noexcept {
        shape.SetMobility(ComponentMobility::Movable);
        CHECK(shape.PhysicsBodyMotion() == BodyMotion::Dynamic);
    }));
    REQUIRE(Apply<BoxComponent>(world, child, [](BoxComponent& shape) noexcept {
        CHECK(shape.Mobility() == ComponentMobility::Movable);
        CHECK(shape.PhysicsBodyMotion() == BodyMotion::Kinematic);
    }));
    CHECK(!(fixture.Body(parent) == staticBody));
    CHECK(!fixture.Body(parent).IsNull());
    CHECK(!(fixture.Body(child) == staticChild));
    CHECK(fixture.BodyCount() == bodies);
    // 부모가 Dynamic으로 떨어지고 Kinematic 자식은 부착 전파로 따라간다(다시 만든 body도 Kinematic
    // 목표로 옮겨진다).
    fixture.Step(FallSteps);
    const double parentZ = fixture.Kinematics(parent).location.z;
    CHECK(parentZ < DropHeight);
    CHECK(Near(fixture.ComponentPose(child).translation.z, parentZ, PositionTolerance));
    CHECK(Near(fixture.Kinematics(child).location.z, parentZ, PositionTolerance));
}

// Kinematic → Dynamic 전환은 Kinematic 표지를 지운다: 직전 step에 움직인 body는 그 속도를 이어
// 가고(다음 step의 정지가 Dynamic 속도를 지우지 않는다), 대기 목표가 있던 body는 MoveKinematic 없이
// 그 자리에서 Dynamic이 된다.
TEST(PhysicsBody_KinematicToDynamicClearsKinematicState) {
    constexpr double Offset = 100.0;
    BodyWorldFixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto spawnAt = [&](double y) noexcept {
        return fixture.SpawnShape<BoxComponent>([y](BoxComponent& shape) noexcept {
            (void)shape.SetRelativeLocation(Vec3d(0.0, y, DropHeight));
        });
    };
    auto moveTo = [&](ObjectHandle box, double x, double y) noexcept {
        REQUIRE(world.With<BoxComponent>(box, [x, y](BoxComponent& shape) noexcept {
            return shape.SetWorldLocationAndRotation(Vec3d(x, y, DropHeight), Quatd());
        }));
    };
    auto simulate = [&](ObjectHandle box) noexcept {
        REQUIRE(Apply<BoxComponent>(
            world, box, [](BoxComponent& shape) noexcept { shape.SetSimulatePhysics(true); }));
    };

    // 직전 step에 움직인 Kinematic: 이동량/dt 속도를 Dynamic이 이어 간다.
    const ObjectHandle moved = spawnAt(0.0);
    // 대기 목표가 있는 Kinematic: 전환 뒤 step은 MoveKinematic을 하지 않는다.
    const ObjectHandle pending = spawnAt(SideOffset);
    REQUIRE(!moved.IsNull() && !pending.IsNull());
    moveTo(moved, Offset, 0.0);
    fixture.Step(1);
    REQUIRE(
        Near(fixture.Kinematics(moved).linearVelocity.x, Offset / StepSeconds, VelocityTolerance));
    simulate(moved);
    moveTo(pending, Offset, SideOffset);
    simulate(pending);
    fixture.Step(1);

    // Dynamic이 된 step에 선형 감쇠(DefaultLinearDamping)가 한 번 걸린다(Jolt 적분 — 위 낙하
    // 기대값과 같은 식).
    const double keep = 1.0 - static_cast<double>(DefaultLinearDamping) * StepSeconds;
    const BodyKinematics carried = fixture.Kinematics(moved);
    CHECK(Near(carried.linearVelocity.x, Offset / StepSeconds * keep, VelocityTolerance));
    CHECK(carried.linearVelocity.z < 0.0);
    const BodyKinematics placed = fixture.Kinematics(pending);
    CHECK(Near(placed.linearVelocity.x, 0.0, VelocityTolerance));
    CHECK(Near(placed.location.x, Offset, PositionTolerance));
}

// Dynamic이던 step의 자세가 반영되기 전에 Kinematic이 되어도 그 자세를 컴포넌트에 쓴다(결과 목록은
// 운동 종류를 다시 거르지 않는다 — 7.1.5절).
TEST(PhysicsBody_ResultBeforeKinematicSwitchStillWritesBack) {
    BodyWorldFixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    const ObjectHandle ball =
        fixture.SpawnShape<SphereComponent>([](SphereComponent& shape) noexcept {
            shape.SetSimulatePhysics(true);
            (void)shape.SetRelativeLocation(Vec3d(0.0, 0.0, DropHeight));
        });
    REQUIRE(!ball.IsNull());
    auto* physics = world.GetSubsystem<PhysicsWorldSubsystem>();
    REQUIRE(physics != nullptr);
    // 엔진 자리를 거치지 않고 step과 반영 사이에 운동 종류를 바꾼다(고정 스텝 hook이 하는 일).
    for (int step = 0; step < FallSteps; ++step) {
        REQUIRE(physics->SimulateStep(StepSeconds));
    }

    REQUIRE(Apply<SphereComponent>(
        world, ball, [](SphereComponent& shape) noexcept { shape.SetSimulatePhysics(false); }));
    REQUIRE(physics->ApplyResults(world));
    const BodyKinematics body = fixture.Kinematics(ball);
    CHECK(body.location.z < DropHeight);
    CHECK(fixture.ComponentPose(ball).translation == body.location);
}

// 초기화 되돌림(World.cpp — Subsystem 정리가 Actor 정리보다 먼저): 물리 뒤 Subsystem이 body 있는
// Actor를 Spawn하고 실패해도 장면은 떼어진 뒤 파괴되고, 컴포넌트는 없는 장면의 body를 건드리지
// 않는다.
class SpawningFailingSubsystem final : public WorldSubsystem {
public:
    Result<void> Initialize(World& world) noexcept override {
        auto actor = world.SpawnActorDeferred<PhysicsTestActor>();
        if (!actor) {
            return Result<void>::Failure(actor.Error());
        }

        if (auto component = world.AddComponent<BoxComponent>(actor.Value()); !component) {
            return Result<void>::Failure(component.Error());
        }

        if (auto finished = world.FinishSpawning(actor.Value()); !finished) {
            return finished;
        }

        bodiesAtFailure = world.GetPhysicsScene()->Stats().bodies;
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    void Deinitialize(World&) noexcept override {
    }

    static inline uint32 bodiesAtFailure = 0;
};

TEST(PhysicsBody_InitializeRollbackWithBodiesDetachesScene) {
    static const SubsystemKey afterPhysics[] = {SubsystemKeyOf<PhysicsWorldSubsystem>()};
    const WorldSubsystemType failing = MakeSubsystemType<WorldSubsystem, SpawningFailingSubsystem>(
        "SpawningFailing", afterPhysics);
    SpawningFailingSubsystem::bodiesAtFailure = 0;
    BodyWorldFixture fixture(true, WorldKind::Game, &failing);
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world);
    World& world = *fixture.world;
    auto initialized = world.Initialize();
    REQUIRE(!initialized);
    CHECK(initialized.Error() == ErrorCode::Unsupported);
    CHECK(SpawningFailingSubsystem::bodiesAtFailure == 1u);
    CHECK(world.GetPhysicsScene() == nullptr);
}

} // namespace physics_body_tests_file
