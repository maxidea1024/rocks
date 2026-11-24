#include <Config/Tests/ConfigTestSupport.h>
#include <Core/Memory/SharedPtr.h>
#include <Core/Memory/SystemAllocator.h>
#include <Gameplay/GameInstance.generated.h>
#include <Gameplay/GameInstanceClient.h>
#include <Gameplay/GameplayWorldSubsystem.h>
#include <Gameplay/LocalPlayer.generated.h>
#include <TestSupport/Test.h>
#include <World/Actor.generated.h>
#include <stdio.h>
#include <string.h>

// 시험 실행기(Programs/FoundationTests/Private/Main.cpp)가 정의한다.
namespace rocks::test {
ModuleLoader& ProcessModules() noexcept;
} // namespace rocks::test

// 진단 덤프·콘솔 명령·통계 시험이다(Docs/Design/EngineRuntime 22.1절, S14).
using namespace rocks;
using rocks::test::config::ConfigFixture;
using rocks::test::config::DevelopmentRules;
namespace gameplay_diagnostics_tests_file {

namespace {

// 콘솔 출력을 모으는 sink다.
struct ConsoleText final : config::ConsoleSink {
    explicit ConsoleText(Allocator& allocator) noexcept : text(allocator) {
    }

    Result<void> Write(StringView line) noexcept override {
        return text.TryAppend(line);
    }

    String text;
};

bool Contains(StringView text, const char* needle) noexcept {
    const usize length = strlen(needle);
    for (usize at = 0; at + length <= text.ByteCount(); ++at) {
        if (memcmp(text.Data() + at, needle, length) == 0) {
            return true;
        }
    }

    printf("  missing \"%s\" in:\n%.*s\n", needle, static_cast<int>(text.ByteCount()), text.Data());
    return false;
}

class GameSubsystem final : public GameInstanceSubsystem {};
class PlayerSubsystem final : public LocalPlayerSubsystem {};

const GameInstanceSubsystemType gameSubsystems[] = {
    MakeSubsystemType<GameInstanceSubsystem, GameSubsystem>("DiagGame")};
const LocalPlayerSubsystemType playerSubsystems[] = {
    MakeSubsystemType<LocalPlayerSubsystem, PlayerSubsystem>("DiagPlayer")};

} // namespace

// 22.1절: engine.dumpsubsystems(Engine → GameInstance → LocalPlayer → World 계층), world.dumpticks
// (단계·hook·선행·엔진 단계 자리), game.dumpframework(GameInstance → LocalPlayer → PlayerController
// → Pawn·PlayerState·CameraDirector, GameDirector·GameState)와 GameplayStats·WorldStats.
TEST(GameplayDiagnostics_ConsoleDumpsDescribeEveryLayer) {
    ConfigFixture fixture(DevelopmentRules());
    config::ConfigContext& settings = fixture.Context();
    REQUIRE(settings.Sync());
    SystemAllocator allocator;
    {
        GameInstanceConfig game;
        game.subsystems = gameSubsystems;
        game.localPlayerSubsystems = playerSubsystems;
        game.timeStep = {1.0 / 60.0, 0.25, 8};
        game.defaultMap.name = "Arena";
        GameInstanceClient client(allocator, game);
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.client = &client;
        config.settings = &settings;
        config.fixedFrameDeltaSeconds = 1.0 / 60.0;
        Engine engine(allocator);
        REQUIRE(engine.Initialize(config));
        REQUIRE(engine.RunFrame());

        auto made = SharedPtr<ConsoleText>::TryMake(allocator, allocator);
        REQUIRE(made);
        SharedPtr<ConsoleText> console = Move(made.Value());
        auto run = [&](StringView line) noexcept {
            console->text.Clear();
            auto queued = settings.ExecuteConsoleLine(line, console);
            REQUIRE(queued);
            CHECK(queued.Value() == config::ConsoleLineResult::CommandQueued);
            REQUIRE(settings.Sync());
        };

        run("engine.dumpsubsystems");
        const StringView subsystems = console->text.View();
        CHECK(Contains(subsystems, "Engine subsystems (0 initialized"));
        CHECK(Contains(subsystems, "GameInstance subsystems (1 initialized"));
        CHECK(Contains(subsystems, "1. DiagGame"));
        CHECK(Contains(subsystems, "LocalPlayer 0 subsystems (1 initialized"));
        CHECK(Contains(subsystems, "World Arena subsystems (1 initialized"));
        CHECK(Contains(subsystems, "1. Gameplay"));

        run("world.dumpticks Arena");
        const StringView ticks = console->text.View();
        CHECK(Contains(ticks, "World Arena tick graph"));
        CHECK(Contains(ticks, "PrePhysics"));
        // 기본 Pawn은 Tick하지 않아 선행 줄이 없다(선행 줄은 WorldDiagnostics 시험이 본다).
        CHECK(Contains(ticks, "rocks::PlayerController#"));
        CHECK(Contains(ticks, "OnTick priority=0 interval=0 whenPaused"));
        CHECK(Contains(ticks, "whenPaused"));
        CHECK(Contains(ticks, "[engine stage CameraUpdate at start registered]"));
        CHECK(Contains(ticks, "[engine stage Timers at start empty]"));
        String missing(allocator);
        CHECK(engine.DumpTickGraphs(missing, "Nowhere").Error() == ErrorCode::NotFound);

        run("game.dumpframework");
        const StringView framework = console->text.View();
        CHECK(Contains(
            framework,
            "GameInstance rocks::GameInstance netMode=Standalone world=Arena localPlayers=1"));
        CHECK(Contains(framework, "LocalPlayer 0 controllerId=0"));
        CHECK(Contains(framework, "state=Playing pawn=rocks::Pawn#"));
        CHECK(Contains(framework, "CameraDirector viewTarget=rocks::Pawn#"));
        CHECK(Contains(framework, "GameDirector rocks::GameDirector#"));
        CHECK(Contains(framework, "controllers=1 playerControllers=1 pawns=1 playerStates=1"));
        CHECK(Contains(framework, "GameState playerStates=1 hasBegunPlay=true matchStarted=true"));

        // 통계: GameplayStats와 WorldStats의 LifeSpan 대기 수.
        World* world = engine.WorldAt(0);
        REQUIRE(world != nullptr);
        auto* gameplay = world->GetSubsystem<GameplayWorldSubsystem>();
        REQUIRE(gameplay != nullptr);
        const GameplayStats stats = gameplay->Stats(*world);
        CHECK(stats.controllers == 1 && stats.playerControllers == 1 && stats.pawns == 1 &&
              stats.playerStates == 1);
        REQUIRE(world->With<Actor>(gameplay->Director(),
                                   [](Actor& actor) noexcept { return actor.SetLifeSpan(10.0); }));
        CHECK(world->Stats().pendingLifeSpans == 1);
        REQUIRE(engine.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace gameplay_diagnostics_tests_file
