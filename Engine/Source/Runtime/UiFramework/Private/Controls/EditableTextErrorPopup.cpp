#include "EditableTextErrorPopup.h"

#include <UiCore/PaintList.h>

namespace rocks::ui {

EditableTextErrorPopup::EditableTextErrorPopup(Allocator& allocator, const StyleBrush& background,
                                               Vec4 padding) noexcept
    : Element(allocator), background_(background), padding_(padding) {
}

Vec2 EditableTextErrorPopup::ComputeDesiredSize(float) const noexcept {
    const Vec2 inset{padding_.x + padding_.z, padding_.y + padding_.w};
    const Element* content = ChildCount() > 0 ? ChildAt(0) : nullptr;
    return content ? content->GetDesiredSize() + inset : inset;
}

Result<void> EditableTextErrorPopup::Arrange(const Geometry& geometry) noexcept {
    SetGeometry(geometry);
    Element* content = ChildCount() > 0 ? ChildAt(0) : nullptr;
    if (!content) {
        return Result<void>::Success();
    }

    const Vec2 size{math::Max(0.0f, geometry.size.x - padding_.x - padding_.z),
                    math::Max(0.0f, geometry.size.y - padding_.y - padding_.w)};
    return content->ArrangeIfNeeded(
        GetGeometry().MakeChild(size, {1, 0, 0, 1, padding_.x, padding_.y}, {}, {}));
}

Result<void> EditableTextErrorPopup::OnPaint(PaintList& list, int32 layer) const noexcept {
    return list.TryAddBrush(GetGeometry(), background_.Resolve(list.Theme(), list.ColorContext()),
                            LinearColor{1, 1, 1, 1}, layer);
}

} // namespace rocks::ui
