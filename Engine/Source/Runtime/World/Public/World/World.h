#pragma once

#include <Base/TypeTraits.h>
#include <Config/CVarDefinition.h>
#include <Core/Containers/HashMap.h>
#include <Core/Text/String.h>
#include <Core/Time/GameClock.h>
#include <Core/Traits.h>
#include <Object/ModuleRegistration.h>
#include <Object/NativeClass.h>
#include <Object/ObjectRegistry.h>
#include <World/Actor.h>
#include <World/Component.h>
#include <World/Level.h>
#include <World/Subsystem.h>

namespace rocks::config {

class ConfigContext;

} // namespace rocks::config

namespace rocks {

class ConstructionContext;
class World;

// World 수명과 같이하는 Subsystem의 기반이다(SubsystemCollection 참고).
class WorldSubsystem {
public:
    virtual ~WorldSubsystem() = default;

    // World 초기화 중 의존 순서대로 호출된다. 실패하면 World 초기화가 실패한다. 이때 Actor를
    // Spawn해도 된다(초기화 실패 시 함께 정리된다).
    virtual Result<void> Initialize(World&) noexcept {
        return Result<void>::Success();
    }

    // 모든 Subsystem의 Initialize가 끝난 뒤 같은 순서로 호출된다(다른 Subsystem의 초기화 결과를
    // 쓰는 자리 — UE PostInitialize).
    virtual void PostInitialize(World&) noexcept {
    }

    // World BeginPlay 안에서 Actor의 OnStart보다 먼저 호출된다(엔진 내부 단계 이름 BeginPlay가
    // Actor hook OnStart에 대응한다).
    virtual void OnWorldBeginPlay(World&) noexcept {
    }

    // 게임 진행이 끝날 때(Actor 정리 뒤) 초기화 역순으로 호출된다(UE OnWorldEndPlay — UE는
    // 정방향). 바깥 계층 Subsystem을 쓰는 정리는 여기서 끝낸다(이 뒤로 GetOuterSubsystem은
    // nullptr).
    virtual void OnWorldEndPlay(World&) noexcept {
    }

    // 모든 Deinitialize 전에 초기화 역순으로 한 번씩 호출된다(UE PreDeinitialize).
    virtual void PreDeinitialize(World&) noexcept {
    }

    // World 종료 중(모든 Actor 정리 후) 초기화 역순으로 호출된다.
    virtual void Deinitialize(World&) noexcept {
    }

    // 참이면 매 프레임 LateUpdate 시작의 엔진 단계 TickableSubsystems에서 OnTick을 받는다(초기화
    // 순서, 15.4절 — UE UTickableWorldSubsystem). 초기화 뒤 바뀌지 않아야 한다.
    virtual bool IsTickable() const noexcept {
        return false;
    }

    // 참이면 pause 중에도 unscaled delta로 OnTick을 받는다(UE IsTickableWhenPaused).
    virtual bool TickWhenPaused() const noexcept {
        return false;
    }

    virtual void OnTick(World&, const TickContext&) noexcept {
    }
};

// World보다 바깥 계층(Engine·GameInstance)의 Subsystem을 키로 찾는 창이다. World는 해석하지 않는다
// (World 모듈은 Engine·Gameplay를 모른다 — 3절). 타입 있는 도우미는 바깥 모듈의 자유 함수다
// (GetEngineSubsystem<T> — Engine.h, GetGameInstanceSubsystem<T> — Gameplay). 16.3절 ERD10.
struct OuterSubsystemLookup {
    void* (*find)(const void* context, SubsystemKey key) noexcept = nullptr;
    const void* context = nullptr;
};

using WorldSubsystemType = SubsystemType<WorldSubsystem>;

// World 모듈의 리플렉션 타입(Level·Actor·Component·SceneComponent — WorldOnly)을 registration에
// 등록한다(rocks::generated::World::RegisterModuleTypes — 의존 모듈 CoreTypeBindings의 struct를
// 먼저, 그다음 base 먼저 클래스). 같은 registration에 다시 부르거나, 다른 registration이 같은 C++
// 타입을 이미 등록했으면 새로 등록하지 않고 lease를 붙잡아 성공한다(멱등). 같은 ID가 다른 C++
// 타입이면 AlreadyExists다. 실패하면 이 호출이 남긴 기록은 되돌려진다. 게임 타입(리플렉션
// 모듈·native 파생)보다 먼저 부른다 — native 파생은 리플렉션 base가 등록된 뒤에만 등록된다.
//
// 수명: registration이 등록한 타입은 registration이 파괴돼도 해제되지 않는다(붙잡은 lease만
// 놓는다). 해제하려면 native 파생을 먼저 Unregister한 뒤 registration.Unregister()를 부르거나(남은
// 파생이 있으면 Busy), Engine처럼 Unregister하지 않고 registration 파괴 -> TypeRegistry::Shutdown
// 순서로 끝낸다(Shutdown은 남은 lease만 검사하고 dependent 순서로 반납한다).
//
// 사용 예:
//   TypeRegistry types(allocator);
//   ModuleRegistration modules(types, allocator);       // types 뒤에 선언해 먼저 파괴한다
//   if (auto registered = RegisterWorldTypes(modules); !registered) { ... }
//   auto spinner = RegisterNativeClass<Spinner>(types);  // Actor.generated.h를 include한 TU
ROCKS_WORLD_API Result<void> RegisterWorldTypes(ModuleRegistration& registration) noexcept;

// World GC 일정 방식이다(Docs/Design/GarbageCollectionScale.ko.md 17.3절).
// - Incremental: World 파괴가 있으면 프레임 끝마다 WorldDesc::gcBudget만큼 증분 slice를 진행한다.
// - FullCollection: 마지막 전체 수집 뒤 실제 경과 시간(Tick에 넘긴 unscaled delta의 합)이
//   WorldGcSchedule::fullCollectionIntervalSeconds를 지나면 프레임 끝에 STW 병렬 전체 수집을
//   한다(UE의 주기 수집 방식 — World 파괴가 있었는지는 보지 않는다).
enum class WorldGcMode : uint8 {
    Incremental,
    FullCollection
};

// World GC 일정이다. 어느 방식이든 World::RequestFullCollection은 다음 GC 단계(Tick 끝)에 STW 병렬
// 전체 수집을 하고, 회수(소멸자·저장소 반환)는 그 뒤 프레임마다 reclaimBudget씩 나눈다.
//
// 사용 예(주기 수집, 간격 30초):
//   WorldDesc desc;
//   desc.gcSchedule.mode = WorldGcMode::FullCollection;
//   desc.gcSchedule.fullCollectionIntervalSeconds = 30.0;
struct WorldGcSchedule {
    // FullCollection 간격 기본값이다(UE gc.TimeBetweenPurgingPendingKillObjects 61.1초에 대응).
    static constexpr double DefaultFullCollectionIntervalSeconds = 60.0;
    // 전체 수집 뒤 프레임마다의 회수 시간 기본값이다(UE gc.IncrementalGCTimePerFrame 2ms에 대응).
    static constexpr uint64 DefaultReclaimNanoseconds = 2000000;

    WorldGcMode mode = WorldGcMode::Incremental;
    // FullCollection에서 전체 수집 사이의 실제 경과 시간(초, 일시정지·시간 배율과 무관)이다. 0
    // 이하면 주기 수집을 하지 않는다(요청만).
    double fullCollectionIntervalSeconds = DefaultFullCollectionIntervalSeconds;
    // 회수 대기가 있을 때 프레임마다 회수에 쓰는 예산이다(ObjectRegistry::DrainRetired).
    GcBudget reclaimBudget{GcBudget::DefaultMaxObjects, DefaultReclaimNanoseconds};
    // 전체 수집의 회수가 끝난 GC 단계(미룬 회수가 다 빠졌을 때 — 회수할 것이 없었으면 수집 직후)에
    // World의 allocator에 Allocator::TrimMemory를 부른다: 쥐고만 있는 메모리를 OS에 돌려준다(UE가
    // GC purge 끝에 FMemory::Trim을 부르는 것에 대응 — GC 설계 18.11.6절). Engine도 이 World를
    // 파괴한 뒤에 같은 값을 보고 부른다. 증분 cycle이 끝날 때는 부르지 않는다. 끄면 호스트가 필요할
    // 때 TrimMemory를 직접 부른다.
    bool trimMemory = true;
};

// Actor Spawn 옵션이다.
struct SpawnParams {
    // 소속 Level이다. null이면 영구 Level이다. 파괴 대기 중인 Level이면 Spawn이 Destroyed로
    // 실패한다.
    ObjectHandle level;
};

// World 생성 설정이다.
struct WorldDesc {
    // 진단용 이름이다. World가 복사해 보관한다.
    StringView name;
    // 이 World 객체들의 실행 문맥이다. 살아 있는 World끼리 겹치지 않는 값이어야 한다.
    ContextId context;
    TimeStepPolicy timeStep;
    RegistryOptions objects;
    // 프레임 끝 증분 GC 한 번의 예산이다(WorldGcMode::Incremental).
    GcBudget gcBudget;
    // GC 일정(증분·주기 전체 수집, 전체 수집 뒤 회수 예산)이다.
    WorldGcSchedule gcSchedule;
    // 주면 World의 GC 표식(증분 slice, 전체 수집, 종료 수집)을 이 scheduler로 병렬로 한다(GC 설계
    // GC-S4 — ObjectRegistry::StepCollection의 scheduler 규칙). 빌림이며 시작된 scheduler여야 하고
    // World보다 오래 살아야 한다. World를 그 worker 스레드에서 tick하지 않는다. 없으면 직렬
    // 표식이다.
    WorkerScheduler* gcScheduler = nullptr;
    // 초기화할 WorldSubsystem 종류다. 배열은 Initialize가 끝날 때까지 살아 있어야 한다.
    Span<const WorldSubsystemType> subsystems;
    // World 종류다(Subsystem 기본 생성 판정 — 16.4절).
    WorldKind kind = WorldKind::Game;
    // 바깥 계층 Subsystem 조회 창이다(없으면 조회가 언제나 nullptr).
    OuterSubsystemLookup outerSubsystems;
    // Level마다 GC 클러스터를 만든다(기본 — Docs/Design/GarbageCollectionScale.ko.md 13.3절).
    // Level이 클러스터 루트이고 SpawnActor·AddComponent로 만든 리플렉션 Actor·Component가 가입한다.
    // 끄는 것은 측정 비교와 객체 단위 표식 순서를 보는 시험용이다.
    bool levelClusters = true;
    // 설정 문맥이다(선택 — Docs/Design/CVarSystem.ko.md 14.2절). 있으면 Initialize가 Global의 자식
    // CVar 범위를 만들고 Shutdown이 파괴한다(CVarScope). World와 같은 게임 lane이 소유하고
    // World보다 오래 살아야 한다(범위가 남은 채 문맥을 파괴하면 문맥이 Fatal로 잡는다).
    config::ConfigContext* settings = nullptr;
};

namespace detail {

template <class R>
struct WithResult {
    using Type = Result<R>;
};

template <>
struct WithResult<void> {
    using Type = Result<void>;
};

template <class R>
struct WithResult<Result<R>> {
    using Type = Result<R>;
};

template <class R>
inline constexpr bool IsResultV = false;

template <class R>
inline constexpr bool IsResultV<Result<R>> = true;

template <class R>
inline constexpr bool IsSpanV = false;

template <class R>
inline constexpr bool IsSpanV<Span<R>> = true;

} // namespace detail

// Actor의 실행 환경이다. 자기 ObjectRegistry(독립 실행 문맥), Level(Actor 소유 묶음), Actor 수명
// 기록, TickScheduler, 게임 시계, WorldSubsystem을 소유한다. GameDirector 없이도 동작한다.
//
// 소유 사슬: World -> Level(RootRef) -> Actor(Level의 ObjectRefArray) -> Component(Actor의
// ObjectRefArray). World가 보유하는 객체 루트는 Level 루트뿐이다.
//
// 사용 예:
//   RegisterWorldTypes(modules);                       // 기본 타입 -> 게임 타입 순서로 등록
//   RegisterNativeClass<Spinner>(types);
//   auto created = UniquePtr<World>::TryMake(allocator, allocator, types, desc);
//   World& world = *created.Value();
//   if (!world.Initialize() || !world.BeginPlay()) { ... }
//   auto actor = world.SpawnActor<Spinner>();          // OnAwake·OnEnable까지, OnStart는 프레임
//   시작 while (running) {
//       if (!world.Tick(frameSeconds)) { ... }
//   }
//   if (!world.Shutdown()) { ... }                     // 모든 Actor 정리·Registry 종료
//
// 프레임(Tick) 순서:
//   1) 게임 시계 진행 -> 2) 시작 대기 Actor/Component의 OnStart(Playing·활성일 때) -> 3) Tick 단계:
//   PrePhysics -> Physics(고정 스텝 수만큼) -> PostPhysics -> LateUpdate -> PreRender ->
//   4) 파괴 대기 객체 정리(OnDisable·OnDestroy·OnUnregister·회수 요청) -> 5) 증분 GC 한 조각.
//
// hook 순서(Actor A, Component C):
//   생성: C.OnRegister -> A.OnAwake -> C.OnAwake -> A.OnEnable -> C.OnEnable
//   시작: C.OnStart -> A.OnStart(BeginPlay 전 Spawn은 BeginPlay 안, 이후 Spawn은 다음 프레임 시작)
//   Tick: 같은 단계에서 (priority, 등록 순서) — 같은 priority면 먼저 등록되는 A가 C보다 먼저(자동
//   의존은 없다. 부착된 SceneComponent는 부모 뒤, 그 밖의 순서는 AddTickPrerequisite)
//   정리: C.OnDisable -> A.OnDisable -> C.OnDestroy -> A.OnDestroy -> C.OnUnregister
//   비활성 객체는 활성화될 때까지 OnStart를 미룬다. OnDestroy는 OnAwake가 성공한 객체만 받는다.
//
// 소유·수명 규약:
// - 외부 코드는 Actor·Component·Level의 ObjectHandle을 보관하고 필요할 때 With로 접근한다.
// RootRef를
//   따로 잡지 않는다(정리 후에도 저장소가 남는다). Resolve로 받은 Borrow와 PinForEncode 고정은
//   Tick·Shutdown 전에 반환한다(Borrow가 남으면 GC가 미뤄지고 Shutdown이 Busy다 —
//   WorldStats::externalBorrows. 고정한 Level에서 그 프레임에 Actor를 정리하면 Level 배열을 고칠 수
//   없어 Fatal이다).
// - 파괴 요청(DestroyActor·DestroyComponent·DestroyLevel)은 즉시 메모리를 해제하지 않는다. 대상은
//   곧바로 Tick에서 빠지고, 프레임 끝(4단계)에서 hook과 함께 정리된다. 같은 대상의 중복 요청은
//   성공이다.
// - Objects()로 Registry를 직접 조작해 Actor·Component·Level을 파괴하지 않는다. 그런 객체를 Tick이
//   만나면 World가 레코드를 정리하지만 hook은 받지 못한다.
// - Initialize·BeginPlay·Tick·Shutdown과 소멸은 hook 밖에서만 한다(hook 안에서 호출하면 Busy).
// - World registry에는 World API(SpawnActor·AddComponent·CreateLevel)로만 Actor·Component·Level을
//   만든다. 그 밖에서 만든 객체(직접 NewObject, 그래프 로드)는 World 장부 밖이다 — 수명 기록이 없어
//   Tick·hook을 받지 않는다. 그 객체의 계층 API(Actor::SetRootComponent·Destroy,
//   Component::Destroy, SceneComponent::AttachTo(그 객체를 부모로 쓰는 경우 포함)·Detach)와
//   DestroyComponent는 InvalidArgument이고, 수명 기록이 필요한 World API(DestroyActor·AddComponent·
//   GetComponents)는 StaleHandle이다(편입·외부 생성 거부 정책은 EngineRuntime 문서 4.6절). 컨테이너
//   순회는 원소 수 기준이고 null 원소를 건너뛴다.
//
// 생성 헤더 규약: 이 헤더는 리플렉션 생성 헤더를 include하지 않는다(Engine.h 소비자 전체로 컴파일
// 비용이 번지지 않게). native 파생 T의 SpawnActor<T>·AddComponent<T>는 T::StaticTypeId만 쓰지만,
// 리플렉션 World 타입(Actor·SceneComponent 등)을 템플릿 인자로 쓰는 TU와 RegisterNativeClass<T>를
// 부르는 TU는 그 타입의 생성 헤더 <World/<Type>.generated.h>를 include한다 — 없으면
// ObjectTypeIdOf·RegisterNativeClass의 static_assert가 안내한다(모듈 생성 헤더
// <World/World.module.generated.h>는 등록 함수의 선언만 담는다).
//
// 스레드: 생성한 게임 스레드에서만 사용한다.
class ROCKS_WORLD_API World final : private TickExecutor {
public:
    World(Allocator& allocator, TypeRegistry& types, const WorldDesc& desc) noexcept;
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    // Shutdown하지 않았으면 Shutdown을 시도한다. 외부가 Borrow·RootRef를 보유해 끝낼 수 없으면
    // Fatal이다.
    ~World();

    // 영구 Level을 만들고 WorldSubsystem을 초기화한다. Created에서만 호출할 수 있다(그 외
    // InvalidArgument). 실패하면 초기화한 Subsystem을 역순 정리하고, 그동안 Spawn된 Actor와 Level을
    // 정리한 뒤 Created에 머문다.
    Result<void> Initialize() noexcept;

    // 게임플레이를 시작한다. Initialized에서만 호출할 수 있다. Subsystem의 OnWorldBeginPlay 뒤에
    // 시작 대기 중인 준비된 활성 Actor가 OnStart한다.
    Result<void> BeginPlay() noexcept;

    // 한 프레임을 실행한다(클래스 설명의 프레임 순서). Initialized·Playing에서만 가능하다(Created는
    // InvalidArgument, 종료 이후는 ShuttingDown). 재진입·hook 안 호출은 Busy다.
    Result<void> Tick(double realDeltaSeconds) noexcept;

    // 모든 Actor·Level을 정리하고 Subsystem·Registry를 닫는다. 여러 번 호출해도 안전하다(완료 후
    // Success). Tick·hook 안이면 Busy. 외부가 Borrow·RootRef를 보유해 회수할 수 없으면 그 오류이며
    // TearingDown에 머문다(반환 후 다시 호출).
    Result<void> Shutdown() noexcept;

    // STW 병렬 전체 수집을 요청한다(일정 방식과 무관 — 레벨 전환·로딩 화면·메모리 압박). 요청은
    // 다음 GC 단계(Tick 끝)에서 소비된다: Tick 밖에서 부르면 다음 Tick 끝, Tick 안(hook)에서 부르면
    // 그 Tick 끝이다. 증분 cycle이 Marking이면 취소하고, sweep·회수 중이면 회수 없이 끝낸 뒤
    // 수집한다. 회수는 그 뒤 프레임마다 gcSchedule.reclaimBudget씩 나눈다. 외부가 Borrow를 보유해
    // 수집할 수 없으면 요청을 남겨 다음 GC 단계에서 다시 한다. 어느 스레드에서나 부를 수 있다(요청
    // 표지는 atomic이고 게임 스레드의 Tick이 소비한다 — 메모리 압박 신호용).
    //
    // 사용 예(레벨을 내린 뒤):
    //   world.DestroyLevel(level);
    //   world.RequestFullCollection();
    void RequestFullCollection() noexcept;

    // 이 World의 GC 일정이다(WorldDesc::gcSchedule의 사본).
    const WorldGcSchedule& GcSchedule() const noexcept {
        return gcSchedule_;
    }

    // GC 표식에 쓰는 병렬 scheduler다(WorldDesc::gcScheduler — 없으면 nullptr, 직렬).
    WorkerScheduler* GcScheduler() const noexcept {
        return gcScheduler_;
    }

    WorldState State() const noexcept {
        return state_;
    }

    StringView Name() const noexcept {
        return name_.View();
    }

    // 이 World의 CVar 범위다(CVarSystem 8·14.2절 — Initialize가 만들고 Shutdown이 파괴한다). World
    // 안 객체는 `ShadowDistance.Get(GetWorld().CVarScope())`로 World별 값을 읽는다. 설정 문맥이
    // 없거나 Initialize 전·Shutdown 뒤면 Global이다.
    cvar::CVarScopeId CVarScope() const noexcept {
        return cvarScope_;
    }

    // --- Actor ---

    // T를 만들어 Level에 넣고 레코드에 등록한다(Constructed). OnAwake 전이므로 Component
    // 추가·설정을 할 수 있다. FinishSpawning까지 hook·Tick이 없다. Created 상태는 InvalidArgument,
    // T가 등록되지 않았으면 NotFound, 종료 중이면 ShuttingDown, params.level이 이 World의 Level이
    // 아니면 StaleHandle이다.
    template <class T>
    Result<ObjectHandle> SpawnActorDeferred(const SpawnParams& params = {}) noexcept {
        static_assert(IsBaseOfV<Actor, T>);
        return SpawnActorDeferred(ObjectTypeIdOf<T>(), params);
    }

    // 런타임 TypeId로 만든다. 등록된 Actor 파생 타입이 아니면 TypeMismatch다.
    Result<ObjectHandle> SpawnActorDeferred(TypeId type, const SpawnParams& params = {}) noexcept;

    // Constructed Actor를 초기화한다(클래스 설명의 생성 hook 순서). hook 안에서 추가한 Component도
    // 같은 단계까지 진행시킨다. Actor OnAwake가 실패하면 그 오류를 반환하고 Actor는 OnDestroy 없이
    // 정리 대기한다. Tick 설정이 잘못되면(OnTick 단계가 Physics·LateUpdate, 음수·비유한 interval)
    // InvalidArgument로 실패하고 정리한다. 이미 끝났으면 InvalidArgument, 진행 중 재진입은 Busy,
    // 파괴 대기면 Destroyed다.
    Result<void> FinishSpawning(ObjectHandle actor) noexcept;

    // SpawnActorDeferred + FinishSpawning이다. FinishSpawning이 실패하면 오류를 반환한다.
    template <class T>
    Result<ObjectHandle> SpawnActor(const SpawnParams& params = {}) noexcept {
        static_assert(IsBaseOfV<Actor, T>);
        return SpawnActor(ObjectTypeIdOf<T>(), params);
    }

    Result<ObjectHandle> SpawnActor(TypeId type, const SpawnParams& params = {}) noexcept;

    // 파괴를 요청한다. 이미 요청됐으면 성공, 이 World의 Actor가 아니면 StaleHandle이다.
    Result<void> DestroyActor(ObjectHandle actor) noexcept;

    // 이 World에 레코드가 있는 Actor인지다(Constructed·파괴 대기 포함, 정리 완료 후 false).
    bool HasActor(ObjectHandle actor) const noexcept;

    Result<ActorState> GetActorState(ObjectHandle actor) const noexcept;

    // 레코드가 있는 모든 Actor(Constructed·파괴 대기 포함)의 handle을 visitor(ObjectHandle)로
    // 넘긴다. 순회 중 Spawn·Destroy 요청은 허용하며 새 Actor는 이번 순회에 포함되지 않을 수 있다.
    template <class Visitor>
    void ForEachActor(Visitor&& visitor) noexcept {
        for (const ActorRecord& record : actors_) {
            visitor(record.handle);
        }
    }

    // --- Tick 의존·설정(Docs/Design/EngineRuntime 15절) ---

    // dependent hook이 prerequisite hook 뒤에 실행되게 한다(UE AddTickPrerequisiteActor·Component).
    // Actor·Component 어느 쪽이든, hook 세 종류 어느 것이든 된다. 시작 전(Spawn 직후·AddComponent
    // 직후)에도 선언할 수 있고, 양쪽이 Tick을 등록할 때 적용한다. 선행이 더 늦은 단계면 dependent가
    // 그 단계로 늦춰진다(다음 프레임부터). 순서는 다음 단계 경계부터 반영된다.
    // - 판정은 선언 시점의 TickSettings다: 그 hook을 켜지 않은 대상은 InvalidArgument(생성자에서
    // 정한다).
    // - 오류: 이 World의 Actor·Component가 아니면 StaleHandle, 파괴 대기면 Destroyed, 정의되지 않은
    // 출처·같은 hook 자신·Fixed hook과 다른 hook
    //   사이 의존은 InvalidArgument, 사용자 간선이 순환을 만들면 CycleDetected(상태 불변,
    //   World.Tick Warning — 시작 전 선언을 포함한 선언 전체에서 검사한다). 자동
    //   출처(Attachment·Framework)는 순환이면 걸지 않고 Warning 뒤 성공이다. 이미 있는 의존은
    //   성공(출처만 더한다)이다.
    //
    // 사용 예:
    //   world.AddTickPrerequisite({turret}, {vehicle});                    // vehicle OnTick 먼저
    //   world.AddTickPrerequisite({arm, TickHook::Late}, {body, TickHook::Late});
    Result<void> AddTickPrerequisite(TickPoint dependent, TickPoint prerequisite,
                                     TickEdgeSource source = TickEdgeSource::User) noexcept;

    // 그 출처의 의존을 뗀다. 모든 출처가 떼면 간선이 없어진다. 없으면 NotFound다.
    Result<void> RemoveTickPrerequisite(TickPoint dependent, TickPoint prerequisite,
                                        TickEdgeSource source = TickEdgeSource::User) noexcept;

    // OnTick 단계를 바꾼다(PrePhysics·PostPhysics·PreRender). 시작 뒤면 다음 프레임부터다. OnTick을
    // 받지 않는 대상·Physics·LateUpdate는 InvalidArgument, 이 World의 대상이 아니면
    // StaleHandle이다.
    Result<void> SetTickPhase(ObjectHandle object, TickPhase phase) noexcept;

    // 같은 단계 안 순서 값(모든 hook 공통)을 바꾼다. 시작 뒤면 다음 단계 경계부터다.
    Result<void> SetTickPriority(ObjectHandle object, int32 priority) noexcept;

    // 실행 간격(초, 모든 hook 공통)을 바꾸고 누적을 0으로 되돌린다. 음수·비유한 값은
    // InvalidArgument다.
    Result<void> SetTickInterval(ObjectHandle object, double intervalSeconds) noexcept;

    // hook이 지금 실행되는 단계(늦춤 반영)다. Tick을 등록하지 않았으면 NotFound다.
    Result<TickPhase> GetTickEffectivePhase(TickPoint point) const noexcept;

    // 엔진 단계 자리에 콜백을 둔다(자리마다 하나 — 이미 있으면 AlreadyExists, null·범위 밖은
    // InvalidArgument). 물리·애니메이션·Gameplay 모듈이 World를 만든 뒤 등록한다.
    Result<void> SetEngineStage(EngineStage stage, EngineStageFunction function,
                                void* user) noexcept;

    // 엔진 단계 자리를 비운다(비어 있어도 성공).
    void ClearEngineStage(EngineStage stage) noexcept;

    // actor가 속한 Level이다. 이 World의 Actor가 아니면 StaleHandle이다.
    Result<ObjectHandle> GetActorLevel(ObjectHandle actor) const noexcept;

    // --- Level ---

    // 영구 Level이다. Initialize 전과 Shutdown 후에는 null handle이다. World가 끝날 때까지 파괴할
    // 수 없다.
    ObjectHandle PersistentLevel() const noexcept {
        return persistentLevel_;
    }

    // 서브 Level을 만든다. Initialized·Playing에서만 가능하다(Created는 InvalidArgument, 종료
    // 이후는 ShuttingDown). name은 진단용이며 Level이 복사해 보관한다.
    Result<ObjectHandle> CreateLevel(StringView name) noexcept;

    // Level 파괴를 요청한다. 소속 Actor를 모두 파괴 대기로 만들고, 정리 단계에서 Actor가 모두
    // 정리되면 Level을 회수한다(Tick 밖에서 비어 있으면 즉시). 영구 Level은 InvalidArgument, 이미
    // 요청됐으면 성공, 모르는 handle은 StaleHandle이다.
    Result<void> DestroyLevel(ObjectHandle level) noexcept;

    // 이 World에 레코드가 있는 Level인지다(파괴 대기 포함).
    bool HasLevel(ObjectHandle level) const noexcept;

    // 모든 Level handle을 visitor(ObjectHandle)로 넘긴다. 첫 번째는 영구 Level이다. 순회 중 Level
    // 생성·파괴를 요청해도 안전하지만 그 변경은 이번 순회에 반영되지 않을 수 있다.
    template <class Visitor>
    void ForEachLevel(Visitor&& visitor) noexcept {
        for (usize index = 0; index < levels_.Size(); ++index) {
            visitor(levels_[index].handle);
        }
    }

    // --- Component ---

    // actor에 T를 추가한다. Actor가 Constructed이면 FinishSpawning 때(그 hook 안에서 추가해도),
    // 이미 초기화됐으면 즉시 OnRegister·OnAwake·OnEnable을 부르고 OnStart는 Actor 시작 때 또는 다음
    // 프레임 시작에 부른다. OnRegister·OnAwake가 실패하거나 Tick 설정이 잘못되면 그 오류를 반환하고
    // Component를 정리한다. 파괴 대기 Actor는 Destroyed다.
    template <class T>
    Result<ObjectHandle> AddComponent(ObjectHandle actor) noexcept {
        static_assert(IsBaseOfV<Component, T>);
        return AddComponent(actor, ObjectTypeIdOf<T>());
    }

    Result<ObjectHandle> AddComponent(ObjectHandle actor, TypeId type) noexcept;

    // Component 파괴를 요청한다. 이미 요청됐으면 성공이다. 이 World의 Component가 아니면
    // StaleHandle, 이 World registry에 있지만 World 장부 밖(AddComponent 밖에서 만든) 객체면
    // InvalidArgument다.
    Result<void> DestroyComponent(ObjectHandle component) noexcept;

    Result<ComponentState> GetComponentState(ObjectHandle component) noexcept;

    // actor가 가진 Component 중 T(또는 파생)인 첫 번째다. 없으면 null handle이다. null 원소는
    // 건너뛴다.
    template <class T>
    ObjectHandle FindComponent(ObjectHandle actor) noexcept {
        static_assert(IsBaseOfV<Component, T>);
        ObjectAccessScope scope(Access());
        for (usize index = 0;; ++index) {
            const ComponentSlot slot = ComponentSlotAt(actor, index);
            if (index >= slot.count) {
                return {};
            }

            if (!slot.component.IsNull() && objects_.Resolve<T>(Access(), slot.component).IsOk()) {
                return slot.component;
            }
        }
    }

    // actor의 Component 수와 handle(추가 순서)이다. out이 작으면 앞부분만 채우고 전체 수를
    // 반환한다.
    Result<uint32> GetComponents(ObjectHandle actor, Span<ObjectHandle> out) noexcept;

    // --- 객체 접근 ---

    // handle 객체를 T&로 빌려 function을 호출하고 결과를 돌려준다. function이 void면 Result<void>,
    // Result<X>면 그대로, X면 Result<X>를 반환한다. 대상이 없거나 타입이 다르면 그 오류다.
    // 호출 동안 이 World의 ObjectAccessScope를 세우므로 다른 Registry 문맥(엔진 hook 등) 안에서
    // 불러도 된다. 결과로 포인터·참조·Span을 돌려줄 수 없다(Borrow가 끝나면 무효이기 때문이다).
    //
    // 사용 예:
    //   auto moved = world.With<SceneComponent>(node, [](SceneComponent& scene) noexcept {
    //       return scene.SetRelativeLocation(Vec3d(0.0, 0.0, 100.0)); // 위로 100 cm, Result<void>
    //   });
    //   auto location = world.With<SceneComponent>(
    //       node, [](SceneComponent& scene) noexcept { return scene.WorldLocation(); });
    template <class T, class Function>
    auto With(ObjectHandle handle, Function&& function) noexcept {
        using Returned = decltype(function(DeclVal<T&>()));
        static_assert(
            !IsPointerV<Returned> && !IsReferenceV<Returned> && !detail::IsSpanV<Returned>,
            "World::With must not return pointers, references, or Spans into the borrowed "
            "object; copy the values out instead");
        using Output = typename detail::WithResult<Returned>::Type;
        ObjectAccessScope scope(Access());
        auto borrowed = objects_.Resolve<T>(Access(), handle);
        if (!borrowed) {
            return Output::Failure(borrowed.Error());
        }

        auto pointer = borrowed.Value().Get();
        if (!pointer) {
            return Output::Failure(pointer.Error());
        }

        if constexpr (IsSameV<Returned, void>) {
            function(*pointer.Value());
            return Output::Success();
        } else if constexpr (detail::IsResultV<Returned>) {
            return function(*pointer.Value());
        } else {
            return Output::Success(function(*pointer.Value()));
        }
    }

    // T(또는 파생) 객체의 Borrow다. 가능하면 With를 쓴다. 받은 Borrow는 Tick·Shutdown 전에
    // 반환하고, Get은 이 World의 문맥에서(또는 scope 없이) 호출한다.
    template <class T>
    Result<Borrow<T>> Resolve(ObjectHandle handle) noexcept {
        return objects_.Resolve<T>(Access(), handle);
    }

    // 이 World의 Registry다. 조회·진단용이며 Actor·Component·Level을 직접 파괴하지 않는다.
    ObjectRegistry& Objects() noexcept {
        return objects_;
    }

    ObjectAccessContext Access() const noexcept {
        return objects_.OwnerAccess();
    }

    // --- 시간·Tick·진단 ---

    GameClock& Clock() noexcept {
        return clock_;
    }

    const GameClock& Clock() const noexcept {
        return clock_;
    }

    // World pause다. pause 중에는 tickWhenPaused 등록만 unscaled delta로 실행한다. 디버그 단일 스텝
    // 프레임(Clock().RequestSingleStep())은 예외로 pause가 아닌 것처럼 실행한다.
    void SetPaused(bool paused) noexcept {
        clock_.SetPaused(paused);
    }

    bool IsPaused() const noexcept {
        return clock_.IsPaused();
    }

    TickScheduler& Ticks() noexcept {
        return ticks_;
    }

    // 마지막 Tick의 스텝 계획이다.
    const StepPlan& LastStep() const noexcept {
        return lastStep_;
    }

    WorldStats Stats() const noexcept;

    WorldKind Kind() const noexcept {
        return kind_;
    }

    // 바깥 계층 Subsystem을 키로 찾는다. 조회 창은 게임 진행이 끝날 때(Shutdown 시작) 닫히므로
    // PreDeinitialize·Deinitialize 안에서는 nullptr이다(바깥 Subsystem이 먼저 정리될 수 있다
    // — 16.3절).
    void* FindOuterSubsystem(SubsystemKey key) const noexcept {
        return outerOpen_ && outer_.find != nullptr ? outer_.find(outer_.context, key) : nullptr;
    }

    // 이 World Subsystem 묶음이 마지막 초기화에서 생략한 것이다(진단용).
    Span<const SkippedSubsystem> SkippedSubsystems() const noexcept {
        return subsystems_.Skipped();
    }

    template <class T>
    T* GetSubsystem() const noexcept {
        return subsystems_.template Get<T>();
    }

private:
    // CVar 범위를 파괴한다(Initialize 실패 되돌리기·Shutdown — 범위가 없으면 아무것도 하지 않는다).
    void ReleaseCVarScope() noexcept;

    friend class Actor;
    friend class Component;
    friend class SceneComponent;

    // World가 관리하는 Actor 수명 기록이다. Actor 저장소는 Level이, Component는 Actor가 소유한다.
    struct ActorRecord {
        explicit ActorRecord(Allocator& allocator) noexcept : components(allocator) {
        }

        ObjectHandle handle;
        ObjectHandle level;
        // AddComponent로 붙인 Component handle이다(Actor의 components_와 같은 순서). Actor가 World
        // API 밖에서 파괴되어 Borrow로 목록을 읽을 수 없어도 Component를 빠짐없이 정리하려고 따로
        // 둔다(Docs/Design/GarbageCollectionScale.ko.md 13.3절 — 고아 소속 방지).
        Array<ObjectHandle> components;
        ActorState state = ActorState::Constructed;
        // Level 클러스터의 소속이다(Component도 같은 클러스터에 넣는다).
        bool clustered = false;
        bool awake = false;
        bool enabledApplied = false;
        // FinishSpawning 진행 중(재진입 방지)과 활성 전환 중(재진입 hook의 중간 상태 노출
        // 방지)이다.
        bool finishing = false;
        bool transitioning = false;
        // 꺼진 채 시작 큐에서 빠졌다(활성화가 다시 넣는다).
        bool startDeferred = false;
        // Level의 actors_ 배열에서의 위치다(파괴가 끝 원소와 바꿔 O(1)로 뗀다 — Level 배열 순서는
        // 정하지 않는다).
        uint32 levelPosition = 0;
        TickHandle ticks[detail::TickHookCount];
    };

    // World가 만든 Component의 레코드다(componentRecords_). Component 객체가 World API 밖에서
    // 파괴되면 객체를 읽을 수 없으므로 정리에 필요한 것을 World 쪽에 둔다.
    struct ComponentRecord {
        ObjectHandle component;
        ObjectHandle actor;
        // Component의 ticks_ 사본이다(등록 때 맞춘다). 이미 해제된 handle이 남아도 해제는 세대로
        // 거부되므로 안전하다.
        TickHandle ticks[detail::TickHookCount];
        // 우회 파괴 정리가 파괴 큐에 넣었다(Tick 실행과 복구가 같은 Component를 두 번 넣지 않게).
        bool bypassQueued = false;
    };

    // Level 루트다. World가 보유하는 유일한 객체 루트다.
    struct LevelRecord {
        RootRef<Level> root;
        ObjectHandle handle;
        uint32 actorCount = 0;
        bool pendingDestroy = false;
        // Level이 클러스터 루트다(Level 타입이 완전한 필드 배치일 때 — GC-S7).
        bool clustered = false;
    };

    // hook 실행 깊이를 세는 범위다. 깊이가 0이 아니면 Initialize·BeginPlay·Tick·Shutdown은 Busy다.
    struct HookScope {
        explicit HookScope(World& world) noexcept : owner(world) {
            ++owner.hookDepth_;
        }

        ~HookScope() {
            --owner.hookDepth_;
        }

        HookScope(const HookScope&) = delete;
        HookScope& operator=(const HookScope&) = delete;

        World& owner;
    };

    void ExecuteTick(const TickTarget& target, const TickContext& context) noexcept override;
    Result<ObjectHandle> CreateLevelInternal(StringView name, bool persistent) noexcept;
    LevelRecord* FindLevelRecord(ObjectHandle level) noexcept;
    const LevelRecord* FindLevelRecord(ObjectHandle level) const noexcept;
    // 파괴 대기 Level 중 Actor가 모두 정리된 것을 회수한다.
    void FinishLevelDestroys() noexcept;
    void ReleaseLevel(usize index) noexcept;
    // 모든 Actor를 파괴 대기로 만들고 정리한다(hook 포함). 종료·초기화 실패 경로에서 쓴다.
    void DestroyAllActors() noexcept;

    // actor의 index번째 Component와 지금의 Component 수다. 순회는 count로 끝을 판정하고 null
    // component(로드된 목록의 null 원소)는 건너뛴다 — null을 끝 표지로 쓰지 않는다. hook이 목록을
    // 바꿀 수 있으므로 원소마다 다시 얻는다. Actor에 접근할 수 없으면 count는 0이다.
    struct ComponentSlot {
        ObjectHandle component;
        usize count = 0;
    };

    ActorRecord* FindActorRecord(ObjectHandle actor) noexcept;
    const ActorRecord* FindActorRecord(ObjectHandle actor) const noexcept;
    ComponentSlot ComponentSlotAt(ObjectHandle actor, usize index) noexcept;
    // actor의 index번째 Component다. 범위 밖이거나 Actor에 접근할 수 없으면 null handle이다.
    ObjectHandle ComponentAt(ObjectHandle actor, usize index) noexcept;
    usize ComponentCountOf(ObjectHandle actor) noexcept;
    // Component의 수명 기록 사본이다. 접근할 수 없으면 실패다.
    Result<ComponentState> ReadComponentState(ObjectHandle component) noexcept;

    // Component를 Actor 단계에 맞춰 OnRegister·OnAwake·OnEnable까지 진행한다. 실패한 Component는
    // 정리 대기로 만들고 그 오류를 반환한다.
    Result<void> AdvanceComponent(ObjectHandle actor, ObjectHandle component) noexcept;
    // Actor 단계에 아직 못 미친 Component를 모두 진행한다(hook이 추가한 Component 포함).
    void AdvanceComponents(ObjectHandle actor) noexcept;
    void EnableActor(ObjectHandle actor, bool enabled) noexcept;
    void EnableComponent(ObjectHandle component, bool enabled) noexcept;
    Result<void> SetActorEnabled(ObjectHandle actor, bool enabled) noexcept;
    Result<void> SetComponentEnabled(ObjectHandle component, bool enabled) noexcept;
    Result<void> SetActorTickEnabled(ObjectHandle actor, bool enabled) noexcept;
    Result<void> SetComponentTickEnabled(ObjectHandle component, bool enabled) noexcept;
    void RegisterTicks(ObjectHandle object, uint8 kind, const TickSettings& settings, bool enabled,
                       TickHandle (&handles)[detail::TickHookCount]) noexcept;
    void UnregisterTicks(TickHandle (&handles)[detail::TickHookCount]) noexcept;
    // Playing Component의 Tick을 등록하고 소유 Actor의 같은 hook Tick 뒤에 오도록 선행 의존을 건다.
    // 소유 Actor가 아직 Playing이 아니면 아무것도 하지 않는다(Actor 시작 때 등록된다).
    void RegisterComponentTicks(ObjectHandle component) noexcept;
    void SetTicksEnabled(TickHandle (&handles)[detail::TickHookCount], bool enabled) noexcept;
    // Component를 시작한다(OnStart -> Playing -> Tick 등록). 시작 대상이 아니면 아무것도 하지
    // 않는다.
    void StartComponent(ObjectHandle component) noexcept;
    // 다음 프레임 시작에 시작하도록 큐에 넣는다(중복 없이).
    void QueueComponentStart(ObjectHandle component) noexcept;
    void ProcessStarts() noexcept;
    void FlushDestroys() noexcept;
    // owner(actor의 Borrow 안)의 대표가 component면 Registry SetReference로 비운다. component가
    // null이면 아무것도 하지 않는다.
    void ClearRootComponent(ObjectHandle actor, Actor& owner, ObjectHandle component) noexcept;
    void DestroyComponentNow(ObjectHandle actor, ObjectHandle component,
                             bool ownerDestroyed) noexcept;
    void DestroyActorNow(ObjectHandle actor) noexcept;
    void StepGarbageCollection() noexcept;
    // STW 병렬 전체 수집(회수는 미룸)을 한다. 진행 중인 증분 cycle은 Marking이면 취소하고, 그
    // 뒤면 회수 없이 끝낸다. 수집하지 못했으면(Busy) 거짓이다.
    bool RunFullCollection() noexcept;
    // Registry가 Idle이고 회수 대기가 있으면 reclaimBudget만큼 회수한다(GC 설계 17.3절).
    void DrainDeferredReclaim() noexcept;
    // 전체 수집 뒤의 회수가 다 끝났으면 allocator의 TrimMemory를 한 번 부른다
    // (WorldGcSchedule::trimMemory). 회수가 끝나기 전에 전체 수집이 겹치면 합쳐서 한 번이다.
    void TrimAfterFullCollection() noexcept;
    // World가 소유한 객체의 RequestDestroy다. 결과를 확인한다(클러스터 이탈이 실패하면 고아 소속이
    // 남는다 — 13.3절). 이미 파괴됐거나 종료 중이면 무시하고, 그 밖의 실패는 불변식 위반으로
    // Fatal이다.
    void RequestDestroyOwned(ObjectHandle object) noexcept;
    // 우회 파괴된 Component를 파괴 큐에 한 번만 넣는다(Component 색인에 없거나 이미 넣었으면
    // 거짓).
    bool QueueBypassedComponent(ObjectHandle component) noexcept;
    // Component를 색인에서 빼고 사본 Tick 등록을 해제한다(World 정리가 회수 요청 직전에 부른다).
    void ForgetComponent(ObjectHandle component) noexcept;

    // --- Tick 의존 선언(WorldTickDependencies.cpp) ---

    struct TickDependency {
        TickPoint point;
        uint8 sources = 0;
    };

    // (객체, hook) 하나의 선언이다. 등록 여부와 무관하게 의존 선언 전체를 담아 순환 검사와 늦은
    // 등록의 적용에 쓴다. 해제된 객체를 가리키는 항목은 다음 선언·적용 때 지운다.
    struct TickDeclarations {
        explicit TickDeclarations(Allocator& allocator) noexcept
            : prerequisites(allocator), dependents(allocator) {
        }

        TickPoint self;
        Array<TickDependency> prerequisites;
        Array<TickPoint> dependents;
        uint32 mark = 0;
    };

    struct EngineStageSlot {
        EngineStageFunction function = nullptr;
        void* user = nullptr;
    };

    static uint64 TickPointKey(const TickPoint& point) noexcept;
    bool IsTickPointAlive(const TickPoint& point) const noexcept;
    TickHandle RegisteredTick(const TickPoint& point) const noexcept;
    // 이 World의 Actor·Component의 Tick 설정을 읽는다(아니면 StaleHandle).
    Result<TickSettings> ReadTickSettings(ObjectHandle object) noexcept;
    Result<void> WriteTickSettings(ObjectHandle object, const TickSettings& settings) noexcept;
    void PruneTickDeclarations(TickDeclarations& node) noexcept;
    Result<bool> TickDeclarationReaches(const TickPoint& from, const TickPoint& to) noexcept;
    void WarnTickCycle(const TickPoint& before, const TickPoint& after, bool automatic) noexcept;
    // 막 등록한 hook에 선언된 의존(양방향)을 스케줄러에 건다. handles는 그 객체의 등록 중인
    // 배열이다(같은 객체의 다른 hook을 여기서 찾는다).
    void ApplyTickDeclarations(ObjectHandle object, uint8 hook,
                               const TickHandle (&handles)[detail::TickHookCount]) noexcept;
    // 파괴 대기(PendingDestroy)인 Actor·Component다.
    bool IsTickTargetDestroying(ObjectHandle object) noexcept;
    // 파괴되는 객체의 선언을 지운다(다른 선언의 참조는 다음 정리 때 지운다).
    void ForgetTickDeclarations(ObjectHandle object) noexcept;
    // SceneComponent 부모가 바뀌었다(자식 hook → 부모 같은 hook 간선, UE AttachToComponent).
    void UpdateAttachmentTicks(ObjectHandle child, ObjectHandle oldParent,
                               ObjectHandle newParent) noexcept;
    Result<void> RunEngineStage(EngineStage stage, TickPhase phase,
                                const TickPhaseFrame& frame) noexcept;
    // tickable WorldSubsystem의 OnTick을 부른다(엔진 단계 TickableSubsystems의 World 부분).
    void RunTickableSubsystems(const TickPhaseFrame& frame) noexcept;
    // World API 밖에서 파괴(RequestDestroy·Object::Destroy)된 Actor·Component·Level을 찾아 World
    // 파괴로 돌린다(Actor는 DestroyActor, Component는 파괴 큐, Level은 DestroyLevel — 영구 Level은
    // Fatal). 하나라도 있으면 참.
    bool RepairBypassedObjects() noexcept;
    // Actor를 Level의 actors_에서 O(1)로 뗀다(끝 원소와 바꾼다).
    void DetachFromLevel(ObjectHandle level, const ActorRecord& record) noexcept;
    // RepairBypassedObjects가 파괴 요청 목록을 한 번에 꺼내는 수다.
    static constexpr usize RepairBatch = 64;
    // 새 객체의 타입이 클러스터 소속 조건(완전한 필드 배치)을 맞추는지다.
    static bool JoinsCluster(ConstructionContext& transaction, ObjectHandle object) noexcept;
    bool CanMutate() const noexcept;

    Allocator& allocator_;
    // 레코드의 RootRef보다 먼저 생성되고 나중에 파괴되어야 하므로 레코드보다 앞에 선언한다.
    ObjectRegistry objects_;
    String name_;
    GameClock clock_;
    TickScheduler ticks_;
    SubsystemCollection<WorldSubsystem, World> subsystems_;
    WorldKind kind_ = WorldKind::Game;
    OuterSubsystemLookup outer_;
    bool outerOpen_ = false;
    // IsTickable()이 참인 Subsystem이다(초기화 순서, Deinitialize 전에 비운다).
    Array<WorldSubsystem*> tickables_{allocator_};
    Span<const WorldSubsystemType> subsystemTypes_;
    TimeStepPolicy timeStep_;
    SlotArray<ActorRecord> actors_;
    HashMap<uint32, SlotHandle> actorIndex_;
    Array<LevelRecord> levels_;
    // Level 객체의 슬롯 번호 집합이다(값은 쓰지 않는다). 파괴 요청 목록에서 Level을 Level 수에
    // 비례하는 훑기 없이 가려낸다(RepairBypassedObjects).
    HashMap<uint32, uint32> levelSlots_;
    // Component 색인이다(슬롯 번호 → ComponentRecord). AddComponent가 넣고 World 정리가 회수 요청
    // 직전에 뺀다. 우회 파괴로 Borrow를 얻을 수 없는 Component의 소유 Actor·Tick 등록을 찾는다.
    HashMap<uint32, ComponentRecord> componentRecords_;
    ObjectHandle persistentLevel_;
    Array<ObjectHandle> pendingStart_;
    Array<ObjectHandle> pendingComponentStart_;
    Array<ObjectHandle> destroyActors_;
    Array<ObjectHandle> destroyComponents_;
    GcBudget gcBudget_;
    WorkerScheduler* gcScheduler_ = nullptr;
    // 설정 문맥과 그 안의 이 World 범위다(WorldDesc::settings — 없으면 Global).
    config::ConfigContext* settings_ = nullptr;
    cvar::CVarScopeId cvarScope_{};
    StepPlan lastStep_;
    WorldState state_ = WorldState::Created;
    uint32 hookDepth_ = 0;
    bool inTick_ = false;
    bool gcPending_ = false;
    bool destroyedDuringCycle_ = false;
    WorldGcSchedule gcSchedule_;
    // 마지막 전체 수집 뒤 실제 경과 시간(초)이다(FullCollection 간격 — Tick의 unscaled delta 합).
    double secondsSinceFullCollection_ = 0.0;
    // 이번 Tick에 넘긴 실제 경과 시간(초)이다.
    double lastRealDeltaSeconds_ = 0.0;
    // 전체 수집 요청 표지다(0·1, platform atomic — 다른 스레드가 세우고 게임 스레드가 소비한다).
    uint32 fullCollectionRequested_ = 0;
    // 회수 대기를 프레임마다 나눠 회수하는 중이다(끝날 때 한 번 기록한다).
    bool drainingDeferred_ = false;
    // 전체 수집을 했고 그 회수가 끝나면 TrimMemory를 부를 차례다.
    bool trimPending_ = false;
    uint32 componentCount_ = 0;
    uint64 spawnedTotal_ = 0;
    uint64 destroyedTotal_ = 0;
    uint64 phaseNanoseconds_[TickPhaseCount] = {};
    uint64 tickNanoseconds_ = 0;
    uint32 skippedTicks_ = 0;
    uint32 repairedBypasses_ = 0;
    // (객체 slot × 4 + hook) → 선언이다. 순환 검사 방문 번호와 탐색 스택은 재사용한다.
    HashMap<uint64, TickDeclarations> tickDeclarations_{allocator_};
    uint32 tickDeclarationMark_ = 0;
    Array<TickPoint> tickDeclarationStack_{allocator_};
    EngineStageSlot engineStages_[EngineStageCount];
    uint32 engineStageFailures_ = 0;
};

} // namespace rocks
