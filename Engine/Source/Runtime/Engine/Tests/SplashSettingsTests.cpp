#include <Config/ConfigContext.h>
#include <Config/Tests/ConfigTestSupport.h>
#include <Engine/SplashSettings.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace splash_settings_tests_file {

// 기본값은 설계 10절과 같다: 켬, 게임 제목·막대 켬, 진행 문구 끔, 경로·글자 빔.
TEST(SplashSettings_DefaultsMatchTheDesign) {
    test::config::ConfigFixture settings(test::config::DevelopmentRules());
    config::ConfigContext& context = settings.Context();
    REQUIRE(RegisterSplashSettings(context));
    REQUIRE(context.Sync());
    REQUIRE(context.CompleteStartup());
    const SplashSettingValues values = ReadSplashSettings();
    CHECK(values.enabled);
    CHECK(values.gameShowTitle);
    CHECK(values.gameShowProgressBar);
    CHECK(!values.gameShowProgressText);
    CHECK(values.gameImage.IsEmpty());
    CHECK(values.gameTitle.IsEmpty());
    CHECK(values.gameCopyright.IsEmpty());
    CHECK(values.studioImage.IsEmpty());
    CHECK(values.studioCopyright.IsEmpty());
    CHECK(values.minimumSeconds == 2.0f);
}

// 프로젝트·세션 층 값이 읽힌다(부팅 값 — 시작 전 대입).
TEST(SplashSettings_AssignmentsAreRead) {
    test::config::ConfigFixture settings(test::config::DevelopmentRules());
    config::ConfigContext& context = settings.Context();
    REQUIRE(RegisterSplashSettings(context));
    REQUIRE(
        context.AddAssignment(config::ConfigLayer::Session, "test", "ui.splash.enabled", "false"));
    REQUIRE(context.AddAssignment(config::ConfigLayer::Session, "test",
                                  "ui.splash.game.show_progress_text", "true"));
    REQUIRE(context.AddAssignment(config::ConfigLayer::Session, "test", "ui.splash.game.title",
                                  "My Game"));
    REQUIRE(context.AddAssignment(config::ConfigLayer::Session, "test", "ui.splash.game.image",
                                  "{ProjectDir}/Art/Splash.png"));
    REQUIRE(context.AddAssignment(config::ConfigLayer::Session, "test", "ui.splash.min_seconds",
                                  "0.5"));
    REQUIRE(context.Sync());
    REQUIRE(context.CompleteStartup());
    const SplashSettingValues values = ReadSplashSettings();
    CHECK(values.minimumSeconds == 0.5f);
    CHECK(!values.enabled);
    CHECK(values.gameShowProgressText);
    CHECK(values.gameTitle == "My Game");
    CHECK(values.gameImage == "{ProjectDir}/Art/Splash.png");
}

} // namespace splash_settings_tests_file
