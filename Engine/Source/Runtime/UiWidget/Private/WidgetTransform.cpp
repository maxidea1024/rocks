#include <UiWidget/WidgetTransform.h>

namespace rocks::ui {

namespace {

// 기울임 각 한도(도)다 — 90에 가까우면 tan이 발산한다.
constexpr float MaxShearDegrees = 89.0f;

} // namespace

Result<AffineTransform2D> WidgetTransform::ToAffine() const noexcept {
    if (!translation.IsFinite() || !scale.IsFinite() || !shear.IsFinite() ||
        !math::IsFinite(angle)) {
        return Result<AffineTransform2D>::Failure(ErrorCode::InvalidArgument);
    }

    float sine, cosine;
    math::SinCos(math::DegreesToRadians(math::UnwindDegrees(angle)), sine, cosine);
    const float x =
        math::Tan(math::DegreesToRadians(math::Clamp(shear.x, -MaxShearDegrees, MaxShearDegrees)));
    const float y =
        math::Tan(math::DegreesToRadians(math::Clamp(shear.y, -MaxShearDegrees, MaxShearDegrees)));
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
