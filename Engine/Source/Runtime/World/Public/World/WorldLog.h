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
inline constexpr char DiagAutoTickPrerequisiteCycleSkipped[] = "AutoTickPrerequisiteCycleSkipped";
inline constexpr char DiagBeginPlayDispatchContractViolated[] = "BeginPlayDispatchContractViolated";

} // namespace rocks
