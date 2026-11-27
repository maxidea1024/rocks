#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/ManualClock.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/ActivityIndicator.h>

// UI 프레임 시계 시험이다(Docs/Design/UiToolkit 20.3절 — UE FSlateApplication::TickTime): 첫 프레임
// 간격 0, 1/8초 자름(시각은 시계 그대로), 그리기 밖 읽기, PrepareLayout의 프레임 시작, 애니메이션
// 부품이 프레임 시각으로만 위상을 정하는지.

namespace rocks::ui {

TEST(UiFramework_FrameClockClampsDeltaButKeepsClockTime) {
    SystemAllocator allocator;
    ManualClock clock(5.0);
    Application app(allocator);
    app.SetClock(clock.Function());
    app.BeginFrame();
    CHECK(app.GetFrameTime().current == 5.0);
    CHECK(app.GetFrameTime().delta == 0);

    clock.Advance(0.05);
    app.BeginFrame();
    CHECK(app.GetFrameTime().current == 5.05);
    CHECK(math::Abs(app.GetFrameTime().delta - 0.05) < 1.0e-12);

    // 긴 간격은 간격만 자르고 시각은 시계 그대로다(누적 시각이 시계와 갈라지지 않는다).
    clock.Advance(3.0);
    app.BeginFrame();
    CHECK(app.GetFrameTime().current == 8.05);
    CHECK(app.GetFrameTime().delta == MaxFrameDeltaSeconds);

    // 읽어도 전진하지 않는 시계라, 같은 시각의 프레임은 간격 0이다.
    app.BeginFrame();
    CHECK(app.GetFrameTime().delta == 0);

    // 시계를 바꾸면 다음 프레임을 첫 프레임으로 본다(거꾸로 간 시각도 간격 0).
    ManualClock earlier(1.0);
    app.SetClock(earlier.Function());
    app.BeginFrame();
    CHECK(app.GetFrameTime().current == 1.0);
    CHECK(app.GetFrameTime().delta == 0);
}

TEST(UiFramework_FrameClockReachesElementsAndTimers) {
    SystemAllocator allocator;
    ManualClock clock(2.0);
    Application app(allocator);
    app.SetClock(clock.Function());
    auto root = SharedPtr<Element>::TryMake(allocator, allocator);
    auto child = SharedPtr<Element>::TryMake(allocator, allocator);
    REQUIRE(root && child);
    REQUIRE(root.Value()->TryAddChild(child.Value()));
    // 트리에 붙기 전에는 시각 0이다.
    CHECK(child.Value()->FindFrameTime().current == 0);
    app.SetRoot(root.Value());
    REQUIRE(app.SetPopupWorkArea({}));
    app.BeginFrame();
    CHECK(child.Value()->FindFrameTime().current == 2.0);

    // PrepareLayout은 호스트가 프레임을 시작하지 않았으면 스스로 시작한다.
    int fired = 0;
    auto timer = app.StartTimer(root.Value(), 1.0,
                                Delegate<void()>::CreateLambda([&fired]() noexcept { ++fired; }));
    REQUIRE(timer);
    REQUIRE(app.PrepareLayout(Geometry::Root({10, 10}, {}), 1));
    CHECK(fired == 0);
    clock.Advance(10.0);
    REQUIRE(app.PrepareLayout(Geometry::Root({10, 10}, {}), 1));
    CHECK(child.Value()->FindFrameTime().current == 12.0);
    CHECK(child.Value()->FindFrameTime().delta == MaxFrameDeltaSeconds);
    // 간격을 잘라도 마감은 시계 시각으로 본다 — 긴 정지 뒤 타이머가 늦게 터지지 않는다.
    CHECK(fired == 1);
    // 대기 시간은 지금 시계로 잰다.
    CHECK(math::Abs(app.SecondsUntilNextTimer() - 1.0) < 1.0e-9);
    clock.Advance(0.25);
    CHECK(math::Abs(app.SecondsUntilNextTimer() - 0.75) < 1.0e-9);
}

TEST(UiFramework_ActivityIndicatorPhaseFollowsFrameTimeOnly) {
    SystemAllocator allocator;
    ManualClock clock;
    Application app(allocator);
    app.SetClock(clock.Function());
    auto indicator = SharedPtr<ActivityIndicator>::TryMake(allocator, allocator, true);
    REQUIRE(indicator);
    app.SetRoot(indicator.Value());
    const auto viewport = Geometry::Root({32, 32}, {});
    const auto paint = [&](PaintList& list) noexcept {
        return app.BuildPaintList(viewport, 1, list);
    };

    PaintList first(allocator), again(allocator), later(allocator);
    clock.Advance(0.1);
    app.BeginFrame();
    REQUIRE(paint(first));
    // 같은 프레임 시각이면 몇 번 그려도 같은 그림이다(실제 시계를 읽지 않는다).
    app.BeginFrame();
    REQUIRE(paint(again));
    REQUIRE(first.Boxes().Size() == again.Boxes().Size());
    REQUIRE(!first.Boxes().IsEmpty());
    bool same = true;
    for (usize i = 0; i < first.Boxes().Size(); ++i) {
        same = same && first.Boxes()[i] == again.Boxes()[i];
    }

    CHECK(same);
    clock.Advance(0.2);
    app.BeginFrame();
    REQUIRE(paint(later));
    REQUIRE(later.Boxes().Size() == first.Boxes().Size());
    bool moved = false;
    for (usize i = 0; i < first.Boxes().Size(); ++i) {
        moved = moved || !(first.Boxes()[i] == later.Boxes()[i]);
    }

    CHECK(moved);
}

} // namespace rocks::ui
