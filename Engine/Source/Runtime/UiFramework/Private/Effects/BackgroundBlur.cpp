#include <UiFramework/Effects/BackgroundBlur.h>
#include <UiCore/PaintList.h>

namespace rocks::ui {

Result<void> BackgroundBlur::Construct(Args&& args) noexcept {
    if (!math::IsFinite(args.strength.value) || args.strength.value < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto attached = SetCornerRadius(args.cornerRadius);
    if (attached) {
        attached = SetBlurRadius(args.blurRadius);
    }

    if (attached) {
        attached = SetLowQualityFallbackBrush(args.fallback);
    }

    if (attached) {
        attached = args.content.AttachTo(*this);
    }

    SetLowQuality(args.lowQuality);
    SetApplyAlphaToBlur(args.applyAlphaToBlur);
    if (attached) {
        strength_.Assign(Move(args.strength));
    }

    return attached;
}

BackgroundBlur::BackgroundBlur(Allocator& allocator) noexcept
    : SizeBox(allocator), strength_(*this, 0) {
}

Result<void> BackgroundBlur::SetStrength(float pixels) noexcept {
    if (!math::IsFinite(pixels) || pixels < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    strength_.Set(pixels);
    return Result<void>::Success();
}

Result<void> BackgroundBlur::SetCornerRadius(Vec4 corners) noexcept {
    if (!corners.IsFinite() || corners.x < 0 || corners.y < 0 || corners.z < 0 || corners.w < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (cornerRadius_ != corners) {
        cornerRadius_ = corners;
        InvalidatePaint();
    }

    return Result<void>::Success();
}

Result<void> BackgroundBlur::SetBlurRadius(int32 kernel) noexcept {
    if (kernel < -1 || kernel > 255) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (blurRadius_ != kernel) {
        blurRadius_ = kernel;
        InvalidatePaint();
    }

    return Result<void>::Success();
}

void BackgroundBlur::SetApplyAlphaToBlur(bool apply) noexcept {
    if (applyAlpha_ != apply) {
        applyAlpha_ = apply;
        InvalidatePaint();
    }
}

void BackgroundBlur::SetLowQuality(bool enabled) noexcept {
    if (lowQuality_ != enabled) {
        lowQuality_ = enabled;
        InvalidatePaint();
    }
}

Result<void> BackgroundBlur::SetLowQualityFallbackBrush(const Brush& brush) noexcept {
    if (!brush.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (fallback_ != brush) {
        fallback_ = brush;
        InvalidatePaint();
    }

    return Result<void>::Success();
}

Result<void> BackgroundBlur::OnPaint(PaintList& list, int32 layer) const noexcept {
    const float strength = strength_.Get() * (applyAlpha_ ? list.InheritedOpacity() : 1.0f);
    if (!math::IsFinite(strength) || strength <= 0) {
        return Result<void>::Success();
    }

    if (lowQuality_) {
        return list.TryAddBrush(GetGeometry(), fallback_, {1, 1, 1, 1}, layer);
    }

    PaintBox box{GetGeometry(), {1, 1, 1, 1}, layer};
    box.operation = PaintOperation::BlurBackdrop;
    box.blurRadius = strength;
    box.blurKernel = blurRadius_;
    box.cornerRadius = cornerRadius_;
    return list.TryAddBox(box);
}

} // namespace rocks::ui
