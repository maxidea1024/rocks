#include <CurveAssets/ColorCurveValue.h>
#include "CurveChannelCopy.h"

// 색 커브 평탄 값이다(Docs/Design/CurveAssets 2.5절).
namespace rocks {

LinearColor ColorCurveValue::Evaluate(float time) const noexcept {
    return adjusted ? EvaluateAdjustedColorCurve(r, g, b, a, Adjustments(), time, false)
                    : EvaluateUnadjustedColorCurve(r, g, b, a, time);
}

LinearColor ColorCurveValue::EvaluateClamped(float time) const noexcept {
    return adjusted ? EvaluateAdjustedColorCurve(r, g, b, a, Adjustments(), time, true)
                    : EvaluateUnadjustedColorCurve(r, g, b, a, time);
}

ColorCurveAdjustments ColorCurveValue::Adjustments() const noexcept {
    ColorCurveAdjustments adjustments;
    adjustments.hue = adjustHue;
    adjustments.saturation = adjustSaturation;
    adjustments.brightness = adjustBrightness;
    adjustments.brightnessCurve = adjustBrightnessCurve;
    adjustments.vibrance = adjustVibrance;
    adjustments.minAlpha = adjustMinAlpha;
    adjustments.maxAlpha = adjustMaxAlpha;
    return adjustments;
}

void ColorCurveValue::SetAdjustments(const ColorCurveAdjustments& adjustments,
                                     bool apply) noexcept {
    adjustHue = adjustments.hue;
    adjustSaturation = adjustments.saturation;
    adjustBrightness = adjustments.brightness;
    adjustBrightnessCurve = adjustments.brightnessCurve;
    adjustVibrance = adjustments.vibrance;
    adjustMinAlpha = adjustments.minAlpha;
    adjustMaxAlpha = adjustments.maxAlpha;
    adjusted = apply;
}

Result<void> ColorCurveValue::TryCopyFrom(const ColorCurveValue& other) noexcept {
    if (this == &other) {
        return Result<void>::Success();
    }

    TangentCurve* const targets[] = {&r, &g, &b, &a};
    const TangentCurve* const sources[] = {&other.r, &other.g, &other.b, &other.a};
    if (auto copied = curve_assets_detail::TryCopyChannels(targets, sources); !copied) {
        return copied;
    }

    SetAdjustments(other.Adjustments(), other.adjusted);
    return Result<void>::Success();
}

} // namespace rocks
