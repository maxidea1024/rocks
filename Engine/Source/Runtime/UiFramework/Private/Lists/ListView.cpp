#include "../Scrolling/TouchPan.h"
#include <Base/NumericLimits.h>
#include <Core/Containers/HashSet.h>
#include <UiFramework/Lists/ListView.h>

namespace rocks::ui {

ListView::ListView(Allocator& allocator, Application& app, TableRowSource& source) noexcept
    : Element(allocator), source_(source), allocator_(allocator), app_(app), items_(allocator),
      spare_(allocator), selected_(allocator), presses_(allocator) {
    SetClipToBounds(true);
}

ListView::ListView(Allocator& allocator, Application& app,
                   const SharedRef<TableRowSource>& source) noexcept
    : ListView(allocator, app, *source) {
    retainedSource_ = source.AsSharedPtr();
}

ListView::~ListView() noexcept {
    for (usize i = 0; i < items_.Size(); ++i) {
        source_.ReleaseRow(*ChildAt(i), items_[i]);
    }
}

Result<void> ListView::SetRowHeight(float height) noexcept {
    if (!math::IsFinite(height) || height <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (rowHeight_ != height) {
        rowHeight_ = height;
        InvalidateLayout();
    }

    return Result<void>::Success();
}

Result<void> ListView::SetScrollOffset(double offset) noexcept {
    if (!math::IsFinite(offset) || offset < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    requestedIndex_.Reset();
    focusUser_.Reset();
    if (offset_ != offset) {
        offset_ = offset;
        InvalidateArrangement();
    }

    return Result<void>::Success();
}

double ListView::ScrollOffset() const noexcept {
    return offset_;
}

double ListView::MaxScrollOffset() const noexcept {
    return maxOffset_;
}

void ListView::SetPointerScrolling(bool enabled) noexcept {
    pointerScrolling_ = enabled;
}

void ListView::SetSelectOnNavigation(bool enabled) noexcept {
    selectOnNavigation_ = enabled;
}

Result<void> ListView::SetWheelMultiplier(float multiplier) noexcept {
    if (!math::IsFinite(multiplier) || multiplier < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    wheelMultiplier_ = multiplier;
    return Result<void>::Success();
}

Optional<uint64> ListView::SelectedKey() const noexcept {
    return selected_.IsEmpty() ? Optional<uint64>{}
                               : Optional<uint64>(selected_[selected_.Size() - 1]);
}

Span<const uint64> ListView::SelectedKeys() const noexcept {
    return selected_.View();
}

bool ListView::IsSelected(uint64 key) const noexcept {
    for (auto value : selected_) {
        if (value == key) {
            return true;
        }
    }

    return false;
}

bool ListView::IsItemVisible(uint64 key) const noexcept {
    for (auto item : items_) {
        if (item.key == key) {
            return true;
        }
    }

    return false;
}

Result<uint32> ListView::FindIndex(uint64 key) noexcept {
    auto ready = PrepareItems();
    if (!ready) {
        return Result<uint32>::Failure(ready.Error());
    }

    const uint32 count = ItemCount();
    for (uint32 i = 0; i < count; ++i) {
        if (ItemAt(i).key == key) {
            return Result<uint32>::Success(i);
        }
    }

    return Result<uint32>::Failure(ErrorCode::NotFound);
}

void ListView::SelectionChanged() noexcept {
    InvalidatePaint();
    for (usize i = 0; i < items_.Size(); ++i) {
        source_.SetRowSelected(*ChildAt(i), items_[i], IsSelected(items_[i].key));
    }

    selectionChanged_.ExecuteIfBound();
}

void ListView::SetOnItemClicked(Delegate<void(uint64)>&& callback) noexcept {
    if (!callback) {
        itemClicked_.Unbind();
        presses_.Clear();
    } else {
        itemClicked_ = Move(callback);
    }
}

void ListView::SetOnSelectionChanged(Delegate<void()>&& callback) noexcept {
    if (!callback) {
        selectionChanged_.Unbind();
    } else {
        selectionChanged_ = Move(callback);
    }
}

void ListView::SetOnItemScrolledIntoView(Delegate<void(uint64)>&& callback) noexcept {
    if (!callback) {
        itemScrolled_.Unbind();
    } else {
        itemScrolled_ = Move(callback);
    }
}

void ListView::ClearSelection() noexcept {
    rangeAnchor_.Reset();
    if (!selected_.IsEmpty()) {
        selected_.Clear();
        SelectionChanged();
    }
}

void ListView::SetSelectionMode(ListSelectionMode mode) noexcept {
    selectionMode_ = mode;
    if (mode == ListSelectionMode::None) {
        ClearSelection();
    } else if (mode != ListSelectionMode::Multi && selected_.Size() > 1) {
        (void)ReplaceSelection(selected_[selected_.Size() - 1]);
    }
}

Result<void> ListView::ReplaceSelection(uint64 key) noexcept {
    if (selectionMode_ == ListSelectionMode::None ||
        (selected_.Size() == 1 && selected_[0] == key)) {
        return Result<void>::Success();
    }

    if (selected_.IsEmpty()) {
        auto added = selected_.TryPushBack(key);
        if (!added) {
            return added;
        }
    } else {
        selected_[0] = key;
        (void)selected_.TryRemoveAt(1, selected_.Size() - 1);
    }

    SelectionChanged();
    return Result<void>::Success();
}

Result<void> ListView::SetItemSelection(uint64 key, bool selected) noexcept {
    if (selected) {
        auto index = FindIndex(key);
        if (!index) {
            return Result<void>::Failure(index.Error());
        }

        if (selectionMode_ != ListSelectionMode::Multi) {
            return ReplaceSelection(key);
        }

        if (!IsSelected(key)) {
            auto added = selected_.TryPushBack(key);
            if (!added) {
                return added;
            }

            SelectionChanged();
        }
    } else {
        for (usize i = 0; i < selected_.Size(); ++i) {
            if (selected_[i] == key) {
                (void)selected_.TryRemoveAt(i);
                SelectionChanged();
                break;
            }
        }
    }

    return Result<void>::Success();
}

Result<void> ListView::SelectRange(uint32 first, uint32 last) noexcept {
    const uint32 begin = first < last ? first : last;
    const uint32 end = first < last ? last : first;
    Array<uint64> range(allocator_);
    auto reserved = range.TryEnsureCapacity(static_cast<usize>(end) - begin + 1);
    if (!reserved) {
        return reserved;
    }

    for (uint32 i = begin;; ++i) {
        (void)range.TryPushBack(ItemAt(i).key);
        if (i == end) {
            break;
        }
    }

    selected_ = Move(range);
    SelectionChanged();
    return Result<void>::Success();
}

Result<void> ListView::SelectIndex(uint32 index) noexcept {
    auto ready = PrepareItems();
    if (!ready) {
        return ready;
    }

    if (index >= ItemCount()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    rangeAnchor_ = index;
    return ReplaceSelection(ItemAt(index).key);
}

void ListView::CancelScrollIntoView() noexcept {
    requestedIndex_.Reset();
    focusUser_.Reset();
}

bool ListView::IsRefreshPending() const noexcept {
    return refresh_;
}

void ListView::SetScrollIntoViewAlignment(ScrollIntoViewAlignment alignment) noexcept {
    scrollAlignment_ = alignment;
}

Result<void> ListView::RequestIndex(uint32 index, Optional<uint32> focusUser,
                                    bool updateSelection) noexcept {
    auto ready = PrepareItems();
    if (!ready) {
        return ready;
    }

    if (index >= ItemCount()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    requestedIndex_ = index;
    focusUser_ = focusUser;
    if (focusUser) {
        navigationIndex_ = index;
        if (updateSelection && selectOnNavigation_) {
            auto changed = ReplaceSelection(ItemAt(index).key);
            if (!changed) {
                CancelScrollIntoView();
                return changed;
            }
        }
    }

    InvalidateArrangement();
    return Result<void>::Success();
}

Result<void> ListView::ScrollToEnd() noexcept {
    auto ready = PrepareItems();
    if (!ready) {
        return ready;
    }

    return ItemCount() == 0 ? SetScrollOffset(0) : ScrollIndexIntoView(ItemCount() - 1);
}

Result<void> ListView::ScrollIndexIntoView(uint32 index) noexcept {
    return RequestIndex(index, {});
}

Result<void> ListView::NavigateToIndex(uint32 index, uint32 user) noexcept {
    return RequestIndex(index, user);
}

void ListView::Refresh() noexcept {
    presses_.Clear();
    requestedIndex_.Reset();
    focusUser_.Reset();
    navigationIndex_.Reset();
    rangeAnchor_.Reset();
    refresh_ = true;
    InvalidateLayout();
}

Vec2 ListView::ComputeDesiredSize(float) const noexcept {
    return {0, rowHeight_};
}

uint32 ListView::Columns(float) const noexcept {
    return 1;
}

Result<void> ListView::PrepareItems() noexcept {
    return Result<void>::Success();
}

uint32 ListView::ItemCount() const noexcept {
    return source_.RootCount();
}

TableItem ListView::ItemAt(uint32 index) const noexcept {
    return {source_.RootKey(index), 0};
}

TableItem ListView::ItemAtRow(usize row) const noexcept {
    return items_[row];
}

Result<void> ListView::TryAddChild(const SharedPtr<Element>&) noexcept {
    return Result<void>::Failure(ErrorCode::Unsupported);
}

bool ListView::RemoveChild(Element&) noexcept {
    return false;
}

void ListView::RecycleRow(usize index) noexcept {
    auto row = ChildRefAt(index);
    for (usize i = presses_.Size(); i > 0; --i) {
        if (presses_[i - 1].key == items_[index].key) {
            (void)presses_.TryRemoveAt(i - 1);
        }
    }

    app_.ReleaseInputForSubtree(*row);
    source_.ReleaseRow(*row, items_[index]);
    (void)Element::RemoveChild(*row);
    (void)items_.TryRemoveAt(index);
    // 풀 할당에 실패하면 행의 소유권을 여기서 놓는다.
    (void)spare_.TryPushBack(Move(row));
}

Result<void> ListView::AttachRow(uint32 index, bool prepend) noexcept {
    SharedPtr<Element> row;
    if (spare_.IsEmpty()) {
        auto made = source_.CreateRow(allocator_);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        row = Move(made.Value());
        if (!row || row->Parent()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    } else {
        row = Move(spare_[spare_.Size() - 1]);
        (void)spare_.TryRemoveAt(spare_.Size() - 1);
    }

    auto item = ItemAt(index);
    const usize position = prepend ? 0 : items_.Size();
    auto bound = source_.BindRow(*row, item);
    if (!bound) {
        source_.ReleaseRow(*row, item);
        return bound;
    }

    auto stored = items_.TryInsert(item, position);
    if (!stored) {
        source_.ReleaseRow(*row, item);
        return stored;
    }

    auto attached = TryInsertChild(position, row);
    if (!attached) {
        (void)items_.TryRemoveAt(position);
        source_.ReleaseRow(*row, item);
        return attached;
    }

    if (prepend || items_.Size() == 1) {
        first_ = index;
    }

    source_.SetRowSelected(*row, item, IsSelected(item.key));
    row->UpdateBoundAttributes();
    return Result<void>::Success();
}

Result<void> ListView::Arrange(const Geometry& geometry) noexcept {
    if (!geometry.size.IsFinite() || geometry.size.x < 0 || geometry.size.y < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto ready = PrepareItems();
    if (!ready) {
        return ready;
    }

    SetGeometry(geometry);
    const uint32 columns = Columns(geometry.size.x);
    const uint32 count = ItemCount();
    const uint32 lines = count / columns + (count % columns != 0);
    const double height = rowHeight_;
    maxOffset_ = math::Max(0.0, lines * height - geometry.size.y);
    if (refresh_ && !selected_.IsEmpty()) {
        HashSet<uint64> missing(allocator_);
        for (uint64 key : selected_) {
            auto added = missing.Add(key);
            if (!added) {
                return Result<void>::Failure(added.Error());
            }
        }

        for (uint32 i = 0; i < count && !missing.IsEmpty(); ++i) {
            missing.Erase(ItemAt(i).key);
        }

        if (!missing.IsEmpty()) {
            for (usize i = selected_.Size(); i > 0; --i) {
                if (missing.Contains(selected_[i - 1])) {
                    (void)selected_.TryRemoveAt(i - 1);
                }
            }

            SelectionChanged();
        }
    }

    if (requestedIndex_ && *requestedIndex_ < count && geometry.size.y > 0) {
        const double top = (*requestedIndex_ / columns) * height;
        const double bottom = top + height;
        if (scrollAlignment_ == ScrollIntoViewAlignment::Center) {
            offset_ = top + (height - geometry.size.y) * 0.5;
        } else if (scrollAlignment_ == ScrollIntoViewAlignment::End) {
            offset_ = bottom - geometry.size.y;
        } else if (scrollAlignment_ == ScrollIntoViewAlignment::Start || top < offset_ ||
                   height >= geometry.size.y) {
            offset_ = top;
        } else if (bottom > offset_ + geometry.size.y) {
            offset_ = bottom - geometry.size.y;
        }
    }

    offset_ = math::Clamp(offset_, 0.0, maxOffset_);
    const auto startLine =
        static_cast<uint32>(math::Min(static_cast<double>(lines), offset_ / height));
    const auto endLine = static_cast<uint32>(
        math::Min(static_cast<double>(lines), math::Ceil((offset_ + geometry.size.y) / height)));
    const auto indexAt = [&](uint32 line) noexcept -> uint32 {
        return line == lines ? count : line * columns;
    };
    const uint32 start = indexAt(startLine);
    const uint32 end = geometry.size.x > 0 && geometry.size.y > 0 ? indexAt(endLine) : start;

    // 양 끝의 바깥 행만 빼므로 한 줄 스크롤할 때 가운데 행의 인스턴스와 입력 상태가 유지된다.
    while (!items_.IsEmpty() && (refresh_ || first_ + items_.Size() > end)) {
        RecycleRow(items_.Size() - 1);
    }
    while (!items_.IsEmpty() && first_ < start) {
        RecycleRow(0);
        ++first_;
    }

    refresh_ = false;
    if (items_.IsEmpty()) {
        first_ = start;
    }
    while (first_ > start) {
        auto added = AttachRow(first_ - 1, true);
        if (!added) {
            return added;
        }
    }
    while (first_ + items_.Size() < end) {
        auto added = AttachRow(first_ + static_cast<uint32>(items_.Size()), false);
        if (!added) {
            return added;
        }
    }

    spare_.Clear();
    const float cellWidth = geometry.size.x / static_cast<float>(columns);
    for (usize i = 0; i < items_.Size(); ++i) {
        const auto index = first_ + static_cast<uint32>(i);
        const float x = static_cast<float>(index % columns) * cellWidth;
        const float y = static_cast<float>((index / columns) * height - offset_);
        auto* row = ChildAt(i);
        auto placed = row->Prepass(MeasuredLayoutScale() > 0 ? MeasuredLayoutScale() : 1.0f);
        if (placed) {
            placed = row->ArrangeIfNeeded(
                GetGeometry().MakeChild({cellWidth, rowHeight_}, {1, 0, 0, 1, x, y}, {}, {}));
        }

        if (!placed) {
            return placed;
        }
    }

    if (!selected_.IsEmpty()) {
        InvalidatePaint();
    }

    if (requestedIndex_ && *requestedIndex_ >= first_ &&
        *requestedIndex_ - first_ < items_.Size()) {
        const uint64 key = items_[*requestedIndex_ - first_].key;
        auto target = ChildRefAt(*requestedIndex_ - first_);
        const auto user = focusUser_;
        requestedIndex_.Reset();
        focusUser_.Reset();
        itemScrolled_.ExecuteIfBound(key);
        if (user) {
            auto current = app_.GetFocus(*user);
            if (!current || !current->IsWithin(*target)) {
                // 목록 안 이동(방향키·NavigateToIndex)은 늘 Navigation이다(UE
                // STableViewBase::NavigateToWidget, STableViewBase.cpp 1277행).
                auto focused =
                    app_.MoveFocusWithin(*user, target, true, false, FocusCause::Navigation);
                if (!focused) {
                    return Result<void>::Failure(focused.Error());
                }

                if (!focused.Value()) {
                    return app_.SetFocus(*user, *this, FocusCause::Navigation);
                }
            }
        }
    }

    return Result<void>::Success();
}

Result<void> ListView::OnPaintOverlay(PaintList& list, int32 layer) const noexcept {
    if (!selected_.IsEmpty()) {
        for (usize i = 0; i < items_.Size(); ++i) {
            if (IsSelected(items_[i].key)) {
                auto added = list.TryAddBox(
                    {ChildAt(i)->GetGeometry(), {0.18f, 0.42f, 0.78f, 0.18f}, layer});
                if (!added) {
                    return added;
                }
            }
        }
    }

    return Result<void>::Success();
}

bool ListView::SupportsKeyboardFocus() const noexcept {
    return true;
}

void ListView::OnPointerReleaseInPath(const PointerEvent& event) noexcept {
    if (event.type == PointerEventType::Up && event.button != input::MouseButton::Left) {
        return;
    }

    for (usize i = 0; i < presses_.Size(); ++i) {
        auto press = presses_[i];
        if (press.user != event.userId || press.pointer != event.pointerId ||
            press.source != event.source) {
            continue;
        }

        (void)presses_.TryRemoveAt(i);
        if (event.type == PointerEventType::Up && GetGeometry().Contains(event.position)) {
            for (usize row = 0; row < items_.Size(); ++row) {
                if (items_[row].key == press.key &&
                    ChildAt(row)->GetGeometry().Contains(event.position)) {
                    if (event.source == PointerSource::Touch) {
                        SelectFromPointer(first_ + static_cast<uint32>(row), press.key,
                                          event.userId);
                    }

                    itemClicked_.ExecuteIfBound(press.key);
                    break;
                }
            }
        }

        return;
    }
}

void ListView::SelectFromPointer(uint32 index, uint64 key, uint32 user) noexcept {
    if (selectionMode_ == ListSelectionMode::Multi && app_.IsShiftDown(user) && rangeAnchor_) {
        (void)SelectRange(*rangeAnchor_, index);
    } else {
        rangeAnchor_ = index;
        if (selectionMode_ == ListSelectionMode::SingleToggle ||
            (selectionMode_ == ListSelectionMode::Multi && app_.IsControlDown(user))) {
            (void)SetItemSelection(key, !IsSelected(key));
        } else {
            (void)ReplaceSelection(key);
        }
    }
}

PointerReply ListView::OnPreviewPointerDown(const PointerEvent& event) noexcept {
    if (event.button == input::MouseButton::Left) {
        for (usize i = 0; i < items_.Size(); ++i) {
            if (ChildAt(i)->GetGeometry().Contains(event.position)) {
                navigationIndex_ = first_ + static_cast<uint32>(i);
                const uint32 index = *navigationIndex_;
                const uint64 key = items_[i].key;
                if (itemClicked_ || event.source == PointerSource::Touch) {
                    for (usize j = 0; j < presses_.Size(); ++j) {
                        if (presses_[j].user == event.userId &&
                            presses_[j].pointer == event.pointerId &&
                            presses_[j].source == event.source) {
                            (void)presses_.TryRemoveAt(j);
                            break;
                        }
                    }

                    (void)presses_.TryPushBack({event.userId, event.pointerId, event.source, key});
                }

                if (event.source != PointerSource::Touch) {
                    SelectFromPointer(index, key, event.userId);
                }
                break;
            }
        }
    }

    // 행의 단추나 편집기도 같은 누름을 받을 수 있도록 이벤트는 계속 전달한다.
    return {};
}

bool ListView::OnKeyEvent(uint32 user, const input::InputEvent& event) noexcept {
    if (event.type != input::InputEventType::KeyDown || ItemCount() == 0) {
        return false;
    }

    const uint32 columns = Columns(GetGeometry().size.x);
    int64 index = navigationIndex_.ValueOr(first_);
    bool hasAnchor = navigationIndex_.HasValue();
    if (!requestedIndex_) {
        if (auto focus = app_.GetFocus(user)) {
            for (usize i = 0; i < items_.Size(); ++i) {
                if (focus->IsWithin(*ChildAt(i))) {
                    index = first_ + static_cast<int64>(i);
                    hasAnchor = true;
                    break;
                }
            }
        }
    }

    const bool multi = selectionMode_ == ListSelectionMode::Multi;
    if (multi && event.key == input::Key::A && app_.IsControlDown(user)) {
        return static_cast<bool>(SelectRange(0, ItemCount() - 1));
    }

    const uint32 anchor = rangeAnchor_.ValueOr(static_cast<uint32>(index));
    switch (event.key) {
    case input::Key::Up:
        if (hasAnchor) {
            index -= columns;
        }
        break;
    case input::Key::Down:
        if (hasAnchor) {
            index += columns;
        }
        break;
    case input::Key::Left:
    case input::Key::Right:
        if (columns == 1) {
            return false;
        }

        if (hasAnchor) {
            index += event.key == input::Key::Right ? 1 : -1;
        }
        break;
    case input::Key::Home:
        index = 0;
        break;
    case input::Key::End:
        index = ItemCount() - 1;
        break;
    default:
        return false;
    }

    if (index < 0) {
        index = 0;
    } else if (index >= ItemCount()) {
        index = ItemCount() - 1;
    }

    const auto target = static_cast<uint32>(index);
    if (multi && selectOnNavigation_ && app_.IsShiftDown(user)) {
        rangeAnchor_ = anchor;
        auto range = SelectRange(anchor, target);
        return range && RequestIndex(target, user, false);
    }

    rangeAnchor_ = target;
    return static_cast<bool>(RequestIndex(target, user, !multi || !app_.IsControlDown(user)));
}

PointerReply ListView::OnTouchPan(const PointerEvent& move, Vec2 origin) noexcept {
    if (panning_ || !pointerScrolling_) {
        return {};
    }

    float start = 0, current = 0;
    if (!TryTouchPan(GetGeometry(), true, origin, move.position, offset_, maxOffset_, start,
                     current)) {
        return {};
    }

    panning_ = true;
    panUser_ = move.userId;
    panPointer_ = move.pointerId;
    panStart_ = start;
    panOffset_ = offset_;
    (void)SetScrollOffset(math::Clamp(panOffset_ + start - current, 0., maxOffset_));
    return {.handled = true, .capture = true};
}

void ListView::OnPointerCaptureLost(PointerSource source, uint32 user, uint32 pointer) noexcept {
    if (source == PointerSource::Touch && panning_ && user == panUser_ && pointer == panPointer_) {
        panning_ = false;
    }
}

PointerReply ListView::OnPointerEvent(const PointerEvent& event) noexcept {
    if (panning_ && event.source == PointerSource::Touch && event.userId == panUser_ &&
        event.pointerId == panPointer_) {
        if (event.type == PointerEventType::Move || event.type == PointerEventType::Up) {
            AffineTransform2D inverse;
            if (GetGeometry().render.TryInverse(inverse)) {
                const float current = inverse.Apply(event.position).y;
                (void)SetScrollOffset(
                    math::Clamp(panOffset_ + panStart_ - current, 0., maxOffset_));
            }
        }

        if (event.type == PointerEventType::Up || event.type == PointerEventType::Cancel) {
            panning_ = false;
            return {.handled = true, .releaseCapture = true};
        }

        return {.handled = true};
    }

    if (event.type == PointerEventType::Down && event.button == input::MouseButton::Left &&
        (itemClicked_ || event.source == PointerSource::Touch)) {
        for (const auto& press : presses_) {
            if (press.source == event.source && press.user == event.userId &&
                press.pointer == event.pointerId) {
                return {.handled = true,
                        .capture = true,
                        .allowTouchPan = event.source == PointerSource::Touch};
            }
        }
    }

    if (!pointerScrolling_ || event.type != PointerEventType::Wheel ||
        !math::IsFinite(event.wheel)) {
        return {};
    }

    const double next = math::Clamp(offset_ - static_cast<double>(event.wheel) * rowHeight_ * 3.0 *
                                                  wheelMultiplier_,
                                    0.0, maxOffset_);
    if (offset_ == next) {
        return {};
    }

    (void)SetScrollOffset(next);
    return {.handled = true};
}

AccessibleRole ListView::GetAccessibleRole() const noexcept {
    return AccessibleRole::List;
}

} // namespace rocks::ui
