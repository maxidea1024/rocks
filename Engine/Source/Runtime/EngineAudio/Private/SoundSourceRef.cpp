#include "ValueRange.h"

#include <Core/Math/MathUtility.h>
#include <EngineAudio/SoundSourceRef.h>

namespace rocks {

namespace {

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
    const SoundPcm* pcm = playable.pcm.Get();
    if (pcm == nullptr || !pcm->IsFinite() ||
        !engine_audio_detail::IsFiniteInRange(playable.volume, 0.0f, audio::MaxVoiceGain) ||
        !engine_audio_detail::IsFiniteInRange(playable.pitch, audio::MinPitch, audio::MaxPitch)) {
        return Created::Failure(ErrorCode::InvalidArgument);
    }

    // 믹서 AudioClipDesc와 같은 규칙: 둘 다 0이면 전체, 아니면 시작 < 끝 ≤ frame 수.
    const bool wholeLoop = playable.loopBegin == 0 && playable.loopEnd == 0;
    if (!wholeLoop &&
        (playable.loopEnd <= playable.loopBegin || playable.loopEnd > pcm->FrameCount())) {
        return Created::Failure(ErrorCode::InvalidArgument);
    }

    if (playable.attenuation.HasValue()) {
        if (auto valid = ValidateSoundAttenuation(playable.attenuation.Value()); !valid) {
            return Created::Failure(valid.Error());
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
