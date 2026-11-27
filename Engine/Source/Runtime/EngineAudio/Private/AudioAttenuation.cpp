#include <Core/Math/IntersectionDistance.h>
#include <Core/Math/InterpTo.h>
#include <Core/Math/MathUtility.h>
#include <Core/Math/Shapes.h>
#include <Audio/Panner.h>
#include <EngineAudio/AudioAttenuation.h>
#include <Platform/PlatformMath.h>

// 감쇠 순수 함수 구현이다(Docs/Design/AudioSystem 3.42절). 식과 연산 순서는 UE 5.8을 따른다(인용은
// 함수마다).
namespace rocks {

static_assert(static_cast<uint8>(SoundNonSpatializedMode::OmniDirectional) ==
              static_cast<uint8>(audio::AudioOmniMode::OmniDirectional));
static_assert(static_cast<uint8>(SoundNonSpatializedMode::Direct2D) ==
              static_cast<uint8>(audio::AudioOmniMode::Direct2D));
static_assert(static_cast<uint8>(SoundNonSpatializedMode::Surround2D) ==
              static_cast<uint8>(audio::AudioOmniMode::Surround2D));

namespace {

using platform::PlatformMath;

// UE BIG_NUMBER(로그 상한)와 MinAttenuationValue(Attenuation.cpp:12)다.
constexpr double LogRangeBigNumber = 3.4e+38;
constexpr float MinAttenuationValue = 1.0e-3f;
// UE MinAttenuationValueDb(Attenuation.cpp:13)와 NaturalSound 감쇠 dB 상한(0으로 나누지 않게)이다.
constexpr float AudibleAttenuationFloorDb = -60.0f;
constexpr float MaxNaturalSoundAttenuationDb = -1.0e-4f;
// 방향이 없을 때의 방위각(도 — acos(0))이다.
constexpr float UndefinedDirectionAzimuth = 90.0f;
constexpr float FullAngleDegrees = 180.0f;

Vec3 ToFloat(const Vec3d& value) noexcept {
    return Vec3(static_cast<float>(value.x), static_cast<float>(value.y),
                static_cast<float>(value.z));
}

// UE FMath::GetMappedRangeValueClamped(입력 구간, 출력 구간, 값)이다.
float MapClamped(float inMin, float inMax, float outMin, float outMax, float value) noexcept {
    const float divisor = inMax - inMin;
    const float alpha = divisor != 0.0f ? math::Clamp((value - inMin) / divisor, 0.0f, 1.0f)
                                        : (value >= inMax ? 1.0f : 0.0f);
    return math::Lerp(outMin, outMax, alpha);
}

// UE Audio::GetLogFrequencyClamped(Dsp.h:334–355)다. 끝점은 범위 끝값, 사이는 로그 구간의 선형
// 사상을 exp한다(로그 하한·상한 보정 그대로).
float LogFrequencyClamped(float value, float domainMin, float domainMax, float rangeMin,
                          float rangeMax) noexcept {
    if (value <= domainMin) {
        return rangeMin;
    }

    if (value >= domainMax) {
        return rangeMax;
    }

    if (!math::IsFinite(value)) {
        return rangeMin;
    }

    const double logMin =
        math::Max(PlatformMath::Log(static_cast<double>(rangeMin)), math::SmallNumber<double>);
    const double logMax =
        math::Min(PlatformMath::Log(static_cast<double>(rangeMax)), LogRangeBigNumber);
    const double alpha = (static_cast<double>(value) - domainMin) /
                         (static_cast<double>(domainMax) - static_cast<double>(domainMin));
    return static_cast<float>(PlatformMath::Exp(logMin + (logMax - logMin) * alpha));
}

// 점과 원점 중심 상자 [−extents, +extents] 사이 거리²다(UE ComputeSquaredDistanceFromBoxToPoint —
// Core 거리 질의, 값은 ValidateSoundAttenuation이 유한·0 이상으로 지켰다).
float BoxDistanceSquared(const Vec3& extents, const Vec3& point) noexcept {
    auto squared = TryDistanceSquared(Box<float>(-extents, extents), point);
    return squared ? squared.Value() : 0.0f;
}

// 점과 선분 사이 거리다(UE FMath::PointDistToSegment — Core 거리 질의).
float PointSegmentDistance(const Vec3& point, const Vec3& start, const Vec3& end) noexcept {
    auto squared = TryDistanceSquared(Segment<float>(start, end), point);
    return squared ? PlatformMath::Sqrt(squared.Value()) : 0.0f;
}

} // namespace

bool IsValidGlobalFocusSettings(const GlobalFocusSettings& settings) noexcept {
    const float values[] = {settings.focusAzimuthScale,  settings.nonFocusAzimuthScale,
                            settings.focusDistanceScale, settings.nonFocusDistanceScale,
                            settings.focusVolumeScale,   settings.nonFocusVolumeScale,
                            settings.focusPriorityScale, settings.nonFocusPriorityScale};
    for (const float value : values) {
        if (!math::IsFinite(value) || value < 0.0f) {
            return false;
        }
    }

    return true;
}

float EvaluateAttenuationModel(const SoundAttenuation& attenuation, float distance, float falloff,
                               float scale) noexcept {
    if (attenuation.distance.model == audio::DistanceAttenuationModel::Custom) {
        // UE Attenuation.cpp:245–248 — X축은 falloff로 정규화한 거리다.
        const float limit = math::Max(falloff, audio::MinFalloffDistance);
        const TangentCurve* curve = attenuation.customDistanceCurve.Get();
        const float value = curve != nullptr ? curve->Evaluate(distance * scale / limit) : 0.0f;
        return math::Clamp(value, 0.0f, 1.0f);
    }

    // 음수 거리(Capsule 안쪽·Cone 각도 안)는 다른 모델에서 0과 결과가 같다(모두 1로 잘린다).
    audio::DistanceAttenuationSettings settings = attenuation.distance;
    settings.falloffDistance = math::Max(falloff, 0.0f);
    settings.distanceScale = scale;
    const float clamped = distance > 0.0f ? distance : 0.0f;
    auto gain = audio::EvaluateDistanceAttenuation(settings, clamped);
    return gain ? gain.Value() : 1.0f;
}

float EvaluateShapeAttenuation(const SoundAttenuation& attenuation, const Vec3d& listenerPosition,
                               const Vec3d& sourcePosition, const Quatd& sourceRotation,
                               float scale) noexcept {
    const Vec3 extents = attenuation.shapeExtents;
    const float falloff = attenuation.distance.falloffDistance;
    const Vec3d relative = listenerPosition - sourcePosition;
    switch (attenuation.shape) {
    case SoundAttenuationShape::Sphere: {
        const float distance = static_cast<float>(relative.Length()) - extents.x;
        return EvaluateAttenuationModel(attenuation, math::Max(distance, 0.0f), falloff, scale);
    }
    case SoundAttenuationShape::Box: {
        // 소리 지역 공간(회전만 — UE InverseTransformPositionNoScale)에서 잰다. 차단 경계에는 거리
        // 배수가 들어가지 않는다(Attenuation.cpp:260–269).
        const Vec3 local = ToFloat(sourceRotation.UnrotateVector(relative));
        const float squared = BoxDistanceSquared(extents, local);
        if (math::Abs(squared) <= math::SmallNumber<float> || squared < falloff * falloff) {
            return EvaluateAttenuationModel(attenuation, PlatformMath::Sqrt(squared), falloff,
                                            scale);
        }

        return 0.0f;
    }
    case SoundAttenuationShape::Capsule: {
        // X = 반높이, Y = 반지름. 안쪽은 음수 거리다(Attenuation.cpp:271–292).
        const float halfHeight = extents.x;
        const float radius = extents.y;
        float distance = 0.0f;
        if (halfHeight <= radius) {
            distance = math::Max(static_cast<float>(relative.Length()) - radius, 0.0f);
        } else {
            const Vec3 axis = ToFloat(sourceRotation.RotateVector(Vec3d(0.0, 0.0, 1.0)));
            const Vec3 offset = axis * (halfHeight - radius);
            const Vec3 point = ToFloat(relative);
            distance = PointSegmentDistance(point, offset, -offset) - radius;
        }

        return EvaluateAttenuationModel(attenuation, distance, falloff, scale);
    }
    case SoundAttenuationShape::Cone: {
        // X = 반경, Y = 반각, Z = 각도 falloff(Attenuation.cpp:294–330).
        const Vec3 forward = ToFloat(sourceRotation.RotateVector(Vec3d(1.0, 0.0, 0.0)));
        const Vec3 toListener = ToFloat(relative) + forward * attenuation.coneOffset;
        const float length = toListener.Length();
        float sphere = 0.0f;
        if (math::Abs(attenuation.coneSphereRadius) > math::SmallNumber<float>) {
            sphere = EvaluateAttenuationModel(
                attenuation, math::Max(length - attenuation.coneSphereRadius, 0.0f),
                attenuation.coneSphereFalloffDistance, scale);
        }

        if (attenuation.coneSphereRadius >= extents.x) {
            return sphere;
        }

        float gain = EvaluateAttenuationModel(attenuation, math::Max(length - extents.x, 0.0f),
                                              falloff, scale);
        if (gain > 0.0f) {
            // 원뿔 원점(방향 없음)은 원뿔 안으로 본다(Rocks — 3.42.2절, 7절).
            float theta = 0.0f;
            if (length > 0.0f) {
                const float cosine = math::Clamp(Dot(forward, toListener) / length, -1.0f, 1.0f);
                theta = static_cast<float>(math::RadiansToDegrees(
                    math::Abs(PlatformMath::Acos(static_cast<double>(cosine)))));
            }

            gain *= EvaluateAttenuationModel(attenuation, theta - extents.y, extents.z, 1.0f);
        }

        return math::Max(gain, sphere);
    }
    }

    return 1.0f;
}

float MaxFalloffDistance(const SoundAttenuation& attenuation, float maxWorldDistance) noexcept {
    const audio::DistanceAttenuationSettings& distance = attenuation.distance;
    if (!(distance.distanceScale > 0.0f)) {
        return maxWorldDistance;
    }

    float falloff = math::Max(distance.falloffDistance, audio::MinFalloffDistance);
    if (falloff > maxWorldDistance) {
        return maxWorldDistance;
    }

    switch (distance.model) {
    case audio::DistanceAttenuationModel::Custom: {
        // 마지막 키(Time > 0 중 가장 큰 Time)의 값이 0 근처면 그 지점, 아니면 끝나지 않는다
        // (Attenuation.cpp:72–101). 키가 없으면 마지막 Time이 0이다.
        float lastTime = 0.0f;
        float lastValue = 0.0f;
        if (const TangentCurve* curve = attenuation.customDistanceCurve.Get()) {
            for (const TangentCurveKey& key : curve->Keys()) {
                if (key.time > lastTime) {
                    lastTime = key.time;
                    lastValue = key.value;
                }
            }
        }

        if (math::Max(lastValue, 0.0f) <= MinAttenuationValue) {
            falloff *= lastTime;
        } else {
            return maxWorldDistance;
        }

        break;
    }
    case audio::DistanceAttenuationModel::NaturalSound:
        switch (distance.naturalSoundFalloffMode) {
        case audio::NaturalSoundFalloffMode::Hold:
            return maxWorldDistance;
        case audio::NaturalSoundFalloffMode::Continues:
            falloff = falloff * AudibleAttenuationFloorDb /
                      math::Min(distance.naturalSoundAttenuationDb, MaxNaturalSoundAttenuationDb);
            break;
        case audio::NaturalSoundFalloffMode::Silent:
            break;
        }

        break;
    case audio::DistanceAttenuationModel::Linear:
    case audio::DistanceAttenuationModel::Logarithmic:
    case audio::DistanceAttenuationModel::Inverse:
    case audio::DistanceAttenuationModel::LogReverse:
        break;
    }

    return falloff / distance.distanceScale;
}

float ShapeRadius(const SoundAttenuation& attenuation) noexcept {
    const Vec3 extents = attenuation.shapeExtents;
    switch (attenuation.shape) {
    case SoundAttenuationShape::Sphere:
    case SoundAttenuationShape::Cone:
        return extents.x;
    case SoundAttenuationShape::Box:
        return math::Max(extents.x, math::Max(extents.y, extents.z));
    case SoundAttenuationShape::Capsule:
        return math::Max(extents.x, extents.y);
    }

    return extents.x;
}

float AbsorptionFrequency(const SoundAttenuation& attenuation, float attenuationDistance,
                          bool highPass) noexcept {
    const float atMin =
        highPass ? attenuation.highPassFrequencyAtMin : attenuation.lowPassFrequencyAtMin;
    const float atMax =
        highPass ? attenuation.highPassFrequencyAtMax : attenuation.lowPassFrequencyAtMax;
    const float distanceMin = attenuation.absorptionDistanceMin;
    const float distanceMax = attenuation.absorptionDistanceMax;
    float frequency = 0.0f;
    if (atMin == atMax) {
        frequency = atMin;
    } else if (distanceMin == distanceMax) {
        frequency = attenuationDistance > distanceMin ? atMax : atMin;
    } else if (attenuation.absorptionMethod == SoundAbsorptionMethod::Linear) {
        frequency =
            attenuation.logFrequencyScaling
                ? LogFrequencyClamped(attenuationDistance, distanceMin, distanceMax, atMin, atMax)
                : MapClamped(distanceMin, distanceMax, atMin, atMax, attenuationDistance);
    } else {
        // 곡선 방법은 주파수를 순서 없는 범위로 본다(ActiveSound.cpp:1882–1903).
        const float low = math::Min(atMin, atMax);
        const float high = math::Max(atMin, atMax);
        const float alpha = math::Clamp(
            (attenuationDistance - distanceMin) / (distanceMax - distanceMin), 0.0f, 1.0f);
        const TangentCurve* curve = highPass ? attenuation.customHighPassAbsorptionCurve.Get()
                                             : attenuation.customLowPassAbsorptionCurve.Get();
        const float mapped =
            math::Clamp(curve != nullptr ? curve->Evaluate(alpha) : 0.0f, 0.0f, 1.0f);
        frequency = attenuation.logFrequencyScaling
                        ? LogFrequencyClamped(mapped, 0.0f, 1.0f, low, high)
                        : MapClamped(0.0f, 1.0f, low, high, mapped);
    }

    return math::Clamp(frequency, MinAttenuationFilterFrequency, MaxAttenuationFilterFrequency);
}

float PriorityAttenuationScale(const SoundAttenuation& attenuation,
                               float listenerDistance) noexcept {
    if (!attenuation.enablePriorityAttenuation) {
        return 1.0f;
    }

    float scale = 1.0f;
    if (attenuation.priorityAttenuationMethod == SoundPriorityAttenuationMethod::Manual) {
        scale = attenuation.manualPriorityAttenuation;
    } else {
        const float denominator = math::Max(attenuation.priorityAttenuationDistanceMax -
                                                attenuation.priorityAttenuationDistanceMin,
                                            math::SmallNumber<float>);
        const float alpha = math::Clamp(
            (listenerDistance - attenuation.priorityAttenuationDistanceMin) / denominator, 0.0f,
            1.0f);
        if (attenuation.priorityAttenuationMethod == SoundPriorityAttenuationMethod::Linear) {
            scale = math::Max(math::Lerp(attenuation.priorityAttenuationMin,
                                         attenuation.priorityAttenuationMax, alpha),
                              0.0f);
        } else {
            const TangentCurve* curve = attenuation.customPriorityAttenuationCurve.Get();
            scale = math::Max(curve != nullptr ? curve->Evaluate(alpha) : 0.0f, 0.0f);
        }
    }

    return math::Max(scale, 0.0f);
}

float FocusAzimuthDegrees(const Vec3d& listenerPosition, const Quatd& listenerRotation,
                          const Vec3d& sourcePosition) noexcept {
    const Vec3d toSound = sourcePosition - listenerPosition;
    const double length = toSound.Length();
    if (!(length > 0.0)) {
        return UndefinedDirectionAzimuth;
    }

    const Vec3d forward = listenerRotation.RotateVector(Vec3d(1.0, 0.0, 0.0));
    const double cosine = math::Clamp(Dot(forward, toSound / length), -1.0, 1.0);
    return static_cast<float>(math::RadiansToDegrees(PlatformMath::Acos(cosine)));
}

float FocusFactor(const SoundAttenuation& attenuation, const GlobalFocusSettings& global,
                  float azimuthDegrees) noexcept {
    const float focus =
        math::Clamp(global.focusAzimuthScale * attenuation.focusAzimuth, 0.0f, FullAngleDegrees);
    const float nonFocus = math::Clamp(global.nonFocusAzimuthScale * attenuation.nonFocusAzimuth,
                                       0.0f, FullAngleDegrees);
    if (focus != nonFocus) {
        return math::Clamp((azimuthDegrees - focus) / (nonFocus - focus), 0.0f, 1.0f);
    }

    return azimuthDegrees >= focus ? 1.0f : 0.0f;
}

FocusScales ComputeFocusScales(const SoundAttenuation& attenuation,
                               const GlobalFocusSettings& global, float factor) noexcept {
    FocusScales scales;
    scales.distance = math::Max(
        0.0f, math::Lerp(global.focusDistanceScale * attenuation.focusDistanceScale,
                         global.nonFocusDistanceScale * attenuation.nonFocusDistanceScale, factor));
    scales.priority = math::Max(
        0.0f, math::Lerp(global.focusPriorityScale * attenuation.focusPriorityScale,
                         global.nonFocusPriorityScale * attenuation.nonFocusPriorityScale, factor));
    scales.volume = math::Max(
        0.0f,
        math::Lerp(global.focusVolumeScale * attenuation.focusVolumeAttenuation,
                   global.nonFocusVolumeScale * attenuation.nonFocusVolumeAttenuation, factor));
    return scales;
}

float InterpolateFocusFactor(float current, float target, float deltaSeconds, float attackSpeed,
                             float releaseSpeed) noexcept {
    const float speed = target <= current ? attackSpeed : releaseSpeed;
    return math::FInterpTo(current, target, deltaSeconds, speed);
}

} // namespace rocks
