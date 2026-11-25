#include <Base/NumericLimits.h>
#include <UiFramework/UniformGrid.h>

namespace rocks::ui {

UniformGrid::UniformGrid(Allocator& allocator) noexcept : Element(allocator), slots_(allocator) {
}

Result<void> UniformGrid::TryAddChild(const SharedPtr<Element>& child) noexcept {
    return TryAddSlot(child, 0, 0);
}

Result<void> UniformGrid::TryAddSlot(const SharedPtr<Element>& child, uint32 row, uint32 column,
                                     StackAlignment horizontal, StackAlignment vertical) noexcept {
    const SlotPlacement placement{{}, horizontal, vertical};
    if (!placement.IsValid() || row == NumericLimits<uint32>::Max() ||
        column == NumericLimits<uint32>::Max()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto slot = slots_.TryPushBack({row, column, horizontal, vertical});
    if (!slot) {
        return slot;
    }

    auto added = Element::TryAddChild(child);
    if (!added) {
        (void)slots_.TryRemoveAt(slots_.Size() - 1);
        return added;
    }

    return Result<void>::Success();
}

Result<void> UniformGrid::SetSlot(Element& child, uint32 row, uint32 column,
                                  StackAlignment horizontal, StackAlignment vertical) noexcept {
    const SlotPlacement placement{{}, horizontal, vertical};
    if (!placement.IsValid() || row == NumericLimits<uint32>::Max() ||
        column == NumericLimits<uint32>::Max()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize i = 0; i < ChildCount(); ++i) {
        if (ChildAt(i) == &child) {
            slots_[i] = {row, column, horizontal, vertical};
            InvalidateLayout();
            return Result<void>::Success();
        }
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

bool UniformGrid::RemoveChild(Element& child) noexcept {
    for (usize i = 0; i < ChildCount(); ++i) {
        if (ChildAt(i) == &child) {
            (void)Element::RemoveChild(child);
            (void)slots_.TryRemoveAt(i);
            return true;
        }
    }

    return false;
}

Result<void> UniformGrid::SetCellPadding(Vec4 padding) noexcept {
    if (!padding.IsFinite() || padding.x < 0 || padding.y < 0 || padding.z < 0 || padding.w < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (padding_ != padding) {
        padding_ = padding;
        InvalidateLayout();
    }

    return Result<void>::Success();
}

Result<void> UniformGrid::SetMinimumCellSize(Vec2 size) noexcept {
    if (!size.IsFinite() || size.x < 0 || size.y < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (minimumCellSize_ != size) {
        minimumCellSize_ = size;
        InvalidateLayout();
    }

    return Result<void>::Success();
}

void UniformGrid::GetDimensions(uint32& rows, uint32& columns) const noexcept {
    rows = 0;
    columns = 0;
    for (usize i = 0; i < slots_.Size(); ++i) {
        if (ChildAt(i)->GetVisibility() != Visibility::Collapsed) {
            rows = rows > slots_[i].row + 1 ? rows : slots_[i].row + 1;
            columns = columns > slots_[i].column + 1 ? columns : slots_[i].column + 1;
        }
    }
}

Vec2 UniformGrid::ComputeDesiredSize(float) const noexcept {
    uint32 rows = 0;
    uint32 columns = 0;
    GetDimensions(rows, columns);

    Vec2 cell = minimumCellSize_;
    for (usize i = 0; i < slots_.Size(); ++i) {
        const Element* child = ChildAt(i);
        if (child->GetVisibility() != Visibility::Collapsed) {
            const Vec2 desired = child->GetDesiredSize();
            cell.x = math::Max(cell.x, desired.x + padding_.x + padding_.z);
            cell.y = math::Max(cell.y, desired.y + padding_.y + padding_.w);
        }
    }

    return Vec2(cell.x * static_cast<float>(columns), cell.y * static_cast<float>(rows));
}

Result<void> UniformGrid::Arrange(const Geometry& geometry) noexcept {
    if (slots_.Size() != ChildCount() || !geometry.size.IsFinite() || geometry.size.x < 0 ||
        geometry.size.y < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    uint32 rows = 0;
    uint32 columns = 0;
    GetDimensions(rows, columns);
    SetGeometry(geometry);
    if (rows == 0 || columns == 0) {
        return Result<void>::Success();
    }

    const Vec2 cell(geometry.size.x / static_cast<float>(columns),
                    geometry.size.y / static_cast<float>(rows));
    for (usize i = 0; i < slots_.Size(); ++i) {
        Element* child = ChildAt(i);
        if (child->GetVisibility() == Visibility::Collapsed) {
            continue;
        }

        const AffineTransform2D offset{1,
                                       0,
                                       0,
                                       1,
                                       cell.x * static_cast<float>(slots_[i].column),
                                       cell.y * static_cast<float>(slots_[i].row)};
        const SlotPlacement placement{padding_, slots_[i].horizontal, slots_[i].vertical};
        auto arranged =
            placement.ArrangeChild(*child, GetGeometry().MakeChild(cell, offset, {}, {}));
        if (!arranged) {
            return arranged;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::ui
