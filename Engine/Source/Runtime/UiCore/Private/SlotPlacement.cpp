#include <UiCore/Element.h>
#include <UiCore/SlotPlacement.h>

namespace rocks::ui {

namespace {

float Offset(StackAlignment alignment, float remaining) noexcept {
    if (alignment == StackAlignment::End) {
        return remaining;
    }

    return alignment == StackAlignment::Center ? remaining * 0.5f : 0;
}

} // namespace

bool SlotPlacement::IsValid() const noexcept {
    return padding.IsFinite() && padding.x >= 0 && padding.y >= 0 && padding.z >= 0 &&
           padding.w >= 0 && horizontal <= StackAlignment::End && vertical <= StackAlignment::End;
}

bool SlotPlacement::IsIdentity() const noexcept {
    return padding == Vec4{} && horizontal == StackAlignment::Fill &&
           vertical == StackAlignment::Fill;
}

Vec2 SlotPlacement::PaddedSize(Vec2 desired) const noexcept {
    return desired + Vec2{padding.x + padding.z, padding.y + padding.w};
}

Geometry SlotPlacement::PlaceChild(Vec2 natural, const Geometry& area) const noexcept {
    const auto p = padding;
    const Vec2 available{math::Max(0.0f, area.size.x - p.x - p.z),
                         math::Max(0.0f, area.size.y - p.y - p.w)};
    const Vec2 extent{
        horizontal == StackAlignment::Fill ? available.x : math::Min(available.x, natural.x),
        vertical == StackAlignment::Fill ? available.y : math::Min(available.y, natural.y)};
    const Vec2 pos{p.x + Offset(horizontal, available.x - extent.x),
                   p.y + Offset(vertical, available.y - extent.y)};
    return area.MakeChild(extent, {1, 0, 0, 1, pos.x, pos.y}, {}, {});
}

Result<void> SlotPlacement::ArrangeChild(Element& child, const Geometry& area) const noexcept {
    return child.ArrangeIfNeeded(PlaceChild(child.GetDesiredSize(), area));
}

} // namespace rocks::ui
