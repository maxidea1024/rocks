#include <UiFramework/Controls/RadialSlider.h>

namespace rocks::ui {

namespace {

// 원형 슬라이더 기본 크기·한 바퀴 각도다.
constexpr Vec2 RadialDefaultSize{48, 48};
constexpr float FullTurnDegrees = 360;
// 손잡이·선 굵기 상한과 크기 대비 비율이다.
constexpr float MaxThumbSize = 9.0f;
constexpr float ThumbSizeRatio = 0.25f;
constexpr float MaxArcWidth = 4.0f;
constexpr float ArcWidthRatio = 0.12f;
// 호를 나누는 선분 길이(px)와 선분 수 범위다.
constexpr float ArcSegmentLength = 5.0f;
constexpr float MinArcSegments = 4.0f;
constexpr float MaxArcSegments = 128.0f;

Vec2 PointOnArc(Vec2 center, float radius, float degrees) noexcept {
    const float angle = math::DegreesToRadians(degrees);
    return center + Vec2(-math::Sin(angle), math::Cos(angle)) * radius;
}

Result<void> DrawLine(PaintList& list, const Geometry& area, Vec2 from, Vec2 to, float width,
                      LinearColor color, int32 layer) noexcept {
    const Vec2 delta = to - from;
    const float length = delta.Length();
    if (length == 0) {
        return Result<void>::Success();
    }

    const Vec2 dir = delta / length;
    const Vec2 normal(-dir.y, dir.x);
    const Vec2 origin = from - normal * (width * 0.5f);
    auto segment = area.MakeChild({length, width},
                                  {dir.x, normal.x, dir.y, normal.y, origin.x, origin.y}, {}, {});
    return list.TryAddBox({segment, color, layer});
}

} // namespace

RadialSlider::RadialSlider(Allocator& allocator) noexcept : Slider(allocator) {
    SetDesiredSize(RadialDefaultSize);
    SetColor(DefaultThemeColors().Get(ThemeSlot::Input));
    SetFillColor(DefaultThemeColors().Get(ThemeSlot::Primary));
    SetHandleColor(DefaultThemeColors().Get(ThemeSlot::Foreground));
}

Result<void> RadialSlider::Construct(Args&& args) noexcept {
    if (!args.track.IsFinite() || !args.progress.IsFinite() || !args.handle.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto ready = SetRange(args.minimum, args.maximum);
    if (ready) {
        ready = SetStep(args.step);
    }

    if (ready) {
        ready = SetArc(args.startAngle, args.endAngle, args.angularOffset);
    }

    if (ready) {
        ready = SetValue(args.value);
    }

    if (!ready) {
        return ready;
    }

    SetColor(args.track);
    SetFillColor(args.progress);
    SetHandleColor(args.handle);
    SetSnapToStep(args.snapToStep);
    SetHandVisibility(args.showHandle, args.showHand);
    SetOnChanged(Move(args.onChanged));
    return Result<void>::Success();
}

bool RadialSlider::IsValidArc(float start, float end, float offset) noexcept {
    return math::IsFinite(start) && math::IsFinite(end) && math::IsFinite(offset) && start >= 0 &&
           end <= FullTurnDegrees && end > start;
}

Result<void> RadialSlider::SetArc(float start, float end, float offset) noexcept {
    if (!IsValidArc(start, end, offset)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    offset = math::UnwindDegrees(offset);
    if (start_ != start || end_ != end || offset_ != offset) {
        start_ = start;
        end_ = end;
        offset_ = offset;
        hasAngle_ = false;
        InvalidatePaint();
    }

    return Result<void>::Success();
}

void RadialSlider::SetHandVisibility(bool handle, bool hand) noexcept {
    if (showHandle_ != handle || showHand_ != hand) {
        showHandle_ = handle;
        showHand_ = hand;
        InvalidatePaint();
    }
}

void RadialSlider::SetHandleColor(LinearColor color) noexcept {
    if (handleColor_ != color) {
        handleColor_ = color;
        InvalidatePaint();
    }
}

KeyReply RadialSlider::OnKeyEvent(const KeyEvent& key) noexcept {
    const input::InputEvent& event = key.input;
    if (event.type == input::InputEventType::KeyDown) {
        if (event.key == input::Key::Home || event.key == input::Key::End) {
            SetInteractiveValue(event.key == input::Key::Home ? Minimum() : Maximum());
            return KeyReply::Handled();
        }

        // 위·아래는 값 증감이다(오른쪽·왼쪽으로 바꿔 넘긴다).
        if (event.key == input::Key::Up || event.key == input::Key::Down) {
            auto mapped = event;
            mapped.key = event.key == input::Key::Up ? input::Key::Right : input::Key::Left;
            const KeyEvent remapped{key.user, mapped, key.chord, key.navigation, key.action};
            return Slider::OnKeyEvent(remapped);
        }
    }

    return Slider::OnKeyEvent(key);
}

void RadialSlider::UpdateFromPointer(Vec2 position, bool firstPoint) noexcept {
    if (firstPoint) {
        hasAngle_ = false;
    }

    const auto& area = GetGeometry();
    AffineTransform2D inverse;
    if (!area.render.TryInverse(inverse)) {
        return;
    }

    const Vec2 local = inverse.Apply(position) - area.size * 0.5f;
    if (!local.IsFinite() || local == Vec2{}) {
        return;
    }

    float angle =
        math::UnwindDegrees(math::RadiansToDegrees(math::Atan2(-local.x, local.y)) - offset_);
    if (angle < 0) {
        angle += FullTurnDegrees;
    }

    if (!hasAngle_) {
        const float distanceToStart = math::Abs(math::FindDeltaAngleDegrees(angle, start_));
        const float distanceToEnd = math::Abs(math::FindDeltaAngleDegrees(angle, end_));
        if (angle < start_ || angle > end_) {
            dragAngle_ = distanceToEnd < distanceToStart ? end_ : start_;
        } else if (angle == 0 && end_ == FullTurnDegrees &&
                   GetValue() - Minimum() > (Maximum() - Minimum()) * 0.5f) {
            dragAngle_ = end_;
        } else {
            dragAngle_ = angle;
        }

        hasAngle_ = true;
    } else {
        dragAngle_ = math::Clamp(dragAngle_ + math::FindDeltaAngleDegrees(previousAngle_, angle),
                                 start_, end_);
    }

    previousAngle_ = angle;
    SetInteractiveRatio((dragAngle_ - start_) / (end_ - start_));
}

Result<void> RadialSlider::OnPaint(PaintList& list, int32 layer) const noexcept {
    const auto& area = GetGeometry();
    const float size = math::Min(area.size.x, area.size.y);
    if (size <= 0) {
        return Result<void>::Success();
    }

    const Vec2 center = area.size * 0.5f;
    const float thumb = math::Min(MaxThumbSize, size * ThumbSizeRatio);
    const float width = math::Min(MaxArcWidth, size * ArcWidthRatio);
    const float radius = (size - thumb) * 0.5f;
    const float span = end_ - start_;
    const float active = start_ + span * ((GetValue() - Minimum()) / (Maximum() - Minimum()));
    // 반지름과 호 길이에 맞춰 선분 수를 고른다. 색이 바뀌는 지점은 손잡이 각도에서 나눈다.
    const uint32 segments = static_cast<uint32>(
        math::Clamp(math::Ceil(radius * math::DegreesToRadians(span) / ArcSegmentLength),
                    MinArcSegments, MaxArcSegments));
    const Vec2 handle = PointOnArc(center, radius, active + offset_);
    Vec2 from = PointOnArc(center, radius, start_ + offset_);
    float previous = start_;
    for (uint32 i = 1; i <= segments; ++i) {
        const float angle = start_ + span * static_cast<float>(i) / static_cast<float>(segments);
        const Vec2 to = PointOnArc(center, radius, angle + offset_);
        if (previous < active && angle > active) {
            auto filled = DrawLine(list, area, from, handle, width, FillColor(), layer);
            if (!filled) {
                return filled;
            }

            from = handle;
        }

        auto drawn = DrawLine(list, area, from, to, width,
                              angle <= active ? FillColor() : GetColor(), layer);
        if (!drawn) {
            return drawn;
        }

        from = to;
        previous = angle;
    }

    if (showHand_) {
        auto drawn = DrawLine(list, area, center, handle, width * 0.6f, handleColor_, layer);
        if (!drawn) {
            return drawn;
        }
    }

    if (showHandle_) {
        auto box = area.MakeChild(
            {thumb, thumb}, {1, 0, 0, 1, handle.x - thumb * 0.5f, handle.y - thumb * 0.5f}, {}, {});
        return list.TryAddBox({box, handleColor_, layer});
    }

    return Result<void>::Success();
}

} // namespace rocks::ui
