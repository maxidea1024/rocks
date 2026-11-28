#include <AudioAssets/AudioObjectKey.h>

#include <AudioAssets/AttenuationSettings.generated.h>
#include <AudioAssets/ConcurrencyGroup.generated.h>
#include <AudioAssets/SoundClip.generated.h>
#include <AudioAssets/SoundSource.generated.h>
#include <AudioAssets/SoundCategory.generated.h>
#include <AudioAssets/MixBus.generated.h>
#include <AudioAssets/MixBusSend.generated.h>
#include "AudioAssetsValidation.h"
#include <AssetRuntime/PackageWriter.h>
#include <Core/Diagnostics/Log.h>
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

namespace {

inline constexpr LogCategory LogAudioAssets{"AudioAssets"};

// send 배열을 재생 값으로 평탄화한다(3.45.7절). postOnly면 단계를 보지 않고 post로 쓴다(감쇠 send —
// UE FAttenuationSubmixSendSettings에는 단계가 없다).
Result<void> FlattenBusSends(const StructRefArray<MixBusSend>& sends, bool postOnly,
                             Allocator& allocator, ObjectRegistry& registry,
                             const ObjectAccessContext& access, uint32& count,
                             MixBusSendValue* out) noexcept {
    if (sends.Size() > MaxMixBusSends) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    count = 0;
    for (usize index = 0; index < sends.Size(); ++index) {
        auto value = MakeMixBusSendValue(sends[index], allocator, registry, access);
        if (!value) {
            return Result<void>::Failure(value.Error());
        }

        if (postOnly && value.Value().stage != MixBusSendStage::PostAttenuation) {
            Log::Warning(LogAudioAssets,
                         "Attenuation bus send uses the pre-attenuation stage; attenuation sends "
                         "are always post-attenuation");
            value.Value().stage = MixBusSendStage::PostAttenuation;
        }

        out[count++] = Move(value.Value());
    }

    return Result<void>::Success();
}

} // namespace

Result<SoundAttenuation>
AttenuationSettings::ToSnapshot(Allocator& allocator, ObjectRegistry& registry,
                                const ObjectAccessContext& access) const noexcept {
    using Made = Result<SoundAttenuation>;
    SoundAttenuation snapshot;
    snapshot.attenuate = attenuate;
    snapshot.spatialize = spatialize;
    snapshot.distance.model = static_cast<audio::DistanceAttenuationModel>(model);
    snapshot.distance.falloffDistance = falloffDistance;
    snapshot.distance.naturalSoundAttenuationDb = naturalSoundAttenuationDb;
    snapshot.distance.naturalSoundFalloffMode =
        static_cast<audio::NaturalSoundFalloffMode>(naturalSoundFalloffMode);
    snapshot.shape = shape;
    snapshot.shapeExtents = shapeExtents;
    snapshot.coneOffset = coneOffset;
    snapshot.coneSphereRadius = coneSphereRadius;
    snapshot.coneSphereFalloffDistance = coneSphereFalloffDistance;
    snapshot.attenuateWithLowPass = attenuateWithLowPass;
    snapshot.absorptionMethod = absorptionMethod;
    snapshot.logFrequencyScaling = logFrequencyScaling;
    snapshot.absorptionDistanceMin = absorptionDistanceMin;
    snapshot.absorptionDistanceMax = absorptionDistanceMax;
    snapshot.lowPassFrequencyAtMin = lowPassFrequencyAtMin;
    snapshot.lowPassFrequencyAtMax = lowPassFrequencyAtMax;
    snapshot.highPassFrequencyAtMin = highPassFrequencyAtMin;
    snapshot.highPassFrequencyAtMax = highPassFrequencyAtMax;
    snapshot.enablePriorityAttenuation = enablePriorityAttenuation;
    snapshot.priorityAttenuationMethod = priorityAttenuationMethod;
    snapshot.priorityAttenuationMin = priorityAttenuationMin;
    snapshot.priorityAttenuationMax = priorityAttenuationMax;
    snapshot.priorityAttenuationDistanceMin = priorityAttenuationDistanceMin;
    snapshot.priorityAttenuationDistanceMax = priorityAttenuationDistanceMax;
    snapshot.manualPriorityAttenuation = manualPriorityAttenuation;
    snapshot.enableListenerFocus = enableListenerFocus;
    snapshot.enableFocusInterpolation = enableFocusInterpolation;
    snapshot.focusAzimuth = focusAzimuth;
    snapshot.nonFocusAzimuth = nonFocusAzimuth;
    snapshot.focusDistanceScale = focusDistanceScale;
    snapshot.nonFocusDistanceScale = nonFocusDistanceScale;
    snapshot.focusPriorityScale = focusPriorityScale;
    snapshot.nonFocusPriorityScale = nonFocusPriorityScale;
    snapshot.focusVolumeAttenuation = focusVolumeAttenuation;
    snapshot.nonFocusVolumeAttenuation = nonFocusVolumeAttenuation;
    snapshot.focusAttackInterpSpeed = focusAttackInterpSpeed;
    snapshot.focusReleaseInterpSpeed = focusReleaseInterpSpeed;
    snapshot.stereoSpread = stereoSpread;
    snapshot.nonSpatializedRadiusStart = nonSpatializedRadiusStart;
    snapshot.nonSpatializedRadiusEnd = nonSpatializedRadiusEnd;
    snapshot.nonSpatializedMode = nonSpatializedRadiusMode;
    snapshot.enableOcclusion = enableOcclusion;
    snapshot.occlusionLowPassFrequency = occlusionLowPassFrequency;
    snapshot.occlusionVolumeAttenuation = occlusionVolumeAttenuation;
    snapshot.occlusionInterpolationSeconds = occlusionInterpolationSeconds;

    // 쓰이는 곡선만 평탄화한다.
    const bool absorptionCurves =
        attenuateWithLowPass && absorptionMethod == SoundAbsorptionMethod::CustomCurve;
    const struct {
        bool used;
        const FloatCurveSource* source;
        SharedPtr<const TangentCurve>* out;
    } curves[] = {
        {model == SoundDistanceModel::Custom, &customDistanceCurve, &snapshot.customDistanceCurve},
        {absorptionCurves, &customLowPassAbsorptionCurve, &snapshot.customLowPassAbsorptionCurve},
        {absorptionCurves, &customHighPassAbsorptionCurve, &snapshot.customHighPassAbsorptionCurve},
        {enablePriorityAttenuation &&
             priorityAttenuationMethod == SoundPriorityAttenuationMethod::CustomCurve,
         &customPriorityAttenuationCurve, &snapshot.customPriorityAttenuationCurve},
    };

    for (const auto& curve : curves) {
        if (!curve.used) {
            continue;
        }

        if (auto flattened = audio_assets_detail::FlattenCurve(*curve.source, allocator, registry,
                                                               access, *curve.out);
            !flattened) {
            return Made::Failure(flattened.Error());
        }
    }

    // 버스 send(3.45.7절 — 켰을 때만 평탄화한다).
    snapshot.enableBusSends = enableBusSends;
    if (enableBusSends) {
        if (auto flattened = FlattenBusSends(busSends, true, allocator, registry, access,
                                             snapshot.busSendCount, snapshot.busSends);
            !flattened) {
            return Made::Failure(flattened.Error());
        }
    }

    if (auto valid = ValidateSoundAttenuation(snapshot); !valid) {
        return Made::Failure(valid.Error());
    }

    return Made::Success(Move(snapshot));
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
    playable.priority = priority;
    playable.bypassVolumeScaleForPriority = bypassVolumeScaleForPriority;
    playable.virtualizationMode = virtualizationMode;
    playable.normalizeStereo = normalizeStereo;
    playable.asset = Id();
    if (!category.Handle().IsNull()) {
        auto resolvedCategory = registry.Resolve<SoundCategory>(access, category.Handle());
        if (!resolvedCategory) {
            return Made::Failure(ErrorCode::InvalidArgument);
        }

        auto categoryObject = resolvedCategory.Value().Get();
        if (!categoryObject) {
            return Made::Failure(ErrorCode::InvalidArgument);
        }

        auto chain = categoryObject.Value()->MakeChain(allocator, registry, access);
        if (!chain) {
            return Made::Failure(chain.Error());
        }

        playable.category = chain.Value();
    }

    if (!attenuation.Handle().IsNull()) {
        auto settings = registry.Resolve<AttenuationSettings>(access, attenuation.Handle());
        if (!settings) {
            return Made::Failure(ErrorCode::InvalidArgument);
        }

        auto resolved = settings.Value().Get();
        if (!resolved) {
            return Made::Failure(ErrorCode::InvalidArgument);
        }

        auto snapshot = resolved.Value()->ToSnapshot(allocator, registry, access);
        if (!snapshot) {
            return Made::Failure(snapshot.Error());
        }

        playable.attenuation = Move(snapshot.Value());
    }

    // 버스(3.45.7절): 기본 버스와 send다.
    playable.enableBaseBus = enableBaseBus;
    playable.enableBusSends = enableBusSends;
    if (!bus.Handle().IsNull()) {
        auto resolvedBus = registry.Resolve<MixBus>(access, bus.Handle());
        if (!resolvedBus) {
            return Made::Failure(ErrorCode::InvalidArgument);
        }

        auto busObject = resolvedBus.Value().Get();
        if (!busObject) {
            return Made::Failure(ErrorCode::InvalidArgument);
        }

        auto busRef = busObject.Value()->MakeRef(allocator, registry, access);
        if (!busRef) {
            return Made::Failure(busRef.Error());
        }

        playable.bus = busRef.Value();
    }

    if (auto flattened = FlattenBusSends(busSends, false, allocator, registry, access,
                                         playable.busSendCount, playable.busSends);
        !flattened) {
        return Made::Failure(flattened.Error());
    }

    // 동시 발음: 재정의면 이 사운드만의 그룹 하나(키는 이 객체), 아니면 참조한 그룹들(3.41.2절).
    if (overrideConcurrency) {
        playable.concurrencyCount = 1;
        playable.concurrency[0].key = AudioObjectKey(Handle());
        playable.concurrency[0].isOverride = true;
        playable.concurrency[0].settings = concurrencyOverrides;
    } else {
        if (concurrency.Size() > MaxConcurrencyGroups) {
            return Made::Failure(ErrorCode::CapacityExceeded);
        }

        for (usize index = 0; index < concurrency.Size(); ++index) {
            auto group = registry.Resolve<ConcurrencyGroup>(access, concurrency[index]);
            if (!group) {
                return Made::Failure(ErrorCode::InvalidArgument);
            }

            auto resolved = group.Value().Get();
            if (!resolved) {
                return Made::Failure(ErrorCode::InvalidArgument);
            }

            auto entry = resolved.Value()->MakeEntry();
            if (!entry) {
                return Made::Failure(entry.Error());
            }

            // 같은 그룹을 두 번 참조했으면 하나로 본다(UE ConcurrencySet은 집합).
            bool duplicate = false;
            for (uint32 earlier = 0; earlier < playable.concurrencyCount; ++earlier) {
                duplicate = duplicate || playable.concurrency[earlier].key == entry.Value().key;
            }

            if (!duplicate) {
                playable.concurrency[playable.concurrencyCount++] = entry.Value();
            }
        }
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

    Array<uint8> joined(allocator);
    auto read = ReadResidentBytes(reader, context, joined);
    if (!read) {
        return Result<void>::Failure(read.Error());
    }

    return LoadPcmBytes(allocator, read.Value(), decoders);
}

Result<void> SoundClip::LoadPcmBytes(Allocator& allocator, Span<const uint8> bytes,
                                     const audio::AudioDecoderTable& decoders) noexcept {
    // 형식 위반은 Parse가 CorruptData로, 모르는 판·codec은 Unsupported로 알린다(3.38절 — 그대로
    // 넘긴다).
    auto view = audio::AudioPlatformDataView::Parse(bytes);
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

    Array<uint8> joined(allocator);
    auto read = ReadResidentBytes(reader, context, joined);
    if (!read) {
        return Result<void>::Failure(read.Error());
    }

    return LoadStreamBytes(allocator, read.Value(), decoders);
}

Result<void> SoundClip::LoadStreamBytes(Allocator& allocator, Span<const uint8> bytes,
                                        const audio::AudioDecoderTable& decoders) noexcept {
    // 복사·해석·모든 청크 검증(3.40.2절 — 공급자의 "검증된 payload" 계약을 여기서 만든다).
    auto made = SoundStreamData::TryCreate(allocator, bytes);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    return AdoptStream(Move(made.Value()), decoders);
}

Result<void> SoundClip::AdoptStream(SharedPtr<SoundStreamData> made,
                                    const audio::AudioDecoderTable& decoders) noexcept {
    const SoundStreamData& data = *made;
    if (data.ChannelCount() != channelCount || data.SampleRate() != sampleRate ||
        data.FrameCount() != frameCount) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    if (!decoders.Contains(data.View().Header().codec)) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    stream_ = SharedPtr<const SoundStreamData>(Move(made));
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

    // 분류 계층의 값이 다음이다(UE SoundClass LoadingBehavior — 3.12절).
    if (options.categoryLoading != SoundLoadingBehavior::Inherited) {
        return options.categoryLoading;
    }

    if (options.defaultLoading < SoundLoadingBehavior::RetainOnLoad) {
        return SoundLoadingBehavior::RetainOnLoad;
    }

    return options.defaultLoading > SoundLoadingBehavior::LoadOnDemand
               ? SoundLoadingBehavior::LoadOnDemand
               : options.defaultLoading;
}

bool SoundClip::UsesWholeDecode() const noexcept {
    uint32 loopBegin = 0;
    uint32 loopEnd = 0;
    EffectiveLoop(loopBegin, loopEnd);
    return IsWholeDecodeLoop(looping, loopBegin, loopEnd, frameCount, sampleRate);
}

bool SoundClip::IsWholeDecodeLoop(bool looping, uint32 loopBegin, uint32 loopEnd, uint32 frames,
                                  uint32 sampleRate) noexcept {
    // 짧은 loop는 바퀴마다 디코더를 다시 여는 비용이 커서 통째로 푼다(3.40.2절).
    const uint64 loopLength = loopBegin == 0 && loopEnd == 0 ? frames : loopEnd - loopBegin;
    return looping && loopLength < static_cast<uint64>(sampleRate) * StreamMinLoopSeconds;
}

bool SoundClip::HasCookedData() const noexcept {
    const SoundPcmOrigin origin =
        UsesWholeDecode() ? SoundPcmOrigin::Cooked : SoundPcmOrigin::CookedStream;
    return !platformData.IsEmpty() &&
           SameKey(pcmKey_, origin, platformData, SoundSourceStorage::None, channelCount,
                   sampleRate, frameCount);
}

Result<void> SoundClip::LoadCooked(Allocator& allocator, SoundBulkReader reader, void* context,
                                   const audio::AudioDecoderTable& decoders,
                                   const SoundCookedLoadOptions& options,
                                   SharedPtr<SoundStreamedPayload> streamed) noexcept {
    // 로딩 동작은 엔진 로드 도우미의 청크 1 후크가 쓴다(3.46.5절).
    (void)options;
    if (!streamed) {
        // 상주 경로다. payload가 패키지 안 bulk(sidecar 출력이 없는 쿠킹)여도 두 bulk를 이어
        // 읽는다.
        return UsesWholeDecode() ? LoadCookedPcm(allocator, reader, context, decoders)
                                 : LoadCookedStream(allocator, reader, context, decoders);
    }

    // sidecar 경로다(3.46.3절): 메타 + 청크 0만 메모리에 둔다. sidecar가 없는 clip(패키지만)이나
    // 통째 디코드 clip에는 streamed를 줄 수 없다.
    if (streamedPayload.IsEmpty() || UsesWholeDecode()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

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

    auto prefix = reader(context, platformData);
    if (!prefix) {
        return Result<void>::Failure(prefix.Error());
    }

    auto made = SoundStreamData::TryCreateStreamed(allocator, prefix.Value(), Move(streamed));
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    return AdoptStream(Move(made.Value()), decoders);
}

Result<Span<const uint8>> SoundClip::ReadResidentBytes(SoundBulkReader reader, void* context,
                                                       Array<uint8>& joined) const noexcept {
    using Read = Result<Span<const uint8>>;
    auto prefix = reader(context, platformData);
    if (!prefix || streamedPayload.IsEmpty()) {
        return prefix;
    }

    // payload가 패키지 안 bulk다(sidecar 출력이 없는 쿠킹 — 3.46.3절): 메타 뒤에 payload 전체를
    // 이어 붙인다. prefix를 먼저 해석해 메타 길이를 정한다(prefix의 청크 0은 payload 쪽과 같은
    // byte다).
    auto meta = audio::AudioPlatformDataView::Parse(prefix.Value());
    if (!meta) {
        return Read::Failure(meta.Error());
    }

    if (auto copied = joined.TryAppend(prefix.Value().Slice(0, meta.Value().MetaBytes()));
        !copied) {
        return Read::Failure(copied.Error());
    }

    auto payload = reader(context, streamedPayload);
    if (!payload) {
        return Read::Failure(payload.Error());
    }

    if (auto copied = joined.TryAppend(payload.Value()); !copied) {
        return Read::Failure(copied.Error());
    }

    return Read::Success(joined.View());
}

} // namespace rocks
