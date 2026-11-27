// Physics 모듈 PH5c1 컴포넌트 시험이다(Docs/Design/PhysicsModule 2.6절, 7.1.17절): PA04(충격량·
// 중력 상쇄 힘)·PA05(토크·관성 — 반폭이 다른 상자, …InDegrees)를 PrimitiveComponent 경로로, 속도
// setter·점 속도·도 변환, body 없음 0, 방사 무시 표지, 잘못된 호출 경고. 질량은 UE 규칙(균일 밀도
// 질량^0.75 — 7.1.15절)으로 계산한다(질량 덮어쓰기 API는 PH5c2). World·컴포넌트 공개 API만 쓰므로
// Modular 시험에서도 돈다.
#include "PhysicsBodyFixture.h"

#include <Base/Compiler.h>
#include <Base/InitializerList.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <DeterministicMath/DeterministicMath.h>

using namespace rocks;
namespace physics_force_tests_file {

using namespace physics_test_support;

namespace {

// UE 기본 물리 재질 밀도(kg/cm³ — 1 g/cm³)와 질량 지수다(7.1.15절).
constexpr double DefaultDensity = 0.001;
constexpr double UnrealRaiseMassToPower = 0.75;
constexpr double KickVelocity = 100.0;    // cm/s(PA04 — 충격량 m·100)
constexpr double TorqueMagnitude = 1.0e4; // kg·cm²/s²(= 1 N·m — PA05 크기 판정)
// 상대 허용 오차다(Jolt float 질량·관성·속도 — 1e-6 수준).
constexpr double RelativeTolerance = 1.0e-4;
// 중력을 힘으로 상쇄한 body의 높이 허용 오차(cm — float 적분 잔차)다.
constexpr double BalanceTolerance = 0.05;
constexpr double RadialRadius = 500.0;   // cm
constexpr double RadialStrength = 300.0; // cm/s(속도 변화)
constexpr double PointArm = 10.0;        // cm
constexpr double QuarterTurnDegrees = 90.0;
// PA05 상자 반폭이다(cm — 축마다 다른 관성).
constexpr double BoxHalfX = 10.0;
constexpr double BoxHalfY = 20.0;
constexpr double BoxHalfZ = 30.0;

double SphereMass(double radius) noexcept {
    const double volume = 4.0 / 3.0 * math::Pi<double> * radius * radius * radius;
    return deterministic::Pow(volume * DefaultDensity, UnrealRaiseMassToPower);
}

double BoxMass() noexcept {
    const double volume = 8.0 * BoxHalfX * BoxHalfY * BoxHalfZ;
    return deterministic::Pow(volume * DefaultDensity, UnrealRaiseMassToPower);
}

// 한 스텝 감쇠 배율이다(Jolt — 중력·힘을 더한 뒤 v·(1 − c·dt), BodySettings 기본 선형 감쇠).
double LinearKeep() noexcept {
    return 1.0 - static_cast<double>(DefaultLinearDamping) * StepSeconds;
}

bool NearRelative(double value, double expected) noexcept {
    return Near(value, expected, math::Abs(expected) * RelativeTolerance);
}

bool StartWorld(BodyWorldFixture& fixture) noexcept {
    return fixture.registered && fixture.world && fixture.world->Initialize() &&
           fixture.world->BeginPlay();
}

// x에 떠 있는 시뮬레이션 구(반경 SphereRadius)를 둔다. configure는 등록 전에 부른다.
template <class Configure>
ObjectHandle SpawnBall(BodyWorldFixture& fixture, double x, Configure&& configure) noexcept {
    return fixture.SpawnShape<SphereComponent>([&](SphereComponent& sphere) noexcept {
        (void)sphere.SetSphereRadius(static_cast<float>(SphereRadius));
        (void)sphere.SetRelativeLocation(Vec3d(x, 0.0, DropHeight));
        sphere.SetSimulatePhysics(true);
        configure(sphere);
    });
}

ObjectHandle SpawnBall(BodyWorldFixture& fixture, double x) noexcept {
    return SpawnBall(fixture, x, [](SphereComponent&) noexcept {});
}

ObjectHandle SpawnBox(BodyWorldFixture& fixture, double x) noexcept {
    return fixture.SpawnShape<BoxComponent>([&](BoxComponent& box) noexcept {
        (void)box.SetBoxExtent(Vec3d(BoxHalfX, BoxHalfY, BoxHalfZ));
        (void)box.SetRelativeLocation(Vec3d(x, 0.0, DropHeight));
        box.SetSimulatePhysics(true);
    });
}

// 진단 코드 셋의 Warning 수를 센다.
class PhysicsOperationWarnings final : public RuntimeLogSink {
public:
    Result<void> Write(const LogEvent& event) noexcept override {
        const StringView message(event.message);
        if (message.Contains("(PhysicsOperationWithoutSimulation)",
                             StringComparison::CaseSensitive)) {
            ++withoutSimulation;
        } else if (message.Contains("(PhysicsOperationWithoutPhysicsCollision)",
                                    StringComparison::CaseSensitive)) {
            ++withoutCollision;
        } else if (message.Contains("(StaticComponentMoved)", StringComparison::CaseSensitive)) {
            ++notMovable;
        }

        return Result<void>::Success();
    }

    uint32 withoutSimulation = 0;
    uint32 withoutCollision = 0;
    uint32 notMovable = 0;
};

} // namespace

// PA04 컴포넌트 경로: 충격량 (0, 0, m·100)은 바로 읽으면 옛 속도이고 한 스텝 뒤 기준 구보다
// 100 cm/s(감쇠 배율) 빠르다. velocityChange는 질량 없이 같은 결과다.
TEST(PhysicsForce_ImpulseThroughComponent) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle reference = SpawnBall(fixture, 0.0);
    const ObjectHandle pushed = SpawnBall(fixture, SideOffset);
    const ObjectHandle changed = SpawnBall(fixture, 2.0 * SideOffset);
    REQUIRE(!reference.IsNull() && !pushed.IsNull() && !changed.IsNull());
    const double mass = SphereMass(SphereRadius);
    Vec3d before;
    REQUIRE(Apply<PrimitiveComponent>(world, pushed, [&](PrimitiveComponent& ball) noexcept {
        ball.AddImpulse(Vec3d(0.0, 0.0, mass * KickVelocity));
        before = ball.GetPhysicsLinearVelocity();
    }));
    CHECK(Near(before.z, 0.0, VelocityTolerance));
    REQUIRE(Apply<PrimitiveComponent>(world, changed, [&](PrimitiveComponent& ball) noexcept {
        ball.AddImpulse(Vec3d(0.0, 0.0, KickVelocity), true);
    }));
    fixture.Step(1);
    const double base = fixture.Kinematics(reference).linearVelocity.z;
    const double expected = KickVelocity * LinearKeep();
    CHECK(NearRelative(fixture.Kinematics(pushed).linearVelocity.z - base, expected));
    CHECK(NearRelative(fixture.Kinematics(changed).linearVelocity.z - base, expected));
}

// PA04: 스텝마다 위로 m·980 힘(또는 가속 980)을 주면 중력이 상쇄되어 높이가 그대로다.
TEST(PhysicsForce_ForceCancelsGravity) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle byForce = SpawnBall(fixture, 0.0);
    const ObjectHandle byAcceleration = SpawnBall(fixture, SideOffset);
    REQUIRE(!byForce.IsNull() && !byAcceleration.IsNull());
    const double mass = SphereMass(SphereRadius);
    for (int step = 0; step < FallSteps; ++step) {
        REQUIRE(Apply<PrimitiveComponent>(world, byForce, [&](PrimitiveComponent& ball) noexcept {
            ball.AddForce(Vec3d(0.0, 0.0, -GravityZ * mass));
        }));
        REQUIRE(Apply<PrimitiveComponent>(world, byAcceleration,
                                          [&](PrimitiveComponent& ball) noexcept {
                                              ball.AddForce(Vec3d(0.0, 0.0, -GravityZ), true);
                                          }));
        fixture.Step(1);
    }

    CHECK(Near(fixture.ComponentPose(byForce).translation.z, DropHeight, BalanceTolerance));
    CHECK(Near(fixture.ComponentPose(byAcceleration).translation.z, DropHeight, BalanceTolerance));
}

// PA05 컴포넌트 경로: 반폭이 다른 상자에 축마다 토크 1 N·m를 주면 한 스텝 뒤 각속도가 τ/I·dt다
// (Ixx = m·(hy² + hz²)/3 …). …InDegrees는 도를 rad로 바꾼 같은 결과다.
TEST(PhysicsForce_TorqueThroughComponent) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle boxes[] = {SpawnBox(fixture, 0.0), SpawnBox(fixture, SideOffset),
                                  SpawnBox(fixture, 2.0 * SideOffset)};
    for (const ObjectHandle box : boxes) {
        REQUIRE(!box.IsNull());
    }

    REQUIRE(Apply<PrimitiveComponent>(world, boxes[0], [](PrimitiveComponent& box) noexcept {
        box.AddTorqueInRadians(Vec3d(TorqueMagnitude, 0.0, 0.0));
    }));
    REQUIRE(Apply<PrimitiveComponent>(world, boxes[1], [](PrimitiveComponent& box) noexcept {
        box.AddTorqueInRadians(Vec3d(0.0, TorqueMagnitude, 0.0));
    }));
    REQUIRE(Apply<PrimitiveComponent>(world, boxes[2], [](PrimitiveComponent& box) noexcept {
        box.AddTorqueInDegrees(Vec3d(0.0, 0.0, math::RadiansToDegrees(TorqueMagnitude)));
    }));
    fixture.Step(1);
    const double mass = BoxMass();
    const double inertia[] = {mass * (BoxHalfY * BoxHalfY + BoxHalfZ * BoxHalfZ) / 3.0,
                              mass * (BoxHalfX * BoxHalfX + BoxHalfZ * BoxHalfZ) / 3.0,
                              mass * (BoxHalfX * BoxHalfX + BoxHalfY * BoxHalfY) / 3.0};
    const Vec3d spins[] = {fixture.Kinematics(boxes[0]).angularVelocityRadians,
                           fixture.Kinematics(boxes[1]).angularVelocityRadians,
                           fixture.Kinematics(boxes[2]).angularVelocityRadians};
    CHECK(NearRelative(spins[0].x, TorqueMagnitude / inertia[0] * StepSeconds));
    CHECK(NearRelative(spins[1].y, TorqueMagnitude / inertia[1] * StepSeconds));
    CHECK(NearRelative(spins[2].z, TorqueMagnitude / inertia[2] * StepSeconds));
    // 관성이 Ixx > Iyy > Izz라 같은 토크의 각속도는 반대 순서다(축 index 확인).
    CHECK(spins[0].x < spins[1].y);
    CHECK(spins[1].y < spins[2].z);
}

// 속도 setter는 바로 보인다. 각속도 도 ↔ rad, 점 속도 v + ω × (점 − 질량 중심). body가 없으면
// 0이다.
TEST(PhysicsForce_VelocityThroughComponent) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle ball = SpawnBall(fixture, 0.0);
    const ObjectHandle ghost = SpawnBall(fixture, SideOffset, [](SphereComponent& sphere) noexcept {
        (void)sphere.SetCollisionEnabled(CollisionEnabled::NoCollision);
    });
    REQUIRE(!ball.IsNull() && !ghost.IsNull());
    const Vec3d center = fixture.ComponentPose(ball).translation;
    REQUIRE(Apply<PrimitiveComponent>(world, ball, [&](PrimitiveComponent& sphere) noexcept {
        sphere.SetPhysicsLinearVelocity(Vec3d(KickVelocity, 0.0, 0.0));
        sphere.SetPhysicsAngularVelocityInDegrees(Vec3d(0.0, 0.0, QuarterTurnDegrees));
        CHECK(Near(sphere.GetPhysicsLinearVelocity().x, KickVelocity, VelocityTolerance));
        CHECK(Near(sphere.GetPhysicsAngularVelocityInRadians().z, math::Pi<double> * 0.5,
                   VelocityTolerance));
        CHECK(Near(sphere.GetPhysicsAngularVelocityInDegrees().z, QuarterTurnDegrees,
                   VelocityTolerance));
        // ω = (0, 0, π/2), 팔 (r, 0, 0) → ω × 팔 = (0, π/2·r, 0).
        const Vec3d point =
            sphere.GetPhysicsLinearVelocityAtPoint(center + Vec3d(PointArm, 0.0, 0.0));
        CHECK(Near(point.x, KickVelocity, VelocityTolerance));
        CHECK(Near(point.y, math::Pi<double> * 0.5 * PointArm, VelocityTolerance));
        sphere.SetPhysicsLinearVelocity(Vec3d(0.0, KickVelocity, 0.0), true);
        CHECK(Near(sphere.GetPhysicsLinearVelocity().y, KickVelocity, VelocityTolerance));
    }));
    REQUIRE(Apply<PrimitiveComponent>(world, ghost, [](PrimitiveComponent& sphere) noexcept {
        sphere.SetPhysicsLinearVelocity(Vec3d(KickVelocity, 0.0, 0.0));
        CHECK(sphere.GetPhysicsLinearVelocity() == Vec3d::Zero());
        CHECK(sphere.GetPhysicsAngularVelocityInRadians() == Vec3d::Zero());
    }));
}

// ignoreRadialImpulse·ignoreRadialForce면 방사 호출을 무시한다(기준 구와 같은 속도). 표지가 없으면
// 아래 중심의 방사 속도 변화가 위로 더해진다.
TEST(PhysicsForce_RadialIgnoreFlags) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle reference = SpawnBall(fixture, 0.0);
    const ObjectHandle ignoring = SpawnBall(fixture, SideOffset, [](SphereComponent& s) noexcept {
        s.SetIgnoreRadialImpulse(true);
        s.SetIgnoreRadialForce(true);
    });
    const ObjectHandle pushed = SpawnBall(fixture, 2.0 * SideOffset);
    REQUIRE(!reference.IsNull() && !ignoring.IsNull() && !pushed.IsNull());
    for (const ObjectHandle target : {ignoring, pushed}) {
        const Vec3d below = fixture.ComponentPose(target).translation - Vec3d(0.0, 0.0, PointArm);
        REQUIRE(Apply<PrimitiveComponent>(world, target, [&](PrimitiveComponent& ball) noexcept {
            CHECK(ball.GetIgnoreRadialImpulse() == (target == ignoring));
            ball.AddRadialImpulse(below, RadialRadius, RadialStrength, RadialFalloff::Constant,
                                  true);
            ball.AddRadialForce(below, RadialRadius, -GravityZ, RadialFalloff::Constant, true);
        }));
    }

    fixture.Step(1);
    const double base = fixture.Kinematics(reference).linearVelocity.z;
    CHECK(Near(fixture.Kinematics(ignoring).linearVelocity.z, base, VelocityTolerance));
    // 방사 속도 변화 + 중력 상쇄 가속(한 스텝)이다.
    const double expected = (RadialStrength - GravityZ * StepSeconds) * LinearKeep();
    CHECK(NearRelative(fixture.Kinematics(pushed).linearVelocity.z - base, expected));
}

// 잘못된 호출 경고(UE WarnInvalidPhysicsOperations): 시뮬레이션 꺼짐, 물리 충돌 없음(QueryOnly),
// Movable 아님. 방사·속도 setter는 body 경고를 하지 않는다(속도 setter는 Mobility만).
TEST(PhysicsForce_WarnsInvalidOperations) {
    PhysicsOperationWarnings warnings;
    SystemAllocator logAllocator;
    LogSystemConfig logConfig;
    logConfig.mode = LogMode::Manual;
    logConfig.globalMinimum = LogLevel::Debug;
    LogSystem logs(logAllocator, logConfig);
    REQUIRE(logs.Start() && logs.AddSink(warnings));
    LogInstallScope installed(logs);
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle kinematic = SpawnBall(
        fixture, 0.0, [](SphereComponent& sphere) noexcept { sphere.SetSimulatePhysics(false); });
    const ObjectHandle queryOnly =
        SpawnBall(fixture, SideOffset, [](SphereComponent& sphere) noexcept {
            (void)sphere.SetCollisionEnabled(CollisionEnabled::QueryOnly);
        });
    const ObjectHandle fixed = fixture.SpawnShape<SphereComponent>([](SphereComponent& s) noexcept {
        s.SetMobility(ComponentMobility::Static);
        (void)s.SetRelativeLocation(Vec3d(2.0 * SideOffset, 0.0, DropHeight));
    });
    REQUIRE(!kinematic.IsNull() && !queryOnly.IsNull() && !fixed.IsNull());
    (void)logs.Pump();
    warnings.withoutSimulation = 0;
    warnings.withoutCollision = 0;
    warnings.notMovable = 0;

    REQUIRE(Apply<PrimitiveComponent>(world, kinematic, [](PrimitiveComponent& ball) noexcept {
        ball.AddImpulse(Vec3d(0.0, 0.0, KickVelocity));
        ball.AddRadialImpulse(Vec3d::Zero(), RadialRadius, RadialStrength, RadialFalloff::Linear);
        ball.SetPhysicsLinearVelocity(Vec3d(KickVelocity, 0.0, 0.0));
    }));
    REQUIRE(Apply<PrimitiveComponent>(world, queryOnly, [](PrimitiveComponent& ball) noexcept {
        ball.AddForce(Vec3d(0.0, 0.0, KickVelocity));
    }));
    REQUIRE(Apply<PrimitiveComponent>(world, fixed, [](PrimitiveComponent& ball) noexcept {
        ball.AddTorqueInRadians(Vec3d(0.0, 0.0, TorqueMagnitude));
        ball.SetPhysicsAngularVelocityInRadians(Vec3d(0.0, 0.0, 1.0));
    }));
    (void)logs.Pump();
    CHECK(warnings.notMovable == 2u);
#if !ROCKS_BUILD_RELEASE
    CHECK(warnings.withoutSimulation == 1u);
    CHECK(warnings.withoutCollision == 1u);
#else
    CHECK(warnings.withoutSimulation == 0u);
    CHECK(warnings.withoutCollision == 0u);
#endif
}

// 고정 스텝 밖의 힘은 그 프레임의 모든 고정 스텝에 작용하고(두 스텝 프레임에서 중력 상쇄가 둘 다),
// 고정 스텝이 0인 프레임의 힘은 버린다(다음 프레임 스텝에 남지 않는다 — UE bAllowSubstepping,
// 7.1.17절). 프레임 속도와 상관없이 같은 지속 힘이다.
TEST(PhysicsForce_ForceSpansFrameFixedSteps) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle reference = SpawnBall(fixture, 0.0);
    const ObjectHandle lifted = SpawnBall(fixture, SideOffset);
    const ObjectHandle skipped = SpawnBall(fixture, 2.0 * SideOffset);
    REQUIRE(!reference.IsNull() && !lifted.IsNull() && !skipped.IsNull());
    // 두 고정 스텝 프레임: 중력 상쇄가 두 스텝 다 작용해 멈춰 있다.
    REQUIRE(Apply<PrimitiveComponent>(world, lifted, [](PrimitiveComponent& ball) noexcept {
        ball.AddForce(Vec3d(0.0, 0.0, -GravityZ), true);
    }));
    CHECK(world.Tick(2.0 * StepSeconds));
    CHECK(Near(fixture.Kinematics(lifted).linearVelocity.z, 0.0, VelocityTolerance));
    CHECK(fixture.Kinematics(reference).linearVelocity.z < 0.0);

    // 고정 스텝이 0인 프레임의 힘은 버린다: 다음 프레임의 스텝에서 기준 구와 같이 떨어진다.
    const double before = fixture.Kinematics(skipped).linearVelocity.z;
    const double referenceBefore = fixture.Kinematics(reference).linearVelocity.z;
    REQUIRE(Apply<PrimitiveComponent>(world, skipped, [](PrimitiveComponent& ball) noexcept {
        ball.AddForce(Vec3d(0.0, 0.0, -GravityZ), true);
    }));
    CHECK(world.Tick(0.5 * StepSeconds));
    CHECK(Near(fixture.Kinematics(skipped).linearVelocity.z, before, VelocityTolerance));
    CHECK(world.Tick(0.5 * StepSeconds));
    const double fell = fixture.Kinematics(skipped).linearVelocity.z - before;
    const double referenceFell = fixture.Kinematics(reference).linearVelocity.z - referenceBefore;
    CHECK(Near(fell, referenceFell, VelocityTolerance));
    CHECK(fell < 0.0);
}

} // namespace physics_force_tests_file
