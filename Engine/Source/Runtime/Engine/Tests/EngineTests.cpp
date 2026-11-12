#include <Core/Memory/SystemAllocator.h>
#include <Engine/Engine.h>
#include <TestSupport/Test.h>
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
    ROCKS_NATIVE_CLASS(TickingActor, Actor, 0x98bc5cdd579047d2ull, 0xaf07b4855fa639dcull)

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

Result<void> RegisterGameTypes(TypeRegistry& types) noexcept {
    auto registered = RegisterNativeClass<TickingActor>(types);
    return registered ? Result<void>::Success() : Result<void>::Failure(registered.Error());
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
        // World 기본 타입을 게임 등록에서 다시 등록하면 AlreadyExists로 실패하고 정리된다.
        config.registerTypes = [](TypeRegistry& types) noexcept {
            return RegisterWorldTypes(types);
        };
        Engine engine(allocator);
        CHECK(engine.Initialize(config).Error() == ErrorCode::AlreadyExists);
        CHECK(engine.State() == EngineState::Failed);
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
