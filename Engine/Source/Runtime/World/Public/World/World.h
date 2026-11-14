#pragma once

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

namespace rocks {

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

    // World BeginPlay 안에서 Actor의 OnStart보다 먼저 호출된다(엔진 내부 단계 이름 BeginPlay가
    // Actor hook OnStart에 대응한다).
    virtual void OnWorldBeginPlay(World&) noexcept {
    }

    // World 종료 중(모든 Actor 정리 후) 초기화 역순으로 호출된다.
    virtual void Deinitialize(World&) noexcept {
    }
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
//   auto spinner = RegisterNativeClass<Spinner>(types);  // World.module.generated.h를 include한 TU
Result<void> RegisterWorldTypes(ModuleRegistration& registration) noexcept;

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
    // 프레임 끝 증분 GC 한 번의 예산이다.
    GcBudget gcBudget;
    // 초기화할 WorldSubsystem 종류다. 배열은 Initialize가 끝날 때까지 살아 있어야 한다.
    Span<const WorldSubsystemType> subsystems;
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
//   Tick: 같은 단계에서 A의 Tick 뒤에 C의 Tick
//   정리: C.OnDisable -> A.OnDisable -> C.OnDestroy -> A.OnDestroy -> C.OnUnregister
//   비활성 객체는 활성화될 때까지 OnStart를 미룬다. OnDestroy는 OnAwake가 성공한 객체만 받는다.
//
// 소유·수명 규약:
// - 외부 코드는 Actor·Component·Level의 ObjectHandle을 보관하고 필요할 때 With로 접근한다.
// RootRef를
//   따로 잡지 않는다(정리 후에도 저장소가 남는다). Resolve로 받은 Borrow는 Tick·Shutdown 전에
//   반환한다(Borrow가 남으면 GC가 미뤄지고 Shutdown이 Busy다. WorldStats::externalBorrows 참고).
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
// 부르는 TU는 <World/World.module.generated.h>(또는 그 타입의 <World/<Type>.generated.h>)를
// include한다 — 없으면 ObjectTypeIdOf·RegisterNativeClass의 static_assert가 안내한다.
//
// 스레드: 생성한 게임 스레드에서만 사용한다.
class World final : private TickExecutor {
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

    WorldState State() const noexcept {
        return state_;
    }

    StringView Name() const noexcept {
        return name_.View();
    }

    // --- Actor ---

    // T를 만들어 Level에 넣고 레코드에 등록한다(Constructed). OnAwake 전이므로 Component
    // 추가·설정을 할 수 있다. FinishSpawning까지 hook·Tick이 없다. Created 상태는 InvalidArgument,
    // T가 등록되지 않았으면 NotFound, 종료 중이면 ShuttingDown, params.level이 이 World의 Level이
    // 아니면 StaleHandle이다.
    template <class T>
    Result<ObjectHandle> SpawnActorDeferred(const SpawnParams& params = {}) noexcept {
        static_assert(__is_base_of(Actor, T));
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
        static_assert(__is_base_of(Actor, T));
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
        static_assert(__is_base_of(Component, T));
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
        static_assert(__is_base_of(Component, T));
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
    //       return scene.SetRelativeLocation(Vec3d(0.0, 0.0, 100.0));     // Result<void>
    //   });
    //   auto location = world.With<SceneComponent>(
    //       node, [](SceneComponent& scene) noexcept { return scene.WorldLocation(); });
    template <class T, class Function>
    auto With(ObjectHandle handle, Function&& function) noexcept {
        using Returned = decltype(function(DeclVal<T&>()));
        static_assert(
            !__is_pointer(Returned) && !__is_reference(Returned) && !detail::IsSpanV<Returned>,
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

        if constexpr (__is_same(Returned, void)) {
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

    template <class T>
    T* GetSubsystem() const noexcept {
        return subsystems_.template Get<T>();
    }

private:
    friend class Actor;
    friend class Component;

    // World가 관리하는 Actor 수명 기록이다. Actor 저장소는 Level이, Component는 Actor가 소유한다.
    struct ActorRecord {
        ObjectHandle handle;
        ObjectHandle level;
        ActorState state = ActorState::Constructed;
        bool awake = false;
        bool enabledApplied = false;
        // FinishSpawning 진행 중(재진입 방지)과 활성 전환 중(재진입 hook의 중간 상태 노출
        // 방지)이다.
        bool finishing = false;
        bool transitioning = false;
        TickHandle ticks[detail::TickHookCount];
    };

    // Level 루트다. World가 보유하는 유일한 객체 루트다.
    struct LevelRecord {
        RootRef<Level> root;
        ObjectHandle handle;
        uint32 actorCount = 0;
        bool pendingDestroy = false;
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
    bool CanMutate() const noexcept;

    Allocator& allocator_;
    // 레코드의 RootRef보다 먼저 생성되고 나중에 파괴되어야 하므로 레코드보다 앞에 선언한다.
    ObjectRegistry objects_;
    String name_;
    GameClock clock_;
    TickScheduler ticks_;
    SubsystemCollection<WorldSubsystem, World> subsystems_;
    Span<const WorldSubsystemType> subsystemTypes_;
    TimeStepPolicy timeStep_;
    SlotArray<ActorRecord> actors_;
    HashMap<uint32, SlotHandle> actorIndex_;
    Array<LevelRecord> levels_;
    ObjectHandle persistentLevel_;
    Array<ObjectHandle> pendingStart_;
    Array<ObjectHandle> pendingComponentStart_;
    Array<ObjectHandle> destroyActors_;
    Array<ObjectHandle> destroyComponents_;
    GcBudget gcBudget_;
    StepPlan lastStep_;
    WorldState state_ = WorldState::Created;
    uint32 hookDepth_ = 0;
    bool inTick_ = false;
    bool gcPending_ = false;
    bool destroyedDuringCycle_ = false;
    uint32 componentCount_ = 0;
    uint64 spawnedTotal_ = 0;
    uint64 destroyedTotal_ = 0;
    uint64 phaseNanoseconds_[TickPhaseCount] = {};
    uint64 tickNanoseconds_ = 0;
    uint32 skippedTicks_ = 0;
    uint32 repairedTicks_ = 0;
};

} // namespace rocks
