#include <UiCore/InputMetrics.h>
#include <UiFramework/Scrolling/ScrollView.h>
#include <UiCore/UiHostServices.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Application/UiTimer.h>
#include <UiFramework/Scrolling/ScrollBar.h>
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
    Vec2 desired =
        child && child->GetVisibility() != Visibility::Collapsed ? child->GetDesiredSize() : Vec2{};
    // Reserve 칸을 더한다 — 넘침은 지난 배치 값이다(UE도 지난 프레임 막대 보임으로 정한다, 23.6절).
    const float reserved = ReservedBarExtent(overflowing_);
    if (direction_ == StackDirection::Vertical) {
        desired.x += reserved;
    } else {
        desired.y += reserved;
    }

    return desired;
}

float ScrollView::ReservedBarExtent(bool overflowing) const noexcept {
    if (!showBar_ || thickness_ <= 0 || GetScrollBarPlacement() != ScrollBarPlacement::Reserve ||
        (!overflowing && visibility_ != ScrollBarVisibility::Always)) {
        return 0;
    }

    return direction_ == StackDirection::Vertical ? thickness_ + barPadding_.x + barPadding_.z
                                                  : thickness_ + barPadding_.y + barPadding_.w;
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
        overflowing_ = false;
        return Result<void>::Success();
    }

    const bool vertical = direction_ == StackDirection::Vertical;
    const Vec2 desired = child->GetDesiredSize();
    const float viewport = vertical ? geometry.size.y : geometry.size.x;
    const float content = math::Max(viewport, vertical ? desired.y : desired.x);
    const float before = padBefore_ ? viewport : 0;
    maxOffset_ = math::Max(0.0f, (vertical ? desired.y : desired.x) + before +
                                     (padAfter_ ? viewport : 0) - viewport);
    offset_ = endRequested_ ? maxOffset_ : math::Clamp(offset_, 0.0f, maxOffset_);
    endRequested_ = false;
    // 넘침이 바뀌면 원하는 크기(Reserve 칸)가 바뀐다 — 부모가 다시 재게 한다.
    const bool overflowing = maxOffset_ > 0;
    if (overflowing_ != overflowing) {
        overflowing_ = overflowing;
        if (GetScrollBarPlacement() == ScrollBarPlacement::Reserve) {
            InvalidateLayout();
        }
    }

    // 위치가 바뀌면 막대 상호작용이다(UE SScrollBar::SetState — 흐려짐 정책이면 다시 보인다).
    if (offset_ != arrangedOffset_) {
        arrangedOffset_ = offset_;
        NoteBarInteraction();
    }

    // Reserve면 막대 칸만큼 내용의 교차 축을 줄인다(막대는 그 칸에 놓인다).
    const float reserved = ReservedBarExtent(overflowing);
    const float cross = math::Max(0.0f, (vertical ? geometry.size.x : geometry.size.y) - reserved);
    const Vec2 size = vertical ? Vec2(cross, content) : Vec2(content, cross);
    const AffineTransform2D translation{
        1, 0, 0, 1, vertical ? 0 : before - offset_, vertical ? before - offset_ : 0};
    auto arranged = child->ArrangeIfNeeded(GetGeometry().MakeChild(size, translation, {}, {}));
    // 미룬 스크롤 요청을 자식 배치 뒤에 맞추고, 옮겼으면 같은 배치 안에서 다시 놓는다.
    if (const SharedPtr<Element> target = reveal_.Lock(); arranged && target) {
        reveal_.Reset();
        const float previous = offset_;
        if (target->IsWithin(*this) && ScrollIntoView(*target, revealAlignment_, revealPadding_) &&
            offset_ != previous) {
            return Arrange(geometry);
        }
    }

    reveal_.Reset();
    return arranged;
}

void ScrollView::RequestScrollIntoView(const SharedPtr<Element>& child,
                                       ScrollIntoViewAlignment alignment, float padding) noexcept {
    reveal_ = WeakPtr<Element>(child);
    revealAlignment_ = alignment;
    revealPadding_ = math::IsFinite(padding) && padding > 0 ? padding : 0;
    InvalidateArrangement();
}

PointerReply ScrollView::OnPreviewPointerDown(const PointerEvent& event) noexcept {
    if (maxOffset_ <= 0 || dragging_ ||
        (event.source == PointerSource::Touch && !touchScrolling_) ||
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

    SetDragging(true, false);
    dragButton_ = event.button;
    dragSource_ = event.source;
    dragUser_ = event.userId;
    dragPointer_ = event.pointerId;
    dragPosition_ = direction_ == StackDirection::Vertical ? local.y : local.x;
    dragOffset_ = offset_;
    return {.handled = true, .capture = true};
}

PointerReply ScrollView::OnTouchPan(const PointerEvent& move, Vec2 origin) noexcept {
    if (dragging_ || !touchScrolling_) {
        return {};
    }

    const bool vertical = direction_ == StackDirection::Vertical;
    float start = 0, current = 0;
    if (!TryTouchPan(GetGeometry(), vertical, origin, move.position, offset_, maxOffset_, start,
                     current)) {
        return {};
    }

    SetDragging(true, true);
    dragSource_ = move.source;
    dragUser_ = move.userId;
    dragPointer_ = move.pointerId;
    dragButton_ = move.button;
    dragPosition_ = start;
    dragOffset_ = offset_;
    SetUserOffset(dragOffset_ + start - current);
    return {.handled = true, .capture = true};
}

void ScrollView::SetDragging(bool dragging, bool content) noexcept {
    if (dragging_ != dragging || draggingContent_ != content) {
        const bool ended = dragging_ && !dragging;
        dragging_ = dragging;
        draggingContent_ = content;
        InvalidatePaint();
        // 끌기가 끝난 시각부터 흐려진다(UE EndScrolling).
        if (ended) {
            NoteBarInteraction();
        }
    }
}

void ScrollView::UpdateThumbHover(Vec2 position) noexcept {
    Vec2 thumbPosition;
    Vec2 size;
    Vec2 local;
    const bool inside = ToLocal(position, local);
    const bool over = GetThumb(thumbPosition, size) && inside && local.x >= thumbPosition.x &&
                      local.y >= thumbPosition.y && local.x < thumbPosition.x + size.x &&
                      local.y < thumbPosition.y + size.y;
    if (thumbHovered_ != over) {
        thumbHovered_ = over;
        InvalidatePaint();
    }

    // 막대(트랙) 위 들어옴·나감은 상호작용이고, 올린 동안 손잡이는 불투명도 1이다(UE IsHovered —
    // 막대 전체).
    Vec2 trackPosition;
    Vec2 trackSize;
    const bool overBar = (maxOffset_ > 0 || visibility_ == ScrollBarVisibility::Always) &&
                         GetTrack(trackPosition, trackSize) && inside &&
                         local.x >= trackPosition.x && local.y >= trackPosition.y &&
                         local.x < trackPosition.x + trackSize.x &&
                         local.y < trackPosition.y + trackSize.y;
    if (barHovered_ != overBar) {
        barHovered_ = overBar;
        NoteBarInteraction();
    }
}

double ScrollView::Now() const noexcept {
    const UiHostServices* host = FindUiHostServices();
    return host ? host->Time().current : 0.0;
}

ScrollThumbState ScrollView::ThumbState() const noexcept {
    return {.active = barHovered_ || (dragging_ && !draggingContent_),
            .scrolling = dragging_ && draggingContent_,
            .areaHovered = areaHovered_,
            .sinceInteraction = Now() - lastInteraction_};
}

float ScrollView::GetThumbOpacity() const noexcept {
    Vec2 position;
    Vec2 size;
    return GetThumb(position, size) ? ScrollThumbOpacity(visibility_, ThumbState()) : 0.0f;
}

void ScrollView::NoteBarInteraction() noexcept {
    lastInteraction_ = Now();
    InvalidatePaint();
    UpdateFadeTimer();
}

void ScrollView::UpdateFadeTimer() noexcept {
    if (!IsScrollThumbFading(visibility_, ThumbState())) {
        fadeTimer_.Reset();
        return;
    }

    if (fadeTimer_) {
        return;
    }

    // 흐려지는 동안 매 프레임 다시 그린다. 다 흐려지면 타이머를 놓는다. 트리에 붙지 않았거나
    // 타이머를 못 만들면 다음 상호작용·그리기에서 맞춘다(흐려짐만 늦다).
    Application* app = Application::Find(*this);
    const SharedPtr<Element> self = Parent() ? Parent()->FindChild(this) : SharedPtr<Element>{};
    if (!app || !self) {
        return;
    }

    auto timer =
        app->StartFrameTimer(self, Delegate<void(double)>::CreateLambda([this](double) noexcept {
                                 InvalidatePaint();
                                 if (!IsScrollThumbFading(visibility_, ThumbState())) {
                                     fadeTimer_.Reset();
                                 }
                             }));
    fadeTimer_ = timer ? Move(timer.Value()) : SharedPtr<UiTimer>{};
}

void ScrollView::OnMouseEnter(uint32 user) noexcept {
    Super::OnMouseEnter(user);
    if (!areaHovered_) {
        areaHovered_ = true;
        // OnHover는 들어오면 보인다.
        if (visibility_ == ScrollBarVisibility::OnHover) {
            InvalidatePaint();
            UpdateFadeTimer();
        }
    }
}

void ScrollView::OnMouseLeave(uint32 user) noexcept {
    Super::OnMouseLeave(user);
    const bool wasOverBar = barHovered_;
    barHovered_ = false;
    areaHovered_ = false;
    if (thumbHovered_) {
        thumbHovered_ = false;
        InvalidatePaint();
    }

    // 나감은 상호작용이다 — 흐려짐 정책이면 그 시각부터 흐려진다(UE OnMouseLeave).
    if (wasOverBar || visibility_ == ScrollBarVisibility::OnHover) {
        NoteBarInteraction();
    }
}

PointerReply ScrollView::OnPointerEvent(const PointerEvent& event) noexcept {
    // 손잡이 위 올림(UE SScrollBar IsHovered → HoveredThumbImage, 319-327행)은 이동마다 갱신한다.
    if (event.type == PointerEventType::Move && event.source == PointerSource::Mouse) {
        UpdateThumbHover(event.position);
    }

    if (!dragging_ && event.type == PointerEventType::Down &&
        event.source == PointerSource::Touch) {
        if (touchScrolling_ && maxOffset_ > 0) {
            return {.handled = true, .capture = true, .allowTouchPan = true};
        }

        return {};
    }

    if (event.type == PointerEventType::Up && event.source == PointerSource::Mouse &&
        event.button != dragButton_) {
        return {};
    }

    if (!dragging_ && event.type == PointerEventType::Down && maxOffset_ > 0 &&
        (event.source == PointerSource::Mouse && event.button == input::MouseButton::Right &&
         rightDragScrolling_)) {
        Vec2 local;
        if (!ToLocal(event.position, local)) {
            return {};
        }

        SetDragging(true, true);
        dragSource_ = event.source;
        dragUser_ = event.userId;
        dragPointer_ = event.pointerId;
        dragButton_ = event.button;
        dragPosition_ = direction_ == StackDirection::Vertical ? local.y : local.x;
        dragOffset_ = offset_;
        return {.handled = true, .capture = true};
    }

    if (dragging_ && dragSource_ == event.source && dragUser_ == event.userId &&
        dragPointer_ == event.pointerId) {
        if (event.type == PointerEventType::Move || event.type == PointerEventType::Up) {
            Vec2 position;
            Vec2 size;
            Vec2 local;
            if (draggingContent_ && ToLocal(event.position, local)) {
                const auto current = direction_ == StackDirection::Vertical ? local.y : local.x;
                SetUserOffset(dragOffset_ + dragPosition_ - current);
            } else if (GetThumb(position, size) && ToLocal(event.position, local)) {
                const bool vertical = direction_ == StackDirection::Vertical;
                Vec2 trackPosition, trackSize;
                (void)GetTrack(trackPosition, trackSize);
                const float track = vertical ? trackSize.y - size.y : trackSize.x - size.x;
                if (track > 0) {
                    const float current = vertical ? local.y : local.x;
                    SetUserOffset(dragOffset_ + (current - dragPosition_) * maxOffset_ / track);
                }
            }
        }

        if (event.type == PointerEventType::Up || event.type == PointerEventType::Cancel) {
            SetDragging(false, false);
            return {.handled = true, .releaseCapture = true};
        }

        return {.handled = true};
    }

    if (event.type == PointerEventType::Wheel && wheelPolicy_ != ConsumeMouseWheel::Never &&
        math::IsFinite(event.wheel)) {
        const float next = math::Clamp(offset_ - event.wheel * (WheelScrollStep * wheelMultiplier_),
                                       0.0f, maxOffset_);
        if (next != offset_) {
            SetUserOffset(next);
            return {.handled = true};
        }

        return {.handled = wheelPolicy_ == ConsumeMouseWheel::Always};
    }

    return {};
}

void ScrollView::OnPointerCaptureLost(PointerSource source, uint32 userId,
                                      uint32 pointerId) noexcept {
    if (dragging_ && dragSource_ == source && dragUser_ == userId && dragPointer_ == pointerId) {
        SetDragging(false, false);
    }
}

Result<void> ScrollView::OnPaintOverlay(PaintList& list, int32 layer) const noexcept {
    Vec2 position;
    Vec2 size;
    if ((alwaysShowTrack_ || visibility_ == ScrollBarVisibility::Always) &&
        GetTrack(position, size)) {
        const auto geometry =
            GetGeometry().MakeChild(size, {1, 0, 0, 1, position.x, position.y}, {}, {});
        // 항상 보이는 트랙은 스타일 트랙이다(UE AlwaysShowScrollbarTrack — Starship 트랙은 None이라
        // 기본 스타일에서는 그리지 않는다).
        const StyleBrush& trackBrush = barStyle_.track;
        if (trackBrush.mode != BrushMode::None) {
            auto track =
                list.TryAddBrush(geometry, trackBrush.Resolve(list.Theme(), list.ColorContext()),
                                 {1, 1, 1, 1}, layer);
            if (!track) {
                return track;
            }
        }
    }

    if (!GetThumb(position, size)) {
        return Result<void>::Success();
    }

    // 손잡이는 스크롤 막대 스타일이다(UE "Scrollbar"). 손잡이를 끄는 동안 끌기, 손잡이 위면 올림
    // 모양이다. 내용 끌기·터치 팬은 손잡이 끌기가 아니다(UE bDraggingThumb).
    const ScrollBarStyle& style = barStyle_;
    const StyleBrush& thumb = dragging_ && !draggingContent_ ? style.thumbDragged
                              : thumbHovered_                ? style.thumbHovered
                                                             : style.thumb;
    // 불투명도는 정책·상호작용 시각에서 나온다(23.6절 — UE GetThumbOpacity).
    const float opacity = ScrollThumbOpacity(visibility_, ThumbState());
    if (opacity <= 0) {
        return Result<void>::Success();
    }

    const AffineTransform2D translation{1, 0, 0, 1, position.x, position.y};
    return list.TryAddBrush(GetGeometry().MakeChild(size, translation, {}, {}),
                            thumb.Resolve(list.Theme(), list.ColorContext()), {1, 1, 1, opacity},
                            layer);
}

void ScrollView::ApplyStyles(const UiHostServices& host) noexcept {
    const ScrollBarStyle* style = host.FindStyle<ScrollBarStyle>("Scrollbar");
    barStyle_ = style ? *style : DefaultScrollBarStyle();
}

bool ScrollView::GetTrack(Vec2& position, Vec2& size) const noexcept {
    if (!showBar_ || thickness_ <= 0) {
        return false;
    }

    const auto view = GetGeometry().size;
    const float width = view.x - barPadding_.x - barPadding_.z;
    const float height = view.y - barPadding_.y - barPadding_.w;
    if (width <= 0 || height <= 0) {
        return false;
    }

    if (direction_ == StackDirection::Vertical) {
        size = {math::Min(thickness_, width), height};
        position = {view.x - barPadding_.z - size.x, barPadding_.y};
    } else {
        size = {width, math::Min(thickness_, height)};
        position = {barPadding_.x, view.y - barPadding_.w - size.y};
    }

    return true;
}

bool ScrollView::GetThumb(Vec2& position, Vec2& size) const noexcept {
    Vec2 trackPosition, viewport;
    if (!GetTrack(trackPosition, viewport)) {
        return false;
    }

    const bool vertical = direction_ == StackDirection::Vertical;
    const float length = vertical ? viewport.y : viewport.x;
    const float cross = vertical ? viewport.x : viewport.y;
    if (!showBar_ || thickness_ <= 0 ||
        (maxOffset_ <= 0 && visibility_ != ScrollBarVisibility::Always) || length <= 0 ||
        cross <= 0) {
        return false;
    }

    const float proportional = length * GetViewFraction();
    const float thumbLength = math::Min(length, math::Max(MinScrollThumbLength, proportional));
    const float thickness = math::Min(thickness_, cross);
    const float start = maxOffset_ > 0 ? (length - thumbLength) * offset_ / maxOffset_ : 0;
    position = trackPosition + (vertical ? Vec2(0, start) : Vec2(start, 0));
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

    endRequested_ = false;
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
