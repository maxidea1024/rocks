#include <UiCore/Element.h>
#include <UiCore/CacheRoot.h>
#include <UiCore/Styles/CoreStyles.h>
#include <Base/NumericLimits.h>

namespace rocks::ui {

Element::Element(Allocator& allocator) noexcept : children_(allocator) {
}

Element::~Element() noexcept {
    for (usize i = 0; i < children_.Size(); ++i) {
        children_[i]->parent_ = nullptr;
    }
}

Result<void> Element::TryAddChild(const SharedPtr<Element>& child) noexcept {
    return TryInsertChild(children_.Size(), child);
}

void Element::MoveChildWithin(usize from, usize to) noexcept {
    if (from == to || from >= children_.Size() || to >= children_.Size()) {
        return;
    }

    auto held = Move(children_[from]);
    while (from != to) {
        const usize next = from < to ? from + 1 : from - 1;
        children_[from] = Move(children_[next]);
        from = next;
    }

    children_[to] = Move(held);
    InvalidateLayout();
}

Result<void> Element::TryInsertChild(usize index, const SharedPtr<Element>& child) noexcept {
    if (index > children_.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!child || child->parent_ != nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (Element* current = this; current != nullptr; current = current->parent_) {
        if (current == child.Get()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    auto added = children_.TryInsert(child, index);
    if (!added) {
        return added;
    }

    child->parent_ = this;
    if (child->subtreeBindings_) {
        for (auto* ancestor = this; ancestor; ancestor = ancestor->parent_) {
            ancestor->subtreeBindings_ += child->subtreeBindings_;
        }
    }

    InvalidateLayout();
    return Result<void>::Success();
}

Result<void> Element::AttachPopupLayer(const PopupRequest& popup) noexcept {
    return parent_ ? parent_->AttachPopupLayer(popup) : Result<void>::Failure(ErrorCode::NotFound);
}

bool Element::IsHostedPopupLayer(const Element&) const noexcept {
    return false;
}

Optional<Vec2> Element::PopupWorkAreaPoint() const noexcept {
    return {};
}

void Element::SetPopupArea(const Optional<Box2<float>>&) noexcept {
}

void Element::DismissHostedPopups() noexcept {
}

void Element::OnDescendantDetached(const Element&) noexcept {
}

bool Element::RemoveChild(Element& child) noexcept {
    for (usize i = 0; i < children_.Size(); ++i) {
        if (children_[i].Get() == &child) {
            if (child.subtreeBindings_) {
                for (auto* ancestor = this; ancestor; ancestor = ancestor->parent_) {
                    ancestor->subtreeBindings_ -= child.subtreeBindings_;
                }
            }

            auto detached = Move(children_[i]);
            child.parent_ = nullptr;
            (void)children_.TryRemoveAt(i);
            InvalidateLayout();
            for (auto* host = this; host; host = host->parent_) {
                host->OnDescendantDetached(*detached);
            }

            return true;
        }
    }

    return false;
}

Result<void> Element::WrapAttachedChild(Element& child,
                                        const SharedPtr<Element>& wrapper) noexcept {
    auto* parent = child.parent_;
    if (!parent || !wrapper || wrapper->parent_ || wrapper->ChildCount() ||
        parent->IsWithin(*wrapper) || wrapper.Get() == &child) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (auto& entry : parent->children_) {
        if (entry.Get() != &child) {
            continue;
        }

        auto attached = wrapper->children_.TryPushBack(entry);
        if (!attached) {
            return attached;
        }

        for (auto* ancestor = parent; ancestor; ancestor = ancestor->parent_) {
            ancestor->subtreeBindings_ += wrapper->subtreeBindings_;
        }

        wrapper->subtreeBindings_ += child.subtreeBindings_;
        child.parent_ = wrapper.Get();
        wrapper->parent_ = parent;
        entry = wrapper;
        parent->InvalidateLayout();
        return Result<void>::Success();
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

bool Element::IsWithin(const Element& root) const noexcept {
    for (const auto* current = this; current; current = current->Parent()) {
        if (current == &root) {
            return true;
        }
    }

    return false;
}

bool Element::IsInputPathActive(const Element& root, bool pointer) const noexcept {
    for (const Element* current = this; current; current = current->parent_) {
        const auto visibility = current->visibility_;
        if (!current->enabled_ || visibility == Visibility::Hidden ||
            visibility == Visibility::Collapsed ||
            (pointer && visibility == Visibility::HitTestInvisible)) {
            return false;
        }

        if (current == &root) {
            return true;
        }

        const auto* parent = current->parent_;
        if (parent) {
            const auto active = parent->ActiveChildren();
            if (active.Size() != parent->ChildCount()) {
                bool found = false;
                for (const auto& child : active) {
                    if (child.Get() == current) {
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    return false;
                }
            }
        }
    }

    return false;
}

Element* Element::Parent() const noexcept {
    return parent_;
}

usize Element::ChildCount() const noexcept {
    return children_.Size();
}

Element* Element::ChildAt(usize index) const noexcept {
    return index < children_.Size() ? children_[index].Get() : nullptr;
}

Span<const SharedPtr<Element>> Element::ActiveChildren() const noexcept {
    return children_.View();
}

float Element::RelativeLayoutScale() const noexcept {
    return 1.0f;
}

float Element::MeasuredLayoutScale() const noexcept {
    return measuredScale_;
}

SharedPtr<Element> Element::ChildRefAt(usize index) const noexcept {
    return index < children_.Size() ? children_[index] : SharedPtr<Element>();
}

SharedPtr<Element> Element::FindChild(const Element* child) const noexcept {
    for (usize i = 0; i < children_.Size(); ++i) {
        if (children_[i].Get() == child) {
            return children_[i];
        }
    }

    return {};
}

PointerReply Element::OnTouchPan(const PointerEvent&, Vec2) noexcept {
    return {};
}

void Element::OnPointerReleaseInPath(const PointerEvent&) noexcept {
}

PointerReply Element::OnPreviewPointerDown(const PointerEvent&) noexcept {
    return {};
}

input::WindowRegion Element::GetWindowRegion() const noexcept {
    return input::WindowRegion::Client;
}

void Element::SetCursorOverride(input::MouseCursor cursor) noexcept {
    cursorOverride_ = static_cast<uint8>(cursor);
}

void Element::ResetCursorOverride() noexcept {
    cursorOverride_ = NoCursorOverride;
}

input::MouseCursor Element::ResolveMouseCursor(Vec2 position) const noexcept {
    for (auto* node = this; node; node = node->Parent()) {
        if (node->cursorOverride_ != NoCursorOverride) {
            return static_cast<input::MouseCursor>(node->cursorOverride_);
        }

        auto cursor = node->QueryMouseCursor(position);
        if (cursor != input::MouseCursor::Arrow) {
            return cursor;
        }
    }

    return input::MouseCursor::Arrow;
}

void Element::OnMouseEnter(uint32) noexcept {
}

void Element::OnMouseLeave(uint32) noexcept {
}

input::MouseCursor Element::QueryMouseCursor(Vec2) const noexcept {
    return input::MouseCursor::Arrow;
}

PointerReply Element::OnPointerEvent(const PointerEvent&) noexcept {
    return {};
}

void Element::OnPointerCaptureLost(PointerSource, uint32, uint32) noexcept {
}

bool Element::SupportsKeyboardFocus() const noexcept {
    return false;
}

bool Element::OnPreviewKeyChord(uint32 user, const input::InputEvent& event, KeyChord) noexcept {
    return OnPreviewKeyDown(user, event);
}

bool Element::OnGamepadButtonDown(uint32, input::GamepadButton) noexcept {
    return false;
}

bool Element::OnPreviewKeyDown(uint32, const input::InputEvent&) noexcept {
    return false;
}

bool Element::OnKeyEvent(uint32, const input::InputEvent&) noexcept {
    return false;
}

void Element::OnKeyCancelled(uint32, input::Key) noexcept {
}

void Element::OnFocusReceived(uint32) noexcept {
}

void Element::OnFocusLost(uint32) noexcept {
}

Result<void> Element::OnPaint(PaintList&, int32) const noexcept {
    return Result<void>::Success();
}

void Element::SetFocusOutlineVisible(bool visible) noexcept {
    const bool before = focusOutlines_ != 0;
    if (visible) {
        focusOutlines_ = focusOutlines_ == NumericLimits<uint8>::Max()
                             ? focusOutlines_
                             : static_cast<uint8>(focusOutlines_ + 1);
    } else if (focusOutlines_ != 0) {
        --focusOutlines_;
    }

    if (before != (focusOutlines_ != 0)) {
        InvalidatePaint();
    }
}

Vec2 Element::MeasureForWidth(float) noexcept {
    return GetDesiredSize();
}

Optional<bool> Element::OnQueryShowFocus(FocusCause) const noexcept {
    return {};
}

bool Element::IsFocusOutlineVisible() const noexcept {
    return focusOutlines_ != 0;
}

const StyleBrush* Element::FocusOutlineBrush() const noexcept {
    return &FocusOutlineStyle();
}

Result<void> Element::PaintOverlayPass(PaintList& list, int32 layer) const noexcept {
    auto overlay = OnPaintOverlay(list, layer);
    if (!overlay || focusOutlines_ == 0) {
        return overlay;
    }

    const StyleBrush* brush = FocusOutlineBrush();
    if (!brush || brush->mode == BrushMode::None) {
        return overlay;
    }

    // 내용 위에 요소 크기 그대로 그린다(UE SWidget::Paint의 초점 brush).
    return list.TryAddBrush(geometry_, brush->Resolve(list.Theme(), list.ColorContext()),
                            LinearColor{1, 1, 1, 1}, layer);
}

Result<void> Element::OnPaintOverlay(PaintList&, int32) const noexcept {
    return Result<void>::Success();
}

Result<void> Element::Paint(PaintList& list, int32 layer) const noexcept {
    int32 highest = layer;
    return PaintSubtree(list, layer, highest, nullptr, false);
}

bool Element::StartsPaintLayer(usize, usize) const noexcept {
    return false;
}

CacheRoot* Element::LocalPaintCache() const noexcept {
    return nullptr;
}

Result<void> Element::PaintSubtree(PaintList& list, int32 layer, int32& highest, CacheRoot* cache,
                                   bool inheritedVolatile) const noexcept {
    highest = layer;
    if (visibility_ == Visibility::Hidden || visibility_ == Visibility::Collapsed) {
        return Result<void>::Success();
    }

    if (!cache && !inheritedVolatile) {
        if (auto* local = LocalPaintCache()) {
            // 지역 캐시도 바깥 테마·전경색으로 기록한다(달라지면 Update가 전체를 다시 만든다).
            if (&local->scratch_.Theme() != &list.Theme() ||
                local->scratch_.ThemeRevision() != list.ThemeRevision()) {
                local->SetTheme(&list.Theme(), list.ThemeRevision());
            }

            // 벡터 brush resolver도 바깥 목록의 것을 쓴다(바뀌면 지역 기록을 다시 만든다 — 17.2절).
            if (local->scratch_.GetVectorImageResolver() != list.GetVectorImageResolver()) {
                local->SetVectorImageResolver(list.GetVectorImageResolver());
            }

            auto updated = local->Update(*this, layer, list.foreground_, list.disabled_);
            if (!updated) {
                return updated;
            }

            highest = local->HighestLayer();
            return list.TryAppendRecorded(local->List().Boxes());
        }
    }

    if (clipToBounds_) {
        auto clipped = list.PushClip(geometry_);
        if (!clipped) {
            return clipped;
        }
    }

    const usize entry = cache ? cache->next_.Size() : 0;
    const float parentOpacity = list.opacity_;
    const LinearColor parentForeground = list.foreground_;
    const bool parentDisabled = list.disabled_;
    list.opacity_ *= renderOpacity_;
    list.disabled_ = parentDisabled || !enabled_;
    list.foreground_ = ResolveForeground(list.Theme(), parentForeground, list.disabled_);
    auto result = PaintContents(list, layer, highest, cache, inheritedVolatile || IsVolatile());
    list.opacity_ = parentOpacity;
    list.foreground_ = parentForeground;
    list.disabled_ = parentDisabled;
    if (cache && result) {
        cache->next_[entry].subtreeEnd = cache->next_.Size();
    }

    if (clipToBounds_) {
        list.PopClip();
    }

    return result;
}

Span<const SharedPtr<Element>> Element::PaintChildren() const noexcept {
    return ActiveChildren();
}

Result<void> Element::PaintContents(PaintList& list, int32 layer, int32& highest, CacheRoot* cache,
                                    bool inheritedVolatile) const noexcept {
    auto includeLayers = [&](usize first) noexcept {
        const auto boxes = list.Boxes();
        for (usize i = first; i < boxes.Size(); ++i) {
            highest = highest > boxes[i].layer ? highest : boxes[i].layer;
        }
    };

    const usize body = cache ? cache->next_.Size() : 0;
    const usize first = list.Boxes().Size();
    auto painted = cache ? cache->Record(*this, layer, false, inheritedVolatile, highest)
                         : OnPaint(list, layer);
    if (!painted) {
        return painted;
    }

    if (!cache) {
        includeLayers(first);
    }

    const auto children = PaintChildren();
    usize previous = children.Size();
    for (usize i = 0; i < children.Size(); ++i) {
        const Visibility visibility = children[i]->GetVisibility();
        if (visibility == Visibility::Hidden || visibility == Visibility::Collapsed) {
            continue;
        }

        int32 childLayer = highest;
        if (previous != children.Size() && StartsPaintLayer(previous, i)) {
            if (childLayer == NumericLimits<int32>::Max()) {
                return Result<void>::Failure(ErrorCode::Overflow);
            }

            ++childLayer;
        }

        const usize childBody = cache ? cache->next_.Size() : 0;
        auto result =
            children[i]->PaintSubtree(list, childLayer, highest, cache, inheritedVolatile);
        if (!result) {
            return result;
        }

        if (cache) {
            cache->next_[childBody].parentBody = body;
            cache->next_[cache->next_.Size() - 1].parentBody = body;
        }

        previous = i;
    }

    const usize overlayStart = list.Boxes().Size();
    auto overlay = cache ? cache->Record(*this, highest, true, inheritedVolatile, highest)
                         : PaintOverlayPass(list, highest);
    if (!overlay) {
        return overlay;
    }

    if (!cache) {
        includeLayers(overlayStart);
    }

    return Result<void>::Success();
}

uint64 Element::PaintPlanRevision() const noexcept {
    return planRevision_;
}

WidgetSurface* Element::GetWidgetSurface() noexcept {
    return nullptr;
}

void Element::SetGeometry(const Geometry& geometry) noexcept {
    const auto resolved =
        hasRenderTransform_ ? geometry.MakeChild(geometry.size, {}, renderTransform_, renderPivot_)
                            : geometry;
    if (geometry_ != resolved) {
        geometry_ = resolved;
        InvalidatePaintPlan();
    }

    arrangeDirty_ = true;
}

Result<void> Element::SetRenderTransform(const AffineTransform2D& transform, Vec2 pivot) noexcept {
    if (!pivot.IsFinite() || !math::IsFinite(transform.m00) || !math::IsFinite(transform.m01) ||
        !math::IsFinite(transform.m10) || !math::IsFinite(transform.m11) ||
        !math::IsFinite(transform.tx) || !math::IsFinite(transform.ty)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (hasRenderTransform_ && transform == renderTransform_ && pivot == renderPivot_) {
        return Result<void>::Success();
    }

    InvalidateArrangement();
    renderTransform_ = transform;
    renderPivot_ = pivot;
    hasRenderTransform_ = true;
    return Result<void>::Success();
}

void Element::ClearRenderTransform() noexcept {
    if (!hasRenderTransform_) {
        return;
    }

    hasRenderTransform_ = false;
    InvalidateArrangement();
}

Result<void> Element::Arrange(const Geometry& geometry) noexcept {
    SetGeometry(geometry);
    return Result<void>::Success();
}

Result<void> Element::Prepass(float scale) noexcept {
    if (!math::IsFinite(scale) || scale <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!layoutDirty_ && !measureChildren_ && measuredScale_ == scale) {
        return Result<void>::Success();
    }

    arrangeDirty_ = true;
    if (scale != measuredScale_) {
        layoutDirty_ = true;
        InvalidatePaintPlan();
    }

    if (visibility_ == Visibility::Collapsed) {
        desiredSize_ = {};
        measuredScale_ = scale;
        layoutDirty_ = false;
        measureChildren_ = false;
        return Result<void>::Success();
    }

    const float childScale = scale * RelativeLayoutScale();
    for (const auto& child : ActiveChildren()) {
        auto measured = child->Prepass(childScale);
        if (!measured) {
            return measured;
        }

        // FitBox처럼 자식을 다른 배율로도 측정하는 경우가 있어 부모가 마지막으로 본 값과 비교한다.
        if (child->desiredSeenByParent_ != child->desiredSize_) {
            child->desiredSeenByParent_ = child->desiredSize_;
            layoutDirty_ = true;
        }
    }

    measureChildren_ = false;
    if (!layoutDirty_) {
        return Result<void>::Success();
    }

    const Vec2 desired = ComputeDesiredSize(scale);
    if (!desired.IsFinite() || desired.x < 0 || desired.y < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    desiredSize_ = desired;
    measuredScale_ = scale;
    layoutDirty_ = false;
    return Result<void>::Success();
}

void Element::InvalidateLayout() noexcept {
    layoutDirty_ = true;
    for (Element* current = parent_; current; current = current->parent_) {
        current->measureChildren_ = true;
    }

    InvalidateArrangement();
    InvalidatePaintPlan();
}

Result<void> Element::ArrangeIfNeeded(const Geometry& geometry) noexcept {
    if (!arrangeDirty_ && allotted_ == geometry) {
        return Result<void>::Success();
    }

    auto result = Arrange(geometry);
    if (result) {
        allotted_ = geometry;
        arrangeDirty_ = false;
    }

    return result;
}

void Element::InvalidatePaint() noexcept {
    ++paintRevision_;
    for (auto* p = this; p; p = p->parent_) {
        ++p->subtreePaintRevision_;
    }
}

void Element::InvalidatePaintPlan() noexcept {
    for (auto* p = this; p; p = p->parent_) {
        ++p->planRevision_;
    }
}

void Element::InvalidateArrangement() noexcept {
    for (auto* p = this; p; p = p->parent_) {
        p->arrangeDirty_ = true;
    }

    InvalidatePaint();
}

void Element::SetVolatile(bool enabled) noexcept {
    if (volatile_ != enabled) {
        volatile_ = enabled;
        InvalidatePaintPlan();
    }
}

Result<void> Element::SetRenderOpacity(float opacity) noexcept {
    if (!math::IsFinite(opacity) || opacity < 0 || opacity > 1) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (renderOpacity_ != opacity) {
        renderOpacity_ = opacity;
        // 캐시는 부모 본문을 통해 누적 알파가 달라진 자식만 다시 기록한다.
        InvalidatePaint();
    }

    return Result<void>::Success();
}

float Element::GetRenderOpacity() const noexcept {
    return renderOpacity_;
}

Result<void> Element::SetForegroundColor(ColorRef color) noexcept {
    if (!color.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (foregroundColor_ != color) {
        foregroundColor_ = color;
        // 캐시는 부모 본문을 통해 상속 전경색이 달라진 자식만 다시 기록한다.
        InvalidatePaint();
    }

    return Result<void>::Success();
}

ColorRef Element::GetForegroundColor() const noexcept {
    return foregroundColor_;
}

ColorRef Element::PaintForegroundColor(bool) const noexcept {
    return GetForegroundColor();
}

LinearColor Element::ResolveForeground(const ThemeColors& theme, LinearColor inherited,
                                       bool disabled) const noexcept {
    return PaintForegroundColor(disabled).Resolve(theme, {inherited, inherited});
}

void Element::SetForceVolatile(bool enabled) noexcept {
    if (forceVolatile_ != enabled) {
        forceVolatile_ = enabled;
        InvalidatePaintPlan();
    }
}

bool Element::IsVolatile() const noexcept {
    return volatile_ || forceVolatile_;
}

Vec2 Element::GetDesiredSize() const noexcept {
    return desiredSize_;
}

Vec2 Element::ComputeDesiredSize(float) const noexcept {
    return {};
}

const Geometry& Element::GetGeometry() const noexcept {
    return geometry_;
}

void Element::SetVisibility(Visibility visibility) noexcept {
    if (visibility_ == visibility) {
        return;
    }

    const bool collapsedChanged =
        (visibility_ == Visibility::Collapsed) != (visibility == Visibility::Collapsed);
    visibility_ = visibility;
    if (collapsedChanged && parent_) {
        // 크기가 0인 자식도 접으면 슬롯 간격과 줄바꿈에서 빠진다.
        parent_->InvalidateLayout();
    }

    InvalidateLayout();
}

Visibility Element::GetVisibility() const noexcept {
    return visibility_;
}

void Element::SetEnabled(bool enabled) noexcept {
    if (enabled_ == enabled) {
        return;
    }

    enabled_ = enabled;
    InvalidatePaintPlan();
}

bool Element::IsEnabled() const noexcept {
    return enabled_;
}

void Element::SetClipToBounds(bool enabled) noexcept {
    if (clipToBounds_ == enabled) {
        return;
    }

    clipToBounds_ = enabled;
    InvalidatePaintPlan();
}

bool Element::ClipsToBounds() const noexcept {
    return clipToBounds_;
}

Element* Element::HitTest(Vec2 position) noexcept {
    if (!enabled_ || visibility_ == Visibility::Hidden || visibility_ == Visibility::Collapsed ||
        visibility_ == Visibility::HitTestInvisible) {
        return nullptr;
    }

    if (clipToBounds_ && !geometry_.Contains(position)) {
        return nullptr;
    }

    const auto children = ActiveChildren();
    for (usize i = children.Size(); i > 0; --i) {
        if (Element* hit = children[i - 1]->HitTest(position)) {
            return hit;
        }
    }

    return visibility_ == Visibility::Visible && geometry_.Contains(position) ? this : nullptr;
}

} // namespace rocks::ui
