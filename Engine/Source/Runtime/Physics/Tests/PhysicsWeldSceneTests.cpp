// Physics 모듈 PH6b1 장면 시험이다(Docs/Design/PhysicsModule 7.1.22절): WeldBody·UnweldBody의
// 형상·질량·질량 중심, 루트 scale ⊙ relative scale 굽기, hit 컴포넌트·원소 번호, 부분별 무시 목록,
// 부분별 overlap·(칸, 부분) 합치기, 컴포넌트 sweep의 부분 Actor 제외, 접촉 hit 컴포넌트, 같은 자식
// 다시 weld, 실패 원자성. Jolt 장면을 직접 만들므로 Monolithic 시험(EngineTests)에서만 돈다
// (BuildTargets 3.8절 BD29 시험 실행 파일 예외).
#include "../Private/JoltBodyBuild.h"
#include "../Private/JoltPhysicsScene.h"

#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <DeterministicMath/DeterministicMath.h>
#include <Physics/PhysicsSettings.h>
#include <Platform/PlatformMath.h>
#include <TestSupport/Test.h>
#include <World/Physics/BodyCreateDesc.h>
#include <World/Physics/BodyWeldDesc.h>
#include <World/Physics/QueryFilter.h>
#include <World/Physics/QueryParams.h>

using namespace rocks;
namespace physics_weld_scene_tests_file {

namespace {

constexpr float StepSeconds = 1.0f / 60.0f;
constexpr uint32 TestMaxBodies = 64;
constexpr uint32 TestMaxBodyPairs = 256;
constexpr uint32 TestMaxContactConstraints = 256;
constexpr uint32 TestTempAllocatorBytes = 4u * 1024u * 1024u;

constexpr uint64 FakeRegistry = 1;
constexpr uint32 FakeGeneration = 1;
// 컴포넌트·Actor 표지(slot 값)다 — body 칸과 겹치지 않게 큰 값을 쓴다.
constexpr uint32 RootComponentSlot = 100;
constexpr uint32 ChildComponentSlot = 101;
constexpr uint32 OtherComponentSlot = 102;
constexpr uint32 RootActorSlot = 200;
constexpr uint32 ChildActorSlot = 201;
constexpr uint32 OtherActorSlot = 202;

constexpr double CentimetersPerMeter = 100.0;
constexpr double JoltDensity = 1000.0;       // kg/m³(재질 1 g/cm³)
constexpr double MassTolerance = 1.0e-3;     // 상대
constexpr double ComTolerance = 1.0e-2;      // cm
constexpr double DistanceTolerance = 1.0e-2; // cm

constexpr double RootRadius = 10.0;
constexpr double ChildHalf = 10.0;
constexpr double ChildOffset = 50.0;
constexpr double MovedChildOffset = 80.0;
constexpr double RayStart = 500.0;

// scale 굽기 시험: 루트 scale 2, 자식 relative (0, 0, 25)·scale (1, 1, 0.5), 반폭 5.
constexpr double RootScale = 2.0;
constexpr double ScaledChildHalf = 5.0;
constexpr double ScaledChildOffset = 25.0;
constexpr double ScaledChildZScale = 0.5;

// 접촉 시험: 자식 상자가 루트 구 아래에 있고 바닥(윗면 z = 0)에 떨어진다.
constexpr double FloorHalf = 500.0;
constexpr double FloorHalfThickness = 50.0;
constexpr double DropHeight = 100.0;
constexpr int FallSteps = 90;

// 세 부분 시험의 두 번째 자식 높이(cm)다.
constexpr double SecondChildOffset = 120.0;
constexpr uint32 SecondChildComponentSlot = 103;

// 자식 질량 중심 이동(cm)이다.
constexpr double ChildNudge = 10.0;

// relative 회전 시험: 긴 상자 반폭 (20, 5, 5)를 Z로 90° 돌린다(긴 축이 루트 Y).
constexpr double LongHalf = 20.0;
constexpr double ShortHalf = 5.0;
constexpr double LongProbe = 15.0;

// 접촉 시험: 자식을 루트 옆에 둔다(둘 다 바닥에 닿는다).
constexpr double SideOffset = 30.0;
constexpr int SettleSteps = 120;

// sweep 시험의 표적 반폭·거리다.
constexpr double TargetHalf = 20.0;
constexpr double NearTargetX = 200.0;
constexpr double FarTargetX = 400.0;
constexpr double SweepLength = 600.0;

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

double RawSphereMass(double radius) noexcept {
    return 4.0 / 3.0 * math::Pi<double> * Cube(radius / CentimetersPerMeter) * JoltDensity;
}

double RawCubeMass(double half) noexcept {
    return Cube(2.0 * half / CentimetersPerMeter) * JoltDensity;
}

double UnrealMass(double raw) noexcept {
    return math::Max(deterministic::Pow(raw, physics::DefaultPhysicalMaterialRaiseMassToPower),
                     physics::MinBodyMassKilograms);
}

ObjectHandle Handle(uint32 slot) noexcept {
    return ObjectHandle{FakeRegistry, slot, FakeGeneration};
}

QueryFilter StaticObjects() noexcept {
    return QueryFilter::ByObjectTypes(CollisionObjectTypeBit(CollisionChannel::WorldStatic));
}

struct WeldScene {
    WeldScene() noexcept {
        auto created = physics::JoltPhysicsScene::Create(allocator, TestSettings(), nullptr);
        if (created) {
            scene = Move(created.Value());
            scene->SetSimulates(true);
        }
    }

    PhysicsBodyHandle Add(const BodyShapeElement& element, ObjectHandle component,
                          ObjectHandle actor, const Transform& at, BodyMotion motion,
                          bool gravity = false, bool notify = false) noexcept {
        BodyCreateDesc desc;
        desc.component = component;
        desc.actor = actor;
        desc.geometry = Span<const BodyShapeElement>(&element, 1);
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

    PhysicsBodyHandle AddAt(const BodyShapeElement& element, ObjectHandle component,
                            ObjectHandle actor, const Vec3d& at, BodyMotion motion,
                            bool gravity = false, bool notify = false) noexcept {
        Transform pose;
        pose.translation = at;
        return Add(element, component, actor, pose, motion, gravity, notify);
    }

    // 루트(반경 RootRadius 구)를 원점에 만든다.
    PhysicsBodyHandle Root(BodyMotion motion = BodyMotion::Dynamic, bool gravity = false,
                           bool notify = false) noexcept {
        return AddAt(BodyShapeElement::Sphere(RootRadius), Handle(RootComponentSlot),
                     Handle(RootActorSlot), Vec3d::Zero(), motion, gravity, notify);
    }

    // 반폭 ChildHalf 상자 자식을 루트 기준 at에 weld한다(component 기본은 자식 표지).
    Result<void> WeldChild(PhysicsBodyHandle root, const Vec3d& at,
                           uint32 component = ChildComponentSlot,
                           const BodySettings* childSettings = nullptr) noexcept {
        const BodyShapeElement box = BodyShapeElement::Box(Vec3d(ChildHalf, ChildHalf, ChildHalf));
        BodyWeldDesc weld;
        weld.component = Handle(component);
        weld.actor = Handle(ChildActorSlot);
        weld.geometry = Span<const BodyShapeElement>(&box, 1);
        weld.relative.translation = at;
        weld.settings = childSettings;
        return scene->WeldBody(root, weld);
    }

    Result<void> WeldChild(PhysicsBodyHandle root, double offset) noexcept {
        return WeldChild(root, Vec3d(0.0, 0.0, offset));
    }

    BodyKinematics Read(PhysicsBodyHandle body) noexcept {
        auto read = scene->ReadKinematics(body);
        CHECK(read);
        return read ? read.Value() : BodyKinematics{};
    }

    bool TraceDown(const Vec3d& at, HitResult& hit, const QueryParams& params = {}) noexcept {
        auto traced =
            scene->TraceLine(at + Vec3d(0.0, 0.0, RayStart), at - Vec3d(0.0, 0.0, RayStart),
                             StaticObjects(), params, TraceMode::First, &hit, nullptr);
        return traced && traced.Value();
    }

    void Steps(int count) noexcept {
        for (int step = 0; step < count; ++step) {
            CHECK(scene->Step(StepSeconds));
        }
    }

    SystemAllocator allocator;
    UniquePtr<physics::JoltPhysicsScene> scene;
    BodySettings settings{allocator};
};

} // namespace

// weld하면 루트 body에 자식 원소가 더해진다: 질량은 부분마다 UE 규칙을 적용해 더하고(UE
// UpdateMassProperties), 질량 중심은 그 질량 가중이다. 위에서 쏜 ray는 자식 윗면을 자식
// 컴포넌트·원소 번호 1로 맞힌다. unweld하면 되돌아온다.
TEST(Physics_WeldShapeAndMass) {
    WeldScene scene;
    REQUIRE(scene.scene);
    const PhysicsBodyHandle root = scene.Root();
    REQUIRE(scene.WeldChild(root, ChildOffset));
    const double sphere = RawSphereMass(RootRadius);
    const double box = RawCubeMass(ChildHalf);
    const BodyKinematics welded = scene.Read(root);
    const double sphereMass = UnrealMass(sphere);
    const double boxMass = UnrealMass(box);
    CHECK(NearRelative(welded.mass, sphereMass + boxMass, MassTolerance));
    CHECK(
        Near(welded.centerOfMass.z, ChildOffset * boxMass / (sphereMass + boxMass), ComTolerance));
    HitResult hit;
    REQUIRE(scene.TraceDown(Vec3d::Zero(), hit));
    CHECK(hit.component == Handle(ChildComponentSlot));
    CHECK(hit.actor == Handle(ChildActorSlot));
    CHECK(hit.elementIndex == 1);
    CHECK(Near(hit.impactPoint.z, ChildOffset + ChildHalf, DistanceTolerance));

    scene.scene->UnweldBody(root, Handle(ChildComponentSlot));
    CHECK(NearRelative(scene.Read(root).mass, UnrealMass(sphere), MassTolerance));
    REQUIRE(scene.TraceDown(Vec3d::Zero(), hit));
    CHECK(hit.component == Handle(RootComponentSlot));
    CHECK(Near(hit.impactPoint.z, RootRadius, DistanceTolerance));
}

// 부분 원소의 크기 scale은 루트 scale ⊙ relative scale, 중심은 relative로 옮긴 뒤 루트 scale을
// 곱한다(UE GetFinalScaled(Scale3D, RelativeTM)).
TEST(Physics_WeldScaleComposition) {
    WeldScene scene;
    REQUIRE(scene.scene);
    Transform scaled;
    scaled.scale = Vec3d(RootScale, RootScale, RootScale);
    const PhysicsBodyHandle root =
        scene.Add(BodyShapeElement::Sphere(RootRadius * 0.5), Handle(RootComponentSlot),
                  Handle(RootActorSlot), scaled, BodyMotion::Static);
    const BodyShapeElement box =
        BodyShapeElement::Box(Vec3d(ScaledChildHalf, ScaledChildHalf, ScaledChildHalf));
    BodyWeldDesc weld;
    weld.component = Handle(ChildComponentSlot);
    weld.actor = Handle(ChildActorSlot);
    weld.geometry = Span<const BodyShapeElement>(&box, 1);
    weld.relative.translation = Vec3d(0.0, 0.0, ScaledChildOffset);
    weld.relative.scale = Vec3d(1.0, 1.0, ScaledChildZScale);
    REQUIRE(scene.scene->WeldBody(root, weld));
    // 중심 z = 25 × 2 = 50, 반폭 z = 5 × 2 × 0.5 = 5 — 윗면 55.
    const double centerZ = ScaledChildOffset * RootScale;
    HitResult hit;
    REQUIRE(scene.TraceDown(Vec3d::Zero(), hit));
    CHECK(Near(hit.impactPoint.z, centerZ + ScaledChildHalf * RootScale * ScaledChildZScale,
               DistanceTolerance));
    // 반폭 x = 5 × 2 = 10: +X에서 중심 높이로 쏜 ray가 x = 10에 닿는다.
    HitResult side;
    auto traced =
        scene.scene->TraceLine(Vec3d(RayStart, 0.0, centerZ), Vec3d(-RayStart, 0.0, centerZ),
                               StaticObjects(), {}, TraceMode::First, &side, nullptr);
    REQUIRE(traced);
    REQUIRE(traced.Value());
    CHECK(side.component == Handle(ChildComponentSlot));
    CHECK(Near(side.impactPoint.x, ScaledChildHalf * RootScale, DistanceTolerance));
}

// 무시 목록은 부분마다 본다(UE 형상 filter data의 원래 body): 자식 컴포넌트를 무시하면 같은 ray가
// 루트를 맞힌다. 루트 Actor를 무시해도 자식은 맞는다.
TEST(Physics_WeldPartIgnore) {
    WeldScene scene;
    REQUIRE(scene.scene);
    const PhysicsBodyHandle root = scene.Root(BodyMotion::Static);
    REQUIRE(scene.WeldChild(root, ChildOffset));
    const ObjectHandle child = Handle(ChildComponentSlot);
    QueryParams params;
    params.ignoreComponents = Span<const ObjectHandle>(&child, 1);
    HitResult hit;
    REQUIRE(scene.TraceDown(Vec3d::Zero(), hit, params));
    CHECK(hit.component == Handle(RootComponentSlot));
    const ObjectHandle rootActor = Handle(RootActorSlot);
    QueryParams actorParams;
    actorParams.ignoreActors = Span<const ObjectHandle>(&rootActor, 1);
    REQUIRE(scene.TraceDown(Vec3d::Zero(), hit, actorParams));
    CHECK(hit.component == child);
}

// 컴포넌트 overlap은 그 부분의 원소만 쓴다(UE IsShapeBoundToBody): 자식 자리에만 닿는 표적은 자식
// 부분 overlap에만 나온다. 장면 overlap 결과는 (칸, 부분)마다 하나다.
TEST(Physics_WeldPartOverlap) {
    WeldScene scene;
    REQUIRE(scene.scene);
    const PhysicsBodyHandle root = scene.Root(BodyMotion::Static);
    REQUIRE(scene.WeldChild(root, ChildOffset));
    (void)scene.AddAt(BodyShapeElement::Box(Vec3d(TargetHalf, TargetHalf, TargetHalf)),
                      Handle(OtherComponentSlot), Handle(OtherActorSlot),
                      Vec3d(ChildHalf + TargetHalf * 0.5, 0.0, ChildOffset), BodyMotion::Static);
    Array<OverlapResult> overlaps(scene.allocator);
    auto rootOverlap = scene.scene->OverlapBody(root, {}, Vec3d::Zero(), Quatd::Identity(),
                                                StaticObjects(), {}, overlaps);
    REQUIRE(rootOverlap);
    CHECK(overlaps.IsEmpty());
    auto childOverlap =
        scene.scene->OverlapBody(root, Handle(ChildComponentSlot), Vec3d(0.0, 0.0, ChildOffset),
                                 Quatd::Identity(), StaticObjects(), {}, overlaps);
    REQUIRE(childOverlap);
    REQUIRE(overlaps.Size() == 1u);
    CHECK(overlaps[0].component == Handle(OtherComponentSlot));

    // 루트와 자식을 함께 덮는 상자: 결과는 루트·자식·표적 각각 하나다.
    Array<OverlapResult> all(scene.allocator);
    auto shapeOverlap = scene.scene->OverlapShape(
        Vec3d(0.0, 0.0, ChildOffset * 0.5), Quatd::Identity(),
        CollisionShape::Box(Vec3d(TargetHalf * 2.0, TargetHalf, ChildOffset)), StaticObjects(), {},
        OverlapMode::All, &all);
    REQUIRE(shapeOverlap);
    int rootCount = 0;
    int childCount = 0;
    for (const OverlapResult& overlap : all) {
        rootCount += overlap.component == Handle(RootComponentSlot) ? 1 : 0;
        childCount += overlap.component == Handle(ChildComponentSlot) ? 1 : 0;
    }

    CHECK(rootCount == 1);
    CHECK(childCount == 1);
}

// 컴포넌트 sweep은 모든 부분의 Actor를 뺀다(UE "All actors pointed to by shapes should be
// ignored"): 자식 Actor의 다른 body는 맞지 않고, 상관없는 Actor의 body는 맞는다.
TEST(Physics_WeldSweepExcludesPartActors) {
    WeldScene scene;
    REQUIRE(scene.scene);
    const PhysicsBodyHandle root = scene.Root(BodyMotion::Kinematic);
    REQUIRE(scene.WeldChild(root, ChildOffset));
    const BodyShapeElement target =
        BodyShapeElement::Box(Vec3d(TargetHalf, TargetHalf, TargetHalf));
    (void)scene.AddAt(target, Handle(OtherComponentSlot), Handle(ChildActorSlot),
                      Vec3d(NearTargetX, 0.0, 0.0), BodyMotion::Static);
    (void)scene.AddAt(target, Handle(OtherComponentSlot + 1), Handle(OtherActorSlot),
                      Vec3d(FarTargetX, 0.0, 0.0), BodyMotion::Static);
    Array<HitResult> hits(scene.allocator);
    auto traced = scene.scene->TraceBody(root, Vec3d::Zero(), Vec3d(SweepLength, 0.0, 0.0),
                                         Quatd::Identity(), StaticObjects(), {}, hits);
    REQUIRE(traced);
    REQUIRE(!hits.IsEmpty());
    for (const HitResult& hit : hits) {
        CHECK(hit.actor != Handle(ChildActorSlot));
    }

    CHECK(hits[0].component == Handle(OtherComponentSlot + 1));
}

// 접촉 hit는 닿은 원소의 부분 컴포넌트로 난다(UE GetOriginalBodyInstance): 루트 아래에 weld한
// 자식이 바닥에 먼저 닿는다.
TEST(Physics_WeldContactHitComponent) {
    WeldScene scene;
    REQUIRE(scene.scene);
    (void)scene.AddAt(BodyShapeElement::Box(Vec3d(FloorHalf, FloorHalf, FloorHalfThickness)),
                      Handle(OtherComponentSlot), Handle(OtherActorSlot),
                      Vec3d(0.0, 0.0, -FloorHalfThickness), BodyMotion::Static);
    Transform high;
    high.translation = Vec3d(0.0, 0.0, DropHeight);
    const PhysicsBodyHandle root =
        scene.Add(BodyShapeElement::Sphere(RootRadius), Handle(RootComponentSlot),
                  Handle(RootActorSlot), high, BodyMotion::Dynamic, true, true);
    REQUIRE(scene.WeldChild(root, -ChildOffset));
    scene.Steps(FallSteps);
    int childHits = 0;
    int rootHits = 0;
    for (const PhysicsContactHit& contact : scene.scene->TakeContactHits()) {
        childHits += contact.myComponent == Handle(ChildComponentSlot) ? 1 : 0;
        rootHits += contact.myComponent == Handle(RootComponentSlot) ? 1 : 0;
    }

    CHECK(childHits > 0);
    CHECK(rootHits == 0);
}

// 같은 자식을 다시 weld하면 relative를 바꾼다(UE SetWeldedBodyTransform). 잘못된 relative(0
// 회전)·루트 자신은 거부하고 이전 형상을 둔다.
TEST(Physics_WeldReweldAndFailure) {
    WeldScene scene;
    REQUIRE(scene.scene);
    const PhysicsBodyHandle root = scene.Root(BodyMotion::Static);
    REQUIRE(scene.WeldChild(root, ChildOffset));
    REQUIRE(scene.WeldChild(root, MovedChildOffset));
    HitResult hit;
    REQUIRE(scene.TraceDown(Vec3d::Zero(), hit));
    CHECK(hit.elementIndex == 1);
    CHECK(Near(hit.impactPoint.z, MovedChildOffset + ChildHalf, DistanceTolerance));

    const BodyShapeElement box = BodyShapeElement::Box(Vec3d(ChildHalf, ChildHalf, ChildHalf));
    BodyWeldDesc broken;
    broken.component = Handle(OtherComponentSlot);
    broken.geometry = Span<const BodyShapeElement>(&box, 1);
    broken.relative.rotation = Quatd(0.0, 0.0, 0.0, 0.0);
    CHECK(!scene.scene->WeldBody(root, broken));
    BodyWeldDesc self;
    self.component = Handle(RootComponentSlot);
    self.geometry = Span<const BodyShapeElement>(&box, 1);
    CHECK(!scene.scene->WeldBody(root, self));
    REQUIRE(scene.TraceDown(Vec3d::Zero(), hit));
    CHECK(Near(hit.impactPoint.z, MovedChildOffset + ChildHalf, DistanceTolerance));
}

// 자식 body의 질량 중심 이동은 그 부분 질량 중심을 옮긴다(UE MassModifierTransform — 부분별).
TEST(Physics_WeldChildCenterOfMassNudge) {
    WeldScene scene;
    REQUIRE(scene.scene);
    const PhysicsBodyHandle root = scene.Root();
    BodySettings childSettings(scene.allocator);
    childSettings.centerOfMassOffset = Vec3d(0.0, 0.0, ChildNudge);
    REQUIRE(
        scene.WeldChild(root, Vec3d(0.0, 0.0, ChildOffset), ChildComponentSlot, &childSettings));
    const double sphereMass = UnrealMass(RawSphereMass(RootRadius));
    const double boxMass = UnrealMass(RawCubeMass(ChildHalf));
    CHECK(Near(scene.Read(root).centerOfMass.z,
               (ChildOffset + ChildNudge) * boxMass / (sphereMass + boxMass), ComTolerance));
}

// 셋 이상 부분: 가운데 부분을 unweld하면 뒤 부분의 범위·원소 번호가 당겨지고, 다시 weld하면 맨 뒤로
// 간다.
TEST(Physics_WeldThreeParts) {
    WeldScene scene;
    REQUIRE(scene.scene);
    const PhysicsBodyHandle root = scene.Root(BodyMotion::Static);
    REQUIRE(scene.WeldChild(root, ChildOffset));
    REQUIRE(scene.WeldChild(root, Vec3d(0.0, 0.0, SecondChildOffset), SecondChildComponentSlot));
    HitResult hit;
    REQUIRE(scene.TraceDown(Vec3d::Zero(), hit));
    CHECK(hit.component == Handle(SecondChildComponentSlot));
    CHECK(hit.elementIndex == 2);
    scene.scene->UnweldBody(root, Handle(ChildComponentSlot));
    REQUIRE(scene.TraceDown(Vec3d::Zero(), hit));
    CHECK(hit.component == Handle(SecondChildComponentSlot));
    CHECK(hit.elementIndex == 1);
    CHECK(Near(hit.impactPoint.z, SecondChildOffset + ChildHalf, DistanceTolerance));
    REQUIRE(scene.WeldChild(root, ChildOffset));
    REQUIRE(scene.TraceDown(Vec3d::Zero(), hit));
    CHECK(hit.elementIndex == 1);
    // 가운데(첫 자식) 자리는 아래에서 쏜 ray가 루트 다음에 닿는다 — 첫 자식은 이제 원소 2다.
    HitResult up;
    auto traced = scene.scene->TraceLine(Vec3d(0.0, 0.0, ChildOffset - ChildHalf - 1.0),
                                         Vec3d(0.0, 0.0, RayStart), StaticObjects(), {},
                                         TraceMode::First, &up, nullptr);
    REQUIRE(traced);
    REQUIRE(traced.Value());
    CHECK(up.component == Handle(ChildComponentSlot));
    CHECK(up.elementIndex == 2);
}

// weld한 루트의 형상 값·scale 변경은 부분을 지킨다(루트 원소만 바꾸고 부분은 루트 scale로 다시
// 굽는다).
TEST(Physics_WeldRootShapeChangeKeepsParts) {
    WeldScene scene;
    REQUIRE(scene.scene);
    const PhysicsBodyHandle root = scene.Root(BodyMotion::Kinematic);
    REQUIRE(scene.WeldChild(root, ChildOffset));
    const BodyShapeElement bigger = BodyShapeElement::Sphere(RootRadius * 2.0);
    REQUIRE(scene.scene->SetBodyGeometry(root, Span<const BodyShapeElement>(&bigger, 1)));
    HitResult hit;
    REQUIRE(scene.TraceDown(Vec3d::Zero(), hit));
    CHECK(hit.component == Handle(ChildComponentSlot));
    Transform scaled;
    scaled.scale = Vec3d(RootScale, RootScale, RootScale);
    scene.scene->SetBodyTransform(root, scaled, TeleportMode::TeleportPhysics);
    REQUIRE(scene.TraceDown(Vec3d::Zero(), hit));
    CHECK(hit.component == Handle(ChildComponentSlot));
    CHECK(Near(hit.impactPoint.z, (ChildOffset + ChildHalf) * RootScale, DistanceTolerance));
}

// relative 회전은 부분 원소를 돌린다. 부분 overlap은 부분 공간 자세(루트 공간 형상과 같은 형상)를
// 쓴다.
TEST(Physics_WeldRelativeRotation) {
    WeldScene scene;
    REQUIRE(scene.scene);
    const PhysicsBodyHandle root = scene.Root(BodyMotion::Static);
    const BodyShapeElement bar = BodyShapeElement::Box(Vec3d(LongHalf, ShortHalf, ShortHalf));
    BodyWeldDesc weld;
    weld.component = Handle(ChildComponentSlot);
    weld.actor = Handle(ChildActorSlot);
    weld.geometry = Span<const BodyShapeElement>(&bar, 1);
    weld.relative.translation = Vec3d(0.0, 0.0, ChildOffset);
    const Quatd quarter = Quatd::FromUnitAxisAngle(Vec3d(0.0, 0.0, 1.0), math::HalfPi<double>);
    weld.relative.rotation = quarter;
    REQUIRE(scene.scene->WeldBody(root, weld));
    HitResult hit;
    CHECK(scene.TraceDown(Vec3d(0.0, LongProbe, 0.0), hit));
    CHECK(hit.component == Handle(ChildComponentSlot));
    CHECK(!scene.TraceDown(Vec3d(LongProbe, 0.0, 0.0), hit) ||
          hit.component != Handle(ChildComponentSlot));
    // 긴 축(+Y) 끝에만 닿는 표적은 자식 자세(회전 포함)의 부분 overlap에 나온다.
    (void)scene.AddAt(BodyShapeElement::Box(Vec3d(ShortHalf, ShortHalf, ShortHalf)),
                      Handle(OtherComponentSlot), Handle(OtherActorSlot),
                      Vec3d(0.0, LongHalf + ShortHalf * 0.5, ChildOffset), BodyMotion::Static);
    Array<OverlapResult> overlaps(scene.allocator);
    REQUIRE(scene.scene->OverlapBody(root, Handle(ChildComponentSlot), Vec3d(0.0, 0.0, ChildOffset),
                                     quarter, StaticObjects(), {}, overlaps));
    REQUIRE(overlaps.Size() == 1u);
    CHECK(overlaps[0].component == Handle(OtherComponentSlot));
}

// 부분별 무시 목록은 shape sweep·overlap에도 적용된다.
TEST(Physics_WeldPartIgnoreShapeQueries) {
    WeldScene scene;
    REQUIRE(scene.scene);
    const PhysicsBodyHandle root = scene.Root(BodyMotion::Static);
    REQUIRE(scene.WeldChild(root, ChildOffset));
    const ObjectHandle child = Handle(ChildComponentSlot);
    QueryParams params;
    params.ignoreComponents = Span<const ObjectHandle>(&child, 1);
    HitResult swept;
    auto traced =
        scene.scene->TraceShape(Vec3d(0.0, 0.0, RayStart), Vec3d(0.0, 0.0, -RayStart),
                                Quatd::Identity(), CollisionShape::Sphere(ShortHalf),
                                StaticObjects(), params, TraceMode::First, &swept, nullptr);
    REQUIRE(traced);
    REQUIRE(traced.Value());
    CHECK(swept.component == Handle(RootComponentSlot));
    Array<OverlapResult> overlaps(scene.allocator);
    REQUIRE(
        scene.scene->OverlapShape(Vec3d(0.0, 0.0, ChildOffset * 0.5), Quatd::Identity(),
                                  CollisionShape::Box(Vec3d(TargetHalf, TargetHalf, ChildOffset)),
                                  StaticObjects(), params, OverlapMode::All, &overlaps));
    REQUIRE(overlaps.Size() == 1u);
    CHECK(overlaps[0].component == Handle(RootComponentSlot));
}

// weld는 잠든 body를 깨우지 않는다(UE Weld — 형상 교체와 PostShapeChange뿐).
TEST(Physics_WeldDoesNotWake) {
    WeldScene scene;
    REQUIRE(scene.scene);
    BodyCreateDesc desc;
    const BodyShapeElement sphere = BodyShapeElement::Sphere(RootRadius);
    desc.component = Handle(RootComponentSlot);
    desc.actor = Handle(RootActorSlot);
    desc.geometry = Span<const BodyShapeElement>(&sphere, 1);
    desc.motion = BodyMotion::Dynamic;
    desc.collision.collisionEnabled = CollisionEnabled::QueryAndPhysics;
    desc.startAwake = false;
    auto created = scene.scene->CreateBody(desc);
    REQUIRE(created);
    REQUIRE(!scene.Read(created.Value()).awake);
    REQUIRE(scene.WeldChild(created.Value(), ChildOffset));
    CHECK(!scene.Read(created.Value()).awake);
}

// 접촉 hit는 body 쌍마다 하나다(UE 입자 쌍마다 — 부분이 둘 다 닿아도): 루트와 옆 자식이 함께 바닥에
// 놓인다.
TEST(Physics_WeldContactHitPerBodyPair) {
    WeldScene scene;
    REQUIRE(scene.scene);
    (void)scene.AddAt(BodyShapeElement::Box(Vec3d(FloorHalf, FloorHalf, FloorHalfThickness)),
                      Handle(OtherComponentSlot), Handle(OtherActorSlot),
                      Vec3d(0.0, 0.0, -FloorHalfThickness), BodyMotion::Static);
    const PhysicsBodyHandle root = scene.AddAt(
        BodyShapeElement::Box(Vec3d(ChildHalf, ChildHalf, ChildHalf)), Handle(RootComponentSlot),
        Handle(RootActorSlot), Vec3d(0.0, 0.0, ChildHalf), BodyMotion::Dynamic, true, true);
    REQUIRE(scene.WeldChild(root, Vec3d(SideOffset, 0.0, 0.0)));
    int maxPerStep = 0;
    int total = 0;
    for (int step = 0; step < SettleSteps; ++step) {
        CHECK(scene.scene->Step(StepSeconds));
        int count = 0;
        for (const PhysicsContactHit& contact : scene.scene->TakeContactHits()) {
            count += contact.myActor == Handle(RootActorSlot) ||
                             contact.myActor == Handle(ChildActorSlot)
                         ? 1
                         : 0;
        }

        maxPerStep = count > maxPerStep ? count : maxPerStep;
        total += count;
    }

    CHECK(total > 0);
    CHECK(maxPerStep == 1);
}

// 부분 원소가 Jolt 형상을 만들지 못하면(비유한 값) 이전 형상·질량·부분을 둔다.
TEST(Physics_WeldFailureKeepsState) {
    WeldScene scene;
    REQUIRE(scene.scene);
    const PhysicsBodyHandle root = scene.Root();
    REQUIRE(scene.WeldChild(root, ChildOffset));
    const double before = scene.Read(root).mass;
    BodyShapeElement broken = BodyShapeElement::Sphere(RootRadius);
    broken.center = Vec3d(platform::PlatformMath::QuietNaN<double>(), 0.0, 0.0);
    BodyWeldDesc weld;
    weld.component = Handle(SecondChildComponentSlot);
    weld.geometry = Span<const BodyShapeElement>(&broken, 1);
    CHECK(!scene.scene->WeldBody(root, weld));
    CHECK(scene.Read(root).mass == before);
    HitResult hit;
    REQUIRE(scene.TraceDown(Vec3d::Zero(), hit));
    CHECK(hit.component == Handle(ChildComponentSlot));
}

} // namespace physics_weld_scene_tests_file
