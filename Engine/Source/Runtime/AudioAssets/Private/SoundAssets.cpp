#include <AudioAssets/AttenuationSettings.generated.h>
#include <AudioAssets/SoundClip.generated.h>
#include <AudioAssets/SoundSource.generated.h>
#include <AssetRuntime/PackageWriter.h>
#include <Base/Compiler.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Memory.h>

// 사운드 에셋 클래스의 스냅숏 만들기와 에디터 원본 재생 PCM이다(Docs/Design/AudioSystem 3.35.3·
// 3.36.2·3.36.4절).
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

uint32 SoundSourceStorageBytes(SoundSourceStorage storage) noexcept {
    switch (storage) {
    case SoundSourceStorage::None:
        return 0;
    case SoundSourceStorage::Int16:
        return SoundSourceInt16Bytes;
    case SoundSourceStorage::Int24:
        return SoundSourceInt24Bytes;
    case SoundSourceStorage::Float32:
        return SoundSourceFloat32Bytes;
    }

    return 0;
}

void SoundClip::EffectiveLoop(uint32& begin, uint32& end) const noexcept {
    if (loopOverride.enabled) {
        begin = loopOverride.begin;
        end = loopOverride.end;
    } else if (hasFileLoop) {
        begin = fileLoopBegin;
        end = fileLoopEnd;
    } else {
        begin = 0;
        end = 0;
    }
}

Result<void> SoundClip::SetPcm(SharedPtr<const SoundPcm> pcm) noexcept {
    if (pcm && !pcm->IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    channelCount = pcm ? pcm->ChannelCount() : 0;
    sampleRate = pcm ? pcm->SampleRate() : 0;
    frameCount = pcm ? pcm->FrameCount() : 0;
    pcm_ = Move(pcm);
    ++pcmRevision_;
    editorPcmKey_ = SoundEditorPcmKey{};
    return Result<void>::Success();
}

Result<void> SoundClip::FillPlayable(SoundPlayable& playable) const noexcept {
    // 재임포트 직후(LoadEditorPcm 전)에는 형식 필드가 새 값이고 PCM은 옛 것이다. 새 loop 구간이 옛
    // PCM과 섞이지 않게 차원이 다르면 준비되지 않은 것으로 본다(3.36.2절).
    if (!pcm_ || pcm_->ChannelCount() != channelCount || pcm_->SampleRate() != sampleRate ||
        pcm_->FrameCount() != frameCount) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    playable.pcm = pcm_;
    playable.looping = looping;
    EffectiveLoop(playable.loopBegin, playable.loopEnd);
    playable.revision = pcmRevision_;
    return Result<void>::Success();
}

#if ROCKS_WITH_EDITOR_DATA

namespace {

float ReadStoredSample(const uint8* bytes, SoundSourceStorage format) noexcept {
    switch (format) {
    case SoundSourceStorage::Int16: {
        const int16 value =
            static_cast<int16>(static_cast<uint16>(bytes[0]) | static_cast<uint16>(bytes[1]) << 8);
        return static_cast<float>(value) / SoundSourceInt16FullScale;
    }
    case SoundSourceStorage::Int24: {
        uint32 raw = static_cast<uint32>(bytes[0]) | static_cast<uint32>(bytes[1]) << 8 |
                     static_cast<uint32>(bytes[2]) << 16;
        // 부호 확장(24비트 → 32비트).
        if ((raw & SoundSourceInt24SignBit) != 0) {
            raw |= SoundSourceInt24SignExtension;
        }

        return static_cast<float>(static_cast<int32>(raw)) / SoundSourceInt24FullScale;
    }
    case SoundSourceStorage::Float32: {
        const uint32 raw = static_cast<uint32>(bytes[0]) | static_cast<uint32>(bytes[1]) << 8 |
                           static_cast<uint32>(bytes[2]) << 16 |
                           static_cast<uint32>(bytes[3]) << 24;
        float value = 0.0f;
        platform::MemoryCopy(&value, &raw, sizeof(value));
        return value;
    }
    case SoundSourceStorage::None:
        break;
    }

    return 0.0f;
}

} // namespace

Result<void> SoundClip::LoadEditorPcm(Allocator& allocator, PackageBulkSource& bulk) noexcept {
    if (source.format == SoundSourceStorage::None || source.bulk.IsEmpty() || frameCount == 0) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    const SoundEditorPcmKey& key = editorPcmKey_;
    if (key.loaded && key.source.format == source.format &&
        key.source.bulk.SameContent(source.bulk) && key.channels == channelCount &&
        key.sampleRate == sampleRate && key.frames == frameCount) {
        return Result<void>::Success();
    }

    if (channelCount > audio::MaxClipChannels) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    const uint32 bytesPerSample = SoundSourceStorageBytes(source.format);
    const uint64 expected = static_cast<uint64>(bytesPerSample) * channelCount * frameCount;
    auto read = bulk.ReadBulk(source.bulk);
    if (!read) {
        return Result<void>::Failure(read.Error());
    }

    const Span<const uint8> bytes = read.Value();
    if (channelCount == 0 || bytes.Size() != expected) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    auto made = SoundPcm::TryAllocate(allocator, channelCount, frameCount, sampleRate);
    if (!made) {
        // 형식 필드가 SoundPcm 범위 밖이면(rate·채널) 원본이 손상된 것이다.
        return Result<void>::Failure(
            made.Error() == ErrorCode::InvalidArgument ? ErrorCode::CorruptData : made.Error());
    }

    for (uint32 channel = 0; channel < channelCount; ++channel) {
        Span<float> samples = made.Value()->MutableChannel(channel);
        const uint8* base =
            bytes.Data() + static_cast<usize>(channel) * frameCount * bytesPerSample;
        for (uint32 frame = 0; frame < frameCount; ++frame) {
            samples[frame] =
                ReadStoredSample(base + static_cast<usize>(frame) * bytesPerSample, source.format);
        }
    }

    if (!made.Value()->IsFinite()) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    pcm_ = SharedPtr<const SoundPcm>(Move(made.Value()));
    ++pcmRevision_;
    editorPcmKey_.loaded = true;
    editorPcmKey_.source = source;
    editorPcmKey_.channels = channelCount;
    editorPcmKey_.sampleRate = sampleRate;
    editorPcmKey_.frames = frameCount;
    return Result<void>::Success();
}

#endif

} // namespace rocks
