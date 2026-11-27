#include <UiFramework/Scrolling/ScrollBar.h>

namespace rocks::ui {

Result<void> ScrollBar::Construct(Args&& args) noexcept {
    auto result = SetState(args.offset, args.thumbSize);
    if (result) {
        SetHorizontal(args.horizontal);
        BindColor(Move(args.color));
        SetOnScrolled(Move(args.onScrolled));
    }

    return result;
}

ScrollBar::ScrollBar(Allocator& allocator) noexcept : Frame(allocator) {
    UpdateDesiredSize();
}

void ScrollBar::UpdateDesiredSize() noexcept {
    // 두께는 스타일 두께(UE "Scrollbar" 8)이고, 길이 방향 기본값은 두께의
    // ScrollBarLengthFactor배다.
    const float thickness = style_.thickness;
    const float length = thickness * ScrollBarLengthFactor;
    SetDesiredSize(horizontal_ ? Vec2{length, thickness} : Vec2{thickness, length});
}

void ScrollBar::SetDragging(bool dragging) noexcept {
    if (dragging_ != dragging) {
        dragging_ = dragging;
        InvalidatePaint();
    }
}

Result<void> ScrollBar::SetState(float offset, float thumbSize) noexcept {
    if (!math::IsFinite(offset) || !math::IsFinite(thumbSize) || thumbSize < 0 || thumbSize > 1) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    offset = math::Clamp(offset, 0.0f, 1.0f - thumbSize);
    if (offset_ != offset || thumbSize_ != thumbSize) {
        offset_ = offset;
        thumbSize_ = thumbSize;
        InvalidatePaint();
    }

    return Result<void>::Success();
}

void ScrollBar::SetHorizontal(bool horizontal) noexcept {
    if (horizontal != horizontal_) {
        horizontal_ = horizontal;
        UpdateDesiredSize();
        InvalidatePaint();
    }
}

float ScrollBar::Offset() const noexcept {
    return offset_;
}

float ScrollBar::ThumbSize() const noexcept {
    return thumbSize_;
}

void ScrollBar::SetOnScrolled(Delegate<void(float)>&& callback) noexcept {
    scrolled_ = Move(callback);
}

float ScrollBar::ThumbLength(float track) const noexcept {
    return math::Min(track, math::Max(MinScrollThumbLength, track * thumbSize_));
}

float ScrollBar::ThumbStart(float track, float thumb) const noexcept {
    return thumbSize_ < 1 ? (track - thumb) * (offset_ / (1.0f - thumbSize_)) : 0;
}

void ScrollBar::ScrollTo(float offset) noexcept {
    const float old = offset_;
    if (SetState(offset, thumbSize_) && offset_ != old) {
        scrolled_.ExecuteIfBound(offset_);
    }
}

PointerReply ScrollBar::OnPointerEvent(const PointerEvent& event) noexcept {
    if (dragging_ &&
        (event.source != source_ || event.pointerId != pointer_ || event.userId != user_)) {
        return {};
    }

    if ((event.type == PointerEventType::Down || event.type == PointerEventType::Up) &&
        event.source == PointerSource::Mouse && event.button != input::MouseButton::Left) {
        return {};
    }

    if (dragging_ &&
        (event.type == PointerEventType::Up || event.type == PointerEventType::Cancel)) {
        SetDragging(false);
        return {.handled = true, .releaseCapture = true};
    }

    const auto& geometry = GetGeometry();
    const float length = horizontal_ ? geometry.size.x : geometry.size.y;
    AffineTransform2D inverse;
    if (length <= 0 || thumbSize_ >= 1 || !geometry.render.TryInverse(inverse)) {
        return {};
    }

    const auto local = inverse.Apply(event.position);
    if (!local.IsFinite()) {
        return {};
    }

    const float position = horizontal_ ? local.x : local.y;
    const float thumb = ThumbLength(length);
    const float travel = length - thumb;
    if (travel <= 0) {
        return {};
    }

    const float start = ThumbStart(length, thumb);
    const float logicalTravel = 1.0f - thumbSize_;
    if (event.type == PointerEventType::Down && !dragging_) {
        source_ = event.source;
        user_ = event.userId;
        pointer_ = event.pointerId;
        SetDragging(true);
        if (position >= start && position <= start + thumb) {
            grabOffset_ = position - start;
        } else {
            grabOffset_ = thumb * 0.5f;
            ScrollTo(((position - grabOffset_) / travel) * logicalTravel);
        }

        return {.handled = true, .capture = true};
    }

    if (event.type == PointerEventType::Move && dragging_) {
        ScrollTo(((position - grabOffset_) / travel) * logicalTravel);
        return {.handled = true};
    }

    return {};
}

void ScrollBar::OnPointerCaptureLost(PointerSource source, uint32 user, uint32 pointer) noexcept {
    if (source_ == source && user_ == user && pointer_ == pointer) {
        SetDragging(false);
    }
}

Result<void> ScrollBar::SetStyle(const ScrollBarStyle& style) noexcept {
    auto applied = ApplyStyle(style);
    if (applied) {
        styleExplicit_ = true;
    }

    return applied;
}

void ScrollBar::ApplyStyles(const UiHostServices& host) noexcept {
    if (!styleExplicit_) {
        // 활성 집합에 키가 없으면(등록부 없음 포함) 내장 기본값으로 돌린다.
        const ScrollBarStyle* style = host.FindStyle<ScrollBarStyle>("Scrollbar");
        (void)ApplyStyle(style ? *style : DefaultScrollBarStyle());
    }
}

Result<void> ScrollBar::ApplyStyle(const ScrollBarStyle& style) noexcept {
    const ThemeColors& theme = DefaultThemeColors();
    const StyleBrush* brushes[] = {&style.track, &style.thumb, &style.thumbHovered,
                                   &style.thumbDragged};
    for (const StyleBrush* brush : brushes) {
        if (!brush->Resolve(theme, {}).IsValid()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    if (!math::IsFinite(style.thickness) || style.thickness < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (style_ != style) {
        style_ = style;
        UpdateDesiredSize();
        InvalidatePaint();
    }

    return Result<void>::Success();
}

const ScrollBarStyle& ScrollBar::GetStyle() const noexcept {
    return style_;
}

void ScrollBar::OnMouseEnter(uint32) noexcept {
    if (++hoverCount_ == 1) {
        InvalidatePaint();
    }
}

void ScrollBar::OnMouseLeave(uint32) noexcept {
    if (hoverCount_ && --hoverCount_ == 0) {
        InvalidatePaint();
    }
}

Result<void> ScrollBar::OnPaint(PaintList& list, int32 layer) const noexcept {
    const StyleColorContext context = list.ColorContext();
    if (style_.track.mode != BrushMode::None) {
        auto track = list.TryAddBrush(GetGeometry(), style_.track.Resolve(list.Theme(), context),
                                      {1, 1, 1, 1}, layer);
        if (!track) {
            return track;
        }
    }

    const auto& g = GetGeometry();
    const float trackLength = horizontal_ ? g.size.x : g.size.y;
    const float thumb = ThumbLength(trackLength);
    const float start = ThumbStart(trackLength, thumb);
    const Vec2 size = horizontal_ ? Vec2{thumb, g.size.y} : Vec2{g.size.x, thumb};
    const Vec2 origin = horizontal_ ? Vec2{start, 0} : Vec2{0, start};
    const StyleBrush& thumbBrush = dragging_          ? style_.thumbDragged
                                   : hoverCount_ != 0 ? style_.thumbHovered
                                                      : style_.thumb;
    if (thumbBrush.mode == BrushMode::None) {
        return Result<void>::Success();
    }

    return list.TryAddBrush(g.MakeChild(size, {1, 0, 0, 1, origin.x, origin.y}, {}, {}),
                            thumbBrush.Resolve(list.Theme(), context), GetColor(), layer);
}

AccessibleRole ScrollBar::GetAccessibleRole() const noexcept {
    return AccessibleRole::ScrollBar;
}

} // namespace rocks::ui
