#pragma once

#include <Core/Containers/HashMap.h>
#include <Core/Text/String.h>
#include <Core/Time/GameClock.h>
#include <Core/Traits.h>
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

    // World 초기화 중 의존 순서대로 호출된다. 실패하면 World 초기화가 실패한다.
    virtual Result<void> Initialize(World&) noexcept {
        return Result<void>::Success();
    }

    // World BeginPlay 직후 호출된다.
    virtual void OnWorldBeginPlay(World&) noexcept {
    }

    // World 종료 중(모든 Actor 정리 후) 초기화 역순으로 호출된다.
    virtual void Deinitialize(World&) noexcept {
    }
};

using WorldSubsystemType = SubsystemType<WorldSubsystem>;

// World 모듈의 기본 타입(Actor·Component·SceneComponent)을 등록한다. 게임 타입보다 먼저 한 번
// 호출한다. 이미 등록된 타입이 있으면 AlreadyExists다.
Result<void> RegisterWorldTypes(TypeRegistry& types) noexcept;

// World 생성 설정이다.
// Actor Spawn 옵션이다.
struct SpawnParams {
    // 소속 Level이다. null이면 영구 Level이다. 파괴 대기 중인 Level이면 Spawn이 Destroyed로
    // 실패한다.
    ObjectHandle level;
};

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

} // namespace detail

// Actor의 실행 환경이다. 자기 ObjectRegistry(독립 실행 문맥), Level(Actor 소유 묶음), Actor 수명
// 기록, TickScheduler, 게임 시계, WorldSubsystem을 소유한다. GameDirector 없이도 동작한다.
//
// 소유 사슬: World → Level(RootRef) → Actor(Level의 ObjectRefArray) → Component(Actor의
// ObjectRefArray). World가 보유하는 객체 루트는 Level 루트뿐이다.
//
// 사용 예:
//   auto world = UniquePtr<World>::TryMake(allocator, allocator, types, desc);
//   world.Value()->Initialize();                       // WorldSubsystem
//   world.Value()->BeginPlay();                        // 게임플레이 시작
//   auto actor = world.Value()->SpawnActor<Spinner>();
//   while (running) { world.Value()->Tick(frameSeconds); }
//   world.Value()->Shutdown();                         // 모든 Actor 정리·Registry 종료
//
// 프레임(Tick) 순서:
//   1) 게임 시계 진행 → 2) 시작 대기 Actor/Component의 OnStart(Playing일 때) → 3) Tick 단계:
//   PrePhysics → Physics(고정 스텝 수만큼) → PostPhysics → LateUpdate → PreRender → 4) 파괴 대기
//   객체 정리(OnDisable·OnDestroy·OnUnregister·회수 요청) → 5) 증분 GC 한 조각.
//
// 소유·수명 규약:
// - Actor·Component의 RootRef는 World 레코드만 보관한다. 외부 코드는 ObjectHandle을 보관하고 필요할
//   때 With/Resolve로 접근하며, 받은 Borrow는 Tick·Shutdown 호출 전에 반환한다.
// - 파괴 요청(DestroyActor·DestroyComponent)은 즉시 메모리를 해제하지 않는다. 대상은 곧바로
// Tick에서
//   빠지고, 프레임 끝(4단계)에서 hook과 함께 정리된다. 같은 대상의 중복 요청은 성공이다.
// - Shutdown과 소멸은 Tick 밖에서만 한다(Tick·hook 안에서 호출하면 Busy).
// - 객체 hook·Tick 실행 중에는 World의 ObjectAccessScope가 설정된다. 다른 Registry 객체에
// 접근하려면
//   그 Registry의 scope를 중첩한다.
//
// 스레드: 생성한 게임 스레드에서만 사용한다.
class World final : private TickExecutor {
public:
    World(Allocator& allocator, TypeRegistry& types, const WorldDesc& desc) noexcept;
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    // Shutdown하지 않았으면 Shutdown을 시도한다. 외부가 Borrow·RootRef를 보유해 끝낼 수 없으면
    // Fatal.
    ~World();

    // WorldSubsystem을 초기화한다. Created에서만 호출할 수 있다(그 외 InvalidArgument). 실패하면
    // 초기화한 Subsystem을 역순 정리하고 Created에 머문다.
    Result<void> Initialize() noexcept;

    // 게임플레이를 시작한다. Initialized에서만 호출할 수 있다. 시작 대기 중인 준비된 Actor가
    // OnStart한다.
    Result<void> BeginPlay() noexcept;

    // 한 프레임을 실행한다(클래스 설명의 프레임 순서). Initialized·Playing에서만 가능하다. 재진입은
    // Busy.
    Result<void> Tick(double realDeltaSeconds) noexcept;

    // 모든 Actor를 정리하고 Subsystem·Registry를 닫는다. 여러 번 호출해도 안전하다(완료 후
    // Success). Tick 중이면 Busy. 외부가 Borrow·RootRef를 보유해 회수할 수 없으면 Busy이며
    // TearingDown에 머문다 (반환 후 다시 호출).
    Result<void> Shutdown() noexcept;

    WorldState State() const noexcept {
        return state_;
    }

    StringView Name() const noexcept {
        return name_.View();
    }

    // --- Actor ---

    // T를 만들어 Level에 넣고 레코드에 등록한다(Constructed). OnAwake 전이므로 Component
    // 추가·설정을 할 수 있다. FinishSpawning까지 hook·Tick이 없다. T가 등록되지 않았으면 NotFound,
    // 종료 중이면 ShuttingDown, params.level이 이 World의 Level이 아니면 StaleHandle이다.
    template <class T>
    Result<ObjectHandle> SpawnActorDeferred(const SpawnParams& params = {}) noexcept {
        static_assert(__is_base_of(Actor, T));
        return SpawnActorDeferred(NativeTypeIdOf<T>(), params);
    }

    // 런타임 TypeId로 만든다. 등록된 Actor 파생 타입이 아니면 TypeMismatch다.
    Result<ObjectHandle> SpawnActorDeferred(TypeId type, const SpawnParams& params = {}) noexcept;

    // Constructed Actor를 초기화한다: Component OnRegister → Actor OnAwake → Component OnAwake →
    // OnEnable(활성일 때). Actor OnAwake가 실패하면 그 오류를 반환하고 Actor는 OnDestroy 없이 정리
    // 대기한다. 이미 끝났으면 InvalidArgument, 파괴 대기면 Destroyed.
    Result<void> FinishSpawning(ObjectHandle actor) noexcept;

    // SpawnActorDeferred + FinishSpawning이다. FinishSpawning이 실패하면 오류를 반환한다.
    template <class T>
    Result<ObjectHandle> SpawnActor(const SpawnParams& params = {}) noexcept {
        static_assert(__is_base_of(Actor, T));
        return SpawnActor(NativeTypeIdOf<T>(), params);
    }

    Result<ObjectHandle> SpawnActor(TypeId type, const SpawnParams& params = {}) noexcept;

    // 파괴를 요청한다. 이미 요청됐으면 성공, 이 World의 Actor가 아니면 StaleHandle이다.
    Result<void> DestroyActor(ObjectHandle actor) noexcept;

    // 이 World에 레코드가 있는 Actor인지다(파괴 대기 포함, 정리 완료 후 false).
    bool HasActor(ObjectHandle actor) const noexcept;

    Result<ActorState> GetActorState(ObjectHandle actor) const noexcept;

    // 살아 있는 모든 Actor의 handle을 visitor(ObjectHandle)로 넘긴다. 순회 중 Spawn·Destroy 요청은
    // 허용하며 새 Actor는 이번 순회에 포함되지 않을 수 있다.
    template <class Visitor>
    void ForEachActor(Visitor&& visitor) noexcept {
        for (const ActorRecord& record : actors_) {
            visitor(record.handle);
        }
    }

    // actor가 속한 Level이다. 이 World의 Actor가 아니면 StaleHandle이다.
    Result<ObjectHandle> GetActorLevel(ObjectHandle actor) const noexcept;

    // --- Level ---

    // 영구 Level이다. Initialize 전에는 null handle이다. World가 끝날 때까지 파괴할 수 없다.
    ObjectHandle PersistentLevel() const noexcept {
        return persistentLevel_;
    }

    // 서브 Level을 만든다. Initialized·Playing에서만 가능하다(그 외 InvalidArgument·ShuttingDown).
    // name은 진단용이며 Level이 복사해 보관한다.
    Result<ObjectHandle> CreateLevel(StringView name) noexcept;

    // Level 파괴를 요청한다. 소속 Actor를 모두 DestroyActor한 뒤 프레임 끝 정리에서 Actor가 모두
    // 정리되면 Level을 회수한다. 영구 Level은 InvalidArgument, 이미 요청됐으면 성공, 모르는
    // handle은 StaleHandle.
    Result<void> DestroyLevel(ObjectHandle level) noexcept;

    // 이 World에 레코드가 있는 Level인지다(파괴 대기 포함).
    bool HasLevel(ObjectHandle level) const noexcept;

    // 모든 Level handle을 visitor(ObjectHandle)로 넘긴다. 첫 번째는 영구 Level이다.
    template <class Visitor>
    void ForEachLevel(Visitor&& visitor) noexcept {
        for (const LevelRecord& record : levels_) {
            visitor(record.handle);
        }
    }

    // --- Component ---

    // actor에 T를 추가한다. Actor가 Constructed이면 FinishSpawning 때, 이미 초기화됐으면 즉시
    // OnRegister·OnAwake·OnEnable을 부르고 다음 프레임 시작에 OnStart한다. OnRegister·OnAwake가
    // 실패하면 그 오류를 반환한다. 파괴 대기 Actor는 Destroyed다.
    template <class T>
    Result<ObjectHandle> AddComponent(ObjectHandle actor) noexcept {
        static_assert(__is_base_of(Component, T));
        return AddComponent(actor, NativeTypeIdOf<T>());
    }

    Result<ObjectHandle> AddComponent(ObjectHandle actor, TypeId type) noexcept;

    Result<void> DestroyComponent(ObjectHandle component) noexcept;

    Result<ComponentState> GetComponentState(ObjectHandle component) noexcept;

    // actor가 가진 Component 중 T(또는 파생)인 첫 번째다. 없으면 null handle이다.
    template <class T>
    ObjectHandle FindComponent(ObjectHandle actor) noexcept {
        static_assert(__is_base_of(Component, T));
        for (usize index = 0;; ++index) {
            const ObjectHandle component = ComponentAt(actor, index);
            if (component.IsNull()) {
                return {};
            }
            if (objects_.Resolve<T>(Access(), component).IsOk()) {
                return component;
            }
        }
    }

    // actor의 Component 수와 handle이다. out이 작으면 앞부분만 채우고 전체 수를 반환한다.
    Result<uint32> GetComponents(ObjectHandle actor, Span<ObjectHandle> out) noexcept;

    // --- 객체 접근 ---

    // T(또는 파생) 객체의 Borrow다. 반환 전에 Tick·Shutdown을 호출하지 않는다.
    template <class T>
    Result<Borrow<T>> Resolve(ObjectHandle handle) noexcept {
        return objects_.Resolve<T>(Access(), handle);
    }

    // handle 객체를 T&로 빌려 function을 호출하고 결과를 돌려준다. function이 void면 Result<void>,
    // Result<X>면 그대로, X면 Result<X>를 반환한다. 대상이 없거나 타입이 다르면 그 오류다.
    template <class T, class Function>
    auto With(ObjectHandle handle, Function&& function) noexcept {
        using Returned = decltype(function(DeclVal<T&>()));
        using Output = typename detail::WithResult<Returned>::Type;
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

    // World pause다. pause 중에는 tickWhenPaused 등록만 unscaled delta로 실행한다.
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
        TickHandle ticks[detail::TickHookCount];
    };

    // Level 루트다. World가 보유하는 유일한 객체 루트다.
    struct LevelRecord {
        RootRef<Level> root;
        ObjectHandle handle;
        bool pendingDestroy = false;
    };

    void ExecuteTick(const TickTarget& target, const TickContext& context) noexcept override;
    Result<ObjectHandle> CreateLevelInternal(StringView name, bool persistent) noexcept;
    LevelRecord* FindLevelRecord(ObjectHandle level) noexcept;
    const LevelRecord* FindLevelRecord(ObjectHandle level) const noexcept;
    // 파괴 대기 Level 중 Actor가 모두 정리된 것을 회수한다.
    void FinishLevelDestroys() noexcept;
    void ReleaseLevel(usize index) noexcept;

    ActorRecord* FindActorRecord(ObjectHandle actor) noexcept;
    const ActorRecord* FindActorRecord(ObjectHandle actor) const noexcept;
    // actor의 index번째 Component다. 범위 밖이거나 Actor에 접근할 수 없으면 null handle이다.
    ObjectHandle ComponentAt(ObjectHandle actor, usize index) noexcept;
    usize ComponentCountOf(ObjectHandle actor) noexcept;
    // Component의 수명 기록 사본이다. 접근할 수 없으면 실패다.
    Result<ComponentState> ReadComponentState(ObjectHandle component) noexcept;
    Result<void> SetComponentState(ObjectHandle component, ComponentState state) noexcept;

    Result<void> InitializeComponent(ObjectHandle actor, ObjectHandle component) noexcept;
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
    void ProcessStarts() noexcept;
    void FlushDestroys() noexcept;
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
    bool inTick_ = false;
    bool gcPending_ = false;
    bool destroyedDuringCycle_ = false;
    uint32 componentCount_ = 0;
    uint64 spawnedTotal_ = 0;
    uint64 destroyedTotal_ = 0;
    uint64 phaseNanoseconds_[TickPhaseCount] = {};
    uint64 tickNanoseconds_ = 0;
    uint32 skippedTicks_ = 0;
};

} // namespace rocks
