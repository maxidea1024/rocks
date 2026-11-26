#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Controls/Button.h>

namespace rocks::ui {

TEST(UiFramework_ButtonBalancesPressReleaseAndHonorsInputMethods) {
    SystemAllocator allocator;
    Button button(allocator);
    REQUIRE(button.Arrange(Geometry::Root({40, 24}, {})));
    uint32 presses = 0, releases = 0, clicks = 0;
    button.SetOnPressed(Delegate<void()>::CreateLambda([&]() noexcept { ++presses; }));
    button.SetOnReleased(Delegate<void()>::CreateLambda([&]() noexcept { ++releases; }));
    button.SetOnClicked(Delegate<void()>::CreateLambda([&]() noexcept { ++clicks; }));
    REQUIRE(button.SetInputMethods(ButtonClickMethod::Down, ButtonClickMethod::Precise,
                                   ButtonPressMethod::Down));
    REQUIRE(button.OnPointerEvent({PointerEventType::Down, {5, 5}}).handled);
    CHECK(presses == 1 && clicks == 1);
    button.OnPointerCaptureLost(PointerSource::Mouse, 0, 0);
    CHECK(releases == 1 && !button.IsPressed());
    button.OnPointerCaptureLost(PointerSource::Mouse, 0, 0);
    CHECK(releases == 1);
    REQUIRE(button.OnPointerEvent({PointerEventType::Down, {5, 5}, 1, 0, PointerSource::Touch})
                .handled);
    REQUIRE(button.OnPointerEvent({PointerEventType::Move, {20, 5}, 1, 0, PointerSource::Touch})
                .releaseCapture);
    CHECK(presses == 2 && releases == 2 && clicks == 1);
    REQUIRE(button.OnKeyEvent(0, input::InputEvent::KeyDown(input::Key::Space)));
    CHECK(clicks == 2 && presses == 3);
    button.SetFocusable(false);
    CHECK(releases == 3 && !button.SupportsKeyboardFocus());
    CHECK(!button.OnKeyEvent(0, input::InputEvent::KeyUp(input::Key::Space)));
    REQUIRE(button.SetInputMethods(ButtonClickMethod::Up, ButtonClickMethod::DownAndUp,
                                   ButtonPressMethod::Up));
    REQUIRE(button.OnPointerEvent({PointerEventType::Up, {5, 5}}).handled);
    CHECK(clicks == 3);
}

TEST(UiFramework_ButtonPaintsFirstHoverAndLastLeaveAcrossUsers) {
    SystemAllocator allocator;
    Button button(allocator);
    REQUIRE(button.Arrange(Geometry::Root({16, 16}, {})));
    uint32 enters = 0, leaves = 0;
    button.SetOnHovered(Delegate<void()>::CreateLambda([&]() noexcept { ++enters; }));
    button.SetOnUnhovered(Delegate<void()>::CreateLambda([&]() noexcept { ++leaves; }));
    button.OnMouseEnter(0);
    button.OnMouseEnter(1);
    button.OnMouseLeave(0);
    CHECK(button.IsHovered() && enters == 1 && leaves == 0);
    PaintList list(allocator);
    REQUIRE(button.OnPaint(list, 0));
    REQUIRE(list.Boxes().Size() == 1);
    // 올림 상태는 기본 스타일의 올림 brush(RB(Hover, 4, Input, 1))다.
    CHECK(list.Boxes()[0].color == DefaultThemeColors().Get(ThemeSlot::Hover));
    button.OnMouseLeave(1);
    CHECK(!button.IsHovered() && leaves == 1);
}

} // namespace rocks::ui
