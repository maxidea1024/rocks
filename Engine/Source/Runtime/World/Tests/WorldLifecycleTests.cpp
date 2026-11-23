#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <TestSupport/Test.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>
#include <stdio.h>
#include <string.h>

// 정리 이유·World EndPlay·프레임워크 hook·BeginPlay 위임 시험이다(Docs/Design/EngineRuntime 17절,
// S9a).
using namespace rocks;
namespace world_lifecycle_tests_file {

namespace {

constexpr uint32 MaxEvents = 64;
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

const char* ReasonEvent(const char* who, EndPlayReason reason) noexcept {
    // 정적 문자열만 기록한다.
    static const char* const actorNames[] = {"a.destroy:Destroyed", "a.destroy:LevelTransition",
                                             "a.destroy:RemovedFromWorld", "a.destroy:Quit",
                                             "a.destroy:TryoutEnded"};
    static const char* const componentNames[] = {"c.destroy:Destroyed", "c.destroy:LevelTransition",
                                                 "c.destroy:RemovedFromWorld", "c.destroy:Quit",
                                                 "c.destroy:TryoutEnded"};
    static const char* const frameworkNames[] = {"fw.end:Destroyed", "fw.end:LevelTransition",
                                                 "fw.end:RemovedFromWorld", "fw.end:Quit",
                                                 "fw.end:TryoutEnded"};
    const uint32 at = static_cast<uint32>(reason);
    return who[0] == 'a' ? actorNames[at] : who[0] == 'c' ? componentNames[at] : frameworkNames[at];
}

class ReasonComponent final : public Component {
    ROCKS_NATIVE_TYPE(ReasonComponent, Component, 0x6e0c2b9d41f74a38ull, 0x8f5d13a7c92e0b64ull)

public:
    Result<void> OnAwake() noexcept override {
        Record("c.awake");
        return Result<void>::Success();
    }

    void OnEnable() noexcept override {
        Record("c.enable");
    }

    void OnDisable() noexcept override {
        Record("c.disable");
    }

    void OnDestroy(EndPlayReason reason) noexcept override {
        Record(ReasonEvent("c", reason));
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

// 프레임워크 hook을 재정의하는 시험 Actor다(Gameplay 프레임워크 클래스 흉내).
class FrameworkActor final : public Actor {
    ROCKS_NATIVE_TYPE(FrameworkActor, Actor, 0x24b9e7d0c3a14f52ull, 0x9a6c3e81f0d27b45ull)

public:
    explicit FrameworkActor(Allocator& allocator) noexcept : Actor(allocator) {
    }

    Result<void> OnAwake() noexcept override {
        Record("a.awake");
        return Result<void>::Success();
    }

    void OnEnable() noexcept override {
        Record("a.enable");
    }

    void OnStart() noexcept override {
        Record("a.start");
    }

    void OnDisable() noexcept override {
        Record("a.disable");
    }

    void OnDestroy(EndPlayReason reason) noexcept override {
        Record(ReasonEvent("a", reason));
    }

    static inline bool failPre = false;
    static inline bool failPost = false;

protected:
    Result<void> OnFrameworkPreAwake() noexcept override {
        Record("fw.pre");
        return failPre ? Result<void>::Failure(ErrorCode::DependencyFailed)
                       : Result<void>::Success();
    }

    Result<void> OnFrameworkPostAwake() noexcept override {
        Record("fw.post");
        return failPost ? Result<void>::Failure(ErrorCode::DependencyFailed)
                        : Result<void>::Success();
    }

    void OnFrameworkEndPlay(EndPlayReason reason) noexcept override {
        Record(ReasonEvent("fw", reason));
    }
};

class EndPlaySubsystem final : public WorldSubsystem {
public:
    void OnWorldEndPlay(World&) noexcept override {
        Record("s.endplay");
    }

    void Deinitialize(World&) noexcept override {
        Record("s.deinit");
    }
};

struct Fixture {
    explicit Fixture(Span<const WorldSubsystemType> subsystems = {}) noexcept
        : types(allocator), modules(types, allocator) {
        registered = RegisterWorldTypes(modules).IsOk() && Register<ReasonComponent>() &&
                     Register<FrameworkActor>();
        WorldDesc desc;
        desc.name = "Test";
        desc.context = {0x574f524c44ull, 11};
        desc.timeStep = {1.0 / 60.0, 0.25, 8};
        desc.subsystems = subsystems;
        auto created = UniquePtr<World>::TryMake(allocator, allocator, types, desc);
        if (created) {
            world = Move(created.Value());
        }
    }

    ~Fixture() {
        world.Reset();
        while (tokenCount != 0) {
            CHECK(types.Unregister(tokens[--tokenCount]).IsOk());
        }

        CHECK(modules.Unregister().IsOk());
    }

    template <class T>
    bool Register() noexcept {
        auto token = RegisterNativeClass<T>(types);
        if (!token || tokenCount == 4) {
            return false;
        }

        tokens[tokenCount++] = token.Value();
        return true;
    }

    // Actor 하나와 Component 하나를 만든다.
    ObjectHandle SpawnPair(ObjectHandle level = {}) noexcept {
        SpawnParams params;
        params.level = level;
        auto actor = world->SpawnActorDeferred<FrameworkActor>(params);
        if (!actor || !world->AddComponent<ReasonComponent>(actor.Value())) {
            return {};
        }

        return world->FinishSpawning(actor.Value()) ? actor.Value() : ObjectHandle{};
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    TypeRegistrationToken tokens[4];
    uint32 tokenCount = 0;
    bool registered = false;
    UniquePtr<World> world;
};

} // namespace

// 17.8절: 프레임워크 hook은 Component OnRegister 뒤·Actor OnAwake 전(PreAwake), 모든 Component
// OnAwake 뒤·OnEnable 전(PostAwake), 정리의 OnDisable 전(EndPlay)이다.
TEST(WorldLifecycle_FrameworkHooksBracketUserHooks) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());
    ResetEvents();
    const ObjectHandle actor = fixture.SpawnPair();
    REQUIRE(!actor.IsNull());
    const char* const created[] = {"c.register", "fw.pre",   "a.awake", "c.awake",
                                   "fw.post",    "a.enable", "c.enable"};
    CHECK(EventsAre(created));
    REQUIRE(world.Tick(1.0 / 60.0));
    ResetEvents();
    REQUIRE(world.DestroyActor(actor));
    REQUIRE(world.Tick(1.0 / 60.0));
    const char* const destroyed[] = {"fw.end:Destroyed",    "c.disable",           "a.disable",
                                     "c.destroy:Destroyed", "a.destroy:Destroyed", "c.unregister"};
    CHECK(EventsAre(destroyed));
    REQUIRE(world.Shutdown());
}

// PreAwake 실패는 OnAwake 실패와 같다(Spawn 실패, OnDestroy 없음). PostAwake 실패도 Spawn 실패지만
// OnAwake가 성공했으므로 OnDestroy를 받는다.
TEST(WorldLifecycle_FrameworkAwakeFailures) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    FrameworkActor::failPre = true;
    ResetEvents();
    CHECK(world.SpawnActor<FrameworkActor>().Error() == ErrorCode::DependencyFailed);
    REQUIRE(world.Tick(1.0 / 60.0));
    const char* const pre[] = {"fw.pre"};
    CHECK(EventsAre(pre));
    FrameworkActor::failPre = false;

    FrameworkActor::failPost = true;
    ResetEvents();
    CHECK(world.SpawnActor<FrameworkActor>().Error() == ErrorCode::DependencyFailed);
    REQUIRE(world.Tick(1.0 / 60.0));
    const char* const post[] = {"fw.pre", "a.awake", "fw.post", "fw.end:Destroyed",
                                "a.destroy:Destroyed"};
    CHECK(EventsAre(post));
    FrameworkActor::failPost = false;
    REQUIRE(world.Shutdown());
}

// 17.2절: 정리 이유가 경로마다 전달되고, 먼저 정한 이유가 남는다.
TEST(WorldLifecycle_EndPlayReasonsFollowTheInitiator) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    REQUIRE(world.BeginPlay());

    // DestroyComponent → Destroyed(Component 단독).
    auto owner = world.SpawnActor<FrameworkActor>();
    REQUIRE(owner);
    auto lone = world.AddComponent<ReasonComponent>(owner.Value());
    REQUIRE(lone);
    REQUIRE(world.Tick(1.0 / 60.0));
    ResetEvents();
    REQUIRE(world.DestroyComponent(lone.Value()));
    REQUIRE(world.Tick(1.0 / 60.0));
    const char* const component[] = {"c.disable", "c.destroy:Destroyed", "c.unregister"};
    CHECK(EventsAre(component));

    // DestroyLevel → RemovedFromWorld(Actor와 그 Component).
    auto level = world.CreateLevel("Sub");
    REQUIRE(level);
    REQUIRE(!fixture.SpawnPair(level.Value()).IsNull());
    ResetEvents();
    REQUIRE(world.DestroyLevel(level.Value()));
    REQUIRE(world.Tick(1.0 / 60.0));
    const char* const removed[] = {
        "fw.end:RemovedFromWorld",    "c.disable",   "a.disable", "c.destroy:RemovedFromWorld",
        "a.destroy:RemovedFromWorld", "c.unregister"};
    CHECK(EventsAre(removed));

    // 먼저 Destroyed로 요청된 Actor는 뒤따른 EndPlay(LevelTransition)에서도 Destroyed다.
    REQUIRE(world.DestroyActor(owner.Value()));
    ResetEvents();
    REQUIRE(world.EndPlay(EndPlayReason::LevelTransition));
    const char* const first[] = {"fw.end:Destroyed", "a.disable", "a.destroy:Destroyed"};
    CHECK(EventsAre(first));
    REQUIRE(world.Shutdown());
}

// 17.6절: EndPlay는 Actor 정리(그 이유로) → OnWorldEndPlay → Initialized다. 두 번째 호출은
// 성공(무동작), EndPlay 뒤 Spawn은 되지만 시작하지 않으며, Shutdown은 Playing이면 EndPlay(Quit)를
// 먼저 부른다.
TEST(WorldLifecycle_WorldEndPlaySeparatesFromShutdown) {
    const WorldSubsystemType types[] = {
        MakeSubsystemType<WorldSubsystem, EndPlaySubsystem>("EndPlay")};
    {
        Fixture fixture(types);
        REQUIRE(fixture.registered);
        World& world = *fixture.world;
        REQUIRE(world.Initialize());
        REQUIRE(world.BeginPlay());
        REQUIRE(!fixture.SpawnPair().IsNull());
        REQUIRE(world.Tick(1.0 / 60.0));
        ResetEvents();
        REQUIRE(world.EndPlay(EndPlayReason::LevelTransition));
        const char* const ended[] = {
            "fw.end:LevelTransition",    "c.disable",    "a.disable", "c.destroy:LevelTransition",
            "a.destroy:LevelTransition", "c.unregister", "s.endplay"};
        CHECK(EventsAre(ended));
        CHECK(world.State() == WorldState::Initialized);
        CHECK(world.EndPlay(EndPlayReason::Quit).IsOk());

        ResetEvents();
        auto late = world.SpawnActor<FrameworkActor>();
        REQUIRE(late);
        REQUIRE(world.Tick(1.0 / 60.0));
        CHECK(world.GetActorState(late.Value()).Value() == ActorState::Initialized);
        ResetEvents();
        REQUIRE(world.Shutdown());
        // EndPlay 뒤의 Shutdown은 OnWorldEndPlay를 다시 부르지 않는다. 남은 Actor는 Quit으로 정리.
        const char* const shut[] = {"fw.end:Destroyed", "a.disable", "a.destroy:Destroyed",
                                    "s.deinit"};
        CHECK(EventsAre(shut));
    }
    {
        Fixture fixture(types);
        REQUIRE(fixture.registered);
        World& world = *fixture.world;
        REQUIRE(world.Initialize());
        REQUIRE(world.BeginPlay());
        REQUIRE(!fixture.SpawnPair().IsNull());
        ResetEvents();
        REQUIRE(world.Shutdown());
        const char* const quit[] = {"fw.end:Quit",    "c.disable",    "a.disable", "c.destroy:Quit",
                                    "a.destroy:Quit", "c.unregister", "s.endplay", "s.deinit"};
        CHECK(EventsAre(quit));
    }
}

namespace {

// StartPlay에서 할 일을 고르는 시험 dispatcher다.
class TestDispatcher final : public BeginPlayDispatcher {
public:
    Result<void> StartPlay(World& world) noexcept override {
        Record("d.start");
        if (mode == 1) {
            return Result<void>::Success();
        }

        auto dispatched = world.DispatchBeginPlay();
        if (!dispatched) {
            return dispatched;
        }

        if (mode == 2) {
            secondCall = world.DispatchBeginPlay().Error();
        }

        Record("d.after");
        return Result<void>::Success();
    }

    uint32 mode = 0;
    ErrorCode secondCall = ErrorCode::None;
};

} // namespace

// 19.5절 위임 계약: StartPlay가 DispatchBeginPlay를 정확히 한 번 부른다. 안 부르면 NotReady, 두
// 번째·밖의 호출은 WrongContext. BeginPlay 전 Spawn한 Actor는 DispatchBeginPlay 안에서 시작한다.
TEST(WorldLifecycle_BeginPlayDispatcherContract) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    TestDispatcher dispatcher;
    REQUIRE(world.SetBeginPlayDispatcher(&dispatcher));
    REQUIRE(!fixture.SpawnPair().IsNull());
    CHECK(world.DispatchBeginPlay().Error() == ErrorCode::WrongContext);

    dispatcher.mode = 1;
    ResetEvents();
    CHECK(world.BeginPlay().Error() == ErrorCode::NotReady);
    CHECK(world.State() == WorldState::Initialized);

    dispatcher.mode = 2;
    ResetEvents();
    REQUIRE(world.BeginPlay());
    const char* const started[] = {"d.start", "a.start", "d.after"};
    CHECK(EventsAre(started));
    CHECK(dispatcher.secondCall == ErrorCode::WrongContext);
    CHECK(world.State() == WorldState::Playing);
    CHECK(world.SetBeginPlayDispatcher(nullptr).Error() == ErrorCode::InvalidArgument);
    REQUIRE(world.Shutdown());
}

} // namespace world_lifecycle_tests_file
