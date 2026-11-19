#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/UniquePtr.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Task/WorkerScheduler.h>
#include <World/Subsystem.h>
#include <World/World.h>

namespace rocks {

namespace config {
class ConfigContext;
} // namespace config

class Engine;

// 종료 때 최종 GC와 정리 drain을 반복하는 최대 횟수다. 넘으면 닫히지 않은 것으로 보고 Busy를
// 보고한다(계획 3.2절 "timeout은 실패").
inline constexpr uint32 MaxShutdownCollectPasses = 8;

// 파괴가 미뤄진 World(외부가 Borrow·RootRef를 보유해 Shutdown이 Busy)를 경고로 알리기까지의 프레임
// 수다. 첫 지연은 Debug, 이 프레임 수를 넘도록 남아 있으면 Warning 한 번, 해소되면 Info 한 번을
// 남긴다(Docs/Design/LoggingSystem.ko.md 13절). 벽 시간이 아니라 엔진 프레임 수로 센다.
inline constexpr uint64 WorldDestroyWarnFrames = 300;

// 엔진 실행 상태다.
enum class EngineState : uint8 {
    Uninitialized,
    Initializing,
    Running,
    Stopping,
    Stopped,
    Failed
};

const char* EngineStateName(EngineState state) noexcept;

// 엔진 수명과 같이하는 Subsystem의 기반이다(SubsystemCollection 참고). 초기화는 TypeRegistry와 엔진
// Registry가 준비된 뒤, EngineClient보다 먼저 의존 순서대로 한다.
class EngineSubsystem {
public:
    virtual ~EngineSubsystem() = default;

    virtual Result<void> Initialize(Engine&) noexcept {
        return Result<void>::Success();
    }

    // 매 프레임 World Tick 전에 호출된다.
    virtual void OnFrameBegin(Engine&) noexcept {
    }

    // 매 프레임 World Tick·지연 작업 뒤에 호출된다.
    virtual void OnFrameEnd(Engine&) noexcept {
    }

    // 종료 1단계(계획 3.2절 "신규 요청 차단")다. 새 작업 제출을 막는다. 종료 중 한 번만 불린다.
    virtual void BeginShutdown(Engine&) noexcept {
    }

    // 종료 2단계(World 파괴 뒤, "GPU fence·audio epoch·physics step 완료")다. 진행 중인 비동기
    // 작업이 끝날 때까지 기다린다. 실패는 Shutdown의 결과로 보고되며 뒤 단계는 계속 실행한다.
    virtual Result<void> CompleteInFlightWork(Engine&) noexcept {
        return Result<void>::Success();
    }

    virtual void Deinitialize(Engine&) noexcept {
    }
};

using EngineSubsystemType = SubsystemType<EngineSubsystem>;

// 실행 파일이 엔진 루프에 끼워 넣는 확장 지점이다. 창 이벤트 펌프·입력 수집·렌더 추출과 제출처럼
// Engine이 직접 의존하지 않는 시스템을 연결한다. 헤드리스 실행은 hook 없이 돈다.
//
// 종료 단계 hook(CP6, Docs/Design/RenderBase.ko.md 4.3·5.5절, 계획 3.2절 Shutdown 순서)은 렌더
// lane처럼 Engine이 모르는 실행기가 엔진 종료 순서에 끼어드는 자리다. Engine::Shutdown이 다음
// 순서로 부른다(각 hook은 종료 중 한 번, DrainShutdownCleanup만 반복):
//   BeginShutdown → client 종료·World 파괴 → CompleteInFlightWork → Subsystem 정리 → [최종 GC →
//   DrainShutdownCleanup]을 닫힐 때까지 반복 → VerifyShutdownClosure → StopExecutors → 엔진
//   Registry· 타입 정리.
// 실패한 단계가 있어도 뒤 단계를 건너뛰지 않으며 Shutdown은 첫 오류를 돌려준다.
//
// 사용 예(렌더 lane 연결):
//   void BeginShutdown(Engine&) noexcept override { renderLane.BlockFrames(); }
//   Result<void> CompleteInFlightWork(Engine&) noexcept override {
//       return renderLane.WaitFramesIdle();
//   }
//   Result<uint64> DrainShutdownCleanup(Engine&) noexcept override {
//       return renderLane.DrainRetires();
//   }
//   Result<void> VerifyShutdownClosure(Engine&) noexcept override {
//       return renderLane.CloseResources();
//   }
//   Result<void> StopExecutors(Engine&) noexcept override { return renderLane.Stop(); }
class EngineLoopHooks {
public:
    // 1단계: 프레임의 첫 단계다. 창 메시지·입력을 펌프한다. 종료가 필요하면
    // engine.RequestExit()를 호출한다.
    virtual void PumpEvents(Engine&) noexcept {
    }

    // 3단계: 시간 갱신 뒤, World Tick 전이다.
    virtual void BeginFrame(Engine&) noexcept {
    }

    // 6단계: 프레임의 마지막 단계다. World 상태로 렌더를 추출·제출한다.
    virtual void EndFrame(Engine&) noexcept {
    }

    // 종료: 새 게임·편집 요청과 새 프레임 제출을 막는다(계획 3.2절 첫 단계).
    virtual void BeginShutdown(Engine&) noexcept {
    }

    // 종료: World 파괴 뒤 진행 중인 GPU 프레임·비동기 작업이 끝날 때까지 기다린다("GPU fence
    // 완료").
    virtual Result<void> CompleteInFlightWork(Engine&) noexcept {
        return Result<void>::Success();
    }

    // 종료: 최종 GC 한 번이 끝날 때마다 불린다. 그 GC가 만든 정리 작업(Retire·backend 회수)을
    // 끝까지 처리하고 이번에 처리한 수를 돌려준다. Engine은 GC 회수 수와 이 값이 모두 0이 될 때까지
    // 반복한다(closure). 실패는 보고되고 0으로 본다.
    virtual Result<uint64> DrainShutdownCleanup(Engine&) noexcept {
        return Result<uint64>::Success(0);
    }

    // 종료: owner·retire·queue closure를 확인한다(남은 자원·대기 0). 남아 있으면 실패를 돌려준다.
    virtual Result<void> VerifyShutdownClosure(Engine&) noexcept {
        return Result<void>::Success();
    }

    // 종료: 전용 lane·scheduler 같은 실행기를 끝낸다(남은 작업 0 확인·Join). 엔진 Registry·타입
    // 정리 전이다.
    virtual Result<void> StopExecutors(Engine&) noexcept {
        return Result<void>::Success();
    }

protected:
    ~EngineLoopHooks() = default;
};

// 게임 세션 경계다. Engine은 게임플레이 프레임워크를 모르며, GameInstance가 이 인터페이스를
// 구현한다.
class EngineClient {
public:
    // 엔진 초기화의 마지막 단계다. 실패하면 엔진 초기화가 실패하고 OnEngineShutdown은 호출되지
    // 않는다.
    virtual Result<void> OnEngineStart(Engine&) noexcept {
        return Result<void>::Success();
    }

    // 매 프레임 World Tick 전에 호출된다.
    virtual void OnEngineTick(Engine&) noexcept {
    }

    // 엔진 종료의 첫 단계다(World 파괴 전).
    virtual void OnEngineShutdown(Engine&) noexcept {
    }

protected:
    ~EngineClient() = default;
};

// 엔진 설정이다. 포인터 필드는 엔진보다 오래 살아야 한다.
struct EngineConfig {
    // 엔진 Registry(World보다 오래 사는 객체, 예: GameInstance)의 실행 문맥이다.
    ContextId context = {0x524f434b53454e47ull, 1};
    Span<const EngineSubsystemType> subsystems;
    EngineLoopHooks* hooks = nullptr;
    EngineClient* client = nullptr;
    // 0보다 크면 측정한 실제 시간 대신 이 값을 매 프레임 delta로 쓴다(헤드리스 결정적 실행·테스트).
    double fixedFrameDeltaSeconds = 0.0;
    // 0이 아니면 Run이 이 프레임 수를 실행한 뒤 스스로 종료 요청한다.
    uint64 maxFrames = 0;
    // 엔진 ModuleRegistration에 World 타입 다음으로 등록할 리플렉션 모듈 등록 함수다(선택). 게임
    // 모듈의 생성 집계 함수(rocks::generated::<Module>::RegisterModuleTypes)를 부른다. 같은
    // ModuleRegistration이라 이미 등록된 의존 모듈(World 등)은 다시 등록하지 않는다. 실패하면 엔진
    // 초기화가 실패한다.
    //
    // 사용 예:
    //   config.registerModuleTypes = [](ModuleRegistration& modules) noexcept {
    //       return generated::MyGame::RegisterModuleTypes(modules);
    //   };
    Result<void> (*registerModuleTypes)(ModuleRegistration&) noexcept = nullptr;
    // 엔진 TypeRegistry에 마지막으로 등록할 native 타입(ROCKS_NATIVE_TYPE) 등록 함수다(선택).
    // native 파생은 리플렉션 타입을 base로 둘 수 있으므로 World·리플렉션 모듈 뒤에 부른다. 받은
    // 등록 토큰은 버려도 된다 — Engine은 종료할 때 타입을 Unregister하지 않고
    // TypeRegistry::Shutdown이 모두 반납한다.
    Result<void> (*registerTypes)(TypeRegistry&) noexcept = nullptr;
    // 호스트가 만든 설정 문맥이다(선택, Docs/Design/ConfigSystem.ko.md 5.3절). 엔진은 빌려 쓰기만
    // 하고 만들거나 파괴하지 않는다. 있으면 프레임마다 시작 단계(3) 전에 Sync를 부른다(sync 지점 —
    // 콘솔 입력·설정 변경은 여기서만 값이 된다). 엔진과 같은 스레드(게임 스레드)가 소유해야 하고
    // 엔진보다 오래 살아야 한다. 층 읽기·`--set`·CompleteStartup은 엔진 초기화 전에 호스트가 한다.
    config::ConfigContext* settings = nullptr;
    // World GC 표식의 병렬 worker 수다. 엔진이 WorkerScheduler를 만들어 World에
    // 빌려준다(Docs/Design/ GarbageCollectionScale.ko.md 17.4절). GcWorkersAuto면 논리 프로세서 수
    // − 1을 MaxAutoGcWorkers로 자른 값이고, 0이면 만들지 않는다(직렬 표식).
    static constexpr uint32 GcWorkersAuto = NumericLimits<uint32>::Max();
    // 자동 worker 수의 상한이다(표식 gate를 ×8로 판정 — 실험 21절에서 ×16의 추가 이득이 작다).
    static constexpr uint32 MaxAutoGcWorkers = 8;
    uint32 gcWorkers = GcWorkersAuto;
};

// 새 World 설정이다(WorldDesc에서 엔진이 정하는 실행 문맥을 뺀 것).
struct WorldCreateDesc {
    StringView name;
    TimeStepPolicy timeStep;
    RegistryOptions objects;
    GcBudget gcBudget;
    // World GC 일정이다(WorldDesc::gcSchedule).
    WorldGcSchedule gcSchedule;
    // World GC 표식의 병렬 scheduler다(WorldDesc::gcScheduler — 빌림). 없으면 엔진의 GC scheduler를
    // 쓴다(EngineConfig::gcWorkers가 0이면 직렬).
    WorkerScheduler* gcScheduler = nullptr;
    Span<const WorldSubsystemType> subsystems;
    // 만든 뒤 바로 BeginPlay한다. 에디터·미리보기 World는 false로 둔다.
    bool beginPlay = true;
};

// 엔진 프레임 시간이다. 실제(벽) 시간 기준이며 World별 게임 시간은 각 World의 GameClock이 가진다.
struct EngineTime {
    uint64 frameIndex = 0;
    double deltaSeconds = 0.0;
    double realTimeSeconds = 0.0;
    // 최근 프레임의 지수 이동 평균(초)이다.
    double averageFrameSeconds = 0.0;
};

struct EngineStats {
    uint32 worlds = 0;
    uint32 subsystems = 0;
    uint64 frameNanoseconds = 0;
    uint64 worldTickNanoseconds = 0;
    uint32 pendingWorldDestroys = 0;
};

// 엔진 실행을 맡는다: 상태 기계, 메인 루프, 타입 등록, 엔진 Registry, EngineSubsystem, World 실행
// 문맥.
//
// 사용 예(헤드리스):
//   Engine engine(allocator);
//   EngineConfig config;
//   config.maxFrames = 100;
//   if (!engine.Initialize(config)) { ... }          // 실패하면 Failed, 이미 한 단계는 역순 정리
//   auto world = engine.CreateWorld({.name = "Main"});
//   world.Value()->SpawnActor<MyActor>();
//   engine.Run();                                    // RequestExit 또는 maxFrames까지
//   engine.Shutdown();                               // 여러 번 호출해도 안전
//
// 프레임 순서(RunFrame): 1) hooks.PumpEvents -> 2) 시간 갱신·설정 sync 지점(EngineConfig.settings가
// 있으면 ConfigContext::Sync) -> 3) hooks.BeginFrame·Subsystem.OnFrameBegin·client.OnEngineTick ->
// 4) World마다 Tick -> 5) 지연 작업(World 파괴) -> 6) Subsystem.OnFrameEnd·hooks.EndFrame.
//
// 수명 규약:
// - 초기화 순서: TypeRegistry·ModuleRegistration -> 타입 등록(World 리플렉션 타입 ->
//   EngineConfig.registerModuleTypes -> EngineConfig.registerTypes(native, 마지막)) -> 엔진
//   Registry -> EngineSubsystem -> EngineClient. 어느 단계든 실패하면 이미 한 단계를 역순으로
//   정리하고 Failed가 된다.
// - 종료 순서(계획 3.2절): BeginShutdown(Subsystem 역순 → hooks) -> client.OnEngineShutdown -> 모든
//   World Shutdown·파괴 -> CompleteInFlightWork(Subsystem 역순 → hooks) -> Subsystem 역순 정리 ->
//   [엔진 Registry 최종 GC -> hooks.DrainShutdownCleanup]을 둘 다 0이 될 때까지 반복(최대
//   MaxShutdownCollectPasses) -> hooks.VerifyShutdownClosure -> hooks.StopExecutors -> 엔진
//   Registry 종료 -> ModuleRegistration 파괴(붙잡은 lease 해제) -> TypeRegistry::Shutdown. 타입은
//   Unregister하지 않는다 — 게임이 토큰을 버린 native 파생이 리플렉션 base의 dependent로 남아
//   Unregister가 Busy로 멈추기 때문이다. Shutdown은 남은 lease만 검사하고 dependent 순서로 레코드를
//   반납한다. 초기화 실패 경로는 hook 단계 없이 같은 정리 순서다. 프레임 실행 중(hook 안)에는
//   Busy이므로 RequestExit 후 Run이 돌아온 뒤 호출한다.
// - World는 Engine이 소유한다. 프레임 안에서 DestroyWorld를 호출하면 5단계(지연 작업)에서 파괴된다.
//
// 로그(Docs/Design/LoggingSystem.ko.md 13절, 카테고리 LogEngine): 초기화 성공·단계별 실패, Run
// 종료 사유, 종료 시작·완료·실패, World 생성 실패·Tick 실패(연속 실패는 첫 번째와 요약 한 번)·지연
// 파괴(Debug → WorldDestroyWarnFrames 넘으면 Warning 한 번 → 해소 Info)를 남기고, 초기화 단계마다
// "Stage", World Tick마다 "World" 문맥을 걸고, 프레임마다 Log::SetFrame을 부른다. LogSystem은
// 만들거나 설치하지 않는다(호스트가 LogInstallScope로 설치한다). 실패는 여기서 한 번 기록하므로
// 호스트는 반환된 오류를 다시 기록하지 않는다. 호출 순서 오류(InvalidArgument·Busy)는 기록하지
// 않는다.
//
// 스레드: 생성한 게임 스레드에서만 사용한다.
class Engine {
public:
    explicit Engine(Allocator& allocator) noexcept;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    // Shutdown하지 않았으면 Shutdown한다. 끝낼 수 없으면(외부 Borrow 보유 등) Fatal이다.
    ~Engine();

    // Uninitialized에서만 호출할 수 있다(그 외 InvalidArgument). 실패하면 Failed이며 다시 초기화할
    // 수 없다.
    Result<void> Initialize(const EngineConfig& config) noexcept;

    // 한 프레임을 실행한다(클래스 설명의 순서). Running에서만 가능하다. 재진입은 Busy다. World Tick
    // 오류는 첫 오류를 반환하되 나머지 World와 남은 단계는 계속 실행한다.
    Result<void> RunFrame() noexcept;

    // RequestExit 또는 maxFrames까지 RunFrame을 반복한다. 끝나면 Stopping 상태로 돌아온다(Shutdown
    // 필요).
    Result<void> Run() noexcept;

    // 다음 프레임 경계에서 Run을 끝내도록 요청한다. 언제 호출해도 안전하다.
    void RequestExit() noexcept {
        exitRequested_ = true;
    }

    bool IsExitRequested() const noexcept {
        return exitRequested_;
    }

    // 엔진을 정리한다. Uninitialized·Stopped·Failed에서는 아무것도 하지 않고 성공한다. 프레임 실행
    // 중이면 Busy, World가 외부 보유권 때문에 끝나지 않으면 그 오류이며 Stopping에 머문다(반환 후
    // 다시 호출 — 이미 부른 종료 hook은 다시 부르지 않는다). 종료 hook 단계가 실패하거나 최종 GC·
    // 정리가 MaxShutdownCollectPasses 안에 닫히지 않으면(Busy) 나머지 단계를 마저 실행해 Stopped가
    // 된 뒤 첫 오류를 돌려준다.
    Result<void> Shutdown() noexcept;

    EngineState State() const noexcept {
        return state_;
    }

    // --- World ---

    // World를 만들어 초기화한다(desc.beginPlay면 BeginPlay까지). Running에서만 가능하다. 반환
    // 포인터는 DestroyWorld·Shutdown까지 유효하다.
    Result<World*> CreateWorld(const WorldCreateDesc& desc) noexcept;

    // World 파괴를 요청한다. 프레임 밖이면 즉시, 프레임 안이면 5단계(지연 작업)에서 파괴한다.
    // 모르는 World는 NotFound다. 파괴가 Busy로 실패하면(외부 Borrow) 다음 프레임에 다시 시도한다.
    Result<void> DestroyWorld(World* world) noexcept;

    usize WorldCount() const noexcept {
        return worlds_.Size();
    }

    // 모든 World에 STW 병렬 전체 수집을 요청한다(World::RequestFullCollection — 레벨 전환·로딩
    // 화면·메모리 압박을 아는 쪽이 부른다).
    void RequestFullCollection() noexcept;

    // 엔진이 World에 빌려주는 GC 표식 scheduler다(없으면 nullptr — 직렬). Running 동안 유효하다.
    WorkerScheduler* GcScheduler() const noexcept {
        return gcScheduler_.Get();
    }

    // 실행 순서대로 index번째 World다. 범위 밖이면 nullptr이다.
    World* WorldAt(usize index) const noexcept {
        return index < worlds_.Size() ? worlds_[index].world.Get() : nullptr;
    }

    // --- 공용 서비스 ---

    TypeRegistry& Types() noexcept {
        return *types_;
    }

    // World보다 오래 사는 객체의 Registry다(Initialize 전에는 Fatal).
    ObjectRegistry& Objects() noexcept;

    Allocator& GetAllocator() const noexcept {
        return allocator_;
    }

    const EngineTime& Time() const noexcept {
        return time_;
    }

    template <class T>
    T* GetSubsystem() const noexcept {
        return subsystems_.template Get<T>();
    }

    EngineStats Stats() const noexcept;

private:
    struct WorldContext {
        UniquePtr<World> world;
        bool pendingDestroy = false;
        // 파괴 시도가 실패해 미뤄진 상태다. 첫 실패 프레임부터 센다(WorldDestroyWarnFrames).
        bool destroyDeferred = false;
        bool destroyWarned = false;
        ErrorCode destroyError = ErrorCode::None;
        uint64 destroyDeferredFrame = 0;
        // 연속 Tick 실패다. 첫 실패만 Error로 남기고 뒤따른 실패는 세어 두었다가 회복·파괴 때 한 번
        // 요약한다.
        bool tickFailing = false;
        ErrorCode lastTickError = ErrorCode::None;
        uint64 suppressedTickFailures = 0;
    };

    // stage는 실패한 초기화 단계 이름이다(정리 실패 Fatal에 싣는다).
    void Teardown(uint32 completedSteps, const char* stage) noexcept;
    // 종료 hook 단계(BeginShutdown 등)를 진행 정도에 맞춰 한 번씩 부른다.
    Result<void> RunShutdownHooksBeforeWorlds() noexcept;
    // failedStage는 첫 실패가 난 종료 단계 이름을 받는다(실패가 없으면 그대로).
    Result<void> CloseAfterSubsystems(const char*& failedStage) noexcept;
    Result<void> DestroyWorldNow(usize index) noexcept;
    void ProcessDeferred() noexcept;
    // World Tick 결과를 반영한다. 연속 실패는 첫 번째만 Error로 남긴다.
    void NoteWorldTick(WorldContext& context, const Result<void>& ticked) noexcept;
    // 연속 Tick 실패를 끝낸다. 첫 실패 뒤 더 실패한 것이 있으면 한 번 요약한다.
    void EndTickFailureStreak(WorldContext& context) noexcept;
    // GC scheduler를 끝내고 놓는다(없으면 성공).
    Result<void> StopGcScheduler() noexcept;
    // 종료 실패를 남은 World·지연 파괴·살아 있는 객체 수와 함께 한 번 기록한다.
    void LogShutdownFailure(const char* stage, ErrorCode error) const noexcept;

    Allocator& allocator_;
    EngineConfig config_;
    UniquePtr<TypeRegistry> types_;
    // TypeRegistry 뒤에 선언해 먼저 소멸시킨다(붙잡은 lease를 TypeRegistry::Shutdown 전에 놓는다).
    UniquePtr<ModuleRegistration> modules_;
    UniquePtr<ObjectRegistry> objects_;
    // GC 표식 scheduler다(17.4절). World보다 먼저 선언해 나중에 소멸시킨다 — Shutdown은 World를
    // 모두 파괴한 뒤 끝낸다.
    UniquePtr<WorkerScheduler> gcScheduler_;
    SubsystemCollection<EngineSubsystem, Engine> subsystems_;
    Array<WorldContext> worlds_;
    EngineTime time_;
    EngineState state_ = EngineState::Uninitialized;
    uint64 lastClock_ = 0;
    uint64 nextWorldContext_ = 1;
    uint64 frameNanoseconds_ = 0;
    uint64 worldTickNanoseconds_ = 0;
    uint32 initializedSteps_ = 0;
    // 종료 hook 단계의 진행 정도다(재시도 때 같은 hook을 다시 부르지 않는다).
    uint32 shutdownProgress_ = 0;
    bool inFrame_ = false;
    bool exitRequested_ = false;
    // maxFrames에 닿아 스스로 종료를 요청했는지다(Run 종료 사유).
    bool maxFramesReached_ = false;
    // 설정 Sync가 실패하고 있는지다(연속 실패는 첫 번째만 Error로 남긴다).
    bool settingsSyncFailing_ = false;
};

} // namespace rocks
