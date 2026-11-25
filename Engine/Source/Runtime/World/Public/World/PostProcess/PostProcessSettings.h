#pragma once

#include <Core/Math/Vector4.h>
#include <Meta/Annotations.h>
#include <World/PostProcess/PostProcessTypes.h>

namespace rocks {

// 후처리 설정이다(UE FPostProcessSettings의 1차 묶음 — Docs/Design/EngineRuntime 27.5절, 기본
// Scene.cpp:398). 값마다 override 플래그(UE bOverride_X, 기본 거짓)가 짝이다: 켜진 값만 아래 층을
// 덮는다(PostProcessBlending.h OverridePostProcessSettings). 색 보정 Vec4는 (r, g, b, 휘도)다.
//
// 사용 예:
//   PostProcessSettings settings;
//   settings.overrideBloomIntensity = true;
//   settings.bloomIntensity = 2.0f;
//   (void)postProcess.SetSettings(settings);
ROCKS_STRUCT()
struct PostProcessSettings {
    ROCKS_GENERATED_BODY()

    // 흰색 균형.
    ROCKS_PROPERTY() bool overrideTemperatureType = false;
    ROCKS_PROPERTY() TemperatureMethod temperatureType = TemperatureMethod::WhiteBalance;
    ROCKS_PROPERTY() bool overrideWhiteTemp = false;
    ROCKS_PROPERTY() float whiteTemp = 6500.0f;
    ROCKS_PROPERTY() bool overrideWhiteTint = false;
    ROCKS_PROPERTY() float whiteTint = 0.0f;
    // 색 보정 전역.
    ROCKS_PROPERTY() bool overrideColorSaturation = false;
    ROCKS_PROPERTY() Vec4d colorSaturation = Vec4d(1.0, 1.0, 1.0, 1.0);
    ROCKS_PROPERTY() bool overrideColorContrast = false;
    ROCKS_PROPERTY() Vec4d colorContrast = Vec4d(1.0, 1.0, 1.0, 1.0);
    ROCKS_PROPERTY() bool overrideColorGamma = false;
    ROCKS_PROPERTY() Vec4d colorGamma = Vec4d(1.0, 1.0, 1.0, 1.0);
    ROCKS_PROPERTY() bool overrideColorGain = false;
    ROCKS_PROPERTY() Vec4d colorGain = Vec4d(1.0, 1.0, 1.0, 1.0);
    ROCKS_PROPERTY() bool overrideColorOffset = false;
    ROCKS_PROPERTY() Vec4d colorOffset = Vec4d(0.0, 0.0, 0.0, 0.0);
    // 톤 매핑.
    ROCKS_PROPERTY() bool overrideTonemappingMethod = false;
    ROCKS_PROPERTY() TonemappingMethod tonemappingMethod = TonemappingMethod::Filmic;
    ROCKS_PROPERTY() bool overrideFilmSlope = false;
    ROCKS_PROPERTY() float filmSlope = 0.88f;
    ROCKS_PROPERTY() bool overrideFilmToe = false;
    ROCKS_PROPERTY() float filmToe = 0.55f;
    ROCKS_PROPERTY() bool overrideFilmShoulder = false;
    ROCKS_PROPERTY() float filmShoulder = 0.26f;
    ROCKS_PROPERTY() bool overrideFilmBlackClip = false;
    ROCKS_PROPERTY() float filmBlackClip = 0.0f;
    ROCKS_PROPERTY() bool overrideFilmWhiteClip = false;
    ROCKS_PROPERTY() float filmWhiteClip = 0.04f;
    // 색 수차·bloom.
    ROCKS_PROPERTY() bool overrideSceneFringeIntensity = false;
    ROCKS_PROPERTY() float sceneFringeIntensity = 0.0f;
    ROCKS_PROPERTY() bool overrideBloomMethod = false;
    ROCKS_PROPERTY() BloomMethod bloomMethod = BloomMethod::Standard;
    ROCKS_PROPERTY() bool overrideBloomIntensity = false;
    ROCKS_PROPERTY() float bloomIntensity = 0.675f;
    ROCKS_PROPERTY() bool overrideBloomThreshold = false;
    ROCKS_PROPERTY() float bloomThreshold = -1.0f;
    ROCKS_PROPERTY() bool overrideBloomSizeScale = false;
    ROCKS_PROPERTY() float bloomSizeScale = 4.0f;
    // 노출. 밝기는 EV100이다(UE5 기본 확장 범위 — Scene.cpp:500, UE4 범위 0.03·8은 레거시라 넣지
    // 않는다).
    ROCKS_PROPERTY() bool overrideAutoExposureMethod = false;
    ROCKS_PROPERTY() AutoExposureMethod autoExposureMethod = AutoExposureMethod::Histogram;
    ROCKS_PROPERTY() bool overrideAutoExposureBias = false;
    ROCKS_PROPERTY() float autoExposureBias = 1.0f;
    ROCKS_PROPERTY() bool overrideAutoExposureMinBrightness = false;
    ROCKS_PROPERTY() float autoExposureMinBrightness = -10.0f;
    ROCKS_PROPERTY() bool overrideAutoExposureMaxBrightness = false;
    ROCKS_PROPERTY() float autoExposureMaxBrightness = 20.0f;
    ROCKS_PROPERTY() bool overrideAutoExposureSpeedUp = false;
    ROCKS_PROPERTY() float autoExposureSpeedUp = 3.0f;
    ROCKS_PROPERTY() bool overrideAutoExposureSpeedDown = false;
    ROCKS_PROPERTY() float autoExposureSpeedDown = 1.0f;
    ROCKS_PROPERTY() bool overrideAutoExposureLowPercent = false;
    ROCKS_PROPERTY() float autoExposureLowPercent = 10.0f;
    ROCKS_PROPERTY() bool overrideAutoExposureHighPercent = false;
    ROCKS_PROPERTY() float autoExposureHighPercent = 90.0f;
    ROCKS_PROPERTY() bool overrideAutoExposureApplyPhysicalCameraExposure = false;
    ROCKS_PROPERTY() bool autoExposureApplyPhysicalCameraExposure = true;
    ROCKS_PROPERTY() bool overrideCameraShutterSpeed = false;
    ROCKS_PROPERTY() float cameraShutterSpeed = 60.0f;
    ROCKS_PROPERTY() bool overrideCameraIso = false;
    ROCKS_PROPERTY() float cameraIso = 100.0f;
    // 렌즈·vignette·grain.
    ROCKS_PROPERTY() bool overrideLensFlareIntensity = false;
    ROCKS_PROPERTY() float lensFlareIntensity = 1.0f;
    ROCKS_PROPERTY() bool overrideVignetteIntensity = false;
    ROCKS_PROPERTY() float vignetteIntensity = 0.4f;
    ROCKS_PROPERTY() bool overrideFilmGrainIntensity = false;
    ROCKS_PROPERTY() float filmGrainIntensity = 0.0f;
    // AO.
    ROCKS_PROPERTY() bool overrideAmbientOcclusionIntensity = false;
    ROCKS_PROPERTY() float ambientOcclusionIntensity = 0.5f;
    ROCKS_PROPERTY() bool overrideAmbientOcclusionRadius = false;
    ROCKS_PROPERTY() float ambientOcclusionRadius = 200.0f;
    // DOF(초점 거리 0 = 끔 — UE "의도적으로 무효").
    ROCKS_PROPERTY() bool overrideDepthOfFieldFocalDistance = false;
    ROCKS_PROPERTY() float depthOfFieldFocalDistance = 0.0f;
    ROCKS_PROPERTY() bool overrideDepthOfFieldFstop = false;
    ROCKS_PROPERTY() float depthOfFieldFstop = 4.0f;
    ROCKS_PROPERTY() bool overrideDepthOfFieldSensorWidth = false;
    ROCKS_PROPERTY() float depthOfFieldSensorWidth = 24.576f;
    // motion blur.
    ROCKS_PROPERTY() bool overrideMotionBlurAmount = false;
    ROCKS_PROPERTY() float motionBlurAmount = 0.5f;
    ROCKS_PROPERTY() bool overrideMotionBlurMax = false;
    ROCKS_PROPERTY() float motionBlurMax = 5.0f;
    ROCKS_PROPERTY() bool overrideMotionBlurTargetFps = false;
    ROCKS_PROPERTY() int32 motionBlurTargetFps = 30;
    ROCKS_PROPERTY() bool overrideMotionBlurPerObjectSize = false;
    ROCKS_PROPERTY() float motionBlurPerObjectSize = 0.0f;

    friend bool operator==(const PostProcessSettings&,
                           const PostProcessSettings&) noexcept = default;
};

// UE ClampMin·ClampMax 범위 안인지다(27.5절 — 비유한 값·범위 밖 enum은 거짓).
ROCKS_WORLD_API bool IsValidPostProcessSettings(const PostProcessSettings& settings) noexcept;

} // namespace rocks
