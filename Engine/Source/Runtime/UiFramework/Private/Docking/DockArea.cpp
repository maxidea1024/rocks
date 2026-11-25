#include <UiFramework/Docking/DockManager.h>

#include "DockTabHeader.h"

namespace rocks::ui {

namespace {

// 탭 머리가 하나도 없을 때의 탭 줄 높이다(Normal 10pt 한 줄 + UE 탭 여백과 비슷한 값).
constexpr float EmptyTabHeight = 22;
constexpr float Divider = 5;
constexpr float DragThreshold = 5;

bool Contains(const Box2<float>& box, Vec2 p) noexcept {
    return p.x >= box.min.x && p.x < box.max.x && p.y >= box.min.y && p.y < box.max.y;
}

Geometry ChildGeometry(const Geometry& parent, Vec2 pos, Vec2 size) noexcept {
    return parent.MakeChild(size, {1, 0, 0, 1, pos.x, pos.y}, {}, {});
}

Result<void> Fill(PaintList& list, const Geometry& geometry, const Box2<float>& rect,
                  LinearColor color, int32 layer) noexcept {
    if (rect.max.x <= rect.min.x || rect.max.y <= rect.min.y) {
        return Result<void>::Success();
    }

    return list.TryAddBox({ChildGeometry(geometry, rect.min, rect.max - rect.min), color, layer});
}

constexpr DockSide GuideSides[] = {DockSide::Center, DockSide::Left, DockSide::Right, DockSide::Top,
                                   DockSide::Bottom};

// 표시와 히트 테스트가 작은 패널에서도 같은 다섯 영역을 사용한다.
Box2<float> GuideTile(Box2<float> bounds, DockSide side, float tabHeight) noexcept {
    bounds.min.y = math::Min(bounds.min.y + tabHeight, bounds.max.y);
    const Vec2 size = bounds.max - bounds.min;
    const float step = math::Min(32.0f, math::Min(size.x, size.y) / 3.0f);
    Vec2 center = bounds.min + size * 0.5f;
    switch (side) {
    case DockSide::Left:
        center.x -= step;
        break;
    case DockSide::Right:
        center.x += step;
        break;
    case DockSide::Top:
        center.y -= step;
        break;
    case DockSide::Bottom:
        center.y += step;
        break;
    case DockSide::Center:
        break;
    }

    const Vec2 half{step * 0.46f, step * 0.46f};
    return {center - half, center + half};
}

Box2<float> PlacementRect(Box2<float> rect, DockSide side, float gap) noexcept {
    const Vec2 center = rect.min + (rect.max - rect.min) * 0.5f;
    const float dx = math::Min(gap, rect.max.x - rect.min.x) * 0.5f;
    const float dy = math::Min(gap, rect.max.y - rect.min.y) * 0.5f;
    switch (side) {
    case DockSide::Left:
        rect.max.x = center.x - dx;
        break;
    case DockSide::Right:
        rect.min.x = center.x + dx;
        break;
    case DockSide::Top:
        rect.max.y = center.y - dy;
        break;
    case DockSide::Bottom:
        rect.min.y = center.y + dy;
        break;
    case DockSide::Center:
        break;
    }

    return rect;
}

constexpr DockSide OuterSides[] = {DockSide::Left, DockSide::Right, DockSide::Top,
                                   DockSide::Bottom};

Box2<float> OuterTile(Vec2 size, DockSide side) noexcept {
    const float half = math::Min(15.0f, math::Min(size.x, size.y) / 8.0f);
    const float inset = half * 1.4f;
    Vec2 center = size * 0.5f;
    switch (side) {
    case DockSide::Left:
        center.x = inset;
        break;
    case DockSide::Right:
        center.x = size.x - inset;
        break;
    case DockSide::Top:
        center.y = inset;
        break;
    case DockSide::Bottom:
        center.y = size.y - inset;
        break;
    case DockSide::Center:
        break;
    }

    const Vec2 extent{half, half};
    return {center - extent, center + extent};
}

DockSide OuterSide(Vec2 size, Vec2 point) noexcept {
    for (DockSide side : OuterSides) {
        if (Contains(OuterTile(size, side), point)) {
            return side;
        }
    }

    const float dx = math::Min(point.x, size.x - point.x);
    const float dy = math::Min(point.y, size.y - point.y);
    const float edge = math::Min(8.0f, math::Min(size.x, size.y) * 0.05f);
    if (math::Min(dx, dy) >= edge) {
        return DockSide::Center;
    }

    if (dx < dy) {
        return point.x < size.x * 0.5f ? DockSide::Left : DockSide::Right;
    }

    return point.y < size.y * 0.5f ? DockSide::Top : DockSide::Bottom;
}

Result<void> PaintGuide(PaintList& list, const Geometry& geometry, Box2<float> tile, DockSide side,
                        bool selected, int32 layer) noexcept {
    const LinearColor face =
        selected ? LinearColor{0.05f, 0.30f, 0.72f, 1} : LinearColor{0.045f, 0.075f, 0.12f, 1};
    auto result = Fill(list, geometry, tile, face, layer);
    if (!result) {
        return result;
    }

    const Vec2 inset = tile.Size() * 0.22f;
    tile.min += inset;
    tile.max -= inset;
    result = Fill(list, geometry, tile, {0.18f, 0.24f, 0.32f, 1}, layer);
    return result ? Fill(list, geometry, PlacementRect(tile, side, 0), {0.68f, 0.85f, 1, 1}, layer)
                  : Move(result);
}

} // namespace

Vec2 DockManager::MeasureNode(SlotHandle handle) const noexcept {
    if (handle.IsNull()) {
        return {};
    }

    const Node& node = nodes_[handle];
    if (node.kind == Kind::Stack) {
        const Tab& tab = tabs_[node.tabs[node.selected]];
        const Vec2 content = tab.content->GetDesiredSize();
        const float maxWidth = DefaultDockTabStyle().maxTabSize.x;
        float width = TabGap() * static_cast<float>(node.tabs.Size() - 1);
        for (usize i : node.tabs) {
            width += math::Min(tabs_[i].header->GetDesiredSize().x, maxWidth);
        }

        return {math::Max(width, content.x), TabHeight() + content.y};
    }

    const Vec2 a = MeasureNode(node.first);
    const Vec2 b = MeasureNode(node.second);
    return node.kind == Kind::Horizontal ? Vec2(a.x + Divider + b.x, math::Max(a.y, b.y))
                                         : Vec2(math::Max(a.x, b.x), a.y + Divider + b.y);
}

float DockManager::TabHeight() const noexcept {
    float height = 0;
    for (const auto& tab : tabs_) {
        if (tab.header && !tab.stack.IsNull()) {
            height = math::Max(height, tab.header->GetDesiredSize().y);
        }
    }

    return math::Min(height > 0 ? height : EmptyTabHeight, DefaultDockTabStyle().maxTabSize.y);
}

float DockManager::TabGap() const noexcept {
    return DefaultDockTabStyle().tabGap;
}

Vec2 DockManager::ComputeDesiredSize(float) const noexcept {
    return MeasureNode(root_);
}

Result<void> DockManager::Arrange(const Geometry& geometry) noexcept {
    if (!geometry.size.IsFinite() || geometry.size.x < 0 || geometry.size.y < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    SetGeometry(geometry);
    return root_.IsNull() ? Result<void>::Success() : ArrangeNode(root_, {}, geometry.size);
}

Result<void> DockManager::ArrangeNode(SlotHandle handle, Vec2 origin, Vec2 size) noexcept {
    Node& node = nodes_[handle];
    node.bounds = {origin, origin + size};
    if (node.kind == Kind::Stack) {
        const float height = math::Min(TabHeight(), size.y);
        // 한 탭 줄의 탭은 모두 같은 너비다: 줄 너비를 나누고 최대 너비로 자른다(UE
        // SDockingTabWell::ComputeChildSize — (W − Overlap)/N + Overlap, Overlap = −간격).
        const float gap = TabGap();
        const float count = static_cast<float>(node.tabs.Size());
        const float width = math::Max(
            0.0f, math::Min((size.x + gap) / count - gap, DefaultDockTabStyle().maxTabSize.x));
        float x = origin.x;
        for (usize i : node.tabs) {
            auto& tab = tabs_[i];
            const Vec2 headerSize{width, height};
            tab.headerBounds = {{x, origin.y}, {x + width, origin.y + height}};
            auto arranged = tab.header->ArrangeIfNeeded(
                ChildGeometry(GetGeometry(), tab.headerBounds.min, headerSize));
            if (!arranged) {
                return arranged;
            }

            x += width + gap;
        }

        const Vec2 position{origin.x, origin.y + height};
        return tabs_[node.tabs[node.selected]].content->ArrangeIfNeeded(
            ChildGeometry(GetGeometry(), position, {size.x, size.y - height}));
    }

    const bool horizontal = node.kind == Kind::Horizontal;
    const float extent = horizontal ? size.x : size.y;
    const float gap = math::Min(extent, Divider);
    const float available = extent - gap;
    const float first = available * node.fraction;
    Vec2 firstSize = size;
    Vec2 secondSize = size;
    Vec2 secondOrigin = origin;
    if (horizontal) {
        firstSize.x = first;
        secondSize.x = available - first;
        secondOrigin.x += first + gap;
        node.handle = {{origin.x + first, origin.y}, {secondOrigin.x, origin.y + size.y}};
    } else {
        firstSize.y = first;
        secondSize.y = available - first;
        secondOrigin.y += first + gap;
        node.handle = {{origin.x, origin.y + first}, {origin.x + size.x, secondOrigin.y}};
    }

    auto arranged = ArrangeNode(node.first, origin, firstSize);
    if (!arranged) {
        return arranged;
    }

    return ArrangeNode(node.second, secondOrigin, secondSize);
}

Result<void> DockManager::OnPaint(PaintList& list, int32 layer) const noexcept {
    // 탭 줄은 TabWellBrush, 내용은 ContentAreaBrush, 패널 사이 틈은 탭 줄과 같은 바탕이다(UE
    // "Docking.Tab"). 탭 자체는 DockTabHeader가 그린다.
    const DockTabStyle& style = DefaultDockTabStyle();
    const ThemeColors& theme = list.Theme();
    const StyleColorContext context = list.ColorContext();
    const auto paint = [&](const StyleBrush& brush, Box2<float> rect) noexcept {
        if (brush.mode == BrushMode::None || rect.max.x <= rect.min.x || rect.max.y <= rect.min.y) {
            return Result<void>::Success();
        }

        return list.TryAddBrush(ChildGeometry(GetGeometry(), rect.min, rect.max - rect.min),
                                brush.Resolve(theme, context), {1, 1, 1, 1}, layer);
    };
    const float tabHeight = TabHeight();
    for (const auto& node : nodes_) {
        if (node.kind != Kind::Stack) {
            auto painted = paint(style.tabWell, node.handle);
            if (!painted) {
                return painted;
            }

            continue;
        }

        const float split = math::Min(node.bounds.min.y + tabHeight, node.bounds.max.y);
        auto painted = paint(style.tabWell, {node.bounds.min, {node.bounds.max.x, split}});
        if (painted) {
            painted = paint(style.contentArea, {{node.bounds.min.x, split}, node.bounds.max});
        }

        if (!painted) {
            return painted;
        }
    }

    return Result<void>::Success();
}

Result<void> DockManager::OnPaintOverlay(PaintList& list, int32 layer) const noexcept {
    const Node* node = nodes_.Find(target_);
    if (!node && previewOwner_ && root_.IsNull()) {
        return Fill(list, GetGeometry(), {{}, GetGeometry().size}, {0.10f, 0.42f, 0.90f, 0.38f},
                    layer);
    }

    if ((!node && !showOuterGuides_) || (!dragging_ && !previewOwner_)) {
        return Result<void>::Success();
    }

    if (node) {
        auto rect = PlacementRect(node->bounds, side_, Divider);
        if (insertion_ != MaxSize) {
            const auto& tab =
                tabs_[node->tabs[insertion_ < node->tabs.Size() - 1 ? insertion_
                                                                    : node->tabs.Size() - 1]];
            rect = tab.headerBounds;
            const float x = insertion_ == node->tabs.Size() ? rect.max.x : rect.min.x;
            rect.min.x = math::Max(node->bounds.min.x, x - 1);
            rect.max.x = math::Min(node->bounds.max.x, x + 2);
        }

        auto painted = Fill(list, GetGeometry(), rect, {0.10f, 0.42f, 0.90f, 0.38f}, layer);
        if (!painted) {
            return painted;
        }

        if (node->kind == Kind::Stack) {
            for (DockSide side : GuideSides) {
                auto result =
                    PaintGuide(list, GetGeometry(), GuideTile(node->bounds, side, TabHeight()),
                               side, insertion_ == MaxSize && side_ == side, layer);
                if (!result) {
                    return result;
                }
            }
        }
    }

    if (showOuterGuides_) {
        for (DockSide side : OuterSides) {
            auto result = PaintGuide(list, GetGeometry(), OuterTile(GetGeometry().size, side), side,
                                     target_ == root_ && side_ == side, layer);
            if (!result) {
                return result;
            }
        }
    }

    return Result<void>::Success();
}

bool DockManager::LocalPoint(Vec2 window, Vec2& local) const noexcept {
    AffineTransform2D inverse;
    if (!GetGeometry().render.TryInverse(inverse)) {
        return false;
    }

    local = inverse.Apply(window);
    return local.IsFinite();
}

void DockManager::UpdateTarget(Vec2 local) noexcept {
    target_ = {};
    insertion_ = MaxSize;
    side_ = DockSide::Center;
    const Vec2 size = GetGeometry().size;
    showOuterGuides_ =
        !root_.IsNull() && nodes_[root_].kind != Kind::Stack && Contains({{}, size}, local);
    if (showOuterGuides_) {
        // 바깥 가이드는 포인터 아래 패널보다 우선하며 창 전체를 분할한다.
        side_ = OuterSide(size, local);
        if (side_ != DockSide::Center) {
            target_ = root_;
            return;
        }
    }

    for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
        const Node& node = *it;
        if (node.kind != Kind::Stack || !Contains(node.bounds, local)) {
            continue;
        }

        target_ = it.Handle();
        if (dragTab_ != MaxSize && tabs_[dragTab_].stack == target_ && node.tabs.Size() == 1) {
            target_ = {};
            return;
        }

        if (local.y < node.bounds.min.y + TabHeight()) {
            insertion_ = node.tabs.Size();
            for (usize i = 0; i < node.tabs.Size(); ++i) {
                const auto& box = tabs_[node.tabs[i]].headerBounds;
                if (local.x < box.min.x + (box.max.x - box.min.x) * 0.5f) {
                    insertion_ = i;
                    break;
                }
            }

            return;
        }

        for (DockSide side : GuideSides) {
            if (Contains(GuideTile(node.bounds, side, TabHeight()), local)) {
                side_ = side;
                return;
            }
        }

        const Vec2 relative = (local - node.bounds.min) / (node.bounds.max - node.bounds.min);
        const float x = math::Min(relative.x, 1.0f - relative.x);
        const float y = math::Min(relative.y, 1.0f - relative.y);
        if (math::Min(x, y) < 0.25f) {
            if (x < y) {
                side_ = relative.x < 0.5f ? DockSide::Left : DockSide::Right;
            } else {
                side_ = relative.y < 0.5f ? DockSide::Top : DockSide::Bottom;
            }
        }

        return;
    }
}

Result<void> DockManager::AcceptDrop(DockManager& source, StringView id,
                                     Vec2 windowPoint) noexcept {
    if (this == &source || !source.IsOpen(id) || !GetGeometry().Contains(windowPoint)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (spawning_ || source.spawning_ || dragTab_ != MaxSize || !dragSplit_.IsNull()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (FindTab(id) != MaxSize) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    Vec2 local;
    if (!LocalPoint(windowPoint, local)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    UpdateTarget(local);
    const auto target = target_;
    const auto insertion = insertion_;
    const auto side = side_;
    EndDrag();
    if (target.IsNull() && !root_.IsNull()) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    return source.Transfer(source.FindTab(id), *this, target, side, insertion);
}

void DockManager::ClearTarget() noexcept {
    if (!target_.IsNull() || showOuterGuides_ || (previewOwner_ && root_.IsNull())) {
        InvalidatePaint();
    }

    target_ = {};
    insertion_ = MaxSize;
    showOuterGuides_ = false;
}

void DockManager::ReleasePreview() noexcept {
    if (previewWindow_ && previewWindow_->previewOwner_ == this) {
        previewWindow_->ClearTarget();
        previewWindow_->previewOwner_ = nullptr;
    }

    previewWindow_.Reset();
}

Vec2 DockManager::TrackTarget(Vec2 point, Vec2 local) noexcept {
    dragPosition_ = point;
    Vec2 destinationPoint = point;
    SharedPtr<DockManager> window;
    if (windowHitTest_.IsBound()) {
        window = windowHitTest_.Execute(point, destinationPoint);
        overDesktop_ = !window;
    } else {
        overDesktop_ = !GetGeometry().Contains(point);
    }

    DockManager* destination = window && window.Get() != this ? window.Get() : nullptr;
    if (previewWindow_.Get() != destination) {
        ReleasePreview();
    }

    DockManager* area = this;
    if (destination || overDesktop_) {
        ClearTarget();
        if (!destination || destination->dragTab_ != MaxSize || !destination->dragSplit_.IsNull() ||
            (destination->previewOwner_ && destination->previewOwner_ != this) ||
            destination->FindTab(tabs_[dragTab_].id.View()) != MaxSize ||
            !destination->LocalPoint(destinationPoint, local)) {
            ReleasePreview();
            return destinationPoint;
        }

        if (!previewWindow_) {
            previewWindow_ = Move(window);
            destination->previewOwner_ = this;
            if (destination->root_.IsNull()) {
                destination->InvalidatePaint();
            }
        }

        area = destination;
    }

    const auto previousTarget = area->target_;
    const auto previousSide = area->side_;
    const auto previousInsertion = area->insertion_;
    const bool hadOuterGuides = area->showOuterGuides_;
    area->UpdateTarget(local);
    if (area->target_ != previousTarget || area->side_ != previousSide ||
        area->insertion_ != previousInsertion || area->showOuterGuides_ != hadOuterGuides) {
        area->InvalidatePaint();
    }

    return destinationPoint;
}

void DockManager::EndDrag() noexcept {
    ReleasePreview();
    ClearTarget();
    previewOwner_ = nullptr;
    overDesktop_ = false;
    dragTab_ = MaxSize;
    dragSplit_ = {};
    dragging_ = false;
}

SlotHandle DockManager::FindSplitter(Vec2 local) const noexcept {
    for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
        if (it->kind != Kind::Stack && Contains(it->handle, local)) {
            return it.Handle();
        }
    }

    return {};
}

input::MouseCursor DockManager::QueryMouseCursor(Vec2 position) const noexcept {
    if (dragTab_ != MaxSize || previewOwner_) {
        return input::MouseCursor::Arrow;
    }

    SlotHandle handle = dragSplit_;
    if (handle.IsNull()) {
        Vec2 local;
        if (LocalPoint(position, local)) {
            handle = FindSplitter(local);
        }
    }

    if (const auto* split = nodes_.Find(handle)) {
        return split->kind == Kind::Horizontal ? input::MouseCursor::ResizeHorizontal
                                               : input::MouseCursor::ResizeVertical;
    }

    return input::MouseCursor::Arrow;
}

PointerReply DockManager::OnPreviewPointerDown(const PointerEvent& event) noexcept {
    Vec2 local;
    if (previewOwner_ || dragTab_ != MaxSize || !dragSplit_.IsNull() ||
        !LocalPoint(event.position, local)) {
        return {};
    }

    const bool middle =
        event.source == PointerSource::Mouse && event.button == input::MouseButton::Middle;
    if (!middle && event.source == PointerSource::Mouse &&
        event.button != input::MouseButton::Left) {
        return {};
    }

    if (!middle) {
        dragSplit_ = FindSplitter(local);
    }

    if (const auto* split = nodes_.Find(dragSplit_)) {
        handleOffset_ = split->kind == Kind::Horizontal ? local.x - split->handle.min.x
                                                        : local.y - split->handle.min.y;
    } else {
        for (const Node& node : nodes_) {
            for (usize i : node.tabs) {
                if (!Contains(tabs_[i].headerBounds, local)) {
                    continue;
                }

                if (middle) {
                    return {.handled = static_cast<bool>(CloseTab(tabs_[i].id.View()))};
                }

                (void)ActivateTab(tabs_[i].id.View());
                dragTab_ = i;
                break;
            }

            if (dragTab_ != MaxSize) {
                break;
            }
        }
    }

    if (dragTab_ == MaxSize && dragSplit_.IsNull()) {
        return {};
    }

    source_ = event.source;
    user_ = event.userId;
    pointer_ = event.pointerId;
    press_ = local;
    ++gesture_;
    return {.handled = true, .capture = true};
}

PointerReply DockManager::OnPointerEvent(const PointerEvent& event) noexcept {
    if ((dragTab_ == MaxSize && dragSplit_.IsNull()) || source_ != event.source ||
        user_ != event.userId || pointer_ != event.pointerId) {
        return {};
    }

    if (event.type == PointerEventType::Up && source_ == PointerSource::Mouse &&
        event.button != input::MouseButton::Left) {
        return {};
    }

    Vec2 local;
    if (event.type == PointerEventType::Cancel || !LocalPoint(event.position, local)) {
        EndDrag();
        return {.handled = true, .releaseCapture = true};
    }

    if (event.type != PointerEventType::Move && event.type != PointerEventType::Up) {
        return {.handled = true};
    }

    if (Node* split = nodes_.Find(dragSplit_)) {
        const bool horizontal = split->kind == Kind::Horizontal;
        const Vec2 size = split->bounds.max - split->bounds.min;
        const float available = (horizontal ? size.x : size.y) - Divider;
        if (available > 0) {
            const float pos =
                horizontal ? local.x - split->bounds.min.x : local.y - split->bounds.min.y;
            const float fraction = math::Clamp((pos - handleOffset_) / available, 0.05f, 0.95f);
            if (split->fraction != fraction) {
                split->fraction = fraction;
                InvalidateArrangement();
            }
        }
    } else if (dragTab_ != MaxSize) {
        if (math::Max(math::Abs(local.x - press_.x), math::Abs(local.y - press_.y)) >=
            DragThreshold) {
            dragging_ = true;
        }

        if (dragging_) {
            const Vec2 destinationPoint = TrackTarget(event.position, local);
            if (event.type == PointerEventType::Up && previewWindow_) {
                auto destination = previewWindow_;
                const usize tab = dragTab_;
                EndDrag();
                (void)destination->AcceptDrop(*this, tabs_[tab].id.View(), destinationPoint);
                return {.handled = true, .releaseCapture = true};
            }
        }
    }

    if (event.type == PointerEventType::Up) {
        if (dragging_ && !target_.IsNull()) {
            (void)Place(dragTab_, target_, side_, insertion_);
        } else if (dragging_ && dragTab_ != MaxSize && overDesktop_ && externalDrop_.IsBound()) {
            const usize tab = dragTab_;
            externalDrop_.Execute(tabs_[tab].id.View(), event.position);
            EndDrag();
            return {.handled = true, .releaseCapture = true};
        }

        EndDrag();
        return {.handled = true, .releaseCapture = true};
    }

    return {.handled = true};
}

void DockManager::OnPointerCaptureLost(PointerSource source, uint32 user, uint32 pointer) noexcept {
    if (source_ == source && user_ == user && pointer_ == pointer) {
        EndDrag();
    }
}

} // namespace rocks::ui
