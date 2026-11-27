#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application/Application.h>

// 다음 타이머 마감 시험이다(Docs/Design/UiToolkit 19.2절 펌프 수준 대기 — 호스트가 이것으로 대기
// 시간을 정한다). 시계는 주입해 결정적으로 진행한다.

namespace rocks::ui {

namespace {

double deadlineClock = 0;

double DeadlineClock() noexcept {
    return deadlineClock;
}

} // namespace

TEST(UiFramework_SecondsUntilNextTimerFollowsTimersAndFrames) {
    SystemAllocator allocator;
    deadlineClock = 10.0;
    Application app(allocator);
    app.SetClock(ClockFunction::CreateStatic(&DeadlineClock));
    // 타이머는 프레임 시각으로 등록된다(20.3절) — 프레임을 시작해 10초에 둔다.
    app.BeginFrame();
    auto root = SharedPtr<Element>::TryMake(allocator, allocator);
    REQUIRE(root);
    app.SetRoot(root.Value());
    // 타이머가 없으면 기다릴 마감이 없다.
    CHECK(app.SecondsUntilNextTimer() == NoTimerDeadline);
    auto timer =
        app.StartTimer(root.Value(), 0.5, Delegate<void()>::CreateLambda([]() noexcept {}));
    REQUIRE(timer);
    CHECK(app.SecondsUntilNextTimer() == 0.5);
    deadlineClock = 10.2;
    CHECK(math::Abs(app.SecondsUntilNextTimer() - 0.3) < 1.0e-9);
    // 지난 마감은 0이다.
    deadlineClock = 11.0;
    CHECK(app.SecondsUntilNextTimer() == 0);
    // 프레임 타이머(애니메이션)가 있으면 매 프레임 돈다.
    auto frame = app.StartFrameTimer(root.Value(),
                                     Delegate<void(double)>::CreateLambda([](double) noexcept {}));
    REQUIRE(frame);
    deadlineClock = 10.0;
    CHECK(app.SecondsUntilNextTimer() == 0);
    // 수명 토큰을 놓으면 그 타이머는 마감에서 빠진다.
    frame.Value() = {};
    timer.Value() = {};
    CHECK(app.SecondsUntilNextTimer() == NoTimerDeadline);
}

} // namespace rocks::ui
