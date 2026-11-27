#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/ManualClock.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/ActivityIndicator.h>
#include <UiFramework/Effects/CachePanel.h>

// UI 프레임 시계 시험이다(Docs/Design/UiToolkit 20.3절 — UE FSlateApplication::TickTime): 첫 프레임
// 간격 0, 1/8초 자름(시각은 시계 그대로), 그리기 밖 읽기, PrepareLayout의 프레임 시작, 애니메이션
// 부품이 프레임 시각으로만 위상을 정하는지.

namespace rocks::ui {

TEST(UiFramework_FrameClockClampsDeltaButKeepsClockTime) {
    SystemAllocator allocator;
    ManualClock clock(5.0);
    ManualClock replacement(1.0);
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

    // 프레임을 연 뒤 시계를 바꾸면 UI 시각을 이어 붙인다(새 시계 1초 = UI 8.05초).
    app.SetClock(replacement.Function());
    app.BeginFrame();
    CHECK(app.GetFrameTime().current == 8.05);
    CHECK(app.GetFrameTime().delta == 0);
    replacement.Advance(0.1);
    app.BeginFrame();
    CHECK(math::Abs(app.GetFrameTime().current - 8.15) < 1.0e-12);
    CHECK(math::Abs(app.GetFrameTime().delta - 0.1) < 1.0e-12);

    // 바깥 시각은 유한하지 않으면 무시하고 간격을 자른다.
    app.BeginFrame(FrameTime{100.0, 5.0});
    CHECK(app.GetFrameTime().current == 100.0);
    CHECK(app.GetFrameTime().delta == MaxFrameDeltaSeconds);
    app.BeginFrame(FrameTime{platform::PlatformMath::Infinity<double>(), 0});
    CHECK(app.GetFrameTime().current == 100.0);
}

TEST(UiFramework_ImplicitFramesOpenOncePerLayout) {
    SystemAllocator allocator;
    ManualClock clock(2.0);
    Application app(allocator);
    app.SetClock(clock.Function());
    auto root = SharedPtr<Element>::TryMake(allocator, allocator);
    auto child = SharedPtr<Element>::TryMake(allocator, allocator);
    REQUIRE(root && child);
    REQUIRE(root.Value()->TryAddChild(child.Value()));
    // 트리에 붙기 전에는 시각 0이다. 루트를 받으면 묶음이 생긴다.
    CHECK(child.Value()->FindFrameTime().current == 0);
    app.SetRoot(root.Value());
    CHECK(child.Value()->FindUiHostServices() != nullptr);

    // BeginFrame을 부르지 않는 호스트는 입력·배치가 프레임을 연다.
    int fired = 0;
    auto timer = app.StartTimer(root.Value(), 1.0,
                                Delegate<void()>::CreateLambda([&fired]() noexcept { ++fired; }));
    REQUIRE(timer);
    const auto viewport = Geometry::Root({10, 10}, {});
    REQUIRE(app.PrepareLayout(viewport, 1));
    CHECK(child.Value()->FindFrameTime().current == 2.0);
    CHECK(fired == 0);
    clock.Advance(10.0);
    REQUIRE(app.PrepareLayout(viewport, 1));
    CHECK(child.Value()->FindFrameTime().current == 12.0);
    CHECK(child.Value()->FindFrameTime().delta == MaxFrameDeltaSeconds);
    // 간격을 잘라도 마감은 시계 시각으로 본다 — 긴 정지 뒤 타이머가 늦게 터지지 않는다.
    CHECK(fired == 1);
    // 대기 시간은 지금 시계로 잰다.
    CHECK(math::Abs(app.SecondsUntilNextTimer() - 1.0) < 1.0e-9);
    clock.Advance(0.25);
    CHECK(math::Abs(app.SecondsUntilNextTimer() - 0.75) < 1.0e-9);
}

TEST(UiFramework_FrameTimersAdvanceOncePerFrameByClampedDelta) {
    SystemAllocator allocator;
    ManualClock clock;
    Application app(allocator);
    app.SetClock(clock.Function());
    auto root = SharedPtr<Element>::TryMake(allocator, allocator);
    REQUIRE(root);
    app.SetRoot(root.Value());
    app.BeginFrame();
    double total = 0;
    int calls = 0;
    auto frame = app.StartFrameTimer(
        root.Value(), Delegate<void(double)>::CreateLambda([&](double elapsed) noexcept {
            total += elapsed;
            ++calls;
        }));
    REQUIRE(frame);
    const auto viewport = Geometry::Root({10, 10}, {});
    // 건 프레임의 진행은 0이다.
    REQUIRE(app.PrepareLayout(viewport, 1));
    CHECK(calls == 1);
    CHECK(total == 0);
    // 3초 멈춘 뒤의 프레임은 자른 간격(1/8초)만 진행한다. 같은 프레임의 배치를 또 해도 한 번이다.
    clock.Advance(3.0);
    app.BeginFrame();
    REQUIRE(app.PrepareLayout(viewport, 1));
    REQUIRE(app.PrepareLayout(viewport, 1));
    CHECK(calls == 2);
    CHECK(total == MaxFrameDeltaSeconds);
    // 명시 시작 호스트에서는 배치가 프레임을 새로 열지 않는다(프레임 시각이 그대로다).
    clock.Advance(1.0);
    REQUIRE(app.PrepareLayout(viewport, 1));
    CHECK(app.GetFrameTime().current == 3.0);
    CHECK(calls == 2);
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

TEST(UiFramework_LocalPaintCacheForwardsFrameTime) {
    SystemAllocator allocator;
    ManualClock clock;
    Application app(allocator);
    app.SetClock(clock.Function());
    auto panel = SharedPtr<CachePanel>::TryMake(allocator, allocator);
    auto indicator = SharedPtr<ActivityIndicator>::TryMake(allocator, allocator, true);
    REQUIRE(panel && indicator);
    REQUIRE(panel.Value()->TryAddChild(indicator.Value()));
    app.SetRoot(panel.Value());
    const auto viewport = Geometry::Root({32, 32}, {});
    PaintList first(allocator), later(allocator);
    clock.Advance(0.1);
    app.BeginFrame();
    REQUIRE(app.BuildPaintList(viewport, 1, first));
    clock.Advance(0.2);
    app.BeginFrame();
    REQUIRE(app.BuildPaintList(viewport, 1, later));
    REQUIRE(!first.Boxes().IsEmpty());
    REQUIRE(first.Boxes().Size() == later.Boxes().Size());
    bool moved = false;
    for (usize i = 0; i < first.Boxes().Size(); ++i) {
        moved = moved || !(first.Boxes()[i] == later.Boxes()[i]);
    }

    // CachePanel 안의 volatile throbber가 바깥 프레임 시각으로 다시 그려진다.
    CHECK(moved);
}

} // namespace rocks::ui
