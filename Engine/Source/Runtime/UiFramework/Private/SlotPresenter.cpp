#include <UiFramework/SlotPresenter.h>

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

Vec2 SlotPlacement::PaddedSize(Vec2 desired) const noexcept {
    return desired + Vec2{padding.x + padding.z, padding.y + padding.w};
}

SlotPresenter::SlotPresenter(Allocator& allocator, const SlotPlacement& placement) noexcept
    : Element(allocator), placement_(placement) {
    SetVisibility(Visibility::SelfHitTestInvisible);
}
Result<void> SlotPresenter::TryAddChild(const SharedPtr<Element>& child) noexcept {
    return ChildCount() ? Result<void>::Failure(ErrorCode::CapacityExceeded)
                        : Element::TryAddChild(child);
}
Result<void> SlotPresenter::WrapChild(Allocator& allocator, Element& child,
                                      const SlotPlacement& placement) noexcept {
    auto presenter = SharedPtr<SlotPresenter>::TryMake(allocator, allocator, placement);
    return presenter ? WrapAttachedChild(child, presenter.Value())
                     : Result<void>::Failure(presenter.Error());
}

void SlotPresenter::SetPlacement(const SlotPlacement& placement) noexcept {
    if (placement.padding != placement_.padding) {
        placement_ = placement;
        InvalidateLayout();
    } else if (placement.horizontal != placement_.horizontal ||
               placement.vertical != placement_.vertical) {
        placement_ = placement;
        InvalidateArrangement();
    }
}

Vec2 SlotPresenter::ComputeDesiredSize(float) const noexcept {
    auto* child = ChildAt(0);
    if (!child || child->GetVisibility() == Visibility::Collapsed) {
        return {};
    }
    return placement_.PaddedSize(child->GetDesiredSize());
}
Result<void> SlotPresenter::Arrange(const Geometry& area) noexcept {
    SetGeometry(area);
    auto* child = ChildAt(0);
    if (!child || child->GetVisibility() == Visibility::Collapsed) {
        return Result<void>::Success();
    }
    return placement_.ArrangeChild(*child, area);
}

Result<void> SlotPlacement::ArrangeChild(Element& child, const Geometry& area) const noexcept {
    const auto p = padding;
    const Vec2 available{math::Max(0.0f, area.size.x - p.x - p.z),
                         math::Max(0.0f, area.size.y - p.y - p.w)};
    const auto natural = child.GetDesiredSize();
    const Vec2 extent{
        horizontal == StackAlignment::Fill ? available.x : math::Min(available.x, natural.x),
        vertical == StackAlignment::Fill ? available.y : math::Min(available.y, natural.y)};
    const Vec2 pos{p.x + Offset(horizontal, available.x - extent.x),
                   p.y + Offset(vertical, available.y - extent.y)};
    return child.ArrangeIfNeeded(area.MakeChild(extent, {1, 0, 0, 1, pos.x, pos.y}, {}, {}));
}
} // namespace rocks::ui
