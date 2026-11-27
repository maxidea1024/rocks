#include <UiCore/SlotPlacement.h>
#include <UiFramework/Layout/StackPanel.h>

namespace rocks::ui {

Result<void> StackPanel::Construct(const Args& args) noexcept {
    SetFlowDirection(args.flow);
    SetAllowShrink(args.allowShrink);
    for (const auto& slot : args.slots) {
        if (slot.content.error != ErrorCode::None) {
            return Result<void>::Failure(slot.content.error);
        }

        StackSlot layout = slot.size;
        layout.before += slot.padding;
        layout.after += slot.padding;
        layout.crossBefore += slot.padding;
        layout.crossAfter += slot.padding;
        layout.alignment = slot.align;
        auto added = TryAddSlot(slot.content.element, layout);
        if (!added) {
            return added;
        }
    }

    return Result<void>::Success();
}

StackPanel::StackPanel(Allocator& allocator, StackDirection direction) noexcept
    : Element(allocator), allocator_(allocator), slots_(allocator), direction_(direction) {
}

Result<void> StackPanel::TryAddChild(const SharedPtr<Element>& child) noexcept {
    return TryAddSlot(child, {});
}

bool StackPanel::RemoveChild(Element& child) noexcept {
    return RemoveSlot(child);
}

Result<void> StackPanel::TryAddSlot(const SharedPtr<Element>& child,
                                    const StackSlot& slot) noexcept {
    StackPlacement placement;
    auto valid =
        ArrangeStack(Span<const StackSlot>(&slot, 1), Span<StackPlacement>(&placement, 1), 0);
    if (!valid) {
        return valid;
    }

    auto addedSlot = slots_.TryPushBack(slot);
    if (!addedSlot) {
        return addedSlot;
    }

    auto addedChild = Element::TryAddChild(child);
    if (!addedChild) {
        (void)slots_.TryRemoveAt(slots_.Size() - 1);
        return addedChild;
    }

    return Result<void>::Success();
}

Result<void> StackPanel::SetSlotLayout(const Element& child, const StackSlot& slot) noexcept {
    StackPlacement placement;
    auto valid = ArrangeStack({&slot, 1}, {&placement, 1}, 0);
    if (!valid || slot.alignment > StackAlignment::End) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize index = 0; index < slots_.Size(); ++index) {
        if (ChildAt(index) != &child) {
            continue;
        }

        if (slots_[index] != slot) {
            slots_[index] = slot;
            InvalidateLayout();
        }

        return Result<void>::Success();
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

bool StackPanel::RemoveSlot(Element& child) noexcept {
    for (usize i = 0; i < ChildCount(); ++i) {
        if (ChildAt(i) == &child) {
            (void)Element::RemoveChild(child);
            (void)slots_.TryRemoveAt(i);
            return true;
        }
    }

    return false;
}

Result<void> StackPanel::SetSpacing(float spacing) noexcept {
    if (!math::IsFinite(spacing) || spacing < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (spacing_ != spacing) {
        spacing_ = spacing;
        InvalidateLayout();
    }

    return Result<void>::Success();
}

void StackPanel::SetDirection(StackDirection direction) noexcept {
    if (direction_ != direction) {
        direction_ = direction;
        InvalidateLayout();
    }
}

void StackPanel::SetFlowDirection(FlowDirection flow) noexcept {
    if (flow_ != flow) {
        flow_ = flow;
        InvalidateArrangement();
    }
}

void StackPanel::SetAllowShrink(bool allowShrink) noexcept {
    if (allowShrink_ != allowShrink) {
        allowShrink_ = allowShrink;
        InvalidateArrangement();
    }
}

Result<void> StackPanel::Arrange(const Geometry& geometry) noexcept {
    if (slots_.Size() != ChildCount()) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    if (!geometry.size.IsFinite() || geometry.size.x < 0 || geometry.size.y < 0 ||
        (direction_ != StackDirection::Horizontal && direction_ != StackDirection::Vertical)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Array<StackSlot> visibleSlots(allocator_);
    Array<usize> visibleIndices(allocator_);
    auto reservedSlots = visibleSlots.TryReserve(slots_.Size());
    if (!reservedSlots) {
        return reservedSlots;
    }

    auto reservedIndices = visibleIndices.TryReserve(slots_.Size());
    if (!reservedIndices) {
        return reservedIndices;
    }

    const bool vertical = direction_ == StackDirection::Vertical;
    const bool rightToLeft = flow_ == FlowDirection::RightToLeft;
    for (usize n = 0; n < slots_.Size(); ++n) {
        const usize i = rightToLeft && !vertical ? slots_.Size() - 1 - n : n;
        if (ChildAt(i)->GetVisibility() == Visibility::Collapsed) {
            continue;
        }

        StackSlot slot = slots_[i];
        if (rightToLeft && vertical) {
            slot.crossBefore = slots_[i].crossAfter;
            slot.crossAfter = slots_[i].crossBefore;
            if (slot.alignment == StackAlignment::Start) {
                slot.alignment = StackAlignment::End;
            } else if (slot.alignment == StackAlignment::End) {
                slot.alignment = StackAlignment::Start;
            }
        } else if (rightToLeft) {
            slot.before = slots_[i].after;
            slot.after = slots_[i].before;
            if (slot.mainAlignment == StackAlignment::Start) {
                slot.mainAlignment = StackAlignment::End;
            } else if (slot.mainAlignment == StackAlignment::End) {
                slot.mainAlignment = StackAlignment::Start;
            }
        }

        const Vec2 desired = ChildAt(i)->SlotDesiredSize();
        if (!visibleSlots.IsEmpty()) {
            slot.before += spacing_;
        }

        slot.desired = direction_ == StackDirection::Vertical ? desired.y : desired.x;
        auto addedSlot = visibleSlots.TryPushBack(slot);
        if (!addedSlot) {
            return addedSlot;
        }

        auto addedIndex = visibleIndices.TryPushBack(i);
        if (!addedIndex) {
            return addedIndex;
        }
    }

    Array<StackPlacement> placements(allocator_);
    auto resized = placements.TryResize(visibleSlots.Size());
    if (!resized) {
        return resized;
    }

    const float available = vertical ? geometry.size.y : geometry.size.x;
    auto arranged = ArrangeStack(visibleSlots.View(), placements.View(), available, allowShrink_);
    if (!arranged) {
        return arranged;
    }

    SetGeometry(geometry);
    for (usize i = 0; i < placements.Size(); ++i) {
        const StackPlacement& placement = placements[i];
        const StackSlot& slot = visibleSlots[i];
        Element* child = ChildAt(visibleIndices[i]);
        const SlotPlacement content{vertical ? Vec4{slot.crossBefore, 0, slot.crossAfter, 0}
                                             : Vec4{0, slot.crossBefore, 0, slot.crossAfter},
                                    vertical ? slot.alignment : slot.mainAlignment,
                                    vertical ? slot.mainAlignment : slot.alignment};
        const Vec2 size = vertical ? Vec2{geometry.size.x, placement.size}
                                   : Vec2{placement.size, geometry.size.y};
        const AffineTransform2D offset{
            1, 0, 0, 1, vertical ? 0.0f : placement.offset, vertical ? placement.offset : 0.0f};
        auto childResult = child->ArrangeInSlot(content.PlaceChild(
            child->SlotDesiredSize(), GetGeometry().MakeChild(size, offset, {}, {})));
        if (!childResult) {
            return childResult;
        }
    }

    return Result<void>::Success();
}

Vec2 StackPanel::MeasureForWidth(float width) noexcept {
    if (direction_ != StackDirection::Vertical) {
        // 가로 쌓기는 배치(ArrangeStack)와 같은 규칙으로 잰다: Auto 칸은 원하는 폭 그대로이고, 넘칠
        // 때 Fill 칸이 남는 폭을 가중치대로 나눠 그 폭에서 다시 잰다(Fill 칸이 없으면 원하는 크기
        // 그대로).
        const Vec2 desired = GetDesiredSize();
        if (desired.x <= width) {
            return desired;
        }

        float fixed = 0;
        float weights = 0;
        usize visible = 0;
        for (usize i = 0; i < slots_.Size() && i < ChildCount(); ++i) {
            const Element* child = ChildAt(i);
            if (child->GetVisibility() == Visibility::Collapsed) {
                continue;
            }

            ++visible;
            fixed += slots_[i].before + slots_[i].after;
            if (slots_[i].rule == StackSizeRule::Fill) {
                weights += slots_[i].weight;
            } else {
                fixed += child->SlotDesiredSize().x;
            }
        }

        if (weights <= 0) {
            return desired;
        }

        fixed += visible > 1 ? spacing_ * static_cast<float>(visible - 1) : 0.0f;
        const float remaining = math::Max(0.0f, width - fixed);
        float height = 0;
        for (usize i = 0; i < slots_.Size() && i < ChildCount(); ++i) {
            Element* child = ChildAt(i);
            if (child->GetVisibility() == Visibility::Collapsed) {
                continue;
            }

            const Vec2 size = slots_[i].rule == StackSizeRule::Fill
                                  ? child->MeasureInSlot(remaining * slots_[i].weight / weights)
                                  : child->SlotDesiredSize();
            height = math::Max(height, size.y + slots_[i].crossBefore + slots_[i].crossAfter);
        }

        return {width, height};
    }

    float along = 0;
    float across = 0;
    bool hasVisible = false;
    for (usize i = 0; i < slots_.Size() && i < ChildCount(); ++i) {
        Element* child = ChildAt(i);
        if (child->GetVisibility() == Visibility::Collapsed) {
            continue;
        }

        const float inner = math::Max(0.0f, width - slots_[i].crossBefore - slots_[i].crossAfter);
        const Vec2 size = child->MeasureInSlot(inner);
        if (hasVisible) {
            along += spacing_;
        }

        hasVisible = true;
        along += math::Clamp(size.y, slots_[i].minSize, slots_[i].maxSize) + slots_[i].before +
                 slots_[i].after;
        across = math::Max(across, size.x + slots_[i].crossBefore + slots_[i].crossAfter);
    }

    return {across, along};
}

Vec2 StackPanel::ComputeDesiredSize(float) const noexcept {
    const bool vertical = direction_ == StackDirection::Vertical;
    float along = 0;
    float across = 0;
    bool hasVisible = false;
    for (usize i = 0; i < slots_.Size() && i < ChildCount(); ++i) {
        const Element* child = ChildAt(i);
        if (child->GetVisibility() == Visibility::Collapsed) {
            continue;
        }

        const Vec2 size = child->SlotDesiredSize();
        const float childAlong = vertical ? size.y : size.x;
        const float childAcross = vertical ? size.x : size.y;
        if (hasVisible) {
            along += spacing_;
        }

        hasVisible = true;
        along += math::Clamp(childAlong, slots_[i].minSize, slots_[i].maxSize) + slots_[i].before +
                 slots_[i].after;
        across = math::Max(across, childAcross + slots_[i].crossBefore + slots_[i].crossAfter);
    }

    return vertical ? Vec2(across, along) : Vec2(along, across);
}

} // namespace rocks::ui
