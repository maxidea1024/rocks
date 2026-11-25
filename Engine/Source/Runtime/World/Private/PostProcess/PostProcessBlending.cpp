#include <Core/Math/MathUtility.h>
#include <World/PostProcess/PostProcessBlending.h>

// 후처리 설정 덮어쓰기다(Docs/Design/EngineRuntime 27.5절, D5).
namespace rocks {

namespace {

// UE LERP_PP: 플래그가 켜졌으면 Lerp(dest, src, w).
void LerpPostProcessIf(bool overridden, float& destination, float source, float weight) noexcept {
    if (overridden) {
        destination = destination + weight * (source - destination);
    }
}

void LerpPostProcessIf(bool overridden, Vec4d& destination, const Vec4d& source,
                       float weight) noexcept {
    if (overridden) {
        const double w = static_cast<double>(weight);
        destination = Vec4d(destination.x + w * (source.x - destination.x),
                            destination.y + w * (source.y - destination.y),
                            destination.z + w * (source.z - destination.z),
                            destination.w + w * (source.w - destination.w));
    }
}

// UE SET_PP: 플래그가 켜졌으면 그대로(이산 값).
template <typename T>
void SetPostProcessIf(bool overridden, T& destination, const T& source) noexcept {
    if (overridden) {
        destination = source;
    }
}

} // namespace

void OverridePostProcessSettings(PostProcessSettings& d, const PostProcessSettings& s,
                                 float weight) noexcept {
    // UE OverridePostProcessSettings(SceneView.cpp:1622): weight > 1은 1로 자르고(+inf 포함 —
    // :1638), 0 이하는 아무것도 하지 않는다. NaN은 값을 망가뜨리므로 무시한다(UE에 없는 방어).
    if (math::IsNaN(weight) || !(weight > 0.0f)) {
        return;
    }

    const float w = weight > 1.0f ? 1.0f : weight;

    SetPostProcessIf(s.overrideTemperatureType, d.temperatureType, s.temperatureType);
    LerpPostProcessIf(s.overrideWhiteTemp, d.whiteTemp, s.whiteTemp, w);
    LerpPostProcessIf(s.overrideWhiteTint, d.whiteTint, s.whiteTint, w);
    LerpPostProcessIf(s.overrideColorSaturation, d.colorSaturation, s.colorSaturation, w);
    LerpPostProcessIf(s.overrideColorContrast, d.colorContrast, s.colorContrast, w);
    LerpPostProcessIf(s.overrideColorGamma, d.colorGamma, s.colorGamma, w);
    LerpPostProcessIf(s.overrideColorGain, d.colorGain, s.colorGain, w);
    LerpPostProcessIf(s.overrideColorOffset, d.colorOffset, s.colorOffset, w);
    SetPostProcessIf(s.overrideTonemappingMethod, d.tonemappingMethod, s.tonemappingMethod);
    LerpPostProcessIf(s.overrideFilmSlope, d.filmSlope, s.filmSlope, w);
    LerpPostProcessIf(s.overrideFilmToe, d.filmToe, s.filmToe, w);
    LerpPostProcessIf(s.overrideFilmShoulder, d.filmShoulder, s.filmShoulder, w);
    LerpPostProcessIf(s.overrideFilmBlackClip, d.filmBlackClip, s.filmBlackClip, w);
    LerpPostProcessIf(s.overrideFilmWhiteClip, d.filmWhiteClip, s.filmWhiteClip, w);
    LerpPostProcessIf(s.overrideSceneFringeIntensity, d.sceneFringeIntensity,
                      s.sceneFringeIntensity, w);
    SetPostProcessIf(s.overrideBloomMethod, d.bloomMethod, s.bloomMethod);
    LerpPostProcessIf(s.overrideBloomIntensity, d.bloomIntensity, s.bloomIntensity, w);
    LerpPostProcessIf(s.overrideBloomThreshold, d.bloomThreshold, s.bloomThreshold, w);
    LerpPostProcessIf(s.overrideBloomSizeScale, d.bloomSizeScale, s.bloomSizeScale, w);
    SetPostProcessIf(s.overrideAutoExposureMethod, d.autoExposureMethod, s.autoExposureMethod);
    LerpPostProcessIf(s.overrideAutoExposureBias, d.autoExposureBias, s.autoExposureBias, w);
    LerpPostProcessIf(s.overrideAutoExposureMinBrightness, d.autoExposureMinBrightness,
                      s.autoExposureMinBrightness, w);
    LerpPostProcessIf(s.overrideAutoExposureMaxBrightness, d.autoExposureMaxBrightness,
                      s.autoExposureMaxBrightness, w);
    LerpPostProcessIf(s.overrideAutoExposureSpeedUp, d.autoExposureSpeedUp, s.autoExposureSpeedUp,
                      w);
    LerpPostProcessIf(s.overrideAutoExposureSpeedDown, d.autoExposureSpeedDown,
                      s.autoExposureSpeedDown, w);
    LerpPostProcessIf(s.overrideAutoExposureLowPercent, d.autoExposureLowPercent,
                      s.autoExposureLowPercent, w);
    LerpPostProcessIf(s.overrideAutoExposureHighPercent, d.autoExposureHighPercent,
                      s.autoExposureHighPercent, w);
    SetPostProcessIf(s.overrideAutoExposureApplyPhysicalCameraExposure,
                     d.autoExposureApplyPhysicalCameraExposure,
                     s.autoExposureApplyPhysicalCameraExposure);
    LerpPostProcessIf(s.overrideCameraShutterSpeed, d.cameraShutterSpeed, s.cameraShutterSpeed, w);
    LerpPostProcessIf(s.overrideCameraIso, d.cameraIso, s.cameraIso, w);
    LerpPostProcessIf(s.overrideLensFlareIntensity, d.lensFlareIntensity, s.lensFlareIntensity, w);
    LerpPostProcessIf(s.overrideVignetteIntensity, d.vignetteIntensity, s.vignetteIntensity, w);
    LerpPostProcessIf(s.overrideFilmGrainIntensity, d.filmGrainIntensity, s.filmGrainIntensity, w);
    LerpPostProcessIf(s.overrideAmbientOcclusionIntensity, d.ambientOcclusionIntensity,
                      s.ambientOcclusionIntensity, w);
    LerpPostProcessIf(s.overrideAmbientOcclusionRadius, d.ambientOcclusionRadius,
                      s.ambientOcclusionRadius, w);
    // 초점 거리 0은 "DOF 끔" 표지라 어느 쪽이든 0이면 보간하지 않고 그대로 쓴다(UE :1780).
    if (s.overrideDepthOfFieldFocalDistance) {
        if (d.depthOfFieldFocalDistance == 0.0f || s.depthOfFieldFocalDistance == 0.0f) {
            d.depthOfFieldFocalDistance = s.depthOfFieldFocalDistance;
        } else {
            LerpPostProcessIf(true, d.depthOfFieldFocalDistance, s.depthOfFieldFocalDistance, w);
        }
    }

    LerpPostProcessIf(s.overrideDepthOfFieldFstop, d.depthOfFieldFstop, s.depthOfFieldFstop, w);
    LerpPostProcessIf(s.overrideDepthOfFieldSensorWidth, d.depthOfFieldSensorWidth,
                      s.depthOfFieldSensorWidth, w);
    LerpPostProcessIf(s.overrideMotionBlurAmount, d.motionBlurAmount, s.motionBlurAmount, w);
    LerpPostProcessIf(s.overrideMotionBlurMax, d.motionBlurMax, s.motionBlurMax, w);
    // 정수 값은 이산으로 둔다(보간하면 반올림 규칙이 생긴다).
    SetPostProcessIf(s.overrideMotionBlurTargetFps, d.motionBlurTargetFps, s.motionBlurTargetFps);
    LerpPostProcessIf(s.overrideMotionBlurPerObjectSize, d.motionBlurPerObjectSize,
                      s.motionBlurPerObjectSize, w);
}

} // namespace rocks
