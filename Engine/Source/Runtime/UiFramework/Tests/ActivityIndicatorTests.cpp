#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Controls/ActivityIndicator.h>

namespace rocks::ui {

namespace {

// 활동 표시는 프레임 시각만 본다(Docs/Design/UiToolkit 20.3절). 시험은 host 묶음(그리기 밖 읽기)과
// 그리기 목록(OnPaint)에 같은 시각을 준다.
struct IndicatorClock {
    SharedPtr<UiHostServices> host;
    PaintList* list = nullptr;

    void At(double seconds) const noexcept {
        host->SetFrameTime({seconds, 0});
        list->SetFrameTime({seconds, 0});
    }
};

} // namespace

TEST(UiFramework_ThrobberUsesPieceBrushAndIndependentAxes) {
    SystemAllocator allocator;
    ActivityIndicator widget(allocator, false);
    PaintList list(allocator);
    auto host = SharedPtr<UiHostServices>::TryMake(allocator);
    REQUIRE(host);
    REQUIRE(widget.SetUiHostServices(host.Value()));
    const IndicatorClock clock{host.Value(), &list};
    clock.At(0);
    Brush brush;
    brush.imageSize = {9, 6};
    brush.tint = {0.5f, 1, 1, 0.8f};
    brush.resource = {2, 1};
    REQUIRE(widget.SetPieceBrush(brush));
    REQUIRE(widget.SetPieceCount(2));
    CHECK(widget.ComputeDesiredSize(1) == Vec2(18, 6));
    REQUIRE(widget.Arrange(Geometry::Root({18, 6}, {})));
    widget.SetAnimationAxes(false, false, false);
    CHECK(!widget.IsVolatile());
    REQUIRE(widget.Paint(list, 0));
    REQUIRE(list.Boxes().Size() == 2);
    CHECK(list.Boxes()[0].resource == brush.resource);
    CHECK(list.Boxes()[0].color == brush.tint);
    CHECK(list.Boxes()[0].geometry.render.m00 == 1);
    CHECK(list.Boxes()[0].geometry.render.m11 == 1);
    widget.SetAnimationAxes(true, false, true);
    CHECK(widget.IsVolatile());
    clock.At(0.125);
    list.Clear();
    REQUIRE(widget.Paint(list, 0));
    CHECK(math::Abs(list.Boxes()[0].geometry.render.m00 - 0.5f) < 0.001f);
    CHECK(list.Boxes()[0].geometry.render.m11 == 1);
    CHECK(math::Abs(list.Boxes()[0].color.a - 0.4f) < 0.001f);
    widget.SetAnimating(false);
    CHECK(!widget.IsVolatile());
    const PaintBox frozen = list.Boxes()[0];
    clock.At(0.999999999);
    list.Clear();
    REQUIRE(widget.Paint(list, 0));
    CHECK(list.Boxes()[0] == frozen);
}

TEST(UiFramework_CircularThrobberUsesRadiusPeriodAndProgressivePieceSize) {
    SystemAllocator allocator;
    ActivityIndicator ring(allocator, true);
    PaintList list(allocator);
    auto host = SharedPtr<UiHostServices>::TryMake(allocator);
    REQUIRE(host);
    REQUIRE(ring.SetUiHostServices(host.Value()));
    const IndicatorClock clock{host.Value(), &list};
    clock.At(0);
    REQUIRE(ring.SetRadius(20));
    REQUIRE(ring.SetPeriod(2));
    REQUIRE(ring.SetPieceCount(4));
    CHECK(!ring.SetPeriod(0));
    CHECK(ring.ComputeDesiredSize(1) == Vec2(40, 40));
    REQUIRE(ring.Arrange(Geometry::Root({40, 40}, {})));
    REQUIRE(ring.Paint(list, 0));
    REQUIRE(list.Boxes().Size() == 4);
    auto first = list.Boxes()[0].geometry.render;
    CHECK(first.m00 == 0.25f);
    CHECK(list.Boxes()[3].geometry.render.m00 == 1);
    clock.At(0.5);
    list.Clear();
    REQUIRE(ring.Paint(list, 0));
    auto quarter = list.Boxes()[0].geometry.render;
    CHECK(math::Abs(quarter.tx - first.tx - 16) < 0.01f);
    CHECK(math::Abs(quarter.ty - first.ty + 16) < 0.01f);
}

} // namespace rocks::ui
