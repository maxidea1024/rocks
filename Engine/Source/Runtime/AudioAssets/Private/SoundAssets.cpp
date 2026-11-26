#include <AudioAssets/AttenuationSettings.generated.h>
#include <AudioAssets/SoundClip.generated.h>
#include <AudioAssets/SoundSource.generated.h>
#include <Object/ObjectRegistry.h>

// 사운드 에셋 클래스의 스냅숏 만들기다(Docs/Design/AudioSystem 3.35.3절).
namespace rocks {

static_assert(static_cast<uint8>(SoundDistanceModel::Linear) ==
              static_cast<uint8>(audio::DistanceAttenuationModel::Linear));
static_assert(static_cast<uint8>(SoundDistanceModel::Logarithmic) ==
              static_cast<uint8>(audio::DistanceAttenuationModel::Logarithmic));
static_assert(static_cast<uint8>(SoundDistanceModel::Inverse) ==
              static_cast<uint8>(audio::DistanceAttenuationModel::Inverse));
static_assert(static_cast<uint8>(SoundDistanceModel::LogReverse) ==
              static_cast<uint8>(audio::DistanceAttenuationModel::LogReverse));
static_assert(static_cast<uint8>(SoundDistanceModel::NaturalSound) ==
              static_cast<uint8>(audio::DistanceAttenuationModel::NaturalSound));
static_assert(static_cast<uint8>(SoundDistanceModel::Custom) ==
              static_cast<uint8>(audio::DistanceAttenuationModel::Custom));
static_assert(static_cast<uint8>(SoundNaturalFalloffMode::Hold) ==
              static_cast<uint8>(audio::NaturalSoundFalloffMode::Hold));
static_assert(static_cast<uint8>(SoundNaturalFalloffMode::Silent) ==
              static_cast<uint8>(audio::NaturalSoundFalloffMode::Silent));
static_assert(static_cast<uint8>(SoundNaturalFalloffMode::Continues) ==
              static_cast<uint8>(audio::NaturalSoundFalloffMode::Continues));

Result<SoundAttenuation> AttenuationSettings::ToSnapshot() const noexcept {
    SoundAttenuation snapshot;
    snapshot.attenuate = attenuate;
    snapshot.spatialize = spatialize;
    snapshot.distance.model = static_cast<audio::DistanceAttenuationModel>(model);
    snapshot.distance.falloffDistance = falloffDistance;
    snapshot.distance.naturalSoundAttenuationDb = naturalSoundAttenuationDb;
    snapshot.distance.naturalSoundFalloffMode =
        static_cast<audio::NaturalSoundFalloffMode>(naturalSoundFalloffMode);
    snapshot.innerRadius = innerRadius;
    snapshot.nonSpatializedRadiusStart = nonSpatializedRadiusStart;
    snapshot.nonSpatializedRadiusEnd = nonSpatializedRadiusEnd;
    if (auto valid = ValidateSoundAttenuation(snapshot); !valid) {
        return Result<SoundAttenuation>::Failure(valid.Error());
    }

    return Result<SoundAttenuation>::Success(snapshot);
}

Result<SoundSourceRef> SoundSource::MakePlayable(Allocator& allocator, ObjectRegistry& registry,
                                                 const ObjectAccessContext& access) const noexcept {
    using Made = Result<SoundSourceRef>;
    SoundPlayable playable;
    if (auto filled = FillPlayable(playable); !filled) {
        return Made::Failure(filled.Error());
    }

    playable.volume = volume;
    playable.pitch = pitch;
    playable.asset = Id();
    if (!attenuation.Handle().IsNull()) {
        auto settings = registry.Resolve<AttenuationSettings>(access, attenuation.Handle());
        if (!settings) {
            return Made::Failure(ErrorCode::InvalidArgument);
        }

        auto resolved = settings.Value().Get();
        if (!resolved) {
            return Made::Failure(ErrorCode::InvalidArgument);
        }

        auto snapshot = resolved.Value()->ToSnapshot();
        if (!snapshot) {
            return Made::Failure(snapshot.Error());
        }

        playable.attenuation = snapshot.Value();
    }

    return SoundSourceRef::TryCreate(allocator, playable);
}

Result<void> SoundClip::SetPcm(SharedPtr<const SoundPcm> pcm) noexcept {
    if (pcm && !pcm->IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    pcm_ = Move(pcm);
    ++pcmRevision_;
    return Result<void>::Success();
}

Result<void> SoundClip::FillPlayable(SoundPlayable& playable) const noexcept {
    if (!pcm_) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    playable.pcm = pcm_;
    playable.looping = looping;
    playable.loopBegin = loopBegin;
    playable.loopEnd = loopEnd;
    playable.revision = pcmRevision_;
    return Result<void>::Success();
}

} // namespace rocks
