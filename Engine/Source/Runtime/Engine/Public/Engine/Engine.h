#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/UniquePtr.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <World/Subsystem.h>
#include <World/World.h>

namespace rocks {

class Engine;

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

    virtual void Deinitialize(Engine&) noexcept {
    }
};

using EngineSubsystemType = SubsystemType<EngineSubsystem>;

// 실행 파일이 엔진 루프에 끼워 넣는 확장 지점이다. 창 이벤트 펌프·입력 수집·렌더 추출과 제출처럼
// Engine이 직접 의존하지 않는 시스템을 연결한다. 헤드리스 실행은 hook 없이 돈다.
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
};

// 새 World 설정이다(WorldDesc에서 엔진이 정하는 실행 문맥을 뺀 것).
struct WorldCreateDesc {
    StringView name;
    TimeStepPolicy timeStep;
    RegistryOptions objects;
    GcBudget gcBudget;
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
// 프레임 순서(RunFrame): 1) hooks.PumpEvents -> 2) 시간 갱신 -> 3) hooks.BeginFrame·
// Subsystem.OnFrameBegin·client.OnEngineTick -> 4) World마다 Tick -> 5) 지연 작업(World 파괴)
// -> 6) Subsystem.OnFrameEnd·hooks.EndFrame.
//
// 수명 규약:
// - 초기화 순서: TypeRegistry·ModuleRegistration -> 타입 등록(World 리플렉션 타입 ->
//   EngineConfig.registerModuleTypes -> EngineConfig.registerTypes(native, 마지막)) -> 엔진
//   Registry -> EngineSubsystem -> EngineClient. 어느 단계든 실패하면 이미 한 단계를 역순으로
//   정리하고 Failed가 된다.
// - 종료 순서: client.OnEngineShutdown -> 모든 World Shutdown·파괴 -> Subsystem 역순 정리 -> 엔진
//   Registry -> ModuleRegistration 파괴(붙잡은 lease 해제) -> TypeRegistry::Shutdown. 타입은
//   Unregister하지 않는다 — 게임이 토큰을 버린 native 파생이 리플렉션 base의 dependent로 남아
//   Unregister가 Busy로 멈추기 때문이다. Shutdown은 남은 lease만 검사하고 dependent 순서로 레코드를
//   반납한다. 초기화 실패 경로도 같은 순서다. 프레임 실행 중(hook 안)에는 Busy이므로 RequestExit 후
//   Run이 돌아온 뒤 호출한다.
// - World는 Engine이 소유한다. 프레임 안에서 DestroyWorld를 호출하면 5단계(지연 작업)에서 파괴된다.
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
    // 다시 호출).
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
    };

    void Teardown(uint32 completedSteps) noexcept;
    Result<void> DestroyWorldNow(usize index) noexcept;
    void ProcessDeferred() noexcept;

    Allocator& allocator_;
    EngineConfig config_;
    UniquePtr<TypeRegistry> types_;
    // TypeRegistry 뒤에 선언해 먼저 소멸시킨다(붙잡은 lease를 TypeRegistry::Shutdown 전에 놓는다).
    UniquePtr<ModuleRegistration> modules_;
    UniquePtr<ObjectRegistry> objects_;
    SubsystemCollection<EngineSubsystem, Engine> subsystems_;
    Array<WorldContext> worlds_;
    EngineTime time_;
    EngineState state_ = EngineState::Uninitialized;
    uint64 lastClock_ = 0;
    uint64 nextWorldContext_ = 1;
    uint64 frameNanoseconds_ = 0;
    uint64 worldTickNanoseconds_ = 0;
    uint32 initializedSteps_ = 0;
    bool inFrame_ = false;
    bool exitRequested_ = false;
};

} // namespace rocks
