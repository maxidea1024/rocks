#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application.h>
#include <UiFramework/InputKeySelector.h>
#include <UiFramework/InputAdapter.h>
#include <UiFramework/ScrollBar.h>
#include <UiFramework/ProgressBar.h>
#include <UiFramework/Slider.h>
#include <UiFramework/SpinBox.h>

namespace rocks::ui {

TEST(UiFramework_ScrollBarKeepsGrabPointAndClampsToContentEnd) {
    SystemAllocator allocator;
    ScrollBar bar(allocator);
    REQUIRE(bar.Arrange(Geometry::Root({12, 100}, {})));
    REQUIRE(bar.SetState(0.25f, 0.25f));
    float position = -1;
    bar.SetOnScrolled(
        Delegate<void(float)>::CreateLambda([&](float value) noexcept { position = value; }));
    CHECK(bar.OnPointerEvent({PointerEventType::Down, {6, 30}}).capture);
    CHECK(position == -1);
    CHECK(bar.OnPointerEvent({PointerEventType::Move, {6, 65}}).handled);
    CHECK(math::Abs(position - 0.60f) < 0.0001f);
    CHECK(bar.OnPointerEvent({PointerEventType::Move, {6, 140}}).handled);
    CHECK(position == 0.75f);
    CHECK(bar.OnPointerEvent({PointerEventType::Cancel, {}}).releaseCapture);
    CHECK(!bar.OnPointerEvent({PointerEventType::Move, {0, 0}}).handled);
    REQUIRE(!bar.SetState(0, -1));
    CHECK(bar.Offset() == 0.75f);
}

TEST(UiFramework_SpinBoxCommitsTextAndRejectsInvalidEntry) {
    SystemAllocator allocator;
    SpinBox box(allocator);
    REQUIRE(box.SetLimits(-10, 10, 2));
    uint32 changes = 0;
    box.SetOnChanged(Delegate<void(float)>::CreateLambda([&](float) noexcept { ++changes; }));
    REQUIRE(box.OnKeyEvent(0, input::InputEvent::KeyDown(input::Key::Up)));
    CHECK(box.Value() == 2);
    REQUIRE(box.OnKeyEvent(0, input::InputEvent::Text('-')));
    REQUIRE(box.OnKeyEvent(0, input::InputEvent::Text('7')));
    REQUIRE(box.OnKeyEvent(0, input::InputEvent::KeyDown(input::Key::Enter)));
    CHECK(box.Value() == -7 && changes == 2);
    REQUIRE(box.OnKeyEvent(0, input::InputEvent::Text('1')));
    REQUIRE(box.OnKeyEvent(0, input::InputEvent::Text('e')));
    REQUIRE(box.OnKeyEvent(0, input::InputEvent::KeyDown(input::Key::Enter)));
    CHECK(box.Value() == -7 && !box.IsEditing() && changes == 2);
    REQUIRE(box.Arrange(Geometry::Root({100, 30}, {})));
    CHECK(box.OnPointerEvent({PointerEventType::Down, {4, 4}}).capture);
    CHECK(box.OnPointerEvent({PointerEventType::Move, {24, 4}}).handled);
    CHECK(box.OnPointerEvent({PointerEventType::Up, {24, 4}}).releaseCapture);
    CHECK(box.Value() == 3 && changes == 3);
}

TEST(UiFramework_KeySelectorUsesHeldModifiersAndCancelsOnEscape) {
    SystemAllocator allocator;
    auto selector = SharedPtr<InputKeySelector>::TryMake(allocator, allocator);
    REQUIRE(selector);
    Application app(allocator);
    app.SetRoot(selector.Value());
    REQUIRE(app.RouteKey(0, input::InputEvent::KeyDown(input::Key::LeftControl)));
    REQUIRE(app.SetFocus(0, selector.Value()));
    REQUIRE(app.RouteKey(0, input::InputEvent::KeyDown(input::Key::Enter)));
    REQUIRE(app.RouteKey(0, input::InputEvent::KeyUp(input::Key::Enter)));
    REQUIRE(selector.Value()->IsSelecting());
    REQUIRE(app.RouteKey(0, input::InputEvent::KeyDown(input::Key::A)));
    CHECK(selector.Value()->SelectedKey().key == input::Key::A &&
          selector.Value()->SelectedKey().control);
    REQUIRE(app.RouteKey(0, input::InputEvent::KeyDown(input::Key::Enter)));
    REQUIRE(app.RouteKey(0, input::InputEvent::KeyUp(input::Key::Enter)));
    REQUIRE(app.RouteKey(0, input::InputEvent::KeyDown(input::Key::Escape)));
    CHECK(!selector.Value()->IsSelecting() && selector.Value()->SelectedKey().key == input::Key::A);
}

TEST(UiFramework_KeySelectorCapturesDeviceButtonsAndPersistsNames) {
    SystemAllocator allocator;
    auto selector = SharedPtr<InputKeySelector>::TryMake(allocator, allocator);
    REQUIRE(selector);
    Application app(allocator);
    app.SetRoot(selector.Value());
    REQUIRE(app.UpdatePaintCache(Geometry::Root({110, 24}, {}), 1));
    InputAdapter adapter(app);
    using input::GamepadButton;
    using input::InputEvent;
    using input::MouseButton;

    REQUIRE(adapter.Route(InputEvent::MouseDown(MouseButton::Left, 10, 10)).Value());
    REQUIRE(adapter.Route(InputEvent::MouseUp(MouseButton::Left, 10, 10)).Value());
    REQUIRE(selector.Value()->IsSelecting());
    REQUIRE(adapter.Route(InputEvent::MouseDown(MouseButton::X1, 10, 10)).Value());
    CHECK(selector.Value()->SelectedKey().mouse == MouseButton::X1);
    REQUIRE(adapter.Route(InputEvent::MouseDown(MouseButton::Right, 10, 10)).Value());
    CHECK(selector.Value()->SelectedKey() == KeyChord{});

    REQUIRE(app.SetFocus(0, selector.Value()));
    REQUIRE(adapter.Route(InputEvent::KeyDown(input::Key::Enter)).Value());
    REQUIRE(adapter.Route(InputEvent::KeyUp(input::Key::Enter)).Value());
    input::GamepadSnapshot pad{.connected = true,
                               .buttons = 1u << static_cast<uint32>(GamepadButton::East)};
    REQUIRE(adapter.Route(InputEvent::Gamepad(0, pad)).Value());
    CHECK(selector.Value()->IsSelecting());
    selector.Value()->SetAllowGamepad(true);
    CHECK(!adapter.Route(InputEvent::Gamepad(0, pad)).Value());
    pad.buttons = 1u << static_cast<uint32>(GamepadButton::DPadRight);
    REQUIRE(adapter.Route(InputEvent::Gamepad(0, pad)).Value());
    auto chord = selector.Value()->SelectedKey();
    CHECK(chord.gamepad == GamepadButton::DPadRight && !selector.Value()->IsSelecting());
    auto loaded = KeyChord::FromInputName(chord.InputName());
    REQUIRE(loaded);
    CHECK(chord == loaded.Value());
    chord.mouse = MouseButton::Left;
    CHECK(!selector.Value()->SetSelectedKey(chord));
}

TEST(UiFramework_ProgressBarDrawsSevenFillDirectionsAndRestoresCache) {
    SystemAllocator allocator;
    ProgressBar bar(allocator);
    REQUIRE(bar.Arrange(Geometry::Root({80, 40}, {})));
    REQUIRE(bar.SetValue(0.25f));
    struct FillCase {
        ProgressDirection direction;
        Vec2 size;
        Vec2 offset;
    };
    const FillCase cases[] = {{ProgressDirection::LeftToRight, {20, 40}, {0, 0}},
                              {ProgressDirection::RightToLeft, {20, 40}, {60, 0}},
                              {ProgressDirection::TopToBottom, {80, 10}, {0, 0}},
                              {ProgressDirection::BottomToTop, {80, 10}, {0, 30}},
                              {ProgressDirection::FromCenter, {20, 10}, {30, 15}},
                              {ProgressDirection::FromCenterHorizontal, {20, 40}, {30, 0}},
                              {ProgressDirection::FromCenterVertical, {80, 10}, {0, 15}}};
    for (const auto& item : cases) {
        PaintList list(allocator);
        REQUIRE(bar.SetDirection(item.direction));
        REQUIRE(bar.OnPaint(list, 1));
        REQUIRE(list.Boxes().Size() == 2);
        const auto& fill = list.Boxes()[1].geometry;
        CHECK(fill.size == item.size && fill.render.Apply({}) == item.offset);
    }

    REQUIRE(bar.SetValue(0));
    bar.SetMarquee(true);
    CHECK(bar.IsVolatile());
    bar.SetForceVolatile(true);
    bar.SetMarquee(false);
    CHECK(bar.IsVolatile());
    bar.SetForceVolatile(false);
    CHECK(!bar.IsVolatile() && bar.GetValue() == 0);
}

TEST(UiFramework_VerticalSliderUsesTheSameTrackForPickingAndPaint) {
    SystemAllocator allocator;
    Slider slider(allocator);
    REQUIRE(slider.SetRange(-2, 6));
    REQUIRE(slider.SetStep(2));
    slider.SetVertical(true);
    slider.SetSnapToStep(true);
    REQUIRE(slider.Arrange(Geometry::Root({12, 92}, {})));
    CHECK(slider.OnPointerEvent({PointerEventType::Down, {6, 62}}).capture);
    CHECK(slider.GetValue() == 0);
    CHECK(slider.OnPointerEvent({PointerEventType::Up, {6, 6}}).releaseCapture);
    CHECK(slider.GetValue() == 6);
    CHECK(slider.OnKeyEvent(0, input::InputEvent::KeyDown(input::Key::Down)));
    CHECK(slider.GetValue() == 4);
    PaintList list(allocator);
    REQUIRE(slider.OnPaint(list, 0));
    REQUIRE(list.Boxes().Size() == 3);
    const auto& handle = list.Boxes()[2].geometry;
    CHECK(handle.render.Apply({6, 6}) == Vec2(6, 26));
}

} // namespace rocks::ui
