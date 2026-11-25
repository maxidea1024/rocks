// 물리 어댑터 수용 시험 PH2a 몫이다(Docs/Design/PhysicsJoltAdapter.ko.md 9절,
// PhysicsModule 7.1.2절): PA01 방향 상수, PA02 각속도·quaternion, PA03 중력, PA04 힘·충격량, PA05
// 토크·관성, PA09 큰 월드(PA03 부분), PA10 캡슐 축, PA13 테이퍼드 캡슐. 기대값은 Rocks
// 좌표·단위(cm·kg·초)로 적고 변환 함수(JoltConversions.h)를 거친 Jolt 결과를 되돌려 비교한다.
// Jolt를 직접 쓰므로 Monolithic 시험(EngineTests)에서만 돈다(BuildTargets 3.8절 BD29 시험 실행 파일
// 예외).
#include "../Private/JoltConversions.h"
#include "../Private/JoltLayers.h"
#include "../Private/JoltPhysicsScene.h"

#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Physics/PhysicsSettings.h>
#include <TestSupport/Test.h>

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/TaperedCapsuleShape.h>
// clang-format on

using namespace rocks;
namespace physics_adapter_tests_file {

namespace {

constexpr double StepSeconds = 1.0 / 60.0;
constexpr int StepsPerSecond = 60;

// 시험 장면 용량(작게).
constexpr uint32 TestMaxBodies = 64;
constexpr uint32 TestMaxBodyPairs = 256;
constexpr uint32 TestMaxContactConstraints = 256;
constexpr uint32 TestTempAllocatorBytes = 2u * 1024u * 1024u;

// UE 기본 중력(cm/s²)이다(어댑터 경계 3절).
constexpr double GravityZ = -980.0;

// 허용 오차. float(Jolt) 한 번 반올림과 적분 누적을 덮는다.
constexpr double VelocityTolerance = 0.05;    // cm/s
constexpr double PositionTolerance = 0.05;    // cm
constexpr double DirectionTolerance = 1.0e-3; // 단위 벡터 성분
constexpr double AngularTolerance = 1.0e-6;   // rad/s
// 쌓여 정지한 형상의 높이 허용 오차: Jolt mPenetrationSlop 0.02 m + 여유(cm).
constexpr double RestHeightTolerance = 3.0;
constexpr double RayDistanceTolerance = 0.05; // cm

constexpr double SphereRadius = 20.0;
constexpr double FloorHalfExtent = 2000.0;
constexpr double FloorHalfThickness = 50.0;

// 큰 월드 시험의 지역 원점(cm — 10⁹ cm = 10,000 km). 소수 cm를 더해 원점을 float에서 빼는 잘못된
// 구현이면 지역 위치가 어긋나게 한다(어댑터 경계 6절 — double에서 먼저 뺀다).
constexpr double FarCoordinate = 1.0e9;
constexpr double FarFraction = 0.37;

// 자유 낙하 시작 높이(cm — m 단위 정수가 아니어서 float 반올림 차이를 드러낸다).
constexpr double FallStartHeight = 1000.37;

// 힘 시험에서 충격량 시험 body와 떨어뜨려 놓는 거리(cm).
constexpr double SeparationX = 500.0;

// 캡슐을 바닥 위로 띄워 놓는 여유(cm).
constexpr double DropMargin = 5.0;

// PA05 box 반폭(cm)과 비대칭 주축 회전(축·각 rad).
constexpr double BoxHalfX = 10.0;
constexpr double BoxHalfY = 20.0;
constexpr double BoxHalfZ = 30.0;
constexpr double TiltedAxisY = 0.6;
constexpr double TiltedAxisZ = 0.8;
constexpr double TiltedAngle = 0.7;

PhysicsSettings TestSettings() noexcept {
    PhysicsSettings settings;
    settings.defaultGravityZ = static_cast<float>(GravityZ);
    settings.maxBodies = TestMaxBodies;
    settings.maxBodyPairs = TestMaxBodyPairs;
    settings.maxContactConstraints = TestMaxContactConstraints;
    settings.tempAllocatorBytes = TestTempAllocatorBytes;
    return settings;
}

const JPH::ObjectLayer StaticLayer = physics::MakeObjectLayer(
    0, physics::LayerMotion::Static,
    physics::LayerParticipationPhysics | physics::LayerParticipationQuery, 0);
const JPH::ObjectLayer MovingLayer = physics::MakeObjectLayer(
    1, physics::LayerMotion::Moving,
    physics::LayerParticipationPhysics | physics::LayerParticipationQuery, 0);

bool Near(double value, double expected, double tolerance) noexcept {
    return math::Abs(value - expected) <= tolerance;
}

bool NearVector(const Vec3d& value, const Vec3d& expected, double tolerance) noexcept {
    return Near(value.x, expected.x, tolerance) && Near(value.y, expected.y, tolerance) &&
           Near(value.z, expected.z, tolerance);
}

// 시험 장면이다. 원점은 지역 원점(cm)이다.
struct AdapterScene {
    explicit AdapterScene(const Vec3d& regionOrigin = Vec3d::Zero()) noexcept
        : origin(regionOrigin) {
        auto created = physics::JoltPhysicsScene::Create(allocator, TestSettings(), nullptr);
        if (created) {
            scene = Move(created.Value());
        }
    }

    JPH::BodyInterface& Bodies() noexcept {
        return scene->System().GetBodyInterface();
    }

    // 동적 body 설정이다(감쇠 0 — 적분 식과 맞춘다).
    JPH::BodyCreationSettings Dynamic(const JPH::Shape* shape, const Vec3d& worldCentimeters,
                                      const Quatd& rotation = Quatd()) const noexcept {
        JPH::BodyCreationSettings settings(shape, physics::ToJoltPosition(worldCentimeters, origin),
                                           physics::ToJoltRotation(rotation),
                                           JPH::EMotionType::Dynamic, MovingLayer);
        settings.mLinearDamping = 0.0f;
        settings.mAngularDamping = 0.0f;
        settings.mAllowSleeping = false;
        return settings;
    }

    JPH::BodyID Add(const JPH::BodyCreationSettings& settings) noexcept {
        return Bodies().CreateAndAddBody(settings, JPH::EActivation::Activate);
    }

    // 윗면이 Rocks z = 0인 정적 바닥이다(원점 기준).
    JPH::BodyID AddFloor() noexcept {
        JPH::BodyCreationSettings floor(
            new JPH::BoxShape(physics::ToJoltHalfExtents(
                Vec3d(FloorHalfExtent, FloorHalfExtent, FloorHalfThickness))),
            physics::ToJoltPosition(origin + Vec3d(0.0, 0.0, -FloorHalfThickness), origin),
            JPH::Quat::sIdentity(), JPH::EMotionType::Static, StaticLayer);
        return Bodies().CreateAndAddBody(floor, JPH::EActivation::DontActivate);
    }

    void Step(int count) noexcept {
        for (int index = 0; index < count; ++index) {
            CHECK(scene->Step(static_cast<float>(StepSeconds)));
        }
    }

    // 질량 중심 위치다(테이퍼드 캡슐처럼 질량 중심이 형상 원점과 다른 형상에는 쓰지 않는다).
    Vec3d Position(JPH::BodyID body) noexcept {
        return physics::FromJoltPosition(Bodies().GetCenterOfMassPosition(body), origin);
    }

    Vec3d Velocity(JPH::BodyID body) noexcept {
        return physics::FromJoltLinearVelocity(Bodies().GetLinearVelocity(body));
    }

    Quatd Rotation(JPH::BodyID body) noexcept {
        return physics::FromJoltRotation(Bodies().GetRotation(body));
    }

    SystemAllocator allocator;
    Vec3d origin;
    UniquePtr<physics::JoltPhysicsScene> scene;
};

// 반암시 Euler(Jolt 적분)의 n 스텝 뒤 변위(cm)다: GravityZ·dt²·n(n + 1)/2(GravityZ는 음수).
double FreeFallDisplacement(int steps) noexcept {
    const double n = static_cast<double>(steps);
    return GravityZ * StepSeconds * StepSeconds * n * (n + 1.0) * 0.5;
}

// 자유 낙하 1초 뒤 변위·속도(cm, cm/s)다.
struct FallResult {
    Vec3d position;
    Vec3d velocity;
};

// PA03을 원점 origin 둘레에서 돌리고 1초 뒤 변위·속도를 돌려준다.
FallResult FallOneSecond(const Vec3d& origin) {
    AdapterScene scene(origin);
    CHECK(scene.scene);
    if (!scene.scene) {
        return {};
    }

    const Vec3d start = origin + Vec3d(0.0, 0.0, FallStartHeight);
    const JPH::BodyID ball =
        scene.Add(scene.Dynamic(new JPH::SphereShape(physics::ToJoltLength(SphereRadius)), start));
    scene.Step(StepsPerSecond);
    return {scene.Position(ball) - start, scene.Velocity(ball)};
}

} // namespace

// PA01: 방향 상수는 A로 정확히 옮겨지고 A⁻¹로 정확히 돌아온다.
TEST(PhysicsAdapter_DirectionConstantsMapExactly) {
    struct Case {
        Vec3d rocks;
        JPH::Vec3 jolt;
    };

    const Case cases[] = {
        {Vec3d::Forward(), JPH::Vec3(0.0f, 0.0f, 1.0f)},
        {Vec3d::Backward(), JPH::Vec3(0.0f, 0.0f, -1.0f)},
        {Vec3d::Right(), JPH::Vec3(1.0f, 0.0f, 0.0f)},
        {Vec3d::Left(), JPH::Vec3(-1.0f, 0.0f, 0.0f)},
        {Vec3d::Up(), JPH::Vec3(0.0f, 1.0f, 0.0f)},
        {Vec3d::Down(), JPH::Vec3(0.0f, -1.0f, 0.0f)},
    };
    for (const Case& item : cases) {
        CHECK(physics::AxisToJolt(item.rocks) == item.jolt);
        CHECK(physics::AxisFromJolt(item.jolt) == item.rocks);
        CHECK(physics::ToJoltNormal(item.rocks) == item.jolt);
        CHECK(physics::FromJoltNormal(item.jolt) == item.rocks);
    }

    // 크기 변환: 중력 (0, 0, −980) cm/s² → (0, −9.8, 0) m/s², 토크 10⁴ kg·cm²/s² → 1 N·m(+Y).
    CHECK(physics::ToJoltAcceleration(Vec3d(0.0, 0.0, GravityZ)) == JPH::Vec3(0.0f, -9.8f, 0.0f));
    CHECK(physics::ToJoltTorque(Vec3d(0.0, 0.0, 1.0e4)) == JPH::Vec3(0.0f, 1.0f, 0.0f));
    CHECK(physics::ToJoltLength(250.0) == 2.5f);
    CHECK(physics::ToJoltDensity(1.0) == 1000.0f);

    // 물리량별 계수와 성분 재배치(정확히 표현되는 값): 1차 길이 계수 10⁻², 2차(각충격량) 10⁻⁴.
    CHECK(physics::ToJoltLinearVelocity(Vec3d(100.0, 200.0, 300.0)) == JPH::Vec3(2.0f, 3.0f, 1.0f));
    CHECK(physics::ToJoltDisplacement(Vec3d(100.0, 200.0, 300.0)) == JPH::Vec3(2.0f, 3.0f, 1.0f));
    CHECK(physics::ToJoltAngularImpulse(Vec3d(0.0, 0.0, 1.0e4)) == JPH::Vec3(0.0f, 1.0f, 0.0f));
    const JPH::Vec3 joltValue(1.0f, 2.0f, 3.0f);
    const Vec3d rocksValue(300.0, 100.0, 200.0);
    CHECK(physics::FromJoltLinearVelocity(joltValue) == rocksValue);
    CHECK(physics::FromJoltAcceleration(joltValue) == rocksValue);
    CHECK(physics::FromJoltLinearImpulse(joltValue) == rocksValue);
    CHECK(physics::FromJoltDisplacement(joltValue) == rocksValue);
    CHECK(physics::FromJoltAngularVelocity(joltValue) == Vec3d(3.0, 1.0, 2.0));
    CHECK(physics::FromJoltLength(2.5f) == 250.0);
}

// PA02: Rocks +Z 각속도 +ω(yaw +)로 t초 돈 body의 전방은 FromUnitAxisAngle(Up, ω·t)의 전방과
// 같고(+X → +Y 쪽), 되읽은 각속도는 (0, 0, ω) rad/s다.
TEST(PhysicsAdapter_AngularVelocityTurnsLikeRocksYaw) {
    constexpr double Omega = 1.0; // rad/s
    AdapterScene scene;
    REQUIRE(scene.scene);
    JPH::BodyCreationSettings settings = scene.Dynamic(
        new JPH::SphereShape(physics::ToJoltLength(SphereRadius)), Vec3d(0.0, 0.0, 0.0));
    settings.mGravityFactor = 0.0f;
    settings.mAngularVelocity = physics::ToJoltAngularVelocity(Vec3d(0.0, 0.0, Omega));
    const JPH::BodyID ball = scene.Add(settings);
    scene.Step(StepsPerSecond);

    const double seconds = static_cast<double>(StepsPerSecond) * StepSeconds;
    const Vec3d forward = scene.Rotation(ball).GetForwardVector();
    const Vec3d expected =
        Quatd::FromUnitAxisAngle(Vec3d::Up(), Omega * seconds).GetForwardVector();
    CHECK(NearVector(forward, expected, DirectionTolerance));
    CHECK(forward.y > 0.0);
    CHECK(NearVector(physics::FromJoltAngularVelocity(scene.Bodies().GetAngularVelocity(ball)),
                     Vec3d(0.0, 0.0, Omega), AngularTolerance));
}

// PA03: 기본 중력에서 1초 자유 낙하 — 속도 (0, 0, −980) cm/s, 변위는 반암시 Euler 식(연속해 −490
// cm와는 g·dt·t/2만큼 다르다). PA09: 원점에서 10⁹ cm 떨어진 지역 원점으로 돌려도 같은 결과다.
TEST(PhysicsAdapter_GravityFallsInRocksUnitsNearAndFar) {
    const FallResult near = FallOneSecond(Vec3d::Zero());
    CHECK(NearVector(near.velocity, Vec3d(0.0, 0.0, GravityZ), VelocityTolerance));
    CHECK(NearVector(near.position, Vec3d(0.0, 0.0, FreeFallDisplacement(StepsPerSecond)),
                     PositionTolerance));

    const FallResult far = FallOneSecond(Vec3d(
        FarCoordinate + FarFraction, -FarCoordinate + FarFraction, FarCoordinate + FarFraction));
    CHECK(far.velocity == near.velocity);
    CHECK(NearVector(far.position, near.position, PositionTolerance));
}

// PA04: 질량 2 kg body에 충격량 (0, 0, 200) kg·cm/s → 속도 변화 (0, 0, 100) cm/s. 힘 (0, 0, 1960)
// kg·cm/s²는 2 kg의 중력을 상쇄한다.
TEST(PhysicsAdapter_ImpulseAndForceUseRocksUnits) {
    constexpr float Mass = 2.0f;
    AdapterScene scene;
    REQUIRE(scene.scene);
    JPH::BodyCreationSettings floating = scene.Dynamic(
        new JPH::SphereShape(physics::ToJoltLength(SphereRadius)), Vec3d(0.0, 0.0, 0.0));
    floating.mGravityFactor = 0.0f;
    floating.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    floating.mMassPropertiesOverride.mMass = Mass;
    const JPH::BodyID kicked = scene.Add(floating);
    scene.Bodies().AddImpulse(kicked, physics::ToJoltLinearImpulse(Vec3d(0.0, 0.0, 200.0)));
    CHECK(NearVector(scene.Velocity(kicked), Vec3d(0.0, 0.0, 100.0), VelocityTolerance));

    JPH::BodyCreationSettings held = scene.Dynamic(
        new JPH::SphereShape(physics::ToJoltLength(SphereRadius)), Vec3d(SeparationX, 0.0, 0.0));
    held.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    held.mMassPropertiesOverride.mMass = Mass;
    const JPH::BodyID supported = scene.Add(held);
    for (int step = 0; step < StepsPerSecond; ++step) {
        scene.Bodies().AddForce(supported, physics::ToJoltForce(Vec3d(
                                               0.0, 0.0, -GravityZ * static_cast<double>(Mass))));
        scene.Step(1);
    }

    CHECK(NearVector(scene.Velocity(supported), Vec3d::Zero(), VelocityTolerance));
}

// PA05: 주축 관성 I(kg·cm²) body에 토크 τ(kg·cm²/s²)를 한 스텝 주면 각속도 τ/I·dt다(토크 크기
// 판정 τ = 10⁴ → 1 N·m은 PA01 시험에 있다). 주축을 돌린
// 비대칭 관성(A·I·Aᵀ)에서도 주축 방향 토크는 그 주축 둘레로만 돈다.
TEST(PhysicsAdapter_TorqueAndInertiaUseRocksUnits) {
    constexpr double Torque = 1.0e4; // kg·cm²/s² = 1 N·m
    const Vec3d principal(1.0e4, 2.0e4, 4.0e4);
    struct Case {
        Quatd principalRotation;
        int axis;
    };

    const Case cases[] = {
        {Quatd(), 2},
        {Quatd::FromUnitAxisAngle(Vec3d::Forward(), math::Pi<double> / 4.0), 1},
        {Quatd::FromUnitAxisAngle(Vec3d(0.0, TiltedAxisY, TiltedAxisZ), TiltedAngle), 0},
    };
    for (const Case& item : cases) {
        AdapterScene scene;
        REQUIRE(scene.scene);
        JPH::BodyCreationSettings settings = scene.Dynamic(
            new JPH::BoxShape(physics::ToJoltHalfExtents(Vec3d(BoxHalfX, BoxHalfY, BoxHalfZ))),
            Vec3d::Zero());
        settings.mGravityFactor = 0.0f;
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
        settings.mMassPropertiesOverride.mMass = 1.0f;
        settings.mMassPropertiesOverride.mInertia =
            physics::ToJoltInertia(principal, item.principalRotation);
        const JPH::BodyID body = scene.Add(settings);

        const Vec3d localAxis = item.axis == 0   ? Vec3d::Forward()
                                : item.axis == 1 ? Vec3d::Right()
                                                 : Vec3d::Up();
        const Vec3d worldAxis = item.principalRotation.RotateVector(localAxis);
        const double inertia = item.axis == 0   ? principal.x
                               : item.axis == 1 ? principal.y
                                                : principal.z;
        scene.Bodies().AddTorque(body, physics::ToJoltTorque(worldAxis * Torque));
        scene.Step(1);
        const Vec3d omega =
            physics::FromJoltAngularVelocity(scene.Bodies().GetAngularVelocity(body));
        CHECK(NearVector(omega, worldAxis * (Torque / inertia * StepSeconds), AngularTolerance));
    }
}

// PA10: 로컬 +Z 캡슐을 세운 body는 평면 위에 서서 쓰러지지 않고 중심 높이가 원통부 반높이 +
// 반지름(cm)이다.
TEST(PhysicsAdapter_CapsuleStandsOnRocksZAxis) {
    constexpr double Radius = 30.0;
    constexpr double HalfCylinder = 60.0;
    constexpr int SettleSteps = 180;
    AdapterScene scene;
    REQUIRE(scene.scene);
    scene.AddFloor();
    const JPH::BodyID capsule = scene.Add(scene.Dynamic(
        new JPH::CapsuleShape(physics::ToJoltLength(HalfCylinder), physics::ToJoltLength(Radius)),
        Vec3d(0.0, 0.0, HalfCylinder + Radius + DropMargin)));
    scene.Step(SettleSteps);
    const Vec3d center = scene.Position(capsule);
    CHECK(Near(center.z, HalfCylinder + Radius, RestHeightTolerance));
    CHECK(scene.Rotation(capsule).GetUpVector().z > 1.0 - DirectionTolerance);

    // 반경은 중심 높이에서 옆으로 쏜 ray로 따로 고정한다(반높이·반경 인자가 바뀌어도 위 높이
    // 판정은 같다).
    constexpr double SideRayLength = 1000.0;
    const JPH::RRayCast side{
        physics::ToJoltPosition(center - Vec3d(SideRayLength * 0.5, 0.0, 0.0), scene.origin),
        physics::ToJoltDisplacement(Vec3d(SideRayLength, 0.0, 0.0))};
    JPH::RayCastResult hit;
    REQUIRE(scene.scene->System().GetNarrowPhaseQuery().CastRay(side, hit));
    CHECK(Near(static_cast<double>(hit.mFraction) * SideRayLength, SideRayLength * 0.5 - Radius,
               RayDistanceTolerance));
}

// PA13: radius0(+Z 끝) > radius1(−Z 끝)인 테이퍼드 캡슐을 +Z로 세운 body에 위·아래에서 축을 따라
// 쏜 ray가 각각 반높이 + radius0, 반높이 + radius1 거리의 끝에 맞는다.
TEST(PhysicsAdapter_TaperedCapsuleRadiusZeroIsTopEnd) {
    constexpr double Radius0 = 40.0;
    constexpr double Radius1 = 20.0;
    constexpr double HalfHeight = 50.0;
    constexpr double RayStart = 500.0;
    constexpr double RayLength = 1000.0;
    AdapterScene scene;
    REQUIRE(scene.scene);
    JPH::TaperedCapsuleShapeSettings shape(physics::ToJoltLength(HalfHeight),
                                           physics::ToJoltLength(Radius0),
                                           physics::ToJoltLength(Radius1));
    auto created = shape.Create();
    REQUIRE(created.IsValid());
    JPH::BodyCreationSettings settings(
        created.Get(), physics::ToJoltPosition(Vec3d::Zero(), scene.origin), JPH::Quat::sIdentity(),
        JPH::EMotionType::Static, StaticLayer);
    scene.Bodies().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
    scene.scene->System().OptimizeBroadPhase();

    struct Ray {
        double startZ;
        double directionZ;
        double expectedDistance;
    };

    const Ray rays[] = {
        {RayStart, -1.0, RayStart - (HalfHeight + Radius0)},
        {-RayStart, 1.0, RayStart - (HalfHeight + Radius1)},
    };
    for (const Ray& ray : rays) {
        const JPH::RRayCast cast{
            physics::ToJoltPosition(Vec3d(0.0, 0.0, ray.startZ), scene.origin),
            physics::ToJoltDisplacement(Vec3d(0.0, 0.0, ray.directionZ * RayLength))};
        JPH::RayCastResult hit;
        REQUIRE(scene.scene->System().GetNarrowPhaseQuery().CastRay(cast, hit));
        CHECK(Near(static_cast<double>(hit.mFraction) * RayLength, ray.expectedDistance,
                   RayDistanceTolerance));
    }
}

} // namespace physics_adapter_tests_file
