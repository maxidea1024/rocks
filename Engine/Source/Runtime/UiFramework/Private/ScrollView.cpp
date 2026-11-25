#include <UiFramework/ScrollView.h>
#include "TouchPan.h"

namespace rocks::ui {

ScrollView::ScrollView(Allocator& allocator, StackDirection direction) noexcept
    : Element(allocator), direction_(direction) {
    SetClipToBounds(true);
}

Result<void> ScrollView::Construct(const Args& args) noexcept {
    direction_ = args.direction;
    return args.content.AttachTo(*this);
}

Result<void> ScrollView::TryAddChild(const SharedPtr<Element>& child) noexcept {
    if (ChildCount() != 0) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }
    return Element::TryAddChild(child);
}

Vec2 ScrollView::ComputeDesiredSize(float) const noexcept {
    const Element* child = ChildAt(0);
    return child && child->GetVisibility() != Visibility::Collapsed ? child->GetDesiredSize()
                                                                    : Vec2{};
}

Result<void> ScrollView::Arrange(const Geometry& geometry) noexcept {
    if (!geometry.size.IsFinite() || geometry.size.x < 0 || geometry.size.y < 0 ||
        (direction_ != StackDirection::Horizontal && direction_ != StackDirection::Vertical)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    SetGeometry(geometry);
    arranged_ = true;
    Element* child = ChildAt(0);
    if (!child || child->GetVisibility() == Visibility::Collapsed) {
        maxOffset_ = 0;
        offset_ = 0;
        return Result<void>::Success();
    }

    const bool vertical = direction_ == StackDirection::Vertical;
    const Vec2 desired = child->GetDesiredSize();
    const float viewport = vertical ? geometry.size.y : geometry.size.x;
    const float content = math::Max(viewport, vertical ? desired.y : desired.x);
    const float before = padBefore_ ? viewport : 0;
    maxOffset_ = math::Max(0.0f,(vertical ? desired.y : desired.x) + before + (padAfter_ ? viewport : 0) - viewport);
    offset_ = endRequested_ ? maxOffset_ : math::Clamp(offset_, 0.0f, maxOffset_);
    endRequested_ = false;
    const Vec2 size = vertical ? Vec2(geometry.size.x, content) : Vec2(content, geometry.size.y);
    const AffineTransform2D translation{
        1, 0, 0, 1, vertical ? 0 : before - offset_, vertical ? before - offset_ : 0};
    return child->ArrangeIfNeeded(GetGeometry().MakeChild(size, translation, {}, {}));
}

PointerReply ScrollView::OnPreviewPointerDown(const PointerEvent& event) noexcept {
    if (maxOffset_ <= 0 || dragging_ || (event.source==PointerSource::Touch && !touchScrolling_) ||
        (event.source == PointerSource::Mouse && event.button != input::MouseButton::Left)) {
        return {};
    }
    Vec2 position;
    Vec2 size;
    Vec2 local;
    if (!GetThumb(position, size) || !ToLocal(event.position, local) || local.x < position.x ||
        local.y < position.y || local.x >= position.x + size.x || local.y >= position.y + size.y) {
        return {};
    }
    dragging_ = true;
    draggingContent_=false;
    dragButton_=event.button;
    dragSource_ = event.source;
    dragUser_ = event.userId;
    dragPointer_ = event.pointerId;
    dragPosition_ = direction_ == StackDirection::Vertical ? local.y : local.x;
    dragOffset_ = offset_;
    return {.handled = true, .capture = true};
}

PointerReply ScrollView::OnTouchPan(const PointerEvent& move, Vec2 origin) noexcept {
    if (dragging_ || !touchScrolling_) { return {}; }
    const bool vertical = direction_ == StackDirection::Vertical;
    float start = 0, current = 0;
    if (!TryTouchPan(GetGeometry(), vertical, origin, move.position, offset_, maxOffset_, start, current)) { return {}; }
    dragging_ = true;
    draggingContent_ = true;
    dragSource_ = move.source;
    dragUser_ = move.userId;
    dragPointer_ = move.pointerId;
    dragButton_ = move.button;
    dragPosition_ = start;
    dragOffset_ = offset_;
    SetUserOffset(dragOffset_ + start - current);
    return {.handled = true, .capture = true};
}

PointerReply ScrollView::OnPointerEvent(const PointerEvent& event) noexcept {
    if (!dragging_ && event.type == PointerEventType::Down && event.source == PointerSource::Touch) {
        if (touchScrolling_ && maxOffset_ > 0) { return {.handled = true, .capture = true, .allowTouchPan = true}; }
        return {};
    }
    if (event.type == PointerEventType::Up && event.source == PointerSource::Mouse &&
        event.button != dragButton_) {
        return {};
    }
    if (!dragging_ && event.type==PointerEventType::Down && maxOffset_>0 &&
        (event.source==PointerSource::Mouse && event.button==input::MouseButton::Right && rightDragScrolling_)) {
        Vec2 local;
        if (!ToLocal(event.position,local)) {return {};}
        dragging_=true;draggingContent_=true;dragSource_=event.source;dragUser_=event.userId;dragPointer_=event.pointerId;dragButton_=event.button;
        dragPosition_=direction_==StackDirection::Vertical ? local.y : local.x;
        dragOffset_=offset_;
        return {.handled=true,.capture=true};
    }
    if (dragging_ && dragSource_ == event.source && dragUser_ == event.userId &&
        dragPointer_ == event.pointerId) {
        if (event.type == PointerEventType::Move || event.type == PointerEventType::Up) {
            Vec2 position;
            Vec2 size;
            Vec2 local;
            if (draggingContent_ && ToLocal(event.position,local)) {
                const auto current=direction_==StackDirection::Vertical ? local.y : local.x;
                SetUserOffset(dragOffset_+dragPosition_-current);
            } else if (GetThumb(position, size) && ToLocal(event.position, local)) {
                const bool vertical = direction_ == StackDirection::Vertical;
                Vec2 trackPosition, trackSize;
                (void)GetTrack(trackPosition,trackSize);
                const float track = vertical ? trackSize.y-size.y : trackSize.x-size.x;
                if (track > 0) {
                    const float current = vertical ? local.y : local.x;
                    SetUserOffset(dragOffset_ +
                                          (current - dragPosition_) * maxOffset_ / track);
                }
            }
        }
        if (event.type == PointerEventType::Up || event.type == PointerEventType::Cancel) {
            dragging_ = false;
            return {.handled = true, .releaseCapture = true};
        }
        return {.handled = true};
    }
    if (event.type == PointerEventType::Wheel && wheelPolicy_!=ConsumeMouseWheel::Never && math::IsFinite(event.wheel)) {
        const float next = math::Clamp(offset_ - event.wheel * (48.0f*wheelMultiplier_), 0.0f, maxOffset_);
        if (next != offset_) {
            SetUserOffset(next);
            return {.handled = true};
        }
        return {.handled=wheelPolicy_==ConsumeMouseWheel::Always};
    }
    return {};
}

void ScrollView::OnPointerCaptureLost(PointerSource source, uint32 userId,
                                      uint32 pointerId) noexcept {
    if (dragging_ && dragSource_ == source && dragUser_ == userId && dragPointer_ == pointerId) {
        dragging_ = false;
    }
}

Result<void> ScrollView::OnPaintOverlay(PaintList& list, int32 layer) const noexcept {
    Vec2 position;
    Vec2 size;
    if (alwaysShowTrack_ && GetTrack(position,size)) {
        const auto geometry=GetGeometry().MakeChild(size,{1,0,0,1,position.x,position.y},{},{});
        auto track=list.TryAddBox({geometry,{0.12f,0.12f,0.12f,0.8f},layer});
        if (!track) {return track;}
    }
    if (!GetThumb(position, size)) {
        return Result<void>::Success();
    }
    const AffineTransform2D translation{1, 0, 0, 1, position.x, position.y};
    return list.TryAddBox({GetGeometry().MakeChild(size, translation, {}, {}),
                           LinearColor(0.5f, 0.5f, 0.5f, 0.8f), layer});
}

bool ScrollView::GetTrack(Vec2& position,Vec2& size) const noexcept {
    if (!showBar_ || thickness_<=0) {return false;}
    const auto view=GetGeometry().size;
    const float width=view.x-barPadding_.x-barPadding_.z;
    const float height=view.y-barPadding_.y-barPadding_.w;
    if (width<=0 || height<=0) {return false;}
    if (direction_==StackDirection::Vertical) {
        size={math::Min(thickness_,width),height};
        position={view.x-barPadding_.z-size.x,barPadding_.y};
    } else {
        size={width,math::Min(thickness_,height)};
        position={barPadding_.x,view.y-barPadding_.w-size.y};
    }
    return true;
}
bool ScrollView::GetThumb(Vec2& position, Vec2& size) const noexcept {
    Vec2 trackPosition,viewport;
    if (!GetTrack(trackPosition,viewport)) {return false;}
    const bool vertical = direction_ == StackDirection::Vertical;
    const float length = vertical ? viewport.y : viewport.x;
    const float cross = vertical ? viewport.x : viewport.y;
    if (!showBar_ || thickness_<=0 || (maxOffset_ <= 0 && !alwaysShowBar_) || length <= 0 || cross <= 0) {
        return false;
    }
    const float proportional = length * GetViewFraction();
    const float thumbLength = math::Min(length, math::Max(16.0f, proportional));
    const float thickness = math::Min(thickness_, cross);
    const float start = maxOffset_>0 ? (length - thumbLength) * offset_ / maxOffset_ : 0;
    position = trackPosition + (vertical ? Vec2(0,start) : Vec2(start,0));
    size = vertical ? Vec2(thickness, thumbLength) : Vec2(thumbLength, thickness);
    return true;
}

bool ScrollView::ToLocal(Vec2 position, Vec2& local) const noexcept {
    AffineTransform2D inverse;
    if (!GetGeometry().render.TryInverse(inverse)) {
        return false;
    }
    local = inverse.Apply(position);
    return local.IsFinite();
}

Result<void> ScrollView::SetScrollOffset(float offset) noexcept {
    if (!math::IsFinite(offset)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    endRequested_=false;
    const float next = arranged_ ? math::Clamp(offset, 0.0f, maxOffset_) : math::Max(0.0f, offset);
    if (offset_ != next) {
        offset_ = next;
        InvalidateArrangement();
    }
    return Result<void>::Success();
}

float ScrollView::GetScrollOffset() const noexcept {
    return offset_;
}

float ScrollView::GetMaxScrollOffset() const noexcept {
    return maxOffset_;
}

} // namespace rocks::ui
