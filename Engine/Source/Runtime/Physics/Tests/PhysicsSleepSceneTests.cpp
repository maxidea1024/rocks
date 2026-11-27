// Physics 모듈 PH5d 장면 시험이다(Docs/Design/PhysicsModule 2.7절, 7.1.19절): 잠듦·깨움 접점과
// 이벤트 — 깨우기·재우기는 이벤트 없음(깨우기는 쌓인 이벤트를 지우고 재우기는 둔다), 재우기의 속도
// 0·대기 몫 버림, 힘·teleport·속도 setter의 깨움은 Awake, solver 잠듦은 Sleep, 마지막 전이가 이김,
// 생성·Kinematic → Dynamic 전환·형상 변경은 이벤트 없음, Kinematic·지운 body 무시. Jolt 칸을 직접
// 만들므로 Monolithic 시험(EngineTests)에서만 돈다(BuildTargets 3.8절 BD29 시험 실행 파일 예외).
#include "../Private/JoltPhysicsScene.h"

#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Physics/PhysicsSettings.h>
#include <TestSupport/Test.h>
#include <World/Physics/BodyCreateDesc.h>

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSettings.h>
// clang-format on

using namespace rocks;
namespace physics_sleep_scene_tests_file {

namespace {

constexpr float StepSeconds = 1.0f / 60.0f;
// 떠 있는 body가 잠들기에 넉넉한 step 수다: Jolt 기본 mTimeBeforeSleep(Rocks는 바꾸지 않는다)을
// step 길이로 나눈 수에 여유를 더한다(고정 step — 시간 의존 없음).
constexpr int SleepMarginSteps = 15;
const int SleepSteps =
    static_cast<int>(JPH::PhysicsSettings().mTimeBeforeSleep / StepSeconds) + SleepMarginSteps;
constexpr uint32 TestMaxBodies = 64;
constexpr uint32 TestMaxBodyPairs = 256;
constexpr uint32 TestMaxContactConstraints = 256;
constexpr uint32 TestTempAllocatorBytes = 4u * 1024u * 1024u;
constexpr double SphereRadius = 20.0;
constexpr double LargerRadius = 30.0;
constexpr double BodySpacing = 500.0; // cm
constexpr double Speed = 100.0;       // cm/s
constexpr double TeleportDistance = 50.0;
constexpr double VelocityTolerance = 1.0e-3;
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

// 중력·감쇠 없이 떠 있는 설정이다(asleep이면 잠든 채 만든다).
BodySettings Floating(Allocator& allocator, bool asleep = false) noexcept {
    BodySettings settings(allocator);
    settings.enableGravity = false;
    settings.startAwake = !asleep;
    return settings;
}

struct SleepScene {
    SleepScene() noexcept {
        auto created = physics::JoltPhysicsScene::Create(allocator, TestSettings(), nullptr);
        if (created) {
            scene = Move(created.Value());
            scene->SetSimulates(true);
        }
    }

    PhysicsBodyHandle Sphere(const BodySettings& settings,
                             BodyMotion motion = BodyMotion::Dynamic) noexcept {
        BodyCreateDesc desc;
        desc.component = ObjectHandle{FakeRegistry, ++tags, FakeGeneration};
        desc.actor = desc.component;
        desc.geometry.kind = BodyGeometryKind::Sphere;
        desc.geometry.radius = SphereRadius;
        desc.worldTransform.translation = Vec3d(static_cast<double>(tags) * BodySpacing, 0.0, 0.0);
        desc.motion = motion;
        desc.collision.collisionEnabled = CollisionEnabled::QueryAndPhysics;
        desc.settings = &settings;
        desc.startAwake = settings.startAwake;
        auto created = scene->CreateBody(desc);
        CHECK(created);
        return created ? created.Value() : PhysicsBodyHandle{};
    }

    void Steps(int count) noexcept {
        for (int step = 0; step < count; ++step) {
            CHECK(scene->Step(StepSeconds));
        }
    }

    BodyKinematics Read(PhysicsBodyHandle body) noexcept {
        auto read = scene->ReadKinematics(body);
        CHECK(read);
        return read ? read.Value() : BodyKinematics{};
    }

    // 꺼낸 이벤트 수와 body의 이벤트다. 컴포넌트 표지는 칸 번호 + 1로 찾는다(이 시험들은 칸을
    // 재사용하지 않는다 — 지운 칸 뒤에 만들지 않는다).
    struct Taken {
        usize count = 0;
        bool found = false;
        bool awake = false;
    };

    Taken Take(PhysicsBodyHandle body) noexcept {
        Taken taken;
        const ObjectHandle component{FakeRegistry, body.index + 1, FakeGeneration};
        for (const physics::WakeEvent& event : scene->TakeWakeEvents()) {
            ++taken.count;
            if (event.component == component) {
                taken.found = true;
                taken.awake = event.awake;
            }
        }

        return taken;
    }

    SystemAllocator allocator;
    UniquePtr<physics::JoltPhysicsScene> scene;
    uint32 tags = 0;
};

BodyForce ZeroForce() noexcept {
    BodyForce force;
    force.kind = BodyForceKind::Force;
    return force;
}

} // namespace

// 깨우기·재우기는 이벤트가 없다. 재우면 속도 0이고 대기 충격량을 버린다. 깨우기는 쌓인 Awake를
// 지우고(UE ClearEvents), 재우기는 쌓인 이벤트를 둔다.
TEST(Physics_SleepApiRaisesNoEvents) {
    SleepScene scene;
    REQUIRE(scene.scene);
    const BodySettings awake = Floating(scene.allocator);
    const PhysicsBodyHandle ball = scene.Sphere(awake);
    scene.scene->SetBodyLinearVelocity(ball, Vec3d(Speed, 0.0, 0.0), false);
    BodyForce kick;
    kick.kind = BodyForceKind::Impulse;
    kick.value = Vec3d(0.0, 0.0, Speed);
    kick.massIndependent = true;
    scene.scene->ApplyBodyForce(ball, kick);
    (void)scene.Take(ball);
    scene.scene->SetBodyAwake(ball, false);
    CHECK(!scene.Read(ball).awake);
    CHECK(scene.Read(ball).linearVelocity.Length() <= VelocityTolerance);
    scene.scene->SetBodyAwake(ball, true);
    CHECK(scene.Read(ball).awake);
    CHECK(scene.Take(ball).count == 0u);
    scene.Steps(1);
    CHECK(scene.Read(ball).linearVelocity.Length() <= VelocityTolerance);

    const BodySettings asleep = Floating(scene.allocator, true);
    const PhysicsBodyHandle cleared = scene.Sphere(asleep);
    scene.scene->ApplyBodyForce(cleared, ZeroForce());
    scene.scene->SetBodyAwake(cleared, true);
    CHECK(!scene.Take(cleared).found);

    const PhysicsBodyHandle kept = scene.Sphere(asleep);
    scene.scene->ApplyBodyForce(kept, ZeroForce());
    scene.scene->SetBodyAwake(kept, false);
    const SleepScene::Taken taken = scene.Take(kept);
    CHECK(taken.found);
    CHECK(taken.awake);
}

// 게임 API의 깨움(힘·teleport·속도 setter)은 Awake다(UE bAllowEvents 참). 떠 있는 body가 잠들면
// Sleep이고, 같은 프레임에 깼다 잠들면 마지막 Sleep 하나다.
TEST(Physics_SleepTransitionsRaiseEvents) {
    SleepScene scene;
    REQUIRE(scene.scene);
    const BodySettings asleep = Floating(scene.allocator, true);
    const PhysicsBodyHandle forced = scene.Sphere(asleep);
    const PhysicsBodyHandle moved = scene.Sphere(asleep);
    const PhysicsBodyHandle pushed = scene.Sphere(asleep);
    scene.scene->ApplyBodyForce(forced, ZeroForce());
    Transform target;
    target.translation = scene.Read(moved).location + Vec3d(0.0, 0.0, TeleportDistance);
    scene.scene->SetBodyTransform(moved, target, TeleportMode::TeleportPhysics);
    scene.scene->SetBodyLinearVelocity(pushed, Vec3d(Speed, 0.0, 0.0), false);
    usize awakeEvents = 0;
    for (const physics::WakeEvent& event : scene.scene->TakeWakeEvents()) {
        CHECK(event.awake);
        ++awakeEvents;
    }

    CHECK(awakeEvents == 3u);

    // forced는 꺼낸 뒤 깨어 있다가 잠든다(Sleep). moved는 Awake가 쌓인 채 잠든다 — 마지막
    // Sleep 하나만 남는다(Chaos 입자마다 마지막 값).
    scene.scene->SetBodyAwake(pushed, false);
    scene.scene->SetBodyAwake(moved, false);
    scene.scene->ApplyBodyForce(moved, ZeroForce());
    scene.Steps(SleepSteps);
    CHECK(!scene.Read(forced).awake);
    CHECK(!scene.Read(moved).awake);
    const ObjectHandle forcedComponent{FakeRegistry, forced.index + 1, FakeGeneration};
    const ObjectHandle movedComponent{FakeRegistry, moved.index + 1, FakeGeneration};
    int forcedEvents = 0;
    int movedEvents = 0;
    for (const physics::WakeEvent& event : scene.scene->TakeWakeEvents()) {
        if (event.component == forcedComponent || event.component == movedComponent) {
            CHECK(!event.awake);
            ++(event.component == forcedComponent ? forcedEvents : movedEvents);
        }
    }

    CHECK(forcedEvents == 1);
    CHECK(movedEvents == 1);
}

// 생성(깨어서·잠들어서), Kinematic → Dynamic 전환의 깨움, 형상 변경의 깨움은 이벤트가 없고(뒤 둘은
// 쌓인 이벤트를 지운다), Kinematic body·지운 body의 전이는 버린다.
TEST(Physics_SleepIgnoresSilentPaths) {
    SleepScene scene;
    REQUIRE(scene.scene);
    const BodySettings awake = Floating(scene.allocator);
    const BodySettings asleep = Floating(scene.allocator, true);
    (void)scene.Sphere(awake);
    (void)scene.Sphere(asleep);
    CHECK(scene.scene->TakeWakeEvents().Size() == 0u);

    const PhysicsBodyHandle switched = scene.Sphere(awake, BodyMotion::Kinematic);
    scene.scene->SetBodyMotion(switched, BodyMotion::Dynamic, true);
    CHECK(scene.Read(switched).awake);
    const PhysicsBodyHandle reshaped = scene.Sphere(asleep);
    scene.scene->ApplyBodyForce(reshaped, ZeroForce());
    BodyGeometry larger;
    larger.kind = BodyGeometryKind::Sphere;
    larger.radius = LargerRadius;
    CHECK(scene.scene->SetBodyGeometry(reshaped, larger));
    CHECK(scene.scene->TakeWakeEvents().Size() == 0u);

    const PhysicsBodyHandle mover = scene.Sphere(awake, BodyMotion::Kinematic);
    Transform target;
    target.translation = scene.Read(mover).location + Vec3d(TeleportDistance, 0.0, 0.0);
    scene.scene->SetBodyTransform(mover, target, TeleportMode::None);
    scene.Steps(SleepSteps);
    CHECK(!scene.Take(mover).found);

    const PhysicsBodyHandle doomed = scene.Sphere(asleep);
    scene.scene->ApplyBodyForce(doomed, ZeroForce());
    scene.scene->DestroyBody(doomed);
    CHECK(!scene.Take(doomed).found);
}

// 중력 켬·충돌 필터 변경의 깨움은 이벤트가 없고 쌓인 이벤트를 지운다(UE WakeUp_AssumesLocked).
TEST(Physics_SleepGravityAndFilterWakeSilently) {
    SleepScene scene;
    REQUIRE(scene.scene);
    BodySettings asleep = Floating(scene.allocator, true);
    const PhysicsBodyHandle lifted = scene.Sphere(asleep);
    const PhysicsBodyHandle filtered = scene.Sphere(asleep);
    scene.scene->ApplyBodyForce(lifted, ZeroForce());
    asleep.enableGravity = true;
    scene.scene->UpdateBodySettings(lifted, asleep, BodySettingsChange::Gravity);
    CHECK(scene.Read(lifted).awake);
    scene.scene->ApplyBodyForce(filtered, ZeroForce());
    ResolvedCollision collision;
    collision.collisionEnabled = CollisionEnabled::QueryAndPhysics;
    collision.objectType = CollisionChannel::WorldDynamic;
    scene.scene->SetBodyCollision(filtered, collision);
    CHECK(scene.Read(filtered).awake);
    CHECK(scene.scene->TakeWakeEvents().Size() == 0u);
}

} // namespace physics_sleep_scene_tests_file
