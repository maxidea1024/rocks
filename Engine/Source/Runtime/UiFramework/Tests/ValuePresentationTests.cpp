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
    // 잠긴 슬라이더는 값을 바꾸지 않고, 방향키는 기본 처리로 navigation이 된다(UE SSlider
    // — 20.7절).
    const KeyReply locked = slider.SendKey(0, input::InputEvent::KeyDown(input::Key::Right));
    CHECK(locked.Navigation().HasValue());
    CHECK(slider.GetNormalizedValue() == 0.25f);
    REQUIRE(slider.SetValue(0.5f));
    CHECK(slider.OnPointerEvent({PointerEventType::Up, {90, 6}}).releaseCapture);
    CHECK(slider.GetValue() == 0.5f && starts == 1 && ends == 1);
    slider.OnPointerCaptureLost(PointerSource::Mouse, 0, 0);
    CHECK(ends == 1);
    using B = input::GamepadButton;
    slider.SetLocked(false);
    REQUIRE(slider.SetStep(0.25f));
    // 패드 버튼은 키 경로로 온다(20.8절). 슬라이더가 스스로 처리했는지(navigation 아님)를 본다.
    const auto consumed = [&slider](uint32 user, B button) noexcept {
        const KeyReply reply = slider.SendGamepadButton(user, button);
        return reply.IsHandled() && !reply.Navigation();
    };
    CHECK(!consumed(2, B::DPadRight));
    CHECK(consumed(2, B::South));
    CHECK(!consumed(3, B::DPadRight));
    CHECK(consumed(2, B::DPadRight) && slider.GetValue() == 0.75f);
    slider.OnFocusLost(2);
    CHECK(!consumed(2, B::DPadRight));
    slider.SetRequiresControllerLock(false);
    CHECK(consumed(2, B::DPadLeft) && slider.GetValue() == 0.5f);
}

} // namespace rocks::ui
