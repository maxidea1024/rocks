#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/InputKeySelector.h>
#include <UiFramework/Application/InputAdapter.h>
#include <UiFramework/Scrolling/ScrollBar.h>
#include <UiFramework/Controls/ProgressBar.h>
#include <UiFramework/Controls/Slider.h>
#include <UiFramework/Controls/SpinBox.h>
#include <UiFramework/Layout/Frame.h>

namespace rocks::ui {

TEST(UiFramework_ScrollBarKeepsGrabPointAndClampsToContentEnd) {
    SystemAllocator allocator;
    ScrollBar bar(allocator);
    // 길이 200, 손잡이 50(최소 35보다 길다), 이동 150, 손잡이 시작 150 x (0.25 / 0.75) = 50.
    REQUIRE(bar.Arrange(Geometry::Root({12, 200}, {})));
    REQUIRE(bar.SetState(0.25f, 0.25f));
    float position = -1;
    bar.SetOnScrolled(
        Delegate<void(float)>::CreateLambda([&](float value) noexcept { position = value; }));
    // 손잡이 안(60)을 잡으면 잡은 점(10)을 유지한다: (130 - 10) / 150 x 0.75 = 0.6.
    CHECK(bar.OnPointerEvent({PointerEventType::Down, {6, 60}}).capture);
    CHECK(position == -1);
    CHECK(bar.OnPointerEvent({PointerEventType::Move, {6, 130}}).handled);
    CHECK(math::Abs(position - 0.60f) < 0.0001f);
    CHECK(bar.OnPointerEvent({PointerEventType::Move, {6, 280}}).handled);
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
    REQUIRE(box.SendKey(0, input::InputEvent::KeyDown(input::Key::Up)).IsHandled());
    CHECK(box.Value() == 2);
    REQUIRE(box.SendKey(0, input::InputEvent::Text('-')).IsHandled());
    REQUIRE(box.SendKey(0, input::InputEvent::Text('7')).IsHandled());
    REQUIRE(box.SendKey(0, input::InputEvent::KeyDown(input::Key::Enter)).IsHandled());
    CHECK(box.Value() == -7 && changes == 2);
    REQUIRE(box.SendKey(0, input::InputEvent::Text('1')).IsHandled());
    REQUIRE(box.SendKey(0, input::InputEvent::Text('e')).IsHandled());
    REQUIRE(box.SendKey(0, input::InputEvent::KeyDown(input::Key::Enter)).IsHandled());
    CHECK(box.Value() == -7 && !box.IsEditing() && changes == 2);
    REQUIRE(box.Arrange(Geometry::Root({100, 30}, {})));
    CHECK(box.OnPointerEvent({PointerEventType::Down, {4, 4}}).capture);
    CHECK(box.OnPointerEvent({PointerEventType::Move, {24, 4}}).handled);
    CHECK(box.OnPointerEvent({PointerEventType::Up, {24, 4}}).releaseCapture);
    CHECK(box.Value() == 3 && changes == 3);
}

// 키 선택 크기는 내용 + "Button" 여백 + margin이다(23.4절 — 고정 100x24 없음, 기본 margin 0).
TEST(UiFramework_KeySelectorSizesFromContentStylePaddingAndMargin) {
    SystemAllocator allocator;
    auto selector = SharedPtr<InputKeySelector>::TryMake(allocator, allocator);
    auto content = SharedPtr<Frame>::TryMake(allocator, allocator);
    REQUIRE(selector && content);
    constexpr Vec2 ContentSize{12, 14};
    content.Value()->SetDesiredSize(ContentSize);
    REQUIRE(selector.Value()->Construct({.content = content.Value()}));
    CHECK(selector.Value()->GetMargin() == Vec4{});
    CHECK(selector.Value()->GetStyle() == DefaultButtonStyle());
    const Vec4 padding = DefaultButtonStyle().normalPadding;
    REQUIRE(selector.Value()->Prepass(1));
    CHECK(selector.Value()->GetDesiredSize() ==
          Vec2(ContentSize.x + padding.x + padding.z, ContentSize.y + padding.y + padding.w));
    constexpr Vec4 Margin{3, 1, 5, 2};
    REQUIRE(selector.Value()->SetMargin(Margin));
    REQUIRE(selector.Value()->Prepass(2));
    CHECK(selector.Value()->GetDesiredSize() ==
          Vec2(ContentSize.x + padding.x + padding.z + Margin.x + Margin.z,
               ContentSize.y + padding.y + padding.w + Margin.y + Margin.w));
    CHECK(!selector.Value()->SetMargin({-1, 0, 0, 0}));
    CHECK(selector.Value()->GetMargin() == Margin);
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
    // 길이 96, 손잡이 폭 8, 들여쓰기: 값 위치 = 8 + 비율 x 80(UE SSlider 81-92·439-445행).
    REQUIRE(slider.Arrange(Geometry::Root({12, 96}, {})));
    // 아래에서 48(위에서 48) = 비율 0.5 = 값 2. 단계 2로 맞춘다.
    CHECK(slider.OnPointerEvent({PointerEventType::Down, {6, 48}}).capture);
    CHECK(slider.GetValue() == 2);
    // 위 끝 근처(아래에서 92)는 범위 밖이라 최대값이다.
    CHECK(slider.OnPointerEvent({PointerEventType::Up, {6, 4}}).releaseCapture);
    CHECK(slider.GetValue() == 6);
    CHECK(slider.SendKey(0, input::InputEvent::KeyDown(input::Key::Down)).IsHandled());
    CHECK(slider.GetValue() == 4);
    PaintList list(allocator);
    REQUIRE(slider.OnPaint(list, 0));
    // 막대와 손잡이다(기본 슬라이더에는 채움이 없다).
    REQUIRE(list.Boxes().Size() == 2);
    const auto& handle = list.Boxes()[1].geometry;
    CHECK(handle.size == Vec2(8, 8));
    // 값 4 = 비율 0.75 → 아래에서 8 + 60 = 68, 위에서 28이 손잡이 가운데다.
    CHECK(handle.render.Apply({4, 4}) == Vec2(6, 28));
}

} // namespace rocks::ui
