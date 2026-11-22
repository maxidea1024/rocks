#pragma once

#include <Base/Types.h>
#include <Core/Result.h>

// 거리 감쇠 식이다(Docs/Design/AudioSystem.ko.md 3.20.7절). UE
// FBaseAttenuationSettings::AttenuationEval (Engine/Private/Attenuation.cpp)과 같은 식이다. 거리는
// cm(UE 단위)다.
//
// 구 모양 감쇠의 거리는 max(|소리 − 리스너| − innerRadius, 0)이고(SphereAttenuationDistance), 그
// 거리를 EvaluateDistanceAttenuation에 넣는다. 결과는 [0, 1]의 선형 이득이다.
//
// 사용 예:
//   DistanceAttenuationSettings settings;
//   settings.model = DistanceAttenuationModel::NaturalSound;
//   settings.falloffDistance = 3600.0f;
//   const float distance = SphereAttenuationDistance(length, 400.0f);
//   auto gain = EvaluateDistanceAttenuation(settings, distance);
namespace rocks::audio {

enum class DistanceAttenuationModel : uint8 {
    Linear,
    Logarithmic,
    Inverse,
    LogReverse,
    NaturalSound,
    // 커브 감쇠는 AU7에서 Curves 모듈 커브로 더한다. 지금은 Unsupported다.
    Custom
};

// NaturalSound가 falloff 밖에서 하는 일이다(UE ENaturalSoundFalloffMode).
enum class NaturalSoundFalloffMode : uint8 {
    // falloff에서의 값(dBAttenuationAtMax)을 유지한다.
    Hold,
    // falloff 이상이면 0이다.
    Silent,
    // falloff 밖에서도 같은 기울기로 계속 줄어든다.
    Continues
};

// UE KINDA_SMALL_NUMBER다. log·나눗셈의 0 거리를 막는다.
inline constexpr float AttenuationEpsilon = 1.0e-4f;

// falloff 거리의 하한(cm)이다. UE처럼 1보다 짧은 falloff는 1로 본다.
inline constexpr float MinFalloffDistance = 1.0f;

// NaturalSound의 기본 falloff 지점 감쇠(dB)다(UE dBAttenuationAtMax 기본값).
inline constexpr float DefaultNaturalSoundAttenuationDb = -60.0f;

struct DistanceAttenuationSettings {
    DistanceAttenuationModel model = DistanceAttenuationModel::Linear;
    // innerRadius 밖에서 감쇠가 끝나는 거리(cm)다. 0 이상, 1보다 작으면 1로 본다.
    float falloffDistance = 3600.0f;
    // 거리 배율이다. 0 이상.
    float distanceScale = 1.0f;
    // NaturalSound 전용. 0 이하(dB).
    float naturalSoundAttenuationDb = DefaultNaturalSoundAttenuationDb;
    NaturalSoundFalloffMode naturalSoundFalloffMode = NaturalSoundFalloffMode::Continues;
};

// 구 모양 감쇠의 거리(cm)다: max(distance − innerRadius, 0). 음수·NaN 입력은 0이다.
float SphereAttenuationDistance(float distance, float innerRadius) noexcept;

// 감쇠 이득([0, 1])이다.
// - distance: 0 이상 유한(cm). 아니면 InvalidArgument.
// - settings의 falloffDistance·distanceScale이 음수·비유한이거나 naturalSoundAttenuationDb > 0이면
//   InvalidArgument.
// - model이 Custom이면 Unsupported.
Result<float> EvaluateDistanceAttenuation(const DistanceAttenuationSettings& settings,
                                          float distance) noexcept;

} // namespace rocks::audio
