#include <Audio/OpusCodecHeader.h>

#include <Base/Endian.h>

namespace rocks::audio {

namespace {

// codec 머리 칸 위치다(3.32.2절).
constexpr usize OpusFamilyOffset = 0;
constexpr usize OpusStreamsOffset = 1;
constexpr usize OpusCoupledOffset = 2;
constexpr usize OpusReservedOffset = 3;
constexpr usize OpusPreSkipOffset = 4;
constexpr usize OpusGainOffset = 6;
constexpr usize OpusMappingOffset = 8;

} // namespace

void StoreOpusCodecHeader(const OpusCodecHeader& header, uint32 channels, uint8* out) noexcept {
    for (usize index = 0; index < OpusCodecHeaderBytes; ++index) {
        out[index] = 0;
    }

    out[OpusFamilyOffset] = OpusMappingFamilyVorbis;
    out[OpusStreamsOffset] = header.streams;
    out[OpusCoupledOffset] = header.coupledStreams;
    StoreLittleEndian<uint16>(out + OpusPreSkipOffset, header.preSkip);
    for (uint32 channel = 0; channel < channels && channel < MaxOutputChannels; ++channel) {
        out[OpusMappingOffset + channel] = header.mapping[channel];
    }
}

Result<OpusCodecHeader> ParseOpusCodecHeader(Span<const uint8> bytes,
                                             const AudioPlatformDataHeader& header) noexcept {
    const uint32 channels = header.channels;
    if (bytes.Size() != OpusCodecHeaderBytes || channels == 0 || channels > MaxOutputChannels) {
        return Result<OpusCodecHeader>::Failure(ErrorCode::InvalidArgument);
    }

    const uint8* data = bytes.Data();
    OpusCodecHeader parsed;
    parsed.streams = data[OpusStreamsOffset];
    parsed.coupledStreams = data[OpusCoupledOffset];
    parsed.preSkip = LoadLittleEndian<uint16>(data + OpusPreSkipOffset);
    const uint32 codedChannels = uint32{parsed.streams} + parsed.coupledStreams;
    // family 1(Vorbis 배치)은 coded 채널 수가 채널 수와 같다(libopus vorbis_mappings). 이 규칙이
    // 없으면 1채널 머리에 stream 255를 적어 디코더 상태를 수 MB 잡게 할 수 있다.
    if (data[OpusFamilyOffset] != OpusMappingFamilyVorbis || parsed.streams == 0 ||
        parsed.coupledStreams > parsed.streams || codedChannels != channels ||
        data[OpusReservedOffset] != 0 ||
        LoadLittleEndian<uint16>(data + OpusGainOffset) != 0 ||
        parsed.preSkip != header.encoderDelay) {
        return Result<OpusCodecHeader>::Failure(ErrorCode::InvalidArgument);
    }

    for (uint32 channel = 0; channel < MaxOutputChannels; ++channel) {
        const uint8 value = data[OpusMappingOffset + channel];
        if (channel < channels ? value >= codedChannels : value != 0) {
            return Result<OpusCodecHeader>::Failure(ErrorCode::InvalidArgument);
        }

        parsed.mapping[channel] = value;
    }

    return Result<OpusCodecHeader>::Success(parsed);
}

} // namespace rocks::audio
