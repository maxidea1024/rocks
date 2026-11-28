// Physics 모듈 PH6a 장면 시험이다(Docs/Design/PhysicsModule 7.1.21절): 여러 형상 body의 질량·질량
// 중심(contributeToMass·UE 대체값), 원소별 UE scale 굽기·거울 scale, 원소 충돌 켬(질의·시뮬레이션·
// probe), hit 원소 번호, 관성 주값(UE 고유값 식), 컴포넌트 sweep 원소별 병합, PA13 테이퍼드
// 캡슐(원소 경로). Jolt 장면을 직접 만들므로 Monolithic 시험(EngineTests)에서만 돈다(BuildTargets
// 3.8절 BD29 시험 실행 파일 예외).
#include "../Private/JoltBodyBuild.h"
#include "../Private/JoltPhysicsScene.h"

#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <DeterministicMath/DeterministicMath.h>
#include <Physics/PhysicsSettings.h>
#include <TestSupport/Test.h>
#include <World/Physics/BodyCreateDesc.h>
#include <World/Physics/QueryFilter.h>
#include <World/Physics/QueryParams.h>

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyInterface.h>
// clang-format on

using namespace rocks;
namespace physics_compound_scene_tests_file {

namespace {

constexpr float StepSeconds = 1.0f / 60.0f;
constexpr uint32 TestMaxBodies = 64;
constexpr uint32 TestMaxBodyPairs = 256;
constexpr uint32 TestMaxContactConstraints = 256;
constexpr uint32 TestTempAllocatorBytes = 4u * 1024u * 1024u;

constexpr uint64 FakeRegistry = 1;
constexpr uint32 FakeGeneration = 1;

constexpr double CentimetersPerMeter = 100.0;
constexpr double JoltDensity = 1000.0;       // kg/m³(재질 1 g/cm³)
constexpr double MassTolerance = 1.0e-3;     // 상대
constexpr double InertiaTolerance = 2.0e-3;  // 상대(Jolt float 관성)
constexpr double ComTolerance = 1.0e-2;      // cm(Jolt float 위치)
constexpr double DistanceTolerance = 1.0e-2; // cm

// 질량 시험: 반폭 10 상자를 +X 30, 반경 10 구를 −X 30에 둔다.
constexpr double PartHalf = 10.0;
constexpr double PartOffset = 30.0;

// 원소 scale 시험이다.
constexpr double RayStart = 500.0;
constexpr double ScaledCenterX = 10.0;
const Vec3d ScaledBoxHalf(10.0, 20.0, 30.0);
const Vec3d NonUniformScale(2.0, 1.0, 1.0);
constexpr double MirrorCenterX = 50.0;
constexpr double MirrorSphereRadius = 10.0;

// 원소 충돌 켬 시험: 바닥 원소 셋(X −200·0·+200, 반폭 80·80·10)과 그 위에서 떨어지는 구다.
constexpr double FloorPartSpacing = 200.0;
constexpr double FloorPartHalf = 80.0;
constexpr double FloorPartHalfThickness = 10.0;
constexpr double DropRadius = 10.0;
constexpr double DropHeight = 60.0;
constexpr int FallSteps = 90;
// 원소 위에 남은 구 중심의 하한·떨어진 구 중심의 상한(cm)이다.
constexpr double RestingMinZ = 0.0;
constexpr double FallenMaxZ = -50.0;

// 관성 주값 시험: 반경 10 구 둘을 (±d, ±d, 0)에 둔다.
constexpr double DiagonalOffset = 40.0;
constexpr double DiagonalSphereRadius = 10.0;

// 원소별 sweep 시험이다.
constexpr double SweepPartRadius = 10.0;
constexpr double SweepUpperZ = 100.0;
constexpr double TargetHalf = 20.0;
constexpr double NearTargetX = 200.0;
constexpr double FarTargetX = 300.0;
constexpr double SweepLength = 500.0;

// 회전한 원소의 거울 scale 시험: 반폭 (50, 5, 5) 상자를 Z로 30° 돌린다. 축 방향 40 cm 지점에서
// 위아래로 쏜다.
constexpr double ThinBoxLength = 50.0;
constexpr double ThinBoxHalf = 5.0;
constexpr double ThinBoxDegrees = 30.0;
constexpr double ThinBoxProbe = 40.0;

// 원소 하나짜리 대체 질량 중심 시험의 원소 중심(cm)이다.
constexpr double LoneCenterX = 100.0;

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

double Cube(double value) noexcept {
    return value * value * value;
}

// 반경 r(cm) 구·반폭 h(cm) 정육면체의 균일 밀도 원시 질량(kg)이다.
double RawSphereMass(double radius) noexcept {
    return 4.0 / 3.0 * math::Pi<double> * Cube(radius / CentimetersPerMeter) * JoltDensity;
}

double RawCubeMass(double half) noexcept {
    return Cube(2.0 * half / CentimetersPerMeter) * JoltDensity;
}

// UE 질량 규칙이다(재질 RaiseMassToPower, 하한 1 g).
double UnrealMass(double raw) noexcept {
    return math::Max(deterministic::Pow(raw, physics::DefaultPhysicalMaterialRaiseMassToPower),
                     physics::MinBodyMassKilograms);
}

Quatd AboutZ(double radians) noexcept {
    return Quatd::FromUnitAxisAngle(Vec3d(0.0, 0.0, 1.0), radians);
}

struct CompoundScene {
    CompoundScene() noexcept {
        auto created = physics::JoltPhysicsScene::Create(allocator, TestSettings(), nullptr);
        if (created) {
            scene = Move(created.Value());
            scene->SetSimulates(true);
        }
    }

    PhysicsBodyHandle Add(Span<const BodyShapeElement> elements, const Transform& at,
                          BodyMotion motion, bool gravity = false, bool notify = false) noexcept {
        BodyCreateDesc desc;
        desc.component = ObjectHandle{FakeRegistry, ++tags, FakeGeneration};
        desc.actor = desc.component;
        desc.geometry = elements;
        desc.worldTransform = at;
        desc.motion = motion;
        desc.collision.collisionEnabled = CollisionEnabled::QueryAndPhysics;
        settings.enableGravity = gravity;
        settings.notifyRigidBodyCollision = notify;
        desc.settings = &settings;
        auto created = scene->CreateBody(desc);
        CHECK(created);
        return created ? created.Value() : PhysicsBodyHandle{};
    }

    PhysicsBodyHandle AddAt(Span<const BodyShapeElement> elements, const Vec3d& at,
                            BodyMotion motion, bool gravity = false, bool notify = false) noexcept {
        Transform pose;
        pose.translation = at;
        return Add(elements, pose, motion, gravity, notify);
    }

    // body의 컴포넌트 표지다(Add가 칸 번호 + 1로 준다 — 이 시험들은 칸을 재사용하지 않는다).
    static ObjectHandle Component(PhysicsBodyHandle body) noexcept {
        return ObjectHandle{FakeRegistry, body.index + 1, FakeGeneration};
    }

    // body의 Jolt manifold reduction 표지다.
    bool UsesManifoldReduction(PhysicsBodyHandle handle) noexcept {
        JPH::BodyIDVector ids;
        scene->System().GetBodies(ids);
        const JPH::BodyInterface& bodies = scene->System().GetBodyInterface();
        for (const JPH::BodyID& id : ids) {
            if (bodies.GetUserData(id) == physics::PackUserData(handle.index, handle.generation)) {
                return bodies.GetUseManifoldReduction(id);
            }
        }

        return true;
    }

    BodyKinematics Read(PhysicsBodyHandle body) noexcept {
        auto read = scene->ReadKinematics(body);
        CHECK(read);
        return read ? read.Value() : BodyKinematics{};
    }

    // 위에서 아래로 쏜 line trace의 첫 hit다(WorldStatic만).
    bool TraceDown(const Vec3d& at, HitResult& hit) noexcept {
        const QueryFilter filter =
            QueryFilter::ByObjectTypes(CollisionObjectTypeBit(CollisionChannel::WorldStatic));
        auto traced =
            scene->TraceLine(at + Vec3d(0.0, 0.0, RayStart), at - Vec3d(0.0, 0.0, RayStart), filter,
                             {}, TraceMode::First, &hit, nullptr);
        return traced && traced.Value();
    }

    // x축 위 start에서 dx 방향으로 쏜 line trace의 hit 거리다(없으면 −1).
    double TraceAlongX(double startX, double direction) noexcept {
        const QueryFilter filter =
            QueryFilter::ByObjectTypes(CollisionObjectTypeBit(CollisionChannel::WorldStatic));
        HitResult hit;
        const Vec3d start(startX, 0.0, 0.0);
        auto traced = scene->TraceLine(start, start + Vec3d(direction * 2.0 * RayStart, 0.0, 0.0),
                                       filter, {}, TraceMode::First, &hit, nullptr);
        return traced && traced.Value() ? hit.distance : -1.0;
    }

    void Steps(int count) noexcept {
        for (int step = 0; step < count; ++step) {
            CHECK(scene->Step(StepSeconds));
        }
    }

    SystemAllocator allocator;
    UniquePtr<physics::JoltPhysicsScene> scene;
    BodySettings settings{allocator};
    uint32 tags = 0;
};

BodyShapeElement Cube(double half, const Vec3d& center) noexcept {
    BodyShapeElement element = BodyShapeElement::Box(Vec3d(half, half, half));
    element.center = center;
    return element;
}

BodyShapeElement Ball(double radius, const Vec3d& center) noexcept {
    BodyShapeElement element = BodyShapeElement::Sphere(radius);
    element.center = center;
    return element;
}

} // namespace

// 원소 둘(상자 + 구)의 질량은 두 원시 질량 합의 UE 규칙, 질량 중심은 질량 가중이다. 질량에 넣지
// 않는 원소는 빠진다.
TEST(Physics_CompoundMassAndCenter) {
    CompoundScene scene;
    REQUIRE(scene.scene);
    const double boxMass = RawCubeMass(PartHalf);
    const double sphereMass = RawSphereMass(PartHalf);
    const BodyShapeElement both[] = {Cube(PartHalf, Vec3d(PartOffset, 0.0, 0.0)),
                                     Ball(PartHalf, Vec3d(-PartOffset, 0.0, 0.0))};
    const PhysicsBodyHandle body = scene.AddAt(both, Vec3d::Zero(), BodyMotion::Dynamic);
    const BodyKinematics read = scene.Read(body);
    CHECK(NearRelative(read.mass, UnrealMass(boxMass + sphereMass), MassTolerance));
    const double expectedX = PartOffset * (boxMass - sphereMass) / (boxMass + sphereMass);
    CHECK(Near(read.centerOfMass.x, expectedX, ComTolerance));
    CHECK(Near(read.centerOfMass.y, 0.0, ComTolerance));

    BodyShapeElement massless = Ball(PartHalf, Vec3d(-PartOffset, 0.0, 0.0));
    massless.contributeToMass = false;
    const BodyShapeElement boxOnly[] = {Cube(PartHalf, Vec3d(PartOffset, 0.0, 0.0)), massless};
    const PhysicsBodyHandle partial =
        scene.AddAt(boxOnly, Vec3d(0.0, 0.0, RayStart), BodyMotion::Dynamic);
    const BodyKinematics partialRead = scene.Read(partial);
    CHECK(NearRelative(partialRead.mass, UnrealMass(boxMass), MassTolerance));
    CHECK(Near(partialRead.centerOfMass.x, PartOffset, ComTolerance));
}

// 모든 원소가 질량에서 빠지면 UE 대체값(523 kg·5.24e5 kg·cm², 질량 중심 0)에 수정자를 그대로
// 적용한다(MassProperties.cpp:474-483).
TEST(Physics_CompoundMasslessFallback) {
    CompoundScene scene;
    REQUIRE(scene.scene);
    BodyShapeElement a = Cube(PartHalf, Vec3d(PartOffset, 0.0, 0.0));
    BodyShapeElement b = Ball(PartHalf, Vec3d(-PartOffset * 2.0, 0.0, 0.0));
    a.contributeToMass = false;
    b.contributeToMass = false;
    const BodyShapeElement none[] = {a, b};
    const PhysicsBodyHandle body = scene.AddAt(none, Vec3d::Zero(), BodyMotion::Dynamic);
    const BodyKinematics read = scene.Read(body);
    const double mass = UnrealMass(physics::FallbackBodyMassKilograms);
    CHECK(NearRelative(read.mass, mass, MassTolerance));
    const double inertia = physics::FallbackBodyInertiaKilogramSquareCentimeters * mass /
                           physics::FallbackBodyMassKilograms;
    CHECK(NearRelative(read.inertia.x, inertia, InertiaTolerance));
    CHECK(NearRelative(read.inertia.y, inertia, InertiaTolerance));
    CHECK(NearRelative(read.inertia.z, inertia, InertiaTolerance));
    CHECK(Near(read.centerOfMass.x, 0.0, ComTolerance));
}

// 회전한 상자 원소의 비균일 scale은 원소 축 반폭에 곱한다(UE FKBoxElem::GetFinalScaled — 원소 Y
// 반폭 20 × |Sy| 1이 월드 X 반폭이다), 중심은 × scale이다. 거울 scale은 중심을 뒤집는다.
TEST(Physics_CompoundElementScale) {
    CompoundScene scene;
    REQUIRE(scene.scene);
    BodyShapeElement box = BodyShapeElement::Box(ScaledBoxHalf);
    box.center = Vec3d(ScaledCenterX, 0.0, 0.0);
    box.rotation = AboutZ(math::HalfPi<double>);
    const BodyShapeElement boxes[] = {box};
    Transform scaled;
    scaled.scale = NonUniformScale;
    (void)scene.Add(boxes, scaled, BodyMotion::Static);
    // 오른쪽 면 = 중심 20 + 반폭(원소 Y 20 × 1) = 40.
    const double faceX = ScaledCenterX * NonUniformScale.x + ScaledBoxHalf.y * NonUniformScale.y;
    CHECK(Near(scene.TraceAlongX(RayStart, -1.0), RayStart - faceX, DistanceTolerance));

    CompoundScene mirrored;
    REQUIRE(mirrored.scene);
    const BodyShapeElement balls[] = {Ball(MirrorSphereRadius, Vec3d(MirrorCenterX, 0.0, 0.0))};
    Transform mirror;
    mirror.scale = Vec3d(-1.0, 1.0, 1.0);
    (void)mirrored.Add(balls, mirror, BodyMotion::Static);
    // 거울 중심 −50, 왼쪽 면 −60.
    CHECK(Near(mirrored.TraceAlongX(-RayStart, 1.0),
               RayStart - (MirrorCenterX + MirrorSphereRadius), DistanceTolerance));
    CHECK(Near(mirrored.TraceAlongX(RayStart, -1.0), RayStart + MirrorCenterX - MirrorSphereRadius,
               DistanceTolerance));
}

// 원소 충돌 켬(7.1.21절): 물리 전용 원소는 질의 hit가 없고 구를 받친다, 질의 전용 원소는 질의
// hit가 있고 구가 지나간다, probe 원소는 구를 밀지 않는다. hit 원소 번호는 원소 순서다.
TEST(Physics_CompoundElementCollisionEnabled) {
    CompoundScene scene;
    REQUIRE(scene.scene);
    const Vec3d half(FloorPartHalf, FloorPartHalf, FloorPartHalfThickness);
    BodyShapeElement physicsOnly = BodyShapeElement::Box(half);
    physicsOnly.center = Vec3d(-FloorPartSpacing, 0.0, 0.0);
    physicsOnly.collisionEnabled = CollisionEnabled::PhysicsOnly;
    BodyShapeElement probeOnly = BodyShapeElement::Box(half);
    probeOnly.collisionEnabled = CollisionEnabled::ProbeOnly;
    BodyShapeElement queryOnly = BodyShapeElement::Box(half);
    queryOnly.center = Vec3d(FloorPartSpacing, 0.0, 0.0);
    queryOnly.collisionEnabled = CollisionEnabled::QueryOnly;
    const BodyShapeElement floor[] = {physicsOnly, probeOnly, queryOnly};
    (void)scene.AddAt(floor, Vec3d::Zero(), BodyMotion::Static);

    HitResult hit;
    CHECK(!scene.TraceDown(Vec3d(-FloorPartSpacing, 0.0, 0.0), hit));
    CHECK(!scene.TraceDown(Vec3d::Zero(), hit));
    REQUIRE(scene.TraceDown(Vec3d(FloorPartSpacing, 0.0, 0.0), hit));
    CHECK(hit.elementIndex == 2);
    CHECK(Near(hit.impactPoint.z, FloorPartHalfThickness, DistanceTolerance));

    const BodyShapeElement ball[] = {BodyShapeElement::Sphere(DropRadius)};
    const PhysicsBodyHandle rests = scene.AddAt(ball, Vec3d(-FloorPartSpacing, 0.0, DropHeight),
                                                BodyMotion::Dynamic, true, true);
    const PhysicsBodyHandle probed =
        scene.AddAt(ball, Vec3d(0.0, 0.0, DropHeight), BodyMotion::Dynamic, true, true);
    const PhysicsBodyHandle passes = scene.AddAt(ball, Vec3d(FloorPartSpacing, 0.0, DropHeight),
                                                 BodyMotion::Dynamic, true, true);
    scene.Steps(FallSteps);
    CHECK(scene.Read(rests).location.z > RestingMinZ);
    CHECK(scene.Read(probed).location.z < FallenMaxZ);
    CHECK(scene.Read(passes).location.z < FallenMaxZ);
    // probe 원소는 접촉 통지를 낸다(밀지 않음), 질의 전용 원소는 시뮬레이션에서 빠져 통지가 없다.
    int probedHits = 0;
    int passedHits = 0;
    for (const PhysicsContactHit& contact : scene.scene->TakeContactHits()) {
        probedHits += contact.myComponent == CompoundScene::Component(probed) ? 1 : 0;
        passedHits += contact.myComponent == CompoundScene::Component(passes) ? 1 : 0;
    }

    CHECK(probedHits > 0);
    CHECK(passedHits == 0);
}

// 원소 충돌 켬이 섞인 body는 manifold reduction을 끈다(합친 manifold의 하위 형상 ID 하나로 원소별
// probe가 섞이지 않게 — 7.1.21절). 형상을 바꾸면 다시 맞춘다.
TEST(Physics_CompoundManifoldReduction) {
    CompoundScene scene;
    REQUIRE(scene.scene);
    BodyShapeElement probe = Cube(PartHalf, Vec3d(PartOffset, 0.0, 0.0));
    probe.collisionEnabled = CollisionEnabled::ProbeOnly;
    const BodyShapeElement mixed[] = {Cube(PartHalf, Vec3d(-PartOffset, 0.0, 0.0)), probe};
    const BodyShapeElement uniform[] = {Cube(PartHalf, Vec3d(-PartOffset, 0.0, 0.0)),
                                        Cube(PartHalf, Vec3d(PartOffset, 0.0, 0.0))};
    const PhysicsBodyHandle mixedBody = scene.AddAt(mixed, Vec3d::Zero(), BodyMotion::Static);
    const PhysicsBodyHandle uniformBody =
        scene.AddAt(uniform, Vec3d(0.0, 0.0, RayStart), BodyMotion::Static);
    CHECK(!scene.UsesManifoldReduction(mixedBody));
    CHECK(scene.UsesManifoldReduction(uniformBody));
    REQUIRE(scene.scene->SetBodyGeometry(mixedBody, uniform));
    CHECK(scene.UsesManifoldReduction(mixedBody));
}

// 형상 값 변경(SetBodyGeometry)은 compound를 새로 만든다: 구 하나 → 구 + 위 상자, hit 원소 번호 1.
TEST(Physics_CompoundSetBodyGeometry) {
    CompoundScene scene;
    REQUIRE(scene.scene);
    const BodyShapeElement single[] = {Ball(PartHalf, Vec3d::Zero())};
    const PhysicsBodyHandle body = scene.AddAt(single, Vec3d::Zero(), BodyMotion::Static);
    const BodyShapeElement stacked[] = {Ball(PartHalf, Vec3d::Zero()),
                                        Cube(PartHalf, Vec3d(0.0, 0.0, PartOffset))};
    REQUIRE(scene.scene->SetBodyGeometry(body, stacked));
    HitResult hit;
    REQUIRE(scene.TraceDown(Vec3d::Zero(), hit));
    CHECK(hit.elementIndex == 1);
    CHECK(Near(hit.impactPoint.z, PartOffset + PartHalf, DistanceTolerance));
}

// 회전한 원소의 거울 scale은 UE 거울 회전이다(FTransform 음수 scale 경로 — 7.1.21절): +30° 상자가
// X 거울에서 −30° 방향으로 놓인다(원소 회전을 그대로 두면 +30°).
TEST(Physics_CompoundMirroredRotatedElement) {
    CompoundScene scene;
    REQUIRE(scene.scene);
    BodyShapeElement thin = BodyShapeElement::Box(Vec3d(ThinBoxLength, ThinBoxHalf, ThinBoxHalf));
    const double radians = ThinBoxDegrees * math::Pi<double> / 180.0;
    thin.rotation = AboutZ(radians);
    const BodyShapeElement elements[] = {thin};
    Transform mirror;
    mirror.scale = Vec3d(-1.0, 1.0, 1.0);
    (void)scene.Add(elements, mirror, BodyMotion::Static);
    const double c = math::Cos(radians) * ThinBoxProbe;
    const double s = math::Sin(radians) * ThinBoxProbe;
    HitResult hit;
    CHECK(scene.TraceDown(Vec3d(c, -s, 0.0), hit));
    CHECK(!scene.TraceDown(Vec3d(c, s, 0.0), hit));
}

// 질량에 넣는 원소가 없는 원소 하나짜리 body의 질량 중심은 body 원점이다(UE 대체값 — Jolt는 원소
// 질량 중심을 둔다).
TEST(Physics_CompoundMasslessSingleElementCenter) {
    CompoundScene scene;
    REQUIRE(scene.scene);
    BodyShapeElement lone = Ball(PartHalf, Vec3d(LoneCenterX, 0.0, 0.0));
    lone.contributeToMass = false;
    const BodyShapeElement elements[] = {lone};
    const PhysicsBodyHandle body = scene.AddAt(elements, Vec3d::Zero(), BodyMotion::Dynamic);
    const BodyKinematics read = scene.Read(body);
    CHECK(Near(read.centerOfMass.x, 0.0, ComTolerance));
    CHECK(NearRelative(read.mass, UnrealMass(physics::FallbackBodyMassKilograms), MassTolerance));
}

// 관성 주값(UE GetLocalInertiaTensor — Chaos 고유값, 7.1.21절): 대각선 위 두 구의 텐서는 비대각이
// 있다. 고유값은 2I₀ + 4md²(둘)·2I₀이고 UE 식 순서(m00 최대, m11 최소, m22 가운데)로 (큰, 작은,
// 큰)이다 — 축 대각(2I₀ + 2md², 2I₀ + 2md², 2I₀ + 4md²)과 다르다.
TEST(Physics_CompoundPrincipalInertia) {
    CompoundScene scene;
    REQUIRE(scene.scene);
    const BodyShapeElement pair[] = {
        Ball(DiagonalSphereRadius, Vec3d(DiagonalOffset, DiagonalOffset, 0.0)),
        Ball(DiagonalSphereRadius, Vec3d(-DiagonalOffset, -DiagonalOffset, 0.0))};
    const PhysicsBodyHandle body = scene.AddAt(pair, Vec3d::Zero(), BodyMotion::Dynamic);
    const double m = RawSphereMass(DiagonalSphereRadius);
    const double r = DiagonalSphereRadius;
    const double d = DiagonalOffset;
    const double own = 0.4 * m * r * r; // kg·cm²
    const double ratio = UnrealMass(2.0 * m) / (2.0 * m);
    const double large = (2.0 * own + 4.0 * m * d * d) * ratio;
    const double small = 2.0 * own * ratio;
    const BodyKinematics read = scene.Read(body);
    CHECK(NearRelative(read.inertia.x, large, InertiaTolerance));
    CHECK(NearRelative(read.inertia.y, small, InertiaTolerance));
    CHECK(NearRelative(read.inertia.z, large, InertiaTolerance));
}

// 컴포넌트 sweep은 원소마다 따로 쓸고 시간순으로 합친다(UE ComponentSweepMultiByChannel): 위 원소가
// 가까운 표적에, 아래 원소가 먼 표적에 닿는다.
TEST(Physics_CompoundTraceBodyPerElement) {
    CompoundScene scene;
    REQUIRE(scene.scene);
    const BodyShapeElement mover[] = {Ball(SweepPartRadius, Vec3d::Zero()),
                                      Ball(SweepPartRadius, Vec3d(0.0, 0.0, SweepUpperZ))};
    const PhysicsBodyHandle body = scene.AddAt(mover, Vec3d::Zero(), BodyMotion::Kinematic);
    const BodyShapeElement target[] = {Cube(TargetHalf, Vec3d::Zero())};
    const PhysicsBodyHandle nearTarget =
        scene.AddAt(target, Vec3d(NearTargetX, 0.0, SweepUpperZ), BodyMotion::Static);
    const PhysicsBodyHandle farTarget =
        scene.AddAt(target, Vec3d(FarTargetX, 0.0, 0.0), BodyMotion::Static);
    REQUIRE(!nearTarget.IsNull());
    REQUIRE(!farTarget.IsNull());

    Array<HitResult> hits(scene.allocator);
    const QueryFilter filter =
        QueryFilter::ByObjectTypes(CollisionObjectTypeBit(CollisionChannel::WorldStatic));
    auto traced = scene.scene->TraceBody(body, Vec3d::Zero(), Vec3d(SweepLength, 0.0, 0.0),
                                         Quatd::Identity(), filter, {}, hits);
    REQUIRE(traced);
    REQUIRE(hits.Size() == 2u);
    CHECK(hits[0].time < hits[1].time);
    // 결과 위치는 컴포넌트 기준이다(원소 자리는 형상 안 — UE GeomSweepMulti).
    CHECK(Near(hits[0].distance, NearTargetX - TargetHalf - SweepPartRadius, DistanceTolerance));
    CHECK(Near(hits[1].distance, FarTargetX - TargetHalf - SweepPartRadius, DistanceTolerance));
    CHECK(hits[0].component.slot == nearTarget.index + 1u);
    CHECK(hits[1].component.slot == farTarget.index + 1u);
}

// PA13(원소 경로 — 어댑터 경계 9절): radius0(+Z 끝)·radius1(−Z 끝)의 테이퍼드 캡슐 원소는 두 끝이
// 중심 + L/2 + r0, 중심 − L/2 − r1에 있다(선분 중점이 원소 중심). 거울 Z scale은 두
// 반경을 바꾼다. 한 구가 다른 구를 품으면 큰 구 하나다.
TEST(Physics_CompoundTaperedCapsuleElement) {
    constexpr double Radius0 = 40.0;
    constexpr double Radius1 = 20.0;
    constexpr double Length = 100.0;
    constexpr double CenterZ = 30.0;
    const double halfHeight = (Length + Radius0 + Radius1) * 0.5;
    BodyShapeElement capsule = BodyShapeElement::TaperedCapsule(Radius0, Radius1, halfHeight);
    capsule.center = Vec3d(0.0, 0.0, CenterZ);
    const BodyShapeElement elements[] = {capsule};

    CompoundScene scene;
    REQUIRE(scene.scene);
    (void)scene.AddAt(elements, Vec3d::Zero(), BodyMotion::Static);
    HitResult hit;
    REQUIRE(scene.TraceDown(Vec3d::Zero(), hit));
    // 위 끝 = 중심 + L/2 + radius0.
    CHECK(Near(hit.impactPoint.z, CenterZ + Length * 0.5 + Radius0, DistanceTolerance));
    const QueryFilter filter =
        QueryFilter::ByObjectTypes(CollisionObjectTypeBit(CollisionChannel::WorldStatic));
    HitResult up;
    auto traced = scene.scene->TraceLine(Vec3d(0.0, 0.0, -RayStart), Vec3d(0.0, 0.0, RayStart),
                                         filter, {}, TraceMode::First, &up, nullptr);
    REQUIRE(traced);
    REQUIRE(traced.Value());
    // 아래 끝 = 중심 − L/2 − radius1.
    CHECK(Near(up.impactPoint.z, CenterZ - Length * 0.5 - Radius1, DistanceTolerance));

    CompoundScene mirrored;
    REQUIRE(mirrored.scene);
    Transform flip;
    flip.scale = Vec3d(1.0, 1.0, -1.0);
    (void)mirrored.Add(elements, flip, BodyMotion::Static);
    HitResult flipped;
    REQUIRE(mirrored.TraceDown(Vec3d::Zero(), flipped));
    // 거울: 중심 −30, 위 끝은 원래 −Z 끝(radius1)이다.
    CHECK(Near(flipped.impactPoint.z, -CenterZ + Length * 0.5 + Radius1, DistanceTolerance));

    // 같은 반경은 캡슐이다(Jolt Create와 같은 판정): 위 끝 = L/2 + r.
    CompoundScene equal;
    REQUIRE(equal.scene);
    const BodyShapeElement even[] = {
        BodyShapeElement::TaperedCapsule(Radius1, Radius1, (Length + 2.0 * Radius1) * 0.5)};
    (void)equal.AddAt(even, Vec3d::Zero(), BodyMotion::Static);
    HitResult evenTop;
    REQUIRE(equal.TraceDown(Vec3d::Zero(), evenTop));
    CHECK(Near(evenTop.impactPoint.z, Length * 0.5 + Radius1, DistanceTolerance));

    CompoundScene embedded;
    REQUIRE(embedded.scene);
    constexpr double Big = 60.0;
    constexpr double Tiny = 5.0;
    constexpr double ShortLength = 10.0;
    const BodyShapeElement swallowed[] = {
        BodyShapeElement::TaperedCapsule(Big, Tiny, (ShortLength + Big + Tiny) * 0.5)};
    (void)embedded.AddAt(swallowed, Vec3d::Zero(), BodyMotion::Static);
    HitResult top;
    REQUIRE(embedded.TraceDown(Vec3d::Zero(), top));
    // radius0 > L + radius1이라 UE가 radius0을 반높이 h로 자르고(GetScaledRadii) 선분 L' = h −
    // Tiny가 되어 큰 구가 작은 구를 품는다: 위 끝 = L'/2 + h.
    const double clampedHalf = (ShortLength + Big + Tiny) * 0.5;
    CHECK(Near(top.impactPoint.z, (clampedHalf - Tiny) * 0.5 + clampedHalf, DistanceTolerance));
}

} // namespace physics_compound_scene_tests_file
