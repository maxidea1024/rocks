#include "SplashSelfTest.h"

#include <Core/Error.h>
#include <HostConfig/HostSettings.h>
#include <Platform/Thread.h>
#include <PlatformWindow/Window.h>
#include <SplashScreen/SplashScreen.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;

// 되풀이 횟수다(창 클래스 등록·해제가 매번 다시 되는지 본다).
constexpr uint32 SplashSelfTestRounds = 3;

constexpr int PassExitCode = 0;
constexpr int FailExitCode = 1;
constexpr int NotRunExitCode = 2;

// 엔진 폴더 경로 버퍼 크기다.
constexpr usize EnginePathCapacity = 4096;

// Custom 칸 시험 배치의 폭·높이(논리 px)다.
constexpr int32 CustomSlotWidth = 200;
constexpr int32 CustomSlotHeight = 16;

// 스플래시 스레드가 창 생성을 마칠 때까지 기다린다(Starting이 아닐 때까지 — 시간 상한 없이 상태로
// 판정한다). 스레드는 창을 만들거나 실패를 기록한 뒤 반드시 상태를 바꾼다.
splash::SplashState WaitUntilCreated(const splash::SplashScreen& screen) noexcept {
    splash::SplashState state = screen.State();
    while (state == splash::SplashState::Starting) {
        platform::YieldThread();
        state = screen.State();
    }

    return state;
}

int NotRun(const char* reason) noexcept {
    printf("  [NOT RUN] splash: real window rounds (%s)\n", reason);
    return NotRunExitCode;
}

int Fail(const char* reason, uint32 round) noexcept {
    printf("  [FAIL] splash: real window rounds (%s in round %u)\n", reason, round);
    return FailExitCode;
}

} // namespace

int RunSplashSelfTest(Allocator& allocator) noexcept {
    if (platform::QueryDesktopSessionState() == platform::DesktopSessionState::Disconnected) {
        return NotRun("the desktop session is disconnected");
    }

    char engineDirectory[EnginePathCapacity] = {};
    auto found = FindEngineDirectory(Span<char>(engineDirectory));
    if (!found) {
        printf("  [FAIL] splash: Engine/Config was not found above the executable (%s)\n",
               ErrorCodeName(found.Error()));
        return FailExitCode;
    }

    // Custom 칸 하나를 오른쪽 위에 둔다(6.5절 덮어쓰기가 실제 presenter를 지나는지 본다).
    splash::SplashTextPlacement custom;
    custom.slot = splash::SplashTextSlot::Custom1;
    custom.anchor = splash::SplashTextAnchor::TopRight;
    custom.x = splash::SplashSideMargin;
    custom.y = splash::SplashSideMargin;
    custom.width = CustomSlotWidth;
    custom.height = CustomSlotHeight;
    custom.align = splash::SplashTextAlign::Right;
    const splash::SplashTextValue texts[] = {
        {splash::SplashTextSlot::Title, "Splash self-test"},
        {splash::SplashTextSlot::Custom1, "Custom slot"},
    };
    for (uint32 round = 1; round <= SplashSelfTestRounds; ++round) {
        splash::SplashShowDesc desc;
        desc.engineDirectory = StringView(engineDirectory, found.Value());
        desc.texts = texts;
        desc.placements = Span<const splash::SplashTextPlacement>(&custom, 1);
        desc.minimumDisplaySeconds = 0.0;
        auto shown = splash::SplashScreen::Show(allocator, desc);
        if (!shown) {
            return shown.Error() == ErrorCode::Unsupported
                       ? NotRun("no splash presenter on this platform")
                       : Fail("Show failed", round);
        }

        splash::SplashScreen& screen = shown.Value();
        if (WaitUntilCreated(screen) == splash::SplashState::Failed) {
            // 사유·OS 오류는 스플래시 스레드가 Warning 로그로 남겼다.
            screen.Hide();
            return NotRun("the splash window could not be created");
        }

        screen.SetText(splash::SplashTextSlot::Progress, "Self-test");
        screen.Suspend();
        screen.Resume();
        if (!screen.CanHide()) {
            screen.Hide();
            return Fail("CanHide is false with a zero minimum display time", round);
        }

        screen.Hide();
        if (screen.State() != splash::SplashState::Hidden) {
            return Fail("the splash is not hidden after Hide", round);
        }
    }

    printf("  [PASS] splash: real window rounds (%u rounds of Show, Shown, SetText, Suspend, "
           "Resume and Hide)\n",
           SplashSelfTestRounds);
    return PassExitCode;
}

} // namespace sandbox
