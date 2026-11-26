#include <UiFramework/Layout/Switcher.h>

namespace rocks::ui {

Switcher::Switcher(Allocator& allocator) noexcept : Element(allocator) {
}

Result<void> Switcher::SetActiveIndex(usize index) noexcept {
    if (index >= ChildCount()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (activeIndex_ != index) {
        activeIndex_ = index;
        InvalidateLayout();
    }

    return Result<void>::Success();
}

Element* Switcher::ActiveChild() const noexcept {
    return ChildAt(activeIndex_);
}

bool Switcher::RemoveChild(Element& child) noexcept {
    for (usize i = 0; i < ChildCount(); ++i) {
        if (ChildAt(i) == &child) {
            if (activeIndex_ > 0 && i <= activeIndex_) {
                --activeIndex_;
            }

            return Element::RemoveChild(child);
        }
    }

    return false;
}

Span<const SharedPtr<Element>> Switcher::ActiveChildren() const noexcept {
    const auto children = Element::ActiveChildren();
    return activeIndex_ < children.Size() ? children.Slice(activeIndex_, 1)
                                          : Span<const SharedPtr<Element>>{};
}

Vec2 Switcher::ComputeDesiredSize(float) const noexcept {
    const Element* child = ActiveChild();
    return child ? child->GetDesiredSize() : Vec2{};
}

Result<void> Switcher::Arrange(const Geometry& geometry) noexcept {
    SetGeometry(geometry);
    Element* child = ActiveChild();
    if (child && child->GetVisibility() != Visibility::Collapsed) {
        return child->ArrangeIfNeeded(GetGeometry());
    }

    return Result<void>::Success();
}

} // namespace rocks::ui
