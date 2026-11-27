#pragma once

#include <Core/Containers/Array.h>
#include <Core/Math/Vector3.h>
#include <Meta/Annotations.h>
#include <Collision/CollisionSettings.h>
#include <Collision/CollisionTypes.h>

// body 값이다(Docs/Design/CollisionData.ko.md 6절). UE FBodyInstance·FBodyInstanceCore의 영속 필드
// 중 Jolt에서 의미가 있는 것이다(E/Classes/PhysicsEngine/BodyInstance.h,
// PC/Public/BodyInstanceCore.h — 기본값은 UE 생성자와 같다). 단위는 Rocks 공개 물리 API
// 규칙(cm·kg·도)이다. 실행 중 body는 물리 모듈(CR16)이 만들고 이 값을 읽는다. PrimitiveComponent의
// 영속 필드 bodySettings로 둔다(CD2).
namespace rocks {

// 자유도 제한 방식이다(UE EDOFMode — BodyInstance.h:39).
// - Default: 프로젝트 물리 설정의 기본 방식을 따른다.
// - SixDOF: 축별 잠금(lock*Translation·lock*Rotation)을 쓴다.
// - YZPlane·XZPlane·XYPlane: 그 평면 안에서만 움직인다(법선 이동·평면 안 두 축 회전을 늘 잠근다 —
//   lockTranslation·lockRotation 표지는 보지 않는다, UE와 같다).
// - CustomPlane: customDofPlaneNormal에 수직인 평면이다(lockTranslation이면 법선 이동,
//   lockRotation이면 평면 안 두 축 회전을 잠근다).
// - None: 제한하지 않는다.
ROCKS_ENUM()
enum class DofMode : uint8 {
    Default,
    SixDOF,
    YZPlane,
    XZPlane,
    XYPlane,
    CustomPlane,
    None
};

// 캐릭터가 걸을 수 있는 경사를 바꾸는 방식이다(UE EWalkableSlopeBehavior — EngineTypes.h:3694).
// Unwalkable이면 각도를 보지 않는다.
ROCKS_ENUM()
enum class WalkableSlopeBehavior : uint8 {
    Default,
    Increase,
    Decrease,
    Unwalkable
};

// 걷기 경사 덮어쓰기다(UE FWalkableSlopeOverride — EngineTypes.h:3724). 물리가 아니라 캐릭터
// 이동(CM13)이 읽는다.
ROCKS_STRUCT()
struct WalkableSlope {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() WalkableSlopeBehavior behavior = WalkableSlopeBehavior::Default;
    // 경사 각도(도)다. Increase·Decrease일 때 쓴다.
    ROCKS_PROPERTY() float angleDegrees = 0.0f;

    friend constexpr bool operator==(const WalkableSlope&, const WalkableSlope&) noexcept = default;
};

// UE 물리 설정의 기본 최대 각속도(도/s)다(UPhysicsSettingsCore::MaxAngularVelocity —
// PC/Private/PhysicsSettingsCore.cpp:43, 초당 10바퀴).
inline constexpr float DefaultMaxAngularVelocityDegrees = 3600.0f;

// UE body 기본 질량 덮어쓰기 값(kg)이다(FBodyInstance::MassInKgOverride — BodyInstance.cpp:399).
inline constexpr float DefaultMassOverrideKilograms = 100.0f;

// UE body 기본 선형 감쇠다(BodyInstance.cpp:401).
inline constexpr float DefaultLinearDamping = 0.01f;

// UE 기본 solver 반복 횟수다(BodyInstance.cpp:360-361).
inline constexpr uint8 DefaultPositionIterations = 8;
inline constexpr uint8 DefaultVelocityIterations = 2;

// body 하나의 값이다. 필드 뜻과 UE·Jolt 대응은 Docs/Design/CollisionData.ko.md 6.1–6.3절 표다.
ROCKS_STRUCT()
struct BodySettings {
    ROCKS_GENERATED_BODY()

    explicit BodySettings(Allocator& allocator) noexcept : customResponses(allocator) {
    }

    // 사용자 지정 충돌(profile 이름이 "Custom"이거나 표에 없을 때만 쓰인다 — 6.1절).
    ROCKS_PROPERTY() CollisionEnabled collisionEnabled = CollisionEnabled::QueryAndPhysics;
    ROCKS_PROPERTY() CollisionChannel objectType = CollisionChannel::WorldStatic;
    // 기본 응답 묶음과 다른 칸만 담는다(채널 표시 이름).
    ROCKS_PROPERTY() Array<ChannelResponse> customResponses;

    // 시뮬레이션·질량(6.2절).
    ROCKS_PROPERTY() bool simulatePhysics = false;
    ROCKS_PROPERTY() bool startAwake = true;
    ROCKS_PROPERTY() bool enableGravity = true;
    ROCKS_PROPERTY() bool overrideMass = false;
    ROCKS_PROPERTY() float massInKgOverride = DefaultMassOverrideKilograms;
    ROCKS_PROPERTY() float massScale = 1.0f;
    // 계산한 질량 중심에 더하는 위치(cm, body 공간)다.
    ROCKS_PROPERTY() Vec3d centerOfMassOffset = Vec3d(0.0, 0.0, 0.0);
    // 계산한 관성 주축값의 축별 배율이다.
    ROCKS_PROPERTY() Vec3d inertiaTensorScale = Vec3d(1.0, 1.0, 1.0);
    ROCKS_PROPERTY() bool updateMassWhenScaleChanges = true;
    // 감쇠(1/s)다.
    ROCKS_PROPERTY() float linearDamping = DefaultLinearDamping;
    ROCKS_PROPERTY() float angularDamping = 0.0f;
    ROCKS_PROPERTY() bool gyroscopicTorque = false;
    ROCKS_PROPERTY() bool overrideMaxAngularVelocity = false;
    // 최대 각속도(도/s)다. overrideMaxAngularVelocity가 거짓이면 프로젝트 물리 설정 값을 쓴다.
    ROCKS_PROPERTY() float maxAngularVelocity = DefaultMaxAngularVelocityDegrees;
    ROCKS_PROPERTY() bool continuousCollision = false;
    ROCKS_PROPERTY() bool smoothEdgeCollisions = false;
    ROCKS_PROPERTY() bool overrideSolverIterations = false;
    ROCKS_PROPERTY() uint8 positionIterations = DefaultPositionIterations;
    ROCKS_PROPERTY() uint8 velocityIterations = DefaultVelocityIterations;
    // 시뮬레이션하지 않는 body를 부모 body에 합친다.
    ROCKS_PROPERTY() bool autoWeld = false;

    // 이벤트·자유도·보행(6.3절).
    // 접촉(hit) 이벤트를 낸다.
    ROCKS_PROPERTY() bool notifyRigidBodyCollision = false;
    ROCKS_PROPERTY() bool generateWakeEvents = false;
    ROCKS_PROPERTY() DofMode dofMode = DofMode::Default;
    ROCKS_PROPERTY() bool lockTranslation = true;
    ROCKS_PROPERTY() bool lockRotation = true;
    ROCKS_PROPERTY() bool lockXTranslation = false;
    ROCKS_PROPERTY() bool lockYTranslation = false;
    ROCKS_PROPERTY() bool lockZTranslation = false;
    ROCKS_PROPERTY() bool lockXRotation = false;
    ROCKS_PROPERTY() bool lockYRotation = false;
    ROCKS_PROPERTY() bool lockZRotation = false;
    ROCKS_PROPERTY() Vec3d customDofPlaneNormal = Vec3d(0.0, 0.0, 0.0);
    ROCKS_PROPERTY() bool overrideWalkableSlope = false;
    ROCKS_PROPERTY() WalkableSlope walkableSlope;
};

} // namespace rocks
