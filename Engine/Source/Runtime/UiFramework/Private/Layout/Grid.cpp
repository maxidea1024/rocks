#include <Base/NumericLimits.h>
#include <UiFramework/Layout/Grid.h>

namespace rocks::ui {

Grid::Grid(Allocator& allocator) noexcept
    : Element(allocator), slots_(allocator), columns_(allocator), rows_(allocator) {
}

Result<void> Grid::TryAddChild(const SharedPtr<Element>& child) noexcept {
    return TryAddSlot(child, {});
}

bool Grid::StartsPaintLayer(usize previous, usize next) const noexcept {
    return slots_[previous].layer != slots_[next].layer;
}

Result<void> Grid::PrepareSlot(const GridSlot& slot) noexcept {
    const SlotPlacement placement{slot.padding, slot.horizontal, slot.vertical};
    if (slot.rowSpan == 0 || slot.columnSpan == 0 || !placement.IsValid() ||
        !slot.nudge.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 마지막 칸의 끝 좌표도 저장하므로 경계 하나를 더 확보한다.
    const uint64 rowEnd = static_cast<uint64>(slot.row) + slot.rowSpan;
    const uint64 columnEnd = static_cast<uint64>(slot.column) + slot.columnSpan;
    if (rowEnd >= MaxSize || columnEnd >= MaxSize) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    if (columnEnd >= columns_.Size()) {
        auto resized = columns_.TryResize(static_cast<usize>(columnEnd) + 1);
        if (!resized) {
            return resized;
        }
    }

    if (rowEnd >= rows_.Size()) {
        auto resized = rows_.TryResize(static_cast<usize>(rowEnd) + 1);
        if (!resized) {
            return resized;
        }
    }

    return Result<void>::Success();
}

Result<void> Grid::TryAddSlot(const SharedPtr<Element>& child, const GridSlot& slot) noexcept {
    if (!child) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto ready = PrepareSlot(slot);
    if (!ready) {
        return ready;
    }

    usize index = 0;
    while (index < slots_.Size() && slots_[index].layer <= slot.layer) {
        ++index;
    }

    auto stored = slots_.TryInsert(slot, index);
    if (!stored) {
        return stored;
    }

    auto added = TryInsertChild(index, child);
    if (!added) {
        (void)slots_.TryRemoveAt(index);
    }

    return added;
}

Result<void> Grid::SetSlot(Element& child, const GridSlot& slot) noexcept {
    usize index = 0;
    while (index < slots_.Size() && ChildAt(index) != &child) {
        ++index;
    }

    if (index == slots_.Size()) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    auto ready = PrepareSlot(slot);
    if (!ready) {
        return ready;
    }

    // 레이어가 바뀐 자식은 같은 레이어의 기존 자식 뒤에 들어간다.
    usize next = index;
    if (slots_[index].layer != slot.layer) {
        while (next + 1 < slots_.Size() && slots_[next + 1].layer <= slot.layer) {
            slots_[next] = slots_[next + 1];
            ++next;
        }
        while (next > 0 && slots_[next - 1].layer > slot.layer) {
            slots_[next] = slots_[next - 1];
            --next;
        }

        MoveChildWithin(index, next);
    }

    slots_[next] = slot;
    InvalidateLayout();
    return Result<void>::Success();
}

bool Grid::RemoveChild(Element& child) noexcept {
    for (usize i = 0; i < ChildCount(); ++i) {
        if (ChildAt(i) == &child) {
            (void)slots_.TryRemoveAt(i);
            return Element::RemoveChild(child);
        }
    }

    return false;
}

Result<void> Grid::SetFill(Array<Track>& tracks, uint32 index, float weight) noexcept {
    if (!math::IsFinite(weight) || weight < 0 || index == NumericLimits<uint32>::Max()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (index >= tracks.Size()) {
        auto grown = tracks.TryResize(static_cast<usize>(index) + 1);
        if (!grown) {
            return grown;
        }
    }

    if (tracks[index].weight != weight) {
        tracks[index].weight = weight;
        InvalidateArrangement();
    }

    return Result<void>::Success();
}

void Grid::ClearFill() noexcept {
    bool changed = false;
    Array<Track>* axes[] = {&columns_, &rows_};
    for (auto* tracks : axes) {
        for (auto& track : *tracks) {
            changed |= track.weight != 0;
            track.weight = 0;
        }
    }

    if (changed) {
        InvalidateArrangement();
    }
}

Result<void> Grid::SetColumnFill(uint32 column, float weight) noexcept {
    return SetFill(columns_, column, weight);
}

Result<void> Grid::SetRowFill(uint32 row, float weight) noexcept {
    return SetFill(rows_, row, weight);
}

Vec2 Grid::ComputeDesiredSize(float) const noexcept {
    measuredColumns_ = 0;
    measuredRows_ = 0;
    for (Track& track : columns_) {
        track.desired = 0;
    }

    for (Track& track : rows_) {
        track.desired = 0;
    }

    for (usize i = 0; i < slots_.Size(); ++i) {
        const Element* child = ChildAt(i);
        if (child->GetVisibility() == Visibility::Collapsed) {
            continue;
        }

        const GridSlot& slot = slots_[i];
        const Vec2 size = child->GetDesiredSize();
        const double width =
            (static_cast<double>(size.x) + slot.padding.x + slot.padding.z) / slot.columnSpan;
        const double height =
            (static_cast<double>(size.y) + slot.padding.y + slot.padding.w) / slot.rowSpan;
        const usize endX = static_cast<usize>(slot.column) + slot.columnSpan;
        const usize endY = static_cast<usize>(slot.row) + slot.rowSpan;
        measuredColumns_ = measuredColumns_ > endX ? measuredColumns_ : endX;
        measuredRows_ = measuredRows_ > endY ? measuredRows_ : endY;
        for (usize x = slot.column; x < endX; ++x) {
            columns_[x].desired = math::Max(columns_[x].desired, width);
        }

        for (usize y = slot.row; y < endY; ++y) {
            rows_[y].desired = math::Max(rows_[y].desired, height);
        }
    }

    double width = 0;
    double height = 0;
    for (usize x = 0; x < measuredColumns_; ++x) {
        width += columns_[x].desired;
    }

    for (usize y = 0; y < measuredRows_; ++y) {
        height += rows_[y].desired;
    }

    return Vec2(static_cast<float>(width), static_cast<float>(height));
}

void Grid::ArrangeTracks(Array<Track>& tracks, usize count, float available) noexcept {
    if (count == 0) {
        return;
    }

    double fixed = 0;
    double weights = 0;
    for (usize i = 0; i < count; ++i) {
        if (tracks[i].weight == 0) {
            fixed += tracks[i].desired;
        } else {
            weights += tracks[i].weight;
        }
    }

    const double unit = weights == 0 ? 0 : math::Max(0.0, available - fixed) / weights;
    double offset = 0;
    for (usize i = 0; i < count; ++i) {
        tracks[i].offset = offset;
        offset += tracks[i].weight == 0 ? tracks[i].desired : unit * tracks[i].weight;
    }

    tracks[count].offset = offset;
}

Result<void> Grid::Arrange(const Geometry& geometry) noexcept {
    if (!geometry.size.IsFinite() || geometry.size.x < 0 || geometry.size.y < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto measured = Prepass(MeasuredLayoutScale() > 0 ? MeasuredLayoutScale() : 1.0f);
    if (!measured) {
        return measured;
    }

    ArrangeTracks(columns_, measuredColumns_, geometry.size.x);
    ArrangeTracks(rows_, measuredRows_, geometry.size.y);
    SetGeometry(geometry);

    for (usize i = 0; i < slots_.Size(); ++i) {
        Element* child = ChildAt(i);
        if (child->GetVisibility() == Visibility::Collapsed) {
            continue;
        }

        const GridSlot& slot = slots_[i];
        const double x = columns_[slot.column].offset;
        const double y = rows_[slot.row].offset;
        const double right = columns_[static_cast<usize>(slot.column) + slot.columnSpan].offset;
        const double bottom = rows_[static_cast<usize>(slot.row) + slot.rowSpan].offset;
        const Vec2 size(static_cast<float>(math::Max(0.0, right - x)),
                        static_cast<float>(math::Max(0.0, bottom - y)));
        const Vec2 position(static_cast<float>(x + slot.nudge.x),
                            static_cast<float>(y + slot.nudge.y));
        if (!size.IsFinite() || !position.IsFinite()) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        const AffineTransform2D offset{1, 0, 0, 1, position.x, position.y};
        const SlotPlacement placement{slot.padding, slot.horizontal, slot.vertical};
        auto arranged =
            placement.ArrangeChild(*child, GetGeometry().MakeChild(size, offset, {}, {}));
        if (!arranged) {
            return arranged;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::ui
