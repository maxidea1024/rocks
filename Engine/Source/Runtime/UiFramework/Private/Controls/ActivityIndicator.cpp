#include <UiFramework/Controls/ActivityIndicator.h>

namespace rocks::ui {

namespace {

// 원형은 0.75초에 8조각, 막대형은 1.5초에 3조각이다(UE SCircularThrobber·SThrobber 기본).
constexpr float CircularPeriod = 0.75f;
constexpr float BarPeriod = 1.5f;
constexpr uint32 CircularPieces = 8;
constexpr uint32 BarPieces = 3;

constexpr double PieceDelay = 0.05;

} // namespace

ActivityIndicator::ActivityIndicator(Allocator& allocator, bool circular) noexcept
    : Element(allocator), period_(circular ? CircularPeriod : BarPeriod),
      count_(circular ? CircularPieces : BarPieces), circular_(circular) {
    SetVisibility(Visibility::HitTestInvisible);
    UpdateVolatility();
}

double ActivityIndicator::Cycle() const noexcept {
    return circular_ ? period_ : period_ + (count_ - 1) * PieceDelay;
}

double ActivityIndicator::Elapsed(double now) const noexcept {
    const double delta = animating_ && now >= origin_ ? now - origin_ : 0.0;
    return math::Fmod(elapsed_ + delta, Cycle());
}

void ActivityIndicator::UpdateVolatility() noexcept {
    SetVolatile(animating_ && brush_.mode != BrushMode::None &&
                (circular_ || horizontal_ || vertical_ || opacity_));
}

void ActivityIndicator::SetAnimating(bool animate) noexcept {
    if (animate != animating_) {
        const double now = FindFrameTime().current;
        elapsed_ = Elapsed(now);
        origin_ = now;
        animating_ = animate;
        UpdateVolatility();
        InvalidatePaint();
    }
}

Result<void> ActivityIndicator::SetPieceCount(uint32 count) noexcept {
    if (count == 0 || count > MaxActivityIndicatorPieces) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (count_ != count) {
        count_ = count;
        InvalidateLayout();
        InvalidatePaint();
    }

    return Result<void>::Success();
}

Result<void> ActivityIndicator::SetPieceBrush(const Brush& brush) noexcept {
    if (!brush.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (brush_ != brush) {
        const bool resized = brush_.imageSize != brush.imageSize;
        brush_ = brush;
        if (resized) {
            InvalidateLayout();
        }

        UpdateVolatility();
        InvalidatePaint();
    }

    return Result<void>::Success();
}

void ActivityIndicator::SetAnimationAxes(bool horizontal, bool vertical, bool opacity) noexcept {
    if (horizontal_ != horizontal || vertical_ != vertical || opacity_ != opacity) {
        horizontal_ = horizontal;
        vertical_ = vertical;
        opacity_ = opacity;
        UpdateVolatility();
        InvalidatePaint();
    }
}

Result<void> ActivityIndicator::SetRadius(float radius) noexcept {
    if (!math::IsFinite(radius) || radius < 0 || radius > NumericLimits<float>::Max() * 0.5f) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (radius_ != radius) {
        radius_ = radius;
        InvalidateLayout();
        InvalidatePaint();
    }

    return Result<void>::Success();
}

Result<void> ActivityIndicator::SetPeriod(float seconds) noexcept {
    if (!math::IsFinite(seconds) || seconds <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (period_ != seconds) {
        const double now = FindFrameTime().current;
        const double phase = Elapsed(now) / Cycle();
        period_ = seconds;
        elapsed_ = phase * Cycle();
        origin_ = now;
        InvalidatePaint();
    }

    return Result<void>::Success();
}

Result<void> ActivityIndicator::TryAddChild(const SharedPtr<Element>&) noexcept {
    return Result<void>::Failure(ErrorCode::Unsupported);
}

Vec2 ActivityIndicator::ComputeDesiredSize(float) const noexcept {
    return circular_ ? Vec2{2 * radius_, 2 * radius_}
                     : Vec2{brush_.imageSize.x * count_, brush_.imageSize.y};
}

Result<void> ActivityIndicator::OnPaint(PaintList& list, int32 layer) const noexcept {
    const auto& area = GetGeometry();
    if (area.size.x <= 0 || area.size.y <= 0 || brush_.mode == BrushMode::None) {
        return Result<void>::Success();
    }

    const double elapsed = Elapsed(animating_ ? list.GetFrameTime().current : origin_);
    const Vec2 desired = brush_.imageSize;
    for (uint32 i = 0; i < count_; ++i) {
        float scaleX = 1, scaleY = 1, alpha = 1;
        Vec2 center{};
        if (circular_) {
            const float angle = math::TwoPi<float> * (static_cast<float>(elapsed / period_) +
                                                      static_cast<float>(i) / count_);
            const Vec2 extent{math::Max(0.f, (area.size.x - desired.x) * 0.5f),
                              math::Max(0.f, (area.size.y - desired.y) * 0.5f)};
            center = {area.size.x * 0.5f + extent.x * math::Sin(angle),
                      area.size.y * 0.5f + extent.y * math::Cos(angle)};
            scaleX = scaleY = static_cast<float>(i + 1) / count_;
        } else {
            const float fraction =
                static_cast<float>(math::Clamp((elapsed - i * PieceDelay) / period_, 0.0, 1.0));
            const float wave = math::Sin(fraction * math::TwoPi<float>);
            scaleX = horizontal_ ? wave : 1;
            scaleY = vertical_ ? wave : 1;
            alpha = opacity_ ? math::Max(0.f, wave) : 1;
            center = {desired.x * (i + 0.5f), area.size.y * 0.5f};
        }

        const AffineTransform2D transform{scaleX,
                                          0,
                                          0,
                                          scaleY,
                                          center.x - desired.x * scaleX * 0.5f,
                                          center.y - desired.y * scaleY * 0.5f};
        auto result = list.TryAddBrush(area.MakeChild(desired, transform, {}, {}), brush_,
                                       {1, 1, 1, alpha}, layer);
        if (!result) {
            return result;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::ui
