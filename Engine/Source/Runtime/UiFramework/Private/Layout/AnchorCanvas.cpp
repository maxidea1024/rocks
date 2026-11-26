#include <UiFramework/Layout/AnchorCanvas.h>

namespace rocks::ui {

bool CanvasSlot::IsValid() const noexcept {
    return anchorMin.IsFinite() && anchorMax.IsFinite() && offset.IsFinite() &&
           alignment.IsFinite() && math::IsFinite(zOrder) && anchorMin.x <= anchorMax.x &&
           anchorMin.y <= anchorMax.y;
}

Result<void> AnchorCanvas::Construct(const Args& args) noexcept {
    for (const auto& slot : args.slots) {
        if (slot.content.error != ErrorCode::None) {
            return Result<void>::Failure(slot.content.error);
        }

        auto added = TryAddSlot(slot.content.element, slot.layout);
        if (!added) {
            return added;
        }
    }

    return Result<void>::Success();
}

AnchorCanvas::AnchorCanvas(Allocator& allocator) noexcept : Element(allocator), slots_(allocator) {
}

Result<void> AnchorCanvas::TryAddChild(const SharedPtr<Element>& child) noexcept {
    return TryAddSlot(child, {});
}

bool AnchorCanvas::StartsPaintLayer(usize previous, usize next) const noexcept {
    return slots_[previous].zOrder != slots_[next].zOrder;
}

bool AnchorCanvas::RemoveChild(Element& child) noexcept {
    for (usize i = 0; i < ChildCount(); ++i) {
        if (ChildAt(i) == &child) {
            (void)Element::RemoveChild(child);
            (void)slots_.TryRemoveAt(i);
            return true;
        }
    }

    return false;
}

Result<void> AnchorCanvas::TryAddSlot(const SharedPtr<Element>& child,
                                      const CanvasSlot& slot) noexcept {
    if (!slot.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    usize index = 0;
    while (index < slots_.Size() && slots_[index].zOrder <= slot.zOrder) {
        ++index;
    }

    auto addedSlot = slots_.TryInsert(slot, index);
    if (!addedSlot) {
        return addedSlot;
    }

    auto addedChild = TryInsertChild(index, child);
    if (!addedChild) {
        (void)slots_.TryRemoveAt(index);
        return addedChild;
    }

    return Result<void>::Success();
}

Result<void> AnchorCanvas::SetSlot(Element& child, const CanvasSlot& slot) noexcept {
    if (!slot.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    usize index = 0;
    while (index < ChildCount() && ChildAt(index) != &child) {
        ++index;
    }

    if (index == ChildCount()) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (slots_[index] == slot) {
        return Result<void>::Success();
    }

    usize destination = index;
    if (slots_[index].zOrder != slot.zOrder) {
        while (destination + 1 < slots_.Size() && slots_[destination + 1].zOrder <= slot.zOrder) {
            slots_[destination] = slots_[destination + 1];
            ++destination;
        }
        while (destination > 0 && slots_[destination - 1].zOrder > slot.zOrder) {
            slots_[destination] = slots_[destination - 1];
            --destination;
        }

        MoveChildWithin(index, destination);
    }

    slots_[destination] = slot;
    InvalidateLayout();
    return Result<void>::Success();
}

Result<void> AnchorCanvas::Arrange(const Geometry& geometry) noexcept {
    if (!geometry.size.IsFinite() || geometry.size.x < 0 || geometry.size.y < 0 ||
        slots_.Size() != ChildCount()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    SetGeometry(geometry);
    for (usize i = 0; i < slots_.Size(); ++i) {
        Element* child = ChildAt(i);
        if (child->GetVisibility() == Visibility::Collapsed) {
            continue;
        }

        const CanvasSlot& slot = slots_[i];
        const Vec2 anchorStart = slot.anchorMin * geometry.size;
        const Vec2 anchorEnd = slot.anchorMax * geometry.size;
        const Vec2 size =
            slot.autoSize ? child->GetDesiredSize() : Vec2(slot.offset.z, slot.offset.w);
        Vec2 position;
        Vec2 extent;
        position.x = anchorStart.x + slot.offset.x;
        position.y = anchorStart.y + slot.offset.y;
        if (slot.anchorMin.x != slot.anchorMax.x) {
            extent.x = math::Max(0.0f, anchorEnd.x - position.x - slot.offset.z);
        } else {
            extent.x = math::Max(0.0f, size.x);
            position.x -= size.x * slot.alignment.x;
        }

        if (slot.anchorMin.y != slot.anchorMax.y) {
            extent.y = math::Max(0.0f, anchorEnd.y - position.y - slot.offset.w);
        } else {
            extent.y = math::Max(0.0f, size.y);
            position.y -= size.y * slot.alignment.y;
        }

        if (!position.IsFinite() || !extent.IsFinite()) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        const AffineTransform2D offset{1, 0, 0, 1, position.x, position.y};
        auto result = child->ArrangeIfNeeded(GetGeometry().MakeChild(extent, offset, {}, {}));
        if (!result) {
            return result;
        }
    }

    return Result<void>::Success();
}

Vec2 AnchorCanvas::ComputeDesiredSize(float) const noexcept {
    Vec2 desired;
    for (usize i = 0; i < ChildCount(); ++i) {
        const Element* child = ChildAt(i);
        if (child->GetVisibility() != Visibility::Collapsed) {
            const CanvasSlot& slot = slots_[i];
            const Vec2 size =
                slot.autoSize ? child->GetDesiredSize() : Vec2(slot.offset.z, slot.offset.w);
            const bool dockX = slot.anchorMin.x == slot.anchorMax.x &&
                               (slot.anchorMin.x == 0 || slot.anchorMin.x == 1);
            const bool dockY = slot.anchorMin.y == slot.anchorMax.y &&
                               (slot.anchorMin.y == 0 || slot.anchorMin.y == 1);
            desired.x = math::Max(desired.x, size.x + (dockX ? math::Abs(slot.offset.x) : 0.0f));
            desired.y = math::Max(desired.y, size.y + (dockY ? math::Abs(slot.offset.y) : 0.0f));
        }
    }

    return desired;
}

} // namespace rocks::ui
