#include <UiWidget/WidgetTransform.h>

namespace rocks::ui {

Result<AffineTransform2D> WidgetTransform::ToAffine() const noexcept {
    if (!translation.IsFinite() || !scale.IsFinite() || !shear.IsFinite() ||
        !math::IsFinite(angle)) {
        return Result<AffineTransform2D>::Failure(ErrorCode::InvalidArgument);
    }

    float sine, cosine;
    math::SinCos(math::DegreesToRadians(math::UnwindDegrees(angle)), sine, cosine);
    const float x = math::Tan(math::DegreesToRadians(math::Clamp(shear.x, -89.0f, 89.0f)));
    const float y = math::Tan(math::DegreesToRadians(math::Clamp(shear.y, -89.0f, 89.0f)));
    AffineTransform2D result{(cosine - sine * y) * scale.x,
                             (cosine * x - sine) * scale.y,
                             (sine + cosine * y) * scale.x,
                             (sine * x + cosine) * scale.y,
                             translation.x,
                             translation.y};
    if (!math::IsFinite(result.m00) || !math::IsFinite(result.m01) || !math::IsFinite(result.m10) ||
        !math::IsFinite(result.m11)) {
        return Result<AffineTransform2D>::Failure(ErrorCode::Overflow);
    }

    return Result<AffineTransform2D>::Success(result);
}
} // namespace rocks::ui
