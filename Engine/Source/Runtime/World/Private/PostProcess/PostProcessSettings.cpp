#include <Core/Math/MathUtility.h>
#include <World/PostProcess/PostProcessSettings.h>
#include <World/ComponentValueChecks.h>

// 후처리 설정 범위 검사다(Docs/Design/EngineRuntime 27.5절, D5).
namespace rocks {

namespace {

// UE FPostProcessSettings ClampMin·ClampMax(Scene.h)다. UIMin뿐인 값(색 수차·렌즈 flare·vignette·
// grain 등)은 유한이기만 하면 된다.
constexpr float PostProcessMinBloomThreshold = -1.0f;
constexpr float PostProcessMinExposureBrightness = -10.0f;
constexpr float PostProcessMinExposureSpeed = 0.02f;
constexpr float PostProcessMaxPercent = 100.0f;
constexpr float PostProcessMinShutterSpeed = 0.01f;
constexpr float PostProcessMaxShutterSpeed = 8000.0f;
constexpr float PostProcessMinCameraIso = 1.0f;
constexpr float PostProcessMinAmbientOcclusionRadius = 0.1f;
constexpr float PostProcessMinFstop = 1.0f;
constexpr float PostProcessMaxFstop = 32.0f;
constexpr float PostProcessMinSensorWidth = 0.1f;
constexpr float PostProcessMaxMotionBlurPercent = 100.0f;
constexpr int32 PostProcessMaxMotionBlurTargetFps = 120;

bool FinitePostProcessVector(const Vec4d& value) noexcept {
    return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z) &&
           math::IsFinite(value.w);
}

} // namespace

bool IsValidPostProcessSettings(const PostProcessSettings& s) noexcept {
    using component_checks::AtLeast;
    using component_checks::InRange;
    return static_cast<uint8>(s.temperatureType) <=
               static_cast<uint8>(TemperatureMethod::ColorTemperature) &&
           math::IsFinite(s.whiteTemp) && math::IsFinite(s.whiteTint) &&
           FinitePostProcessVector(s.colorSaturation) && FinitePostProcessVector(s.colorContrast) &&
           FinitePostProcessVector(s.colorGamma) && FinitePostProcessVector(s.colorGain) &&
           FinitePostProcessVector(s.colorOffset) &&
           static_cast<uint8>(s.tonemappingMethod) <=
               static_cast<uint8>(TonemappingMethod::StandardAces) &&
           math::IsFinite(s.filmSlope) && math::IsFinite(s.filmToe) &&
           math::IsFinite(s.filmShoulder) && math::IsFinite(s.filmBlackClip) &&
           math::IsFinite(s.filmWhiteClip) && math::IsFinite(s.sceneFringeIntensity) &&
           static_cast<uint8>(s.bloomMethod) <= static_cast<uint8>(BloomMethod::Convolution) &&
           AtLeast(s.bloomIntensity, 0.0f) &&
           AtLeast(s.bloomThreshold, PostProcessMinBloomThreshold) &&
           AtLeast(s.bloomSizeScale, 0.0f) &&
           static_cast<uint8>(s.autoExposureMethod) <=
               static_cast<uint8>(AutoExposureMethod::Manual) &&
           math::IsFinite(s.autoExposureBias) &&
           AtLeast(s.autoExposureMinBrightness, PostProcessMinExposureBrightness) &&
           AtLeast(s.autoExposureMaxBrightness, PostProcessMinExposureBrightness) &&
           AtLeast(s.autoExposureSpeedUp, PostProcessMinExposureSpeed) &&
           AtLeast(s.autoExposureSpeedDown, PostProcessMinExposureSpeed) &&
           InRange(s.autoExposureLowPercent, 0.0f, PostProcessMaxPercent) &&
           InRange(s.autoExposureHighPercent, 0.0f, PostProcessMaxPercent) &&
           InRange(s.cameraShutterSpeed, PostProcessMinShutterSpeed, PostProcessMaxShutterSpeed) &&
           AtLeast(s.cameraIso, PostProcessMinCameraIso) && math::IsFinite(s.lensFlareIntensity) &&
           math::IsFinite(s.vignetteIntensity) && math::IsFinite(s.filmGrainIntensity) &&
           InRange(s.ambientOcclusionIntensity, 0.0f, 1.0f) &&
           AtLeast(s.ambientOcclusionRadius, PostProcessMinAmbientOcclusionRadius) &&
           AtLeast(s.depthOfFieldFocalDistance, 0.0f) &&
           InRange(s.depthOfFieldFstop, PostProcessMinFstop, PostProcessMaxFstop) &&
           AtLeast(s.depthOfFieldSensorWidth, PostProcessMinSensorWidth) &&
           InRange(s.motionBlurAmount, 0.0f, 1.0f) &&
           InRange(s.motionBlurMax, 0.0f, PostProcessMaxMotionBlurPercent) &&
           s.motionBlurTargetFps >= 0 &&
           s.motionBlurTargetFps <= PostProcessMaxMotionBlurTargetFps &&
           AtLeast(s.motionBlurPerObjectSize, 0.0f);
}

} // namespace rocks
