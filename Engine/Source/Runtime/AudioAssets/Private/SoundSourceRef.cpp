#include "AudioAssetsValidation.h"

#include <Core/Math/MathUtility.h>
#include <AudioAssets/SoundSourceRef.h>

namespace rocks {

namespace {

using audio_assets_detail::IsFiniteWithin;

bool NonNegative(float value) noexcept {
    return audio_assets_detail::IsNonNegativeFinite(value);
}

} // namespace

Result<void> ValidateSoundAttenuation(const SoundAttenuation& value) noexcept {
    const auto fail = []() noexcept { return Result<void>::Failure(ErrorCode::InvalidArgument); };
    const audio::DistanceAttenuationSettings& distance = value.distance;
    if (!NonNegative(distance.falloffDistance) || !NonNegative(distance.distanceScale) ||
        !math::IsFinite(distance.naturalSoundAttenuationDb) ||
        distance.naturalSoundAttenuationDb > 0.0f ||
        static_cast<uint8>(distance.model) >
            static_cast<uint8>(audio::DistanceAttenuationModel::Custom) ||
        static_cast<uint8>(distance.naturalSoundFalloffMode) >
            static_cast<uint8>(audio::NaturalSoundFalloffMode::Continues)) {
        return fail();
    }

    // 모양(3.42.2절): 크기 성분·거리 ≥ 0, Cone의 Y·Z는 각도.
    if (static_cast<uint8>(value.shape) > static_cast<uint8>(SoundAttenuationShape::Cone) ||
        !NonNegative(value.shapeExtents.x) || !NonNegative(value.shapeExtents.y) ||
        !NonNegative(value.shapeExtents.z) || !NonNegative(value.coneOffset) ||
        !NonNegative(value.coneSphereRadius) || !NonNegative(value.coneSphereFalloffDistance)) {
        return fail();
    }

    if (value.shape == SoundAttenuationShape::Cone &&
        (value.shapeExtents.y > MaxAttenuationAngleDegrees ||
         value.shapeExtents.z > MaxAttenuationAngleDegrees)) {
        return fail();
    }

    // 공기 흡수(3.42.3절): 구간 Min ≤ Max, 주파수 [0, 20000].
    if (static_cast<uint8>(value.absorptionMethod) >
            static_cast<uint8>(SoundAbsorptionMethod::CustomCurve) ||
        !NonNegative(value.absorptionDistanceMin) || !NonNegative(value.absorptionDistanceMax) ||
        value.absorptionDistanceMin > value.absorptionDistanceMax ||
        !IsFiniteWithin(value.lowPassFrequencyAtMin, 0.0f, MaxAttenuationFilterFrequency) ||
        !IsFiniteWithin(value.lowPassFrequencyAtMax, 0.0f, MaxAttenuationFilterFrequency) ||
        !IsFiniteWithin(value.highPassFrequencyAtMin, 0.0f, MaxAttenuationFilterFrequency) ||
        !IsFiniteWithin(value.highPassFrequencyAtMax, 0.0f, MaxAttenuationFilterFrequency)) {
        return fail();
    }

    // 우선순위(3.42.4절)·focus(3.42.5절): 배수 ≥ 0, 방위각 [0, 180], 속도 ≥ 0.
    if (static_cast<uint8>(value.priorityAttenuationMethod) >
            static_cast<uint8>(SoundPriorityAttenuationMethod::Manual) ||
        !NonNegative(value.priorityAttenuationMin) || !NonNegative(value.priorityAttenuationMax) ||
        !NonNegative(value.priorityAttenuationDistanceMin) ||
        !NonNegative(value.priorityAttenuationDistanceMax) ||
        !NonNegative(value.manualPriorityAttenuation) ||
        !IsFiniteWithin(value.focusAzimuth, 0.0f, MaxAttenuationAngleDegrees) ||
        !IsFiniteWithin(value.nonFocusAzimuth, 0.0f, MaxAttenuationAngleDegrees) ||
        !NonNegative(value.focusDistanceScale) || !NonNegative(value.nonFocusDistanceScale) ||
        !NonNegative(value.focusPriorityScale) || !NonNegative(value.nonFocusPriorityScale) ||
        !NonNegative(value.focusVolumeAttenuation) ||
        !NonNegative(value.nonFocusVolumeAttenuation) ||
        !NonNegative(value.focusAttackInterpSpeed) || !NonNegative(value.focusReleaseInterpSpeed)) {
        return fail();
    }

    // 스테레오·비공간화(3.42.6절)·차폐(3.42.7절).
    if (!NonNegative(value.stereoSpread) || !NonNegative(value.nonSpatializedRadiusStart) ||
        !NonNegative(value.nonSpatializedRadiusEnd) ||
        static_cast<uint8>(value.nonSpatializedMode) >
            static_cast<uint8>(SoundNonSpatializedMode::Surround2D) ||
        !IsFiniteWithin(value.occlusionLowPassFrequency, 0.0f, MaxAttenuationFilterFrequency) ||
        !IsFiniteWithin(value.occlusionVolumeAttenuation, 0.0f, 1.0f) ||
        !NonNegative(value.occlusionInterpolationSeconds)) {
        return fail();
    }

    // 쓰이는 곡선은 있어야 한다(3.42.1절).
    const bool absorptionCurves =
        value.attenuateWithLowPass && value.absorptionMethod == SoundAbsorptionMethod::CustomCurve;
    if ((distance.model == audio::DistanceAttenuationModel::Custom && !value.customDistanceCurve) ||
        (absorptionCurves &&
         (!value.customLowPassAbsorptionCurve || !value.customHighPassAbsorptionCurve)) ||
        (value.enablePriorityAttenuation &&
         value.priorityAttenuationMethod == SoundPriorityAttenuationMethod::CustomCurve &&
         !value.customPriorityAttenuationCurve)) {
        return fail();
    }

    return Result<void>::Success();
}

Result<SoundSourceRef> SoundSourceRef::TryCreate(Allocator& allocator,
                                                 const SoundPlayable& playable) noexcept {
    using Created = Result<SoundSourceRef>;
    // pcm·stream 중 정확히 하나다. 유한성은 pcm만 본다(스트림은 생산자가 쓰기 전에 본다
    // — 3.40.1절).
    const SoundPcm* pcm = playable.pcm.Get();
    const bool hasPcm = pcm != nullptr;
    const bool hasStream = playable.stream.Get() != nullptr;
    if (hasPcm == hasStream || (hasPcm && !pcm->IsFinite()) ||
        !IsFiniteWithin(playable.volume, 0.0f, audio::MaxVoiceGain) ||
        !IsFiniteWithin(playable.pitch, audio::MinPitch, audio::MaxPitch) ||
        !IsFiniteWithin(playable.priority, MinSoundPriority, MaxSoundPriority) ||
        static_cast<uint8>(playable.virtualizationMode) >
            static_cast<uint8>(SoundVirtualizationMode::SeekRestart)) {
        return Created::Failure(ErrorCode::InvalidArgument);
    }

    // 믹서 AudioClipDesc와 같은 규칙: 둘 다 0이면 전체, 아니면 시작 < 끝 ≤ frame 수.
    const bool wholeLoop = playable.loopBegin == 0 && playable.loopEnd == 0;
    if (!wholeLoop &&
        (playable.loopEnd <= playable.loopBegin || playable.loopEnd > playable.FrameCount())) {
        return Created::Failure(ErrorCode::InvalidArgument);
    }

    if (playable.attenuation.HasValue()) {
        if (auto valid = ValidateSoundAttenuation(playable.attenuation.Value()); !valid) {
            return Created::Failure(valid.Error());
        }
    }

    if (playable.category) {
        if (auto valid = ValidateSoundCategoryChain(*playable.category.Get()); !valid) {
            return Created::Failure(valid.Error());
        }
    }

    // 동시 발음 항목: 수 ≤ MaxConcurrencyGroups, 키가 있고, 설정이 범위 안이다(3.41.2절).
    if (playable.concurrencyCount > MaxConcurrencyGroups) {
        return Created::Failure(ErrorCode::CapacityExceeded);
    }

    for (uint32 index = 0; index < playable.concurrencyCount; ++index) {
        const SoundConcurrencyEntry& entry = playable.concurrency[index];
        if (entry.key == 0) {
            return Created::Failure(ErrorCode::InvalidArgument);
        }

        if (auto valid = ValidateSoundConcurrencySettings(entry.settings); !valid) {
            return Created::Failure(valid.Error());
        }

        // 같은 그룹은 한 번만(UE ConcurrencySet은 집합 — 같은 키가 둘이면 자리를 둘 차지한다).
        for (uint32 earlier = 0; earlier < index; ++earlier) {
            if (playable.concurrency[earlier].key == entry.key &&
                playable.concurrency[earlier].isOverride == entry.isOverride) {
                return Created::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    auto made = SharedPtr<SoundPlayable>::TryMake(allocator, playable);
    if (!made) {
        return Created::Failure(made.Error());
    }

    return Created::Success(SoundSourceRef(SharedPtr<const SoundPlayable>(Move(made.Value()))));
}

uint64 SoundSourceRef::UseCount() const noexcept {
    return playable_.UseCount();
}

} // namespace rocks
