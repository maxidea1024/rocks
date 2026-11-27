#pragma once

#include <AudioAssets/SoundSourceRef.h>
#include <EngineAudio/GlobalFocusSettings.h>
#include <Core/Math/Quaternion.h>
#include <Core/Math/Vector3.h>

// 감쇠 순수 함수다(Docs/Design/AudioSystem 3.42절, UE FBaseAttenuationSettings·FActiveSound 감쇠
// 경로). 제어 pipe가 묶음마다 부르고, 시험이 바로 단언한다. 좌표·단위는 UE와 같다(+X 전방, +Y
// 오른쪽, +Z 위, cm, 도). 위치는 double로 빼고 float로 내린다(큰 월드).
//
// 사용 예:
//   const float gain = EvaluateShapeAttenuation(attenuation, listener, source, rotation, 1.0f);
//   const float lowPass = AbsorptionFrequency(attenuation, distance, false);
namespace rocks {

// 거리 모델 평가다(UE AttenuationEval — Attenuation.cpp:170–258): falloff는 1 미만이면 1, 거리에
// scale을 곱하고 [0, 1]로 자른다. Custom이면 clamp(curve(d × scale ÷ falloff), 0, 1)이고 음수
// 거리도 그대로 넘긴다(Capsule 안쪽·Cone 각도 — 다른 모델은 0과 같은 결과다).
ROCKS_ENGINEAUDIO_API float EvaluateAttenuationModel(const SoundAttenuation& attenuation,
                                                     float distance, float falloff,
                                                     float scale) noexcept;

// 모양 감쇠다(UE Evaluate — Attenuation.cpp:138–330, 3.42.2절). sourceRotation은 소리의 world
// 회전이다(scale 없음). scale은 거리 배수(distance.distanceScale × focus Distance 배수)다.
// attenuate 여부는 호출자가 본다.
ROCKS_ENGINEAUDIO_API float EvaluateShapeAttenuation(const SoundAttenuation& attenuation,
                                                     const Vec3d& listenerPosition,
                                                     const Vec3d& sourcePosition,
                                                     const Quatd& sourceRotation,
                                                     float scale) noexcept;

// 최대 감쇠 거리(cm — UE GetMaxFalloffDistance, Attenuation.cpp:61–136)다. falloffDistance(1 미만은
// 1)를 distance.distanceScale로 나눈 값, NaturalSound Hold·distanceScale 0은 world 최대,
// NaturalSound Continues는 −60 dB 지점, Custom은 마지막 키(Time > 0 중 가장 큰 Time)의 값이 1e-3
// 안이면 falloff × 그 Time(키가 없으면 0), 아니면 world 최대다. falloff가 world 최대를 넘으면 world
// 최대다.
ROCKS_ENGINEAUDIO_API float MaxFalloffDistance(const SoundAttenuation& attenuation,
                                               float maxWorldDistance) noexcept;

// 모양 반경(cm — UE GetMaxDimension의 모양 항)이다: Sphere·Cone X, Box max(X, Y, Z), Capsule
// max(X, Y). Cone은 구 부분을 보지 않는다(UE 그대로).
ROCKS_ENGINEAUDIO_API float ShapeRadius(const SoundAttenuation& attenuation) noexcept;

// 공기 흡수 주파수(Hz — UE GetAttenuationFrequency, ActiveSound.cpp:1845–1906, 3.42.3절)다.
// attenuationDistance는 max(|L − O| − shapeExtents.x, 0)(모양과 상관없다). highPass면 HPF 칸과
// 곡선을 쓴다. 결과는 [20, 20000]이다.
ROCKS_ENGINEAUDIO_API float AbsorptionFrequency(const SoundAttenuation& attenuation,
                                                float attenuationDistance, bool highPass) noexcept;

// 우선순위 거리 감쇠 배수(UE ActiveSound.cpp:2080–2103, 3.42.4절)다. listenerDistance는 모양
// 반경을 빼지 않은 거리다. enablePriorityAttenuation이 꺼져 있으면 1이다. 결과는 0 이상이다.
ROCKS_ENGINEAUDIO_API float PriorityAttenuationScale(const SoundAttenuation& attenuation,
                                                     float listenerDistance) noexcept;

// 리스너 기준 방위각(도, [0, 180] — UE FAudioDevice::GetAzimuth의 Azimuth)이다. 방향이 0
// 벡터(같은 위치)면 UE처럼 90°다.
ROCKS_ENGINEAUDIO_API float FocusAzimuthDegrees(const Vec3d& listenerPosition,
                                                const Quatd& listenerRotation,
                                                const Vec3d& sourcePosition) noexcept;

// focus 계수(0 = focus 안, 1 = 밖 — UE GetFocusFactor, AudioDevice.cpp:6513–6532)다.
ROCKS_ENGINEAUDIO_API float FocusFactor(const SoundAttenuation& attenuation,
                                        const GlobalFocusSettings& global,
                                        float azimuthDegrees) noexcept;

// focus 배수다(UE FSoundAttenuationSettings::GetFocus*Scale — SoundAttenuation.cpp:156–178).
struct FocusScales {
    float distance = 1.0f;
    float priority = 1.0f;
    float volume = 1.0f;
};

// max(0, lerp(전역 Focus × focus, 전역 NonFocus × nonFocus, factor))를 셋 각각.
ROCKS_ENGINEAUDIO_API FocusScales ComputeFocusScales(const SoundAttenuation& attenuation,
                                                     const GlobalFocusSettings& global,
                                                     float factor) noexcept;

// focus 계수 보간(UE FInterpTo — UnrealMathUtility.h:1509–1531)이다: target ≤ current면 attack,
// 아니면 release 속도. speed ≤ 0이거나 차² < 1e-8이면 target이다.
ROCKS_ENGINEAUDIO_API float InterpolateFocusFactor(float current, float target, float deltaSeconds,
                                                   float attackSpeed, float releaseSpeed) noexcept;

} // namespace rocks
