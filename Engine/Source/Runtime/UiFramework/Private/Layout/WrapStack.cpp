#include <UiFramework/Layout/WrapStack.h>

namespace rocks::ui {

bool WrapSlot::IsValid() const noexcept {
    return math::IsFinite(fillLineWhenSizeLessThan) && fillLineWhenSizeLessThan >= 0 &&
           placement.IsValid();
}

WrapStack::WrapStack(Allocator& allocator, StackDirection direction) noexcept
    : Element(allocator), slots_(allocator), direction_(direction) {
}

Result<void> WrapStack::TryAddChild(const SharedPtr<Element>& child) noexcept {
    return TryAddSlot(child, {});
}

Result<void> WrapStack::TryAddSlot(const SharedPtr<Element>& child, WrapSlot slot) noexcept {
    if (!slot.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto stored = slots_.TryPushBack(slot);
    if (!stored) {
        return stored;
    }

    auto added = Element::TryAddChild(child);
    if (!added) {
        (void)slots_.TryRemoveAt(slots_.Size() - 1);
    }

    return added;
}

Result<void> WrapStack::SetSlot(Element& child, const WrapSlot& slot) noexcept {
    if (!slot.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize i = 0; i < ChildCount(); ++i) {
        if (ChildAt(i) == &child) {
            slots_[i] = slot;
            InvalidateLayout();
            return Result<void>::Success();
        }
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

bool WrapStack::RemoveChild(Element& child) noexcept {
    for (usize i = 0; i < ChildCount(); ++i) {
        if (ChildAt(i) == &child) {
            (void)slots_.TryRemoveAt(i);
            return Element::RemoveChild(child);
        }
    }

    return false;
}

Result<void> WrapStack::SetPreferredExtent(float extent) noexcept {
    if (!math::IsFinite(extent) || extent < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (preferredExtent_ != extent) {
        preferredExtent_ = extent;
        InvalidateLayout();
    }

    return Result<void>::Success();
}

Result<void> WrapStack::SetSpacing(Vec2 spacing) noexcept {
    if (!spacing.IsFinite() || spacing.x < 0 || spacing.y < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (spacing_ != spacing) {
        spacing_ = spacing;
        InvalidateLayout();
    }

    return Result<void>::Success();
}

void WrapStack::SetUseAllottedSize(bool enabled) noexcept {
    if (useAllottedSize_ != enabled) {
        useAllottedSize_ = enabled;
        hasAllottedExtent_ = false;
        InvalidateLayout();
    }
}

WrapStack::Line WrapStack::MeasureLine(usize begin, float limit) const noexcept {
    const bool vertical = direction_ == StackDirection::Vertical;
    const float gap = vertical ? spacing_.y : spacing_.x;
    const usize count = ChildCount();
    Line line{begin, count, 0, 0};

    for (; line.end < count; ++line.end) {
        const Element* child = ChildAt(line.end);
        if (child->GetVisibility() == Visibility::Collapsed) {
            continue;
        }

        const auto& slot = slots_[line.end];
        const Vec2 desired = slot.placement.PaddedSize(child->GetDesiredSize());
        const bool wholeLine = !slot.forceNewLine && limit < slot.fillLineWhenSizeLessThan;
        const float length = wholeLine ? limit : (vertical ? desired.y : desired.x);
        if (line.last != count) {
            if (slot.forceNewLine || wholeLine || line.length + gap + length > limit) {
                break;
            }

            line.length += gap;
        }

        line.length += length;
        line.breadth = math::Max(line.breadth, vertical ? desired.x : desired.y);
        line.last = line.end;
        if (wholeLine) {
            ++line.end;
            break;
        }
    }

    if (line.last != count && slots_[line.last].fillEmptySpace) {
        line.length = limit;
    }

    return line;
}

Vec2 WrapStack::ComputeDesiredSize(float) const noexcept {
    const bool vertical = direction_ == StackDirection::Vertical;
    const float limit = useAllottedSize_ && hasAllottedExtent_ ? allottedExtent_ : preferredExtent_;
    double length = 0;
    double breadth = 0;
    for (usize begin = 0; begin < ChildCount();) {
        const Line line = MeasureLine(begin, limit);
        if (line.last == ChildCount()) {
            break;
        }

        if (begin != 0) {
            breadth += vertical ? spacing_.x : spacing_.y;
        }

        length = math::Max(length, line.length);
        breadth += line.breadth;
        begin = line.end;
    }

    const Vec2 size(static_cast<float>(length), static_cast<float>(breadth));
    return vertical ? Vec2(size.y, size.x) : size;
}

Result<void> WrapStack::Arrange(const Geometry& geometry) noexcept {
    if (!geometry.size.IsFinite() || geometry.size.x < 0 || geometry.size.y < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const bool vertical = direction_ == StackDirection::Vertical;
    const float extent = vertical ? geometry.size.y : geometry.size.x;
    if (useAllottedSize_ && (!hasAllottedExtent_ || allottedExtent_ != extent)) {
        allottedExtent_ = extent;
        hasAllottedExtent_ = true;
        // 부모의 다음 측정에도 실제 배치에서 결정한 줄 수를 반영한다.
        InvalidateLayout();
    }

    const float limit = useAllottedSize_ ? extent : preferredExtent_;
    SetGeometry(geometry);

    double across = 0;
    for (usize begin = 0; begin < ChildCount();) {
        const Line line = MeasureLine(begin, limit);
        double along = 0;
        for (usize i = begin; i < line.end; ++i) {
            Element* child = ChildAt(i);
            if (child->GetVisibility() == Visibility::Collapsed) {
                continue;
            }

            const auto& slot = slots_[i];
            const Vec2 desired = slot.placement.PaddedSize(child->GetDesiredSize());
            float length = vertical ? desired.y : desired.x;
            if ((!slot.forceNewLine && limit < slot.fillLineWhenSizeLessThan) ||
                (i == line.last && slot.fillEmptySpace)) {
                length = static_cast<float>(math::Max(0.0, line.length - along));
            }

            const Vec2 size = vertical ? Vec2(line.breadth, length) : Vec2(length, line.breadth);
            const Vec2 position(static_cast<float>(vertical ? across : along),
                                static_cast<float>(vertical ? along : across));
            if (!position.IsFinite()) {
                return Result<void>::Failure(ErrorCode::Overflow);
            }

            const AffineTransform2D offset{1, 0, 0, 1, position.x, position.y};
            auto arranged =
                slot.placement.ArrangeChild(*child, GetGeometry().MakeChild(size, offset, {}, {}));
            if (!arranged) {
                return arranged;
            }

            along += static_cast<double>(length) + (vertical ? spacing_.y : spacing_.x);
        }

        across += static_cast<double>(line.breadth) + (vertical ? spacing_.x : spacing_.y);
        begin = line.end;
    }

    return Result<void>::Success();
}

} // namespace rocks::ui
