#include <AudioTools/OpusAudioEncoder.h>

#include "AudioEncoderInternal.h"

#include <Audio/OpusCodecHeader.h>
#include <Audio/VorbisChannelOrder.h>
#include <Base/Endian.h>
#include <Base/NumericLimits.h>

#include <opus_multistream.h>

namespace rocks::audio {

namespace {

// Opus 인코더 판(너겟 키용)이다. libopus 판이나 설정이 바뀌어 출력 byte가 바뀌면 올린다.
constexpr uint32 OpusEncoderVersion = 1;

constexpr AudioCodecInfo OpusInfo = {AudioCodecId::Opus,     "Opus",
                                     OpusEncoderVersion,     OpusBitstreamVersion,
                                     OpusPlatformSampleRate, OpusPlatformSampleRate,
                                     MaxOutputChannels};

// 품질 → 채널당 비트레이트 선형 대응 끝점이다(UE GetBitRateFromQuality).
constexpr uint32 OpusMinBitratePerChannel = 16000;
constexpr uint32 OpusMaxBitratePerChannel = 96000;

// libopus 상태 정렬이다(구조체에 double·포인터가 있다).
constexpr usize OpusEncoderStateAlignment = 16;

// libopus 인코더 상태 메모리다(allocator로 잡고 풀어 준다).
class OpusEncoderMemory {
public:
    explicit OpusEncoderMemory(Allocator& allocator) noexcept : allocator_(allocator) {
    }

    ~OpusEncoderMemory() {
        if (data_ != nullptr) {
            (void)allocator_.Deallocate(data_);
        }
    }

    OpusEncoderMemory(const OpusEncoderMemory&) = delete;
    OpusEncoderMemory& operator=(const OpusEncoderMemory&) = delete;

    Result<void> Allocate(usize bytes) noexcept {
        auto allocated = allocator_.Allocate(bytes, OpusEncoderStateAlignment);
        if (!allocated) {
            return Result<void>::Failure(allocated.Error());
        }

        data_ = allocated.Value();
        return Result<void>::Success();
    }

    OpusMSEncoder* Encoder() const noexcept {
        return static_cast<OpusMSEncoder*>(data_);
    }

private:
    Allocator& allocator_;
    void* data_ = nullptr;
};

Result<void> EncodeOpusInto(Allocator& allocator, const PlanarConstView& pcm, uint32 sampleRate,
                            uint32 channelMask, const AudioEncodeSettings& settings,
                            EncodedAudio& out) noexcept {
    if (auto checked = encoderdetail::CheckEncodeInput(pcm, sampleRate, channelMask, OpusInfo);
        !checked) {
        return checked;
    }

    if (settings.quality < MinAudioCompressionQuality ||
        settings.quality > MaxAudioCompressionQuality) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // family 1은 Vorbis 채널 배치만 나타낸다(3.32.2절).
    const uint32 channels = pcm.channelCount;
    if (channelMask != DefaultAudioChannelMask(channels)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    const opus_int32 stateBytes = opus_multistream_surround_encoder_get_size(
        static_cast<int>(channels), OpusMappingFamilyVorbis);
    if (stateBytes <= 0) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    OpusEncoderMemory state(allocator);
    if (auto allocated = state.Allocate(static_cast<usize>(stateBytes)); !allocated) {
        return allocated;
    }

    int streams = 0;
    int coupledStreams = 0;
    unsigned char mapping[MaxOutputChannels] = {};
    OpusMSEncoder* encoder = state.Encoder();
    if (opus_multistream_surround_encoder_init(
            encoder, static_cast<opus_int32>(OpusPlatformSampleRate), static_cast<int>(channels),
            OpusMappingFamilyVorbis, &streams, &coupledStreams, mapping,
            OPUS_APPLICATION_AUDIO) != OPUS_OK) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    const opus_int32 bitrate =
        static_cast<opus_int32>(OpusBitrateForQuality(settings.quality, channels));
    opus_int32 lookahead = 0;
    if (opus_multistream_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate)) != OPUS_OK ||
        opus_multistream_encoder_ctl(encoder, OPUS_GET_LOOKAHEAD(&lookahead)) != OPUS_OK ||
        lookahead < 0 || lookahead > NumericLimits<uint16>::Max()) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    encoderdetail::FillCommon(OpusInfo, pcm, sampleRate, channelMask, out);
    const uint64 frames = pcm.frameCount;
    const uint32 delay = static_cast<uint32>(lookahead);
    // codec frame 합 = ceil((frames + delay) / 960) × 960, 끝 padding = 합 − delay − frames.
    const uint64 packets = (frames + delay + OpusPacketFrames - 1) / OpusPacketFrames;
    out.encoderDelay = delay;
    out.endPadding = static_cast<uint32>(packets * OpusPacketFrames - delay - frames);
    out.preRollFrames = OpusPreRollFrames;

    OpusCodecHeader header;
    header.streams = static_cast<uint8>(streams);
    header.coupledStreams = static_cast<uint8>(coupledStreams);
    header.preSkip = static_cast<uint16>(delay);
    for (uint32 channel = 0; channel < channels; ++channel) {
        header.mapping[channel] = mapping[channel];
    }

    if (auto resized = out.codecHeader.TryResize(OpusCodecHeaderBytes); !resized) {
        return resized;
    }

    StoreOpusCodecHeader(header, channels, out.codecHeader.Data());

    Array<float> input(allocator);
    Array<uint8> packet(allocator);
    if (auto resized = input.TryResize(static_cast<usize>(OpusPacketFrames) * channels); !resized) {
        return resized;
    }

    if (auto resized = packet.TryResize(OpusMaxPacketBytes); !resized) {
        return resized;
    }

    for (uint64 index = 0; index < packets; ++index) {
        // 원본 frame [first, first + count)를 Vorbis 순서 교차로 넣고 끝 뒤는 0이다.
        const uint64 first = index * OpusPacketFrames;
        const uint64 count =
            first >= frames
                ? 0
                : (frames - first < OpusPacketFrames ? frames - first : OpusPacketFrames);
        for (usize sample = 0; sample < input.Size(); ++sample) {
            input[sample] = 0.0f;
        }

        InterleaveVorbisOrder(pcm.channels, channels, first, count, input.Data());
        const opus_int32 bytes = opus_multistream_encode_float(
            encoder, input.Data(), static_cast<int>(OpusPacketFrames), packet.Data(),
            static_cast<opus_int32>(OpusMaxPacketBytes));
        if (bytes <= 0 || bytes > static_cast<opus_int32>(NumericLimits<uint16>::Max())) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        const uint64 offset = out.payload.Size();
        const usize unitBytes = OpusLengthPrefixBytes + static_cast<usize>(bytes);
        auto appended = out.payload.TryAppendUninitialized(unitBytes);
        if (!appended) {
            return Result<void>::Failure(appended.Error());
        }

        uint8* target = appended.Value().Data();
        StoreLittleEndian<uint16>(target, static_cast<uint16>(bytes));
        for (opus_int32 byte = 0; byte < bytes; ++byte) {
            target[OpusLengthPrefixBytes + static_cast<usize>(byte)] =
                packet[static_cast<usize>(byte)];
        }

        if (auto added = out.units.TryPushBack(
                EncodedAudioUnit{offset, static_cast<uint32>(unitBytes), OpusPacketFrames});
            !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

} // namespace

uint32 OpusBitrateForQuality(uint32 quality, uint32 channels) noexcept {
    if (quality < MinAudioCompressionQuality || quality > MaxAudioCompressionQuality ||
        channels == 0 || channels > MaxOutputChannels) {
        return 0;
    }

    const uint32 perChannel =
        OpusMinBitratePerChannel + (quality - MinAudioCompressionQuality) *
                                       (OpusMaxBitratePerChannel - OpusMinBitratePerChannel) /
                                       (MaxAudioCompressionQuality - MinAudioCompressionQuality);
    return perChannel * channels;
}

const AudioCodecInfo& OpusAudioEncoder::Info() const noexcept {
    return OpusInfo;
}

Result<void> OpusAudioEncoder::Encode(const PlanarConstView& pcm, uint32 sampleRate,
                                      uint32 channelMask, const AudioEncodeSettings& settings,
                                      EncodedAudio& out) noexcept {
    out.Reset();
    Result<void> encoded = EncodeOpusInto(allocator_, pcm, sampleRate, channelMask, settings, out);
    if (!encoded) {
        out.Reset();
    }

    return encoded;
}

} // namespace rocks::audio
