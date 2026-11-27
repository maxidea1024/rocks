#include <Audio/DistanceAttenuation.h>
#include <Audio/Panner.h>
#include <Core/Math/MathUtility.h>
#include <Platform/PlatformMath.h>
#include <EngineAudio/AudioAttenuation.h>
#include <EngineAudio/AudioSpatialization.h>

namespace rocks {

namespace {

using platform::PlatformMath;

// UE MaxNormalizedRadius(Audio.cpp:759)와 비공간화 구간 길이 하한(752)이다.
constexpr float MaxNormalizedOmniRadius = 1000000.0f;
constexpr float MinNonSpatializationRange = 1.0f;
// 상자 가청 거리 배수(UE UE_SQRT_2)다.
constexpr float BoxAudibleDistanceFactor = 1.41421356f;
// stereo spread 영거리(UE KINDA_SMALL_NUMBER 이하)의 좌우 방위각과 한 바퀴(도)다.
constexpr float StereoLeftAzimuth = 270.0f;
constexpr float StereoRightAzimuth = 90.0f;
constexpr float FullCircleDegrees = 360.0f;

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
                                   const Vec3d& sourcePosition, const Quatd& sourceRotation,
                                   float focusDistanceScale) noexcept {
    SpatialParams params;
    const Vec3d relative = listenerRotation.UnrotateVector(sourcePosition - listenerPosition);
    const Vec3 local(static_cast<float>(relative.x), static_cast<float>(relative.y),
                     static_cast<float>(relative.z));
    const float distance = local.Length();
    if (attenuation.attenuate) {
        params.attenuationGain =
            EvaluateShapeAttenuation(attenuation, listenerPosition, sourcePosition, sourceRotation,
                                     attenuation.distance.distanceScale * focusDistanceScale);
    }

    // 공기 흡수는 모양과 상관없이 구 반경을 뺀 거리로 본다(UE AD 313–316).
    if (attenuation.attenuateWithLowPass) {
        const float absorptionDistance = math::Max(distance - attenuation.shapeExtents.x, 0.0f);
        params.lowPassHz = AbsorptionFrequency(attenuation, absorptionDistance, false);
        params.highPassHz = AbsorptionFrequency(attenuation, absorptionDistance, true);
    }

    if (!attenuation.spatialize || channelCount == 0 ||
        channelCount > audio::MaxAzimuthSourceChannels) {
        return params;
    }

    params.azimuthPan = true;
    params.omniMode = static_cast<audio::AudioOmniMode>(attenuation.nonSpatializedMode);
    params.nonSpatializedAmount = NonSpatializedAmount(
        distance, attenuation.nonSpatializedRadiusStart, attenuation.nonSpatializedRadiusEnd);
    float azimuth = 0.0f;
    if (distance > 0.0f) {
        azimuth = audio::ComputeAzimuthDegrees(Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f),
                                               local / distance);
    }

    params.azimuthDegrees = azimuth;
    if (channelCount == 1) {
        return params;
    }

    // stereo spread(UE ComputeStereoChannelMap — AudioMixerSource.cpp:2172–2210). 영거리는 좌우를
    // 맞춘다(UE는 L 90°·R 270° — 7절).
    if (distance > math::KindaSmallNumber<float>) {
        const float offset = static_cast<float>(math::RadiansToDegrees(PlatformMath::Atan(
            0.5 * static_cast<double>(attenuation.stereoSpread) / static_cast<double>(distance))));
        float left = azimuth - offset;
        if (left < 0.0f) {
            left += FullCircleDegrees;
        }

        float right = azimuth + offset;
        if (right > FullCircleDegrees) {
            right -= FullCircleDegrees;
        }

        params.azimuthDegrees = left;
        params.rightAzimuthDegrees = right;
    } else {
        params.azimuthDegrees = StereoLeftAzimuth;
        params.rightAzimuthDegrees = StereoRightAzimuth;
    }

    return params;
}

float MaxAudibleDistance(const SoundAttenuation& attenuation, float maxWorldDistance) noexcept {
    const float maxDimension =
        MaxFalloffDistance(attenuation, maxWorldDistance) + ShapeRadius(attenuation);
    return math::Clamp(maxDimension, 0.0f, maxWorldDistance);
}

float AudibleRange(const SoundAttenuation& attenuation, float maxWorldDistance) noexcept {
    const float maxDistance = MaxAudibleDistance(attenuation, maxWorldDistance);
    // 상자는 꼭짓점까지 닿게 자른 뒤에 √2배다(UE AudioDevice.cpp:6286–6291).
    return attenuation.shape == SoundAttenuationShape::Box ? maxDistance * BoxAudibleDistanceFactor
                                                           : maxDistance;
}

bool IsWithinAudibleRange(const SoundAttenuation& attenuation, const Vec3d& listenerPosition,
                          const Vec3d& sourcePosition, float maxWorldDistance,
                          float focusDistanceScale) noexcept {
    if (!attenuation.attenuate) {
        return true;
    }

    // focus 배수로 나눈 뒤 world 최대와 비교한다(UE LocationIsAudible — AVL 271, AD 6095).
    const double range =
        static_cast<double>(AudibleRange(attenuation, maxWorldDistance)) /
        static_cast<double>(math::Max(focusDistanceScale, math::KindaSmallNumber<float>));
    if (range >= static_cast<double>(maxWorldDistance)) {
        return true;
    }

    return (sourcePosition - listenerPosition).LengthSquared() < range * range;
}

} // namespace rocks
