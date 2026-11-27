#include <AudioAssets/AttenuationSettings.generated.h>
#include <AudioAssets/SoundClip.generated.h>
#include <AudioAssets/SoundSource.generated.h>
#include <AssetRuntime/PackageWriter.h>
#include <Base/Compiler.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Memory.h>

// 사운드 에셋 클래스의 스냅숏 만들기와 재생 PCM(에디터 원본·쿠킹 플랫폼 데이터)이다
// (Docs/Design/AudioSystem 3.35.3·3.36.2·3.36.4·3.37.4절).
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
    stream_ = SharedPtr<const SoundStreamData>{};
    ++pcmRevision_;
    pcmKey_ = SoundPcmKey{};
    return Result<void>::Success();
}

Result<void> SoundClip::FillPlayable(SoundPlayable& playable) const noexcept {
    // 재임포트 직후(LoadEditorPcm 전)에는 형식 필드가 새 값이고 PCM은 옛 것이다. 새 loop 구간이 옛
    // PCM과 섞이지 않게 차원이 다르면 준비되지 않은 것으로 본다(3.36.2절).
    if (stream_) {
        // 실시간 디코드 원본도 같은 차원 검사를 한다(3.40.3절).
        if (stream_->ChannelCount() != channelCount || stream_->SampleRate() != sampleRate ||
            stream_->FrameCount() != frameCount) {
            return Result<void>::Failure(ErrorCode::NotReady);
        }

        playable.stream = stream_;
    } else {
        if (!pcm_ || pcm_->ChannelCount() != channelCount || pcm_->SampleRate() != sampleRate ||
            pcm_->FrameCount() != frameCount) {
            return Result<void>::Failure(ErrorCode::NotReady);
        }

        playable.pcm = pcm_;
    }

    playable.looping = looping;
    EffectiveLoop(playable.loopBegin, playable.loopEnd);
    playable.revision = pcmRevision_;
    return Result<void>::Success();
}

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

// SoundPcm 할당 오류를 손상으로 옮긴다(형식 필드가 SoundPcm 범위 밖이면 데이터가 손상된 것이다).
ErrorCode PcmAllocationError(ErrorCode error) noexcept {
    return error == ErrorCode::InvalidArgument ? ErrorCode::CorruptData : error;
}

bool SameKey(const SoundPcmKey& key, SoundPcmOrigin origin, const BulkDataRef& bulk,
             SoundSourceStorage format, uint32 channels, uint32 sampleRate,
             uint32 frames) noexcept {
    return key.origin == origin && key.bulk.SameContent(bulk) && key.format == format &&
           key.channels == channels && key.sampleRate == sampleRate && key.frames == frames;
}

} // namespace

Result<SharedPtr<SoundPcm>> DecodeSoundClipSource(Allocator& allocator, SoundSourceStorage format,
                                                  uint32 channels, uint32 sampleRate, uint32 frames,
                                                  Span<const uint8> bytes) noexcept {
    using Made = Result<SharedPtr<SoundPcm>>;
    if (channels > audio::MaxClipChannels) {
        return Made::Failure(ErrorCode::Unsupported);
    }

    const uint32 bytesPerSample = SoundSourceStorageBytes(format);
    const uint64 expected = static_cast<uint64>(bytesPerSample) * channels * frames;
    if (bytesPerSample == 0 || channels == 0 || frames == 0 || bytes.Size() != expected) {
        return Made::Failure(ErrorCode::CorruptData);
    }

    auto made = SoundPcm::TryAllocate(allocator, channels, frames, sampleRate);
    if (!made) {
        return Made::Failure(PcmAllocationError(made.Error()));
    }

    for (uint32 channel = 0; channel < channels; ++channel) {
        Span<float> samples = made.Value()->MutableChannel(channel);
        const uint8* base = bytes.Data() + static_cast<usize>(channel) * frames * bytesPerSample;
        for (uint32 frame = 0; frame < frames; ++frame) {
            samples[frame] =
                ReadStoredSample(base + static_cast<usize>(frame) * bytesPerSample, format);
        }
    }

    if (!made.Value()->IsFinite()) {
        return Made::Failure(ErrorCode::CorruptData);
    }

    return made;
}

#if ROCKS_WITH_EDITOR_DATA

Result<void> SoundClip::LoadEditorPcm(Allocator& allocator, PackageBulkSource& bulk) noexcept {
    if (source.format == SoundSourceStorage::None || source.bulk.IsEmpty() || frameCount == 0) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    if (SameKey(pcmKey_, SoundPcmOrigin::Editor, source.bulk, source.format, channelCount,
                sampleRate, frameCount)) {
        return Result<void>::Success();
    }

    if (channelCount > audio::MaxClipChannels) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    auto read = bulk.ReadBulk(source.bulk);
    if (!read) {
        return Result<void>::Failure(read.Error());
    }

    auto made = DecodeSoundClipSource(allocator, source.format, channelCount, sampleRate,
                                      frameCount, read.Value());
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    pcm_ = SharedPtr<const SoundPcm>(Move(made.Value()));
    stream_ = SharedPtr<const SoundStreamData>{};
    ++pcmRevision_;
    pcmKey_ = SoundPcmKey{SoundPcmOrigin::Editor, source.bulk, source.format,
                          channelCount,           sampleRate,  frameCount};
    return Result<void>::Success();
}

#endif

Result<void> SoundClip::LoadCookedPcm(Allocator& allocator, SoundBulkReader reader, void* context,
                                      const audio::AudioDecoderTable& decoders) noexcept {
    if (platformData.IsEmpty() || reader == nullptr) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    if (SameKey(pcmKey_, SoundPcmOrigin::Cooked, platformData, SoundSourceStorage::None,
                channelCount, sampleRate, frameCount)) {
        return Result<void>::Success();
    }

    if (channelCount > audio::MaxClipChannels) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    auto read = reader(context, platformData);
    if (!read) {
        return Result<void>::Failure(read.Error());
    }

    // 형식 위반은 Parse가 CorruptData로, 모르는 판·codec은 Unsupported로 알린다(3.38절 — 그대로
    // 넘긴다).
    auto view = audio::AudioPlatformDataView::Parse(read.Value());
    if (!view) {
        return Result<void>::Failure(view.Error());
    }

    // reader가 준 byte는 플랫폼 데이터 전체여야 한다(payload가 모자라면 잘린 데이터다).
    if (!view.Value().HasAllPayload()) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    // 머리와 필드가 다르면(필드가 다른 쿠킹의 것) 재생 PCM 차원 검사가 원인 없이 NotReady를 내므로
    // 여기서 손상으로 알린다.
    const audio::AudioPlatformDataHeader& header = view.Value().Header();
    if (header.channels != channelCount || header.sampleRate != sampleRate ||
        header.frames != frameCount) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    audio::DecodedAudioPcm decoded(allocator);
    if (auto done = audio::DecodeAudioPlatformData(view.Value(), decoders, allocator, decoded);
        !done) {
        return done;
    }

    // 디코드 결과가 머리 차원과 같아야 복사할 수 있다(디코더 결함이 범위 밖 읽기가 되지 않게).
    if (decoded.ChannelCount() != channelCount || decoded.SampleRate() != sampleRate ||
        decoded.FrameCount() != frameCount) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    auto made = SoundPcm::TryAllocate(allocator, channelCount, frameCount, sampleRate);
    if (!made) {
        return Result<void>::Failure(PcmAllocationError(made.Error()));
    }

    for (uint32 channel = 0; channel < channelCount; ++channel) {
        Span<float> samples = made.Value()->MutableChannel(channel);
        platform::MemoryCopy(samples.Data(), decoded.Channel(channel),
                             static_cast<usize>(frameCount) * sizeof(float));
    }

    if (!made.Value()->IsFinite()) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    pcm_ = SharedPtr<const SoundPcm>(Move(made.Value()));
    stream_ = SharedPtr<const SoundStreamData>{};
    ++pcmRevision_;
    pcmKey_ = SoundPcmKey{SoundPcmOrigin::Cooked, platformData, SoundSourceStorage::None,
                          channelCount,           sampleRate,   frameCount};
    return Result<void>::Success();
}

Result<void> SoundClip::LoadCookedStream(Allocator& allocator, SoundBulkReader reader,
                                         void* context,
                                         const audio::AudioDecoderTable& decoders) noexcept {
    if (platformData.IsEmpty() || reader == nullptr) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    if (SameKey(pcmKey_, SoundPcmOrigin::CookedStream, platformData, SoundSourceStorage::None,
                channelCount, sampleRate, frameCount)) {
        return Result<void>::Success();
    }

    if (channelCount > audio::MaxClipChannels) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    auto read = reader(context, platformData);
    if (!read) {
        return Result<void>::Failure(read.Error());
    }

    // 복사·해석·모든 청크 검증(3.40.2절 — 공급자의 "검증된 payload" 계약을 여기서 만든다).
    auto made = SoundStreamData::TryCreate(allocator, read.Value());
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    const SoundStreamData& data = *made.Value();
    if (data.ChannelCount() != channelCount || data.SampleRate() != sampleRate ||
        data.FrameCount() != frameCount) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    if (!decoders.Contains(data.View().Header().codec)) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    stream_ = SharedPtr<const SoundStreamData>(Move(made.Value()));
    pcm_ = SharedPtr<const SoundPcm>{};
    ++pcmRevision_;
    pcmKey_ = SoundPcmKey{SoundPcmOrigin::CookedStream,
                          platformData,
                          SoundSourceStorage::None,
                          channelCount,
                          sampleRate,
                          frameCount};
    return Result<void>::Success();
}

SoundLoadingBehavior
SoundClip::EffectiveLoadingBehavior(const SoundCookedLoadOptions& options) const noexcept {
    if (loadingBehavior != SoundLoadingBehavior::Inherited) {
        return loadingBehavior;
    }

    if (options.defaultLoading < SoundLoadingBehavior::RetainOnLoad) {
        return SoundLoadingBehavior::RetainOnLoad;
    }

    return options.defaultLoading > SoundLoadingBehavior::LoadOnDemand
               ? SoundLoadingBehavior::LoadOnDemand
               : options.defaultLoading;
}

Result<void> SoundClip::LoadCooked(Allocator& allocator, SoundBulkReader reader, void* context,
                                   const audio::AudioDecoderTable& decoders,
                                   const SoundCookedLoadOptions& options) noexcept {
    // AU6a는 동작마다 디코드 방식이 같다(AU6b의 청크 캐시가 동작을 쓴다).
    (void)options;
    // 짧은 loop는 바퀴마다 디코더를 다시 여는 비용이 커서 통째로 푼다(3.40.2절).
    uint32 loopBegin = 0;
    uint32 loopEnd = 0;
    EffectiveLoop(loopBegin, loopEnd);
    const uint64 loopLength = loopBegin == 0 && loopEnd == 0 ? frameCount : loopEnd - loopBegin;
    const bool shortLoop =
        looping && loopLength < static_cast<uint64>(sampleRate) * StreamMinLoopSeconds;
    if (shortLoop) {
        return LoadCookedPcm(allocator, reader, context, decoders);
    }

    return LoadCookedStream(allocator, reader, context, decoders);
}

} // namespace rocks
