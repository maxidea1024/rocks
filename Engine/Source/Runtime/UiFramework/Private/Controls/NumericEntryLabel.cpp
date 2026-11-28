#include "NumericEntryLabel.h"

#include <UiCore/PaintList.h>
#include <UiCore/Styles/StyleBrush.h>

namespace rocks::ui {

namespace {

// 글자 둘레 여백(UE BuildLabel Padding (1, 0, 6, 0))과 바탕 반경이다.
constexpr Vec4 LabelTextPadding{1.0f, 0.0f, 6.0f, 0.0f};
constexpr float LabelCornerRadius = 3.0f;

} // namespace

NumericEntryLabel::NumericEntryLabel(Allocator& allocator, ColorRef background) noexcept
    : Element(allocator), background_(background) {
    SetVisibility(Visibility::HitTestInvisible);
}

Vec2 NumericEntryLabel::ComputeDesiredSize(float) const noexcept {
    const Element* text = ChildAt(0);
    const Vec2 inner = text ? text->GetDesiredSize() : Vec2{};
    return {inner.x + LabelTextPadding.x + LabelTextPadding.z,
            inner.y + LabelTextPadding.y + LabelTextPadding.w};
}

Result<void> NumericEntryLabel::Arrange(const Geometry& geometry) noexcept {
    SetGeometry(geometry);
    Element* text = ChildAt(0);
    if (!text) {
        return Result<void>::Success();
    }

    // 글자는 세로 가운데다(UE BuildLabel VAlign_Center).
    const Vec2 inner = text->GetDesiredSize();
    const float top = (geometry.size.y - inner.y) * 0.5f;
    return text->ArrangeIfNeeded(
        GetGeometry().MakeChild(inner, {1, 0, 0, 1, LabelTextPadding.x, top}, {}, {}));
}

Result<void> NumericEntryLabel::OnPaint(PaintList& list, int32 layer) const noexcept {
    const StyleColorContext context = list.ColorContext();
    StyleBrush brush = StyleBrush::RoundedBox(LinearColor{1, 1, 1, 1}, LabelCornerRadius,
                                              LinearColor{0, 0, 0, 0}, 0);
    brush.fill = background_;
    return list.TryAddBrush(GetGeometry(), brush.Resolve(list.Theme(), context), {1, 1, 1, 1},
                            layer);
}

} // namespace rocks::ui
