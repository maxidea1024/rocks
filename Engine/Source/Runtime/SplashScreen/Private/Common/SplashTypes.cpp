#include <SplashScreen/SplashTypes.h>

namespace rocks::splash {

StringView SplashSkipReasonName(SplashSkipReason reason) noexcept {
    switch (reason) {
    case SplashSkipReason::None:
        return "None";
    case SplashSkipReason::UnsupportedPlatform:
        return "UnsupportedPlatform";
    case SplashSkipReason::NotInteractiveHost:
        return "NotInteractiveHost";
    case SplashSkipReason::NoMainWindow:
        return "NoMainWindow";
    case SplashSkipReason::DisabledByCommandLine:
        return "DisabledByCommandLine";
    case SplashSkipReason::Headless:
        return "Headless";
    case SplashSkipReason::Unattended:
        return "Unattended";
    case SplashSkipReason::DisabledBySettings:
        return "DisabledBySettings";
    case SplashSkipReason::NoDesktopSession:
        return "NoDesktopSession";
    case SplashSkipReason::NoMainThreadHost:
        return "NoMainThreadHost";
    case SplashSkipReason::ImageNotFound:
        return "ImageNotFound";
    case SplashSkipReason::ImageInvalid:
        return "ImageInvalid";
    case SplashSkipReason::WindowFailed:
        return "WindowFailed";
    }

    return "Unknown";
}

SplashVisibilityInput MakeSplashVisibilityInput() noexcept {
    SplashVisibilityInput input;
    input.platformSupported = ROCKS_PLATFORM_WINDOWS != 0 || ROCKS_PLATFORM_MAC != 0;
    input.requiresMainThreadHost = ROCKS_PLATFORM_MAC != 0;
    return input;
}

SplashSkipReason DecideSplashVisibility(const SplashVisibilityInput& input) noexcept {
    if (!input.platformSupported) {
        return SplashSkipReason::UnsupportedPlatform;
    }

    if (!input.interactiveHost) {
        return SplashSkipReason::NotInteractiveHost;
    }

    if (!input.opensMainWindow) {
        return SplashSkipReason::NoMainWindow;
    }

    if (input.noSplashOption) {
        return SplashSkipReason::DisabledByCommandLine;
    }

    if (input.headless) {
        return SplashSkipReason::Headless;
    }

    if (input.unattended) {
        return SplashSkipReason::Unattended;
    }

    if (!input.settingsEnabled) {
        return SplashSkipReason::DisabledBySettings;
    }

    if (input.desktop == platform::DesktopSessionState::Disconnected) {
        return SplashSkipReason::NoDesktopSession;
    }

    if (input.requiresMainThreadHost && !input.mainThreadHost) {
        return SplashSkipReason::NoMainThreadHost;
    }

    return SplashSkipReason::None;
}

} // namespace rocks::splash
