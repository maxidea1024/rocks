#include <UiFramework/Controls/ProgressBar.h>

namespace rocks::ui {

namespace {

// marquee 막대가 한 번 지나가는 시간(초)이다. Rocks 값이다(UE SProgressBar는 marquee 이미지를
// 흘린다).
constexpr double MarqueePeriodSeconds = 1.2;

} // namespace

ProgressBar::ProgressBar(Allocator& allocator) noexcept : Frame(allocator), value_(*this, 0.0f) {
}

Result<void> ProgressBar::Construct(Args&& args) noexcept {
    auto result = Frame::Construct({.content = args.content,
                                    .color = Move(args.color),
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

    customFill_ = true;
    if (brush != fillBrush_) {
        fillBrush_ = brush;
        InvalidatePaint();
    }

    return Result<void>::Success();
}

void ProgressBar::ClearFillBrush() noexcept {
    if (customFill_) {
        customFill_ = false;
        fillBrush_ = {};
        InvalidatePaint();
    }
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
        marqueeStart_ = FindFrameTime().current;
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

Result<void> ProgressBar::SetStyle(const ProgressBarStyle& style) noexcept {
    auto applied = ApplyStyle(style);
    if (applied) {
        styleExplicit_ = true;
    }

    return applied;
}

void ProgressBar::ApplyStyles(const UiHostServices& host) noexcept {
    if (!styleExplicit_) {
        if (const ProgressBarStyle* style = host.FindStyle<ProgressBarStyle>("ProgressBar")) {
            (void)ApplyStyle(*style);
        }
    }
}

Result<void> ProgressBar::ApplyStyle(const ProgressBarStyle& style) noexcept {
    const ThemeColors& theme = DefaultThemeColors();
    const StyleBrush* brushes[] = {&style.background, &style.fill, &style.marquee};
    for (const StyleBrush* brush : brushes) {
        if (!brush->Resolve(theme, {}).IsValid()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    if (style_ != style) {
        style_ = style;
        InvalidatePaint();
    }

    return Result<void>::Success();
}

const ProgressBarStyle& ProgressBar::GetStyle() const noexcept {
    return style_;
}

Result<void> ProgressBar::OnPaint(PaintList& list, int32 layer) const noexcept {
    const StyleColorContext context = list.ColorContext();
    Result<void> background = Result<void>::Success();
    if (style_.background.mode != BrushMode::None) {
        background = list.TryAddBrush(
            GetGeometry(), style_.background.Resolve(list.Theme(), context), GetColor(), layer);
    }

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
        const double elapsed = math::Max(0.0, list.GetFrameTime().current - marqueeStart_);
        const float phase =
            static_cast<float>(math::Fmod(elapsed, MarqueePeriodSeconds) / MarqueePeriodSeconds);
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
    const Brush fill = marquee_      ? style_.marquee.Resolve(list.Theme(), context)
                       : customFill_ ? fillBrush_
                                     : style_.fill.Resolve(list.Theme(), context);
    if (fillStyle_ == ProgressFillStyle::Scale || marquee_) {
        return list.TryAddBrush(part, fill, fillColor_, layer);
    }

    return list.TryAddBrushRegion(area, fill, fillColor_, layer, {offset, offset + size});
}

AccessibleRole ProgressBar::GetAccessibleRole() const noexcept {
    return AccessibleRole::Progress;
}

} // namespace rocks::ui
