// Physics 모듈 PH5b 장면 시험이다(Docs/Design/PhysicsModule 6.5절, 7.1.16절): 자유도 잠금 —
// ResolveDofLock의 축 bit(축 A), SixDOF 축 잠금, 축 평면(표지와 상관없이 잠금), Default + 프로젝트
// 기본, CustomPlane(축과 나란하면 bit, 아니면 구속 — 이동·회전), 모두 잠금(6축 구속으로 멈춤),
// Kinematic은 잠금 없음·운동 종류 전환과 구속 수명·남은 속도 거르기, teleport·scale 변경 뒤 잠금
// 유지, Static은 구속 없음. Jolt body 값을 직접 읽으므로 Monolithic 시험(EngineTests)에서만 돈다
// (BuildTargets 3.8절 BD29 시험 실행 파일 예외).
#include "../Private/JoltBodyBuild.h"
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
namespace physics_dof_scene_tests_file {

namespace {

constexpr double StepSeconds = 1.0 / 60.0;
constexpr int MoveSteps = 30;
constexpr uint32 TestMaxBodies = 64;
constexpr uint32 TestMaxBodyPairs = 256;
constexpr uint32 TestMaxContactConstraints = 256;
constexpr uint32 TestTempAllocatorBytes = 4u * 1024u * 1024u;
constexpr double SphereRadius = 20.0;
constexpr double BodySpacing = 500.0; // cm(body끼리 닿지 않는 간격)
constexpr double Speed = 100.0;       // cm/s
constexpr double Spin = 2.0;          // rad/s
// 잠그지 않은 축에 남아야 하는 각속도다(rad/s — 접촉·감쇠 없이 거의 그대로 남는다).
constexpr double KeptSpinAtLeast = Spin * 0.5;
constexpr double PositionTolerance = 0.5;        // cm(solver 오차)
constexpr double SpinTolerance = 1.0e-3;         // rad/s(축 bit 잠금 — 정확히 0)
constexpr double ConstraintSpinTolerance = 0.05; // rad/s(구속 — solver 오차)
constexpr double MovedAtLeast = 10.0;            // cm
constexpr double TeleportDistance = 100.0;       // cm
constexpr double ChangedScale = 2.0;
constexpr uint64 FakeRegistry = 1;
constexpr uint32 FakeGeneration = 1;
// 알려지지 않은 프로젝트 기본 자유도 값이다(XYPlane 다음 — 검증이 거부해야 한다).
constexpr SettingsDof UnknownSettingsDof = static_cast<SettingsDof>(9);

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

// 중력·감쇠 없이 떠 있는 설정이다(속도가 그대로 남는다).
BodySettings Floating(Allocator& allocator) noexcept {
    BodySettings settings(allocator);
    settings.enableGravity = false;
    settings.linearDamping = 0.0f;
    settings.angularDamping = 0.0f;
    return settings;
}

uint8 Bits(JPH::EAllowedDOFs dofs) noexcept {
    return static_cast<uint8>(dofs);
}

// Rocks 축 하나의 Jolt bit다(축 A — 이동 X·Y·Z, 회전 X·Y·Z 순서).
uint8 JoltBitOf(int rocksAxis) noexcept {
    constexpr JPH::EAllowedDOFs JoltAxis[6] = {
        JPH::EAllowedDOFs::TranslationZ, JPH::EAllowedDOFs::TranslationX,
        JPH::EAllowedDOFs::TranslationY, JPH::EAllowedDOFs::RotationZ,
        JPH::EAllowedDOFs::RotationX,    JPH::EAllowedDOFs::RotationY};
    return Bits(JoltAxis[rocksAxis]);
}

// 잠그는 Rocks 축 목록으로 기대 허용 bit를 만든다.
uint8 AllowedWithout(std::initializer_list<int> lockedAxes) noexcept {
    uint8 bits = Bits(JPH::EAllowedDOFs::All);
    for (const int axis : lockedAxes) {
        bits = static_cast<uint8>(bits & ~JoltBitOf(axis));
    }

    return bits;
}

struct DofScene {
    explicit DofScene(const PhysicsSettings& settings = TestSettings()) noexcept {
        auto created = physics::JoltPhysicsScene::Create(allocator, settings, nullptr);
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
        const BodyShapeElement sphere = BodyShapeElement::Sphere(SphereRadius);
        desc.geometry = Span<const BodyShapeElement>(&sphere, 1);
        desc.worldTransform.translation = Vec3d(static_cast<double>(tags) * BodySpacing, 0.0, 0.0);
        desc.motion = motion;
        desc.collision.collisionEnabled = CollisionEnabled::QueryAndPhysics;
        desc.settings = &settings;
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

    // Rocks 축 선속도(cm/s)·각속도(rad/s)를 준다(Jolt가 허용 자유도로 거른다).
    void Push(PhysicsBodyHandle body, const Vec3d& linear, const Vec3d& angular) noexcept {
        scene->System().GetBodyInterface().SetLinearAndAngularVelocity(
            JoltBody(body), physics::ToJoltLinearVelocity(linear),
            physics::ToJoltAngularVelocity(angular));
    }

    // 거르지 않고 속도를 직접 넣는다(Kinematic에 남은 속도를 흉내 낸다 — 전환 뒤 거르기 시험).
    void ForceVelocity(PhysicsBodyHandle body, const Vec3d& linear, const Vec3d& angular) noexcept {
        JPH::BodyLockWrite lock(scene->System().GetBodyLockInterface(), JoltBody(body));
        REQUIRE(lock.Succeeded());
        JPH::MotionProperties* motion = lock.GetBody().GetMotionProperties();
        motion->SetLinearVelocity(physics::ToJoltLinearVelocity(linear));
        motion->SetAngularVelocity(physics::ToJoltAngularVelocity(angular));
    }

    void Steps(int count) noexcept {
        for (int step = 0; step < count; ++step) {
            CHECK(scene->Step(static_cast<float>(StepSeconds)));
        }
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

// SixDOF 축 잠금 하나하나가 축 A로 옮긴 Jolt bit 하나만 지운다. 모두 잠그면 6축 구속이다.
TEST(Physics_DofResolveSixDofBits) {
    SystemAllocator allocator;
    for (int axis = 0; axis < 6; ++axis) {
        BodySettings settings(allocator);
        settings.dofMode = DofMode::SixDOF;
        bool* const flags[6] = {&settings.lockXTranslation, &settings.lockYTranslation,
                                &settings.lockZTranslation, &settings.lockXRotation,
                                &settings.lockYRotation,    &settings.lockZRotation};
        *flags[axis] = true;
        const physics::DofLock lock = physics::ResolveDofLock(&settings, SettingsDof::Full3D);
        CHECK(Bits(lock.allowed) == AllowedWithout({axis}));
        CHECK(!lock.constraint);
        CHECK(lock.LocksRotation() == (axis >= 3));
    }

    BodySettings all(allocator);
    all.dofMode = DofMode::SixDOF;
    all.lockXTranslation = all.lockYTranslation = all.lockZTranslation = true;
    all.lockXRotation = all.lockYRotation = all.lockZRotation = true;
    const physics::DofLock locked = physics::ResolveDofLock(&all, SettingsDof::Full3D);
    CHECK(locked.constraint);
    CHECK(locked.lockAll);
    CHECK(Bits(locked.allowed) == Bits(JPH::EAllowedDOFs::All));
    CHECK(!locked.LocksRotation());

    // 설정 없음·None은 잠금이 없다.
    CHECK(Bits(physics::ResolveDofLock(nullptr, SettingsDof::Full3D).allowed) ==
          Bits(JPH::EAllowedDOFs::All));
    BodySettings none(allocator);
    none.dofMode = DofMode::None;
    CHECK(Bits(physics::ResolveDofLock(&none, SettingsDof::YZPlane).allowed) ==
          Bits(JPH::EAllowedDOFs::All));
}

// 축 평면은 법선 이동·평면 안 두 축 회전을 잠근다(XZPlane — Y 이동, X·Z 회전). CustomPlane은 축과
// 나란하면 같은 bit로(표지마다 따로), 법선이 0이거나 표지가 없으면 잠금이 없고, 비스듬하면
// 구속이다.
TEST(Physics_DofResolvePlaneBits) {
    SystemAllocator allocator;
    BodySettings plane(allocator);
    plane.dofMode = DofMode::XZPlane;
    CHECK(Bits(physics::ResolveDofLock(&plane, SettingsDof::Full3D).allowed) ==
          AllowedWithout({1, 3, 5}));

    BodySettings custom(allocator);
    custom.dofMode = DofMode::CustomPlane;
    custom.customDofPlaneNormal = Vec3d(0.0, 0.0, -3.0);
    custom.lockTranslation = true;
    custom.lockRotation = false;
    physics::DofLock lock = physics::ResolveDofLock(&custom, SettingsDof::Full3D);
    CHECK(!lock.constraint);
    CHECK(Bits(lock.allowed) == AllowedWithout({2}));
    custom.lockTranslation = false;
    custom.lockRotation = true;
    lock = physics::ResolveDofLock(&custom, SettingsDof::Full3D);
    CHECK(!lock.constraint);
    CHECK(Bits(lock.allowed) == AllowedWithout({3, 4}));

    custom.customDofPlaneNormal = Vec3d::Zero();
    custom.lockTranslation = true;
    lock = physics::ResolveDofLock(&custom, SettingsDof::Full3D);
    CHECK(!lock.constraint);
    CHECK(Bits(lock.allowed) == Bits(JPH::EAllowedDOFs::All));

    custom.customDofPlaneNormal = Vec3d(1.0, 1.0, 0.0);
    custom.lockTranslation = false;
    custom.lockRotation = false;
    lock = physics::ResolveDofLock(&custom, SettingsDof::Full3D);
    CHECK(!lock.constraint);

    custom.lockRotation = true;
    lock = physics::ResolveDofLock(&custom, SettingsDof::Full3D);
    CHECK(lock.constraint);
    CHECK(!lock.lockAll);
    CHECK(!lock.lockNormalTranslation);
    CHECK(lock.lockPlaneRotation);
    CHECK(Near(lock.constraintNormal.x, 1.0 / math::Sqrt(2.0), SpinTolerance));
    CHECK(Near(lock.constraintNormal.z, 0.0, SpinTolerance));
    CHECK(Bits(lock.allowed) == Bits(JPH::EAllowedDOFs::All));
}

// SixDOF: X 이동을 잠그면 X·Y 속도를 준 구가 Y로만 가고, Z 회전을 잠그면 Z 둘레 각속도가 0이다.
TEST(Physics_DofSixDofLocksWorldAxes) {
    DofScene scene;
    REQUIRE(scene.scene);
    BodySettings settings = Floating(scene.allocator);
    settings.dofMode = DofMode::SixDOF;
    settings.lockXTranslation = true;
    settings.lockZRotation = true;
    const PhysicsBodyHandle ball = scene.Sphere(settings);
    const Vec3d start = scene.Read(ball).location;
    scene.Push(ball, Vec3d(Speed, Speed, 0.0), Vec3d(Spin, 0.0, Spin));
    scene.Steps(MoveSteps);
    const BodyKinematics after = scene.Read(ball);
    CHECK(Near(after.location.x, start.x, PositionTolerance));
    CHECK(after.location.y - start.y > MovedAtLeast);
    CHECK(Near(after.angularVelocityRadians.z, 0.0, SpinTolerance));
    CHECK(after.angularVelocityRadians.x > KeptSpinAtLeast);
}

// XYPlane(법선 Z): 중력이 있어도 Z에 머물고 Z 둘레 회전만 남는다(lockTranslation 거짓이어도 잠근다
// — UE 축 평면).
TEST(Physics_DofAxisPlaneIgnoresFlags) {
    DofScene scene;
    REQUIRE(scene.scene);
    BodySettings settings(scene.allocator);
    settings.dofMode = DofMode::XYPlane;
    settings.lockTranslation = false;
    settings.lockRotation = false;
    const PhysicsBodyHandle ball = scene.Sphere(settings);
    const Vec3d start = scene.Read(ball).location;
    scene.Push(ball, Vec3d(Speed, 0.0, 0.0), Vec3d(Spin, Spin, Spin));
    scene.Steps(MoveSteps);
    const BodyKinematics after = scene.Read(ball);
    CHECK(Near(after.location.z, start.z, PositionTolerance));
    CHECK(after.location.x - start.x > MovedAtLeast);
    CHECK(Near(after.angularVelocityRadians.x, 0.0, SpinTolerance));
    CHECK(Near(after.angularVelocityRadians.y, 0.0, SpinTolerance));
    CHECK(after.angularVelocityRadians.z > KeptSpinAtLeast);
}

// Default는 프로젝트 기본 자유도를 따른다(YZPlane — X 이동 잠금). 기본 Full3D면 잠금이 없다.
TEST(Physics_DofDefaultFollowsProjectSetting) {
    PhysicsSettings plane = TestSettings();
    plane.defaultDegreesOfFreedom = SettingsDof::YZPlane;
    for (const bool planar : {true, false}) {
        DofScene scene(planar ? plane : TestSettings());
        REQUIRE(scene.scene);
        BodySettings settings(scene.allocator);
        settings.enableGravity = false;
        const PhysicsBodyHandle ball = scene.Sphere(settings);
        const Vec3d start = scene.Read(ball).location;
        scene.Push(ball, Vec3d(Speed, 0.0, 0.0), Vec3d::Zero());
        scene.Steps(MoveSteps);
        const double moved = scene.Read(ball).location.x - start.x;
        if (planar) {
            CHECK(Near(moved, 0.0, PositionTolerance));
        } else {
            CHECK(moved > MovedAtLeast);
        }
    }
}

// CustomPlane(법선 (1, 1, 0)/√2, 두 표지 참): 법선 방향으로는 움직이지 않고 평면 안으로는 움직인다.
// 회전은 법선 둘레(twist)만 남고 평면 안 축(Z) 둘레는 막힌다. 표지가 둘 다 거짓이면 구속이 없다.
TEST(Physics_DofCustomPlaneUsesConstraint) {
    DofScene scene;
    REQUIRE(scene.scene);
    BodySettings settings = Floating(scene.allocator);
    settings.dofMode = DofMode::CustomPlane;
    settings.customDofPlaneNormal = Vec3d(1.0, 1.0, 0.0);
    settings.lockTranslation = true;
    settings.lockRotation = true;
    const PhysicsBodyHandle ball = scene.Sphere(settings);
    CHECK(scene.Constraints() == 1u);
    const Vec3d start = scene.Read(ball).location;
    // 법선 방향 (1, 1, 0)과 평면 안 (1, −1, 0)·(0, 0, 1)을 섞어 민다. 각속도는 법선 둘레 + Z
    // 둘레다.
    const double twist = Spin / math::Sqrt(2.0);
    scene.Push(ball, Vec3d(Speed, 0.0, Speed), Vec3d(twist, twist, Spin));
    scene.Steps(MoveSteps);
    const BodyKinematics after = scene.Read(ball);
    const Vec3d moved = after.location - start;
    const double alongNormal = (moved.x + moved.y) / math::Sqrt(2.0);
    CHECK(Near(alongNormal, 0.0, PositionTolerance));
    CHECK(moved.z > MovedAtLeast);
    const Vec3d spin = after.angularVelocityRadians;
    CHECK((spin.x + spin.y) / math::Sqrt(2.0) > KeptSpinAtLeast);
    CHECK(Near(spin.z, 0.0, ConstraintSpinTolerance));
    CHECK(Near(spin.x - spin.y, 0.0, ConstraintSpinTolerance));

    BodySettings loose(scene.allocator);
    loose.dofMode = DofMode::CustomPlane;
    loose.customDofPlaneNormal = Vec3d(1.0, 1.0, 0.0);
    loose.lockTranslation = false;
    loose.lockRotation = false;
    (void)scene.Sphere(loose);
    CHECK(scene.Constraints() == 1u);
}

// 모든 자유도를 잠근 조합은 6축 구속으로 제자리에 둔다(UE와 같다 — 중력에도 떨어지지 않는다).
TEST(Physics_DofAllLockedStaysInPlace) {
    DofScene scene;
    REQUIRE(scene.scene);
    BodySettings settings(scene.allocator);
    settings.dofMode = DofMode::SixDOF;
    settings.lockXTranslation = settings.lockYTranslation = settings.lockZTranslation = true;
    settings.lockXRotation = settings.lockYRotation = settings.lockZRotation = true;
    const PhysicsBodyHandle ball = scene.Sphere(settings);
    CHECK(scene.Constraints() == 1u);
    const Vec3d start = scene.Read(ball).location;
    scene.Push(ball, Vec3d(Speed, Speed, 0.0), Vec3d(Spin, Spin, Spin));
    scene.Steps(MoveSteps);
    const BodyKinematics after = scene.Read(ball);
    CHECK(Near(after.location.x, start.x, PositionTolerance));
    CHECK(Near(after.location.y, start.y, PositionTolerance));
    CHECK(Near(after.location.z, start.z, PositionTolerance));
    CHECK(Near(after.angularVelocityRadians.Length(), 0.0, ConstraintSpinTolerance));
}

// Kinematic은 잠금이 없고(잠긴 축으로 옮겨진다) Dynamic으로 바꾸면 잠긴다. CustomPlane 구속은
// Dynamic일 때만 있고(1 → 0 → 1) body를 지우면 없어진다.
TEST(Physics_DofFollowsMotionSwitches) {
    DofScene scene;
    REQUIRE(scene.scene);
    BodySettings planar(scene.allocator);
    planar.dofMode = DofMode::XYPlane;
    const PhysicsBodyHandle kinematic = scene.Sphere(planar, BodyMotion::Kinematic);
    Transform target;
    target.translation = scene.Read(kinematic).location + Vec3d(0.0, 0.0, MovedAtLeast * 2.0);
    scene.scene->SetBodyTransform(kinematic, target, TeleportMode::None);
    scene.Steps(1);
    CHECK(Near(scene.Read(kinematic).location.z, target.translation.z, PositionTolerance));

    scene.scene->SetBodyMotion(kinematic, BodyMotion::Dynamic, true);
    const double height = scene.Read(kinematic).location.z;
    scene.Steps(MoveSteps);
    CHECK(Near(scene.Read(kinematic).location.z, height, PositionTolerance));

    BodySettings custom(scene.allocator);
    custom.dofMode = DofMode::CustomPlane;
    custom.customDofPlaneNormal = Vec3d(1.0, 1.0, 0.0);
    custom.lockTranslation = true;
    const PhysicsBodyHandle body = scene.Sphere(custom);
    CHECK(scene.Constraints() == 1u);
    scene.scene->SetBodyMotion(body, BodyMotion::Kinematic, false);
    CHECK(scene.Constraints() == 0u);
    scene.scene->SetBodyMotion(body, BodyMotion::Dynamic, true);
    CHECK(scene.Constraints() == 1u);
    scene.scene->DestroyBody(body);
    CHECK(scene.Constraints() == 0u);
}

// Kinematic에 남은 속도는 Dynamic 전환 때 잠긴 축이 걸러진다(XYPlane — Z 이동, X·Y 회전).
TEST(Physics_DofSwitchClearsLockedVelocity) {
    DofScene scene;
    REQUIRE(scene.scene);
    BodySettings planar = Floating(scene.allocator);
    planar.dofMode = DofMode::XYPlane;
    const PhysicsBodyHandle body = scene.Sphere(planar, BodyMotion::Kinematic);
    scene.ForceVelocity(body, Vec3d(Speed, 0.0, Speed), Vec3d(Spin, Spin, Spin));
    scene.scene->SetBodyMotion(body, BodyMotion::Dynamic, true);
    const BodyKinematics after = scene.Read(body);
    CHECK(Near(after.linearVelocity.z, 0.0, SpinTolerance));
    CHECK(Near(after.linearVelocity.x, Speed, PositionTolerance));
    CHECK(Near(after.angularVelocityRadians.x, 0.0, SpinTolerance));
    CHECK(Near(after.angularVelocityRadians.y, 0.0, SpinTolerance));
    CHECK(Near(after.angularVelocityRadians.z, Spin, SpinTolerance));
}

// teleport한 CustomPlane body는 새 자리를 기준으로 잠긴다(법선 방향으로 옮겨도 되돌아가지 않는다).
// scale을 바꿔도 축 bit 잠금은 남는다.
TEST(Physics_DofKeepsLockAfterTeleportAndScale) {
    DofScene scene;
    REQUIRE(scene.scene);
    BodySettings custom = Floating(scene.allocator);
    custom.dofMode = DofMode::CustomPlane;
    custom.customDofPlaneNormal = Vec3d(1.0, 1.0, 0.0);
    custom.lockTranslation = true;
    const PhysicsBodyHandle body = scene.Sphere(custom);
    Transform moved;
    moved.translation = scene.Read(body).location + Vec3d(TeleportDistance, TeleportDistance, 0.0);
    scene.scene->SetBodyTransform(body, moved, TeleportMode::TeleportPhysics);
    CHECK(scene.Constraints() == 1u);
    scene.Push(body, Vec3d(Speed, 0.0, Speed), Vec3d::Zero());
    scene.Steps(MoveSteps);
    const Vec3d offset = scene.Read(body).location - moved.translation;
    CHECK(Near((offset.x + offset.y) / math::Sqrt(2.0), 0.0, PositionTolerance));
    CHECK(offset.z > MovedAtLeast);

    BodySettings sixDof = Floating(scene.allocator);
    sixDof.dofMode = DofMode::SixDOF;
    sixDof.lockXTranslation = true;
    const PhysicsBodyHandle ball = scene.Sphere(sixDof);
    Transform scaled;
    scaled.translation = scene.Read(ball).location;
    scaled.scale = Vec3d(ChangedScale, ChangedScale, ChangedScale);
    scene.scene->SetBodyTransform(ball, scaled, TeleportMode::TeleportPhysics);
    scene.Push(ball, Vec3d(Speed, Speed, 0.0), Vec3d::Zero());
    scene.Steps(MoveSteps);
    CHECK(Near(scene.Read(ball).location.x, scaled.translation.x, PositionTolerance));
}

// Static은 자유도 설정이 있어도 잠금·구속이 없다.
TEST(Physics_DofStaticHasNoConstraint) {
    DofScene scene;
    REQUIRE(scene.scene);
    BodySettings custom(scene.allocator);
    custom.dofMode = DofMode::CustomPlane;
    custom.customDofPlaneNormal = Vec3d(1.0, 1.0, 0.0);
    custom.lockTranslation = true;
    custom.lockRotation = true;
    const PhysicsBodyHandle body = scene.Sphere(custom, BodyMotion::Static);
    CHECK(scene.Constraints() == 0u);
    Transform moved;
    moved.translation = scene.Read(body).location + Vec3d(TeleportDistance, 0.0, 0.0);
    scene.scene->SetBodyTransform(body, moved, TeleportMode::TeleportPhysics);
    scene.Steps(1);
    CHECK(scene.Constraints() == 0u);
    CHECK(Near(scene.Read(body).location.x, moved.translation.x, PositionTolerance));
}

// 프로젝트 기본 자유도 검증이다.
TEST(Physics_SettingsRejectUnknownDefaultDof) {
    PhysicsSettings settings = TestSettings();
    CHECK(PhysicsSettings::Validate(settings));
    settings.defaultDegreesOfFreedom = UnknownSettingsDof;
    CHECK(PhysicsSettings::Validate(settings).Error() == ErrorCode::InvalidArgument);
}

} // namespace physics_dof_scene_tests_file
