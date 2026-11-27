#pragma once

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

ROCKS_STRUCT(Settings(Branch = "Engine", Section = "Physics"))
struct PhysicsSettings {
    ROCKS_GENERATED_BODY()

    // 리플렉션 검증 hook이다: 용량이 0이거나 중력이 유한하지 않거나 hit 문턱이 음수·비유한이면
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
};

// Physics 모듈의 리플렉션 타입(PhysicsSettings)을 registration에 등록한다
// (rocks::generated::Physics::RegisterModuleTypes). 같은 registration에 다시 부르면 멱등이다.
ROCKS_PHYSICS_API Result<void> RegisterPhysicsTypes(ModuleRegistration& registration) noexcept;

} // namespace rocks
