#include <Base/Compiler.h>
#include <Base/InitializerList.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Engine/Engine.h>
#include <Platform/CrashReporting.h>
#include <Platform/Fatal.h>
#include <Platform/File.h>
#include <Platform/Memory.h>
#include <Platform/Path.h>
#include <Platform/Process.h>
#include <Platform/Thread.h>
#include <TestSupport/HostFailure.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/ProcessProbe.h>
#include <TestSupport/Test.h>
#include <World/SceneComponent.h>
#include <World/Actor.generated.h>
#include <World/SceneComponent.generated.h>
#include <WorldTestFixtures/SaveFixture.generated.h>
#include <WorldTestFixtures/WorldTestFixtures.module.generated.h>
#include <stdio.h>
#include <string.h>

// 시험 실행기(Programs/FoundationTests/Private/Main.cpp)가 정의한다. 파일 고유 namespace 밖에 둬야
// 같은 기호를 가리킨다.
namespace rocks::test {
bool RegisterProcessProbe(const char* mode,
                          int (*function)(int, const char* const*) noexcept) noexcept;
} // namespace rocks::test

using namespace rocks;
namespace engine_tests_file {

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
        // World Tick 안의 DestroyWorld는 5단계로 미뤄진다(18.5절).
        if (destroyEngine != nullptr) {
            const usize before = destroyEngine->WorldCount();
            CHECK(destroyEngine->DestroyWorld(destroyTarget).IsOk());
            CHECK(destroyEngine->WorldCount() == before);
            destroyEngine = nullptr;
        }
    }

    uint32 ticks = 0;
    double lastDelta = 0.0;
    static inline Engine* destroyEngine = nullptr;
    static inline World* destroyTarget = nullptr;
};

// 정리(Deinitialize) 중에 다른 World 파괴를 요청하는 WorldSubsystem이다(파괴 중 요청은 미뤄진다).
class TeardownDestroyerSubsystem final : public WorldSubsystem {
public:
    void Deinitialize(World&) noexcept override {
        if (engine != nullptr) {
            const usize before = engine->WorldCount();
            CHECK(engine->DestroyWorld(target).IsOk());
            CHECK(engine->WorldCount() == before);
            engine = nullptr;
        }
    }

    static inline Engine* engine = nullptr;
    static inline World* target = nullptr;
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
            // World Tick 밖(loop hook)에서는 즉시 파괴한다(18.5절).
            CHECK(engine.WorldCount() == 0);
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

    Result<void> OnEngineReady(Engine& engine) noexcept override {
        Step("client.ready");
        CHECK(engine.State() == EngineState::Running);
        if (createWorldOnReady) {
            WorldCreateDesc desc;
            desc.name = "Ready";
            CHECK(engine.CreateWorld(desc).IsOk());
        }

        return failReady ? Result<void>::Failure(ErrorCode::DependencyFailed)
                         : Result<void>::Success();
    }

    void OnEngineTick(Engine&) noexcept override {
        Step("client.tick");
    }

    void OnEngineShutdown(Engine&) noexcept override {
        Step("client.shutdown");
    }

    bool fail = false;
    bool failReady = false;
    bool createWorldOnReady = false;
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
        config.modules = &test::ProcessModules();
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

// 엔진 묶음(Docs/Design/ModuleLoader.ko.md 3.11절, ML1): Engine은 시험 실행기의 프로세스 로더로
// 엔진 단계 모듈(World·Engine 등)을 시작하고 PostEngineInit 단계를 돈 뒤, Shutdown에서 엔진 묶음을
// 내리고 자기 타입 등록기를 뗀다. 한 프로세스에서 Engine을 차례로 만들면 매번 시작·종료하고, 동시에
// 둘이면 처음 Engine이 시작하고 마지막 Engine이 내린다(참조 수, 등록기는 Engine마다).
TEST(Engine_StartsAndStopsTheEngineModuleGroup) {
    SystemAllocator allocator;
    ModuleLoader& modules = test::ProcessModules();
    CHECK(modules.EngineGroupUsers() == 0);
    for (uint32 round = 0; round < 2; ++round) {
        EngineConfig config;
        config.modules = &modules;
        Engine engine(allocator);
        REQUIRE(engine.Initialize(config));
        CHECK(modules.IsStarted("World"));
        CHECK(modules.IsStarted("Engine"));
        CHECK(modules.EngineGroupUsers() == 1);
        CHECK(modules.PhaseStarted(LoadingPhase::PostEngineInit));
        CHECK(modules.TypeRegistrarCount() == 1);
        REQUIRE(engine.Shutdown());
        CHECK(modules.State("World").Value() == ModuleState::Stopped);
        CHECK(modules.EngineGroupUsers() == 0);
        CHECK(!modules.PhaseStarted(LoadingPhase::PreDefault));
        CHECK(modules.TypeRegistrarCount() == 0);
        CHECK(modules.CurrentGroup() == ModuleGroup::Host);
    }

    {
        EngineConfig config;
        config.modules = &modules;
        Engine first(allocator);
        Engine second(allocator);
        REQUIRE(first.Initialize(config));
        REQUIRE(second.Initialize(config));
        CHECK(modules.EngineGroupUsers() == 2);
        CHECK(modules.TypeRegistrarCount() == 2);
        REQUIRE(first.Shutdown());
        CHECK(modules.IsStarted("World"));
        CHECK(modules.TypeRegistrarCount() == 1);
        REQUIRE(second.Shutdown());
        CHECK(!modules.IsStarted("World"));
        CHECK(modules.EngineGroupUsers() == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 엔진 묶음 실패 경로 시험용 로더 표다. Engine은 아무 로더나 받으므로 시험이 실패를 넣을 모듈
// 둘(엔진 단계 Default·PostEngineInit)만 가진 표로 자기 로더를 만든다.
ErrorCode groupProbeErrors[2] = {};
uint32 groupProbeStarts[2] = {};

template <usize Index>
class GroupProbe final : public ModuleImplementation {
public:
    Result<void> Startup(ModuleStartContext&) noexcept override {
        if (groupProbeErrors[Index] != ErrorCode::None) {
            return Result<void>::Failure(groupProbeErrors[Index]);
        }

        ++groupProbeStarts[Index];
        return Result<void>::Success();
    }
};

const ModuleEntry& GroupProbeDefaultEntry() noexcept {
    static constexpr ModuleEntry entry = detail::MakeModuleEntry<GroupProbe<0>>("GroupDefault");
    return entry;
}

const ModuleEntry& GroupProbeLateEntry() noexcept {
    static constexpr ModuleEntry entry = detail::MakeModuleEntry<GroupProbe<1>>("GroupLate");
    return entry;
}

constexpr ModuleTableRow GroupProbeRows[] = {
    {"GroupDefault", &GroupProbeDefaultEntry, nullptr, 0, LoadingPhase::Default, false},
    {"GroupLate", &GroupProbeLateEntry, nullptr, 0, LoadingPhase::PostEngineInit, false},
};

// 엔진 묶음의 실패 경로(3.11절): PostEngineInit 실패는 클라이언트 OnEngineShutdown을 한 번 부른 뒤
// 초기화 실패 정리를 하고, 엔진 단계 모듈 실패는 RegisterTypes 단계 실패다. 어느 쪽이든 엔진 묶음
// 사용자와 등록기가 0으로 돌아가 다음 Engine이 다시 시도한다. 동시에 사는 두 번째 Engine이 뒤
// 단계에서 실패하면 자기 사용자 몫과 등록기만 놓는다.
TEST(Engine_EngineGroupFailureRestoresTheLoader) {
    ResetSteps();
    SystemAllocator allocator;
    {
        auto created = ModuleLoader::TryCreate(allocator, ModuleTable(GroupProbeRows));
        REQUIRE(created);
        UniquePtr<ModuleLoader> modules = Move(created.Value());
        groupProbeErrors[0] = ErrorCode::None;
        groupProbeErrors[1] = ErrorCode::OutOfMemory;
        {
            RecordingClient client;
            EngineConfig config;
            config.modules = modules.Get();
            config.client = &client;
            Engine engine(allocator);
            CHECK(engine.Initialize(config).Error() == ErrorCode::OutOfMemory);
            const char* const late[] = {"client.start", "client.shutdown"};
            CHECK(StepsAre(late));
            CHECK(modules->EngineGroupUsers() == 0);
            CHECK(modules->TypeRegistrarCount() == 0);
            CHECK(!modules->IsStarted("GroupDefault"));
        }

        groupProbeErrors[0] = ErrorCode::Unsupported;
        groupProbeErrors[1] = ErrorCode::None;
        {
            EngineConfig config;
            config.modules = modules.Get();
            Engine engine(allocator);
            CHECK(engine.Initialize(config).Error() == ErrorCode::Unsupported);
            CHECK(modules->EngineGroupUsers() == 0);
            CHECK(modules->TypeRegistrarCount() == 0);
        }

        groupProbeErrors[0] = ErrorCode::None;
        {
            EngineConfig config;
            config.modules = modules.Get();
            Engine first(allocator);
            REQUIRE(first.Initialize(config));
            CHECK(modules->IsStarted("GroupLate"));
            EngineConfig invalid = config;
            invalid.gcWorkers = EngineConfig::MaxGcWorkers + 1;
            Engine second(allocator);
            CHECK(second.Initialize(invalid).Error() == ErrorCode::InvalidArgument);
            CHECK(modules->EngineGroupUsers() == 1);
            CHECK(modules->TypeRegistrarCount() == 1);
            CHECK(modules->IsStarted("GroupDefault"));
            REQUIRE(first.Shutdown());
            CHECK(modules->EngineGroupUsers() == 0);
            CHECK(!modules->IsStarted("GroupDefault"));
        }

        modules->ShutdownGroup(ModuleGroup::Process);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// EngineConfig::modules가 없으면 초기화가 InvalidArgument로 실패한다(대체 로더는 없다 — 3.11절).
TEST(Engine_RequiresTheProcessModuleLoader) {
    SystemAllocator allocator;
    {
        EngineConfig config;
        Engine engine(allocator);
        CHECK(engine.Initialize(config).Error() == ErrorCode::InvalidArgument);
        CHECK(engine.State() == EngineState::Failed);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(Engine_FixedDeltaDrivesWorldTimeDeterministically) {
    SystemAllocator allocator;
    {
        EngineConfig config;
        config.modules = &test::ProcessModules();
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
        config.modules = &test::ProcessModules();
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
        config.modules = &test::ProcessModules();
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
        config.modules = &test::ProcessModules();
        // World 타입 등록은 멱등이라 다시 불러도 성공한다(모듈 로더가 이미 등록한 타입은 건너뛴다).
        // native 타입을 두 번 등록하면 AlreadyExists로 실패하고, 첫 등록 토큰이 남은 채로
        // 정리된다(Teardown은 Unregister하지 않고 TypeRegistry::Shutdown에 맡긴다).
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

TEST(Engine_RequestExitFromHookAndWorldDestroyOutsideWorldTick) {
    ResetSteps();
    SystemAllocator allocator;
    {
        RecordingHooks hooks;
        EngineConfig config;
        config.modules = &test::ProcessModules();
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
        // 3번째 프레임 시작(BeginFrame)에 바로 파괴되어 1·2번째 프레임만 Tick한다.
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
        config.modules = &test::ProcessModules();
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

// 리플렉션 모듈 등록: World 타입은 이미 있고(모듈 로더가 World 모듈의 자기 ModuleRegistration에
// 등록했다 — Docs/Design/ModuleLoader.ko.md 3.10절) fixture 모듈은 아직 없다. 이
// ModuleRegistration에 World를 다시 등록하면 타입은 이미 있어 건너뛰고 모듈 키 기록만 남는다. 같은
// ModuleRegistration에 한 번 더 부르면 기록이 늘지 않는다(멱등). fixture 모듈 등록도 World를 다시
// 등록하지 않는다.
Result<void> RegisterFixtureModules(ModuleRegistration& modules) noexcept {
    Step("modules");
    CHECK(Registered(ObjectTypeIdOf<Actor>()) && Registered(ObjectTypeIdOf<SceneComponent>()));
    CHECK(!Registered(ObjectTypeIdOf<world_fixture::SaveActor>()));
    if (auto first = RegisterWorldTypes(modules); !first) {
        return first;
    }

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

// 타입 등록 순서는 World 리플렉션 타입(모듈 로더) -> registerModuleTypes(엔진의 ModuleRegistration)
// -> registerTypes(native, 마지막)이다. 토큰을 버린 native 파생(Actor·SceneComponent 파생)을 등록한
// 채로 World를 돌리고 종료해도 Engine Shutdown이 성공한다(타입을 Unregister하지 않고
// ModuleRegistration 파괴 -> TypeRegistry::Shutdown). World 재등록은
// 멱등이다(RegisterFixtureModules).
TEST(Engine_RegistersWorldThenModulesThenNativeTypesAndShutsDown) {
    ResetSteps();
    SystemAllocator allocator;
    {
        EngineConfig config;
        config.modules = &test::ProcessModules();
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
        config.modules = &test::ProcessModules();
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
        config.modules = &test::ProcessModules();
        config.subsystems = subsystems;
        config.registerTypes = &RegisterNativeGameTypes;
        Engine engine(allocator);
        CHECK(engine.Initialize(config).Error() == ErrorCode::OutOfMemory);
        const char* const expected[] = {"alpha.init", "failing.init", "alpha.deinit"};
        CHECK(StepsAre(expected));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

// 종료 단계 hook(CP6) 순서를 기록하는 Subsystem이다.
class ShutdownSubsystem final : public EngineSubsystem {
public:
    void BeginShutdown(Engine&) noexcept override {
        Step("sub.begin-shutdown");
    }

    Result<void> CompleteInFlightWork(Engine&) noexcept override {
        Step("sub.complete-work");
        return Result<void>::Success();
    }

    void Deinitialize(Engine&) noexcept override {
        Step("sub.deinit");
    }
};

// 종료 단계 hook 순서와 drain 반복·실패 보고를 기록한다.
class ShutdownHooks final : public EngineLoopHooks {
public:
    void BeginShutdown(Engine&) noexcept override {
        Step("hook.begin-shutdown");
    }

    Result<void> CompleteInFlightWork(Engine&) noexcept override {
        Step("hook.complete-work");
        return failWork ? Result<void>::Failure(ErrorCode::DependencyFailed)
                        : Result<void>::Success();
    }

    Result<uint64> DrainShutdownCleanup(Engine&) noexcept override {
        Step("hook.drain");
        // 앞 두 번은 정리 작업을 처리했다고 알린다(최종 GC가 만든 Retire 흉내).
        if (pendingRounds == NumericLimits<uint32>::Max()) {
            return Result<uint64>::Success(1);
        }

        if (pendingRounds == 0) {
            return Result<uint64>::Success(0);
        }

        --pendingRounds;
        return Result<uint64>::Success(1);
    }

    Result<void> VerifyShutdownClosure(Engine&) noexcept override {
        Step("hook.closure");
        return Result<void>::Success();
    }

    Result<void> StopExecutors(Engine&) noexcept override {
        Step("hook.stop");
        return Result<void>::Success();
    }

    uint32 pendingRounds = 2;
    bool failWork = false;
};

} // namespace

TEST(Engine_ShutdownStageHooksRunInPlanOrderAndRepeatDrainUntilClosed) {
    SystemAllocator allocator;
    ResetSteps();
    {
        ShutdownHooks hooks;
        const EngineSubsystemType subsystems[] = {
            MakeSubsystemType<EngineSubsystem, ShutdownSubsystem>("Shutdown")};
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.hooks = &hooks;
        config.subsystems = subsystems;
        config.maxFrames = 1;
        Engine engine(allocator);
        REQUIRE(engine.Initialize(config));
        REQUIRE(engine.Run());
        REQUIRE(engine.Shutdown());
        CHECK(engine.State() == EngineState::Stopped);
        // 계획 3.2절: 신규 요청 차단 → (World 파괴) → GPU·비동기 완료 → Subsystem 정리 → 최종 GC와
        // drain을 닫힐 때까지 반복 → closure 확인 → 실행기 종료.
        const char* const expected[] = {"sub.begin-shutdown", "hook.begin-shutdown",
                                        "sub.complete-work",  "hook.complete-work",
                                        "sub.deinit",         "hook.drain",
                                        "hook.drain",         "hook.drain",
                                        "hook.closure",       "hook.stop"};
        CHECK(StepsAre(expected));
        // 이미 끝났으므로 다시 불러도 hook을 부르지 않는다.
        ResetSteps();
        CHECK(engine.Shutdown());
        CHECK(stepCount == 0);
    }
}

TEST(Engine_ShutdownStageFailureIsReportedWithoutSkippingLaterStages) {
    SystemAllocator allocator;
    ResetSteps();
    {
        ShutdownHooks hooks;
        hooks.failWork = true;
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.hooks = &hooks;
        Engine engine(allocator);
        REQUIRE(engine.Initialize(config));
        CHECK(engine.Shutdown().Error() == ErrorCode::DependencyFailed);
        CHECK(engine.State() == EngineState::Stopped);
        const char* const expected[] = {
            "hook.begin-shutdown", "hook.complete-work", "hook.drain", "hook.drain",
            "hook.drain",          "hook.closure",       "hook.stop"};
        CHECK(StepsAre(expected));
    }

    ResetSteps();
    {
        // drain이 닫히지 않으면 MaxShutdownCollectPasses 뒤 Busy로 보고하고 뒤 단계를 마저
        // 실행한다.
        ShutdownHooks hooks;
        hooks.pendingRounds = NumericLimits<uint32>::Max();
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.hooks = &hooks;
        Engine engine(allocator);
        REQUIRE(engine.Initialize(config));
        CHECK(engine.Shutdown().Error() == ErrorCode::Busy);
        CHECK(engine.State() == EngineState::Stopped);
        CHECK(stepCount == 2 + MaxShutdownCollectPasses + 2);
        CHECK(strcmp(steps[stepCount - 1], "hook.stop") == 0);
    }
}

namespace {

// 종료 단계마다 Shutdown을 다시 부르고 그 결과를 세는 hook·client·Subsystem이다.
struct ReentrantShutdownState {
    uint32 calls = 0;
    uint32 busy = 0;
};

ReentrantShutdownState reentrantShutdown;

void CallShutdownAgain(Engine& engine) noexcept {
    ++reentrantShutdown.calls;
    if (engine.Shutdown().Error() == ErrorCode::Busy) {
        ++reentrantShutdown.busy;
    }
}

class ReentrantShutdownHooks final : public EngineLoopHooks, public EngineClient {
public:
    void BeginShutdown(Engine& engine) noexcept override {
        CallShutdownAgain(engine);
    }

    Result<void> CompleteInFlightWork(Engine& engine) noexcept override {
        CallShutdownAgain(engine);
        return Result<void>::Success();
    }

    Result<uint64> DrainShutdownCleanup(Engine& engine) noexcept override {
        CallShutdownAgain(engine);
        return Result<uint64>::Success(0);
    }

    Result<void> VerifyShutdownClosure(Engine& engine) noexcept override {
        CallShutdownAgain(engine);
        return Result<void>::Success();
    }

    Result<void> StopExecutors(Engine& engine) noexcept override {
        CallShutdownAgain(engine);
        return Result<void>::Success();
    }

    Result<void> OnEngineStart(Engine& engine) noexcept override {
        CallShutdownAgain(engine);
        return Result<void>::Success();
    }

    void OnEngineShutdown(Engine& engine) noexcept override {
        CallShutdownAgain(engine);
    }
};

class ReentrantShutdownSubsystem final : public EngineSubsystem {
public:
    Result<void> Initialize(Engine& engine) noexcept override {
        CallShutdownAgain(engine);
        return Result<void>::Success();
    }

    void BeginShutdown(Engine& engine) noexcept override {
        CallShutdownAgain(engine);
    }

    Result<void> CompleteInFlightWork(Engine& engine) noexcept override {
        CallShutdownAgain(engine);
        return Result<void>::Success();
    }

    void Deinitialize(Engine& engine) noexcept override {
        CallShutdownAgain(engine);
    }
};

} // namespace

// 종료 재진입: 초기화 중(Subsystem Initialize, client OnEngineStart)과 종료 단계 안(Subsystem
// BeginShutdown·CompleteInFlightWork·Deinitialize, client OnEngineShutdown, hook 다섯 단계)에서
// 부른 Shutdown은 모두 Busy이고, 초기화는 그대로 Running이 되며 바깥 종료는 그대로 끝난다
// (Stopped·성공). 끝난 뒤 다시 부르면 아무것도 하지 않고 성공한다.
TEST(Engine_ReentrantShutdownIsBusy) {
    SystemAllocator allocator;
    reentrantShutdown = ReentrantShutdownState{};
    ReentrantShutdownHooks hooks;
    const EngineSubsystemType subsystems[] = {
        MakeSubsystemType<EngineSubsystem, ReentrantShutdownSubsystem>("Reentrant")};
    EngineConfig config;
    config.modules = &test::ProcessModules();
    config.hooks = &hooks;
    config.client = &hooks;
    config.subsystems = subsystems;
    Engine engine(allocator);
    REQUIRE(engine.Initialize(config));
    CHECK(engine.State() == EngineState::Running);
    constexpr uint32 InitializationStages = 2;
    CHECK(reentrantShutdown.calls == InitializationStages);
    CHECK(reentrantShutdown.busy == InitializationStages);
    CHECK(engine.Shutdown());
    CHECK(engine.State() == EngineState::Stopped);
    constexpr uint32 ReentrantStages = InitializationStages + 9;
    CHECK(reentrantShutdown.calls == ReentrantStages);
    CHECK(reentrantShutdown.busy == ReentrantStages);
    CHECK(engine.Shutdown());
    CHECK(reentrantShutdown.calls == ReentrantStages);
}

// --- 로그(LG6) ---

namespace {

// 기록을 복사해 두는 sink다. 메시지·템플릿·문맥("Name=Value;" 이어 붙임)·프레임을 남긴다. 크기가
// 커서 정적 객체 하나를 시험마다 비워 쓴다.
class EngineLogCapture final : public RuntimeLogSink {
public:
    static constexpr uint32 MaxRecords = 256;
    static constexpr usize TextBytes = 256;

    struct Record {
        LogLevel level = LogLevel::Trace;
        uint64 frame = 0;
        char messageTemplate[TextBytes]{};
        char message[TextBytes]{};
        char context[TextBytes]{};
    };

    Result<void> Write(const LogEvent& event) noexcept override {
        ++total;
        if (count == MaxRecords) {
            return Result<void>::Success();
        }

        Record& record = records[count++];
        record.level = event.level;
        record.frame = event.frame;
        Copy(record.messageTemplate, event.messageTemplate);
        Copy(record.message, event.message);
        FixedStringBuilder context{Span<char>(record.context)};
        for (const LogProperty& property : event.context) {
            (void)context.TryAppendFormat("{}={};", property.name, property.text);
        }

        return Result<void>::Success();
    }

    void Reset() noexcept {
        count = 0;
        total = 0;
    }

    // level이고 템플릿이 text와 같은 기록 수다.
    uint32 Count(LogLevel level, const char* text) const noexcept {
        uint32 matches = 0;
        for (uint32 index = 0; index < count; ++index) {
            if (records[index].level == level &&
                strcmp(records[index].messageTemplate, text) == 0) {
                ++matches;
            }
        }

        return matches;
    }

    // level이고 템플릿이 text인 첫 기록이다. 없으면 nullptr이다.
    const Record* Find(LogLevel level, const char* text) const noexcept {
        for (uint32 index = 0; index < count; ++index) {
            if (records[index].level == level &&
                strcmp(records[index].messageTemplate, text) == 0) {
                return &records[index];
            }
        }

        return nullptr;
    }

    void Print() const noexcept {
        for (uint32 index = 0; index < count; ++index) {
            printf("  log[%u] %s | %s | frame %llu\n", index, records[index].message,
                   records[index].context, static_cast<unsigned long long>(records[index].frame));
        }
    }

    Record records[MaxRecords];
    uint32 count = 0;
    uint32 total = 0;

private:
    static void Copy(char (&out)[TextBytes], StringView text) noexcept {
        const usize bytes = text.ByteCount() < TextBytes - 1 ? text.ByteCount() : TextBytes - 1;
        for (usize index = 0; index < bytes; ++index) {
            out[index] = text.Data()[index];
        }

        out[bytes] = 0;
    }
};

EngineLogCapture capturedLogs;

// Manual 모드 LogSystem에 capturedLogs를 붙인다. 설치는 시험이 LogInstallScope로 한다. 앞 시험이
// 설치 밖에서 버린 기록의 알림은 버리는 설치 하나로 먼저 소비한다.
struct ManualLogs {
    ManualLogs() noexcept : logs(allocator, MakeConfig()) {
        {
            LogSystem reset(allocator, LogSystemConfig::ForTool());
            (void)reset.Start();
            LogInstallScope drained(reset);
        }

        capturedLogs.Reset();
        if (!logs.Start() || !logs.AddSink(capturedLogs)) {
            platform::Fatal("Manual log system did not start");
        }
    }

    static LogSystemConfig MakeConfig() noexcept {
        LogSystemConfig config;
        config.mode = LogMode::Manual;
        config.globalMinimum = LogLevel::Debug;
        return config;
    }

    void Pump() noexcept {
        (void)logs.Pump();
    }

    SystemAllocator allocator;
    LogSystem logs;
};

constexpr const char* InitFailedTemplate = "Engine initialization failed at {Stage}: {Error}";
constexpr const char* InitializedTemplate =
    "Engine initialized with {SubsystemCount} subsystems in {Ms:.2f} ms";

// 초기화 실패 Error가 정확히 하나이고 메시지·"Stage" 문맥이 stage인지 확인한다.
bool InitFailureLoggedOnceAt(const char* stage, const char* error) noexcept {
    if (capturedLogs.Count(LogLevel::Error, InitFailedTemplate) != 1) {
        capturedLogs.Print();
        return false;
    }

    const EngineLogCapture::Record* record = capturedLogs.Find(LogLevel::Error, InitFailedTemplate);
    char message[EngineLogCapture::TextBytes];
    char context[EngineLogCapture::TextBytes];
    snprintf(message, sizeof(message), "Engine initialization failed at %s: %s", stage, error);
    snprintf(context, sizeof(context), "Stage=%s;", stage);
    const bool same =
        strcmp(record->message, message) == 0 && strcmp(record->context, context) == 0;
    if (!same) {
        capturedLogs.Print();
    }

    return same;
}

test::NFailAllocator* attemptCounter = nullptr;
uint64 attemptsAtNativeRegistration = 0;

// 타입 등록 단계의 마지막 호출이다. 그때까지의 할당 시도 수를 기록한다(할당하지 않는다).
Result<void> RecordAttempts(TypeRegistry&) noexcept {
    attemptsAtNativeRegistration = attemptCounter->Attempts();
    return Result<void>::Success();
}

Result<void> FailNativeRegistration(TypeRegistry&) noexcept {
    return Result<void>::Failure(ErrorCode::AlreadyExists);
}

WorldCreateDesc NamedWorld(const char* name) noexcept {
    WorldCreateDesc desc;
    desc.name = name;
    return desc;
}

class FailingWorldSubsystem final : public WorldSubsystem {
public:
    Result<void> Initialize(World&) noexcept override {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }
};

} // namespace

TEST(EngineLog_EachInitializationStageFailureIsLoggedOnceWithItsStage) {
    ManualLogs logs;
    LogInstallScope installed(logs.logs);
    SystemAllocator system;
    test::NFailAllocator allocator(system);
    attemptCounter = &allocator;

    // 할당 시도 수를 잰 뒤(실패 없음) 그 시도 각각을 실패시켜 단계 판정을 모두 확인한다.
    {
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.registerTypes = &RecordAttempts;
        allocator.DisableFailure();
        Engine engine(allocator);
        REQUIRE(engine.Initialize(config));
        REQUIRE(engine.Shutdown());
    }

    const uint64 objectRegistryAttempt = attemptsAtNativeRegistration + 1;
    REQUIRE(objectRegistryAttempt > 2);
    bool sawTypeRegistry = false;
    bool sawModuleRegistration = false;
    bool sawRegisterTypes = false;
    for (uint64 attempt = 1; attempt <= objectRegistryAttempt; ++attempt) {
        logs.Pump();
        capturedLogs.Reset();
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.registerTypes = &RecordAttempts;
        allocator.FailOnAttempt(attempt);
        {
            Engine engine(allocator);
            REQUIRE(!engine.Initialize(config));
            CHECK(engine.State() == EngineState::Failed);
        }

        allocator.DisableFailure();
        logs.Pump();
        REQUIRE(capturedLogs.Count(LogLevel::Error, InitFailedTemplate) == 1);
        CHECK(capturedLogs.Count(LogLevel::Info, InitializedTemplate) == 0);
        const char* context = capturedLogs.Find(LogLevel::Error, InitFailedTemplate)->context;
        if (attempt == 1) {
            CHECK(strcmp(context, "Stage=TypeRegistry;") == 0);
        }

        if (attempt == objectRegistryAttempt) {
            CHECK(strcmp(context, "Stage=ObjectRegistry;") == 0);
        }

        sawTypeRegistry |= strcmp(context, "Stage=TypeRegistry;") == 0;
        sawModuleRegistration |= strcmp(context, "Stage=ModuleRegistration;") == 0;
        sawRegisterTypes |= strcmp(context, "Stage=RegisterTypes;") == 0;
    }

    CHECK(sawTypeRegistry && sawModuleRegistration && sawRegisterTypes);
    attemptCounter = nullptr;

    // 할당이 아닌 단계: 타입 등록 함수, Subsystem, client 시작.
    {
        capturedLogs.Reset();
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.registerTypes = &FailNativeRegistration;
        Engine engine(system);
        CHECK(engine.Initialize(config).Error() == ErrorCode::AlreadyExists);
        logs.Pump();
        CHECK(InitFailureLoggedOnceAt("RegisterTypes", "AlreadyExists"));
    }

    {
        ResetSteps();
        capturedLogs.Reset();
        const EngineSubsystemType subsystems[] = {
            MakeSubsystemType<EngineSubsystem, FailingSubsystem>("Failing")};
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.subsystems = subsystems;
        Engine engine(system);
        CHECK(engine.Initialize(config).Error() == ErrorCode::OutOfMemory);
        logs.Pump();
        CHECK(InitFailureLoggedOnceAt("Subsystems", "OutOfMemory"));
    }

    {
        ResetSteps();
        capturedLogs.Reset();
        RecordingClient client;
        client.fail = true;
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.client = &client;
        Engine engine(system);
        CHECK(engine.Initialize(config).Error() == ErrorCode::DependencyFailed);
        logs.Pump();
        CHECK(InitFailureLoggedOnceAt("ClientStart", "DependencyFailed"));
    }

    // 성공하면 Info 한 번이고 실패 Error는 없다. 상태 오류(두 번째 Initialize)는 기록하지 않는다.
    {
        capturedLogs.Reset();
        EngineConfig config;
        config.modules = &test::ProcessModules();
        Engine engine(system);
        REQUIRE(engine.Initialize(config));
        CHECK(engine.Initialize(config).Error() == ErrorCode::InvalidArgument);
        REQUIRE(engine.Shutdown());
        logs.Pump();
        CHECK(capturedLogs.Count(LogLevel::Info, InitializedTemplate) == 1);
        CHECK(capturedLogs.Count(LogLevel::Error, InitFailedTemplate) == 0);
        CHECK(capturedLogs.Count(LogLevel::Info, "Engine shutting down with {Worlds} worlds "
                                                 "after {Frames} frames") == 1);
        CHECK(capturedLogs.Count(LogLevel::Info, "Engine shut down") == 1);
    }
}

TEST(EngineLog_DeferredWorldDestroyWarnsOnceAndReportsResolution) {
    ManualLogs logs;
    LogInstallScope installed(logs.logs);
    SystemAllocator allocator;
    {
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.fixedFrameDeltaSeconds = 0.016;
        config.registerTypes = &RegisterGameTypes;
        Engine engine(allocator);
        REQUIRE(engine.Initialize(config));
        auto world = engine.CreateWorld(NamedWorld("Held"));
        REQUIRE(world);
        auto actor = world.Value()->SpawnActor<TickingActor>();
        REQUIRE(actor);
        {
            // 밖에서 Borrow를 쥐고 있으면 World Shutdown이 끝나지 않아 파괴가 미뤄진다.
            auto borrow = world.Value()->Resolve<TickingActor>(actor.Value());
            REQUIRE(borrow);
            CHECK(!engine.DestroyWorld(world.Value()));
            constexpr uint64 Frames = 1000;
            for (uint64 frame = 0; frame < Frames; ++frame) {
                REQUIRE(engine.RunFrame());
                logs.Pump();
            }

            CHECK(engine.WorldCount() == 1);
            CHECK(engine.Stats().pendingWorldDestroys == 1);
        }

        REQUIRE(engine.RunFrame());
        CHECK(engine.WorldCount() == 0);
        REQUIRE(engine.Shutdown());
    }

    logs.Pump();
    // Debug 기록은 컴파일 하한(LogCompiledMinimum — Release는 Info)보다 낮으면 빠진다.
    const usize expectedDebug = LogLevel::Debug >= LogCompiledMinimum ? 1 : 0;
    CHECK(capturedLogs.Count(LogLevel::Debug,
                             "World {World} destroy deferred: {Error}; retrying each frame") ==
          expectedDebug);
    const char* const warning = "World {World} destroy still deferred after {Frames} frames: "
                                "{Error}; something outside still holds its objects";
    CHECK(capturedLogs.Count(LogLevel::Warning, warning) == 1);
    const EngineLogCapture::Record* warned = capturedLogs.Find(LogLevel::Warning, warning);
    REQUIRE(warned != nullptr);
    // DestroyWorld는 프레임 0(첫 프레임 전)에 미뤄졌고 WorldDestroyWarnFrames를 넘은 첫 프레임에
    // 경고한다.
    CHECK(warned->frame == WorldDestroyWarnFrames + 1);
    CHECK(strcmp(warned->message, "World Held destroy still deferred after 301 frames: Busy; "
                                  "something outside still holds its objects") == 0);
    const char* const resolved = "World {World} destroyed after {Frames} deferred frames";
    CHECK(capturedLogs.Count(LogLevel::Info, resolved) == 1);
    const EngineLogCapture::Record* done = capturedLogs.Find(LogLevel::Info, resolved);
    REQUIRE(done != nullptr);
    CHECK(strcmp(done->message, "World Held destroyed after 1001 deferred frames") == 0);
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(EngineLog_RepeatedWorldTickFailureIsLoggedOnceThenSummarized) {
    ManualLogs logs;
    LogInstallScope installed(logs.logs);
    SystemAllocator allocator;
    {
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.fixedFrameDeltaSeconds = 0.016;
        config.registerTypes = &RegisterGameTypes;
        Engine engine(allocator);
        REQUIRE(engine.Initialize(config));
        auto world = engine.CreateWorld(NamedWorld("Broken"));
        REQUIRE(world);
        auto actor = world.Value()->SpawnActor<TickingActor>();
        REQUIRE(actor);
        {
            // World를 직접 Shutdown하다 Borrow 때문에 멈추면 이후 Tick은 매 프레임
            // ShuttingDown이다.
            auto borrow = world.Value()->Resolve<TickingActor>(actor.Value());
            REQUIRE(borrow);
            REQUIRE(!world.Value()->Shutdown());
            for (uint32 frame = 0; frame < 10; ++frame) {
                CHECK(engine.RunFrame().Error() == ErrorCode::ShuttingDown);
                logs.Pump();
            }
        }

        REQUIRE(engine.DestroyWorld(world.Value()));
        REQUIRE(engine.Shutdown());
    }

    logs.Pump();
    const char* const failed = "World {World} tick failed: {Error}";
    CHECK(capturedLogs.Count(LogLevel::Error, failed) == 1);
    const EngineLogCapture::Record* first = capturedLogs.Find(LogLevel::Error, failed);
    REQUIRE(first != nullptr);
    CHECK(strcmp(first->message, "World Broken tick failed: ShuttingDown") == 0);
    CHECK(strcmp(first->context, "World=Broken;") == 0);
    CHECK(first->frame == 1);
    const char* const summary =
        "World {World} tick failed {Count} more times after the first failure (last: {Error})";
    CHECK(capturedLogs.Count(LogLevel::Error, summary) == 1);
    const EngineLogCapture::Record* summarized = capturedLogs.Find(LogLevel::Error, summary);
    REQUIRE(summarized != nullptr);
    CHECK(strcmp(summarized->message, "World Broken tick failed 9 more times after the first "
                                      "failure (last: ShuttingDown)") == 0);
}

TEST(EngineLog_TickPrerequisiteCycleIsRejectedWithActorNames) {
    ManualLogs logs;
    LogInstallScope installed(logs.logs);
    SystemAllocator allocator;
    {
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.fixedFrameDeltaSeconds = 0.016;
        config.registerTypes = &RegisterGameTypes;
        Engine engine(allocator);
        REQUIRE(engine.Initialize(config));
        auto world = engine.CreateWorld(NamedWorld("Main"));
        REQUIRE(world);
        World& main = *world.Value();
        auto first = main.SpawnActor<TickingActor>();
        auto second = main.SpawnActor<TickingActor>();
        REQUIRE(first && second);
        // 시작(Tick 등록) 전에도 선언할 수 있다(시작 때 적용 — EngineRuntime 15.3절 ERD5).
        REQUIRE(main.AddTickPrerequisite({first.Value()}, {second.Value()}));
        REQUIRE(engine.RunFrame());
        CHECK(main.AddTickPrerequisite({first.Value()}, {second.Value()}));
        CHECK(main.AddTickPrerequisite({first.Value()}, {first.Value()}).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(main.AddTickPrerequisite({second.Value()}, {first.Value()}).Error() ==
              ErrorCode::CycleDetected);
        // 거부는 상태를 바꾸지 않는다(다음 프레임도 정상 실행).
        REQUIRE(engine.RunFrame());
        logs.Pump();
        const char* const rejected =
            "Rejected tick prerequisite {Before} -> {After}: would create a cycle";
        CHECK(capturedLogs.Count(LogLevel::Warning, rejected) == 1);
        const EngineLogCapture::Record* record = capturedLogs.Find(LogLevel::Warning, rejected);
        REQUIRE(record != nullptr);
        char expected[EngineLogCapture::TextBytes];
        snprintf(expected, sizeof(expected),
                 "Rejected tick prerequisite TickingActor#%u -> TickingActor#%u: would create a "
                 "cycle",
                 first.Value().slot, second.Value().slot);
        CHECK(strcmp(record->message, expected) == 0);
        CHECK(strcmp(record->context, "World=Main;") == 0);
        REQUIRE(engine.Shutdown());
    }
}

TEST(EngineLog_RunReportsFramesAndReasonAndCreateWorldFailureStage) {
    ManualLogs logs;
    LogInstallScope installed(logs.logs);
    SystemAllocator allocator;
    {
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.fixedFrameDeltaSeconds = 0.016;
        config.maxFrames = 3;
        Engine engine(allocator);
        REQUIRE(engine.Initialize(config));
        const WorldSubsystemType failing[] = {
            MakeSubsystemType<WorldSubsystem, FailingWorldSubsystem>("Failing")};
        WorldCreateDesc desc;
        desc.name = "Doomed";
        desc.subsystems = failing;
        CHECK(engine.CreateWorld(desc).Error() == ErrorCode::DependencyFailed);
        REQUIRE(engine.Run());
        REQUIRE(engine.Shutdown());
    }

    logs.Pump();
    const char* const failed = "World {World} failed during {Stage}: {Error}";
    CHECK(capturedLogs.Count(LogLevel::Error, failed) == 1);
    const EngineLogCapture::Record* worldFailure = capturedLogs.Find(LogLevel::Error, failed);
    REQUIRE(worldFailure != nullptr);
    CHECK(strcmp(worldFailure->message,
                 "World Doomed failed during Initialize: DependencyFailed") == 0);
    const char* const finished = "Engine run finished after {Frames} frames: {Reason}";
    CHECK(capturedLogs.Count(LogLevel::Info, finished) == 1);
    const EngineLogCapture::Record* record = capturedLogs.Find(LogLevel::Info, finished);
    REQUIRE(record != nullptr);
    CHECK(strcmp(record->message, "Engine run finished after 3 frames: MaxFrames") == 0);
    CHECK(record->frame == 3);
}

namespace {

// 자식 프로세스 시험의 버퍼 크기다. 실행 파일 경로·자식 stderr·시험 폴더 경로(임시 폴더
// 아래)·크래시 폴더 이름·CrashInfo.json 경로·CrashInfo.json 내용 순이다.
constexpr usize ProbeExecutablePathBytes = 4096;
constexpr usize ProbeErrorBytes = 4096;
constexpr usize ProbeDirectoryBytes = 4096;
constexpr usize CrashFolderNameBytes = 256;
// 시험 폴더 + '/' + 크래시 폴더 이름 + "/CrashInfo.json"이다(sizeof가 '/'와 NUL 몫을 더한다).
constexpr usize CrashInfoPathBytes =
    ProbeDirectoryBytes + CrashFolderNameBytes + sizeof("/CrashInfo.json");
constexpr usize CrashInfoBytes = 64 * 1024;

// 엔진 단계 크래시 probe의 종료 코드다. 크래시하지 않고 돌아온 경우(준비 실패·크래시 없음)는 모두
// EngineStageProbeLastExitCode 이하다. 크래시는 신호·예외 코드로 끝나 이 범위 밖이다.
constexpr int EngineStageProbeUsageExitCode = 2;
constexpr int EngineStageProbeInstallExitCode = 3;
constexpr int EngineStageProbeEngineExitCode = 4;
constexpr int EngineStageProbeNoCrashExitCode = 5;
constexpr int EngineStageProbeLastExitCode = EngineStageProbeNoCrashExitCode;

// 엔진 단계 크래시 probe 자식의 대기 상한이다. 멈춘 자식을 끊는 장치일 뿐이고 결과 판정에 쓰지
// 않는다.
constexpr uint32 EngineStageProbeTimeoutMilliseconds = 120000;

// 크래시 기록의 단계 이름(설계 LC6)을 엔진 단계마다 남긴다.
struct StageRecorder final : public EngineLoopHooks, public EngineClient {
    const char* atStart = nullptr;
    const char* inFrame = nullptr;
    const char* inShutdown = nullptr;

    Result<void> OnEngineStart(Engine&) noexcept override {
        atStart = platform::CurrentRunStage();
        return Result<void>::Success();
    }

    void PumpEvents(Engine& engine) noexcept override {
        inFrame = platform::CurrentRunStage();
        engine.RequestExit();
    }

    void BeginShutdown(Engine&) noexcept override {
        inShutdown = platform::CurrentRunStage();
    }
};

// 단계 이름의 내용이 같은가다. 이름 상수(inline constexpr char[])는 Modular에서 실행 이미지마다
// 사본이라 주소가 다르므로 내용을 비교한다(Docs/Design/BuildTargets 3.8절 BS8e).
bool SameStage(const char* actual, const char* expected) noexcept {
    return actual != nullptr && strcmp(actual, expected) == 0;
}

void RequestExitFromOtherThread(void* engine) noexcept {
    static_cast<Engine*>(engine)->RequestExit();
}

// "--process-probe engine-request-exit": 게임 스레드가 아닌 스레드의 RequestExit는
// Debug·Development에서 assert 실패다. 정상이면 돌아오지 않는다.
int EngineRequestExitProbe(int, const char* const*) noexcept {
    rocks::test::PrepareFatalTestProcess();
    SystemAllocator allocator;
    Engine engine(allocator);
    EngineConfig config;
    config.modules = &test::ProcessModules();
    if (!engine.Initialize(config)) {
        return 2;
    }

    platform::Thread thread;
    if (thread.Start(&RequestExitFromOtherThread, &engine) != platform::ThreadStatus::Success) {
        return 3;
    }

    (void)thread.Join();
    return 0;
}

const bool EngineProbeRegistered =
    rocks::test::RegisterProcessProbe("engine-request-exit", &EngineRequestExitProbe);

} // namespace

// 엔진은 초기화·초기화 끝·프레임 반복·정리·정리 끝을 크래시 기록의 단계로 남긴다(초기화·종료 중
// 크래시의 CrashInfo.json "stage"). 프레임마다 다시 쓰지 않는다.
TEST(Engine_MarksRunStagesForCrashRecords) {
    const platform::RunPhase phase = platform::CurrentRunPhase();
    const char* stage = platform::CurrentRunStage();
    SystemAllocator allocator;
    {
        StageRecorder recorder;
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.hooks = &recorder;
        config.client = &recorder;
        Engine engine(allocator);
        REQUIRE(engine.Initialize(config));
        CHECK(SameStage(recorder.atStart, EngineStageInitialize));
        CHECK(SameStage(platform::CurrentRunStage(), EngineStageReady));
        CHECK(platform::CurrentRunPhase() == platform::RunPhase::Running);
        REQUIRE(engine.Run());
        CHECK(SameStage(recorder.inFrame, EngineStageRun));
        REQUIRE(engine.Shutdown());
        CHECK(SameStage(recorder.inShutdown, EngineStageShutdown));
        CHECK(SameStage(platform::CurrentRunStage(), EngineStageStopped));
        CHECK(platform::CurrentRunPhase() == platform::RunPhase::Shutdown);
    }

    platform::SetRunPhase(phase, stage);
    CHECK(allocator.Stats().liveAllocations == 0);
}

// RequestExit는 엔진을 소유한 게임 스레드 API다(설계 20절). 다른 스레드에서 부르면 Debug·
// Development는 assert 실패다(자식 프로세스에서 확인, Release는 검사하지 않는다).
TEST(Engine_RequestExitFromAnotherThreadIsFatal) {
    if (IsReleaseBuild || !rocks::test::ProcessSpawnExpected()) {
        return;
    }

    REQUIRE(EngineProbeRegistered);
    char executable[ProbeExecutablePathBytes];
    usize executableBytes = 0;
    REQUIRE(platform::GetExecutablePath(executable, sizeof(executable), executableBytes));
    const platform::ProcessArgument arguments[] = {{"--process-probe", 15},
                                                   {"engine-request-exit", 19}};
    platform::ProcessStartInfo info;
    info.executable = executable;
    info.executableBytes = executableBytes;
    info.arguments = arguments;
    info.argumentCount = 2;
    info.output = platform::ProcessStreamMode::Null;
    info.error = platform::ProcessStreamMode::Pipe;
    static char error[ProbeErrorBytes];
    platform::ProcessRunOptions options;
    options.error = error;
    options.errorCapacity = sizeof(error) - 1;
    const platform::ProcessRunResult run = platform::RunProcess(info, options);
    REQUIRE(run.status);
    REQUIRE(!run.timedOut);
    error[run.errorBytes < sizeof(error) ? run.errorBytes : sizeof(error) - 1] = '\0';
    CHECK(run.exit.kind != platform::ProcessExitKind::Exited || run.exit.code != 0);
    CHECK(strstr(error, "ownerThread_") != nullptr);
}

namespace {

// "--process-probe engine-crash-stage <init|shutdown> <dir>": 크래시 수집을 걸고(프로세스 안 덤프,
// 폴더 dir) 엔진 초기화(client 시작) 또는 정리(BeginShutdown) 안에서 접근 위반을 낸다. 부모가
// CrashInfo.json의 stage가 그 엔진 단계인지 본다. 정상이면 돌아오지 않는다.
ROCKS_NOINLINE void FaultInEngineStage() noexcept {
    volatile int* volatile target = nullptr;
    *target = 1;
}

struct CrashingStage final : public EngineLoopHooks, public EngineClient {
    bool crashOnStart = false;

    Result<void> OnEngineStart(Engine&) noexcept override {
        if (crashOnStart) {
            FaultInEngineStage();
        }

        return Result<void>::Success();
    }

    void PumpEvents(Engine& engine) noexcept override {
        engine.RequestExit();
    }

    void BeginShutdown(Engine&) noexcept override {
        FaultInEngineStage();
    }
};

int EngineCrashStageProbe(int argc, const char* const* argv) noexcept {
    rocks::test::PrepareFatalTestProcess();
    if (argc < 5) {
        return EngineStageProbeUsageExitCode;
    }

    platform::CrashReportingOptions options;
    options.programName = "EngineStageProbe";
    options.outputDirectory = argv[4];
    options.helper = platform::CrashHelperPolicy::InProcess;
    if (platform::InstallCrashReporting(options) != platform::CrashReportingStatus::Installed) {
        return EngineStageProbeInstallExitCode;
    }

    SystemAllocator allocator;
    CrashingStage stage;
    stage.crashOnStart = strcmp(argv[3], "init") == 0;
    EngineConfig config;
    config.modules = &test::ProcessModules();
    config.hooks = &stage;
    config.client = &stage;
    Engine engine(allocator);
    if (!engine.Initialize(config) || !engine.Run()) {
        return EngineStageProbeEngineExitCode;
    }

    (void)engine.Shutdown();
    return EngineStageProbeNoCrashExitCode;
}

const bool EngineStageProbeRegistered =
    rocks::test::RegisterProcessProbe("engine-crash-stage", &EngineCrashStageProbe);

struct FoundFolder {
    char name[CrashFolderNameBytes] = {};
    bool found = false;
};

bool FindFolder(const platform::DirectoryEntry& entry, void* raw) noexcept {
    auto& folder = *static_cast<FoundFolder*>(raw);
    if (entry.info.type != platform::FileType::Directory ||
        entry.nameBytes >= sizeof(folder.name)) {
        return true;
    }

    platform::MemoryCopy(folder.name, entry.name, entry.nameBytes);
    folder.found = true;
    return false;
}

// 자식을 띄워 엔진 단계 안에서 크래시를 내고, 크래시 폴더의 CrashInfo.json 내용을 content에 읽는다.
bool RunCrashStageProbe(const char* scenario, char* content, usize capacity,
                        usize& bytes) noexcept {
    bytes = 0;
    // 크래시 폴더는 작업 폴더가 아니라 사용자 임시 폴더 아래에 만든다(실행 위치에 남지 않게).
    char temporary[ProbeDirectoryBytes];
    usize temporaryBytes = 0;
    if (!platform::GetStandardDirectory(platform::StandardDirectory::Temporary, temporary,
                                        sizeof(temporary), temporaryBytes)) {
        return false;
    }

    char directory[ProbeDirectoryBytes];
    FixedStringBuilder directoryText{Span<char>(directory)};
    if (!directoryText.TryAppendFormat("{}/rocks-engine-stage-{}-{}",
                                       StringView(temporary, temporaryBytes), StringView(scenario),
                                       platform::CurrentProcessId())) {
        return false;
    }

    platform::FileSystem& files = platform::PhysicalFileSystem();
    (void)platform::DeleteDirectoryTree(files, directory, directoryText.ByteCount());
    char executable[ProbeExecutablePathBytes];
    usize executableBytes = 0;
    if (!platform::GetExecutablePath(executable, sizeof(executable), executableBytes)) {
        return false;
    }

    const platform::ProcessArgument arguments[] = {{"--process-probe", 15},
                                                   {"engine-crash-stage", 18},
                                                   {scenario, strlen(scenario)},
                                                   {directory, directoryText.ByteCount()}};
    platform::ProcessStartInfo info;
    info.executable = executable;
    info.executableBytes = executableBytes;
    info.arguments = arguments;
    info.argumentCount = 4;
    info.output = platform::ProcessStreamMode::Null;
    info.error = platform::ProcessStreamMode::Null;
    platform::ProcessRunOptions options;
    options.timeoutMilliseconds = EngineStageProbeTimeoutMilliseconds;
    const platform::ProcessRunResult run = platform::RunProcess(info, options);
    bool crashed = run.status && !run.timedOut &&
                   (run.exit.kind != platform::ProcessExitKind::Exited ||
                    run.exit.code > static_cast<uint32>(EngineStageProbeLastExitCode));
    FoundFolder folder;
    (void)files.EnumerateDirectory(directory, directoryText.ByteCount(), &FindFolder, &folder);
    char path[CrashInfoPathBytes];
    FixedStringBuilder pathText{Span<char>(path)};
    crashed = crashed && folder.found &&
              pathText.TryAppendFormat("{}/{}/CrashInfo.json", directoryText.View(),
                                       StringView(folder.name));
    platform::File metadata;
    if (crashed && metadata.Open(path, platform::FileOpenMode::ReadExisting)) {
        const platform::FileTransfer read = metadata.Read(content, capacity - 1);
        bytes = read.bytes;
        content[bytes] = '\0';
    }

    (void)metadata.Close();
    (void)platform::DeleteDirectoryTree(files, directory, directoryText.ByteCount());
    return crashed && bytes != 0;
}

} // namespace

// 엔진 초기화·종료 중 크래시의 CrashInfo.json "stage"가 그 엔진 단계다(설계 LC6·LC7). 자식
// 프로세스에서 확인한다(크래시 수집은 자식이 직접 건다).
TEST(Engine_CrashDuringInitializeAndShutdownRecordsTheStage) {
    if (!rocks::test::ProcessSpawnExpected()) {
        return;
    }

    REQUIRE(EngineStageProbeRegistered);
    static char content[CrashInfoBytes];
    usize bytes = 0;
    REQUIRE(RunCrashStageProbe("init", content, sizeof(content), bytes));
    CHECK(strstr(content, EngineStageInitialize) != nullptr);
    REQUIRE(RunCrashStageProbe("shutdown", content, sizeof(content), bytes));
    CHECK(strstr(content, EngineStageShutdown) != nullptr);
}

namespace {

// 바깥 조회 창으로 EngineSubsystem을 찾는 WorldSubsystem이다(EngineRuntime 16.3절 ERD10).
class EngineLookupWorldSubsystem final : public WorldSubsystem {
public:
    Result<void> Initialize(World& world) noexcept override {
        foundAtInitialize = GetEngineSubsystem<AlphaSubsystem>(world) != nullptr;
        return Result<void>::Success();
    }

    void PreDeinitialize(World& world) noexcept override {
        foundAtPreDeinitialize = GetEngineSubsystem<AlphaSubsystem>(world) != nullptr;
    }

    static inline bool foundAtInitialize = false;
    static inline bool foundAtPreDeinitialize = true;
};

} // namespace

TEST(Engine_WorldSubsystemFindsEngineSubsystemThroughOuterLookup) {
    ResetSteps();
    SystemAllocator allocator;
    {
        const EngineSubsystemType subsystems[] = {
            MakeSubsystemType<EngineSubsystem, AlphaSubsystem>("Alpha")};
        const WorldSubsystemType worldSubsystems[] = {
            MakeSubsystemType<WorldSubsystem, EngineLookupWorldSubsystem>("EngineLookup")};
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.subsystems = subsystems;
        config.fixedFrameDeltaSeconds = 1.0 / 60.0;
        config.registerTypes = &RegisterGameTypes;
        Engine engine(allocator);
        REQUIRE(engine.Initialize(config));
        WorldCreateDesc desc;
        desc.name = "Main";
        desc.subsystems = worldSubsystems;
        auto world = engine.CreateWorld(desc);
        REQUIRE(world);
        CHECK(world.Value()->Kind() == WorldKind::Game);
        CHECK(EngineLookupWorldSubsystem::foundAtInitialize);
        REQUIRE(engine.Shutdown());
        // 조회 창은 PreDeinitialize부터 닫힌다(EngineSubsystem은 아직 살아 있어도).
        CHECK(!EngineLookupWorldSubsystem::foundAtPreDeinitialize);
    }
}

// 18.2절: OnEngineReady는 Running이 된 뒤 한 번 불려 World를 만들 수 있다. 실패하면 정상 종료 순서
// (client 종료 → World 파괴 → Subsystem 정리)로 정리하고 Failed로 끝난다.
TEST(Engine_ClientReadyRunsWhenRunningAndFailureShutsDownNormally) {
    SystemAllocator allocator;
    const EngineSubsystemType subsystems[] = {
        MakeSubsystemType<EngineSubsystem, AlphaSubsystem>("Alpha")};
    for (bool failReady : {false, true}) {
        ResetSteps();
        RecordingClient client;
        client.createWorldOnReady = true;
        client.failReady = failReady;
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.client = &client;
        config.subsystems = subsystems;
        Engine engine(allocator);
        if (!failReady) {
            REQUIRE(engine.Initialize(config));
            const char* const started[] = {"alpha.init", "client.start", "client.ready"};
            CHECK(StepsAre(started));
            CHECK(engine.WorldCount() == 1);
            REQUIRE(engine.Shutdown());
            continue;
        }

        CHECK(engine.Initialize(config).Error() == ErrorCode::DependencyFailed);
        const char* const failed[] = {"alpha.init", "client.start", "client.ready",
                                      "client.shutdown", "alpha.deinit"};
        CHECK(StepsAre(failed));
        CHECK(engine.State() == EngineState::Failed);
        CHECK(engine.WorldCount() == 0);
        CHECK(engine.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 18.5절: DestroyWorld는 World Tick 안과 다른 World를 파괴하는 중에만 5단계로 미룬다. 미룬 World는
// 그 프레임 Tick에서 빠진다.
TEST(Engine_DestroyWorldDefersOnlyDuringWorldTickAndWorldTeardown) {
    ResetSteps();
    SystemAllocator allocator;
    {
        const WorldSubsystemType destroyer[] = {
            MakeSubsystemType<WorldSubsystem, TeardownDestroyerSubsystem>("TeardownDestroyer")};
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.fixedFrameDeltaSeconds = 1.0 / 60.0;
        config.registerTypes = &RegisterGameTypes;
        Engine engine(allocator);
        REQUIRE(engine.Initialize(config));
        WorldCreateDesc desc;
        desc.name = "Ticking";
        auto ticking = engine.CreateWorld(desc);
        desc.name = "Victim";
        auto victim = engine.CreateWorld(desc);
        REQUIRE(ticking && victim);
        REQUIRE(ticking.Value()->SpawnActor<TickingActor>());
        REQUIRE(victim.Value()->SpawnActor<TickingActor>());
        TickingActor::destroyEngine = &engine;
        TickingActor::destroyTarget = victim.Value();
        ResetSteps();
        REQUIRE(engine.RunFrame());
        // Ticking의 Tick이 Victim 파괴를 미뤘고 Victim은 그 프레임 Tick하지 않았다.
        const char* const ticked[] = {"world.tick"};
        CHECK(StepsAre(ticked));
        CHECK(engine.WorldCount() == 1 && engine.WorldAt(0) == ticking.Value());

        // 파괴 중(Deinitialize) 요청은 미뤄졌다가 다음 프레임 5단계에서 처리된다.
        desc.name = "Torn";
        desc.subsystems = destroyer;
        auto torn = engine.CreateWorld(desc);
        REQUIRE(torn);
        TeardownDestroyerSubsystem::engine = &engine;
        TeardownDestroyerSubsystem::target = ticking.Value();
        REQUIRE(engine.DestroyWorld(torn.Value()));
        CHECK(TeardownDestroyerSubsystem::engine == nullptr);
        CHECK(engine.WorldCount() == 1);
        ResetSteps();
        REQUIRE(engine.RunFrame());
        CHECK(stepCount == 0);
        CHECK(engine.WorldCount() == 0);
        REQUIRE(engine.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
} // namespace engine_tests_file
