#pragma once

#include <World/TickScheduler.h>

namespace rocks {

class World;
class Actor;
class Component;

// Actor의 수명 단계다. 존재(Constructed)·초기화 완료(Initialized)·게임플레이 참여(Playing)·파괴
// 대기(PendingDestroy)를 구분한다. 활성/비활성(enabled)은 이 단계와 별개 플래그다.
enum class ActorState : uint8 {
    // SpawnActorDeferred 직후. 존재하며 Component를 추가할 수 있지만 hook·Tick이 없다.
    Constructed,
    // FinishSpawning 완료. Component 등록과 OnAwake(·OnEnable)가 끝났다. 시작 대기 중이다.
    Initialized,
    // World가 Playing이고 IsReadyToStart()가 참이 된 프레임 시작에 OnStart를 마쳤다. Tick에
    // 참여한다.
    Playing,
    // DestroyActor 요청됨. Tick에서 빠지고 프레임 끝 안전 지점에서 정리된다.
    PendingDestroy
};

// Component의 수명 단계다.
enum class ComponentState : uint8 {
    // AddComponent 직후(소유 Actor가 아직 Constructed이면 FinishSpawning까지 이 상태다).
    Constructed,
    // OnRegister와 OnAwake(·OnEnable)를 마쳤다. 시작 대기 중이다.
    Initialized,
    // OnStart를 마쳤다. Tick에 참여한다.
    Playing,
    // DestroyComponent 요청됨(또는 소유 Actor가 파괴 대기).
    PendingDestroy
};

const char* ActorStateName(ActorState state) noexcept;
const char* ComponentStateName(ComponentState state) noexcept;

// World의 수명 단계다. 초기화와 게임플레이 시작을 분리한다(에디터·미리보기 World는 Initialized에서
// Tick만 하고 BeginPlay하지 않을 수 있다).
enum class WorldState : uint8 {
    Created,
    // WorldSubsystem 초기화 완료. Spawn·Tick 가능. Actor는 Initialized까지만 진행한다.
    Initialized,
    // BeginPlay 이후. 준비된 Actor가 OnStart하고 Tick에 참여한다.
    Playing,
    // Shutdown 진행 중. 새 Spawn·Tick 등록을 ShuttingDown으로 거부한다.
    TearingDown,
    // Shutdown 완료. Registry가 닫혔다.
    Destroyed
};

// Actor·Component의 Tick 설정이다. 시작(OnStart) 시점에 등록한다. 시작 전에 생성자나 OnAwake에서
// 채운다. 시작 후 변경은 SetTickEnabled만 반영된다.
//
// 사용 예(생성자에서):
//   TickSettings& tick = PrimaryTick();
//   tick.tick = true;                      // OnTick을 PrePhysics에서 매 프레임
//   tick.lateTick = true;                  // OnLateTick도 받는다
//   tick.intervalSeconds = 0.5;            // 0.5초마다(누적 delta 전달)
struct TickSettings {
    // OnTick을 받는다. 실행 단계는 phase(PrePhysics·PostPhysics·PreRender 중 하나)다.
    bool tick = false;
    TickPhase phase = TickPhase::PrePhysics;
    // OnFixedTick을 Physics 단계(고정 스텝마다)에서 받는다.
    bool fixedTick = false;
    // OnLateTick을 LateUpdate 단계에서 받는다.
    bool lateTick = false;
    // 같은 단계 안의 순서(작을수록 먼저)와 실행 간격(초, 0이면 매번)이다. 모든 hook에 공통이다.
    int32 priority = 0;
    double intervalSeconds = 0.0;
    // World pause 중에도 unscaled delta로 실행한다.
    bool tickWhenPaused = false;
};

// World 진단 수치다. 값은 호출 시점 스냅샷이다.
struct WorldStats {
    uint32 levels = 0;
    uint32 actors = 0;
    uint32 components = 0;
    uint32 pendingDestroyActors = 0;
    uint32 pendingDestroyComponents = 0;
    uint32 waitingToStart = 0;
    uint64 spawnedTotal = 0;
    uint64 destroyedTotal = 0;
    TickStats ticks;
    // 마지막 Tick의 단계별 소요 시간(ns)과 전체 소요 시간이다.
    uint64 phaseNanoseconds[TickPhaseCount] = {};
    uint64 tickNanoseconds = 0;
    // 대상이 이미 파괴되어 건너뛴 Tick 실행 수(마지막 Tick 기준)다.
    uint32 skippedTicks = 0;
};

namespace detail {
// Tick 대상 종류와 hook 번호다. World 구현 전용이다.
inline constexpr uint8 TickKindActor = 0;
inline constexpr uint8 TickKindComponent = 1;
inline constexpr uint8 TickHookUpdate = 0;
inline constexpr uint8 TickHookFixed = 1;
inline constexpr uint8 TickHookLate = 2;
inline constexpr uint32 TickHookCount = 3;
} // namespace detail

} // namespace rocks
