#include <AudioTools/SoundClipCook.h>

#include <AssetCooker/CookBulkSource.h>
#include <AssetNugget/NuggetCache.h>
#include <AssetNugget/NuggetScc.h>
#include <AudioAssets/SoundClip.generated.h>
#include <AudioTools/AudioEncoder.h>
#include <AudioTools/AudioPlatformDataWriter.h>
#include <AudioTools/CookResampler.h>
#include <AudioTools/OpusAudioEncoder.h>
#include <Base/NumericLimits.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Math/MathUtility.h>
#include <Core/Move.h>
#include <Core/Text/String.h>
#include <Object/ObjectRegistry.h>

// 사운드 clip 쿠킹 처리기다(Docs/Design/AudioSystem 3.37.2·3.37.3절 — AU5b).
namespace rocks {

static_assert(AudioCookMinSampleRate == audio::MinPlatformSampleRate &&
                  AudioCookMaxSampleRate == audio::MaxPlatformSampleRate,
              "TargetProfile audio cook rate range must match the audio platform data rate range");

// 유니티 빌드에서 다른 처리기의 같은 이름 도우미와 겹치지 않게 파일 전용 namespace에 둔다.
namespace sound_clip_cook {

using audio::AudioCodecId;

// 인코더 판 문자열의 최대 byte 수다("e<u32>.b<u32>.simd-<수준>").
constexpr usize EncoderVersionTextBytes = 64;
// Opus 인코더 설정 byte(품질 u32 LE)다.
constexpr usize OpusSettingsBytes = 4;
constexpr uint32 ByteBits = 8;
constexpr uint32 ByteMask = 0xFFu;
// uint32 십진 자릿수 상한과 기수다.
constexpr usize MaxDecimalDigits = 10;
constexpr uint32 DecimalBase = 10;
// clip 정규화 기준(최대 절댓값 — full scale)이다(3.39.4절).
constexpr float SoundCookClipPeak = 1.0f;

constexpr LogCategory LogSoundCook{"SoundCook"};

// 쿠킹 패키지 bulk 최소 정렬이다(0 = 프로파일 값 그대로 — AU5는 플랫폼 데이터 전체를 bulk 하나에
// 넣고 렌더처럼 큰 정렬 요구가 없다. 청크 sidecar는 AU6).
constexpr uint32 SoundCookBulkAlignment = 0;

// 실패 진단을 채우고 error로 실패한다(할당 실패는 진단 코드 없이).
Result<void> CookFail(const CookContext& context, AssetDiagnosticCode code, ErrorCode error,
                      uint64 detail = 0) noexcept {
    if (context.failure != nullptr) {
        *context.failure = AssetDiagnostic{};
        context.failure->code = error == ErrorCode::OutOfMemory ? AssetDiagnosticCode::None : code;
        context.failure->error = error;
        context.failure->asset = context.entry != nullptr ? context.entry->id : AssetId{};
        context.failure->detail = detail;
    }

    return Result<void>::Failure(error);
}

// 처리기 경고를 문맥에 둔다(없으면 버린다). 할당 실패는 OutOfMemory다.
Result<void> CookWarn(const CookContext& context, AssetDiagnosticCode code) noexcept {
    if (context.warnings == nullptr) {
        return Result<void>::Success();
    }

    const AssetId asset = context.entry != nullptr ? context.entry->id : AssetId{};
    return context.warnings->TryPushBack(CookDiagnostic{code, true, asset, {}, 0, ErrorCode::None});
}

// 구간 [begin, end)가 frames 안의 올바른 loop인지다((0, 0)은 clip 전체라 올바르다).
bool ValidLoop(uint32 begin, uint32 end, uint32 frames) noexcept {
    return (begin == 0 && end == 0) || (begin < end && end <= frames);
}

// 위치 하나를 인코딩 rate로 바꾸고 limit으로 자른다(같은 rate면 자르기만). 자르기는 uint64에서 한다
// (비활성 loop의 큰 값이 uint32로 잘려 작은 위치가 되지 않게).
Result<uint32> ConvertPosition(uint32 position, uint32 sourceRate, uint32 targetRate,
                               uint32 limit) noexcept {
    uint64 converted = position;
    if (sourceRate != targetRate) {
        auto made = audio::CookResampledPosition(position, sourceRate, targetRate);
        if (!made) {
            return Result<uint32>::Failure(made.Error());
        }

        converted = made.Value();
    }

    return Result<uint32>::Success(static_cast<uint32>(converted < limit ? converted : limit));
}

// 구간 하나를 바꾸고 맞춘다. clip 전체((0, 0))인지는 원본 값으로 정한다 — 환산 뒤 (0, 0)이 된 짧은
// loop([0, 1) 96 kHz → 48 kHz 등)는 전체가 아니라 길이 1로 맞춘다. active가 아니면 위치만 바꿔
// frames로 자른다.
Result<void> ConvertLoop(uint32 begin, uint32 end, bool active, uint32 sourceRate,
                         uint32 targetRate, uint32 frames, uint32& outBegin,
                         uint32& outEnd) noexcept {
    if (active && begin == 0 && end == 0) {
        outBegin = 0;
        outEnd = 0;
        return Result<void>::Success();
    }

    auto convertedBegin = ConvertPosition(begin, sourceRate, targetRate, frames);
    auto convertedEnd = ConvertPosition(end, sourceRate, targetRate, frames);
    if (!convertedBegin || !convertedEnd) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    outBegin = convertedBegin.Value();
    outEnd = convertedEnd.Value();
    if (active) {
        ClampSoundCookedLoop(outBegin, outEnd, frames);
    }

    return Result<void>::Success();
}

// Cook이 쓰는 clip 값 사본이다(Borrow를 오래 쥐지 않는다).
struct ClipFacts {
    SoundClipSource source;
    SoundCompression compression = SoundCompression::PlatformDefault;
    uint32 quality = 0;
    // 대상 프로파일 배수를 곱한 실효 품질이다(인코더·너겟 키가 쓴다 — 3.39.3절).
    uint32 effectiveQuality = 0;
    uint32 channels = 0;
    uint32 sampleRate = 0;
    uint32 frames = 0;
    uint32 sourceLoopBegin = 0;
    uint32 sourceLoopEnd = 0;
    // payload를 sidecar에 둘 수 있는 clip이다(ForceInline·통째 디코드가 아님 — 3.46.2절).
    bool sidecar = false;
};

// 코덱의 인코더다(Opus는 allocator로 libopus 상태를 잡는다).
struct Encoders {
    explicit Encoders(Allocator& allocator) noexcept : opus(allocator) {
    }

    audio::AudioEncoder* For(AudioCodecId codec) noexcept {
        switch (codec) {
        case AudioCodecId::Pcm16:
            return &pcm16;
        case AudioCodecId::Adpcm:
            return &adpcm;
        case AudioCodecId::Opus:
            return &opus;
        default:
            return nullptr;
        }
    }

    audio::Pcm16AudioEncoder pcm16;
    audio::AdpcmAudioEncoder adpcm;
    audio::OpusAudioEncoder opus;
};

// 인코더 판 문자열이다("e<encoderVersion>.b<bitstreamVersion>", Opus는 ".simd-<수준>"을 더한다 —
// 3.37.3절 5번. PCM16·ADPCM 인코더는 SIMD를 쓰지 않으므로 그 수준이 바뀌어도 키가 그대로다).
void FormatEncoderVersion(const audio::AudioCodecInfo& info, char (&out)[EncoderVersionTextBytes],
                          usize& length) noexcept {
    length = 0;
    const auto put = [&](char value) noexcept {
        if (length + 1 < EncoderVersionTextBytes) {
            out[length++] = value;
        }
    };

    const auto text = [&](StringView value) noexcept {
        for (usize index = 0; index < value.ByteCount(); ++index) {
            put(value.Data()[index]);
        }
    };

    const auto number = [&](uint32 value) noexcept {
        char digits[MaxDecimalDigits];
        usize count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % DecimalBase);
            value /= DecimalBase;
        } while (value != 0);

        while (count != 0) {
            put(digits[--count]);
        }
    };

    put('e');
    number(info.encoderVersion);
    text(".b");
    number(info.bitstreamVersion);
    if (info.codec == AudioCodecId::Opus) {
        text(".simd-");
        text(SoundCookOpusSimdLevel);
    }

    out[length] = '\0';
}

// 너겟 지역 키다(3.37.3절 5번).
Result<Sha256Digest> CookLocalKey(const CookContext& context, const ClipFacts& facts,
                                  const SoundCookedLayout& layout,
                                  const audio::AudioCodecInfo& info) noexcept {
    NuggetKeyWriter writer;
    WriteNuggetKeyHeader(writer, SoundNuggetProcessorId, context.nuggets->ProjectGeneration());
    writer.WriteGuid(TypeIdOf<SoundClip>());
    writer.WriteDigest(facts.source.bulk.sha256);
    writer.WriteU64(facts.source.bulk.rawBytes);
    writer.WriteU32(static_cast<uint32>(facts.source.format));
    writer.WriteU32(facts.channels);
    writer.WriteU32(facts.sampleRate);
    writer.WriteU32(facts.frames);
    writer.WriteU32(static_cast<uint32>(layout.codec));
    // 인코딩 rate(대상 프로파일 재정의의 정규화 값 — 3.39.4절)다.
    writer.WriteU32(layout.sampleRate);
    // 재생 loop 구간(원본 rate)이다 — 머리 loop를 정한다. 파일 loop·override·cue 위치는 쿠킹 필드라
    // 너겟 값에 들지 않는다(layout이 매번 다시 계산한다).
    writer.WriteU32(facts.sourceLoopBegin);
    writer.WriteU32(facts.sourceLoopEnd);
    // 청크 배치(쓰기 상수 — 바뀌면 출력 byte가 바뀐다).
    const audio::AudioPlatformDataOptions chunking;
    writer.WriteU32(chunking.targetChunkBytes);
    writer.WriteU32(chunking.firstChunkBytes);
    char version[EncoderVersionTextBytes];
    usize versionLength = 0;
    FormatEncoderVersion(info, version, versionLength);
    // 품질을 쓰는 codec(Opus)만 품질을 넣는다(PCM16·ADPCM은 품질을 바꿔도 같은 byte다).
    uint8 settings[OpusSettingsBytes] = {};
    usize settingsLength = 0;
    if (layout.codec == AudioCodecId::Opus) {
        for (uint32 index = 0; index < OpusSettingsBytes; ++index) {
            settings[index] =
                static_cast<uint8>((facts.effectiveQuality >> (index * ByteBits)) & ByteMask);
        }

        settingsLength = OpusSettingsBytes;
    }

    writer.WriteList(1);
    WriteNuggetEncoder(writer, StringView(info.name), StringView(version, versionLength),
                       Span<const uint8>(settings, settingsLength));
    writer.WriteBool(layout.resampled);
    if (layout.resampled) {
        writer.WriteU32(audio::CookResamplerVersion);
    }

    writer.WriteU32(audio::AudioPlatformDataVersion);
    return writer.Finish();
}

// 플랫폼 데이터 머리가 layout과 맞는지다(3.37.3절 6번 — 적중 수용 검사, verify 결과 대조에도 쓴다).
bool MatchesLayout(Span<const uint8> bytes, const SoundCookedLayout& layout,
                   uint32 channels) noexcept {
    auto view = audio::AudioPlatformDataView::Parse(bytes);
    if (!view) {
        return false;
    }

    const audio::AudioPlatformDataHeader& header = view.Value().Header();
    return header.codec == layout.codec && header.channels == channels &&
           header.channelMask == audio::DefaultAudioChannelMask(channels) &&
           header.sampleRate == layout.sampleRate && header.frames == layout.frames &&
           header.loopStart == layout.playLoopBegin && header.loopEnd == layout.playLoopEnd;
}

// 원본을 풀고(필요하면 리샘플) 인코드해 플랫폼 데이터 byte를 만든다(3.37.3절 6번 미적중 경로).
Result<void> BuildPlatformData(const CookContext& context, const ClipFacts& facts,
                               const SoundCookedLayout& layout, Encoders& encoders,
                               Array<uint8>& out) noexcept {
    Allocator& allocator = *context.allocator;
    auto read = context.bulkSource->ReadBulk(facts.source.bulk);
    if (!read) {
        return CookFail(context, AssetDiagnosticCode::InvalidSource, read.Error());
    }

    auto pcm = DecodeSoundClipSource(allocator, facts.source.format, facts.channels,
                                     facts.sampleRate, facts.frames, read.Value());
    if (!pcm) {
        return CookFail(context, AssetDiagnosticCode::InvalidSource, pcm.Error());
    }

    audio::PlanarConstView view;
    view.channelCount = facts.channels;
    view.frameCount = facts.frames;
    for (uint32 channel = 0; channel < facts.channels; ++channel) {
        view.channels[channel] = pcm.Value()->Channel(channel).Data();
    }

    audio::DecodedAudioPcm resampled(allocator);
    if (layout.resampled) {
        if (auto done = audio::ResampleForCooking(view, facts.sampleRate, layout.sampleRate,
                                                  allocator, resampled);
            !done) {
            return CookFail(context, AssetDiagnosticCode::None, done.Error());
        }

        view = audio::PlanarConstView{};
        view.channelCount = resampled.ChannelCount();
        view.frameCount = static_cast<uint32>(resampled.FrameCount());
        for (uint32 channel = 0; channel < view.channelCount; ++channel) {
            view.channels[channel] = resampled.Channel(channel);
        }
    }

    // 리샘플 frame 수는 layout 계산과 같은 규칙이다(어긋나면 위치가 틀어지므로 확인한다).
    if (view.frameCount != layout.frames) {
        return CookFail(context, AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    // clip 정규화(3.39.4절, UE AudioDerivedData "Clip Normalize"): 최대 절댓값이 1을 넘으면(float
    // 원본· 리샘플 overshoot) 전체를 1/max로 낮춘다. 넘지 않으면 복사하지 않는다.
    float peak = 0.0f;
    for (uint32 channel = 0; channel < view.channelCount; ++channel) {
        const float* samples = view.channels[channel];
        for (uint32 frame = 0; frame < view.frameCount; ++frame) {
            peak = math::Max(peak, math::Abs(samples[frame]));
        }
    }

    Array<float> normalized(allocator);
    if (peak > SoundCookClipPeak) {
        Log::Info(LogSoundCook,
                  "Audio clipped during cook: the asset is normalized by a factor of 1/{Peak}; "
                  "consider attenuating it",
                  peak);
        const usize total = static_cast<usize>(view.channelCount) * view.frameCount;
        if (auto resized = normalized.TryResize(total); !resized) {
            return CookFail(context, AssetDiagnosticCode::None, resized.Error());
        }

        const float scale = SoundCookClipPeak / peak;
        for (uint32 channel = 0; channel < view.channelCount; ++channel) {
            float* target = normalized.Data() + static_cast<usize>(channel) * view.frameCount;
            const float* samples = view.channels[channel];
            for (uint32 frame = 0; frame < view.frameCount; ++frame) {
                target[frame] = samples[frame] * scale;
            }

            view.channels[channel] = target;
        }
    }

    audio::AudioEncoder* encoder = encoders.For(layout.codec);
    audio::EncodedAudio encoded(allocator);
    audio::AudioEncodeSettings settings;
    settings.quality = facts.effectiveQuality;
    if (auto done =
            encoder->Encode(view, layout.sampleRate, audio::DefaultAudioChannelMask(facts.channels),
                            settings, encoded);
        !done) {
        return CookFail(context, AssetDiagnosticCode::None, done.Error());
    }

    audio::AudioPlatformDataOptions options;
    options.loopStart = layout.playLoopBegin;
    options.loopEnd = layout.playLoopEnd;
    if (auto done = audio::WriteAudioPlatformData(encoded, options, out); !done) {
        return CookFail(context, AssetDiagnosticCode::None, done.Error());
    }

    return Result<void>::Success();
}

// 쿠킹 패키지에 잠시 쓸 값이다(3.37.3절 7번 — 모두 지역에 먼저 만든 뒤 교환한다).
struct CookedFields {
    explicit CookedFields(Allocator& allocator) noexcept : cues(allocator) {
    }

    uint32 sampleRate = 0;
    uint32 frameCount = 0;
    uint32 fileLoopBegin = 0;
    uint32 fileLoopEnd = 0;
    uint32 overrideBegin = 0;
    uint32 overrideEnd = 0;
    Array<SoundCuePoint> cues;
    BulkDataRef platformData;
    BulkDataRef streamedPayload;
};

template <class T>
void SwapValue(T& left, T& right) noexcept {
    T held = Move(left);
    left = Move(right);
    right = Move(held);
}

// 객체 필드와 교환한다(할당하지 않는다 — 실패하지 않는다). 두 번 부르면 원래대로다.
void SwapCookedFields(SoundClip& clip, CookedFields& fields) noexcept {
    SwapValue(clip.sampleRate, fields.sampleRate);
    SwapValue(clip.frameCount, fields.frameCount);
    SwapValue(clip.fileLoopBegin, fields.fileLoopBegin);
    SwapValue(clip.fileLoopEnd, fields.fileLoopEnd);
    SwapValue(clip.loopOverride.begin, fields.overrideBegin);
    SwapValue(clip.loopOverride.end, fields.overrideEnd);
    SwapValue(clip.cues, fields.cues);
    SwapValue(clip.platformData, fields.platformData);
    SwapValue(clip.streamedPayload, fields.streamedPayload);
}

// 플랫폼 데이터 bulk를 더한다(3.46.2절). sidecar clip이고 청크가 둘 이상이면 패키지 bulk =
// 메타 + 청크 0(앞부분), sidecar bulk = payload 영역 전체다(위치·SHA가 나누기 전과 같다
// — 3.30.1절). 아니면 플랫폼 데이터 전체를 패키지 bulk 하나로 둔다. sidecar 출력이 없는 쿠킹(서버
// 프로필)은 쓰기가 Sidecar 배치를 패키지 안으로 돌린다(AssetSystem 20.5 2).
Result<void> AddPlatformBulks(const CookContext& context, CookChainedBulkSource& bulk,
                              Span<const uint8> platformBytes, bool sidecar,
                              CookedFields& fields) noexcept {
    // 나누는 자리(메타 + 청크 0 끝)와 payload 영역 시작(메타 끝)이다. 나누지 않으면 둘 다 전체다.
    usize prefixBytes = platformBytes.Size();
    usize metaBytes = platformBytes.Size();
    if (sidecar) {
        auto view = audio::AudioPlatformDataView::Parse(platformBytes);
        if (!view) {
            return CookFail(context, AssetDiagnosticCode::None, view.Error());
        }

        if (view.Value().Header().chunkCount > 1) {
            const audio::AudioChunkEntry chunk0 = view.Value().Chunk(0);
            metaBytes = view.Value().MetaBytes();
            prefixBytes =
                metaBytes + static_cast<usize>(chunk0.payloadOffset) + chunk0.payloadBytes;
        }
    }

    auto added = bulk.Add(platformBytes.Slice(0, prefixBytes));
    if (!added) {
        return CookFail(context, AssetDiagnosticCode::None, added.Error());
    }

    fields.platformData = added.Value();
    fields.streamedPayload = BulkDataRef{};
    if (prefixBytes == platformBytes.Size()) {
        return Result<void>::Success();
    }

    auto streamed = bulk.Add(platformBytes.Slice(metaBytes, platformBytes.Size() - metaBytes),
                             PackageBulkPlacement::Sidecar);
    if (!streamed) {
        return CookFail(context, AssetDiagnosticCode::None, streamed.Error());
    }

    fields.streamedPayload = streamed.Value();
    return Result<void>::Success();
}

// 쓰기 Borrow 안에서 교환한다.
Result<void> SwapInObject(ObjectRegistry& registry, const ObjectAccessContext& access,
                          ObjectHandle handle, CookedFields& fields) noexcept {
    auto borrowed = registry.ResolveForWrite<SoundClip>(access, handle);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    SwapCookedFields(*value.Value(), fields);
    return Result<void>::Success();
}

} // namespace sound_clip_cook

audio::AudioCodecId ResolveSoundCompression(SoundCompression compression, uint32 sampleRate,
                                            uint32 frameCount) noexcept {
    switch (compression) {
    case SoundCompression::PlatformDefault:
        return static_cast<uint64>(frameCount) < static_cast<uint64>(sampleRate) * ShortClipSeconds
                   ? audio::AudioCodecId::Adpcm
                   : audio::AudioCodecId::Opus;
    case SoundCompression::Pcm:
        return audio::AudioCodecId::Pcm16;
    case SoundCompression::Adpcm:
        return audio::AudioCodecId::Adpcm;
    case SoundCompression::Opus:
        return audio::AudioCodecId::Opus;
    }

    return audio::AudioCodecId::None;
}

void ClampSoundCookedLoop(uint32& begin, uint32& end, uint32 frames) noexcept {
    if (frames == 0) {
        return;
    }

    begin = begin < frames - 1 ? begin : frames - 1;
    end = end > begin + 1 ? end : begin + 1;
    end = end < frames ? end : frames;
}

uint32 SoundCookEncodingRate(audio::AudioCodecId codec, uint32 sourceRate,
                             SoundSampleRateQuality quality,
                             const AudioCookOverrides& overrides) noexcept {
    if (codec == audio::AudioCodecId::Opus) {
        return audio::OpusPlatformSampleRate;
    }

    if ((codec != audio::AudioCodecId::Pcm16 && codec != audio::AudioCodecId::Adpcm) ||
        !overrides.resampleForDevice) {
        return sourceRate;
    }

    uint32 platformRate = 0;
    switch (quality) {
    case SoundSampleRateQuality::Max:
        platformRate = overrides.maxSampleRate;
        break;
    case SoundSampleRateQuality::High:
        platformRate = overrides.highSampleRate;
        break;
    case SoundSampleRateQuality::Medium:
        platformRate = overrides.mediumSampleRate;
        break;
    case SoundSampleRateQuality::Low:
        platformRate = overrides.lowSampleRate;
        break;
    case SoundSampleRateQuality::Min:
        platformRate = overrides.minSampleRate;
        break;
    }

    return platformRate != 0 && platformRate < sourceRate ? platformRate : sourceRate;
}

uint32 SoundCookEffectiveQuality(uint32 quality, float modifier) noexcept {
    float scaled = static_cast<float>(quality);
    if (math::IsFinite(modifier) && modifier >= 0.0f) {
        // UE와 같은 float 곱이다(정수로 바꾸기 전에 범위를 본다 — 큰 배수의 곱은 inf다).
        scaled = static_cast<float>(quality) * modifier;
        if (!math::IsFinite(scaled)) {
            return audio::MaxAudioCompressionQuality;
        }

        scaled = math::Floor(scaled);
    }

    if (scaled < static_cast<float>(audio::MinAudioCompressionQuality)) {
        return audio::MinAudioCompressionQuality;
    }

    if (scaled >= static_cast<float>(audio::MaxAudioCompressionQuality)) {
        return audio::MaxAudioCompressionQuality;
    }

    return static_cast<uint32>(scaled);
}

Result<void> ComputeSoundCookedLayout(const SoundClip& clip, audio::AudioCodecId codec,
                                      uint32 encodingRate, SoundCookedLayout& out) noexcept {
    using namespace sound_clip_cook;
    const uint32 frames = clip.frameCount;
    const uint32 rate = clip.sampleRate;
    if (codec == AudioCodecId::None || clip.channelCount == 0 || frames == 0 ||
        rate < audio::MinPlatformSampleRate || rate > audio::MaxPlatformSampleRate ||
        encodingRate < audio::MinPlatformSampleRate ||
        encodingRate > audio::MaxPlatformSampleRate ||
        (codec == AudioCodecId::Opus && encodingRate != audio::OpusPlatformSampleRate)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if ((clip.hasFileLoop && !ValidLoop(clip.fileLoopBegin, clip.fileLoopEnd, frames)) ||
        (clip.loopOverride.enabled &&
         !ValidLoop(clip.loopOverride.begin, clip.loopOverride.end, frames))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (const SoundCuePoint& cue : clip.cues) {
        if (cue.frame > frames) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    out.codec = codec;
    out.sampleRate = encodingRate;
    out.resampled = out.sampleRate != rate;
    out.frames = frames;
    if (out.resampled) {
        auto converted = audio::CookResampledFrames(frames, rate, out.sampleRate);
        if (!converted || converted.Value() > NumericLimits<uint32>::Max()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        out.frames = static_cast<uint32>(converted.Value());
    }

    if (auto done = ConvertLoop(clip.fileLoopBegin, clip.fileLoopEnd, clip.hasFileLoop, rate,
                                out.sampleRate, out.frames, out.fileLoopBegin, out.fileLoopEnd);
        !done) {
        return done;
    }

    if (auto done =
            ConvertLoop(clip.loopOverride.begin, clip.loopOverride.end, clip.loopOverride.enabled,
                        rate, out.sampleRate, out.frames, out.overrideBegin, out.overrideEnd);
        !done) {
        return done;
    }

    // 재생 loop는 EffectiveLoop의 구간을 같은 함수로 바꾼다(머리 loop와 필드가 늘 맞는다).
    uint32 playBegin = 0;
    uint32 playEnd = 0;
    clip.EffectiveLoop(playBegin, playEnd);
    if (auto done = ConvertLoop(playBegin, playEnd, true, rate, out.sampleRate, out.frames,
                                out.playLoopBegin, out.playLoopEnd);
        !done) {
        return done;
    }

    out.cueFrames.Clear();
    if (auto reserved = out.cueFrames.TryReserve(clip.cues.Size()); !reserved) {
        return reserved;
    }

    for (const SoundCuePoint& cue : clip.cues) {
        auto converted = ConvertPosition(cue.frame, rate, out.sampleRate, out.frames);
        if (!converted) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        (void)out.cueFrames.TryPushBack(converted.Value());
    }

    return Result<void>::Success();
}

Result<void> SoundClipCookProcessor::Cook(CookContext& context) const noexcept {
    using namespace sound_clip_cook;
    Allocator& allocator = *context.allocator;
    ObjectRegistry& registry = *context.registry;
    const ObjectAccessContext& access = context.access;
    const AssetId assetId = context.entry->id;

    // 1. 대표 에셋과 타입.
    auto asset = registry.FindChild(access, context.package, "");
    if (!asset) {
        return CookFail(context, AssetDiagnosticCode::None, asset.Error());
    }

    auto type = registry.AcquireType(access, asset.Value());
    if (!type) {
        return CookFail(context, AssetDiagnosticCode::None, type.Error());
    }

    if (type.Value().Id() != TypeIdOf<SoundClip>()) {
        return CookFail(context, AssetDiagnosticCode::TypeMismatch, ErrorCode::TypeMismatch);
    }

    // 2–4. 원본·설정 판정, codec, 쿠킹 배치와 쿠킹 값(지역에 먼저 만든다 — 실패해도 객체는 그대로).
    ClipFacts facts;
    SoundCookedLayout layout(allocator);
    // 쿠킹 cue 배열은 에디터 객체 배열의 할당기로 만든다(교환한 동안 객체가 쿠킹 할당기를 붙잡지
    // 않는다).
    Allocator* objectAllocator = nullptr;
    {
        auto borrowed = registry.Resolve<SoundClip>(access, asset.Value());
        if (!borrowed) {
            return CookFail(context, AssetDiagnosticCode::None, borrowed.Error());
        }

        auto value = borrowed.Value().Get();
        if (!value) {
            return CookFail(context, AssetDiagnosticCode::None, value.Error());
        }

        const SoundClip& clip = *value.Value();
        objectAllocator = &clip.cues.GetAllocator();
        facts.source = clip.source;
        facts.compression = clip.compression;
        facts.quality = clip.compressionQuality;
        facts.channels = clip.channelCount;
        facts.sampleRate = clip.sampleRate;
        facts.frames = clip.frameCount;
        clip.EffectiveLoop(facts.sourceLoopBegin, facts.sourceLoopEnd);
        if (facts.source.format == SoundSourceStorage::None || facts.source.bulk.IsEmpty()) {
            return CookFail(context, AssetDiagnosticCode::InvalidSource, ErrorCode::NotReady);
        }

        if (facts.channels > audio::MaxClipChannels) {
            return CookFail(context, AssetDiagnosticCode::UnsupportedSourceFormat,
                            ErrorCode::Unsupported, facts.channels);
        }

        // 원본 사실(임포트 결과) 오류는 설정 오류와 나눠 알린다 — 사용자가 설정으로 고칠 수 없다.
        if (facts.sampleRate < audio::MinPlatformSampleRate ||
            facts.sampleRate > audio::MaxPlatformSampleRate) {
            return CookFail(context, AssetDiagnosticCode::UnsupportedSourceFormat,
                            ErrorCode::Unsupported, facts.sampleRate);
        }

        if (facts.channels == 0 || facts.frames == 0) {
            return CookFail(context, AssetDiagnosticCode::InvalidSource, ErrorCode::CorruptData);
        }

        const AudioCodecId codec =
            ResolveSoundCompression(facts.compression, facts.sampleRate, facts.frames);
        if (codec == AudioCodecId::None || facts.quality < audio::MinAudioCompressionQuality ||
            facts.quality > audio::MaxAudioCompressionQuality) {
            return CookFail(context, AssetDiagnosticCode::InvalidSetting,
                            ErrorCode::InvalidArgument);
        }

        const AudioCookOverrides overrides =
            context.profile != nullptr ? context.profile->audioCook : AudioCookOverrides{};
        facts.effectiveQuality =
            SoundCookEffectiveQuality(facts.quality, overrides.compressionQualityModifier);
        const uint32 encodingRate =
            SoundCookEncodingRate(codec, facts.sampleRate, clip.sampleRateQuality, overrides);
        if (auto computed = ComputeSoundCookedLayout(clip, codec, encodingRate, layout);
            !computed) {
            return CookFail(context, AssetDiagnosticCode::InvalidSetting, computed.Error());
        }

        // sidecar 여부는 쿠킹 값(인코딩 rate·옮긴 loop)으로 런타임 UsesWholeDecode와 같은 식이다
        // (3.46.2절 — EffectiveLoop와 같은 우선순위: override, 파일 loop, 전체).
        uint32 cookedLoopBegin = 0;
        uint32 cookedLoopEnd = 0;
        if (clip.loopOverride.enabled) {
            cookedLoopBegin = layout.overrideBegin;
            cookedLoopEnd = layout.overrideEnd;
        } else if (clip.hasFileLoop) {
            cookedLoopBegin = layout.fileLoopBegin;
            cookedLoopEnd = layout.fileLoopEnd;
        }

        facts.sidecar = clip.loadingBehavior != SoundLoadingBehavior::ForceInline &&
                        !SoundClip::IsWholeDecodeLoop(clip.looping, cookedLoopBegin, cookedLoopEnd,
                                                      layout.frames, layout.sampleRate);
    }

    CookedFields fields(*objectAllocator);
    fields.sampleRate = layout.sampleRate;
    fields.frameCount = layout.frames;
    fields.fileLoopBegin = layout.fileLoopBegin;
    fields.fileLoopEnd = layout.fileLoopEnd;
    fields.overrideBegin = layout.overrideBegin;
    fields.overrideEnd = layout.overrideEnd;
    {
        auto borrowed = registry.Resolve<SoundClip>(access, asset.Value());
        if (!borrowed) {
            return CookFail(context, AssetDiagnosticCode::None, borrowed.Error());
        }

        auto value = borrowed.Value().Get();
        if (!value) {
            return CookFail(context, AssetDiagnosticCode::None, value.Error());
        }

        const Array<SoundCuePoint>& cues = value.Value()->cues;
        if (auto reserved = fields.cues.TryReserve(cues.Size()); !reserved) {
            return CookFail(context, AssetDiagnosticCode::None, reserved.Error());
        }

        // SoundCuePoint 필드(id·frame·name)를 모두 옮긴다. 필드를 더하면 여기도 더한다(빠지면 쿠킹
        // 패키지에서 조용히 사라진다).
        for (usize index = 0; index < cues.Size(); ++index) {
            SoundCuePoint cue(*objectAllocator);
            cue.id = cues[index].id;
            cue.frame = layout.cueFrames[index];
            if (auto named = cue.name.TryAssign(cues[index].name.View()); !named) {
                return CookFail(context, AssetDiagnosticCode::None, named.Error());
            }

            (void)fields.cues.TryPushBack(Move(cue));
        }
    }

    Encoders encoders(allocator);
    const audio::AudioEncoder* encoder = encoders.For(layout.codec);
    const audio::AudioCodecInfo& info = encoder->Info();

    // 5–6. 너겟 조회와 적중 수용 검사.
    NuggetCache* nuggets = context.nuggets;
    Sha256Digest sccKey{};
    NuggetCacheGetResult got(allocator);
    bool hit = false;
    if (nuggets != nullptr) {
        auto localKey = CookLocalKey(context, facts, layout, info);
        if (!localKey) {
            return CookFail(context, AssetDiagnosticCode::None, localKey.Error());
        }

        const NuggetSccMember member{assetId, localKey.Value()};
        auto key = NuggetSccKey(allocator, Span<const NuggetSccMember>(&member, 1), {});
        if (!key) {
            return CookFail(context, AssetDiagnosticCode::None, key.Error());
        }

        sccKey = key.Value();
        lastKey_ = sccKey;
        got = nuggets->GetSccMember(NuggetBucketAudio, sccKey, assetId);
        if (got.failed && nuggets->IsRequired()) {
            return CookFail(context, AssetDiagnosticCode::None,
                            got.error != ErrorCode::None ? got.error : ErrorCode::IoError);
        }

        if (got.status == NuggetStatus::Hit) {
            hit = got.values.Size() == 1 &&
                  MatchesLayout(got.values[0].bytes.View(), layout, facts.channels);
            // 받아들이지 못한 Record는 치운다(남기면 다시 만든 값의 Put이 늘 Conflict다).
            if (!hit) {
                auto rejected = nuggets->Reject(NuggetBucketAudio, sccKey, got.digest);
                if (!rejected && nuggets->IsRequired()) {
                    return CookFail(context, AssetDiagnosticCode::None, rejected.Error());
                }
            }
        }
    }

    // 미적중이거나 verify면 만든다.
    Array<uint8> built(allocator);
    if (!hit || context.verifyNuggets) {
        if (context.bulkSource == nullptr) {
            return CookFail(context, AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
        }

        ++builds_;
        if (auto done = BuildPlatformData(context, facts, layout, encoders, built); !done) {
            return done;
        }

        if (hit) {
            const Span<const uint8> cached = got.values[0].bytes.View();
            const Span<const uint8> fresh = built.View();
            bool same = cached.Size() == fresh.Size();
            for (usize index = 0; same && index < fresh.Size(); ++index) {
                same = cached[index] == fresh[index];
            }

            if (!same) {
                return CookFail(context, AssetDiagnosticCode::NuggetMismatch,
                                ErrorCode::InvalidArgument);
            }
        }
    }

    // 7. 쿠킹 패키지 — 쿠킹 값을 교환해 쓰고, 성공·실패 모두 다시 교환한다.
    const Span<const uint8> platformBytes =
        built.IsEmpty() ? got.values[0].bytes.View() : built.View();
    CookChainedBulkSource bulk(allocator, context.bulkSource);
    if (auto placed = AddPlatformBulks(context, bulk, platformBytes, facts.sidecar, fields);
        !placed) {
        return placed;
    }

    if (auto swapped = SwapInObject(registry, access, asset.Value(), fields); !swapped) {
        return CookFail(context, AssetDiagnosticCode::None, swapped.Error());
    }

    CookContext write = context;
    write.bulkSource = &bulk;
    Result<void> cooked = WriteCookedPackage(write, SoundCookBulkAlignment);
    // 첫 교환이 같은 Borrow를 얻었으므로 되돌리기 실패는 사실상 없다. 그래도 실패하면 에디터 객체에
    // 쿠킹 값이 남은 것이라(저장하면 원본이 바뀐다) 쓰기 오류보다 먼저 알린다.
    Result<void> restored = SwapInObject(registry, access, asset.Value(), fields);
    if (!restored) {
        return CookFail(context, AssetDiagnosticCode::None, restored.Error());
    }

    if (!cooked) {
        return cooked;
    }

    // 8. 미적중이면 Put한다(값 byte를 옮긴다 — 쓰기가 끝난 뒤). Put 실패는 required일 때만 오류다.
    if (nuggets == nullptr || hit) {
        return Result<void>::Success();
    }

    Array<NuggetValue> values(allocator);
    if (auto reserved = values.TryReserve(1); !reserved) {
        return CookFail(context, AssetDiagnosticCode::None, reserved.Error());
    }

    NuggetValue value(allocator);
    value.id = NuggetSccValueId(0, 0);
    value.bytes = Move(built);
    (void)values.TryPushBack(Move(value));
    NuggetCachePut put;
    put.bucket = NuggetBucketAudio;
    put.key = sccKey;
    put.values = values.View();
    const AssetId members[1] = {assetId};
    const NuggetCachePutResult stored = nuggets->PutScc(Span<const AssetId>(members), put);
    if (stored.status == NuggetStatus::Conflict) {
        if (auto warned = CookWarn(context, AssetDiagnosticCode::NuggetConflict); !warned) {
            return warned;
        }
    }

    if (stored.failed && nuggets->IsRequired()) {
        return CookFail(context, AssetDiagnosticCode::None,
                        stored.error != ErrorCode::None ? stored.error : ErrorCode::IoError);
    }

    return Result<void>::Success();
}

Result<void> RegisterSoundClipCookProcessor(void* state, CookProcessorTable& table) noexcept {
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return table.Register(TypeIdOf<SoundClip>(),
                          *static_cast<const SoundClipCookProcessor*>(state));
}

} // namespace rocks
