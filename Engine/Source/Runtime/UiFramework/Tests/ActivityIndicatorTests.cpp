#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/ActivityIndicator.h>

namespace rocks::ui {

namespace {

thread_local uint64 indicatorTime = 0;
uint64 ReadIndicatorTime() noexcept {
    return indicatorTime;
}
} // namespace

TEST(UiFramework_ThrobberUsesPieceBrushAndIndependentAxes) {
    SystemAllocator allocator;
    indicatorTime = 0;
    ActivityIndicator widget(allocator, false, ReadIndicatorTime);
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
    PaintList list(allocator);
    REQUIRE(widget.Paint(list, 0));
    REQUIRE(list.Boxes().Size() == 2);
    CHECK(list.Boxes()[0].resource == brush.resource);
    CHECK(list.Boxes()[0].color == brush.tint);
    CHECK(list.Boxes()[0].geometry.render.m00 == 1);
    CHECK(list.Boxes()[0].geometry.render.m11 == 1);
    widget.SetAnimationAxes(true, false, true);
    CHECK(widget.IsVolatile());
    indicatorTime = 125000000;
    list.Clear();
    REQUIRE(widget.Paint(list, 0));
    CHECK(math::Abs(list.Boxes()[0].geometry.render.m00 - 0.5f) < 0.001f);
    CHECK(list.Boxes()[0].geometry.render.m11 == 1);
    CHECK(math::Abs(list.Boxes()[0].color.a - 0.4f) < 0.001f);
    widget.SetAnimating(false);
    CHECK(!widget.IsVolatile());
    const PaintBox frozen = list.Boxes()[0];
    indicatorTime = 999999999;
    list.Clear();
    REQUIRE(widget.Paint(list, 0));
    CHECK(list.Boxes()[0] == frozen);
}

TEST(UiFramework_CircularThrobberUsesRadiusPeriodAndProgressivePieceSize) {
    SystemAllocator allocator;
    indicatorTime = 0;
    ActivityIndicator ring(allocator, true, ReadIndicatorTime);
    REQUIRE(ring.SetRadius(20));
    REQUIRE(ring.SetPeriod(2));
    REQUIRE(ring.SetPieceCount(4));
    CHECK(!ring.SetPeriod(0));
    CHECK(ring.ComputeDesiredSize(1) == Vec2(40, 40));
    REQUIRE(ring.Arrange(Geometry::Root({40, 40}, {})));
    PaintList list(allocator);
    REQUIRE(ring.Paint(list, 0));
    REQUIRE(list.Boxes().Size() == 4);
    auto first = list.Boxes()[0].geometry.render;
    CHECK(first.m00 == 0.25f);
    CHECK(list.Boxes()[3].geometry.render.m00 == 1);
    indicatorTime = 500000000;
    list.Clear();
    REQUIRE(ring.Paint(list, 0));
    auto quarter = list.Boxes()[0].geometry.render;
    CHECK(math::Abs(quarter.tx - first.tx - 16) < 0.01f);
    CHECK(math::Abs(quarter.ty - first.ty + 16) < 0.01f);
}

} // namespace rocks::ui
