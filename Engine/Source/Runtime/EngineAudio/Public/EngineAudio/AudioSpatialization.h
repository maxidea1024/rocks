#pragma once

#include <Core/Math/Quaternion.h>
#include <Core/Math/Vector3.h>
#include <AudioAssets/SoundSourceRef.h>

// 리스너 기준 감쇠·패닝 계산이다(Docs/Design/AudioSystem 3.13·3.35.7절). 제어 pipe가 묶음마다 공간
// 재생에 대해 부르는 순수 함수이며, 시험이 바로 단언한다. 좌표·단위는 UE와 같다(+X 전방, +Y 오른쪽,
// +Z 위, cm).
namespace rocks {

// 믹서 voice 파라미터로 옮길 공간 값이다.
struct SpatialParams {
    // 거리 감쇠 선형 이득([0, 1])이다(attenuate가 false면 1).
    float attenuationGain = 1.0f;
    // true면 방위각 equal-power 패닝(mono만), false면 Direct다.
    bool azimuthPan = false;
    // 리스너 앞 0°, 오른쪽 90°인 [0, 360) 방위각이다.
    float azimuthDegrees = 0.0f;
    // [0, 1] 비공간화 비율이다(UE NonSpatializedAmount).
    float nonSpatializedAmount = 0.0f;
};

// UE NonSpatializedRadius 식(Engine/Private/Audio.cpp:739–779)이다. S = start, E = min(S, end).
// - distance가 0이면(방향 없음) 1이다(S와 상관없이 먼저 본다 — Rocks 규칙).
// - S = 0이면 0이다.
// - E > 0이면 d < S에서 clamp((S − d) / max(S − E, 1), 0, 1), 그 밖은 0이다.
// - E = 0이면 r = clamp(S / d, 0, 10⁶)으로 r > 1에서 1 − 1/r², 그 밖은 0이다.
ROCKS_ENGINEAUDIO_API float NonSpatializedAmount(float distance, float radiusStart,
                                                 float radiusEnd) noexcept;

// 리스너(위치·회전)에서 본 소리(위치)의 공간 값이다. 상대 위치는 double로 뺀 뒤 리스너 회전의
// 역으로 돌리고 float로 내린다(큰 월드). channelCount가 2(stereo 원본)면 Direct에 거리 감쇠만
// 한다(stereo spread는 AU7). 감쇠 평가가 실패하는 값은 SoundSourceRef 검사가 이미 막는다(실패하면
// 이득 1).
ROCKS_ENGINEAUDIO_API SpatialParams ComputeSpatialParams(const SoundAttenuation& attenuation,
                                                         uint32 channelCount,
                                                         const Vec3d& listenerPosition,
                                                         const Quatd& listenerRotation,
                                                         const Vec3d& sourcePosition) noexcept;

// 가청 판정의 world 최대 거리(cm)다(UE au.MaxWorldDistance = UE_OLD_WORLD_MAX — 3.41.3절). 최대
// 가청 거리가 이 값 이상이면 늘 들린다.
inline constexpr float MaxAudioWorldDistance = 2097152.0f;

// 감쇠 설정의 최대 가청 거리(cm)다(UE FBaseAttenuationSettings::GetMaxDimension —
// Attenuation.cpp:32–120, 모양은 구). 최대 감쇠 거리 + innerRadius를 [0, MaxAudioWorldDistance]로
// 자른다. 최대 감쇠 거리는 falloffDistance(distanceScale로 나눔)이고, NaturalSound Hold면 world
// 최대, NaturalSound Continues면 falloffDistance × (−60 dB ÷ min(naturalSoundAttenuationDb,
// −1e-4))다. distanceScale이 0이면 감쇠하지 않으므로 world 최대다.
ROCKS_ENGINEAUDIO_API float MaxAudibleDistance(const SoundAttenuation& attenuation) noexcept;

// 거리로 본 가청 여부다(UE FAudioVirtualLoop::IsInAudibleRange — AudioVirtualLoop.cpp:239–273).
// attenuate가 꺼졌거나 최대 가청 거리가 world 최대 이상이면 늘 들리고, 아니면 리스너까지 거리 <
// 최대 가청 거리다. 감쇠 스냅숏이 없는 재생·NonSpatial 배치는 호출자가 늘 들리는 것으로 본다.
ROCKS_ENGINEAUDIO_API bool IsWithinAudibleRange(const SoundAttenuation& attenuation,
                                                const Vec3d& listenerPosition,
                                                const Vec3d& sourcePosition) noexcept;

} // namespace rocks
