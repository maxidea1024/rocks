#include <AudioTools/AdpcmEncoder.h>
#include <AudioTools/AudioEncoder.h>

#include <Audio/Adpcm.h>
#include <Base/Endian.h>
#include <Core/Math/MathUtility.h>

namespace rocks::audio {

namespace {

// PCM16·ADPCM 인코더 판(너겟 키용)이다. 출력 byte가 바뀌면 올린다.
constexpr uint32 Pcm16EncoderVersion = 1;
constexpr uint32 AdpcmEncoderVersion = 1;

constexpr AudioCodecInfo Pcm16Info = {AudioCodecId::Pcm16,   "PCM16",
                                      Pcm16EncoderVersion,   Pcm16BitstreamVersion,
                                      MinPlatformSampleRate, MaxPlatformSampleRate,
                                      MaxOutputChannels};
constexpr AudioCodecInfo AdpcmInfo = {AudioCodecId::Adpcm,   "ADPCM",
                                      AdpcmEncoderVersion,   AdpcmBitstreamVersion,
                                      MinPlatformSampleRate, MaxPlatformSampleRate,
                                      MaxOutputChannels};

// 공통 입력 검사다(채널·마스크·rate·frame·비유한 표본).
Result<void> CheckEncodeInput(const PlanarConstView& pcm, uint32 sampleRate, uint32 channelMask,
                              const AudioCodecInfo& info) noexcept {
    if (pcm.channelCount == 0 || pcm.channelCount > info.maxChannels || pcm.frameCount == 0 ||
        (channelMask & ~AudioChannelMaskAll) != 0 ||
        AudioChannelMaskCount(channelMask) != pcm.channelCount || sampleRate < info.minSampleRate ||
        sampleRate > info.maxSampleRate) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (uint32 channel = 0; channel < pcm.channelCount; ++channel) {
        for (uint32 frame = 0; frame < pcm.frameCount; ++frame) {
            if (!math::IsFinite(pcm.channels[channel][frame])) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    return Result<void>::Success();
}

void FillCommon(const AudioCodecInfo& info, const PlanarConstView& pcm, uint32 sampleRate,
                uint32 channelMask, EncodedAudio& out) noexcept {
    out.codec = info.codec;
    out.bitstreamVersion = info.bitstreamVersion;
    out.encoderVersion = info.encoderVersion;
    out.channels = pcm.channelCount;
    out.channelMask = channelMask;
    out.sampleRate = sampleRate;
    out.frames = pcm.frameCount;
}

Result<void> EncodePcm16Into(const PlanarConstView& pcm, uint32 sampleRate, uint32 channelMask,
                             EncodedAudio& out) noexcept {
    if (auto checked = CheckEncodeInput(pcm, sampleRate, channelMask, Pcm16Info); !checked) {
        return checked;
    }

    FillCommon(Pcm16Info, pcm, sampleRate, channelMask, out);
    const usize frameBytes = static_cast<usize>(pcm.channelCount) * Pcm16SampleBytes;
    auto bytes =
        out.payload.TryAppendUninitialized(static_cast<usize>(pcm.frameCount) * frameBytes);
    if (!bytes) {
        return Result<void>::Failure(bytes.Error());
    }

    uint8* target = bytes.Value().Data();
    for (uint32 frame = 0; frame < pcm.frameCount; ++frame) {
        for (uint32 channel = 0; channel < pcm.channelCount; ++channel) {
            StoreLittleEndian<uint16>(
                target, static_cast<uint16>(QuantizePcm16(pcm.channels[channel][frame])));
            target += Pcm16SampleBytes;
        }
    }

    // 단위 = Pcm16UnitFrames frame(마지막은 남은 frame)이다.
    for (uint64 first = 0; first < pcm.frameCount; first += Pcm16UnitFrames) {
        const uint32 frames = static_cast<uint32>(
            pcm.frameCount - first < Pcm16UnitFrames ? pcm.frameCount - first : Pcm16UnitFrames);
        const EncodedAudioUnit unit = {first * frameBytes, static_cast<uint32>(frames * frameBytes),
                                       frames};
        if (auto added = out.units.TryPushBack(unit); !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

Result<void> EncodeAdpcmInto(const PlanarConstView& pcm, uint32 sampleRate, uint32 channelMask,
                             EncodedAudio& out) noexcept {
    if (auto checked = CheckEncodeInput(pcm, sampleRate, channelMask, AdpcmInfo); !checked) {
        return checked;
    }

    FillCommon(AdpcmInfo, pcm, sampleRate, channelMask, out);
    if (auto encoded = EncodeAdpcm(pcm, out.payload); !encoded) {
        return encoded;
    }

    // 단위 = 블록 묶음(채널 × 512 byte, 1012 frame — 마지막은 남은 frame)이다.
    const uint32 groupBytes = pcm.channelCount * AdpcmBlockBytes;
    uint64 offset = 0;
    for (uint64 first = 0; first < pcm.frameCount; first += AdpcmFramesPerBlock) {
        const uint32 frames = static_cast<uint32>(pcm.frameCount - first < AdpcmFramesPerBlock
                                                      ? pcm.frameCount - first
                                                      : AdpcmFramesPerBlock);
        if (auto added = out.units.TryPushBack(EncodedAudioUnit{offset, groupBytes, frames});
            !added) {
            return added;
        }

        offset += groupBytes;
    }

    return Result<void>::Success();
}

} // namespace

void EncodedAudio::Reset() noexcept {
    codec = AudioCodecId::None;
    bitstreamVersion = 0;
    encoderVersion = 0;
    channels = 0;
    channelMask = 0;
    sampleRate = 0;
    frames = 0;
    encoderDelay = 0;
    endPadding = 0;
    preRollFrames = 0;
    codecHeader.Clear();
    payload.Clear();
    units.Clear();
}

const AudioCodecInfo& Pcm16AudioEncoder::Info() const noexcept {
    return Pcm16Info;
}

Result<void> Pcm16AudioEncoder::Encode(const PlanarConstView& pcm, uint32 sampleRate,
                                       uint32 channelMask, const AudioEncodeSettings&,
                                       EncodedAudio& out) noexcept {
    out.Reset();
    Result<void> encoded = EncodePcm16Into(pcm, sampleRate, channelMask, out);
    if (!encoded) {
        out.Reset();
    }

    return encoded;
}

const AudioCodecInfo& AdpcmAudioEncoder::Info() const noexcept {
    return AdpcmInfo;
}

Result<void> AdpcmAudioEncoder::Encode(const PlanarConstView& pcm, uint32 sampleRate,
                                       uint32 channelMask, const AudioEncodeSettings&,
                                       EncodedAudio& out) noexcept {
    out.Reset();
    Result<void> encoded = EncodeAdpcmInto(pcm, sampleRate, channelMask, out);
    if (!encoded) {
        out.Reset();
    }

    return encoded;
}

} // namespace rocks::audio
