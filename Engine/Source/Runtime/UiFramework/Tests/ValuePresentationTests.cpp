#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <TestSupport/Test.h>
#include <UiFramework/Controls/ProgressBar.h>
#include <UiFramework/Controls/Slider.h>
#include <UiFramework/Controls/SpinBox.h>

namespace rocks::ui {

TEST(UiFramework_ProgressMasksSourceUvsBeforeRotatingAndPadding) {
    SystemAllocator allocator;
    ProgressBar bar(allocator);
    REQUIRE(bar.Arrange(Geometry::Root({20, 12}, {0, -1, 1, 0, 100, 30})));
    REQUIRE(bar.SetValue(0.25f));
    REQUIRE(bar.SetDirection(ProgressDirection::RightToLeft));
    REQUIRE(bar.SetFillStyle(ProgressFillStyle::Mask));
    REQUIRE(bar.SetBorderPadding({2, 1}));
    Brush brush{};
    brush.mirrorX = true;
    REQUIRE(bar.SetFillBrush(brush));
    PaintList list(allocator);
    REQUIRE(bar.OnPaint(list, 0));
    REQUIRE(list.Boxes().Size() == 2);
    const auto& fill = list.Boxes()[1];
    CHECK(fill.geometry.size == Vec2(4, 10));
    CHECK(fill.geometry.render.Apply({}) == Vec2(99, 44));
    CHECK(fill.uvStart.x == 0.25f && fill.uvEnd.x == 0);
    list.Clear();
    brush.mode = BrushMode::Box;
    brush.imageSize = {8, 8};
    brush.margin = {0.25f, 0.25f, 0.25f, 0.25f};
    REQUIRE(bar.SetFillBrush(brush));
    REQUIRE(bar.OnPaint(list, 0));
    CHECK(list.Boxes().Size() > 2);
    for (usize i = 1; i < list.Boxes().Size(); ++i) {
        const auto& g = list.Boxes()[i].geometry;
        CHECK(g.render.Apply({}).y >= 44 && g.render.Apply(g.size).y <= 48);
    }
}

TEST(UiFramework_SliderLockPreservesProgrammaticChangesAndFinishesCapture) {
    SystemAllocator allocator;
    Slider slider(allocator);
    REQUIRE(slider.Arrange(Geometry::Root({100, 12}, {})));
    slider.SetIndentHandle(false);
    uint32 starts = 0, ends = 0;
    slider.SetOnCaptureBegin(Delegate<void()>::CreateLambda([&]() noexcept { ++starts; }));
    slider.SetOnCaptureEnd(Delegate<void()>::CreateLambda([&]() noexcept { ++ends; }));
    // 들여쓰기 없음: 값 범위 = 손잡이 폭 절반(4) .. 96, 길이 92(UE PositionToValue 439-445행).
    // 4 + 0.25 x 92 = 27이다.
    CHECK(slider.OnPointerEvent({PointerEventType::Down, {27, 6}}).capture);
    CHECK(slider.GetNormalizedValue() == 0.25f);
    slider.SetLocked(true);
    CHECK(!slider.OnKeyEvent(0, input::InputEvent::KeyDown(input::Key::Right)));
    REQUIRE(slider.SetValue(0.5f));
    CHECK(slider.OnPointerEvent({PointerEventType::Up, {90, 6}}).releaseCapture);
    CHECK(slider.GetValue() == 0.5f && starts == 1 && ends == 1);
    slider.OnPointerCaptureLost(PointerSource::Mouse, 0, 0);
    CHECK(ends == 1);
    using B = input::GamepadButton;
    slider.SetLocked(false);
    REQUIRE(slider.SetStep(0.25f));
    CHECK(!slider.OnGamepadButtonDown(2, B::DPadRight));
    CHECK(slider.OnGamepadButtonDown(2, B::South));
    CHECK(!slider.OnGamepadButtonDown(3, B::DPadRight));
    CHECK(slider.OnGamepadButtonDown(2, B::DPadRight) && slider.GetValue() == 0.75f);
    slider.OnFocusLost(2);
    CHECK(!slider.OnGamepadButtonDown(2, B::DPadRight));
    slider.SetRequiresControllerLock(false);
    CHECK(slider.OnGamepadButtonDown(2, B::DPadLeft) && slider.GetValue() == 0.5f);
}

TEST(UiFramework_SpinBoxFormatsAndCommitsTypedValueWithDeltaSnap) {
    SystemAllocator allocator;
    SpinBox spin(allocator);
    String display(allocator);
    REQUIRE(spin.SetCaption(Delegate<Result<void>(StringView)>::CreateLambda(
        [&](StringView caption) noexcept { return display.TryAssign(caption); })));
    REQUIRE(spin.SetFractionalDigits(1, 3));
    REQUIRE(spin.SetValue(2.125f));
    CHECK(display.View() == "2.125");
    REQUIRE(spin.SetValue(2));
    CHECK(display.View() == "2.0");
    REQUIRE(spin.SetLimits(0, 100, 2));
    spin.SetSnapToStep(true);
    spin.SetSliderEnabled(false);
    uint32 commits = 0, begins = 0, ends = 0;
    spin.SetOnCommitted(Delegate<void(float, SpinCommitReason)>::CreateLambda(
        [&](float value, SpinCommitReason reason) noexcept {
            if (value == 8 && reason == SpinCommitReason::FocusLost) {
                ++commits;
            }
        }));
    spin.SetOnDragBegin(Delegate<void()>::CreateLambda([&]() noexcept { ++begins; }));
    spin.SetOnDragEnd(Delegate<void(float)>::CreateLambda([&](float) noexcept { ++ends; }));
    REQUIRE(spin.OnKeyEvent(0, input::InputEvent::Text('7')));
    REQUIRE(spin.OnKeyEvent(0, input::InputEvent::Text('.')));
    REQUIRE(spin.OnKeyEvent(0, input::InputEvent::Text('8')));
    spin.OnFocusLost(0);
    CHECK(commits == 1 && spin.Value() == 8 && display.View() == "8.0");
    REQUIRE(spin.Arrange(Geometry::Root({80, 20}, {})));
    REQUIRE(spin.OnPointerEvent({PointerEventType::Down, {2, 4}}).capture);
    (void)spin.OnPointerEvent({PointerEventType::Move, {26, 4}});
    spin.OnPointerCaptureLost(PointerSource::Mouse, 0, 0);
    CHECK(begins == 0 && ends == 0 && spin.Value() == 8);
    spin.SetSliderEnabled(true);
    REQUIRE(spin.OnPointerEvent({PointerEventType::Down, {2, 4}}).capture);
    REQUIRE(spin.OnPointerEvent({PointerEventType::Move, {18, 4}}).handled);
    spin.OnPointerCaptureLost(PointerSource::Mouse, 0, 0);
    CHECK(begins == 1 && ends == 1 && spin.Value() == 16);
}
} // namespace rocks::ui
