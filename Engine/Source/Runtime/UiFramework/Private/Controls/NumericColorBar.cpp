#include "NumericColorBar.h"

#include <UiCore/PaintList.h>
#include <UiCore/Styles/StyleBrush.h>

namespace rocks::ui {

namespace {

// 막대 폭(UE NarrowDecorator 2×16의 폭)과 반경이다.
constexpr float ColorBarWidth = 2.0f;
constexpr float ColorBarRadius = 1.0f;

} // namespace

NumericColorBar::NumericColorBar(Allocator& allocator, LinearColor color) noexcept
    : Element(allocator), color_(color) {
    SetVisibility(Visibility::HitTestInvisible);
}

Vec2 NumericColorBar::ComputeDesiredSize(float) const noexcept {
    return {ColorBarWidth, 0.0f};
}

Result<void> NumericColorBar::OnPaint(PaintList& list, int32 layer) const noexcept {
    const Geometry& geometry = GetGeometry();
    const auto bar = geometry.MakeChild({ColorBarWidth, geometry.size.y}, {}, {}, {});
    const StyleBrush brush =
        StyleBrush::RoundedBox(color_, ColorBarRadius, LinearColor{0, 0, 0, 0}, 0);
    return list.TryAddBrush(bar, brush.Resolve(list.Theme(), list.ColorContext()), {1, 1, 1, 1},
                            layer);
}

LinearColor NumericColorBar::GetBarColor() const noexcept {
    return color_;
}

} // namespace rocks::ui
