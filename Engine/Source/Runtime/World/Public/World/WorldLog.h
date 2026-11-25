#pragma once

#include <Core/Diagnostics/LogCategory.h>

namespace rocks {

// World 모듈의 로그 카테고리다(Docs/Design/LoggingSystem.ko.md 13절). World 수명과 GC 통계를
// 기록한다.
//
// 사용 예:
//   Log::Debug(LogWorld, "World {World} GC cycle reclaimed {Reclaimed} objects", name, count);
inline constexpr LogCategory LogWorld{"World"};

// Tick 등록·의존(선행 순서) 진단이다. "World" 아래 이름이라 SetLevel("World")로 함께 조절된다.
inline constexpr LogCategory LogWorldTick{"World.Tick"};

// 진단 코드다(Docs/Design/EngineRuntime 22.1절). 로그의 {Code} 속성으로 실어 도구가 문구가 아니라
// 코드로 찾게 한다.
// - SubsystemSkippedMissingDependency: 필수 의존이 생성되지 않아 Subsystem을 생략했다(Warning).
// - AutoTickPrerequisiteCycleSkipped: 자동 Tick 간선(부착·프레임워크)이 순환이라 걸지 않았다
//   (Warning).
// - BeginPlayDispatchContractViolated: BeginPlay dispatcher가 DispatchBeginPlay를 부르지 않았거나,
//   두 번 또는 StartPlay 밖에서 불렀다(Error·Warning).
inline constexpr char DiagSubsystemSkippedMissingDependency[] = "SubsystemSkippedMissingDependency";
// - SystemStateRecreateFailed: Component 시스템 상태 다시 만들기(MarkSystemStateDirty)가 실패해 그
// 종류가
//   없는 채로 남았다(Warning — 23.2절).
inline constexpr char DiagSystemStateRecreateFailed[] = "SystemStateRecreateFailed";
// - StaticComponentMoved: Static·Stationary SceneComponent를 게임 World에서 옮기려 해
// 거부했다(Warning —
//   23.3절, UE CheckStaticMobilityAndWarn).
inline constexpr char DiagStaticComponentMoved[] = "StaticComponentMoved";
// - StaticComponentDataChanged: 게임 World에서 초기화 뒤 Static(값에 따라 Stationary도)
//   SceneComponent의 데이터(광원 세기 등)를 바꾸려 해 거부했다(Warning — 24.2.3절, UE
//   AreDynamicDataChangesAllowed).
inline constexpr char DiagStaticComponentDataChanged[] = "StaticComponentDataChanged";
// - ComponentDataLockedInGame: 게임 World에서 초기화 뒤에는 바꿀 수 없는 데이터(광원
//   affectsWorld)를 바꾸려 해 거부했다(Warning — 24.2.3절).
inline constexpr char DiagComponentDataLockedInGame[] = "ComponentDataLockedInGame";
// - CollisionProfileEntryDropped: 충돌 profile 설정의 의미 오류 항목을 빼고 표를 만들었다(Warning —
//   CollisionData 4절, UE도 로그 뒤 지운다).
inline constexpr char DiagCollisionProfileEntryDropped[] = "CollisionProfileEntryDropped";
// - CollisionProfileMissing: PrimitiveComponent의 충돌 profile 이름이 표에 없어 사용자 지정 값으로
//   해석했다(Warning — 등록 때 한 번, CollisionData 5절).
inline constexpr char DiagCollisionProfileMissing[] = "CollisionProfileMissing";
inline constexpr char DiagAutoTickPrerequisiteCycleSkipped[] = "AutoTickPrerequisiteCycleSkipped";
inline constexpr char DiagBeginPlayDispatchContractViolated[] = "BeginPlayDispatchContractViolated";

} // namespace rocks
