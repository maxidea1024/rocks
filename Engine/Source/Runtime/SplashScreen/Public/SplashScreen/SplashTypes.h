#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>
#include <PlatformWindow/Window.h>

// 시작 화면(스플래시)의 공용 타입과 표시 판정이다(Docs/Design/SplashScreen 4·7·11절).
namespace rocks::splash {

// 스플래시 종류다. 이미지 이름(Splash·StudioSplash)과 칸 표시·창 성질이 다르다.
enum class SplashKind : uint8 {
    Game,   // 게임·엔진 창 프로그램(Sandbox)
    Studio, // Rocks Studio(에디터)
};

// 띄우지 않은(또는 못 띄운) 사유다. None이면 띄운다. 4절 표의 순서다.
enum class SplashSkipReason : uint8 {
    None,
    UnsupportedPlatform,   // Windows·macOS가 아니다
    NotInteractiveHost,    // 게임 호스트가 아니다(명령·도구·시험)
    NoMainWindow,          // 이 실행이 주 창을 열지 않는다
    DisabledByCommandLine, // --no-splash
    Headless,              // 창·그래픽 장치를 만들지 않는 실행
    Unattended,            // 사람이 보지 않는 실행
    DisabledBySettings,    // ui.splash.enabled = false
    NoDesktopSession,      // 데스크톱 세션이 표시 장치에서 떨어졌다
    NoMainThreadHost,      // macOS에서 본문이 메인 스레드를 막는다
    ImageNotFound,         // 스플래시 이미지가 없다
    ImageInvalid,          // 이미지를 읽거나 풀 수 없다·크기 규칙 위반
    WindowFailed,          // 창을 만들지 못했다
};

// 사유의 영어 이름이다(로그용, 예: "DisabledByCommandLine").
ROCKS_SPLASHSCREEN_API StringView SplashSkipReasonName(SplashSkipReason reason) noexcept;

// 스플래시의 상태다(SplashScreen::State).
enum class SplashState : uint8 {
    Starting, // 스플래시 스레드가 창을 만드는 중
    Shown,    // 창이 떠 있다(Suspend로 잠시 숨겼어도 Shown)
    Failed,   // 창을 만들지 못했다(사유는 스플래시 스레드가 로그로 남겼다)
    Hidden,   // Hide가 끝났다
};

// 글자 칸이다(6.1절). Custom 칸(UE User1–6)은 호스트가 배치(SplashTextPlacement)를 준 칸만 그린다
// (6.5절).
enum class SplashTextSlot : uint8 {
    Title,
    Version,
    Progress,
    Copyright,
    Custom1,
    Custom2,
    Custom3,
    Custom4,
    Custom5,
    Custom6,
};

// 글자 칸 수다(기본 칸 넷 + Custom 칸 여섯).
inline constexpr usize SplashTextSlotCount = 10;

// 칸 한 줄의 최대 byte 수(UTF-8)다. 넘으면 UTF-8 문자 경계에서 자른다.
inline constexpr usize MaxSplashLineBytes = 512;

// 표시 판정의 입력이다. 호스트는 MakeSplashVisibilityInput으로 이 빌드의 플랫폼 값을 받은 뒤
// 나머지를 채운다(시험은 모든 칸을 직접 넣는다).
struct SplashVisibilityInput {
    // 스플래시 구현이 있는 플랫폼(Windows·macOS)인지다.
    bool platformSupported = false;
    // 게임 호스트(AppInfo HostKind::Game — 게임·Studio·Sandbox 엔진 실행)인지다.
    bool interactiveHost = false;
    // 이 실행이 주 창을 여는지다(4절 호스트별 표 — Sandbox는 --window일 때만).
    bool opensMainWindow = false;
    // --no-splash를 받았는지다.
    bool noSplashOption = false;
    // 창·그래픽 장치를 만들지 않는 실행인지다(RunTraits::headless).
    bool headless = false;
    // 사람이 보지 않는 실행인지다(RunTraits::unattended).
    bool unattended = false;
    // ui.splash.enabled 값이다.
    bool settingsEnabled = true;
    // 데스크톱 세션 상태다(platform::QueryDesktopSessionState). Disconnected만 막는다.
    platform::DesktopSessionState desktop = platform::DesktopSessionState::Unknown;
    // 이 플랫폼이 메인 스레드 호스트를 요구하는지(macOS)와 호스트가 그것을 쓰는지다.
    bool requiresMainThreadHost = false;
    bool mainThreadHost = false;
};

// 이 빌드의 플랫폼 값(platformSupported·requiresMainThreadHost)을 채운 입력이다. 나머지는
// 기본값이다.
ROCKS_SPLASHSCREEN_API SplashVisibilityInput MakeSplashVisibilityInput() noexcept;

// 띄울지 판정한다(4절). 위 사유 순서대로 처음 맞는 것을 돌려주고, 모두 아니면 None이다. 부수 효과가
// 없다.
//
// 사용 예:
//   splash::SplashVisibilityInput input = splash::MakeSplashVisibilityInput();
//   input.interactiveHost = true;
//   input.opensMainWindow = options.window;
//   input.noSplashOption = options.noSplash;
//   input.settingsEnabled = ReadSplashSettings().enabled;
//   input.desktop = platform::QueryDesktopSessionState();
//   if (splash::DecideSplashVisibility(input) == splash::SplashSkipReason::None) { ... Show ... }
ROCKS_SPLASHSCREEN_API SplashSkipReason
DecideSplashVisibility(const SplashVisibilityInput& input) noexcept;

// 진행 원천이 한 번 당길 때 채우는 상태다(7.1절).
struct SplashStatus {
    // false면 막대는 불확정 표시다(아직 비율이 없다).
    bool determinate = false;
    // 0..1이다(determinate일 때). 범위 밖·NaN은 잘라 쓴다.
    double fraction = 0.0;
    // 시작 작업이 끝났다(진행 문구가 100%가 된다).
    bool finished = false;
    // true면 아래 문구로 진행 문구를 바꾼다(빈 문구 = 비우기).
    bool messageChanged = false;
    // UTF-8이고 NUL이 없어도 된다. messageBytes까지만 읽는다.
    char message[MaxSplashLineBytes]{};
    uint32 messageBytes = 0;
};

// 진행 원천이다. SplashScreen이 소유한다(7.1절): poll은 스플래시 스레드에서 갱신 간격마다 불리고,
// release는 Hide가 스플래시 스레드를 멈춘 뒤 한 번 불린다(Show가 실패해도 불린다).
struct SplashStatusSource {
    // 바뀐 것이 없으면 false다.
    bool (*poll)(void* context, SplashStatus& out) noexcept = nullptr;
    void (*release)(void* context) noexcept = nullptr;
    void* context = nullptr;
};

} // namespace rocks::splash
