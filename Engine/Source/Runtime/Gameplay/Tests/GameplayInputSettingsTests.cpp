#include <Config/Tests/ConfigTestSupport.h>
#include <Core/Memory/SystemAllocator.h>
#include <Gameplay/GameInstance.generated.h>
#include <Gameplay/GameInstanceClient.h>
#include <Gameplay/LocalPlayer.generated.h>
#include <Gameplay/PlayerController.generated.h>
#include <Gameplay/PlayerController.h>
#include <Input/InputSettings.h>
#include <TestSupport/Test.h>
#include <World/Actor.generated.h>

// 시험 실행기(Programs/FoundationTests/Private/Main.cpp)가 정의한다.
namespace rocks::test {
ModuleLoader& ProcessModules() noexcept;
} // namespace rocks::test

// input.* 설정 연결 시험이다(Docs/Design/TouchInput.ko.md 3.4·5.1절).
using namespace rocks;
using rocks::test::config::ConfigFixture;
using rocks::test::config::DevelopmentRules;

namespace gameplay_input_settings_tests_file {

namespace {

// 첫 LocalPlayer의 PlayerController에 function을 부른다.
template <class Function>
bool WithFirstController(GameInstanceClient& client, World& world, Function&& function) noexcept {
    ObjectHandle controller;
    auto found = client.WithInstance([&](GameInstance& game) noexcept {
        (void)game.WithLocalPlayer(game.LocalPlayerAt(0), [&](LocalPlayer& player) noexcept {
            controller = player.PlayerControllerHandle();
        });
    });
    if (!found || controller.IsNull()) {
        return false;
    }

    return static_cast<bool>(world.With<PlayerController>(controller, function));
}

} // namespace

// input.enable_gesture_recognizer가 GameInstance tick에서 PlayerController 기본값으로 들어가고,
// 명시로 정한 값이 우선하며, 명시를 지우면 다시 설정을 따른다. input.use_mouse_for_touch 기본값은
// 꺼짐이다.
TEST(GameplayInputSettings_GestureRecognizerFollowsTheSetting) {
    ConfigFixture fixture(DevelopmentRules());
    config::ConfigContext& settings = fixture.Context();
    REQUIRE(input::RegisterInputSettings(settings));
    REQUIRE(input::RegisterInputSettings(settings));
    REQUIRE(settings.AddAssignment(config::ConfigLayer::Session, "test",
                                   "input.enable_gesture_recognizer", "true"));
    REQUIRE(settings.Sync());
    SystemAllocator allocator;
    {
        GameInstanceConfig game;
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
        CHECK(!input::UseMouseForTouchSetting() && input::GestureRecognizerSetting());
        World* world = engine.WorldAt(0);
        REQUIRE(world != nullptr);
        bool enabled = false;
        REQUIRE(WithFirstController(client, *world, [&](PlayerController& pc) noexcept {
            enabled = pc.IsGestureRecognizerEnabled();
        }));
        CHECK(enabled);

        // 명시로 끄면 설정이 켜져 있어도 꺼진 채다.
        REQUIRE(WithFirstController(client, *world, [](PlayerController& pc) noexcept {
            pc.SetGestureRecognizerEnabled(false);
        }));
        REQUIRE(engine.RunFrame());
        REQUIRE(WithFirstController(client, *world, [&](PlayerController& pc) noexcept {
            enabled = pc.IsGestureRecognizerEnabled();
        }));
        CHECK(!enabled);

        // 명시를 지우면 다음 tick에 설정을 다시 따른다.
        REQUIRE(WithFirstController(client, *world, [](PlayerController& pc) noexcept {
            pc.UseGestureRecognizerSetting();
        }));
        REQUIRE(engine.RunFrame());
        REQUIRE(WithFirstController(client, *world, [&](PlayerController& pc) noexcept {
            enabled = pc.IsGestureRecognizerEnabled();
        }));
        CHECK(enabled);
        REQUIRE(engine.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace gameplay_input_settings_tests_file
