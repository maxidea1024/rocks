#pragma once

#include <Core/Math/Quaternion.h>
#include <Core/Math/Vector3.h>
#include <Audio/Panner.h>
#include <AudioAssets/SoundSourceRef.h>

// 리스너 기준 감쇠·패닝 계산이다(Docs/Design/AudioSystem 3.13·3.35.7절). 제어 pipe가 묶음마다 공간
// 재생에 대해 부르는 순수 함수이며, 시험이 바로 단언한다. 좌표·단위는 UE와 같다(+X 전방, +Y 오른쪽,
// +Z 위, cm).
namespace rocks {

// 믹서 voice 파라미터로 옮길 공간 값이다.
struct SpatialParams {
    // 거리 감쇠 선형 이득([0, 1])이다(attenuate가 false면 1).
    float attenuationGain = 1.0f;
    // true면 방위각 equal-power 패닝(mono·stereo), false면 Direct다.
    bool azimuthPan = false;
    // 리스너 앞 0°, 오른쪽 90°인 [0, 360) 방위각이다. stereo 원본이면 L 채널, rightAzimuthDegrees가
    // R 채널이다(stereo spread — 3.42.6절).
    float azimuthDegrees = 0.0f;
    float rightAzimuthDegrees = 0.0f;
    // [0, 1] 비공간화 비율과 그 이득의 배치다(UE NonSpatializedAmount·NonSpatializedRadiusMode).
    float nonSpatializedAmount = 0.0f;
    audio::AudioOmniMode omniMode = audio::AudioOmniMode::OmniDirectional;
    // 공기 흡수 LPF·HPF 차단 주파수(Hz — 3.42.3절)다. 기본값은 믹서가 건너뛴다(3.20.4절).
    float lowPassHz = MaxAttenuationFilterFrequency;
    float highPassHz = 0.0f;
};

// UE NonSpatializedRadius 식(Engine/Private/Audio.cpp:739–779)이다. S = start, E = min(S, end).
// - distance가 0이면(방향 없음) 1이다(S와 상관없이 먼저 본다 — Rocks 규칙).
// - S = 0이면 0이다.
// - E > 0이면 d < S에서 clamp((S − d) / max(S − E, 1), 0, 1), 그 밖은 0이다.
// - E = 0이면 r = clamp(S / d, 0, 10⁶)으로 r > 1에서 1 − 1/r², 그 밖은 0이다.
ROCKS_ENGINEAUDIO_API float NonSpatializedAmount(float distance, float radiusStart,
                                                 float radiusEnd) noexcept;

// 리스너(위치·회전)에서 본 소리(위치·회전)의 공간 값이다(3.42.2·3.42.3절). 상대 위치는 double로 뺀
// 뒤 리스너 회전의 역으로 돌리고 float로 내린다(큰 월드). 거리 감쇠는 모양 감쇠
// (EvaluateShapeAttenuation, 거리 배수 = distance.distanceScale × focusDistanceScale), 공기 흡수는
// attenuateWithLowPass일 때 max(거리 − shapeExtents.x, 0)로 정한다. spatialize면 mono는 방위각
// 하나, stereo는 좌우 방위각(거리 D > 1e-4면 방위각 ∓ deg(atan(0.5 × stereoSpread ÷ D)), 아니면 L
// 270°·R 90° — UE ComputeStereoChannelMap, 7절)이다. 3채널 이상 원본은 Direct에 감쇠만 한다.
ROCKS_ENGINEAUDIO_API SpatialParams ComputeSpatialParams(
    const SoundAttenuation& attenuation, uint32 channelCount, const Vec3d& listenerPosition,
    const Quatd& listenerRotation, const Vec3d& sourcePosition,
    const Quatd& sourceRotation = Quatd::Identity(), float focusDistanceScale = 1.0f) noexcept;

// 가청 판정의 world 최대 거리(cm)다(UE au.MaxWorldDistance = UE_OLD_WORLD_MAX — 3.41.3절). 최대
// 가청 거리가 이 값 이상이면 늘 들린다.
inline constexpr float MaxAudioWorldDistance = 2097152.0f;

// 감쇠 설정의 최대 가청 거리(cm)다(UE FBaseAttenuationSettings::GetMaxDimension —
// Attenuation.cpp:32–59, 3.42.2절): MaxFalloffDistance + ShapeRadius(AudioAttenuation.h)를 [0,
// maxWorldDistance]로 자른다. maxWorldDistance는 audio.virtual.maxworlddistance(기본
// MaxAudioWorldDistance)다.
ROCKS_ENGINEAUDIO_API float
MaxAudibleDistance(const SoundAttenuation& attenuation,
                   float maxWorldDistance = MaxAudioWorldDistance) noexcept;

// 가청 판정에 쓰는 최대 거리(cm)다: MaxAudibleDistance에 Box면 √2를 곱한다(UE
// AudioDevice.cpp:6286–6291 — 자른 뒤에 곱한다). 가상 검사 간격(3.41.3절)도 이 값을 쓴다.
ROCKS_ENGINEAUDIO_API float AudibleRange(const SoundAttenuation& attenuation,
                                         float maxWorldDistance = MaxAudioWorldDistance) noexcept;

// 거리로 본 가청 여부다(UE FAudioVirtualLoop::IsInAudibleRange — AudioVirtualLoop.cpp:239–273,
// AudioDevice.cpp:6093–6131). attenuate가 꺼졌으면 늘 들린다. AudibleRange ÷
// max(focusDistanceScale, 1e-4)가 world 최대 이상이면 늘 들리고, 아니면 그 거리보다 가까우면
// 들린다. 감쇠 스냅숏이 없는 재생·NonSpatial 배치는 호출자가 늘 들리는 것으로 본다.
ROCKS_ENGINEAUDIO_API bool IsWithinAudibleRange(const SoundAttenuation& attenuation,
                                                const Vec3d& listenerPosition,
                                                const Vec3d& sourcePosition,
                                                float maxWorldDistance = MaxAudioWorldDistance,
                                                float focusDistanceScale = 1.0f) noexcept;

} // namespace rocks
