#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 감쇠 설정의 열거형과 기본값이다(Docs/Design/AudioSystem 3.42.1절, UE FBaseAttenuationSettings·
// FSoundAttenuationSettings — Attenuation.cpp:17–27, SoundAttenuation.cpp:16–90). 에셋
// (AttenuationSettings)과 스냅숏(SoundAttenuation)이 같이 쓴다.
namespace rocks {

// 감쇠 모양이다(UE EAttenuationShape). 모양마다 shapeExtents 성분의 뜻이 다르다(3.42.2절):
// Sphere X = 반경, Capsule X = 반높이·Y = 반지름, Box XYZ = 반폭, Cone X = 반경·Y = 반각(도)·Z =
// 각도 falloff(도).
ROCKS_ENUM()
enum class SoundAttenuationShape : uint8 {
    Sphere = 0,
    Capsule = 1,
    Box = 2,
    Cone = 3
};

// 공기 흡수 주파수를 거리에서 정하는 방법이다(UE EAirAbsorptionMethod).
ROCKS_ENUM()
enum class SoundAbsorptionMethod : uint8 {
    Linear = 0,
    CustomCurve = 1
};

// 우선순위 거리 감쇠 방법이다(UE EPriorityAttenuationMethod).
ROCKS_ENUM()
enum class SoundPriorityAttenuationMethod : uint8 {
    Linear = 0,
    CustomCurve = 1,
    Manual = 2
};

// 비공간화 비율만큼 섞는 이득의 출력 채널 배치다(UE ENonSpatializedRadiusSpeakerMapMode —
// 3.42.6절).
ROCKS_ENUM()
enum class SoundNonSpatializedMode : uint8 {
    // 모든 출력 채널에 1 ÷ 채널 수.
    OmniDirectional = 0,
    // mono는 FL·FR에 mono 업믹스 이득, 스테레오는 L → FL·R → FR.
    Direct2D = 1,
    // Direct2D에 측면·후면 채널을 더한다(스테레오 출력에서는 Direct2D와 같다).
    Surround2D = 2
};

// 감쇠 모양 기본 반경(cm — UE AttenuationShapeExtents (400, 0, 0))이다.
inline constexpr float DefaultAttenuationShapeRadius = 400.0f;
// 공기 흡수 거리 구간(cm — UE LPFRadiusMin·Max)이다.
inline constexpr float DefaultAbsorptionDistanceMin = 3000.0f;
inline constexpr float DefaultAbsorptionDistanceMax = 6000.0f;
// 필터 주파수 범위(Hz — UE MIN_FILTER_FREQUENCY·MAX_FILTER_FREQUENCY, AudioDefines.h:47–48)다.
inline constexpr float MinAttenuationFilterFrequency = 20.0f;
inline constexpr float MaxAttenuationFilterFrequency = 20000.0f;
// 우선순위 감쇠 거리 구간 기본값(cm — UE Ext.X 400, Ext.X + FalloffDistance 4000)이다.
inline constexpr float DefaultPriorityAttenuationDistanceMin = 400.0f;
inline constexpr float DefaultPriorityAttenuationDistanceMax = 4000.0f;
// focus·nonFocus 방위각 기본값(도 — UE FocusAzimuth 30, NonFocusAzimuth 60)과 상한이다.
inline constexpr float DefaultFocusAzimuth = 30.0f;
inline constexpr float DefaultNonFocusAzimuth = 60.0f;
inline constexpr float MaxAttenuationAngleDegrees = 180.0f;
// 스테레오 spread 기본값(cm — UE StereoSpread)이다.
inline constexpr float DefaultStereoSpread = 200.0f;
// 차폐 보간 기본 시간(초 — UE OcclusionInterpolationTime)이다.
inline constexpr float DefaultOcclusionInterpolationSeconds = 0.1f;

} // namespace rocks
