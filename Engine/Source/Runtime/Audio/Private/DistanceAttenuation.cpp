#include <Audio/DistanceAttenuation.h>

#include <Core/Math/MathUtility.h>
#include <Platform/PlatformMath.h>

namespace rocks::audio {

using platform::PlatformMath;

namespace {

// UE Logarithmic·LogReverse의 ln 배율과 Inverse의 분자다(Attenuation.cpp 식 그대로).
constexpr float LogarithmicScale = 0.5f;
constexpr float InverseNumerator = 0.02f;

// dB → 선형 배율(10^(dB/20))의 분모다.
constexpr float DecibelsPerAmplitudeDecade = 20.0f;

bool IsNonNegativeFinite(float value) noexcept {
    return math::IsFinite(value) && value >= 0.0f;
}

float DecibelsToLinear(float decibels) noexcept {
    return PlatformMath::Pow(10.0f, decibels / DecibelsPerAmplitudeDecade);
}

} // namespace

float SphereAttenuationDistance(float distance, float innerRadius) noexcept {
    const float outside = distance - innerRadius;
    // NaN 비교는 거짓이라 NaN도 0으로 간다.
    return outside > 0.0f ? outside : 0.0f;
}

Result<float> EvaluateDistanceAttenuation(const DistanceAttenuationSettings& settings,
                                          float distance) noexcept {
    if (!IsNonNegativeFinite(distance) || !IsNonNegativeFinite(settings.falloffDistance) ||
        !IsNonNegativeFinite(settings.distanceScale) ||
        !math::IsFinite(settings.naturalSoundAttenuationDb) ||
        settings.naturalSoundAttenuationDb > 0.0f) {
        return Result<float>::Failure(ErrorCode::InvalidArgument);
    }

    const float falloff = math::Max(settings.falloffDistance, MinFalloffDistance);
    float scaled = distance * settings.distanceScale;
    float result = 0.0f;
    switch (settings.model) {
    case DistanceAttenuationModel::Linear:
        result = 1.0f - scaled / falloff;
        break;
    case DistanceAttenuationModel::Logarithmic:
        scaled = math::Max(scaled, AttenuationEpsilon);
        result = LogarithmicScale * -PlatformMath::Log(scaled / falloff);
        break;
    case DistanceAttenuationModel::Inverse:
        scaled = math::Max(scaled, AttenuationEpsilon);
        result = InverseNumerator / (scaled / falloff);
        break;
    case DistanceAttenuationModel::LogReverse:
        if (scaled > falloff) {
            result = 0.0f;
        } else {
            const float argument = math::Max(1.0f - scaled / falloff, AttenuationEpsilon);
            result = 1.0f + LogarithmicScale * PlatformMath::Log(argument);
        }

        break;
    case DistanceAttenuationModel::NaturalSound: {
        float alpha = scaled / falloff;
        if (settings.naturalSoundFalloffMode == NaturalSoundFalloffMode::Hold) {
            alpha = math::Clamp(alpha, 0.0f, 1.0f);
            result = DecibelsToLinear(alpha * settings.naturalSoundAttenuationDb);
        } else if (settings.naturalSoundFalloffMode == NaturalSoundFalloffMode::Silent) {
            alpha = math::Clamp(alpha, 0.0f, 1.0f);
            result =
                alpha < 1.0f ? DecibelsToLinear(alpha * settings.naturalSoundAttenuationDb) : 0.0f;
        } else {
            result = DecibelsToLinear(alpha * settings.naturalSoundAttenuationDb);
        }

        break;
    }
    case DistanceAttenuationModel::Custom:
        return Result<float>::Failure(ErrorCode::Unsupported);
    default:
        return Result<float>::Failure(ErrorCode::InvalidArgument);
    }

    // 식의 경계값(0 거리 Inverse 등)이 범위를 넘을 수 있어 UE처럼 자른다.
    return Result<float>::Success(math::Clamp(result, 0.0f, 1.0f));
}

} // namespace rocks::audio
