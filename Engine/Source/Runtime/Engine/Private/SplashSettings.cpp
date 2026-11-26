#include <Config/CVar.h>
#include <Config/ConfigContext.h>
#include <Engine/SplashSettings.h>

// `ui.splash.*` 부팅 CVar 정의다(Docs/Design/SplashScreen 10절). 시작에 한 번 읽으므로 모두 부팅
// 값이다(실행 중 바꿔도 반영하지 않는다).
namespace rocks {

namespace splash_settings_detail {

constinit BootCVar<bool> SplashEnabledVar =
    cvar::Boot("ui.splash.enabled", true)
        .Help("Shows the startup splash screen on desktop platforms. Fixed after startup.")
        .UserSetting();

constinit BootCVar<String> SplashGameImageVar =
    cvar::Boot("ui.splash.game.image", "")
        .Help("Game splash image path ({ProjectDir} and {EngineDir} are expanded). Empty searches "
              "Content/Splash of the project and then of the engine.");

constinit BootCVar<bool> SplashGameShowTitleVar =
    cvar::Boot("ui.splash.game.show_title", true).Help("Shows the title text on the game splash.");

constinit BootCVar<String> SplashGameTitleVar =
    cvar::Boot("ui.splash.game.title", "")
        .Help("Title text of the game splash. Empty uses the project name.");

constinit BootCVar<bool> SplashGameShowProgressTextVar =
    cvar::Boot("ui.splash.game.show_progress_text", false)
        .Help("Shows the startup progress text on the game splash.");

constinit BootCVar<bool> SplashGameShowProgressBarVar =
    cvar::Boot("ui.splash.game.show_progress_bar", true)
        .Help("Shows the startup progress bar on the game splash.");

constinit BootCVar<String> SplashGameCopyrightVar =
    cvar::Boot("ui.splash.game.copyright", "")
        .Help("Copyright text of the game splash. Empty hides it.");

constinit BootCVar<String> SplashStudioImageVar =
    cvar::Boot("ui.splash.studio.image", "")
        .Help("Rocks Studio splash image path. Empty searches Content/Splash of the project and "
              "then of the engine.");

constinit BootCVar<String> SplashStudioCopyrightVar =
    cvar::Boot("ui.splash.studio.copyright", "")
        .Help("Copyright text of the Rocks Studio splash. Empty hides it.");

constinit BootCVar<float> SplashMinimumSecondsVar =
    cvar::Boot("ui.splash.min_seconds", 2.0f)
        .Help("Minimum time in seconds the splash screen stays visible, even when startup finishes "
              "sooner. 0 hands over to the main window as soon as it is ready.")
        .Range(0.0f, 10.0f);

} // namespace splash_settings_detail

Result<void> RegisterSplashSettings(config::ConfigContext& context) noexcept {
    using namespace splash_settings_detail;
    return context.Register(
        SplashEnabledVar, SplashGameImageVar, SplashGameShowTitleVar, SplashGameTitleVar,
        SplashGameShowProgressTextVar, SplashGameShowProgressBarVar, SplashGameCopyrightVar,
        SplashStudioImageVar, SplashStudioCopyrightVar, SplashMinimumSecondsVar);
}

SplashSettingValues ReadSplashSettings() noexcept {
    using namespace splash_settings_detail;
    SplashSettingValues values;
    values.enabled = SplashEnabledVar.Get();
    values.gameImage = SplashGameImageVar.Get();
    values.gameShowTitle = SplashGameShowTitleVar.Get();
    values.gameTitle = SplashGameTitleVar.Get();
    values.gameShowProgressText = SplashGameShowProgressTextVar.Get();
    values.gameShowProgressBar = SplashGameShowProgressBarVar.Get();
    values.gameCopyright = SplashGameCopyrightVar.Get();
    values.studioImage = SplashStudioImageVar.Get();
    values.studioCopyright = SplashStudioCopyrightVar.Get();
    values.minimumSeconds = SplashMinimumSecondsVar.Get();
    return values;
}

} // namespace rocks
