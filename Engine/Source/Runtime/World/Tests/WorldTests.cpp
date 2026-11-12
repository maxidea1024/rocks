#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <TestSupport/Test.h>
#include <World/SceneComponent.h>
#include <World/World.h>
#include <stdio.h>
#include <string.h>

using namespace rocks;

namespace {

// --- hook 기록 ---

// 기록은 문자열 리터럴(정적 수명)만 받는다.
constexpr uint32 MaxEvents = 128;
const char* events[MaxEvents] = {};
uint32 eventCount = 0;

void Record(const char* text) noexcept {
    if (eventCount < MaxEvents) {
        events[eventCount++] = text;
    }
}

void ResetEvents() noexcept {
    eventCount = 0;
}

// 기록된 순서가 expected와 같은지 본다. 다르면 실제 기록을 출력한다.
template <usize Count>
bool EventsAre(const char* const (&expected)[Count]) noexcept {
    bool same = eventCount == Count;
    for (uint32 index = 0; same && index < Count; ++index) {
        same = strcmp(events[index], expected[index]) == 0;
    }
    if (!same) {
        for (uint32 index = 0; index < eventCount; ++index) {
            printf("  event[%u] = %s\n", index, events[index]);
        }
    }
    return same;
}

uint32 CountEvent(const char* text) noexcept {
    uint32 count = 0;
    for (uint32 index = 0; index < eventCount; ++index) {
        count += strcmp(events[index], text) == 0 ? 1u : 0u;
    }
    return count;
}

// --- 시험용 타입 ---

class LoggingComponent : public Component {
    ROCKS_NATIVE_TYPE(LoggingComponent, Component, 0xfb5782f76d854126ull, 0x931d5877944c5b96ull)

public:
    LoggingComponent() noexcept {
        PrimaryTick().tick = true;
    }

    Result<void> OnAwake() noexcept override {
        Record("c.awake");
        return Result<void>::Success();
    }

    void OnEnable() noexcept override {
        Record("c.enable");
    }

    void OnStart() noexcept override {
        Record("c.start");
    }

    void OnTick(const TickContext&) noexcept override {
        Record("c.tick");
    }

    void OnDisable() noexcept override {
        Record("c.disable");
    }

    void OnDestroy() noexcept override {
        Record("c.destroy");
    }

protected:
    Result<void> OnRegister() noexcept override {
        Record("c.register");
        return Result<void>::Success();
    }

    void OnUnregister() noexcept override {
        Record("c.unregister");
    }
};

class LoggingActor : public Actor {
    ROCKS_NATIVE_TYPE(LoggingActor, Actor, 0xf15fcef171ac4370ull, 0xa61f10f1f070605aull)

public:
    explicit LoggingActor(Allocator& allocator) noexcept : Actor(allocator) {
        PrimaryTick().tick = true;
    }

    Result<void> OnAwake() noexcept override {
        Record("a.awake");
        if (failAwake) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }
        return Result<void>::Success();
    }

    void OnEnable() noexcept override {
        Record("a.enable");
    }

    void OnStart() noexcept override {
        Record("a.start");
    }

    void OnTick(const TickContext&) noexcept override {
        Record("a.tick");
        ++ticks;
        if (!victim.IsNull()) {
            CHECK(GetWorld().DestroyActor(victim).IsOk());
            victim = {};
        }
        if (destroySelf) {
            CHECK(Destroy().IsOk());
        }
    }

    void OnDisable() noexcept override {
        Record("a.disable");
    }

    void OnDestroy() noexcept override {
        Record("a.destroy");
    }

    bool IsReadyToStart() const noexcept override {
        return ready;
    }

    static inline bool failAwake = false;
    uint32 ticks = 0;
    ObjectHandle victim;
    bool destroySelf = false;
    bool ready = true;
};

// 고정·Late Tick과 pause 정책 시험용이다.
class PhaseActor final : public Actor {
    ROCKS_NATIVE_TYPE(PhaseActor, Actor, 0xbc5246fdc7e44d63ull, 0xa642a8413ec8877eull)

public:
    explicit PhaseActor(Allocator& allocator) noexcept : Actor(allocator) {
        PrimaryTick().tick = true;
        PrimaryTick().fixedTick = true;
        PrimaryTick().lateTick = true;
    }

    void OnTick(const TickContext&) noexcept override {
        ++updates;
    }

    void OnFixedTick(const TickContext& context) noexcept override {
        ++fixedUpdates;
        lastFixedDelta = context.deltaSeconds;
    }

    void OnLateTick(const TickContext&) noexcept override {
        ++lateUpdates;
    }

    uint32 updates = 0;
    uint32 fixedUpdates = 0;
    uint32 lateUpdates = 0;
    double lastFixedDelta = 0.0;
};

class PausedTicker final : public Actor {
    ROCKS_NATIVE_TYPE(PausedTicker, Actor, 0x0d081e2bd12d4e0bull, 0x86101adb6a8f75f8ull)

public:
    explicit PausedTicker(Allocator& allocator) noexcept : Actor(allocator) {
        PrimaryTick().tick = true;
        PrimaryTick().tickWhenPaused = true;
    }

    void OnTick(const TickContext& context) noexcept override {
        ++ticks;
        lastDelta = context.deltaSeconds;
    }

    uint32 ticks = 0;
    double lastDelta = 0.0;
};

struct OrderLog {
    char names[8] = {};
    uint32 count = 0;
};

OrderLog subsystemLog;

class FirstSubsystem final : public WorldSubsystem {
public:
    Result<void> Initialize(World&) noexcept override {
        subsystemLog.names[subsystemLog.count++] = 'F';
        return Result<void>::Success();
    }

    void Deinitialize(World&) noexcept override {
        subsystemLog.names[subsystemLog.count++] = 'f';
    }
};

class SecondSubsystem final : public WorldSubsystem {
public:
    Result<void> Initialize(World& world) noexcept override {
        // 의존 Subsystem은 먼저 초기화되어 있다.
        CHECK(world.GetSubsystem<FirstSubsystem>() != nullptr);
        subsystemLog.names[subsystemLog.count++] = 'S';
        return Result<void>::Success();
    }

    void OnWorldBeginPlay(World&) noexcept override {
        subsystemLog.names[subsystemLog.count++] = 'B';
    }

    void Deinitialize(World&) noexcept override {
        subsystemLog.names[subsystemLog.count++] = 's';
    }
};

bool RegisterTestTypes(TypeRegistry& types) noexcept {
    return RegisterWorldTypes(types).IsOk() &&
           RegisterNativeClass<LoggingComponent>(types).IsOk() &&
           RegisterNativeClass<LoggingActor>(types).IsOk() &&
           RegisterNativeClass<PhaseActor>(types).IsOk() &&
           RegisterNativeClass<PausedTicker>(types).IsOk();
}

// 시험마다 같은 순서로 만드는 환경이다. World는 TypeRegistry보다 먼저 파괴된다.
struct Fixture {
    explicit Fixture(uint64 context = 1, Span<const WorldSubsystemType> subsystems = {}) noexcept
        : types(allocator) {
        registered = RegisterTestTypes(types);
        WorldDesc desc;
        desc.name = "Test";
        desc.context = {0x574f524c44ull, context};
        desc.timeStep = {1.0 / 60.0, 0.25, 8};
        desc.subsystems = subsystems;
        auto created = UniquePtr<World>::TryMake(allocator, allocator, types, desc);
        if (created) {
            world = Move(created.Value());
        }
    }

    ~Fixture() {
        world.Reset();
    }

    template <class T>
    T* Pointer(ObjectHandle handle, Borrow<T>& borrow) noexcept {
        auto resolved = world->Resolve<T>(handle);
        if (!resolved) {
            return nullptr;
        }
        borrow = Move(resolved.Value());
        auto pointer = borrow.Get();
        return pointer ? pointer.Value() : nullptr;
    }

    SystemAllocator allocator;
    TypeRegistry types;
    bool registered = false;
    UniquePtr<World> world;
};

} // namespace

TEST(World_LifecycleHooksFollowDocumentedOrder) {
    ResetEvents();
    Fixture fixture;
    REQUIRE(fixture.registered && fixture.world.Get() != nullptr);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto actor = world.SpawnActorDeferred<LoggingActor>();
    REQUIRE(actor);
    auto component = world.AddComponent<LoggingComponent>(actor.Value());
    REQUIRE(component);
    // 지연 Spawn 중에는 hook이 없다.
    CHECK(eventCount == 0);
    CHECK(world.GetActorState(actor.Value()).Value() == ActorState::Constructed);
    REQUIRE(world.FinishSpawning(actor.Value()));
    CHECK(world.GetActorState(actor.Value()).Value() == ActorState::Initialized);
    REQUIRE(world.BeginPlay());
    CHECK(world.GetActorState(actor.Value()).Value() == ActorState::Playing);
    REQUIRE(world.Tick(1.0 / 60.0));
    REQUIRE(world.DestroyActor(actor.Value()));
    CHECK(world.DestroyActor(actor.Value()).IsOk()); // 중복 요청은 성공
    REQUIRE(world.Tick(1.0 / 60.0));
    const char* const expected[] = {"c.register", "a.awake",     "c.awake",   "a.enable",
                                    "c.enable",   "c.start",     "a.start",   "a.tick",
                                    "c.tick",     "c.disable",   "a.disable", "c.destroy",
                                    "a.destroy",  "c.unregister"};
    CHECK(EventsAre(expected));
    CHECK(!world.HasActor(actor.Value()));
    CHECK(world.Stats().actors == 0 && world.Stats().components == 0);
    REQUIRE(world.Shutdown());
}

TEST(World_DeferredActorDoesNotTickUntilFinishedAndStarted) {
    ResetEvents();
    Fixture fixture;
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto actor = world.SpawnActorDeferred<LoggingActor>();
    REQUIRE(actor);
    REQUIRE(world.Tick(0.016));
    CHECK(CountEvent("a.tick") == 0);
    REQUIRE(world.FinishSpawning(actor.Value()));
    CHECK(world.FinishSpawning(actor.Value()).Error() == ErrorCode::InvalidArgument);
    // FinishSpawning 직후에는 아직 시작 전이다. 다음 프레임 시작에서 OnStart 후 Tick한다.
    CHECK(world.GetActorState(actor.Value()).Value() == ActorState::Initialized);
    REQUIRE(world.Tick(0.016));
    CHECK(CountEvent("a.start") == 1 && CountEvent("a.tick") == 1);
    REQUIRE(world.Shutdown());
}

TEST(World_DestroyDuringTickSkipsLaterTicksAndReclaimsStorage) {
    ResetEvents();
    Fixture fixture;
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto killer = world.SpawnActor<LoggingActor>();
    auto victim = world.SpawnActor<LoggingActor>();
    auto selfDestroyer = world.SpawnActor<LoggingActor>();
    REQUIRE(killer && victim && selfDestroyer);
    {
        Borrow<LoggingActor> borrow;
        LoggingActor* pointer = fixture.Pointer(killer.Value(), borrow);
        REQUIRE(pointer != nullptr);
        pointer->victim = victim.Value();
        Borrow<LoggingActor> selfBorrow;
        LoggingActor* self = fixture.Pointer(selfDestroyer.Value(), selfBorrow);
        REQUIRE(self != nullptr);
        self->destroySelf = true;
    }
    REQUIRE(world.Tick(0.016));
    // killer가 먼저 등록되어 먼저 실행되고, 같은 단계의 뒤쪽 victim은 Tick하지 않는다.
    CHECK(CountEvent("a.tick") == 2);
    CHECK(world.HasActor(killer.Value()));
    CHECK(!world.HasActor(victim.Value()) && !world.HasActor(selfDestroyer.Value()));
    CHECK(world.Stats().destroyedTotal == 2);
    // 증분 GC가 몇 프레임에 걸쳐 저장소를 회수한다.
    for (int frame = 0; frame < 20; ++frame) {
        REQUIRE(world.Tick(0.016));
    }
    // 영구 Level과 killer만 남는다.
    CHECK(world.Objects().Stats().liveObjects == 2);
    CHECK(world.Resolve<Actor>(victim.Value()).Error() == ErrorCode::StaleHandle);
    REQUIRE(world.Shutdown());
}

TEST(World_ComponentAddedToPlayingActorStartsNextFrame) {
    ResetEvents();
    Fixture fixture;
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto actor = world.SpawnActor<LoggingActor>();
    REQUIRE(actor);
    REQUIRE(world.Tick(0.016));
    ResetEvents();
    auto component = world.AddComponent<LoggingComponent>(actor.Value());
    REQUIRE(component);
    const char* const added[] = {"c.register", "c.awake", "c.enable"};
    CHECK(EventsAre(added));
    CHECK(world.GetComponentState(component.Value()).Value() == ComponentState::Initialized);
    REQUIRE(world.Tick(0.016));
    CHECK(CountEvent("c.start") == 1 && CountEvent("c.tick") == 1);
    CHECK(world.FindComponent<LoggingComponent>(actor.Value()) == component.Value());
    CHECK(world.FindComponent<SceneComponent>(actor.Value()).IsNull());
    // Component만 파괴하면 Actor는 남는다.
    ResetEvents();
    REQUIRE(world.DestroyComponent(component.Value()));
    REQUIRE(world.Tick(0.016));
    const char* const removed[] = {"a.tick", "c.disable", "c.destroy", "c.unregister"};
    CHECK(EventsAre(removed));
    CHECK(world.Stats().components == 0 && world.HasActor(actor.Value()));
    REQUIRE(world.Shutdown());
}

TEST(World_ComponentsAreOwnedByActorThroughGcEdges) {
    Fixture fixture;
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto actor = world.SpawnActorDeferred<Actor>();
    REQUIRE(actor);
    auto first = world.AddComponent<SceneComponent>(actor.Value());
    auto second = world.AddComponent<Component>(actor.Value());
    REQUIRE(first && second);
    // World는 Level 루트만 보유한다. Actor는 Level의, Component는 Actor의 간선으로 살아 있어야
    // 한다.
    REQUIRE(world.Objects().Collect(world.Access()));
    CHECK(world.Objects().Stats().liveObjects == 4);
    CHECK(world.Objects().Stats().roots == 1);
    ObjectHandle handles[4];
    auto listed = world.GetComponents(actor.Value(), handles);
    REQUIRE(listed);
    CHECK(listed.Value() == 2 && handles[0] == first.Value() && handles[1] == second.Value());
    REQUIRE(world.Shutdown());
    CHECK(fixture.world->State() == WorldState::Destroyed);
}

TEST(World_AwakeFailureFailsSpawnWithoutOnDestroy) {
    ResetEvents();
    Fixture fixture;
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    LoggingActor::failAwake = true;
    auto spawned = world.SpawnActor<LoggingActor>();
    LoggingActor::failAwake = false;
    CHECK(spawned.Error() == ErrorCode::DependencyFailed);
    REQUIRE(world.Tick(0.016));
    CHECK(CountEvent("a.awake") == 1 && CountEvent("a.destroy") == 0);
    CHECK(world.Stats().actors == 0);
    REQUIRE(world.Shutdown());
}

TEST(World_NotReadyActorWaitsWithoutBlocking) {
    ResetEvents();
    Fixture fixture;
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto actor = world.SpawnActorDeferred<LoggingActor>();
    REQUIRE(actor);
    {
        Borrow<LoggingActor> borrow;
        LoggingActor* pointer = fixture.Pointer(actor.Value(), borrow);
        REQUIRE(pointer != nullptr);
        pointer->ready = false;
    }
    REQUIRE(world.FinishSpawning(actor.Value()));
    for (int frame = 0; frame < 3; ++frame) {
        REQUIRE(world.Tick(0.016));
    }
    CHECK(CountEvent("a.start") == 0 && world.Stats().waitingToStart == 1);
    {
        Borrow<LoggingActor> borrow;
        LoggingActor* pointer = fixture.Pointer(actor.Value(), borrow);
        REQUIRE(pointer != nullptr);
        pointer->ready = true;
    }
    REQUIRE(world.Tick(0.016));
    CHECK(CountEvent("a.start") == 1 && world.Stats().waitingToStart == 0);
    REQUIRE(world.Shutdown());
}

TEST(World_FixedAndLateTicksFollowClock) {
    Fixture fixture;
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto actor = world.SpawnActor<PhaseActor>();
    REQUIRE(actor);
    // 첫 Tick 시작에서 OnStart 후 같은 프레임부터 Tick한다. 30FPS 프레임 3번 = 60Hz 고정 스텝 6번.
    REQUIRE(world.Tick(1.0 / 30.0));
    REQUIRE(world.Tick(1.0 / 30.0));
    REQUIRE(world.Tick(1.0 / 30.0));
    Borrow<PhaseActor> borrow;
    PhaseActor* pointer = fixture.Pointer(actor.Value(), borrow);
    REQUIRE(pointer != nullptr);
    CHECK(pointer->updates == 3 && pointer->lateUpdates == 3);
    CHECK(pointer->fixedUpdates == 6);
    CHECK(pointer->lastFixedDelta > 0.0166 && pointer->lastFixedDelta < 0.0167);
    borrow.Reset();
    REQUIRE(world.Shutdown());
}

TEST(World_PauseRunsOnlyPausedTickersAndSingleStepRunsGameplay) {
    Fixture fixture;
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto normal = world.SpawnActor<PhaseActor>();
    auto ticker = world.SpawnActor<PausedTicker>();
    REQUIRE(normal && ticker);
    REQUIRE(world.Tick(0.016));
    world.SetPaused(true);
    REQUIRE(world.Tick(0.016));
    REQUIRE(world.Tick(0.016));
    {
        Borrow<PhaseActor> phaseBorrow;
        Borrow<PausedTicker> tickerBorrow;
        PhaseActor* phase = fixture.Pointer(normal.Value(), phaseBorrow);
        PausedTicker* paused = fixture.Pointer(ticker.Value(), tickerBorrow);
        REQUIRE(phase != nullptr && paused != nullptr);
        // 첫 프레임(0.016초 < 고정 스텝)에는 가변 Tick만 돌고, pause 프레임 두 번은 아무것도 돌지
        // 않는다.
        CHECK(phase->updates == 1 && phase->fixedUpdates == 0);
        CHECK(paused->ticks == 3 && paused->lastDelta > 0.0159);
    }
    // 단일 스텝 프레임은 pause가 아닌 것처럼 일반 Tick과 고정 스텝 한 번을 실행한다.
    REQUIRE(world.Clock().RequestSingleStep());
    REQUIRE(world.Tick(0.016));
    {
        Borrow<PhaseActor> phaseBorrow;
        PhaseActor* phase = fixture.Pointer(normal.Value(), phaseBorrow);
        REQUIRE(phase != nullptr);
        CHECK(phase->updates == 2 && phase->fixedUpdates == 1);
    }
    REQUIRE(world.Shutdown());
}

TEST(World_DisabledActorStopsTickingAndGetsEnableHooks) {
    ResetEvents();
    Fixture fixture;
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto actor = world.SpawnActorDeferred<LoggingActor>();
    REQUIRE(actor);
    REQUIRE(world.AddComponent<LoggingComponent>(actor.Value()));
    REQUIRE(world.FinishSpawning(actor.Value()));
    REQUIRE(world.Tick(0.016));
    ResetEvents();
    auto disabled = world.With<Actor>(
        actor.Value(), [](Actor& value) noexcept { return value.SetEnabled(false); });
    REQUIRE(disabled);
    REQUIRE(world.Tick(0.016));
    const char* const off[] = {"c.disable", "a.disable"};
    CHECK(EventsAre(off));
    ResetEvents();
    auto enabled = world.With<Actor>(actor.Value(),
                                     [](Actor& value) noexcept { return value.SetEnabled(true); });
    REQUIRE(enabled);
    REQUIRE(world.Tick(0.016));
    const char* const on[] = {"a.enable", "c.enable", "a.tick", "c.tick"};
    CHECK(EventsAre(on));
    REQUIRE(world.Shutdown());
}

TEST(World_TransformHierarchyComposesAndRejectsCycles) {
    Fixture fixture;
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto actor = world.SpawnActorDeferred<Actor>();
    REQUIRE(actor);
    auto root = world.AddComponent<SceneComponent>(actor.Value());
    auto child = world.AddComponent<SceneComponent>(actor.Value());
    auto grandchild = world.AddComponent<SceneComponent>(actor.Value());
    REQUIRE(root && child && grandchild);
    auto setRoot = world.With<Actor>(
        actor.Value(), [&](Actor& value) noexcept { return value.SetRootComponent(root.Value()); });
    REQUIRE(setRoot);
    auto attach = [&](ObjectHandle node, ObjectHandle parent) noexcept {
        return world.With<SceneComponent>(
            node, [parent](SceneComponent& scene) noexcept { return scene.AttachTo(parent); });
    };
    auto move = [&](ObjectHandle node, double x, double y, double z) noexcept {
        return world.With<SceneComponent>(node, [=](SceneComponent& scene) noexcept {
            return scene.SetRelativeLocation(Vec3d(x, y, z));
        });
    };
    auto location = [&](ObjectHandle node) noexcept {
        return world
            .With<SceneComponent>(
                node, [](SceneComponent& scene) noexcept { return scene.WorldLocation(); })
            .Value();
    };
    REQUIRE(attach(child.Value(), root.Value()));
    REQUIRE(attach(grandchild.Value(), child.Value()));
    REQUIRE(move(root.Value(), 10.0, 0.0, 0.0));
    REQUIRE(move(child.Value(), 0.0, 5.0, 0.0));
    REQUIRE(move(grandchild.Value(), 0.0, 0.0, 1.0));
    CHECK(location(grandchild.Value()) == Vec3d(10.0, 5.0, 1.0));
    // 조상을 옮기면 이미 계산된 자손도 다시 계산된다(dirty 전파).
    REQUIRE(move(root.Value(), 20.0, 0.0, 0.0));
    CHECK(location(grandchild.Value()) == Vec3d(20.0, 5.0, 1.0));
    // 순환과 자기 자신 부착은 거부하고 계층을 유지한다.
    CHECK(attach(root.Value(), grandchild.Value()).Error() == ErrorCode::InvalidArgument);
    CHECK(attach(root.Value(), root.Value()).Error() == ErrorCode::InvalidArgument);
    CHECK(location(grandchild.Value()) == Vec3d(20.0, 5.0, 1.0));
    // SceneComponent가 아닌 대상은 TypeMismatch다.
    auto plain = world.AddComponent<Component>(actor.Value());
    REQUIRE(plain);
    CHECK(attach(child.Value(), plain.Value()).Error() == ErrorCode::TypeMismatch);
    // 중간 노드를 파괴하면 자식은 루트가 되고 상대 위치를 유지한다.
    REQUIRE(world.FinishSpawning(actor.Value()));
    REQUIRE(world.DestroyComponent(child.Value()));
    REQUIRE(world.Tick(0.016));
    CHECK(location(grandchild.Value()) == Vec3d(0.0, 0.0, 1.0));
    auto parent = world.With<SceneComponent>(
        grandchild.Value(), [](SceneComponent& scene) noexcept { return scene.Parent(); });
    CHECK(parent && parent.Value().IsNull());
    REQUIRE(world.Shutdown());
}

TEST(World_MultipleWorldsAreIsolated) {
    ResetEvents();
    Fixture first(10);
    SystemAllocator allocator;
    WorldDesc desc;
    desc.name = "Second";
    desc.context = {0x574f524c44ull, 11};
    auto created = UniquePtr<World>::TryMake(allocator, allocator, first.types, desc);
    REQUIRE(created);
    World& a = *first.world;
    World& b = *created.Value();
    REQUIRE(a.Initialize() && b.Initialize());
    REQUIRE(a.BeginPlay() && b.BeginPlay());
    auto inA = a.SpawnActor<LoggingActor>();
    REQUIRE(inA);
    // 다른 World의 handle은 그 World의 레코드·Registry에 없다.
    CHECK(!b.HasActor(inA.Value()));
    CHECK(b.DestroyActor(inA.Value()).Error() == ErrorCode::StaleHandle);
    CHECK(b.Resolve<Actor>(inA.Value()).Error() == ErrorCode::StaleHandle);
    REQUIRE(b.Tick(0.016));
    CHECK(CountEvent("a.tick") == 0);
    REQUIRE(a.Tick(0.016));
    CHECK(CountEvent("a.tick") == 1);
    REQUIRE(b.Shutdown());
    created.Value().Reset();
    REQUIRE(a.Shutdown());
}

TEST(World_SubsystemsInitializeInDependencyOrderAndShutdownReversed) {
    subsystemLog = {};
    static const SubsystemKey secondDependencies[] = {SubsystemKeyOf<FirstSubsystem>()};
    const WorldSubsystemType types[] = {
        MakeSubsystemType<WorldSubsystem, SecondSubsystem>("Second", secondDependencies),
        MakeSubsystemType<WorldSubsystem, FirstSubsystem>("First")};
    Fixture fixture(20, types);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    REQUIRE(world.Shutdown());
    CHECK(subsystemLog.count == 5);
    CHECK(memcmp(subsystemLog.names, "FSBsf", 5) == 0);
}

TEST(World_ShutdownRejectsSpawnAndReleasesAllMemory) {
    SystemAllocator outer;
    {
        Fixture fixture;
        World& world = *fixture.world;
        REQUIRE(world.Initialize());
        REQUIRE(world.BeginPlay());
        for (int index = 0; index < 50; ++index) {
            auto actor = world.SpawnActor<LoggingActor>();
            REQUIRE(actor);
            REQUIRE(world.AddComponent<SceneComponent>(actor.Value()));
        }
        REQUIRE(world.Tick(0.016));
        REQUIRE(world.Shutdown());
        CHECK(world.State() == WorldState::Destroyed);
        CHECK(world.SpawnActor<LoggingActor>().Error() == ErrorCode::ShuttingDown);
        CHECK(world.Tick(0.016).Error() == ErrorCode::ShuttingDown);
        CHECK(world.Shutdown().IsOk());
        fixture.world.Reset();
        CHECK(fixture.allocator.Stats().liveAllocations != 0); // TypeRegistry는 아직 살아 있다
    }
    (void)outer;
}

TEST(World_LevelsOwnActorsAndDestroyingLevelDestroysItsActors) {
    ResetEvents();
    Fixture fixture;
    World& world = *fixture.world;
    CHECK(world.PersistentLevel().IsNull());
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    const ObjectHandle persistent = world.PersistentLevel();
    REQUIRE(!persistent.IsNull());
    auto dungeon = world.CreateLevel("Dungeon");
    REQUIRE(dungeon);
    SpawnParams inDungeon;
    inDungeon.level = dungeon.Value();
    auto orc = world.SpawnActor<LoggingActor>(inDungeon);
    auto goblin = world.SpawnActor<LoggingActor>(inDungeon);
    auto hero = world.SpawnActor<LoggingActor>();
    REQUIRE(orc && goblin && hero);
    CHECK(world.GetActorLevel(orc.Value()).Value() == dungeon.Value());
    CHECK(world.GetActorLevel(hero.Value()).Value() == persistent);
    auto names = world.With<Level>(dungeon.Value(), [](Level& level) noexcept {
        return level.Name() == StringView("Dungeon") && level.Actors().Size() == 2 &&
               !level.IsPersistent();
    });
    CHECK(names && names.Value());
    CHECK(world.Stats().levels == 2);
    REQUIRE(world.Tick(0.016));

    CHECK(world.DestroyLevel(persistent).Error() == ErrorCode::InvalidArgument);
    REQUIRE(world.DestroyLevel(dungeon.Value()));
    CHECK(world.DestroyLevel(dungeon.Value()).IsOk());
    // 파괴 대기 Level에는 Spawn할 수 없고, 소속 Actor는 즉시 Tick에서 빠진다.
    CHECK(world.SpawnActor<LoggingActor>(inDungeon).Error() == ErrorCode::Destroyed);
    ResetEvents();
    REQUIRE(world.Tick(0.016));
    CHECK(CountEvent("a.tick") == 1 && CountEvent("a.destroy") == 2);
    CHECK(!world.HasActor(orc.Value()) && !world.HasActor(goblin.Value()));
    CHECK(world.HasActor(hero.Value()));
    CHECK(!world.HasLevel(dungeon.Value()) && world.Stats().levels == 1);
    CHECK(world.SpawnActor<LoggingActor>(inDungeon).Error() == ErrorCode::StaleHandle);
    for (int frame = 0; frame < 20; ++frame) {
        REQUIRE(world.Tick(0.016));
    }
    // 영구 Level 루트 하나와 Level·hero만 남는다.
    CHECK(world.Objects().Stats().roots == 1);
    CHECK(world.Objects().Stats().liveObjects == 2);
    REQUIRE(world.Shutdown());
    CHECK(world.PersistentLevel().IsNull() && world.Stats().levels == 0);
}

TEST(World_EmptyLevelIsReleasedImmediatelyOutsideTick) {
    Fixture fixture;
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto empty = world.CreateLevel("Empty");
    REQUIRE(empty);
    REQUIRE(world.DestroyLevel(empty.Value()));
    CHECK(!world.HasLevel(empty.Value()));
    REQUIRE(world.Shutdown());
}

namespace {

// OnAwake·OnStart에서 Component를 추가하는 Actor다.
class BuilderActor final : public Actor {
    ROCKS_NATIVE_TYPE(BuilderActor, Actor, 0x9c3a144c0c4d4505ull, 0xb41cb2d5455ce56full)

public:
    explicit BuilderActor(Allocator& allocator) noexcept : Actor(allocator) {
        PrimaryTick().tick = true;
    }

    Result<void> OnAwake() noexcept override {
        Record("b.awake");
        auto added = GetWorld().AddComponent<LoggingComponent>(Handle());
        if (!added) {
            return Result<void>::Failure(added.Error());
        }
        awakeComponent = added.Value();
        return Result<void>::Success();
    }

    void OnStart() noexcept override {
        Record("b.start");
        auto added = GetWorld().AddComponent<LoggingComponent>(Handle());
        CHECK(added.IsOk());
        if (added) {
            startComponent = added.Value();
        }
    }

    void OnTick(const TickContext&) noexcept override {
        Record("b.tick");
        if (tryReenter) {
            CHECK(GetWorld().Tick(0.016).Error() == ErrorCode::Busy);
            CHECK(GetWorld().Shutdown().Error() == ErrorCode::Busy);
            CHECK(GetWorld().BeginPlay().Error() != ErrorCode::None);
            tryReenter = false;
        }
    }

    ObjectHandle awakeComponent;
    ObjectHandle startComponent;
    bool tryReenter = false;
};

// 잘못된 Tick 설정(OnTick을 Physics 단계로)을 가진 Actor다.
class BadTickActor final : public Actor {
    ROCKS_NATIVE_TYPE(BadTickActor, Actor, 0xc21acd5377604e47ull, 0xa8d34f284a9bd3a5ull)

public:
    explicit BadTickActor(Allocator& allocator) noexcept : Actor(allocator) {
        PrimaryTick().tick = true;
        PrimaryTick().phase = TickPhase::Physics;
    }
};

bool RegisterExtraTypes(TypeRegistry& types) noexcept {
    return RegisterNativeClass<BuilderActor>(types).IsOk() &&
           RegisterNativeClass<BadTickActor>(types).IsOk();
}

} // namespace

TEST(World_ComponentsAddedInsideAwakeAndStartRunFullLifecycle) {
    ResetEvents();
    Fixture fixture;
    REQUIRE(RegisterExtraTypes(fixture.types));
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto actor = world.SpawnActor<BuilderActor>();
    REQUIRE(actor);
    // OnAwake에서 추가한 Component는 Actor OnAwake 뒤에 등록·Awake·Enable을 받는다.
    const char* const spawned[] = {"b.awake", "c.register", "c.awake", "c.enable"};
    CHECK(EventsAre(spawned));
    ResetEvents();
    REQUIRE(world.Tick(0.016));
    // 시작 단계: 기존 Component → Actor OnStart → OnStart 안에서 추가한 Component가 같은 프레임에
    // 시작. Tick은 Actor 뒤에 두 Component(선행 의존).
    const char* const started[] = {"c.start", "b.start", "c.register", "c.awake", "c.enable",
                                   "c.start", "b.tick",  "c.tick",     "c.tick"};
    CHECK(EventsAre(started));
    Borrow<BuilderActor> borrow;
    BuilderActor* builder = fixture.Pointer(actor.Value(), borrow);
    REQUIRE(builder != nullptr);
    const ObjectHandle first = builder->awakeComponent;
    const ObjectHandle second = builder->startComponent;
    borrow.Reset();
    CHECK(world.GetComponentState(first).Value() == ComponentState::Playing);
    CHECK(world.GetComponentState(second).Value() == ComponentState::Playing);
    REQUIRE(world.Shutdown());
}

TEST(World_HooksCannotReenterTickShutdownOrBeginPlay) {
    Fixture fixture;
    REQUIRE(RegisterExtraTypes(fixture.types));
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto actor = world.SpawnActor<BuilderActor>();
    REQUIRE(actor);
    {
        Borrow<BuilderActor> borrow;
        BuilderActor* builder = fixture.Pointer(actor.Value(), borrow);
        REQUIRE(builder != nullptr);
        builder->tryReenter = true;
    }
    REQUIRE(world.Tick(0.016));
    CHECK(world.State() == WorldState::Playing);
    REQUIRE(world.Shutdown());
}

TEST(World_DisabledActorDefersStartUntilEnabled) {
    ResetEvents();
    Fixture fixture;
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto actor = world.SpawnActorDeferred<LoggingActor>();
    REQUIRE(actor);
    auto disabled = world.With<Actor>(
        actor.Value(), [](Actor& value) noexcept { return value.SetEnabled(false); });
    REQUIRE(disabled);
    REQUIRE(world.FinishSpawning(actor.Value()));
    REQUIRE(world.Tick(0.016));
    CHECK(CountEvent("a.start") == 0 && CountEvent("a.tick") == 0);
    CHECK(world.GetActorState(actor.Value()).Value() == ActorState::Initialized);
    auto enabled = world.With<Actor>(actor.Value(),
                                     [](Actor& value) noexcept { return value.SetEnabled(true); });
    REQUIRE(enabled);
    REQUIRE(world.Tick(0.016));
    CHECK(CountEvent("a.enable") == 1 && CountEvent("a.start") == 1 && CountEvent("a.tick") == 1);
    REQUIRE(world.Shutdown());
}

TEST(World_UnregisteredSceneParentDestructionOrphansChildren) {
    Fixture fixture;
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto actor = world.SpawnActorDeferred<Actor>();
    REQUIRE(actor);
    auto parent = world.AddComponent<SceneComponent>(actor.Value());
    auto child = world.AddComponent<SceneComponent>(actor.Value());
    REQUIRE(parent && child);
    auto attached = world.With<SceneComponent>(child.Value(), [&](SceneComponent& scene) noexcept {
        return scene.AttachTo(parent.Value());
    });
    REQUIRE(attached);
    auto moved = world.With<SceneComponent>(parent.Value(), [](SceneComponent& scene) noexcept {
        return scene.SetRelativeLocation(Vec3d(5.0, 0.0, 0.0));
    });
    REQUIRE(moved);
    auto before = world.With<SceneComponent>(
        child.Value(), [](SceneComponent& scene) noexcept { return scene.WorldLocation(); });
    REQUIRE(before);
    CHECK(before.Value() == Vec3d(5.0, 0.0, 0.0));
    // FinishSpawning 전(OnRegister 전) 파괴여도 계층은 엔진이 정리한다.
    REQUIRE(world.DestroyComponent(parent.Value()));
    REQUIRE(world.Tick(0.016));
    auto after = world.With<SceneComponent>(child.Value(), [](SceneComponent& scene) noexcept {
        return scene.Parent().IsNull() && scene.WorldLocation() == Vec3d(0.0, 0.0, 0.0);
    });
    CHECK(after && after.Value());
    REQUIRE(world.Shutdown());
}

TEST(World_InvalidTickSettingsFailSpawn) {
    Fixture fixture;
    REQUIRE(RegisterExtraTypes(fixture.types));
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    CHECK(world.SpawnActor<BadTickActor>().Error() == ErrorCode::InvalidArgument);
    REQUIRE(world.Tick(0.016));
    CHECK(world.Stats().actors == 0);
    REQUIRE(world.Shutdown());
}

TEST(World_BypassedObjectDestroyIsRepairedByTick) {
    ResetEvents();
    Fixture fixture;
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto actor = world.SpawnActor<LoggingActor>();
    REQUIRE(actor);
    REQUIRE(world.Tick(0.016));
    // 계약 위반: Registry로 직접 파괴한다. World는 다음 Tick에서 레코드를 정리한다(hook 없이).
    REQUIRE(world.Objects().RequestDestroy(world.Access(), actor.Value()));
    REQUIRE(world.Tick(0.016));
    CHECK(!world.HasActor(actor.Value()));
    CHECK(world.Stats().repairedTicks == 1);
    REQUIRE(world.Shutdown());
}

TEST(World_MassShutdownReleasesThousandsOfActors) {
    Fixture fixture;
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    auto level = world.CreateLevel("Bulk");
    REQUIRE(level);
    SpawnParams params;
    params.level = level.Value();
    for (uint32 index = 0; index < 3000; ++index) {
        auto actor = world.SpawnActor<Actor>(index % 2 == 0 ? SpawnParams{} : params);
        REQUIRE(actor);
        REQUIRE(world.AddComponent<SceneComponent>(actor.Value()));
    }
    REQUIRE(world.Tick(0.016));
    CHECK(world.Stats().actors == 3000 && world.Stats().components == 3000);
    REQUIRE(world.DestroyLevel(level.Value()));
    REQUIRE(world.Tick(0.016));
    CHECK(world.Stats().actors == 1500 && !world.HasLevel(level.Value()));
    REQUIRE(world.Shutdown());
    CHECK(world.Stats().actors == 0 && world.Stats().components == 0);
}
