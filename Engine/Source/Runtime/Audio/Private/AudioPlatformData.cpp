#include <Audio/AudioPlatformData.h>

#include <Base/Endian.h>
#include <Base/NumericLimits.h>
#include <Core/Assert.h>
#include <Core/Encoding/Sha256.h>
#include <Platform/Memory.h>

namespace rocks::audio {

namespace {

// 머리 칸 위치다(3.30.1절 표).
constexpr usize HeaderMagicOffset = 0;
constexpr usize HeaderVersionOffset = 4;
constexpr usize HeaderCodecOffset = 6;
constexpr usize HeaderChannelsOffset = 7;
constexpr usize HeaderBitstreamOffset = 8;
constexpr usize HeaderRateOffset = 12;
constexpr usize HeaderFramesOffset = 16;
constexpr usize HeaderLoopStartOffset = 24;
constexpr usize HeaderLoopEndOffset = 32;
constexpr usize HeaderDelayOffset = 40;
constexpr usize HeaderPaddingOffset = 44;
constexpr usize HeaderChunkCountOffset = 48;
constexpr usize HeaderCodecHeaderOffset = 52;
constexpr usize HeaderChannelMaskOffset = 56;
constexpr usize HeaderReserved32Offset = 60;
constexpr usize HeaderPayloadBytesOffset = 64;
constexpr usize HeaderReserved64Offset = 72;

// 청크 줄 칸 위치다.
constexpr usize ChunkOffsetOffset = 0;
constexpr usize ChunkBytesOffset = 8;
constexpr usize ChunkFlagsOffset = 12;
constexpr usize ChunkFirstFrameOffset = 16;
constexpr usize ChunkFrameCountOffset = 24;
constexpr usize ChunkPreRollOffset = 28;
constexpr usize ChunkHashOffset = 32;

// magic "RAPD"다.
constexpr uint8 Magic[4] = {'R', 'A', 'P', 'D'};
constexpr usize MagicBytes = 4;

constexpr uint64 MaxUInt64 = NumericLimits<uint64>::Max();

usize AlignUp(usize value, usize alignment) noexcept {
    return (value + alignment - 1) / alignment * alignment;
}

// codec id가 표 범위 안인지다(0 제외). 범위 안의 id는 형식만 보고 디코더 유무는 표가 정한다.
bool CodecInTableRange(uint8 codec) noexcept {
    return codec != static_cast<uint8>(AudioCodecId::None) && codec < MaxAudioCodecs;
}

AudioChunkEntry LoadChunk(const uint8* bytes) noexcept {
    AudioChunkEntry chunk;
    chunk.payloadOffset = LoadLittleEndian<uint64>(bytes + ChunkOffsetOffset);
    chunk.payloadBytes = LoadLittleEndian<uint32>(bytes + ChunkBytesOffset);
    chunk.flags = LoadLittleEndian<uint32>(bytes + ChunkFlagsOffset);
    chunk.firstFrame = LoadLittleEndian<uint64>(bytes + ChunkFirstFrameOffset);
    chunk.frameCount = LoadLittleEndian<uint32>(bytes + ChunkFrameCountOffset);
    chunk.preRollFrames = LoadLittleEndian<uint32>(bytes + ChunkPreRollOffset);
    platform::MemoryCopy(chunk.sha256, bytes + ChunkHashOffset, AudioChunkHashBytes);
    return chunk;
}

// 머리 값만으로 정해지는 규칙을 본다.
Result<void> CheckHeader(const AudioPlatformDataHeader& header) noexcept {
    if (header.channels == 0 || header.channels > MaxOutputChannels ||
        (header.channelMask & ~AudioChannelMaskAll) != 0 ||
        AudioChannelMaskCount(header.channelMask) != header.channels) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    // rate 규칙은 아는 codec에만 둔다(표 범위 안의 다른 id는 rate > 0만 본다).
    bool rateValid = header.sampleRate > 0;
    if (header.codec == AudioCodecId::Opus) {
        rateValid = header.sampleRate == OpusPlatformSampleRate;
    } else if (header.codec == AudioCodecId::Pcm16 || header.codec == AudioCodecId::Adpcm) {
        rateValid = header.sampleRate >= MinPlatformSampleRate &&
                    header.sampleRate <= MaxPlatformSampleRate;
    }

    if (!rateValid || header.frames == 0 || header.chunkCount == 0 ||
        header.codecHeaderBytes % AudioCodecHeaderAlignment != 0) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    // delay + 유효 + padding이 넘치지 않아야 한다.
    if (header.frames > MaxUInt64 - header.encoderDelay - header.endPadding) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    const bool loopValid =
        header.loopEnd == 0 ? header.loopStart == 0
                            : header.loopStart < header.loopEnd && header.loopEnd <= header.frames;
    if (!loopValid) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    return Result<void>::Success();
}

} // namespace

uint32 DefaultAudioChannelMask(uint32 channels) noexcept {
    constexpr uint32 Front = AudioChannelFrontLeft | AudioChannelFrontRight;
    constexpr uint32 Side = AudioChannelSideLeft | AudioChannelSideRight;
    constexpr uint32 Back = AudioChannelBackLeft | AudioChannelBackRight;
    constexpr uint32 Masks[MaxOutputChannels + 1] = {
        0,
        AudioChannelFrontCenter,
        Front,
        Front | AudioChannelFrontCenter,
        Front | Side,
        Front | AudioChannelFrontCenter | Side,
        Front | AudioChannelFrontCenter | AudioChannelLowFrequency | Side,
        Front | AudioChannelFrontCenter | AudioChannelLowFrequency | Side | AudioChannelBackCenter,
        Front | AudioChannelFrontCenter | AudioChannelLowFrequency | Side | Back};
    return channels <= MaxOutputChannels ? Masks[channels] : 0;
}

uint32 AudioChannelMaskCount(uint32 mask) noexcept {
    uint32 count = 0;
    for (; mask != 0; mask &= mask - 1) {
        ++count;
    }

    return count;
}

void StoreAudioPlatformDataHeader(const AudioPlatformDataHeader& header, uint8* out) noexcept {
    platform::MemoryZero(out, AudioPlatformDataHeaderBytes);
    platform::MemoryCopy(out + HeaderMagicOffset, Magic, MagicBytes);
    StoreLittleEndian<uint16>(out + HeaderVersionOffset, AudioPlatformDataVersion);
    out[HeaderCodecOffset] = static_cast<uint8>(header.codec);
    out[HeaderChannelsOffset] = static_cast<uint8>(header.channels);
    StoreLittleEndian<uint32>(out + HeaderBitstreamOffset, header.bitstreamVersion);
    StoreLittleEndian<uint32>(out + HeaderRateOffset, header.sampleRate);
    StoreLittleEndian<uint64>(out + HeaderFramesOffset, header.frames);
    StoreLittleEndian<uint64>(out + HeaderLoopStartOffset, header.loopStart);
    StoreLittleEndian<uint64>(out + HeaderLoopEndOffset, header.loopEnd);
    StoreLittleEndian<uint32>(out + HeaderDelayOffset, header.encoderDelay);
    StoreLittleEndian<uint32>(out + HeaderPaddingOffset, header.endPadding);
    StoreLittleEndian<uint32>(out + HeaderChunkCountOffset, header.chunkCount);
    StoreLittleEndian<uint32>(out + HeaderCodecHeaderOffset, header.codecHeaderBytes);
    StoreLittleEndian<uint32>(out + HeaderChannelMaskOffset, header.channelMask);
    StoreLittleEndian<uint64>(out + HeaderPayloadBytesOffset, header.payloadBytes);
}

void StoreAudioChunkEntry(const AudioChunkEntry& chunk, uint8* out) noexcept {
    StoreLittleEndian<uint64>(out + ChunkOffsetOffset, chunk.payloadOffset);
    StoreLittleEndian<uint32>(out + ChunkBytesOffset, chunk.payloadBytes);
    StoreLittleEndian<uint32>(out + ChunkFlagsOffset, chunk.flags);
    StoreLittleEndian<uint64>(out + ChunkFirstFrameOffset, chunk.firstFrame);
    StoreLittleEndian<uint32>(out + ChunkFrameCountOffset, chunk.frameCount);
    StoreLittleEndian<uint32>(out + ChunkPreRollOffset, chunk.preRollFrames);
    platform::MemoryCopy(out + ChunkHashOffset, chunk.sha256, AudioChunkHashBytes);
}

usize AudioPlatformDataMetaBytes(uint32 codecHeaderBytes, uint32 chunkCount) noexcept {
    // 32비트 두 값이라 64비트 usize에서 넘치지 않는다.
    const uint64 raw = AudioPlatformDataHeaderBytes + uint64{codecHeaderBytes} +
                       uint64{chunkCount} * AudioChunkEntryBytes;
    if (raw > NumericLimits<usize>::Max() - AudioChunkAlignment) {
        return 0;
    }

    return AlignUp(static_cast<usize>(raw), AudioChunkAlignment);
}

Result<AudioPlatformDataView> AudioPlatformDataView::Parse(Span<const uint8> bytes) noexcept {
    using Failure = Result<AudioPlatformDataView>;
    if (bytes.Size() < AudioPlatformDataHeaderBytes) {
        return Failure::Failure(ErrorCode::CorruptData);
    }

    const uint8* data = bytes.Data();
    for (usize index = 0; index < MagicBytes; ++index) {
        if (data[HeaderMagicOffset + index] != Magic[index]) {
            return Failure::Failure(ErrorCode::CorruptData);
        }
    }

    // 판이 다르면 모르는 변형이다(새 쿠커의 데이터 — 손상이 아니다, 3.38절). 다른 규칙보다 먼저
    // 본다.
    if (LoadLittleEndian<uint16>(data + HeaderVersionOffset) != AudioPlatformDataVersion) {
        return Failure::Failure(ErrorCode::Unsupported);
    }

    if (LoadLittleEndian<uint32>(data + HeaderReserved32Offset) != 0 ||
        LoadLittleEndian<uint64>(data + HeaderReserved64Offset) != 0) {
        return Failure::Failure(ErrorCode::CorruptData);
    }

    // codec id 0은 형식 위반, 표 범위 밖은 이 판이 모르는 codec이다.
    const uint8 codec = data[HeaderCodecOffset];
    if (!CodecInTableRange(codec)) {
        return Failure::Failure(codec == static_cast<uint8>(AudioCodecId::None)
                                    ? ErrorCode::CorruptData
                                    : ErrorCode::Unsupported);
    }

    AudioPlatformDataView view;
    AudioPlatformDataHeader& header = view.header_;
    header.codec = static_cast<AudioCodecId>(codec);
    header.channels = data[HeaderChannelsOffset];
    header.bitstreamVersion = LoadLittleEndian<uint32>(data + HeaderBitstreamOffset);
    header.sampleRate = LoadLittleEndian<uint32>(data + HeaderRateOffset);
    header.frames = LoadLittleEndian<uint64>(data + HeaderFramesOffset);
    header.loopStart = LoadLittleEndian<uint64>(data + HeaderLoopStartOffset);
    header.loopEnd = LoadLittleEndian<uint64>(data + HeaderLoopEndOffset);
    header.encoderDelay = LoadLittleEndian<uint32>(data + HeaderDelayOffset);
    header.endPadding = LoadLittleEndian<uint32>(data + HeaderPaddingOffset);
    header.chunkCount = LoadLittleEndian<uint32>(data + HeaderChunkCountOffset);
    header.codecHeaderBytes = LoadLittleEndian<uint32>(data + HeaderCodecHeaderOffset);
    header.channelMask = LoadLittleEndian<uint32>(data + HeaderChannelMaskOffset);
    header.payloadBytes = LoadLittleEndian<uint64>(data + HeaderPayloadBytesOffset);
    if (auto checked = CheckHeader(header); !checked) {
        return Failure::Failure(checked.Error());
    }

    // 메타 크기와 정렬 채움(0)을 본다.
    const usize metaBytes = AudioPlatformDataMetaBytes(header.codecHeaderBytes, header.chunkCount);
    if (metaBytes == 0 || bytes.Size() < metaBytes) {
        return Failure::Failure(ErrorCode::CorruptData);
    }

    const usize tableStart = AudioPlatformDataHeaderBytes + header.codecHeaderBytes;
    const usize tableEnd =
        tableStart + static_cast<usize>(header.chunkCount) * AudioChunkEntryBytes;
    for (usize index = tableEnd; index < metaBytes; ++index) {
        if (data[index] != 0) {
            return Failure::Failure(ErrorCode::CorruptData);
        }
    }

    const uint64 present = bytes.Size() - metaBytes;
    if (present > header.payloadBytes) {
        return Failure::Failure(ErrorCode::CorruptData);
    }

    // 청크: 첫 codec frame이 0부터 이어지고, 위치는 0에서 시작해 16 정렬로 증가하며 payload 영역
    // 안이다.
    uint64 nextFrame = 0;
    uint64 nextOffset = 0;
    for (uint32 index = 0; index < header.chunkCount; ++index) {
        const AudioChunkEntry chunk =
            LoadChunk(data + tableStart + static_cast<usize>(index) * AudioChunkEntryBytes);
        const bool offsetValid =
            index == 0 ? chunk.payloadOffset == 0 : chunk.payloadOffset >= nextOffset;
        if (chunk.flags != AudioChunkFlagIndependent || chunk.frameCount == 0 ||
            chunk.payloadBytes == 0 || chunk.firstFrame != nextFrame ||
            chunk.preRollFrames > chunk.firstFrame || !offsetValid ||
            chunk.payloadOffset % AudioChunkAlignment != 0 ||
            chunk.payloadOffset > header.payloadBytes ||
            chunk.payloadBytes > header.payloadBytes - chunk.payloadOffset ||
            chunk.frameCount > MaxUInt64 - chunk.firstFrame) {
            return Failure::Failure(ErrorCode::CorruptData);
        }

        nextFrame = chunk.firstFrame + chunk.frameCount;
        nextOffset = chunk.payloadOffset + chunk.payloadBytes;
    }

    if (nextFrame != header.CodecFrames()) {
        return Failure::Failure(ErrorCode::CorruptData);
    }

    view.bytes_ = bytes;
    view.metaBytes_ = metaBytes;
    view.presentPayloadBytes_ = present;
    return Result<AudioPlatformDataView>::Success(view);
}

Span<const uint8> AudioPlatformDataView::CodecHeader() const noexcept {
    if (bytes_.Size() == 0) {
        return Span<const uint8>();
    }

    return Span<const uint8>(bytes_.Data() + AudioPlatformDataHeaderBytes,
                             header_.codecHeaderBytes);
}

AudioChunkEntry AudioPlatformDataView::Chunk(uint32 index) const noexcept {
    ROCKS_ASSERT(index < header_.chunkCount);
    const usize tableStart = AudioPlatformDataHeaderBytes + header_.codecHeaderBytes;
    return LoadChunk(bytes_.Data() + tableStart + static_cast<usize>(index) * AudioChunkEntryBytes);
}

Span<const uint8> AudioPlatformDataView::ChunkPayload(uint32 index) const noexcept {
    const AudioChunkEntry chunk = Chunk(index);
    if (chunk.payloadOffset + chunk.payloadBytes > presentPayloadBytes_) {
        return Span<const uint8>();
    }

    return Span<const uint8>(bytes_.Data() + metaBytes_ + static_cast<usize>(chunk.payloadOffset),
                             chunk.payloadBytes);
}

Result<void> VerifyAllAudioChunks(const AudioPlatformDataView& view) noexcept {
    if (!view.HasAllPayload()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (uint32 index = 0; index < view.Header().chunkCount; ++index) {
        if (auto verified = VerifyAudioChunk(view.Chunk(index), view.ChunkPayload(index));
            !verified) {
            return verified;
        }
    }

    return Result<void>::Success();
}

Result<void> VerifyAudioChunk(const AudioChunkEntry& chunk, Span<const uint8> payload) noexcept {
    if (payload.Size() != chunk.payloadBytes) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    const Sha256Digest digest = Sha256::Hash(payload);
    for (usize index = 0; index < AudioChunkHashBytes; ++index) {
        if (digest.bytes[index] != chunk.sha256[index]) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::audio
