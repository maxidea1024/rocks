#include <Core/Math/MathUtility.h>
#include <AudioAssets/SoundSourceRef.h>

namespace rocks {

namespace {

bool IsFiniteWithin(float value, float low, float high) noexcept {
    return math::IsFinite(value) && value >= low && value <= high;
}

bool NonNegative(float value) noexcept {
    return math::IsFinite(value) && value >= 0.0f;
}

} // namespace

Result<void> ValidateSoundAttenuation(const SoundAttenuation& value) noexcept {
    const audio::DistanceAttenuationSettings& distance = value.distance;
    if (!NonNegative(value.innerRadius) || !NonNegative(value.nonSpatializedRadiusStart) ||
        !NonNegative(value.nonSpatializedRadiusEnd) || !NonNegative(distance.falloffDistance) ||
        !NonNegative(distance.distanceScale) ||
        !math::IsFinite(distance.naturalSoundAttenuationDb) ||
        distance.naturalSoundAttenuationDb > 0.0f ||
        distance.model == audio::DistanceAttenuationModel::Custom) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
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
