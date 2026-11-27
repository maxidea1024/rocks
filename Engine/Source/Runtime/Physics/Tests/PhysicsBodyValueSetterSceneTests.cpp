// Physics 모듈 PH5c2 장면 시험이다(Docs/Design/PhysicsModule 2.6절, 7.1.18절): 실행 중 body 값 갱신
// — 묶음마다 Jolt 값(중력 계수·감쇠·motion quality·각속도 상한·gyroscopic·허용 자유도와 구속), 중력
// 켬의 깨움, 질량 다시 계산(override·scale·질량 중심 이동·관성)과 깨우지 않음, Static은 운동 값
// 없음. Jolt body 값을 직접 읽으므로 Monolithic 시험(EngineTests)에서만 돈다(BuildTargets 3.8절
// BD29 시험 실행 파일 예외).
#include "../Private/JoltConversions.h"
#include "../Private/JoltPhysicsScene.h"

#include <Base/InitializerList.h>
#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Physics/PhysicsSettings.h>
#include <TestSupport/Test.h>
#include <World/Physics/BodyCreateDesc.h>

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyLock.h>
// clang-format on

using namespace rocks;
namespace physics_body_value_setter_scene_tests_file {

namespace {

constexpr uint32 TestMaxBodies = 64;
constexpr uint32 TestMaxBodyPairs = 256;
constexpr uint32 TestMaxContactConstraints = 256;
constexpr uint32 TestTempAllocatorBytes = 4u * 1024u * 1024u;
constexpr double SphereRadius = 20.0;
constexpr double BodySpacing = 500.0; // cm
constexpr float NewLinearDamping = 0.25f;
constexpr float NewAngularDamping = 0.5f;
constexpr float OverrideDegrees = 90.0f;
constexpr float NewMass = 5.0f;
constexpr float NewMassScale = 3.0f;
constexpr double ValueTolerance = 1.0e-5;
constexpr double MassTolerance = 1.0e-4;   // kg·kg·cm²(float)
constexpr double OffsetTolerance = 1.0e-3; // cm
constexpr double PushSpeed = 100.0;        // cm/s
constexpr int PushSteps = 30;
constexpr double PositionTolerance = 0.5; // cm(구속 solver 오차)
constexpr float StepSeconds = 1.0f / 60.0f;
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

struct ValueScene {
    ValueScene() noexcept {
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

    // body를 잠그고 read(Body&)를 부른다.
    template <class Read>
    void With(PhysicsBodyHandle handle, Read&& read) noexcept {
        JPH::BodyLockRead lock(scene->System().GetBodyLockInterface(), JoltBody(handle));
        REQUIRE(lock.Succeeded());
        read(lock.GetBody());
    }

    BodyKinematics Read(PhysicsBodyHandle body) noexcept {
        auto read = scene->ReadKinematics(body);
        CHECK(read);
        return read ? read.Value() : BodyKinematics{};
    }

    usize Constraints() noexcept {
        return scene->System().GetConstraints().size();
    }

    SystemAllocator allocator;
    UniquePtr<physics::JoltPhysicsScene> scene;
    uint32 tags = 0;
};

} // namespace

// 운동 값 묶음이 Jolt body 값에 들어간다: 중력 계수, 감쇠, motion quality, 각속도
// 상한(덮어쓰기·기본), gyroscopic(회전 잠금이면 꺼짐).
TEST(Physics_BodySettingsUpdateMotionValues) {
    ValueScene scene;
    REQUIRE(scene.scene);
    BodySettings settings(scene.allocator);
    const PhysicsBodyHandle ball = scene.Sphere(settings);

    settings.enableGravity = false;
    settings.linearDamping = NewLinearDamping;
    settings.angularDamping = NewAngularDamping;
    settings.continuousCollision = true;
    settings.overrideMaxAngularVelocity = true;
    settings.maxAngularVelocity = OverrideDegrees;
    settings.gyroscopicTorque = true;
    for (const BodySettingsChange change :
         {BodySettingsChange::Gravity, BodySettingsChange::Damping,
          BodySettingsChange::ContinuousCollision, BodySettingsChange::MaxAngularVelocity,
          BodySettingsChange::GyroscopicTorque}) {
        scene.scene->UpdateBodySettings(ball, settings, change);
    }

    scene.With(ball, [](const JPH::Body& body) noexcept {
        const JPH::MotionProperties& motion = *body.GetMotionProperties();
        CHECK(motion.GetGravityFactor() == 0.0f);
        CHECK(Near(motion.GetLinearDamping(), NewLinearDamping, ValueTolerance));
        CHECK(Near(motion.GetAngularDamping(), NewAngularDamping, ValueTolerance));
        CHECK(motion.GetMotionQuality() == JPH::EMotionQuality::LinearCast);
        CHECK(Near(motion.GetMaxAngularVelocity(),
                   math::DegreesToRadians(static_cast<double>(OverrideDegrees)), ValueTolerance));
        CHECK(body.GetApplyGyroscopicForce());
    });

    settings.continuousCollision = false;
    settings.overrideMaxAngularVelocity = false;
    scene.scene->UpdateBodySettings(ball, settings, BodySettingsChange::ContinuousCollision);
    scene.scene->UpdateBodySettings(ball, settings, BodySettingsChange::MaxAngularVelocity);
    // 회전 축을 잠그면 gyroscopic은 켜 둬도 꺼진다(7.1.16절).
    settings.dofMode = DofMode::SixDOF;
    settings.lockZRotation = true;
    scene.scene->UpdateBodySettings(ball, settings, BodySettingsChange::DegreesOfFreedom);
    scene.With(ball, [&](const JPH::Body& body) noexcept {
        const JPH::MotionProperties& motion = *body.GetMotionProperties();
        CHECK(motion.GetMotionQuality() == JPH::EMotionQuality::Discrete);
        CHECK(Near(motion.GetMaxAngularVelocity(),
                   math::DegreesToRadians(scene.scene->DefaultMaxAngularVelocityDegrees()),
                   ValueTolerance));
        CHECK(!body.GetApplyGyroscopicForce());
        const uint8 rotationZ =
            static_cast<uint8>(JPH::EAllowedDOFs::RotationY); // Rocks Z → Jolt Y
        CHECK((static_cast<uint8>(motion.GetAllowedDOFs()) & rotationZ) == 0);
    });
}

// 중력을 켜면 잠든 Dynamic body를 깨운다(UE SetEnableGravity). 질량 변경은 깨우지 않는다.
TEST(Physics_BodySettingsGravityWakesMassDoesNot) {
    ValueScene scene;
    REQUIRE(scene.scene);
    BodySettings settings(scene.allocator);
    settings.startAwake = false;
    settings.enableGravity = false;
    const PhysicsBodyHandle ball = scene.Sphere(settings);
    CHECK(!scene.Read(ball).awake);
    settings.overrideMass = true;
    settings.massInKgOverride = NewMass;
    scene.scene->UpdateBodySettings(ball, settings, BodySettingsChange::Mass);
    CHECK(!scene.Read(ball).awake);
    settings.enableGravity = true;
    scene.scene->UpdateBodySettings(ball, settings, BodySettingsChange::Gravity);
    CHECK(scene.Read(ball).awake);

    // Kinematic은 중력을 켜도 깨지 않는다(UE WakeInstance — Dynamic만).
    BodySettings still(scene.allocator);
    still.enableGravity = false;
    const PhysicsBodyHandle mover = scene.Sphere(still, BodyMotion::Kinematic);
    CHECK(!scene.Read(mover).awake);
    still.enableGravity = true;
    scene.scene->UpdateBodySettings(mover, still, BodySettingsChange::Gravity);
    CHECK(!scene.Read(mover).awake);
}

// 질량 묶음은 UE 규칙으로 다시 계산한다(override 질량·관성 2/5·m·r², scale, 질량 중심 이동의
// 감싸기). Static은 질량·관성이 0이다(운동 값 없음). 자유도 방식을 바꾸면 구속을 붙이고 뗀다.
TEST(Physics_BodySettingsRebuildMassAndDof) {
    ValueScene scene;
    REQUIRE(scene.scene);
    BodySettings settings(scene.allocator);
    settings.enableGravity = false;
    const PhysicsBodyHandle ball = scene.Sphere(settings);
    const double baseMass = scene.Read(ball).mass;
    const Vec3d origin = scene.Read(ball).location;
    settings.overrideMass = true;
    settings.massInKgOverride = NewMass;
    settings.centerOfMassOffset = Vec3d(0.0, 0.0, -SphereRadius * 0.5);
    scene.scene->UpdateBodySettings(ball, settings, BodySettingsChange::Mass);
    const BodyKinematics heavy = scene.Read(ball);
    CHECK(Near(heavy.mass, static_cast<double>(NewMass), MassTolerance));
    // 질량 중심 이동은 관성 대각에 성분마다 m·d²를 더한다(UE BodyUtils.cpp:107-109 — 평행축 정리와
    // 다른 UE 단순형, 7.1.15절: Z 이동은 Izz에).
    const double sphere = 0.4 * static_cast<double>(NewMass) * SphereRadius * SphereRadius;
    const double nudge = static_cast<double>(NewMass) * SphereRadius * SphereRadius * 0.25;
    CHECK(Near(heavy.inertia.x, sphere, sphere * MassTolerance));
    CHECK(Near(heavy.inertia.z, sphere + nudge, sphere * MassTolerance));
    CHECK(Near(heavy.centerOfMass.z - heavy.location.z, -SphereRadius * 0.5, OffsetTolerance));
    // 감싸기를 바꿔도 body 원점은 그대로다(Jolt SetShapeInternal — 옛 질량 중심 기준).
    CHECK(Near((heavy.location - origin).Length(), 0.0, OffsetTolerance));

    settings.overrideMass = false;
    settings.massScale = NewMassScale;
    settings.centerOfMassOffset = Vec3d::Zero();
    scene.scene->UpdateBodySettings(ball, settings, BodySettingsChange::Mass);
    CHECK(Near(scene.Read(ball).mass, baseMass * static_cast<double>(NewMassScale),
               baseMass * MassTolerance));

    // Kinematic도 질량을 다시 계산한다(UE rigid body).
    const PhysicsBodyHandle mover = scene.Sphere(settings, BodyMotion::Kinematic);
    settings.overrideMass = true;
    scene.scene->UpdateBodySettings(mover, settings, BodySettingsChange::Mass);
    CHECK(Near(scene.Read(mover).mass, static_cast<double>(NewMass), MassTolerance));

    settings.dofMode = DofMode::CustomPlane;
    settings.customDofPlaneNormal = Vec3d(1.0, 1.0, 0.0);
    settings.lockTranslation = true;
    scene.scene->UpdateBodySettings(ball, settings, BodySettingsChange::DegreesOfFreedom);
    CHECK(scene.Constraints() == 1u);
    settings.dofMode = DofMode::None;
    scene.scene->UpdateBodySettings(ball, settings, BodySettingsChange::DegreesOfFreedom);
    CHECK(scene.Constraints() == 0u);
}

// gyroscopic은 회전 잠금을 풀면 다시 켜지고, 잠긴 body에 켜 달라고 해도 꺼진 채다.
TEST(Physics_BodySettingsGyroscopicFollowsDof) {
    ValueScene scene;
    REQUIRE(scene.scene);
    BodySettings settings(scene.allocator);
    settings.gyroscopicTorque = true;
    settings.dofMode = DofMode::SixDOF;
    settings.lockXRotation = true;
    const PhysicsBodyHandle ball = scene.Sphere(settings);
    scene.With(ball,
               [](const JPH::Body& body) noexcept { CHECK(!body.GetApplyGyroscopicForce()); });
    scene.scene->UpdateBodySettings(ball, settings, BodySettingsChange::GyroscopicTorque);
    scene.With(ball,
               [](const JPH::Body& body) noexcept { CHECK(!body.GetApplyGyroscopicForce()); });
    settings.lockXRotation = false;
    scene.scene->UpdateBodySettings(ball, settings, BodySettingsChange::DegreesOfFreedom);
    scene.With(ball, [](const JPH::Body& body) noexcept { CHECK(body.GetApplyGyroscopicForce()); });
}

// 구속이 있는 body(비스듬한 CustomPlane 이동 잠금)의 질량 중심을 옮기면 구속을 다시 붙이고(수 1)
// 원점이 그대로이며 법선 방향 잠금이 남는다.
TEST(Physics_BodySettingsCenterShiftKeepsConstraint) {
    ValueScene scene;
    REQUIRE(scene.scene);
    BodySettings settings(scene.allocator);
    settings.enableGravity = false;
    settings.linearDamping = 0.0f;
    settings.dofMode = DofMode::CustomPlane;
    settings.customDofPlaneNormal = Vec3d(1.0, 1.0, 0.0);
    settings.lockTranslation = true;
    const PhysicsBodyHandle ball = scene.Sphere(settings);
    REQUIRE(scene.Constraints() == 1u);
    const Vec3d origin = scene.Read(ball).location;
    settings.centerOfMassOffset = Vec3d(SphereRadius * 0.5, 0.0, 0.0);
    scene.scene->UpdateBodySettings(ball, settings, BodySettingsChange::Mass);
    CHECK(scene.Constraints() == 1u);
    CHECK(Near((scene.Read(ball).location - origin).Length(), 0.0, OffsetTolerance));
    scene.scene->SetBodyLinearVelocity(ball, Vec3d(PushSpeed, 0.0, PushSpeed), false);
    for (int step = 0; step < PushSteps; ++step) {
        CHECK(scene.scene->Step(StepSeconds));
    }

    const Vec3d moved = scene.Read(ball).location - origin;
    CHECK(Near((moved.x + moved.y) / math::Sqrt(2.0), 0.0, PositionTolerance));
    CHECK(moved.z > PositionTolerance);
}

} // namespace physics_body_value_setter_scene_tests_file
