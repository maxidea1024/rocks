#include <UiFramework/ProgressBar.h>
#include <Platform/Clock.h>

namespace rocks::ui {

ProgressBar::ProgressBar(Allocator& allocator) noexcept : Frame(allocator), value_(*this, 0.0f) {
}

Result<void> ProgressBar::Construct(Args&& args) noexcept {
    auto result = Frame::Construct({.content = args.content,
                                    .color = Move(args.color),
                                    .brush = args.brush,
                                    .padding = args.padding,
                                    .desiredSize = Move(args.desiredSize)});
    if (result) {
        result = SetDirection(args.direction);
    }

    if (result) {
        BindValue(Move(args.value));
        SetFillColor(args.fillColor);
        SetMarquee(args.marquee);
    }

    return result;
}

void ProgressBar::BindValue(Bindable<float> source) noexcept {
    value_.Assign(Move(source));
}

Result<void> ProgressBar::SetDirection(ProgressDirection direction) noexcept {
    if (direction > ProgressDirection::FromCenterVertical) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (direction_ != direction) {
        direction_ = direction;
        InvalidatePaint();
    }

    return Result<void>::Success();
}

Result<void> ProgressBar::SetFillBrush(const Brush& brush) noexcept {
    if (!brush.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (brush != fillBrush_) {
        fillBrush_ = brush;
        InvalidatePaint();
    }

    return Result<void>::Success();
}

Result<void> ProgressBar::SetFillStyle(ProgressFillStyle style) noexcept {
    if (style > ProgressFillStyle::Mask) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (style != fillStyle_) {
        fillStyle_ = style;
        InvalidatePaint();
    }

    return Result<void>::Success();
}

Result<void> ProgressBar::SetBorderPadding(Vec2 padding) noexcept {
    if (!padding.IsFinite() || padding.x < 0 || padding.y < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (padding != borderPadding_) {
        borderPadding_ = padding;
        InvalidatePaint();
    }

    return Result<void>::Success();
}

void ProgressBar::SetMarquee(bool enabled) noexcept {
    if (marquee_ == enabled) {
        return;
    }

    marquee_ = enabled;
    if (enabled) {
        marqueeStart_ = platform::MonotonicNanoseconds();
    }

    SetVolatile(enabled);
    InvalidatePaint();
}

Result<void> ProgressBar::SetValue(float value) noexcept {
    if (!math::IsFinite(value)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    value_.Set(math::Clamp(value, 0.0f, 1.0f));
    return Result<void>::Success();
}

float ProgressBar::GetValue() const noexcept {
    const float value = value_.Get();
    return math::IsFinite(value) ? math::Clamp(value, 0.0f, 1.0f) : 0.0f;
}

void ProgressBar::SetFillColor(LinearColor color) noexcept {
    if (fillColor_ != color) {
        fillColor_ = color;
        InvalidatePaint();
    }
}

Result<void> ProgressBar::OnPaint(PaintList& list, int32 layer) const noexcept {
    auto background = Frame::OnPaint(list, layer);
    const float value = GetValue();
    if (!background || (!marquee_ && value <= 0)) {
        return background;
    }

    const auto& bounds = GetGeometry();
    const Vec2 usable{math::Max(bounds.size.x - borderPadding_.x * 2, 0.0f),
                      math::Max(bounds.size.y - borderPadding_.y * 2, 0.0f)};
    if (usable.x == 0 || usable.y == 0) {
        return background;
    }

    const auto area =
        bounds.MakeChild(usable, {1, 0, 0, 1, borderPadding_.x, borderPadding_.y}, {}, {});
    Vec2 size = area.size;
    Vec2 offset{};
    if (marquee_) {
        constexpr uint64 period = 1200000000;
        const float phase =
            static_cast<float>((platform::MonotonicNanoseconds() - marqueeStart_) % period) /
            period;
        const float start = phase * 1.25f - 0.25f;
        const float left = math::Max(start, 0.0f);
        offset.x = left * size.x;
        size.x *= math::Min(start + 0.25f, 1.0f) - left;
    } else {
        switch (direction_) {
        case ProgressDirection::RightToLeft:
            offset.x = size.x * (1 - value);
            [[fallthrough]];
        case ProgressDirection::LeftToRight:
            size.x *= value;
            break;
        case ProgressDirection::BottomToTop:
            offset.y = size.y * (1 - value);
            [[fallthrough]];
        case ProgressDirection::TopToBottom:
            size.y *= value;
            break;
        case ProgressDirection::FromCenter:
            size = size * value;
            offset = (area.size - size) * 0.5f;
            break;
        case ProgressDirection::FromCenterHorizontal:
            size.x *= value;
            offset.x = (area.size.x - size.x) * 0.5f;
            break;
        case ProgressDirection::FromCenterVertical:
            size.y *= value;
            offset.y = (area.size.y - size.y) * 0.5f;
            break;
        }
    }

    const auto part = area.MakeChild(size, {1, 0, 0, 1, offset.x, offset.y}, {}, {});
    if (fillStyle_ == ProgressFillStyle::Scale || marquee_) {
        return list.TryAddBrush(part, fillBrush_, fillColor_, layer);
    }

    return list.TryAddBrushRegion(area, fillBrush_, fillColor_, layer, {offset, offset + size});
}

AccessibleRole ProgressBar::GetAccessibleRole() const noexcept {
    return AccessibleRole::Progress;
}

} // namespace rocks::ui
