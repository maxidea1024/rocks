#pragma once

#include <Collision/BodySettings.h>
#include <Core/Result.h>
#include <Meta/Annotations.h>
#include <Object/ModuleRegistration.h>

// 물리 설정이다(Docs/Design/PhysicsModule 6.8절 — UE UPhysicsSettings·UPhysicsSettingsCore). branch
// Engine, 섹션 Physics. PH1은 장면 생성과 step에 쓰는 칸만 두고, 나머지 칸(속도 상한·반발 문턱·hit
// 문턱·결합 방식 등)은 쓰는 단계에서 더한다(7.1.1절). 프로젝트는 자기 Config/Engine.json에서
// 바꾼다:
//   "Physics": { "defaultGravityZ": -490.0, "maxBodies": 16384 }
namespace rocks {

// Jolt 임시 할당기 기본 크기(Jolt 예제 HelloWorld와 같다 — 측정 후 조정, PhysicsModule 6.7절).
inline constexpr uint32 DefaultPhysicsTempAllocatorBytes = 10u * 1024u * 1024u;

// 장면 용량 기본값(제안값 — 측정 후 조정, PhysicsModule 6.8절). Jolt PhysicsSystem::Init 인자다.
inline constexpr uint32 DefaultPhysicsMaxBodies = 65536;
inline constexpr uint32 DefaultPhysicsMaxBodyPairs = 65536;
inline constexpr uint32 DefaultPhysicsMaxContactConstraints = 10240;

// UE DefaultGravityZ(cm/s² — PhysicsSettingsCore.cpp:29)다.
inline constexpr float DefaultPhysicsGravityZ = -980.0f;

// 사실상 없는 선속도 상한(cm/s)이다: √FLT_MAX(제곱해도 float에서 넘치지 않는 가장 큰 값 — UE
// BodyInstance.cpp:1331·ChaosEngineInterface.cpp:693-701, PhysicsModule PMD18).
inline constexpr float UnlimitedLinearVelocity = 1.8446743e19f;

// UE Chaos 반발 문턱(cm/s² — PBDRigidsEvolutionGBF.h:70 DefaultRestitutionThreshold, 판정
// PBDCollisionContainerSolver.cpp:89)이다: 닫히는 속도가 문턱 × step dt(60 Hz면 약 16.7 cm/s)보다
// 작은 접촉은 반발하지 않는다. UE 설정 BounceThresholdVelocity(200 cm/s)는 UE 5.8에서 읽히지
// 않는다(PhysX 시절 칸 — PhysicsModule 7.1.15절).
inline constexpr float DefaultPhysicsRestitutionThreshold = 1000.0f;

// UE Chaos 충돌 margin 비율·상한(cm — PBDRigidsEvolutionGBF.h:65-66, 상자 둥근 모서리 반경 =
// min(2 × 가장 작은 반폭 × 비율, 상한) — PhysicsModule 7.1.15절)이다.
inline constexpr float DefaultPhysicsCollisionMarginFraction = 0.05f;
inline constexpr float DefaultPhysicsCollisionMarginMax = 10.0f;

// 접촉 마찰·반발의 두 body 값 결합 방식이다(UE EFrictionCombineMode — PhysicsSettingsEnums.h:14-19,
// PhysicsModule 6.6절).
ROCKS_ENUM()
enum class CombineMode : uint8 {
    Average,
    Min,
    Multiply,
    Max
};

ROCKS_STRUCT(Settings(Branch = "Engine", Section = "Physics"))
struct PhysicsSettings {
    ROCKS_GENERATED_BODY()

    // 리플렉션 검증 hook이다: 용량이 0이거나, 중력이 유한하지 않거나, hit 문턱·반발 문턱·margin
    // 칸이 음수·비유한이거나, 속도 상한이 0 이하·비유한이거나, 결합 방식을 모르면
    // InvalidArgument다.
    ROCKS_PHYSICS_API static Result<void> Validate(const PhysicsSettings& settings) noexcept;

    // 월드 Z축 중력 가속도(cm/s²)다. 음수가 아래(−Z)다.
    ROCKS_PROPERTY() float defaultGravityZ = DefaultPhysicsGravityZ;
    // 장면 하나가 담는 body 수 상한이다(넘으면 body 생성 실패).
    ROCKS_PROPERTY() uint32 maxBodies = DefaultPhysicsMaxBodies;
    // broadphase가 한 step에 찾는 body 쌍 상한이다(넘으면 그 쌍의 충돌을 놓치고 경고).
    ROCKS_PROPERTY() uint32 maxBodyPairs = DefaultPhysicsMaxBodyPairs;
    // 한 step의 접촉 constraint 상한이다(넘으면 그 접촉을 놓치고 경고).
    ROCKS_PROPERTY() uint32 maxContactConstraints = DefaultPhysicsMaxContactConstraints;
    // World마다 미리 잡는 Jolt 임시 할당기 크기(byte)다.
    ROCKS_PROPERTY() uint32 tempAllocatorBytes = DefaultPhysicsTempAllocatorBytes;
    // 시뮬레이션 접촉 hit 문턱(cm/s — UE MinDeltaVelocityForHitEvents,
    // PhysicsModule 6.4절·7.1.12절) 이다. 0보다 크면 두 body의 선속도 변화가 성분마다 모두 이 값
    // 이하인 접촉은 hit를 내지 않는다. 0이면 모두 낸다.
    ROCKS_PROPERTY() float minDeltaVelocityForHitEvents = 0.0f;
    // body 각속도 상한(도/s — UE MaxAngularVelocity)이다. body가 덮어쓰지 않으면 이 값이다.
    ROCKS_PROPERTY() float maxAngularVelocity = DefaultMaxAngularVelocityDegrees;
    // body 선속도 상한(cm/s)이다. 기본은 사실상 없음(UnlimitedLinearVelocity — UE와 같다).
    ROCKS_PROPERTY() float maxLinearVelocity = UnlimitedLinearVelocity;
    // 반발 문턱(cm/s² — UE Chaos RestitutionThreshold, CVar p.CollisionRestitutionThreshold)이다.
    // step마다 × dt로 속도 문턱이 된다.
    ROCKS_PROPERTY() float restitutionThreshold = DefaultPhysicsRestitutionThreshold;
    // 마찰·반발 결합 방식이다(UE 기본 Average·Average).
    ROCKS_PROPERTY() CombineMode frictionCombineMode = CombineMode::Average;
    ROCKS_PROPERTY() CombineMode restitutionCombineMode = CombineMode::Average;
    // 상자 충돌 margin(둥근 모서리) 비율과 상한(cm)이다(UE SolverOptions.CollisionMarginFraction·
    // CollisionMarginMax).
    ROCKS_PROPERTY() float collisionMarginFraction = DefaultPhysicsCollisionMarginFraction;
    ROCKS_PROPERTY() float collisionMarginMax = DefaultPhysicsCollisionMarginMax;
};

// Physics 모듈의 리플렉션 타입(PhysicsSettings)을 registration에 등록한다
// (rocks::generated::Physics::RegisterModuleTypes). 같은 registration에 다시 부르면 멱등이다.
ROCKS_PHYSICS_API Result<void> RegisterPhysicsTypes(ModuleRegistration& registration) noexcept;

} // namespace rocks
