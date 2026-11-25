#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/ActivityIndicator.h>
#include <UiFramework/ProgressBar.h>
#include <UiFramework/ScrollBar.h>
#include <UiFramework/Slider.h>

// 값 컨트롤의 UE 스타일 시험이다(Docs/Design/UiToolkit 9.1절 UW4b — StarshipCoreStyle.cpp "Slider"
// 589-609행, "ProgressBar" 561-569행, "Scrollbar" 186-195행). 상태별 brush·크기·원하는 크기를
// 단언한다.

namespace rocks::ui {

namespace {

LinearColor Theme(ThemeSlot slot) noexcept {
    return DefaultThemeColors().Get(slot);
}

} // namespace

TEST(UiFramework_SliderPaintsStyleStates) {
    SystemAllocator allocator;
    Slider slider(allocator);
    // 원하는 크기는 (16, max(막대 두께 4, 손잡이 높이 8)) = (16, 8)이다(UE ComputeDesiredSize).
    CHECK(slider.ComputeDesiredSize(1) == Vec2(16, 8));
    REQUIRE(slider.Arrange(Geometry::Root({100, 20}, {})));
    PaintList list(allocator);
    const auto paint = [&]() noexcept {
        list.Clear();
        return slider.OnPaint(list, 0);
    };
    // 보통: 막대 RB(Input, 2, Input, 1) 두께 4를 손잡이 폭 절반(4)씩 들여 그리고, 손잡이는 8x8
    // 원이다.
    REQUIRE(paint());
    REQUIRE(list.Boxes().Size() == 2);
    const PaintBox& bar = list.Boxes()[0];
    CHECK(bar.roundedBox && bar.color == Theme(ThemeSlot::Input));
    CHECK(bar.outlineColor == Theme(ThemeSlot::Input) && bar.outlineWidth == 1.0f);
    CHECK(bar.geometry.size == Vec2(92, 4));
    CHECK(bar.geometry.WindowBounds().Value().min == Vec2(4, 8));
    const PaintBox& thumb = list.Boxes()[1];
    CHECK(thumb.roundedBox && thumb.geometry.size == Vec2(8, 8));
    CHECK(thumb.cornerRadius == Vec4(4, 4, 4, 4));
    CHECK(thumb.color == Theme(ThemeSlot::Foreground));
    // 값 0, 들여쓰기: 손잡이 가운데 = 손잡이 폭 8(UE OnPaint 81-92행).
    CHECK(thumb.geometry.WindowBounds().Value().min == Vec2(4, 6));
    // 올림: 손잡이 ForegroundHover. 다시 그리기를 요청한다.
    slider.OnMouseEnter(0);
    REQUIRE(paint());
    CHECK(list.Boxes()[1].color == Theme(ThemeSlot::ForegroundHover));
    slider.OnMouseLeave(0);
    REQUIRE(paint());
    CHECK(list.Boxes()[1].color == Theme(ThemeSlot::Foreground));
    // 비활성·잠금: 막대 외곽선 Recessed(UE GetBarImage 485행 — 잠금도 비활성 모양).
    slider.SetEnabled(false);
    REQUIRE(paint());
    CHECK(list.Boxes()[0].outlineColor == Theme(ThemeSlot::Recessed));
    slider.SetEnabled(true);
    slider.SetLocked(true);
    REQUIRE(paint());
    CHECK(list.Boxes()[0].outlineColor == Theme(ThemeSlot::Recessed));
    slider.SetLocked(false);
    // 들여쓰기 없음: 값 0의 손잡이 가운데 = 손잡이 폭 절반(4) — 경계 안에 머문다.
    slider.SetIndentHandle(false);
    REQUIRE(paint());
    CHECK(list.Boxes()[1].geometry.WindowBounds().Value().min == Vec2(0, 6));
    // 잘못된 스타일은 거부하고 값을 바꾸지 않는다.
    SliderStyle bad = DefaultSliderStyle();
    bad.barThickness = -1;
    CHECK(!slider.SetStyle(bad));
    bad = DefaultSliderStyle();
    bad.thumbSize = {-1, 8};
    CHECK(!slider.SetStyle(bad));
    CHECK(slider.GetStyle() == DefaultSliderStyle());
}

TEST(UiFramework_SliderPointerMatchesPaintedThumb) {
    SystemAllocator allocator;
    Slider slider(allocator);
    REQUIRE(slider.Arrange(Geometry::Root({100, 20}, {})));
    // 들여쓰기: 값 범위 = 8 .. 92(UE PositionToValue 439-445행). 가운데(50)는 0.5다.
    CHECK(slider.OnPointerEvent({PointerEventType::Down, {50, 10}}).capture);
    CHECK(math::Abs(slider.GetValue() - 0.5f) < 0.0001f);
    PaintList list(allocator);
    REQUIRE(slider.OnPaint(list, 0));
    // 끄는 동안은 올림 모양이고, 놓으면 다시 그려 보통 모양으로 돌아간다.
    CHECK(list.Boxes()[1].color == Theme(ThemeSlot::ForegroundHover));
    CHECK(list.Boxes()[1].geometry.WindowBounds().Value().min == Vec2(46, 6));
    CHECK(slider.OnPointerEvent({PointerEventType::Up, {8, 10}}).releaseCapture);
    CHECK(slider.GetValue() == 0);
    list.Clear();
    REQUIRE(slider.OnPaint(list, 0));
    CHECK(list.Boxes()[1].color == Theme(ThemeSlot::Foreground));
}

TEST(UiFramework_ScrollBarPaintsStyleStatesAndThickness) {
    SystemAllocator allocator;
    ScrollBar bar(allocator);
    // 두께는 스타일 두께 8, 길이 기본값은 두께의 ScrollBarLengthFactor배다.
    CHECK(bar.ComputeDesiredSize(1) == Vec2(8, 8 * ScrollBarLengthFactor));
    bar.SetHorizontal(true);
    CHECK(bar.ComputeDesiredSize(1) == Vec2(8 * ScrollBarLengthFactor, 8));
    ScrollBarStyle thick = DefaultScrollBarStyle();
    thick.thickness = 16;
    REQUIRE(bar.SetStyle(thick));
    CHECK(bar.ComputeDesiredSize(1) == Vec2(16 * ScrollBarLengthFactor, 16));
    REQUIRE(bar.SetStyle(DefaultScrollBarStyle()));
    bar.SetHorizontal(false);
    REQUIRE(bar.SetState(0, 0.25f));
    REQUIRE(bar.Arrange(Geometry::Root({8, 200}, {})));
    PaintList list(allocator);
    const auto paint = [&]() noexcept {
        list.Clear();
        return bar.OnPaint(list, 0);
    };
    // 트랙은 None이라 손잡이 하나만 그린다: RB(Hover, 4).
    REQUIRE(paint());
    REQUIRE(list.Boxes().Size() == 1);
    CHECK(list.Boxes()[0].roundedBox && list.Boxes()[0].color == Theme(ThemeSlot::Hover));
    CHECK(list.Boxes()[0].cornerRadius == Vec4(4, 4, 4, 4));
    // 올림·끌기: RB(Hover2, 4). 끌기를 놓으면 다시 보통이다.
    bar.OnMouseEnter(0);
    REQUIRE(paint());
    CHECK(list.Boxes()[0].color == Theme(ThemeSlot::Hover2));
    bar.OnMouseLeave(0);
    CHECK(bar.OnPointerEvent({PointerEventType::Down, {4, 10}}).capture);
    REQUIRE(paint());
    CHECK(list.Boxes()[0].color == Theme(ThemeSlot::Hover2));
    CHECK(bar.OnPointerEvent({PointerEventType::Up, {4, 10}}).releaseCapture);
    REQUIRE(paint());
    CHECK(list.Boxes()[0].color == Theme(ThemeSlot::Hover));
    // 손잡이 최소 길이는 UE MinThumbSize 35다.
    REQUIRE(bar.SetState(0, 0.01f));
    REQUIRE(paint());
    CHECK(list.Boxes()[0].geometry.size.y == MinScrollThumbLength);
}

TEST(UiFramework_ProgressBarPaintsStyleAndFillOverride) {
    SystemAllocator allocator;
    ProgressBar bar(allocator);
    REQUIRE(bar.SetValue(0.5f));
    REQUIRE(bar.Arrange(Geometry::Root({100, 10}, {})));
    PaintList list(allocator);
    const auto paint = [&]() noexcept {
        list.Clear();
        return bar.OnPaint(list, 0);
    };
    // 바탕 Color(Foldout), 채움 Color(PrimaryHover)이고 채움 색 기본 흰색은 스타일 색 그대로다.
    REQUIRE(paint());
    REQUIRE(list.Boxes().Size() == 2);
    CHECK(list.Boxes()[0].color == Theme(ThemeSlot::Foldout));
    CHECK(list.Boxes()[1].color == Theme(ThemeSlot::PrimaryHover));
    CHECK(list.Boxes()[1].geometry.size == Vec2(50, 10));
    // 채움 brush를 덮으면 그것을 쓰고, ClearFillBrush로 스타일 채움에 돌아간다.
    Brush red;
    red.mode = BrushMode::Box;
    red.tint = {1, 0, 0, 1};
    REQUIRE(bar.SetFillBrush(red));
    REQUIRE(paint());
    CHECK(list.Boxes()[1].color == LinearColor(1, 0, 0, 1));
    // marquee는 덮은 채움이 아니라 스타일 marquee다(UE FProgressBarStyle.MarqueeImage).
    bar.SetMarquee(true);
    REQUIRE(paint());
    CHECK(list.Boxes()[1].color == Theme(ThemeSlot::PrimaryHover));
    bar.SetMarquee(false);
    bar.ClearFillBrush();
    REQUIRE(paint());
    CHECK(list.Boxes()[1].color == Theme(ThemeSlot::PrimaryHover));
    // 채움 색은 채움 brush에 곱한다(UE FillColorAndOpacity).
    bar.SetFillColor({0.5f, 0.5f, 0.5f, 1});
    REQUIRE(paint());
    CHECK(list.Boxes()[1].color ==
          Theme(ThemeSlot::PrimaryHover) * LinearColor(0.5f, 0.5f, 0.5f, 1));
}

TEST(UiFramework_ThrobberDefaultPieceIsCircle) {
    SystemAllocator allocator;
    ActivityIndicator widget(allocator, true);
    widget.SetAnimating(false);
    REQUIRE(widget.Arrange(Geometry::Root({32, 32}, {})));
    PaintList list(allocator);
    REQUIRE(widget.Paint(list, 0));
    REQUIRE(!list.Boxes().IsEmpty());
    // 조각은 8x8 원(높이 절반 반경)이다(UE "Throbber.CircleChunk" 8x8).
    CHECK(list.Boxes()[0].roundedBox);
    CHECK(list.Boxes()[0].cornerRadius.x == list.Boxes()[0].roundedSize.y * 0.5f);
}

} // namespace rocks::ui
