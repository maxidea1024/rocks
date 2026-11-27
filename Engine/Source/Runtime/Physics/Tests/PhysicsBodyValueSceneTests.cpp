// Physics 모듈 PH5a 장면 시험이다(Docs/Design/PhysicsModule 6.5·6.6·6.8절, 7.1.15절): UE 질량
// 규칙(거듭제곱·배율·덮어쓰기·하한, 관성 비율·관성 배율·질량 중심 이동), 형상 변경의 질량 다시
// 계산, 기본 물리 재질·결합 방식·반발 문턱, 상자 margin, 속도 상한·solver·표지, 설정 검증. Jolt
// body 값을 직접 읽으므로 Monolithic 시험(EngineTests)에서만 돈다(BuildTargets 3.8절 BD29 시험 실행
// 파일 예외).
#include "../Private/JoltBodyBuild.h"
#include "../Private/JoltConversions.h"
#include "../Private/JoltPhysicsScene.h"

#include <Base/InitializerList.h>
#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <DeterministicMath/DeterministicMath.h>
#include <Physics/PhysicsSettings.h>
#include <Platform/PlatformMath.h>
#include <TestSupport/Test.h>
#include <World/Physics/BodyCreateDesc.h>

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
// clang-format on

using namespace rocks;
namespace physics_body_value_scene_tests_file {

namespace {

constexpr double StepSeconds = 1.0 / 60.0;
constexpr uint32 TestMaxBodies = 64;
constexpr uint32 TestMaxBodyPairs = 256;
constexpr uint32 TestMaxContactConstraints = 256;
constexpr uint32 TestTempAllocatorBytes = 4u * 1024u * 1024u;

constexpr double SphereRadius = 20.0;
constexpr double CentimetersPerMeter = 100.0;
constexpr double JoltDensity = 1000.0; // kg/m³(재질 1 g/cm³)
constexpr double MassScale = 2.0;
constexpr float MassOverride = 50.0f;
constexpr double MassTolerance = 1.0e-3;    // 상대
constexpr double InertiaTolerance = 1.0e-3; // 상대
const Vec3d InertiaScale(2.0, 1.0, 1.0);
const Vec3d ComOffset(0.0, 0.0, 10.0);  // cm
constexpr double ComTolerance = 1.0e-3; // cm
constexpr double ScaleUp = 2.0;

constexpr double FloorHalfExtent = 500.0;
constexpr double FloorHalfThickness = 50.0;
constexpr double DropHeight = 200.0;
constexpr double LowDropHeight = 5.0;
constexpr int MaxBounceSteps = 120;
constexpr double RestitutionTolerance = 0.06;
// 문턱 아래 충돌의 튀는 속도 상한(cm/s — 위치 보정 몫).
constexpr double NoBounceVelocity = 15.0;
constexpr float MultiplyRestitution = 0.09f; // 0.3 × 0.3

constexpr double SmallBoxHalf = 10.0;
constexpr double LargeBoxHalf = 500.0;
constexpr double LengthTolerance = 1.0e-4; // cm
constexpr float AngularOverrideDegrees = 720.0f;
constexpr uint8 PositionIterationsOverride = 4;
constexpr uint8 VelocityIterationsOverride = 6;
constexpr double AngularTolerance = 1.0e-4;     // rad/s
constexpr double LinearLimitTolerance = 1.0e-3; // 상대

// 반발 문턱 시험: 5 cm 낙하(약 99 cm/s)는 기본 문턱(1000 cm/s² × dt ≈ 16.7 cm/s)보다 빨라 튀고, 큰
// 문턱(20000 cm/s² × dt ≈ 333 cm/s)이면 튀지 않는다.
constexpr float LargeRestitutionThreshold = 20000.0f;
constexpr double ThresholdTolerance = 1.0e-6; // m/s

// 결합 시험의 두 마찰·반발 값이다.
constexpr float LowCoefficient = 0.2f;
constexpr float HighCoefficient = 0.8f;
constexpr float CoefficientTolerance = 1.0e-6f;

// 반폭이 다른 상자(관성 배율 축 시험 — cm)다.
const Vec3d UnevenHalfExtent(10.0, 20.0, 30.0);

// 질량 중심 이동 상자의 sweep 법선 시험이다: 반폭 50 상자의 윗 모서리 둥근 부분(margin 5 cm)을 반경
// 10 구가 +X로 지나며 맞힌다.
constexpr double EdgeBoxHalf = 50.0;
constexpr double EdgeProbeRadius = 10.0;
constexpr double EdgeProbeZ = 55.0;
constexpr double EdgeSweepStart = -200.0;
constexpr double EdgeSweepEnd = 200.0;
constexpr double EdgeBoxSpacing = 1000.0;
constexpr double NormalTolerance = 1.0e-3;

constexpr uint64 FakeRegistry = 1;
constexpr uint32 FakeGeneration = 1;

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

bool NearRelative(double value, double expected, double tolerance) noexcept {
    return math::Abs(value - expected) <= tolerance * math::Abs(expected);
}

// 반경 r(cm) 구의 균일 밀도 원시 질량(kg)이다.
double RawSphereMass(double radiusCentimeters) noexcept {
    const double r = radiusCentimeters / CentimetersPerMeter;
    return 4.0 / 3.0 * math::Pi<double> * r * r * r * JoltDensity;
}

// UE 질량 규칙이다(재질 RaiseMassToPower, 배율, 하한 1 g).
double UnrealMass(double raw, double scale) noexcept {
    return math::Max(scale *
                         deterministic::Pow(raw, physics::DefaultPhysicalMaterialRaiseMassToPower),
                     physics::MinBodyMassKilograms);
}

// Chaos ScaleInertia(bScaleMass 거짓)의 대각이다(Rocks 축).
Vec3d ScaledInertia(const Vec3d& inertia, const Vec3d& factor) noexcept {
    const double half = 0.5 * (inertia.x + inertia.y + inertia.z);
    const double sx = (half - inertia.x) * factor.x * factor.x;
    const double sy = (half - inertia.y) * factor.y * factor.y;
    const double sz = (half - inertia.z) * factor.z * factor.z;
    return Vec3d(sy + sz, sx + sz, sx + sy);
}

struct ValueScene {
    explicit ValueScene(const PhysicsSettings& settings = TestSettings()) noexcept {
        auto created = physics::JoltPhysicsScene::Create(allocator, settings, nullptr);
        if (created) {
            scene = Move(created.Value());
            scene->SetSimulates(true);
        }
    }

    PhysicsBodyHandle Add(BodyGeometry geometry, const Transform& at, BodyMotion motion,
                          const BodySettings& settings) noexcept {
        BodyCreateDesc desc;
        desc.component = ObjectHandle{FakeRegistry, ++tags, FakeGeneration};
        desc.actor = desc.component;
        desc.geometry = geometry;
        desc.worldTransform = at;
        desc.motion = motion;
        desc.collision.collisionEnabled = CollisionEnabled::QueryAndPhysics;
        desc.settings = &settings;
        auto created = scene->CreateBody(desc);
        CHECK(created);
        return created ? created.Value() : PhysicsBodyHandle{};
    }

    PhysicsBodyHandle Sphere(const Vec3d& at, const BodySettings& settings,
                             BodyMotion motion = BodyMotion::Dynamic) noexcept {
        BodyGeometry sphere;
        sphere.kind = BodyGeometryKind::Sphere;
        sphere.radius = SphereRadius;
        Transform pose;
        pose.translation = at;
        return Add(sphere, pose, motion, settings);
    }

    PhysicsBodyHandle Box(double half, const Vec3d& at, const BodySettings& settings,
                          BodyMotion motion = BodyMotion::Static) noexcept {
        BodyGeometry box;
        box.kind = BodyGeometryKind::Box;
        box.halfExtent = Vec3d(half, half, half);
        Transform pose;
        pose.translation = at;
        return Add(box, pose, motion, settings);
    }

    PhysicsBodyHandle Floor(const BodySettings& settings) noexcept {
        BodyGeometry box;
        box.kind = BodyGeometryKind::Box;
        box.halfExtent = Vec3d(FloorHalfExtent, FloorHalfExtent, FloorHalfThickness);
        Transform pose;
        pose.translation = Vec3d(0.0, 0.0, -FloorHalfThickness);
        return Add(box, pose, BodyMotion::Static, settings);
    }

    JPH::BodyID JoltBody(PhysicsBodyHandle handle) noexcept {
        JPH::BodyIDVector ids;
        scene->System().GetBodies(ids);
        const JPH::BodyInterface& bodies = scene->System().GetBodyInterface();
        for (const JPH::BodyID& id : ids) {
            if (bodies.GetUserData(id) == physics::PackUserData(handle.index, handle.generation)) {
                return id;
            }
        }

        return JPH::BodyID();
    }

    // body를 잠가 call(const JPH::Body&)을 부른다.
    template <class Call>
    void Read(PhysicsBodyHandle handle, Call&& call) noexcept {
        JPH::BodyLockRead lock(scene->System().GetBodyLockInterface(), JoltBody(handle));
        REQUIRE(lock.Succeeded());
        call(lock.GetBody());
    }

    double Mass(PhysicsBodyHandle handle) noexcept {
        double mass = 0.0;
        Read(handle, [&](const JPH::Body& body) noexcept {
            mass = 1.0 / static_cast<double>(body.GetMotionProperties()->GetInverseMass());
        });
        return mass;
    }

    // Jolt 축 관성 대각(kg·m²)이다.
    Vec3d InertiaDiagonal(PhysicsBodyHandle handle) noexcept {
        Vec3d diagonal;
        Read(handle, [&](const JPH::Body& body) noexcept {
            const JPH::Vec3 inverse =
                body.GetMotionProperties()->GetLocalSpaceInverseInertia().GetDiagonal3();
            diagonal = Vec3d(1.0 / static_cast<double>(inverse.GetX()),
                             1.0 / static_cast<double>(inverse.GetY()),
                             1.0 / static_cast<double>(inverse.GetZ()));
        });
        return diagonal;
    }

    double VelocityZ(PhysicsBodyHandle handle) noexcept {
        auto read = scene->ReadKinematics(handle);
        return read ? read.Value().linearVelocity.z : 0.0;
    }

    SystemAllocator allocator;
    UniquePtr<physics::JoltPhysicsScene> scene;
    uint32 tags = 0;
};

// 바닥에 떨어뜨린 구의 첫 반발 비율(튀는 속도 / 부딪는 속도)이다.
double BounceRatio(ValueScene& scene, double height) noexcept {
    BodySettings plain(scene.allocator);
    scene.Floor(plain);
    const PhysicsBodyHandle ball = scene.Sphere(Vec3d(0.0, 0.0, SphereRadius + height), plain);
    double impact = 0.0;
    double rebound = 0.0;
    bool bounced = false;
    for (int step = 0; step < MaxBounceSteps; ++step) {
        CHECK(scene.scene->Step(static_cast<float>(StepSeconds)));
        const double velocity = scene.VelocityZ(ball);
        if (!bounced) {
            if (velocity < impact) {
                impact = velocity;
            } else if (impact < 0.0) {
                bounced = true;
                rebound = velocity;
            }
        } else if (velocity > rebound) {
            rebound = velocity;
        } else {
            break;
        }
    }

    return impact < 0.0 ? rebound / -impact : 0.0;
}

} // namespace

// 질량은 UE 규칙이다: 원시 질량^0.75, massScale 배, 덮어쓰기는 그 값, 0 덮어쓰기는 1 g이다. 관성은
// 같은 비율이다(구 2/5·m·r²).
TEST(Physics_BodyMassFollowsUnrealRule) {
    ValueScene scene;
    REQUIRE(scene.scene);
    BodySettings plain(scene.allocator);
    BodySettings scaled(scene.allocator);
    scaled.massScale = static_cast<float>(MassScale);
    BodySettings overridden(scene.allocator);
    overridden.overrideMass = true;
    overridden.massInKgOverride = MassOverride;
    BodySettings zero(scene.allocator);
    zero.overrideMass = true;
    zero.massInKgOverride = 0.0f;

    const double raw = RawSphereMass(SphereRadius);
    const PhysicsBodyHandle a = scene.Sphere(Vec3d::Zero(), plain);
    const PhysicsBodyHandle b = scene.Sphere(Vec3d(200.0, 0.0, 0.0), scaled);
    const PhysicsBodyHandle c = scene.Sphere(Vec3d(400.0, 0.0, 0.0), overridden);
    const PhysicsBodyHandle d = scene.Sphere(Vec3d(600.0, 0.0, 0.0), zero);
    CHECK(NearRelative(scene.Mass(a), UnrealMass(raw, 1.0), MassTolerance));
    CHECK(NearRelative(scene.Mass(b), UnrealMass(raw, MassScale), MassTolerance));
    CHECK(NearRelative(scene.Mass(c), static_cast<double>(MassOverride), MassTolerance));
    CHECK(NearRelative(scene.Mass(d), physics::MinBodyMassKilograms, MassTolerance));

    const double r = SphereRadius / CentimetersPerMeter;
    const double expectedInertia = 0.4 * UnrealMass(raw, 1.0) * r * r;
    const Vec3d inertia = scene.InertiaDiagonal(a);
    CHECK(NearRelative(inertia.x, expectedInertia, InertiaTolerance));
    CHECK(NearRelative(inertia.y, expectedInertia, InertiaTolerance));
    CHECK(NearRelative(inertia.z, expectedInertia, InertiaTolerance));
}

// 관성 배율(Chaos ScaleInertia — 질량 미포함)과 질량 중심 이동(scale 곱, 관성 대각 m·d²)이다.
TEST(Physics_BodyInertiaScaleAndCenterOfMass) {
    ValueScene scene;
    REQUIRE(scene.scene);
    BodySettings scaledInertia(scene.allocator);
    scaledInertia.inertiaTensorScale = InertiaScale;
    BodySettings shifted(scene.allocator);
    shifted.centerOfMassOffset = ComOffset;

    const double mass = UnrealMass(RawSphereMass(SphereRadius), 1.0);
    const double r = SphereRadius / CentimetersPerMeter;
    const double base = 0.4 * mass * r * r;
    // 구(대각 I): s = (1.5I − I) × scale² = 0.5I·scale² → Ixx = sy+sz = I, Iyy = sx+sz =
    // 0.5I(4+1) = 2.5I, Izz = sx+sy = 2.5I (Rocks 축). Jolt 축: Rocks X → Jolt Z, Y → X, Z → Y.
    const PhysicsBodyHandle a = scene.Sphere(Vec3d::Zero(), scaledInertia);
    const Vec3d jolt = scene.InertiaDiagonal(a);
    CHECK(NearRelative(jolt.z, base, InertiaTolerance));       // Rocks Ixx
    CHECK(NearRelative(jolt.x, 2.5 * base, InertiaTolerance)); // Rocks Iyy
    CHECK(NearRelative(jolt.y, 2.5 * base, InertiaTolerance)); // Rocks Izz

    // 질량 중심 이동(Rocks +Z 10 cm × scale 2): body 질량 중심이 그만큼 옮겨지고 Rocks Izz(Jolt
    // Y)에 m·dz²가 더해진다.
    BodyGeometry sphere;
    sphere.kind = BodyGeometryKind::Sphere;
    sphere.radius = SphereRadius;
    Transform pose;
    pose.translation = Vec3d(300.0, 0.0, 0.0);
    pose.scale = Vec3d(ScaleUp, ScaleUp, ScaleUp);
    const PhysicsBodyHandle b = scene.Add(sphere, pose, BodyMotion::Dynamic, shifted);
    const double scaledRaw = RawSphereMass(SphereRadius * ScaleUp);
    const double scaledMass = UnrealMass(scaledRaw, 1.0);
    const double scaledRadius = SphereRadius * ScaleUp / CentimetersPerMeter;
    const double scaledBase = 0.4 * scaledMass * scaledRadius * scaledRadius;
    const double dz = ComOffset.z * ScaleUp / CentimetersPerMeter;
    const Vec3d shiftedInertia = scene.InertiaDiagonal(b);
    CHECK(NearRelative(shiftedInertia.y, scaledBase + scaledMass * dz * dz, InertiaTolerance));
    CHECK(NearRelative(shiftedInertia.x, scaledBase, InertiaTolerance));
    scene.Read(b, [&](const JPH::Body& body) noexcept {
        const Vec3d center =
            physics::FromJoltPosition(body.GetCenterOfMassPosition(), Vec3d::Zero());
        CHECK(Near(center.z, ComOffset.z * ScaleUp, ComTolerance));
        CHECK(Near(center.x, pose.translation.x, ComTolerance));
    });
}

// scale이 바뀌면 updateMassWhenScaleChanges 참은 새 형상으로 UE 규칙을 다시 적용하고, 거짓은 이전
// 질량을 둔다(7.1.5절에서 미룬 시험).
TEST(Physics_BodyMassFollowsScaleChangeFlag) {
    ValueScene scene;
    REQUIRE(scene.scene);
    BodySettings updating(scene.allocator);
    BodySettings keeping(scene.allocator);
    keeping.updateMassWhenScaleChanges = false;
    const PhysicsBodyHandle a = scene.Sphere(Vec3d::Zero(), updating);
    const PhysicsBodyHandle b = scene.Sphere(Vec3d(300.0, 0.0, 0.0), keeping);
    const double before = scene.Mass(a);

    for (const PhysicsBodyHandle body : {a, b}) {
        Transform pose;
        pose.translation = body == a ? Vec3d::Zero() : Vec3d(300.0, 0.0, 0.0);
        pose.scale = Vec3d(ScaleUp, ScaleUp, ScaleUp);
        scene.scene->SetBodyTransform(body, pose, TeleportMode::TeleportPhysics);
    }

    CHECK(NearRelative(scene.Mass(a), UnrealMass(RawSphereMass(SphereRadius * ScaleUp), 1.0),
                       MassTolerance));
    CHECK(NearRelative(scene.Mass(b), before, MassTolerance));
}

// 기본 물리 재질은 마찰 0.7·반발 0.3이고, 반발 결합 Average는 0.3, Multiply는 0.09로 튄다. 반발
// 문턱 아래 충돌은 튀지 않는다.
TEST(Physics_BodyMaterialCombineAndBounceThreshold) {
    {
        ValueScene scene;
        REQUIRE(scene.scene);
        BodySettings plain(scene.allocator);
        const PhysicsBodyHandle ball = scene.Sphere(Vec3d::Zero(), plain);
        scene.Read(ball, [](const JPH::Body& body) noexcept {
            CHECK(body.GetFriction() == physics::DefaultPhysicalMaterialFriction);
            CHECK(body.GetRestitution() == physics::DefaultPhysicalMaterialRestitution);
        });
    }

    {
        ValueScene scene;
        REQUIRE(scene.scene);
        CHECK(Near(BounceRatio(scene, DropHeight), physics::DefaultPhysicalMaterialRestitution,
                   RestitutionTolerance));
    }

    {
        PhysicsSettings settings = TestSettings();
        settings.restitutionCombineMode = CombineMode::Multiply;
        ValueScene scene(settings);
        REQUIRE(scene.scene);
        CHECK(Near(BounceRatio(scene, DropHeight), MultiplyRestitution, RestitutionTolerance));
    }

    // 반발 문턱 = 가속도 문턱 × step dt(UE Chaos): 기본이면 5 cm 낙하도 튀고, 큰 문턱이면 안 튄다.
    {
        ValueScene scene;
        REQUIRE(scene.scene);
        CHECK(Near(BounceRatio(scene, LowDropHeight), physics::DefaultPhysicalMaterialRestitution,
                   RestitutionTolerance));
        const double expected = static_cast<double>(DefaultPhysicsRestitutionThreshold) *
                                StepSeconds / CentimetersPerMeter;
        CHECK(Near(static_cast<double>(
                       scene.scene->System().GetPhysicsSettings().mMinVelocityForRestitution),
                   expected, ThresholdTolerance));
    }

    PhysicsSettings settings = TestSettings();
    settings.restitutionThreshold = LargeRestitutionThreshold;
    ValueScene scene(settings);
    REQUIRE(scene.scene);
    BodySettings plain(scene.allocator);
    scene.Floor(plain);
    const PhysicsBodyHandle ball =
        scene.Sphere(Vec3d(0.0, 0.0, SphereRadius + LowDropHeight), plain);
    double highest = 0.0;
    for (int step = 0; step < MaxBounceSteps; ++step) {
        CHECK(scene.scene->Step(static_cast<float>(StepSeconds)));
        highest = math::Max(highest, scene.VelocityZ(ball));
    }

    CHECK(highest < NoBounceVelocity);
}

// 결합 함수는 값이 다른 두 body에서 방식마다 다르다(Average·Min·Multiply·Max — Jolt 기본
// 기하평균·최대가 아니다).
TEST(Physics_CombineModesUseUnrealRules) {
    struct Case {
        CombineMode mode;
        float expected;
    };

    const Case cases[] = {
        {CombineMode::Average, 0.5f * (LowCoefficient + HighCoefficient)},
        {CombineMode::Min, LowCoefficient},
        {CombineMode::Multiply, LowCoefficient * HighCoefficient},
        {CombineMode::Max, HighCoefficient},
    };
    for (const Case& entry : cases) {
        PhysicsSettings settings = TestSettings();
        settings.frictionCombineMode = entry.mode;
        settings.restitutionCombineMode = entry.mode;
        ValueScene scene(settings);
        REQUIRE(scene.scene);
        BodySettings plain(scene.allocator);
        const PhysicsBodyHandle a = scene.Sphere(Vec3d::Zero(), plain);
        const PhysicsBodyHandle b = scene.Sphere(Vec3d(300.0, 0.0, 0.0), plain);
        JPH::BodyInterface& bodies = scene.scene->System().GetBodyInterface();
        bodies.SetFriction(scene.JoltBody(a), LowCoefficient);
        bodies.SetRestitution(scene.JoltBody(a), LowCoefficient);
        bodies.SetFriction(scene.JoltBody(b), HighCoefficient);
        bodies.SetRestitution(scene.JoltBody(b), HighCoefficient);
        // 두 body를 함께 읽는다: step 밖 단일 스레드라 잠그지 않는 인터페이스다(잠금 둘을 겹치면
        // Jolt 잠금 순서 assert).
        JPH::BodyLockRead first(scene.scene->System().GetBodyLockInterfaceNoLock(),
                                scene.JoltBody(a));
        JPH::BodyLockRead second(scene.scene->System().GetBodyLockInterfaceNoLock(),
                                 scene.JoltBody(b));
        REQUIRE((first.Succeeded() && second.Succeeded()));
        const float friction = scene.scene->System().GetCombineFriction()(
            first.GetBody(), JPH::SubShapeID(), second.GetBody(), JPH::SubShapeID());
        const float restitution = scene.scene->System().GetCombineRestitution()(
            first.GetBody(), JPH::SubShapeID(), second.GetBody(), JPH::SubShapeID());
        CHECK(math::Abs(friction - entry.expected) <= CoefficientTolerance);
        CHECK(math::Abs(restitution - entry.expected) <= CoefficientTolerance);
    }
}

// 반폭이 다른 상자의 관성 배율은 Chaos 식을 Rocks 축에서 적용한다(축 index가 바뀌면 틀린다).
TEST(Physics_BodyInertiaScaleOnUnevenBox) {
    ValueScene scene;
    REQUIRE(scene.scene);
    BodySettings scaled(scene.allocator);
    scaled.inertiaTensorScale = InertiaScale;
    BodyGeometry box;
    box.kind = BodyGeometryKind::Box;
    box.halfExtent = UnevenHalfExtent;
    const PhysicsBodyHandle body = scene.Add(box, Transform(), BodyMotion::Dynamic, scaled);
    const double hx = UnevenHalfExtent.x / CentimetersPerMeter;
    const double hy = UnevenHalfExtent.y / CentimetersPerMeter;
    const double hz = UnevenHalfExtent.z / CentimetersPerMeter;
    const double raw = 8.0 * hx * hy * hz * JoltDensity;
    const double mass = UnrealMass(raw, 1.0);
    // 직육면체 관성 m/3·(b² + c²)(반폭)이다(Rocks 축).
    const Vec3d base(mass / 3.0 * (hy * hy + hz * hz), mass / 3.0 * (hx * hx + hz * hz),
                     mass / 3.0 * (hx * hx + hy * hy));
    const Vec3d expected = ScaledInertia(base, InertiaScale);
    const Vec3d jolt = scene.InertiaDiagonal(body);
    CHECK(NearRelative(jolt.z, expected.x, InertiaTolerance)); // Rocks X = Jolt Z
    CHECK(NearRelative(jolt.x, expected.y, InertiaTolerance)); // Rocks Y = Jolt X
    CHECK(NearRelative(jolt.y, expected.z, InertiaTolerance)); // Rocks Z = Jolt Y
}

// 형상 변경의 질량 다시 계산: Kinematic도 다시 계산하고, 거짓이면 관성도 그대로이며, 질량 중심
// 이동은 새 scale로 다시 감싼다.
TEST(Physics_ShapeChangeKeepsMassRules) {
    ValueScene scene;
    REQUIRE(scene.scene);
    BodySettings updating(scene.allocator);
    BodySettings keeping(scene.allocator);
    keeping.updateMassWhenScaleChanges = false;
    BodySettings shifted(scene.allocator);
    shifted.centerOfMassOffset = ComOffset;
    const PhysicsBodyHandle kinematic =
        scene.Sphere(Vec3d::Zero(), updating, BodyMotion::Kinematic);
    const PhysicsBodyHandle kept = scene.Sphere(Vec3d(300.0, 0.0, 0.0), keeping);
    const PhysicsBodyHandle moved = scene.Sphere(Vec3d(600.0, 0.0, 0.0), shifted);
    const Vec3d keptInertia = scene.InertiaDiagonal(kept);
    for (const PhysicsBodyHandle body : {kinematic, kept, moved}) {
        auto read = scene.scene->ReadKinematics(body);
        REQUIRE(read);
        Transform pose;
        pose.translation = read.Value().location;
        pose.scale = Vec3d(ScaleUp, ScaleUp, ScaleUp);
        scene.scene->SetBodyTransform(body, pose, TeleportMode::TeleportPhysics);
    }

    double kinematicMass = 0.0;
    scene.Read(kinematic, [&](const JPH::Body& body) noexcept {
        kinematicMass =
            1.0 / static_cast<double>(body.GetMotionProperties()->GetInverseMassUnchecked());
    });
    CHECK(NearRelative(kinematicMass, UnrealMass(RawSphereMass(SphereRadius * ScaleUp), 1.0),
                       MassTolerance));
    const Vec3d keptAfter = scene.InertiaDiagonal(kept);
    CHECK(NearRelative(keptAfter.x, keptInertia.x, InertiaTolerance));
    CHECK(NearRelative(keptAfter.y, keptInertia.y, InertiaTolerance));
    scene.Read(moved, [&](const JPH::Body& body) noexcept {
        const Vec3d center =
            physics::FromJoltPosition(body.GetCenterOfMassPosition(), Vec3d::Zero());
        CHECK(Near(center.z, ComOffset.z * ScaleUp, ComTolerance));
    });
}

// 질량 중심을 옮긴 상자도 구 sweep의 모서리 hit 법선을 면 법선으로 고른다(UE FindGeomOpposingNormal
// — 형상이 OffsetCenterOfMassShape로 감싸여 있어도).
TEST(Physics_ShiftedBoxKeepsOpposingFaceNormal) {
    ValueScene scene;
    REQUIRE(scene.scene);
    BodySettings plain(scene.allocator);
    BodySettings shifted(scene.allocator);
    shifted.centerOfMassOffset = ComOffset;
    for (const BodySettings* settings : {&plain, &shifted}) {
        const double offsetY = settings == &plain ? 0.0 : EdgeBoxSpacing;
        BodyGeometry box;
        box.kind = BodyGeometryKind::Box;
        box.halfExtent = Vec3d(EdgeBoxHalf, EdgeBoxHalf, EdgeBoxHalf);
        Transform pose;
        pose.translation = Vec3d(0.0, offsetY, 0.0);
        REQUIRE(!scene.Add(box, pose, BodyMotion::Static, *settings).IsNull());
        HitResult hit;
        auto traced = scene.scene->TraceShape(Vec3d(EdgeSweepStart, offsetY, EdgeProbeZ),
                                              Vec3d(EdgeSweepEnd, offsetY, EdgeProbeZ), Quatd(),
                                              CollisionShape::Sphere(EdgeProbeRadius),
                                              QueryFilter::ByChannel(CollisionChannel::WorldStatic),
                                              {}, TraceMode::First, &hit, nullptr);
        REQUIRE(traced);
        REQUIRE(traced.Value());
        CHECK(Near(hit.impactNormal.x, -1.0, NormalTolerance));
        CHECK(Near(hit.impactNormal.z, 0.0, NormalTolerance));
    }
}

// 상자 convex radius는 UE margin min(2 × 가장 작은 반폭 × 0.05, 10 cm)이다.
TEST(Physics_BoxMarginFollowsChaosRule) {
    ValueScene scene;
    REQUIRE(scene.scene);
    BodySettings plain(scene.allocator);
    const PhysicsBodyHandle small = scene.Box(SmallBoxHalf, Vec3d::Zero(), plain);
    const PhysicsBodyHandle large = scene.Box(LargeBoxHalf, Vec3d(2000.0, 0.0, 0.0), plain);
    auto radius = [&](PhysicsBodyHandle handle) noexcept {
        double value = -1.0;
        scene.Read(handle, [&](const JPH::Body& body) noexcept {
            const auto* box = static_cast<const JPH::BoxShape*>(body.GetShape());
            value = physics::FromJoltLength(box->GetConvexRadius());
        });
        return value;
    };

    CHECK(Near(radius(small),
               2.0 * SmallBoxHalf * static_cast<double>(DefaultPhysicsCollisionMarginFraction),
               LengthTolerance));
    CHECK(Near(radius(large), static_cast<double>(DefaultPhysicsCollisionMarginMax),
               LengthTolerance));
}

// 각속도 상한(설정 3600 도/s, body 덮어쓰기), 선속도 상한(사실상 없음), solver 덮어쓰기,
// gyroscopic· smooth edge 표지가 body에 들어간다.
TEST(Physics_BodyLimitsAndSolverValues) {
    ValueScene scene;
    REQUIRE(scene.scene);
    BodySettings plain(scene.allocator);
    BodySettings custom(scene.allocator);
    custom.overrideMaxAngularVelocity = true;
    custom.maxAngularVelocity = AngularOverrideDegrees;
    custom.overrideSolverIterations = true;
    custom.positionIterations = PositionIterationsOverride;
    custom.velocityIterations = VelocityIterationsOverride;
    custom.gyroscopicTorque = true;
    custom.smoothEdgeCollisions = true;
    const PhysicsBodyHandle a = scene.Sphere(Vec3d::Zero(), plain);
    const PhysicsBodyHandle b = scene.Sphere(Vec3d(300.0, 0.0, 0.0), custom);
    const double degree = math::Pi<double> / 180.0;
    scene.Read(a, [&](const JPH::Body& body) noexcept {
        const JPH::MotionProperties* motion = body.GetMotionProperties();
        CHECK(Near(static_cast<double>(motion->GetMaxAngularVelocity()),
                   DefaultMaxAngularVelocityDegrees * degree, AngularTolerance));
        CHECK(NearRelative(static_cast<double>(motion->GetMaxLinearVelocity()),
                           UnlimitedLinearVelocity / CentimetersPerMeter, LinearLimitTolerance));
        CHECK(motion->GetNumPositionStepsOverride() == 0u);
        CHECK(!body.GetApplyGyroscopicForce());
        CHECK(!body.GetEnhancedInternalEdgeRemoval());
    });
    scene.Read(b, [&](const JPH::Body& body) noexcept {
        const JPH::MotionProperties* motion = body.GetMotionProperties();
        CHECK(Near(static_cast<double>(motion->GetMaxAngularVelocity()),
                   AngularOverrideDegrees * degree, AngularTolerance));
        CHECK(motion->GetNumPositionStepsOverride() == PositionIterationsOverride);
        CHECK(motion->GetNumVelocityStepsOverride() == VelocityIterationsOverride);
        CHECK(body.GetApplyGyroscopicForce());
        CHECK(body.GetEnhancedInternalEdgeRemoval());
    });
}

// 새 설정 칸의 검증이다.
TEST(Physics_SettingsRejectInvalidBodyValues) {
    auto rejected = [](void (*change)(PhysicsSettings&)) noexcept {
        PhysicsSettings settings = TestSettings();
        change(settings);
        return PhysicsSettings::Validate(settings).Error() == ErrorCode::InvalidArgument;
    };

    CHECK(PhysicsSettings::Validate(TestSettings()));
    CHECK(rejected([](PhysicsSettings& s) { s.maxAngularVelocity = 0.0f; }));
    CHECK(rejected([](PhysicsSettings& s) { s.maxLinearVelocity = -1.0f; }));
    CHECK(rejected([](PhysicsSettings& s) { s.restitutionThreshold = -1.0f; }));
    CHECK(rejected([](PhysicsSettings& s) { s.collisionMarginFraction = -0.1f; }));
    CHECK(rejected([](PhysicsSettings& s) {
        s.collisionMarginMax = platform::PlatformMath::Infinity<float>();
    }));
    CHECK(
        rejected([](PhysicsSettings& s) { s.frictionCombineMode = static_cast<CombineMode>(9); }));
}

} // namespace physics_body_value_scene_tests_file
