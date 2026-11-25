#pragma once

#include <Core/Result.h>
#include <Meta/TypeSchema.h>
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

// Actor·Component가 정리되는 이유다(UE EEndPlayReason — Docs/Design/EngineRuntime 17.2절 ERD13).
// 정리를 시작한 쪽이 정하고, 파괴 대기 중에 더 큰 정리(Level·World)가 와도 먼저 정한 이유를
// 유지한다.
enum class EndPlayReason : uint8 {
    // DestroyActor·DestroyComponent·LifeSpan 만료·우회 파괴 복구.
    Destroyed,
    // World 전환(GameInstance OpenWorld)으로 이전 World를 끝낸다.
    LevelTransition,
    // DestroyLevel(Level 내림·스트리밍 아웃).
    RemovedFromWorld,
    // 앱 종료로 World를 끝낸다.
    Quit,
    // Tryout 종료(UE EndPlayInEditor).
    TryoutEnded
};

const char* EndPlayReasonName(EndPlayReason reason) noexcept;

// World 종류다(UE EWorldType — Game·Editor·PIE·EditorPreview·GamePreview·Inactive, Preview가
// EditorPreview·GamePreview를 합친다). Subsystem 생성 판정(16.3절 ERD9)과 GameDirector
// 생성(19.3절)에 쓴다. Docs/Design/EngineRuntime 16.4절.
enum class WorldKind : uint8 {
    // 독립 실행·서버의 게임 World다.
    Game,
    // 편집 World다(BeginPlay하지 않는다 — BeginPlay는 InvalidArgument).
    Editor,
    // 편집 World를 복제해 실행하는 World다(UE PIE).
    Tryout,
    // 에셋 미리보기·썸네일 World다.
    Preview,
    // 적재됐지만 실행하지 않는 World다(BeginPlay는 InvalidArgument, Engine이 Tick하지 않는다).
    Inactive
};

const char* WorldKindName(WorldKind kind) noexcept;
const char* ComponentStateName(ComponentState state) noexcept;

// World의 수명 단계다. 초기화와 게임플레이 시작을 분리한다(에디터·미리보기 World는 Initialized에서
// Tick만 하고 BeginPlay하지 않을 수 있다).
enum class WorldState : uint8 {
    Created,
    // WorldSubsystem 초기화 완료. Spawn·Tick 가능. Actor는 Initialized까지만 진행한다.
    Initialized,
    // BeginPlay 이후. 준비된 Actor가 OnStart하고 Tick에 참여한다.
    Playing,
    // EndPlay 진행 중. 모든 Actor를 정리하고 OnWorldEndPlay를 부른 뒤 Initialized로 돌아간다.
    // Spawn은 되지만 OnStart는 하지 않는다(17.6절).
    EndingPlay,
    // Shutdown 진행 중. 새 Spawn·Tick 등록을 ShuttingDown으로 거부한다.
    TearingDown,
    // Shutdown 완료. Registry가 닫혔다.
    Destroyed
};

// Actor·Component의 Tick 설정이다. 시작(OnStart) 시점에 등록한다. 어느 hook을
// 받을지(tick·fixedTick· lateTick)는 **생성자에서** 정한다 — 의존 선언·부착 간선이 선언 시점의 이
// 값으로 hook 유무를 판정하므로(UE bCanEverTick과 같다) OnAwake에서 켠 hook은 그 전에 건 의존·부착
// 간선을 받지 못한다. phase·priority·interval은 시작 전 OnAwake에서 바꿔도 되고, 시작 후에는
// SetTickEnabled와 World::SetTickPhase·SetTickPriority·SetTickInterval로 바꾼다.
//
// 사용 예(생성자에서):
//   TickSettings& tick = PrimaryTick();
//   tick.tick = true;                      // OnTick을 PrePhysics에서 매 프레임
//   tick.lateTick = true;                  // OnLateTick도 받는다
//   tick.intervalSeconds = 0.5;            // 0.5초마다(누적 delta 전달)
// 잘못된 설정(OnTick 단계가 Physics·LateUpdate·범위 밖, 음수·비유한 interval)은 FinishSpawning·
// AddComponent가 InvalidArgument로 거부한다(IsValidTickSettings). 리플렉션 struct다(Actor·
// Component의 tickSettings 필드 — 생성자 값은 클래스 기본값 레코드에 들어가고 인스턴스 값은
// 저장된다, RuntimeClassMetadata 'A5c 지문 연결'). 경로 쓰기·로드로 들어온 값은 Validate가 같은
// 규칙으로 거부한다.
ROCKS_STRUCT()
struct TickSettings {
    ROCKS_GENERATED_BODY()

    // 리플렉션 검증 hook이다(IsValidTickSettings가 거짓이면 InvalidArgument — 할당·전역 상태
    // 없음). Modular 대상의 다른 모듈(시험)도 부르므로 export한다.
    ROCKS_WORLD_API static Result<void> Validate(const TickSettings& settings) noexcept;

    // OnTick을 받는다. 실행 단계는 phase(PrePhysics·PostPhysics·PreRender 중 하나)다.
    ROCKS_PROPERTY() bool tick = false;
    ROCKS_PROPERTY() TickPhase phase = TickPhase::PrePhysics;
    // OnFixedTick을 Physics 단계(고정 스텝마다)에서 받는다.
    ROCKS_PROPERTY() bool fixedTick = false;
    // OnLateTick을 LateUpdate 단계에서 받는다.
    ROCKS_PROPERTY() bool lateTick = false;
    // 같은 단계 안의 순서(작을수록 먼저)와 실행 간격(초, 0이면 매번)이다. 모든 hook에 공통이다.
    ROCKS_PROPERTY() int32 priority = 0;
    ROCKS_PROPERTY() double intervalSeconds = 0.0;
    // World pause 중에도 unscaled delta로 실행한다.
    ROCKS_PROPERTY() bool tickWhenPaused = false;
};

// 등록할 수 있는 Tick 설정인지다(TickSettings 설명의 규칙).
bool IsValidTickSettings(const TickSettings& settings) noexcept;

// Tick hook 종류다. Update는 OnTick(TickSettings::phase), Fixed는 OnFixedTick(Physics), Late는
// OnLateTick(LateUpdate)이다.
enum class TickHook : uint8 {
    Update,
    Fixed,
    Late
};

// Tick 의존의 한쪽이다(이 World의 Actor·Component와 그 hook).
struct TickPoint {
    ObjectHandle object;
    TickHook hook = TickHook::Update;

    bool operator==(const TickPoint&) const noexcept = default;
};

// Tick 의존의 출처다. 같은 간선을 여러 출처가 걸 수 있고, 모든 출처가 뗄 때 간선이 없어진다. 엔진이
// 거는 자동 간선(Attachment·Framework)은 순환을 만들면 걸지 않고 Warning을 남긴다(사용자 간선은
// CycleDetected로 거부 — Docs/Design/EngineRuntime 15.3절 ERD2).
enum class TickEdgeSource : uint8 {
    // AddTickPrerequisite를 부른 사용자 코드다.
    User = 1 << 0,
    // SceneComponent 부착(자식 hook이 부모의 같은 hook 뒤 — UE AttachToComponent).
    Attachment = 1 << 1,
    // 게임 프레임워크(Controller → Pawn 등, Gameplay 모듈).
    Framework = 1 << 2
};

// 사용자 등록이 아닌 엔진 단계의 자리다(Docs/Design/EngineRuntime 15.4절 ERD7). 열거 순서가 실행
// 순서다: Physics 단계 끝(고정 스텝 뒤, 프레임에 한 번) PhysicsResults → SkeletalPhysicsFinalize,
// LateUpdate 단계 시작 Timers → TickableSubsystems → CameraUpdate.
enum class EngineStage : uint8 {
    PhysicsResults,
    SkeletalPhysicsFinalize,
    Timers,
    TickableSubsystems,
    CameraUpdate,
    Count
};

inline constexpr uint32 EngineStageCount = static_cast<uint32>(EngineStage::Count);

// "PhysicsResults" 같은 엔진 단계 이름이다. 범위 밖이면 "Unknown"이다.
const char* EngineStageName(EngineStage stage) noexcept;

// 엔진 단계 콜백이다. context.phase는 그 자리의 단계(Physics·LateUpdate), delta·pause 정보는 그
// 프레임의 값이다(pause 정책은 콜백이 context.paused로 정한다 — CameraUpdate는 pause 중 부르지
// 않는다). 실패는 그 프레임 Tick의 실패로 돌아가고 WorldStats::engineStageFailures에 센다.
using EngineStageFunction = Result<void> (*)(World& world, const TickContext& context,
                                             void* user) noexcept;

// Component가 엔진 시스템에 두는 상태의 종류다(Docs/Design/EngineRuntime 23.2절 — UE render·physics
// state). 등록 직후 이 순서로 만들고 해제 직전 역순으로 지운다.
enum class SystemStateKind : uint8 {
    Render,
    Physics,
    Audio,
    Count
};

inline constexpr uint32 SystemStateKindCount = static_cast<uint32>(SystemStateKind::Count);

ROCKS_WORLD_API const char* SystemStateKindName(SystemStateKind kind) noexcept;

// Component 타입 규칙이다(23.2절 CR6 — Unity DisallowMultipleComponent·RequireComponent 대응,
// UE에는 없다). AddComponent가 등록 전에 소유 Actor의 기존 Component(파괴 대기 제외)에 대 본다.
// 타입 비교는 파생을 포함한다. Span이 가리키는 배열은 정적 저장 기간이어야 한다.
//
// 사용 예:
//   ComponentTypeRules TypeRules() const noexcept override {
//       static const TypeId needs[] = {ObjectTypeIdOf<SceneComponent>()};
//       ComponentTypeRules rules;
//       rules.unique = true;
//       rules.required = needs;
//       return rules;
//   }
// ComponentTypeRules::required에 둘 수 있는 타입 수 상한이다(넘으면 AddComponent가
// InvalidArgument).
inline constexpr usize MaxRequiredComponentTypes = 16;

struct ComponentTypeRules {
    // Actor당 하나다(같은 타입·파생이 이미 있으면 AlreadyExists).
    bool unique = false;
    // 먼저 있어야 하는 타입이다(없으면 DependencyFailed — 자동으로 더하지 않는다).
    Span<const TypeId> required;
    // 함께 있을 수 없는 타입이다(어느 쪽 규칙이든 걸리면 InvalidArgument).
    Span<const TypeId> excluded;
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
    // 엔진 단계 콜백이 실패한 누적 수다.
    uint32 engineStageFailures = 0;
    // 엔진 단계 자리마다 마지막 실행의 소요 시간(ns)이다(콜백이 없으면 0).
    uint64 engineStageNanoseconds[static_cast<uint32>(EngineStage::Count)] = {};
    // LifeSpan이 걸려 만료를 기다리는 Actor 수(시작 전에 건 것 포함)다.
    uint32 pendingLifeSpans = 0;
    // Registry를 직접 조작해 우회 파괴된 Actor·Component·Level을 World가 발견해(Tick 실행·파괴
    // 요청 목록) 정리로 돌린 누적 수다.
    uint32 repairedBypasses = 0;
    // 지금 살아 있는 Borrow 수다. Tick 밖에서 0이 아니면 외부 코드가 Borrow를 쥐고 있어 GC가
    // 미뤄진다.
    uint32 externalBorrows = 0;
};

namespace detail {
// Tick 대상 종류와 hook 번호다. World 구현 전용이다(hook 번호는 TickHook 값과 같다).
inline constexpr uint8 TickKindActor = 0;
inline constexpr uint8 TickKindComponent = 1;
inline constexpr uint8 TickHookUpdate = 0;
inline constexpr uint8 TickHookFixed = 1;
inline constexpr uint8 TickHookLate = 2;
inline constexpr uint32 TickHookCount = 3;
} // namespace detail

// 레벨 편입 결과다(World::AdoptLevel — Docs/Design/EngineRuntime 29절).
struct LevelAdoption {
    ObjectHandle level;
    // 장부에 넣은 Actor 수다(실패한 Actor 포함).
    uint32 actors = 0;
    // FinishSpawning이 실패해 정리된 Actor 수다.
    uint32 failedActors = 0;
    // 정리한 null·root 밖·잘못된 부착 부모 원소 수다.
    uint32 droppedEntries = 0;
};

} // namespace rocks
