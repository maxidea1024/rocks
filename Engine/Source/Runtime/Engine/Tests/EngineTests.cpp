#include <Core/Memory/SystemAllocator.h>
#include <Engine/Engine.h>
#include <TestSupport/Test.h>
#include <World/SceneComponent.h>
#include <World/World.module.generated.h>
#include <WorldTestFixtures/WorldTestFixtures.module.generated.h>
#include <stdio.h>
#include <string.h>

using namespace rocks;

namespace {

constexpr uint32 MaxSteps = 64;
const char* steps[MaxSteps] = {};
uint32 stepCount = 0;

void Step(const char* text) noexcept {
    if (stepCount < MaxSteps) {
        steps[stepCount++] = text;
    }
}

void ResetSteps() noexcept {
    stepCount = 0;
}

template <usize Count>
bool StepsAre(const char* const (&expected)[Count]) noexcept {
    bool same = stepCount == Count;
    for (uint32 index = 0; same && index < Count; ++index) {
        same = strcmp(steps[index], expected[index]) == 0;
    }

    if (!same) {
        for (uint32 index = 0; index < stepCount; ++index) {
            printf("  step[%u] = %s\n", index, steps[index]);
        }
    }

    return same;
}

class TickingActor final : public Actor {
    ROCKS_NATIVE_TYPE(TickingActor, Actor, 0x98bc5cdd579047d2ull, 0xaf07b4855fa639dcull)

public:
    explicit TickingActor(Allocator& allocator) noexcept : Actor(allocator) {
        PrimaryTick().tick = true;
    }

    void OnTick(const TickContext& context) noexcept override {
        Step("world.tick");
        ++ticks;
        lastDelta = context.deltaSeconds;
    }

    uint32 ticks = 0;
    double lastDelta = 0.0;
};

// SceneComponent(리플렉션 base 두 단계 위)의 native 파생이다. 게임 등록처럼 토큰을 버린다.
class SpinningScene final : public SceneComponent {
    ROCKS_NATIVE_TYPE(SpinningScene, SceneComponent, 0xdafc125aa4de4dd2ull, 0xbe14d1567dc3d49full)

public:
    explicit SpinningScene(Allocator& allocator) noexcept : SceneComponent(allocator) {
        PrimaryTick().tick = true;
    }

    void OnTick(const TickContext&) noexcept override {
        ++ticks;
    }

    uint32 ticks = 0;
};

Result<void> RegisterGameTypes(TypeRegistry& types) noexcept {
    auto registered = RegisterNativeClass<TickingActor>(types);
    return registered ? Result<void>::Success() : Result<void>::Failure(registered.Error());
}

// 게임처럼 native 파생 둘(Actor·SceneComponent 파생)을 등록하고 토큰을 버린다.
Result<void> RegisterNativeGameTypes(TypeRegistry& types) noexcept {
    if (auto actor = RegisterGameTypes(types); !actor) {
        return actor;
    }

    auto scene = RegisterNativeClass<SpinningScene>(types);
    return scene ? Result<void>::Success() : Result<void>::Failure(scene.Error());
}

class RecordingHooks final : public EngineLoopHooks {
public:
    void PumpEvents(Engine& engine) noexcept override {
        Step("pump");
        if (exitAtFrame != 0 && engine.Time().frameIndex + 1 >= exitAtFrame) {
            engine.RequestExit();
        }
    }

    void BeginFrame(Engine& engine) noexcept override {
        Step("begin");
        if (destroyAtFrame != 0 && engine.Time().frameIndex == destroyAtFrame && target) {
            CHECK(engine.DestroyWorld(target).IsOk());
            // 프레임 안에서는 지연되므로 아직 남아 있다.
            CHECK(engine.WorldCount() == 1);
        }

        if (shutdownInsideFrame) {
            CHECK(engine.Shutdown().Error() == ErrorCode::Busy);
            shutdownInsideFrame = false;
        }
    }

    void EndFrame(Engine&) noexcept override {
        Step("end");
    }

    uint64 exitAtFrame = 0;
    uint64 destroyAtFrame = 0;
    World* target = nullptr;
    bool shutdownInsideFrame = false;
};

class RecordingClient final : public EngineClient {
public:
    Result<void> OnEngineStart(Engine&) noexcept override {
        Step("client.start");
        return fail ? Result<void>::Failure(ErrorCode::DependencyFailed) : Result<void>::Success();
    }

    void OnEngineTick(Engine&) noexcept override {
        Step("client.tick");
    }

    void OnEngineShutdown(Engine&) noexcept override {
        Step("client.shutdown");
    }

    bool fail = false;
};

class AlphaSubsystem final : public EngineSubsystem {
public:
    Result<void> Initialize(Engine&) noexcept override {
        Step("alpha.init");
        return Result<void>::Success();
    }

    void OnFrameBegin(Engine&) noexcept override {
        Step("alpha.begin");
    }

    void OnFrameEnd(Engine&) noexcept override {
        Step("alpha.end");
    }

    void Deinitialize(Engine&) noexcept override {
        Step("alpha.deinit");
    }
};

class FailingSubsystem final : public EngineSubsystem {
public:
    Result<void> Initialize(Engine&) noexcept override {
        Step("failing.init");
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    void Deinitialize(Engine&) noexcept override {
        Step("failing.deinit");
    }
};

} // namespace

TEST(Engine_HeadlessRunExecutesFramesInDocumentedOrder) {
    ResetSteps();
    SystemAllocator allocator;
    {
        RecordingHooks hooks;
        RecordingClient client;
        const EngineSubsystemType subsystems[] = {
            MakeSubsystemType<EngineSubsystem, AlphaSubsystem>("Alpha")};
        EngineConfig config;
        config.hooks = &hooks;
        config.client = &client;
        config.subsystems = subsystems;
        config.fixedFrameDeltaSeconds = 1.0 / 60.0;
        config.maxFrames = 1;
        config.registerTypes = &RegisterGameTypes;
        Engine engine(allocator);
        CHECK(engine.State() == EngineState::Uninitialized);
        REQUIRE(engine.Initialize(config));
        CHECK(engine.State() == EngineState::Running);
        CHECK(engine.Initialize(config).Error() == ErrorCode::InvalidArgument);
        WorldCreateDesc desc;
        desc.name = "Main";
        auto world = engine.CreateWorld(desc);
        REQUIRE(world);
        REQUIRE(world.Value()->SpawnActor<TickingActor>());
        ResetSteps();
        REQUIRE(engine.Run());
        CHECK(engine.State() == EngineState::Stopping);
        const char* const frame[] = {"pump",       "begin",     "alpha.begin", "client.tick",
                                     "world.tick", "alpha.end", "end"};
        CHECK(StepsAre(frame));
        CHECK(engine.Time().frameIndex == 1);
        ResetSteps();
        REQUIRE(engine.Shutdown());
        const char* const shutdown[] = {"client.shutdown", "alpha.deinit"};
        CHECK(StepsAre(shutdown));
        CHECK(engine.State() == EngineState::Stopped);
        CHECK(engine.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(Engine_FixedDeltaDrivesWorldTimeDeterministically) {
    SystemAllocator allocator;
    {
        EngineConfig config;
        config.fixedFrameDeltaSeconds = 0.02;
        config.maxFrames = 10;
        config.registerTypes = &RegisterGameTypes;
        Engine engine(allocator);
        REQUIRE(engine.Initialize(config));
        auto world = engine.CreateWorld({});
        REQUIRE(world);
        auto actor = world.Value()->SpawnActor<TickingActor>();
        REQUIRE(actor);
        REQUIRE(engine.Run());
        CHECK(engine.Time().frameIndex == 10);
        CHECK(engine.Time().realTimeSeconds > 0.199 && engine.Time().realTimeSeconds < 0.201);
        auto ticks = world.Value()->With<TickingActor>(
            actor.Value(), [](TickingActor& value) noexcept { return value.ticks; });
        REQUIRE(ticks);
        CHECK(ticks.Value() == 10);
        CHECK(world.Value()->Clock().GameTimeSeconds() > 0.199);
        REQUIRE(engine.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(Engine_SubsystemFailureRollsBackInitializedStepsInReverse) {
    ResetSteps();
    SystemAllocator allocator;
    {
        RecordingClient client;
        const EngineSubsystemType subsystems[] = {
            MakeSubsystemType<EngineSubsystem, AlphaSubsystem>("Alpha"),
            MakeSubsystemType<EngineSubsystem, FailingSubsystem>("Failing")};
        EngineConfig config;
        config.client = &client;
        config.subsystems = subsystems;
        Engine engine(allocator);
        CHECK(engine.Initialize(config).Error() == ErrorCode::OutOfMemory);
        CHECK(engine.State() == EngineState::Failed);
        // 실패한 Subsystem은 Deinitialize하지 않고, 앞서 성공한 것만 정리한다. client는 시작하지
        // 않는다.
        const char* const expected[] = {"alpha.init", "failing.init", "alpha.deinit"};
        CHECK(StepsAre(expected));
        CHECK(engine.Shutdown().IsOk());
        CHECK(engine.RunFrame().Error() == ErrorCode::InvalidArgument);
        CHECK(engine.CreateWorld({}).Error() == ErrorCode::InvalidArgument);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(Engine_ClientStartFailureDeinitializesSubsystemsWithoutClientShutdown) {
    ResetSteps();
    SystemAllocator allocator;
    {
        RecordingClient client;
        client.fail = true;
        const EngineSubsystemType subsystems[] = {
            MakeSubsystemType<EngineSubsystem, AlphaSubsystem>("Alpha")};
        EngineConfig config;
        config.client = &client;
        config.subsystems = subsystems;
        Engine engine(allocator);
        CHECK(engine.Initialize(config).Error() == ErrorCode::DependencyFailed);
        const char* const expected[] = {"alpha.init", "client.start", "alpha.deinit"};
        CHECK(StepsAre(expected));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(Engine_DuplicateTypeRegistrationFailsInitialization) {
    SystemAllocator allocator;
    {
        EngineConfig config;
        // World 타입 등록은 멱등이라 다시 불러도 성공한다(같은 ModuleRegistration은 모듈을 다시
        // 등록하지 않는다). native 타입을 두 번 등록하면 AlreadyExists로 실패하고, 첫 등록 토큰이
        // 남은 채로 정리된다(Teardown은 Unregister하지 않고 TypeRegistry::Shutdown에 맡긴다).
        config.registerModuleTypes = [](ModuleRegistration& modules) noexcept {
            Step("modules");
            return RegisterWorldTypes(modules);
        };
        config.registerTypes = [](TypeRegistry& types) noexcept {
            Step("natives");
            auto first = RegisterGameTypes(types);
            if (!first) {
                return first;
            }

            Step("natives.again");
            return RegisterGameTypes(types);
        };
        ResetSteps();
        Engine engine(allocator);
        CHECK(engine.Initialize(config).Error() == ErrorCode::AlreadyExists);
        CHECK(engine.State() == EngineState::Failed);
        // World 재등록(registerModuleTypes)은 성공했고 실패는 native 두 번째 등록에서 났다.
        const char* const reached[] = {"modules", "natives", "natives.again"};
        CHECK(StepsAre(reached));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(Engine_RequestExitFromHookAndDeferredWorldDestroy) {
    ResetSteps();
    SystemAllocator allocator;
    {
        RecordingHooks hooks;
        EngineConfig config;
        config.hooks = &hooks;
        config.fixedFrameDeltaSeconds = 0.016;
        config.registerTypes = &RegisterGameTypes;
        Engine engine(allocator);
        REQUIRE(engine.Initialize(config));
        auto world = engine.CreateWorld({});
        REQUIRE(world);
        REQUIRE(world.Value()->SpawnActor<TickingActor>());
        hooks.target = world.Value();
        hooks.destroyAtFrame = 3;
        hooks.exitAtFrame = 5;
        hooks.shutdownInsideFrame = true;
        REQUIRE(engine.Run());
        CHECK(engine.Time().frameIndex == 5);
        CHECK(engine.WorldCount() == 0);
        // 파괴를 요청한 3번째 프레임에는 World Tick을 건너뛴다(1·2번째 프레임만 Tick).
        uint32 worldTicks = 0;
        for (uint32 index = 0; index < stepCount; ++index) {
            worldTicks += strcmp(steps[index], "world.tick") == 0 ? 1u : 0u;
        }

        CHECK(worldTicks == 2);
        REQUIRE(engine.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(Engine_MultipleWorldsTickEachFrameAndDestroyOutsideFrameIsImmediate) {
    SystemAllocator allocator;
    {
        EngineConfig config;
        config.fixedFrameDeltaSeconds = 0.016;
        config.registerTypes = &RegisterGameTypes;
        Engine engine(allocator);
        REQUIRE(engine.Initialize(config));
        auto first = engine.CreateWorld({});
        WorldCreateDesc preview;
        preview.name = "Preview";
        preview.beginPlay = false;
        auto second = engine.CreateWorld(preview);
        REQUIRE(first && second);
        CHECK(first.Value()->State() == WorldState::Playing);
        CHECK(second.Value()->State() == WorldState::Initialized);
        auto a = first.Value()->SpawnActor<TickingActor>();
        auto b = second.Value()->SpawnActor<TickingActor>();
        REQUIRE(a && b);
        REQUIRE(engine.RunFrame());
        REQUIRE(engine.RunFrame());
        // BeginPlay하지 않은 World의 Actor는 시작하지 않으므로 Tick하지 않는다.
        auto firstTicks = first.Value()->With<TickingActor>(
            a.Value(), [](TickingActor& value) noexcept { return value.ticks; });
        auto secondTicks = second.Value()->With<TickingActor>(
            b.Value(), [](TickingActor& value) noexcept { return value.ticks; });
        CHECK(firstTicks && firstTicks.Value() == 2);
        CHECK(secondTicks && secondTicks.Value() == 0);
        REQUIRE(engine.DestroyWorld(second.Value()));
        CHECK(engine.WorldCount() == 1);
        CHECK(engine.DestroyWorld(second.Value()).Error() == ErrorCode::NotFound);
        REQUIRE(engine.Shutdown());
        CHECK(engine.WorldCount() == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

// 등록 순서 시험의 엔진이다(등록 함수는 Engine을 받지 않으므로 전역으로 넘긴다).
Engine* orderedEngine = nullptr;

bool Registered(TypeId type) noexcept {
    return orderedEngine != nullptr && orderedEngine->Types().Acquire(type).IsOk();
}

// 리플렉션 모듈 등록: World 타입은 이미 있고 fixture 모듈은 아직 없다. 같은 ModuleRegistration에
// World를 다시 등록하면 기록이 늘지 않고(멱등), fixture 모듈 등록도 World를 다시 등록하지 않는다.
Result<void> RegisterFixtureModules(ModuleRegistration& modules) noexcept {
    Step("modules");
    CHECK(Registered(ObjectTypeIdOf<Actor>()) && Registered(ObjectTypeIdOf<SceneComponent>()));
    CHECK(!Registered(ObjectTypeIdOf<world_fixture::SaveActor>()));
    const usize records = modules.Size();
    if (auto again = RegisterWorldTypes(modules); !again) {
        return again;
    }

    CHECK(modules.Size() == records);
    return generated::WorldTestFixtures::RegisterModuleTypes(modules);
}

// native 등록(마지막): 리플렉션 모듈까지 등록되어 있다.
Result<void> RegisterNativesLast(TypeRegistry& types) noexcept {
    Step("natives");
    CHECK(Registered(ObjectTypeIdOf<world_fixture::SaveActor>()));
    CHECK(!Registered(ObjectTypeIdOf<TickingActor>()));
    return RegisterNativeGameTypes(types);
}

Result<void> FailAfterNatives(TypeRegistry& types) noexcept {
    if (auto registered = RegisterNativeGameTypes(types); !registered) {
        return registered;
    }

    return Result<void>::Failure(ErrorCode::DependencyFailed);
}

} // namespace

// 타입 등록 순서는 World 리플렉션 타입 -> registerModuleTypes(같은 ModuleRegistration) ->
// registerTypes(native, 마지막)이다. 토큰을 버린 native 파생(Actor·SceneComponent 파생)을 등록한
// 채로 World를 돌리고 종료해도 Engine Shutdown이 성공한다(타입을 Unregister하지 않고
// ModuleRegistration 파괴 -> TypeRegistry::Shutdown). World 재등록은
// 멱등이다(RegisterFixtureModules).
TEST(Engine_RegistersWorldThenModulesThenNativeTypesAndShutsDown) {
    ResetSteps();
    SystemAllocator allocator;
    {
        EngineConfig config;
        config.fixedFrameDeltaSeconds = 0.016;
        config.registerModuleTypes = &RegisterFixtureModules;
        config.registerTypes = &RegisterNativesLast;
        Engine engine(allocator);
        orderedEngine = &engine;
        REQUIRE(engine.Initialize(config));
        orderedEngine = nullptr;
        const char* const order[] = {"modules", "natives"};
        CHECK(StepsAre(order));
        {
            auto scene = engine.Types().Acquire(ObjectTypeIdOf<SpinningScene>());
            auto actor = engine.Types().Acquire(ObjectTypeIdOf<TickingActor>());
            REQUIRE(scene && actor);
            CHECK(HasFlag(scene.Value().Schema().flags, TypeFlags::WorldOnly));
            CHECK(scene.Value().Schema().baseType == ObjectTypeIdOf<SceneComponent>());
            CHECK(HasFlag(actor.Value().Schema().flags, TypeFlags::WorldOnly));
            CHECK(actor.Value().Schema().baseType == ObjectTypeIdOf<Actor>());
        }

        auto world = engine.CreateWorld({});
        REQUIRE(world);
        auto ticking = world.Value()->SpawnActor<TickingActor>();
        auto saver = world.Value()->SpawnActor<world_fixture::SaveActor>();
        REQUIRE(ticking && saver);
        auto spinning = world.Value()->AddComponent<SpinningScene>(ticking.Value());
        REQUIRE(spinning);
        REQUIRE(engine.RunFrame());
        REQUIRE(engine.RunFrame());
        // 첫 프레임 시작에 소유 Actor와 함께 시작해 그 프레임부터 Tick한다(두 프레임 = 두 번).
        auto ticks = world.Value()->With<SpinningScene>(
            spinning.Value(), [](SpinningScene& value) noexcept { return value.ticks; });
        CHECK(ticks && ticks.Value() == 2);
        REQUIRE(engine.Shutdown());
        CHECK(engine.State() == EngineState::Stopped);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 초기화 실패 경로의 Teardown도 같은 순서다: native 파생을 등록한 뒤 등록 함수가 실패하거나, 그 뒤
// Subsystem 초기화가 실패해도 Fatal 없이 정리되고 메모리를 모두 돌려준다.
TEST(Engine_InitializationFailureTeardownReleasesNativeDerivedTypes) {
    SystemAllocator allocator;
    {
        EngineConfig config;
        config.registerModuleTypes = [](ModuleRegistration& modules) noexcept {
            return generated::WorldTestFixtures::RegisterModuleTypes(modules);
        };
        config.registerTypes = &FailAfterNatives;
        Engine engine(allocator);
        CHECK(engine.Initialize(config).Error() == ErrorCode::DependencyFailed);
        CHECK(engine.State() == EngineState::Failed);
        CHECK(engine.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);

    ResetSteps();
    {
        const EngineSubsystemType subsystems[] = {
            MakeSubsystemType<EngineSubsystem, AlphaSubsystem>("Alpha"),
            MakeSubsystemType<EngineSubsystem, FailingSubsystem>("Failing")};
        EngineConfig config;
        config.subsystems = subsystems;
        config.registerTypes = &RegisterNativeGameTypes;
        Engine engine(allocator);
        CHECK(engine.Initialize(config).Error() == ErrorCode::OutOfMemory);
        const char* const expected[] = {"alpha.init", "failing.init", "alpha.deinit"};
        CHECK(StepsAre(expected));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
