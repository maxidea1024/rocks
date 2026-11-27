#include <Audio/DistanceAttenuation.h>
#include <Audio/Panner.h>
#include <Core/Math/MathUtility.h>
#include <EngineAudio/AudioSpatialization.h>

namespace rocks {

namespace {

// UE MaxNormalizedRadius(Audio.cpp:759)와 비공간화 구간 길이 하한(752)이다.
constexpr float MaxNormalizedOmniRadius = 1000000.0f;
constexpr float MinNonSpatializationRange = 1.0f;
// UE MinAttenuationValueDb(Attenuation.cpp:13)와 NaturalSound 감쇠 dB 상한(ATT 115 — 0으로 나누지
// 않게)이다.
constexpr float AudibleAttenuationFloorDb = -60.0f;
constexpr float MaxNaturalSoundAttenuationDb = -1.0e-4f;

} // namespace

float NonSpatializedAmount(float distance, float radiusStart, float radiusEnd) noexcept {
    if (!(distance > 0.0f)) {
        return 1.0f;
    }

    if (!(radiusStart > 0.0f)) {
        return 0.0f;
    }

    const float end = math::Min(radiusStart, radiusEnd);
    if (end > 0.0f) {
        if (distance >= radiusStart) {
            return 0.0f;
        }

        const float range = math::Max(radiusStart - end, MinNonSpatializationRange);
        return math::Clamp((radiusStart - distance) / range, 0.0f, 1.0f);
    }

    const float normalized = math::Clamp(radiusStart / distance, 0.0f, MaxNormalizedOmniRadius);
    return normalized > 1.0f ? 1.0f - 1.0f / (normalized * normalized) : 0.0f;
}

SpatialParams ComputeSpatialParams(const SoundAttenuation& attenuation, uint32 channelCount,
                                   const Vec3d& listenerPosition, const Quatd& listenerRotation,
                                   const Vec3d& sourcePosition) noexcept {
    SpatialParams params;
    const Vec3d relative = listenerRotation.UnrotateVector(sourcePosition - listenerPosition);
    const Vec3 local(static_cast<float>(relative.x), static_cast<float>(relative.y),
                     static_cast<float>(relative.z));
    const float distance = local.Length();
    if (attenuation.attenuate) {
        const float shaped = audio::SphereAttenuationDistance(distance, attenuation.innerRadius);
        auto gain = audio::EvaluateDistanceAttenuation(attenuation.distance, shaped);
        params.attenuationGain = gain ? gain.Value() : 1.0f;
    }

    if (!attenuation.spatialize || channelCount != 1) {
        return params;
    }

    params.azimuthPan = true;
    params.nonSpatializedAmount = NonSpatializedAmount(
        distance, attenuation.nonSpatializedRadiusStart, attenuation.nonSpatializedRadiusEnd);
    if (distance > 0.0f) {
        params.azimuthDegrees = audio::ComputeAzimuthDegrees(
            Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), local / distance);
    }

    return params;
}

float MaxAudibleDistance(const SoundAttenuation& attenuation) noexcept {
    const audio::DistanceAttenuationSettings& distance = attenuation.distance;
    if (!(distance.distanceScale > 0.0f)) {
        return MaxAudioWorldDistance;
    }

    float falloff = math::Max(distance.falloffDistance, audio::MinFalloffDistance);
    if (falloff > MaxAudioWorldDistance) {
        return MaxAudioWorldDistance;
    }

    if (distance.model == audio::DistanceAttenuationModel::NaturalSound) {
        switch (distance.naturalSoundFalloffMode) {
        case audio::NaturalSoundFalloffMode::Hold:
            return MaxAudioWorldDistance;
        case audio::NaturalSoundFalloffMode::Continues:
            falloff = falloff * AudibleAttenuationFloorDb /
                      math::Min(distance.naturalSoundAttenuationDb, MaxNaturalSoundAttenuationDb);
            break;
        case audio::NaturalSoundFalloffMode::Silent:
            break;
        }
    }

    const float maxDimension = falloff / distance.distanceScale + attenuation.innerRadius;
    return math::Clamp(maxDimension, 0.0f, MaxAudioWorldDistance);
}

bool IsWithinAudibleRange(const SoundAttenuation& attenuation, const Vec3d& listenerPosition,
                          const Vec3d& sourcePosition) noexcept {
    if (!attenuation.attenuate) {
        return true;
    }

    const float maxDistance = MaxAudibleDistance(attenuation);
    if (maxDistance >= MaxAudioWorldDistance) {
        return true;
    }

    const double range = static_cast<double>(maxDistance);
    return (sourcePosition - listenerPosition).LengthSquared() < range * range;
}

} // namespace rocks
