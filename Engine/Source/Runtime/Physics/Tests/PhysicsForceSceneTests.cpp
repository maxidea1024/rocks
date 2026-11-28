// Physics 모듈 PH5c1 장면 시험이다(Docs/Design/PhysicsModule 2.6절, 7.1.17절): 힘·충격량·토크·방사
// 힘과 속도 접점 — 충격량은 다음 step에 더함(UE impulse velocity), 대기 몫 버림(Kinematic 전환·body
// 삭제), 각·위치 충격량, 방사 충격량·힘의 경계와 감쇠, 질량 무관 힘·토크, body 공간 위치 힘,
// Static·Kinematic 무시와 깨움, 속도 setter의 깨움 문턱·자유도 거르기·더하기, 비유한 입력
// 무시(float 넘침 포함), 돈 비대칭 상자의 월드 관성, 프레임 힘(고정 스텝마다·EndFrame). Jolt 칸을
// 직접 만들므로 Monolithic 시험(EngineTests)에서만 돈다(BuildTargets 3.8절 BD29 시험 실행 파일
// 예외).
#include "../Private/JoltPhysicsScene.h"

#include <Base/InitializerList.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Physics/PhysicsSettings.h>
#include <Platform/PlatformMath.h>
#include <TestSupport/Test.h>
#include <World/Physics/BodyCreateDesc.h>

using namespace rocks;
namespace physics_force_scene_tests_file {

namespace {

constexpr float StepSeconds = 1.0f / 60.0f;
constexpr uint32 TestMaxBodies = 64;
constexpr uint32 TestMaxBodyPairs = 256;
constexpr uint32 TestMaxContactConstraints = 256;
constexpr uint32 TestTempAllocatorBytes = 4u * 1024u * 1024u;
constexpr double SphereRadius = 20.0;
constexpr double BodySpacing = 500.0; // cm(body끼리 닿지 않는 간격)
constexpr float BodyMass = 2.0f;      // kg(PA04)
constexpr float OtherMass = 7.0f;     // kg(질량 무관 시험의 다른 body)
// 구 관성 2/5·m·r²(kg·cm² — UE 질량 비율로 줄인 Jolt 구 관성, 7.1.15절).
constexpr double SphereInertia = 0.4 * static_cast<double>(BodyMass) * SphereRadius * SphereRadius;
constexpr double ImpulseZ = 200.0;           // kg·cm/s
constexpr double VelocityTolerance = 1.0e-2; // cm/s(float 왕복)
constexpr double SpinTolerance = 1.0e-4;     // rad/s
constexpr double RadialStrength = 200.0;     // kg·cm/s·kg·cm/s²
constexpr double RadialDistance = 100.0;     // cm(질량 중심 ↔ 방사 중심)
constexpr double TinyVelocity = 1.0e-5;      // cm/s(깨우지 않는 속도 — KindaSmallNumber 아래)
constexpr double WakeVelocity = 1.0;         // cm/s
constexpr double Gravity = 980.0;            // cm/s²(질량 무관 힘 시험의 가속도)
constexpr double AngularAcceleration = 10.0; // rad/s²
constexpr double TorqueZ = 1.0e4;            // kg·cm²/s²(= 1 N·m)
constexpr uint64 FakeRegistry = 1;
constexpr uint32 FakeGeneration = 1;
// 돈 비대칭 상자 시험의 반폭(cm)과 회전(빗축 둘레 — 대칭이 없어 R·I·Rᵀ와 Rᵀ·I·R이 다르다)이다.
constexpr double BoxHalfX = 10.0;
constexpr double BoxHalfY = 20.0;
constexpr double BoxHalfZ = 30.0;
constexpr double TiltRadians = 0.7;
constexpr double LocalYawRadians = math::Pi<double> / 6.0;
// double로는 유한하지만 Jolt float로 바꾸면 무한대인 값이다.
constexpr double FloatOverflow = 1.0e40;

PhysicsSettings TestSettings() noexcept {
    PhysicsSettings settings;
    settings.maxBodies = TestMaxBodies;
    settings.maxBodyPairs = TestMaxBodyPairs;
    settings.maxContactConstraints = TestMaxContactConstraints;
    settings.tempAllocatorBytes = TestTempAllocatorBytes;
    return settings;
}

bool Near(double value, double expected, double tolerance) noexcept {
    return math::Abs(value - expected) <= tolerance;
}

bool NearVector(const Vec3d& value, const Vec3d& expected, double tolerance) noexcept {
    return Near(value.x, expected.x, tolerance) && Near(value.y, expected.y, tolerance) &&
           Near(value.z, expected.z, tolerance);
}

// 중력·감쇠 없이 떠 있고 질량이 mass인 설정이다(속도 변화가 그대로 남는다).
BodySettings Floating(Allocator& allocator, float mass = BodyMass) noexcept {
    BodySettings settings(allocator);
    settings.enableGravity = false;
    settings.linearDamping = 0.0f;
    settings.angularDamping = 0.0f;
    settings.overrideMass = true;
    settings.massInKgOverride = mass;
    return settings;
}

BodyForce Command(BodyForceKind kind, const Vec3d& value, bool massIndependent = false) noexcept {
    BodyForce force;
    force.kind = kind;
    force.value = value;
    force.massIndependent = massIndependent;
    return force;
}

BodyForce Radial(BodyForceKind kind, const Vec3d& origin, double radius,
                 RadialFalloff falloff) noexcept {
    BodyForce force;
    force.kind = kind;
    force.location = origin;
    force.radius = radius;
    force.strength = RadialStrength;
    force.falloff = falloff;
    return force;
}

struct ForceScene {
    ForceScene() noexcept {
        auto created = physics::JoltPhysicsScene::Create(allocator, TestSettings(), nullptr);
        if (created) {
            scene = Move(created.Value());
            scene->SetSimulates(true);
        }
    }

    PhysicsBodyHandle Sphere(const BodySettings& settings, BodyMotion motion = BodyMotion::Dynamic,
                             const Quatd& rotation = Quatd::Identity()) noexcept {
        BodyShapeElement geometry;
        geometry.kind = BodyGeometryKind::Sphere;
        geometry.radius = SphereRadius;
        return Make(settings, geometry, motion, rotation);
    }

    PhysicsBodyHandle Box(const BodySettings& settings, const Quatd& rotation) noexcept {
        BodyShapeElement geometry;
        geometry.kind = BodyGeometryKind::Box;
        geometry.halfExtent = Vec3d(BoxHalfX, BoxHalfY, BoxHalfZ);
        return Make(settings, geometry, BodyMotion::Dynamic, rotation);
    }

    PhysicsBodyHandle Make(const BodySettings& settings, const BodyShapeElement& geometry,
                           BodyMotion motion, const Quatd& rotation) noexcept {
        BodyCreateDesc desc;
        desc.component = ObjectHandle{FakeRegistry, ++tags, FakeGeneration};
        desc.actor = desc.component;
        desc.geometry = Span<const BodyShapeElement>(&geometry, 1);
        desc.worldTransform.translation = Vec3d(static_cast<double>(tags) * BodySpacing, 0.0, 0.0);
        desc.worldTransform.rotation = rotation;
        desc.motion = motion;
        desc.collision.collisionEnabled = CollisionEnabled::QueryAndPhysics;
        desc.settings = &settings;
        // 잠든 채 넣을지는 컴포넌트가 정한다(startAwake·BeginPlay 전 — 2.3절).
        desc.startAwake = settings.startAwake;
        auto created = scene->CreateBody(desc);
        CHECK(created);
        return created ? created.Value() : PhysicsBodyHandle{};
    }

    void Step() noexcept {
        CHECK(scene->Step(StepSeconds));
    }

    BodyKinematics Read(PhysicsBodyHandle body) noexcept {
        auto read = scene->ReadKinematics(body);
        CHECK(read);
        return read ? read.Value() : BodyKinematics{};
    }

    SystemAllocator allocator;
    UniquePtr<physics::JoltPhysicsScene> scene;
    uint32 tags = 0;
};

} // namespace

// PA04 장면 경로: 질량 2 kg body에 충격량 (0, 0, 200) kg·cm/s → step 전에는 옛 속도, step 뒤 Δv
// (0, 0, 100) cm/s(UE impulse velocity). 같은 프레임의 속도 setter 뒤에도 대기 몫이 더해지고,
// 질량 무관 충격량은 값 그대로 속도 변화다.
TEST(Physics_ForceImpulseWaitsForStep) {
    ForceScene scene;
    REQUIRE(scene.scene);
    const BodySettings settings = Floating(scene.allocator);
    const PhysicsBodyHandle ball = scene.Sphere(settings);
    scene.scene->ApplyBodyForce(ball, Command(BodyForceKind::Impulse, Vec3d(0.0, 0.0, ImpulseZ)));
    CHECK(NearVector(scene.Read(ball).linearVelocity, Vec3d::Zero(), VelocityTolerance));
    scene.Step();
    const double kick = ImpulseZ / static_cast<double>(BodyMass);
    CHECK(Near(scene.Read(ball).linearVelocity.z, kick, VelocityTolerance));

    scene.scene->ApplyBodyForce(ball, Command(BodyForceKind::Impulse, Vec3d(0.0, 0.0, ImpulseZ)));
    scene.scene->SetBodyLinearVelocity(ball, Vec3d::Zero(), false);
    scene.Step();
    CHECK(Near(scene.Read(ball).linearVelocity.z, kick, VelocityTolerance));

    scene.scene->ApplyBodyForce(ball,
                                Command(BodyForceKind::Impulse, Vec3d(0.0, 0.0, ImpulseZ), true));
    scene.Step();
    CHECK(Near(scene.Read(ball).linearVelocity.z, kick + ImpulseZ, VelocityTolerance));
}

// 대기 충격량·누적 힘은 Kinematic 전환이 버리고(다시 Dynamic이어도 0), body를 지우면 같은 칸의 새
// body에 남지 않는다.
TEST(Physics_ForcePendingDroppedOnKinematicAndDestroy) {
    ForceScene scene;
    REQUIRE(scene.scene);
    const BodySettings settings = Floating(scene.allocator);
    const PhysicsBodyHandle ball = scene.Sphere(settings);
    scene.scene->ApplyBodyForce(ball, Command(BodyForceKind::Impulse, Vec3d(0.0, 0.0, ImpulseZ)));
    scene.scene->ApplyBodyForce(ball,
                                Command(BodyForceKind::Force, Vec3d(0.0, 0.0, Gravity), true));
    scene.scene->SetBodyMotion(ball, BodyMotion::Kinematic, false);
    scene.scene->SetBodyMotion(ball, BodyMotion::Dynamic, true);
    scene.Step();
    CHECK(NearVector(scene.Read(ball).linearVelocity, Vec3d::Zero(), VelocityTolerance));

    const PhysicsBodyHandle doomed = scene.Sphere(settings);
    scene.scene->ApplyBodyForce(doomed, Command(BodyForceKind::Impulse, Vec3d(0.0, 0.0, ImpulseZ)));
    scene.scene->DestroyBody(doomed);
    const PhysicsBodyHandle reused = scene.Sphere(settings);
    CHECK(reused.index == doomed.index);
    scene.Step();
    CHECK(NearVector(scene.Read(reused).linearVelocity, Vec3d::Zero(), VelocityTolerance));
}

// 각충격량 Δω = I⁻¹·L(구 2/5·m·r²), 질량 무관이면 값 그대로다. 위치 충격량은 선형 J/m과 각
// I⁻¹·((위치 − 질량 중심) × J)이고, 속도 변화 위치 충격량은 J = m·Δv로 같은 결과다.
TEST(Physics_ForceAngularAndLocatedImpulses) {
    ForceScene scene;
    REQUIRE(scene.scene);
    const BodySettings settings = Floating(scene.allocator);
    const PhysicsBodyHandle spinner = scene.Sphere(settings);
    scene.scene->ApplyBodyForce(
        spinner, Command(BodyForceKind::AngularImpulse, Vec3d(0.0, 0.0, SphereInertia)));
    scene.scene->ApplyBodyForce(spinner,
                                Command(BodyForceKind::AngularImpulse, Vec3d(1.0, 0.0, 0.0), true));
    scene.Step();
    CHECK(NearVector(scene.Read(spinner).angularVelocityRadians, Vec3d(1.0, 0.0, 1.0),
                     SpinTolerance));

    const Vec3d arm(SphereRadius, 0.0, 0.0);
    const double kick = ImpulseZ / static_cast<double>(BodyMass);
    // (r, 0, 0) × (0, 0, J) = (0, −r·J, 0).
    const double expectedSpinY = -SphereRadius * ImpulseZ / SphereInertia;
    for (const bool velocityChange : {false, true}) {
        const PhysicsBodyHandle ball = scene.Sphere(settings);
        BodyForce located =
            Command(BodyForceKind::ImpulseAtLocation,
                    Vec3d(0.0, 0.0, velocityChange ? kick : ImpulseZ), velocityChange);
        located.location = scene.Read(ball).centerOfMass + arm;
        scene.scene->ApplyBodyForce(ball, located);
        scene.Step();
        const BodyKinematics after = scene.Read(ball);
        CHECK(Near(after.linearVelocity.z, kick, VelocityTolerance));
        CHECK(NearVector(after.angularVelocityRadians, Vec3d(0.0, expectedSpinY, 0.0),
                         SpinTolerance));
    }
}

// 방사 충격량: 거리 < radius(엄격)일 때만, Constant는 strength, Linear는 (radius − 거리)/radius
// 비율, 중심 일치는 +X. 방사 힘: 거리 <= radius(경계 포함)이고 Linear 비율이 같다.
TEST(Physics_ForceRadialBoundariesAndFalloff) {
    ForceScene scene;
    REQUIRE(scene.scene);
    const BodySettings settings = Floating(scene.allocator);
    const double mass = static_cast<double>(BodyMass);
    const auto impulseKick = [&](double radius, RadialFalloff falloff, const Vec3d& offset) {
        const PhysicsBodyHandle ball = scene.Sphere(settings);
        const Vec3d origin = scene.Read(ball).centerOfMass - offset;
        scene.scene->ApplyBodyForce(ball,
                                    Radial(BodyForceKind::RadialImpulse, origin, radius, falloff));
        scene.Step();
        return scene.Read(ball).linearVelocity;
    };
    const Vec3d along(RadialDistance, 0.0, 0.0);
    const double full = RadialStrength / mass;
    CHECK(NearVector(impulseKick(2.0 * RadialDistance, RadialFalloff::Constant, along),
                     Vec3d(full, 0.0, 0.0), VelocityTolerance));
    CHECK(NearVector(impulseKick(2.0 * RadialDistance, RadialFalloff::Linear, along),
                     Vec3d(0.5 * full, 0.0, 0.0), VelocityTolerance));
    CHECK(NearVector(impulseKick(RadialDistance, RadialFalloff::Constant, along), Vec3d::Zero(),
                     VelocityTolerance));
    CHECK(NearVector(impulseKick(RadialDistance, RadialFalloff::Linear, Vec3d::Zero()),
                     Vec3d(full, 0.0, 0.0), VelocityTolerance));

    const auto forceKick = [&](double radius, RadialFalloff falloff) {
        const PhysicsBodyHandle ball = scene.Sphere(settings);
        const Vec3d origin = scene.Read(ball).centerOfMass - along;
        scene.scene->ApplyBodyForce(ball,
                                    Radial(BodyForceKind::RadialForce, origin, radius, falloff));
        scene.Step();
        return scene.Read(ball).linearVelocity.x;
    };
    const double step = static_cast<double>(StepSeconds);
    CHECK(Near(forceKick(RadialDistance, RadialFalloff::Constant), full * step, VelocityTolerance));
    CHECK(Near(forceKick(2.0 * RadialDistance, RadialFalloff::Linear), 0.5 * full * step,
               VelocityTolerance));
    CHECK(Near(forceKick(0.5 * RadialDistance, RadialFalloff::Constant), 0.0, VelocityTolerance));
    // 방사 힘 중심 일치는 +X다(Linear는 거리 0이라 strength 그대로).
    const PhysicsBodyHandle centered = scene.Sphere(settings);
    scene.scene->ApplyBodyForce(centered, Radial(BodyForceKind::RadialForce,
                                                 scene.Read(centered).centerOfMass, RadialDistance,
                                                 RadialFalloff::Linear));
    scene.Step();
    CHECK(NearVector(scene.Read(centered).linearVelocity, Vec3d(full * step, 0.0, 0.0),
                     VelocityTolerance));
}

// 질량 무관 힘·토크는 질량·관성과 상관없이 같은 가속이다. 토크 1 N·m(10⁴ kg·cm²/s²)는 τ/I
// 각가속도다 (PA05 장면 경로).
TEST(Physics_ForceAccelerationChange) {
    ForceScene scene;
    REQUIRE(scene.scene);
    const BodySettings light = Floating(scene.allocator);
    const BodySettings heavy = Floating(scene.allocator, OtherMass);
    const PhysicsBodyHandle bodies[] = {scene.Sphere(light), scene.Sphere(heavy)};
    for (const PhysicsBodyHandle body : bodies) {
        scene.scene->ApplyBodyForce(body,
                                    Command(BodyForceKind::Force, Vec3d(0.0, 0.0, Gravity), true));
        scene.scene->ApplyBodyForce(
            body, Command(BodyForceKind::Torque, Vec3d(0.0, 0.0, AngularAcceleration), true));
    }

    scene.Step();
    const double step = static_cast<double>(StepSeconds);
    for (const PhysicsBodyHandle body : bodies) {
        const BodyKinematics after = scene.Read(body);
        CHECK(Near(after.linearVelocity.z, Gravity * step, VelocityTolerance));
        CHECK(Near(after.angularVelocityRadians.z, AngularAcceleration * step, SpinTolerance));
    }

    const PhysicsBodyHandle twisted = scene.Sphere(light);
    scene.scene->ApplyBodyForce(twisted, Command(BodyForceKind::Torque, Vec3d(0.0, 0.0, TorqueZ)));
    scene.Step();
    CHECK(Near(scene.Read(twisted).angularVelocityRadians.z, TorqueZ / SphereInertia * step,
               SpinTolerance));
}

// body 공간 위치 힘은 body 원점 transform으로 바뀐다: Z축 둘레 90° 돈 body에서 로컬 (r, 0, 0)의 위
// 방향 힘은 월드 (0, r, 0)에 작용해 X축 둘레로 돌린다(돌지 않은 body는 Y축 둘레).
TEST(Physics_ForceLocalLocationRotates) {
    ForceScene scene;
    REQUIRE(scene.scene);
    const BodySettings settings = Floating(scene.allocator);
    const Quatd yaw = Quatd::FromUnitAxisAngle(Vec3d::UnitZ(), math::Pi<double> * 0.5);
    const PhysicsBodyHandle turned = scene.Sphere(settings, BodyMotion::Dynamic, yaw);
    const PhysicsBodyHandle plain = scene.Sphere(settings);
    BodyForce local = Command(BodyForceKind::ForceAtLocationLocal, Vec3d(0.0, 0.0, TorqueZ));
    local.location = Vec3d(SphereRadius, 0.0, 0.0);
    scene.scene->ApplyBodyForce(turned, local);
    scene.scene->ApplyBodyForce(plain, local);
    scene.Step();
    const Vec3d turnedSpin = scene.Read(turned).angularVelocityRadians;
    const Vec3d plainSpin = scene.Read(plain).angularVelocityRadians;
    const Vec3d expectedArm = yaw.RotateVector(local.location);
    const double spin = SphereRadius * TorqueZ / SphereInertia * static_cast<double>(StepSeconds);
    CHECK(NearVector(turnedSpin, Cross(expectedArm, Vec3d(0.0, 0.0, 1.0)) * (spin / SphereRadius),
                     SpinTolerance));
    CHECK(NearVector(plainSpin, Vec3d(0.0, -spin, 0.0), SpinTolerance));
}

// Static·Kinematic은 무시한다. 잠든 body는 효력이 있는 호출에 값과 상관없이 깨고(0 힘도),
// 속도 setter는 거의 0이 아닌 속도에만 깬다.
TEST(Physics_ForceIgnoresNonDynamicAndWakes) {
    ForceScene scene;
    REQUIRE(scene.scene);
    const BodySettings settings = Floating(scene.allocator);
    const PhysicsBodyHandle kinematic = scene.Sphere(settings, BodyMotion::Kinematic);
    scene.scene->ApplyBodyForce(kinematic,
                                Command(BodyForceKind::Impulse, Vec3d(0.0, 0.0, ImpulseZ)));
    scene.scene->SetBodyLinearVelocity(kinematic, Vec3d(0.0, 0.0, WakeVelocity), false);
    scene.Step();
    CHECK(NearVector(scene.Read(kinematic).linearVelocity, Vec3d::Zero(), VelocityTolerance));
    const PhysicsBodyHandle fixed = scene.Sphere(settings, BodyMotion::Static);
    scene.scene->ApplyBodyForce(fixed, Command(BodyForceKind::Impulse, Vec3d(0.0, 0.0, ImpulseZ)));
    scene.Step();
    CHECK(NearVector(scene.Read(fixed).linearVelocity, Vec3d::Zero(), VelocityTolerance));

    BodySettings asleep = Floating(scene.allocator);
    asleep.startAwake = false;
    const PhysicsBodyHandle pushed = scene.Sphere(asleep);
    CHECK(!scene.Read(pushed).awake);
    scene.scene->ApplyBodyForce(pushed, Command(BodyForceKind::Force, Vec3d::Zero()));
    CHECK(scene.Read(pushed).awake);

    const PhysicsBodyHandle nudged = scene.Sphere(asleep);
    scene.scene->SetBodyLinearVelocity(nudged, Vec3d(TinyVelocity, 0.0, 0.0), false);
    CHECK(!scene.Read(nudged).awake);
    scene.scene->SetBodyLinearVelocity(nudged, Vec3d(WakeVelocity, 0.0, 0.0), false);
    CHECK(scene.Read(nudged).awake);

    // 범위 밖 방사는 효력이 없어 깨우지 않는다.
    const PhysicsBodyHandle far = scene.Sphere(asleep);
    const Vec3d farOrigin = scene.Read(far).centerOfMass - Vec3d(RadialDistance, 0.0, 0.0);
    scene.scene->ApplyBodyForce(far, Radial(BodyForceKind::RadialImpulse, farOrigin,
                                            0.5 * RadialDistance, RadialFalloff::Constant));
    scene.scene->ApplyBodyForce(far, Radial(BodyForceKind::RadialForce, farOrigin,
                                            0.5 * RadialDistance, RadialFalloff::Constant));
    CHECK(!scene.Read(far).awake);
}

// 속도 setter는 허용 자유도로 거르고(X 이동·Z 회전 잠금) addToCurrent면 지금 속도에 더한다.
TEST(Physics_ForceVelocitySetterMasksAndAdds) {
    ForceScene scene;
    REQUIRE(scene.scene);
    BodySettings settings = Floating(scene.allocator);
    settings.dofMode = DofMode::SixDOF;
    settings.lockXTranslation = true;
    settings.lockZRotation = true;
    const PhysicsBodyHandle ball = scene.Sphere(settings);
    scene.scene->SetBodyLinearVelocity(ball, Vec3d(WakeVelocity, WakeVelocity, 0.0), false);
    CHECK(NearVector(scene.Read(ball).linearVelocity, Vec3d(0.0, WakeVelocity, 0.0),
                     VelocityTolerance));
    scene.scene->SetBodyLinearVelocity(ball, Vec3d(0.0, WakeVelocity, WakeVelocity), true);
    CHECK(NearVector(scene.Read(ball).linearVelocity, Vec3d(0.0, 2.0 * WakeVelocity, WakeVelocity),
                     VelocityTolerance));
    scene.scene->SetBodyAngularVelocity(ball, Vec3d(1.0, 0.0, 1.0), false);
    scene.scene->SetBodyAngularVelocity(ball, Vec3d(0.0, 1.0, 0.0), true);
    CHECK(NearVector(scene.Read(ball).angularVelocityRadians, Vec3d(1.0, 1.0, 0.0), SpinTolerance));
}

// NonFiniteBodyInput 경고 수를 센다.
class NonFiniteWarnings final : public RuntimeLogSink {
public:
    Result<void> Write(const LogEvent& event) noexcept override {
        if (StringView(event.message)
                .Contains("(NonFiniteBodyInput)", StringComparison::CaseSensitive)) {
            ++count;
        }

        return Result<void>::Success();
    }

    uint32 count = 0;
};

// 비유한 값과 Jolt float로 넘치는 값은 무시하고 호출마다 Warning이다(속도·대기 몫이 그대로다).
TEST(Physics_ForceIgnoresNonFiniteInput) {
    NonFiniteWarnings warnings;
    SystemAllocator logAllocator;
    LogSystemConfig logConfig;
    logConfig.mode = LogMode::Manual;
    logConfig.globalMinimum = LogLevel::Debug;
    LogSystem logs(logAllocator, logConfig);
    REQUIRE(logs.Start() && logs.AddSink(warnings));
    LogInstallScope installed(logs);
    ForceScene scene;
    REQUIRE(scene.scene);
    const BodySettings settings = Floating(scene.allocator);
    const PhysicsBodyHandle ball = scene.Sphere(settings);
    const double nan = platform::PlatformMath::QuietNaN<double>();
    scene.scene->ApplyBodyForce(ball, Command(BodyForceKind::Impulse, Vec3d(nan, 0.0, 0.0)));
    BodyForce radial =
        Radial(BodyForceKind::RadialImpulse, Vec3d::Zero(), nan, RadialFalloff::Constant);
    scene.scene->ApplyBodyForce(ball, radial);
    scene.scene->SetBodyLinearVelocity(ball, Vec3d(0.0, nan, 0.0), false);
    scene.scene->SetBodyAngularVelocity(ball, Vec3d(0.0, 0.0, nan), false);
    scene.scene->ApplyBodyForce(ball,
                                Command(BodyForceKind::Impulse, Vec3d(FloatOverflow, 0.0, 0.0)));
    scene.scene->ApplyBodyForce(ball,
                                Command(BodyForceKind::Force, Vec3d(0.0, FloatOverflow, 0.0)));
    scene.scene->SetBodyLinearVelocity(ball, Vec3d(FloatOverflow, 0.0, 0.0), false);
    scene.Step();
    (void)logs.Pump();
    CHECK(warnings.count == 7u);
    const BodyKinematics after = scene.Read(ball);
    CHECK(NearVector(after.linearVelocity, Vec3d::Zero(), VelocityTolerance));
    CHECK(NearVector(after.angularVelocityRadians, Vec3d::Zero(), SpinTolerance));
}

// 돈 비대칭 상자: 질량 무관 토크는 관성·회전과 상관없이 Δω = α·dt이고(월드 관성 R·I·Rᵀ — 틀린
// 순서면 다른 축으로 샌다), 각충격량 Δω = R·I⁻¹·Rᵀ·L이다.
TEST(Physics_ForceWorldInertiaOnTiltedBox) {
    ForceScene scene;
    REQUIRE(scene.scene);
    const BodySettings settings = Floating(scene.allocator);
    const Quatd tilt =
        Quatd::FromUnitAxisAngle(Vec3d(1.0, 1.0, 1.0) * (1.0 / math::Sqrt(3.0)), TiltRadians);
    const Vec3d alpha(AngularAcceleration, -0.5 * AngularAcceleration, 0.25 * AngularAcceleration);
    const PhysicsBodyHandle accelerated = scene.Box(settings, tilt);
    scene.scene->ApplyBodyForce(accelerated, Command(BodyForceKind::Torque, alpha, true));
    const Vec3d momentum(TorqueZ, 0.0, 0.0);
    const PhysicsBodyHandle kicked = scene.Box(settings, tilt);
    scene.scene->ApplyBodyForce(kicked, Command(BodyForceKind::AngularImpulse, momentum));
    scene.Step();
    const double step = static_cast<double>(StepSeconds);
    const double relative = 1.0e-3;
    CHECK(NearVector(scene.Read(accelerated).angularVelocityRadians, alpha * step,
                     AngularAcceleration * step * relative));
    const double mass = static_cast<double>(BodyMass);
    const Vec3d inertia(mass * (BoxHalfY * BoxHalfY + BoxHalfZ * BoxHalfZ) / 3.0,
                        mass * (BoxHalfX * BoxHalfX + BoxHalfZ * BoxHalfZ) / 3.0,
                        mass * (BoxHalfX * BoxHalfX + BoxHalfY * BoxHalfY) / 3.0);
    const Vec3d local = tilt.UnrotateVector(momentum);
    const Vec3d expected =
        tilt.RotateVector(Vec3d(local.x / inertia.x, local.y / inertia.y, local.z / inertia.z));
    CHECK(NearVector(scene.Read(kicked).angularVelocityRadians, expected,
                     expected.Length() * relative));
}

// body 공간 위치 힘(로컬 +X 힘, 로컬 +Y 팔): 힘 방향도 팔도 body 회전으로 돈다(선속도는 R·F/m·dt,
// 각속도는 (R·팔) × (R·F) — Z축 둘레 회전이라 −r·F/I·dt).
TEST(Physics_ForceLocalVectorRotates) {
    ForceScene scene;
    REQUIRE(scene.scene);
    const BodySettings settings = Floating(scene.allocator);
    const Quatd yaw = Quatd::FromUnitAxisAngle(Vec3d::UnitZ(), LocalYawRadians);
    const PhysicsBodyHandle ball = scene.Sphere(settings, BodyMotion::Dynamic, yaw);
    BodyForce local = Command(BodyForceKind::ForceAtLocationLocal, Vec3d(TorqueZ, 0.0, 0.0));
    local.location = Vec3d(0.0, SphereRadius, 0.0);
    scene.scene->ApplyBodyForce(ball, local);
    scene.Step();
    const double step = static_cast<double>(StepSeconds);
    const BodyKinematics after = scene.Read(ball);
    const Vec3d expectedVelocity =
        yaw.RotateVector(Vec3d(TorqueZ, 0.0, 0.0)) * (step / static_cast<double>(BodyMass));
    CHECK(NearVector(after.linearVelocity, expectedVelocity, VelocityTolerance));
    CHECK(NearVector(after.angularVelocityRadians,
                     Vec3d(0.0, 0.0, -SphereRadius * TorqueZ / SphereInertia * step),
                     SpinTolerance));
}

// 프레임 힘(everyFixedStep)은 EndFrame까지 step마다 작용하고(두 step이면 두 번), 그 뒤 step에는
// 없다. 거짓이면 다음 step 한 번이다.
TEST(Physics_ForceFrameForceEveryStep) {
    ForceScene scene;
    REQUIRE(scene.scene);
    const BodySettings settings = Floating(scene.allocator);
    const PhysicsBodyHandle framed = scene.Sphere(settings);
    const PhysicsBodyHandle once = scene.Sphere(settings);
    BodyForce lift = Command(BodyForceKind::Force, Vec3d(0.0, 0.0, Gravity), true);
    lift.everyFixedStep = true;
    scene.scene->ApplyBodyForce(framed, lift);
    lift.everyFixedStep = false;
    scene.scene->ApplyBodyForce(once, lift);
    scene.Step();
    scene.Step();
    scene.scene->EndFrame();
    scene.Step();
    const double kick = Gravity * static_cast<double>(StepSeconds);
    CHECK(Near(scene.Read(framed).linearVelocity.z, 2.0 * kick, VelocityTolerance));
    CHECK(Near(scene.Read(once).linearVelocity.z, kick, VelocityTolerance));
}

} // namespace physics_force_scene_tests_file
