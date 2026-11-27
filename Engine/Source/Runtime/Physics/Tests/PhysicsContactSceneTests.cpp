// Physics 모듈 PH4c 장면 시험이다(Docs/Design/PhysicsModule 6.4절, 7.1.12절): hit 문턱
// (minDeltaVelocityForHitEvents — 쉬는 접촉은 거르고 부딪는 접촉은 남김), CCD(continuousCollision —
// 얇은 벽을 지나가지 않고 solver 충격량 대신 추정 충격량), 설정 검증. 설정을 장면에 바로 넘기고
// Jolt body 속도를 직접 정하므로 Monolithic 시험(EngineTests)에서만 돈다(BuildTargets 3.8절 BD29
// 시험 실행 파일 예외).
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
#include <Jolt/Physics/Body/BodyInterface.h>
// clang-format on

using namespace rocks;
namespace physics_contact_scene_tests_file {

namespace {

constexpr double StepSeconds = 1.0 / 60.0;
constexpr uint32 TestMaxBodies = 64;
constexpr uint32 TestMaxBodyPairs = 256;
constexpr uint32 TestMaxContactConstraints = 256;
constexpr uint32 TestTempAllocatorBytes = 4u * 1024u * 1024u;

constexpr double FloorHalfExtent = 500.0;
constexpr double FloorHalfThickness = 50.0;
constexpr double SphereRadius = 20.0;
constexpr double DropHeight = 200.0;
constexpr int MaxFallSteps = 90;
constexpr int RestingSteps = 5;
// 내려앉기를 기다리는 스텝 수, 멈췄다고 보는 수직 속도(cm/s), 바닥 높이 허용 오차(cm)다.
constexpr int MaxSettleSteps = 120;
constexpr double RestVelocity = 2.0;
constexpr int SettledSteps = 5;
constexpr double RestHeightTolerance = 3.0;
// 쉬는 접촉의 스텝당 속도 변화(g·dt ≈ 16 cm/s)보다 크고, 200 cm 낙하의 충돌 속도 변화(≈ 600
// cm/s)보다 작은 문턱(cm/s)이다.
constexpr float RestingFilterThreshold = 50.0f;

// CCD 장면: 중력 없는 작은 구가 얇은 벽으로 빠르게 간다(스텝당 이동 500 cm ≫ 벽 두께 + 지름).
constexpr double BulletRadius = 5.0;
constexpr double BulletSpeed = 30000.0; // cm/s(Jolt 기본 속도 상한 500 m/s 아래)
constexpr double WallX = 200.0;
constexpr double WallHalfThickness = 1.0;
constexpr double WallHalfSize = 100.0;
constexpr int BulletSteps = 2;
constexpr double CentimetersPerMeter = 100.0;
constexpr double JoltDefaultDensity = 1000.0; // kg/m³
// 추정 충격량((1 + e)·m·v — 정적 벽)의 상대 허용 오차다.
constexpr double EstimateTolerance = 0.1;

// 가짜 handle 값이다: registry·세대(0이 아니면 null이 아니다), Actor handle 칸 번호의 offset.
constexpr uint64 FakeRegistry = 1;
constexpr uint32 FakeGeneration = 1;
constexpr uint32 ActorSlotOffset = 1000;

// probe가 바닥을 다 지나가는 스텝 수다.
constexpr int ProbeFallSteps = 150;

// listener 용량 넘침 장면: 바닥에 놓인 구가 처음 step의 용량 하한(64)보다 많다.
constexpr uint32 CrowdRows = 10;
constexpr uint32 CrowdSpheres = CrowdRows * CrowdRows;
constexpr double CrowdSpacing = 60.0;
constexpr uint32 CrowdMaxBodies = 256;
constexpr uint32 CrowdMaxPairs = 1024;

PhysicsSettings TestSettings() noexcept {
    PhysicsSettings settings;
    settings.maxBodies = TestMaxBodies;
    settings.maxBodyPairs = TestMaxBodyPairs;
    settings.maxContactConstraints = TestMaxContactConstraints;
    settings.tempAllocatorBytes = TestTempAllocatorBytes;
    return settings;
}

// 시험이 hit의 컴포넌트를 가려 볼 수 있게 칸마다 다른 가짜 handle이다(장면은 값만 옮긴다).
ObjectHandle FakeHandle(uint32 slot) noexcept {
    return ObjectHandle{FakeRegistry, slot, FakeGeneration};
}

struct ContactScene {
    explicit ContactScene(const PhysicsSettings& settings) noexcept {
        auto created = physics::JoltPhysicsScene::Create(allocator, settings, nullptr);
        if (created) {
            scene = Move(created.Value());
            scene->SetSimulates(true);
        }
    }

    PhysicsBodyHandle Add(uint32 tag, BodyGeometry geometry, const Vec3d& at, BodyMotion motion,
                          const BodySettings& settings,
                          CollisionEnabled enabled = CollisionEnabled::QueryAndPhysics) noexcept {
        BodyCreateDesc desc;
        desc.component = FakeHandle(tag);
        desc.actor = FakeHandle(tag + ActorSlotOffset);
        desc.geometry = geometry;
        desc.worldTransform.translation = at;
        desc.motion = motion;
        desc.collision.collisionEnabled = enabled;
        desc.collision.objectType = motion == BodyMotion::Static ? CollisionChannel::WorldStatic
                                                                 : CollisionChannel::PhysicsBody;
        desc.settings = &settings;
        auto created = scene->CreateBody(desc);
        CHECK(created);
        return created ? created.Value() : PhysicsBodyHandle{};
    }

    PhysicsBodyHandle Floor(const BodySettings& settings) noexcept {
        BodyGeometry box;
        box.kind = BodyGeometryKind::Box;
        box.halfExtent = Vec3d(FloorHalfExtent, FloorHalfExtent, FloorHalfThickness);
        return Add(1, box, Vec3d(0.0, 0.0, -FloorHalfThickness), BodyMotion::Static, settings);
    }

    PhysicsBodyHandle
    Sphere(uint32 tag, double radius, const Vec3d& at, const BodySettings& settings,
           CollisionEnabled enabled = CollisionEnabled::QueryAndPhysics) noexcept {
        BodyGeometry sphere;
        sphere.kind = BodyGeometryKind::Sphere;
        sphere.radius = radius;
        return Add(tag, sphere, at, BodyMotion::Dynamic, settings, enabled);
    }

    // handle의 Jolt body ID다(user data로 찾는다).
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

    // step 하나를 돌고 쌓인 hit 수를 돌려준다(꺼낸 hit는 last에 남는다).
    usize StepAndTake() noexcept {
        CHECK(scene->Step(static_cast<float>(StepSeconds)));
        last = scene->TakeContactHits();
        return last.Size();
    }

    // 구가 바닥에 내려앉을 때까지 간다: 기본 재질 반발(0.3 — 7.1.15절)로 몇 번 튄 뒤 반발 문턱(1000
    // cm/s² × dt) 아래 충돌에서 멈춘다. 돌려주는 값은 내려앉았는지다.
    bool SettleOnFloor() noexcept {
        // 튀는 정점에서도 속도가 잠깐 0이므로 몇 스텝 연속으로 멈춰 있어야 내려앉은 것이다.
        int still = 0;
        for (int step = 0; step < MaxSettleSteps; ++step) {
            (void)StepAndTake();
            auto read = scene->ReadKinematics(ball);
            const bool resting = read && math::Abs(read.Value().linearVelocity.z) < RestVelocity &&
                                 read.Value().location.z <= SphereRadius + RestHeightTolerance;
            still = resting ? still + 1 : 0;
            if (still >= SettledSteps) {
                return true;
            }
        }

        return false;
    }

    SystemAllocator allocator;
    UniquePtr<physics::JoltPhysicsScene> scene;
    Span<const PhysicsContactHit> last;
    PhysicsBodyHandle ball;
};

// 표지 켠 구를 바닥에 떨어뜨려 첫 hit 스텝까지 간다. 돌려주는 값은 첫 hit를 찾았는지다.
bool DropUntilFirstHit(ContactScene& scene) noexcept {
    BodySettings plain(scene.allocator);
    BodySettings notify(scene.allocator);
    notify.notifyRigidBodyCollision = true;
    scene.Floor(plain);
    scene.ball = scene.Sphere(2, SphereRadius, Vec3d(0.0, 0.0, DropHeight), notify);
    for (int step = 0; step < MaxFallSteps; ++step) {
        if (scene.StepAndTake() != 0) {
            return true;
        }
    }

    return false;
}

} // namespace

// 문턱이 0이면 쉬는 접촉도 스텝마다 hit다. 문턱이 쉬는 접촉의 속도 변화보다 크면 부딪는 첫 hit만
// 남고 쉬는 접촉 hit는 없다(UE MinDeltaVelocityForHitEvents의 의도 — UE 5.8은 계산만 한다).
TEST(Physics_ContactHitThresholdDropsRestingContacts) {
    ContactScene unfiltered(TestSettings());
    PhysicsSettings filteredSettings = TestSettings();
    filteredSettings.minDeltaVelocityForHitEvents = RestingFilterThreshold;
    ContactScene filtered(filteredSettings);
    REQUIRE(unfiltered.scene);
    REQUIRE(filtered.scene);

    REQUIRE(DropUntilFirstHit(unfiltered));
    REQUIRE(DropUntilFirstHit(filtered));
    REQUIRE(filtered.last.Size() == 1);
    CHECK(filtered.last[0].normalImpulse.z > 0.0);
    REQUIRE(unfiltered.SettleOnFloor());
    REQUIRE(filtered.SettleOnFloor());

    for (int step = 0; step < RestingSteps; ++step) {
        CHECK(unfiltered.StepAndTake() == 1);
        CHECK(filtered.StepAndTake() == 0);
    }
}

// CCD를 켠 빠른 구는 얇은 벽에 막혀 hit 하나를 받는다: CCD로만 닿아 solver 충격량이 없으므로 추정
// 충격량(≈ (1 + e)·m·v, 벽에서 구 쪽 −X)이다. CCD가 없으면 지나가고 hit가 없다.
TEST(Physics_ContactHitContinuousCollisionUsesEstimate) {
    for (const bool continuous : {true, false}) {
        ContactScene scene(TestSettings());
        REQUIRE(scene.scene);
        BodySettings wallSettings(scene.allocator);
        BodySettings bulletSettings(scene.allocator);
        bulletSettings.enableGravity = false;
        bulletSettings.notifyRigidBodyCollision = true;
        bulletSettings.continuousCollision = continuous;
        BodyGeometry wall;
        wall.kind = BodyGeometryKind::Box;
        wall.halfExtent = Vec3d(WallHalfThickness, WallHalfSize, WallHalfSize);
        const PhysicsBodyHandle wallBody =
            scene.Add(1, wall, Vec3d(WallX, 0.0, 0.0), BodyMotion::Static, wallSettings);
        const PhysicsBodyHandle bullet =
            scene.Sphere(2, BulletRadius, Vec3d::Zero(), bulletSettings);
        REQUIRE(!wallBody.IsNull() && !bullet.IsNull());
        const JPH::BodyID bulletId = scene.JoltBody(bullet);
        REQUIRE(!bulletId.IsInvalid());
        // Rocks +X(cm/s)를 Jolt 축·단위로 바꿔 넣는다(축 A — Rocks +X는 Jolt +Z).
        scene.scene->System().GetBodyInterface().SetLinearVelocity(
            bulletId, physics::ToJoltLinearVelocity(Vec3d(BulletSpeed, 0.0, 0.0)));

        usize hits = 0;
        usize firstStepHits = 0;
        Vec3d impulse = Vec3d::Zero();
        Vec3d normal = Vec3d::Zero();
        ObjectHandle other;
        for (int step = 0; step < BulletSteps; ++step) {
            const usize taken = scene.StepAndTake();
            if (taken != 0 && hits == 0) {
                // discrete·CCD 콜백이 같은 쌍에 여럿 와도 hit는 하나로 합친다.
                firstStepHits = taken;
                impulse = scene.last[0].normalImpulse;
                normal = scene.last[0].hit.normal;
                other = scene.last[0].otherComponent;
            }

            hits += taken;
        }

        auto read = scene.scene->ReadKinematics(bullet);
        REQUIRE(read);
        if (!continuous) {
            CHECK(read.Value().location.x > WallX);
            CHECK(hits == 0);
            continue;
        }

        CHECK(read.Value().location.x < WallX);
        REQUIRE(hits >= 1);
        CHECK(firstStepHits == 1);
        const double radius = BulletRadius / CentimetersPerMeter;
        // body 질량은 균일 밀도 질량^0.75다(UE RaiseMassToPower — 7.1.15절).
        const double mass = deterministic::Pow(4.0 / 3.0 * math::Pi<double> * radius * radius *
                                                   radius * JoltDefaultDensity,
                                               physics::DefaultPhysicalMaterialRaiseMassToPower);
        // 기본 재질 반발 0.3(7.1.15절)이라 충격량은 (1 + e)·m·v다.
        const double momentum =
            (1.0 + static_cast<double>(physics::DefaultPhysicalMaterialRestitution)) * mass *
            BulletSpeed; // kg·cm/s
        CHECK(other == FakeHandle(1));
        CHECK(math::Abs(impulse.x + momentum) <= momentum * EstimateTolerance);
        CHECK(normal.x < 0.0);
    }
}

// 문턱이 있어도 probe hit는 걸러지지 않는다(반작용이 없어 속도 변화가 늘 0이다). probe hit는 실제로
// 겹친 접촉만이다(깊이 > 0, 충격량 0).
TEST(Physics_ContactHitThresholdKeepsProbeHits) {
    PhysicsSettings settings = TestSettings();
    settings.minDeltaVelocityForHitEvents = RestingFilterThreshold;
    ContactScene scene(settings);
    REQUIRE(scene.scene);
    BodySettings plain(scene.allocator);
    BodySettings notify(scene.allocator);
    notify.notifyRigidBodyCollision = true;
    scene.Floor(plain);
    const PhysicsBodyHandle probe = scene.Sphere(2, SphereRadius, Vec3d(0.0, 0.0, DropHeight),
                                                 notify, CollisionEnabled::ProbeOnly);
    REQUIRE(!probe.IsNull());
    usize hits = 0;
    for (int step = 0; step < ProbeFallSteps; ++step) {
        const usize taken = scene.StepAndTake();
        for (const PhysicsContactHit& hit : scene.last) {
            CHECK(hit.normalImpulse == Vec3d::Zero());
            CHECK(hit.hit.penetrationDepth > 0.0);
        }

        hits += taken;
    }

    CHECK(hits > 0);
    auto read = scene.scene->ReadKinematics(probe);
    REQUIRE(read);
    CHECK(read.Value().location.z < -FloorHalfThickness);
}

// 처음 step의 listener 용량보다 접촉이 많으면 넘친 몫을 잃고 Warning이 남는다(어느 접촉이
// 빠지는지는 worker 배정에 달렸다 — 7.1.12절 한계). 다음 step은 늘어난 용량으로 모두 받는다.
TEST(Physics_ContactHitCapacityGrowsAfterOverflow) {
    PhysicsSettings settings = TestSettings();
    settings.maxBodies = CrowdMaxBodies;
    settings.maxBodyPairs = CrowdMaxPairs;
    settings.maxContactConstraints = CrowdMaxPairs;
    ContactScene scene(settings);
    REQUIRE(scene.scene);
    BodySettings notify(scene.allocator);
    notify.notifyRigidBodyCollision = true;
    BodySettings plain(scene.allocator);
    scene.Floor(notify);
    const double origin = -0.5 * CrowdSpacing * static_cast<double>(CrowdRows - 1);
    for (uint32 index = 0; index < CrowdSpheres; ++index) {
        const double x = origin + CrowdSpacing * static_cast<double>(index % CrowdRows);
        const double y = origin + CrowdSpacing * static_cast<double>(index / CrowdRows);
        // 바닥에 닿은 채로 둔다(첫 step부터 쉬는 접촉).
        REQUIRE(!scene.Sphere(2 + index, SphereRadius, Vec3d(x, y, SphereRadius), plain).IsNull());
    }

    CHECK(scene.StepAndTake() < CrowdSpheres);
    CHECK(scene.StepAndTake() == CrowdSpheres);
}

// hit 문턱이 음수·비유한이면 설정 검증이 거부한다.
TEST(Physics_SettingsRejectInvalidHitThreshold) {
    PhysicsSettings settings = TestSettings();
    CHECK(PhysicsSettings::Validate(settings));
    settings.minDeltaVelocityForHitEvents = -1.0f;
    CHECK(PhysicsSettings::Validate(settings).Error() == ErrorCode::InvalidArgument);
    settings.minDeltaVelocityForHitEvents = platform::PlatformMath::Infinity<float>();
    CHECK(PhysicsSettings::Validate(settings).Error() == ErrorCode::InvalidArgument);
}

} // namespace physics_contact_scene_tests_file
