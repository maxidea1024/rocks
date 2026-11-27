// Physics 모듈 PH5c2 컴포넌트 시험이다(Docs/Design/PhysicsModule 2.6절, 7.1.18절): PA04 정확(질량
// 덮어쓰기 2 kg), 관성·질량 중심 읽기와 이동, 각속도 상한(기본·덮어쓰기·더하기·자르기), 중력 끔,
// 감쇠·CCD 왕복, SetConstraintMode, body 없음 기본값, 잘못된 값 거부. World·컴포넌트 공개 API만
// 쓰므로 Modular 시험에서도 돈다.
#include "PhysicsBodyFixture.h"

#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>

using namespace rocks;
namespace physics_body_value_tests_file {

using namespace physics_test_support;

namespace {

constexpr float BallMass = 2.0f;   // kg(PA04)
constexpr double ImpulseZ = 200.0; // kg·cm/s → Δv 100 cm/s
constexpr double RelativeTolerance = 1.0e-4;
constexpr double CenterOffset = 10.0;   // cm
constexpr float LimitDegrees = 90.0f;   // 도/s
constexpr float LimitIncrement = 10.0f; // 도/s
constexpr double FastSpin = 10.0;       // rad/s(상한보다 빠름)
constexpr float NewDamping = 0.5f;
constexpr float AngleTolerance = 1.0e-3f;
// 반폭이 다른 상자(cm — 축마다 다른 관성)다.
constexpr double BoxHalfX = 10.0;
constexpr double BoxHalfY = 20.0;
constexpr double BoxHalfZ = 30.0;

bool NearRelative(double value, double expected) noexcept {
    return Near(value, expected, math::Abs(expected) * RelativeTolerance);
}

bool StartWorld(BodyWorldFixture& fixture) noexcept {
    return fixture.registered && fixture.world && fixture.world->Initialize() &&
           fixture.world->BeginPlay();
}

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

class InvalidValueWarnings final : public RuntimeLogSink {
public:
    Result<void> Write(const LogEvent& event) noexcept override {
        if (StringView(event.message)
                .Contains("(InvalidBodyValue)", StringComparison::CaseSensitive)) {
            ++count;
        }

        return Result<void>::Success();
    }

    uint32 count = 0;
};

} // namespace

// PA04 정확: 질량을 2 kg로 덮어쓰면 GetMass가 2이고, 충격량 (0, 0, 200) kg·cm/s가 한 스텝 뒤 기준
// 구보다 100 cm/s(감쇠 배율) 빠르다. 관성은 구 2/5·m·r²다.
TEST(PhysicsBodyValue_MassOverrideMatchesPa04) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle reference = SpawnBall(fixture, 0.0);
    const ObjectHandle pushed = SpawnBall(fixture, SideOffset);
    REQUIRE(!reference.IsNull() && !pushed.IsNull());
    REQUIRE(Apply<PrimitiveComponent>(world, pushed, [](PrimitiveComponent& ball) noexcept {
        ball.SetMassOverrideInKg(BallMass);
        CHECK(NearRelative(ball.GetMass(), BallMass));
        const double inertia = 0.4 * static_cast<double>(BallMass) * SphereRadius * SphereRadius;
        const Vec3d tensor = ball.GetInertiaTensor();
        CHECK(NearRelative(tensor.x, inertia));
        CHECK(NearRelative(tensor.y, inertia));
        CHECK(NearRelative(tensor.z, inertia));
        ball.AddImpulse(Vec3d(0.0, 0.0, ImpulseZ));
    }));
    fixture.Step(1);
    const double keep = 1.0 - static_cast<double>(DefaultLinearDamping) * StepSeconds;
    const double gained = fixture.Kinematics(pushed).linearVelocity.z -
                          fixture.Kinematics(reference).linearVelocity.z;
    CHECK(NearRelative(gained, ImpulseZ / static_cast<double>(BallMass) * keep));
}

// 질량 중심 이동이 GetCenterOfMass를 옮기고, 중력을 끄면 떨어지지 않는다. 감쇠·CCD·gyroscopic 값이
// 왕복한다.
TEST(PhysicsBodyValue_CenterGravityAndFlags) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle ball = SpawnBall(fixture, 0.0);
    REQUIRE(!ball.IsNull());
    REQUIRE(Apply<PrimitiveComponent>(world, ball, [](PrimitiveComponent& sphere) noexcept {
        const Vec3d center = sphere.GetCenterOfMass();
        sphere.SetCenterOfMass(Vec3d(0.0, 0.0, -CenterOffset));
        CHECK(Near(sphere.GetCenterOfMass().z, center.z - CenterOffset, PositionTolerance));
        sphere.SetEnableGravity(false);
        CHECK(!sphere.IsGravityEnabled());
        sphere.SetLinearDamping(NewDamping);
        sphere.SetAngularDamping(NewDamping);
        CHECK(sphere.GetLinearDamping() == NewDamping);
        CHECK(sphere.GetAngularDamping() == NewDamping);
        sphere.SetUseCCD(true);
        CHECK(sphere.GetBodySettings().continuousCollision);
        sphere.SetGyroscopicTorqueEnabled(true);
        CHECK(sphere.GetGyroscopicTorqueEnabled());
    }));
    const double height = fixture.ComponentPose(ball).translation.z;
    fixture.Step(FallSteps);
    CHECK(Near(fixture.ComponentPose(ball).translation.z, height, PositionTolerance));
}

// 각속도 상한: 기본은 물리 설정(3600 도/s), 덮어쓰기·더하기, rad 읽기. 상한보다 빠른 각속도는
// 잘린다. 음수는 거부한다(Warning InvalidBodyValue).
TEST(PhysicsBodyValue_MaxAngularVelocity) {
    InvalidValueWarnings warnings;
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
    const ObjectHandle ball = SpawnBall(fixture, 0.0);
    REQUIRE(!ball.IsNull());
    REQUIRE(Apply<PrimitiveComponent>(world, ball, [](PrimitiveComponent& sphere) noexcept {
        CHECK(sphere.GetPhysicsMaxAngularVelocityInDegrees() == DefaultMaxAngularVelocityDegrees);
        sphere.SetPhysicsMaxAngularVelocityInDegrees(LimitDegrees);
        sphere.SetPhysicsMaxAngularVelocityInDegrees(LimitIncrement, true);
        const float limit = LimitDegrees + LimitIncrement;
        CHECK(sphere.GetPhysicsMaxAngularVelocityInDegrees() == limit);
        CHECK(math::Abs(sphere.GetPhysicsMaxAngularVelocityInRadians() -
                        math::DegreesToRadians(limit)) <= AngleTolerance);
        sphere.SetPhysicsMaxAngularVelocityInDegrees(-LimitDegrees);
        CHECK(sphere.GetPhysicsMaxAngularVelocityInDegrees() == limit);
        sphere.SetPhysicsAngularVelocityInRadians(Vec3d(0.0, 0.0, FastSpin));
        CHECK(Near(sphere.GetPhysicsAngularVelocityInRadians().z,
                   math::DegreesToRadians(static_cast<double>(limit)), AngleTolerance));
        sphere.SetLinearDamping(-NewDamping);
        CHECK(sphere.GetLinearDamping() == DefaultLinearDamping);
    }));
    (void)logs.Pump();
    CHECK(warnings.count == 2u);

    // 덮어쓰기가 아니면 더하기는 물리 설정 상한(3600 도/s)에 더한다.
    const ObjectHandle fresh = SpawnBall(fixture, SideOffset);
    REQUIRE(!fresh.IsNull());
    REQUIRE(Apply<PrimitiveComponent>(world, fresh, [](PrimitiveComponent& sphere) noexcept {
        sphere.SetPhysicsMaxAngularVelocityInDegrees(LimitIncrement, true);
        CHECK(sphere.GetPhysicsMaxAngularVelocityInDegrees() ==
              DefaultMaxAngularVelocityDegrees + LimitIncrement);
    }));
}

// 반폭이 다른 상자의 관성 세 성분이 m·(h2² + h3²)/3이다(축 사상 — 구는 X·Y가 바뀌어도 같다).
TEST(PhysicsBodyValue_BoxInertiaAxes) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle box = fixture.SpawnShape<BoxComponent>([](BoxComponent& shape) noexcept {
        (void)shape.SetBoxExtent(Vec3d(BoxHalfX, BoxHalfY, BoxHalfZ));
        (void)shape.SetRelativeLocation(Vec3d(0.0, 0.0, DropHeight));
        shape.SetSimulatePhysics(true);
    });
    REQUIRE(!box.IsNull());
    REQUIRE(Apply<PrimitiveComponent>(world, box, [](PrimitiveComponent& shape) noexcept {
        shape.SetMassOverrideInKg(BallMass);
        const double mass = static_cast<double>(BallMass);
        const Vec3d tensor = shape.GetInertiaTensor();
        CHECK(NearRelative(tensor.x, mass * (BoxHalfY * BoxHalfY + BoxHalfZ * BoxHalfZ) / 3.0));
        CHECK(NearRelative(tensor.y, mass * (BoxHalfX * BoxHalfX + BoxHalfZ * BoxHalfZ) / 3.0));
        CHECK(NearRelative(tensor.z, mass * (BoxHalfX * BoxHalfX + BoxHalfY * BoxHalfY) / 3.0));
    }));
}

// SetConstraintMode(XYPlane)는 Z 이동을 잠근다(중력에도 높이 유지).
TEST(PhysicsBodyValue_ConstraintModeLocksPlane) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle ball = SpawnBall(fixture, 0.0);
    REQUIRE(!ball.IsNull());
    REQUIRE(Apply<PrimitiveComponent>(world, ball, [](PrimitiveComponent& sphere) noexcept {
        sphere.SetConstraintMode(DofMode::XYPlane);
        CHECK(sphere.GetBodySettings().dofMode == DofMode::XYPlane);
    }));
    const double height = fixture.ComponentPose(ball).translation.z;
    fixture.Step(FallSteps);
    CHECK(Near(fixture.ComponentPose(ball).translation.z, height, PositionTolerance));
}

// body가 없으면 질량·관성·질량 중심은 0이고 setter는 값만 바꾼다.
TEST(PhysicsBodyValue_NoBodyDefaults) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle ghost = SpawnBall(fixture, 0.0, [](SphereComponent& sphere) noexcept {
        (void)sphere.SetCollisionEnabled(CollisionEnabled::NoCollision);
    });
    REQUIRE(!ghost.IsNull());
    REQUIRE(Apply<PrimitiveComponent>(world, ghost, [](PrimitiveComponent& sphere) noexcept {
        CHECK(sphere.GetMass() == 0.0f);
        CHECK(sphere.GetInertiaTensor() == Vec3d::Zero());
        CHECK(sphere.GetCenterOfMass() == Vec3d::Zero());
        sphere.SetMassOverrideInKg(BallMass);
        CHECK(sphere.GetBodySettings().overrideMass);
        CHECK(sphere.GetBodySettings().massInKgOverride == BallMass);
        sphere.SetMassScale(BallMass);
        CHECK(sphere.GetMassScale() == BallMass);
    }));
}

} // namespace physics_body_value_tests_file
