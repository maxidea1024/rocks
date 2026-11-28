#include <AudioTools/AudioPlatformDataWriter.h>

#include <Base/NumericLimits.h>
#include <Core/Hash/Sha256.h>
#include <Platform/Memory.h>

namespace rocks::audio {

namespace {

// 청크 하나가 차지하는 단위 범위다.
// - [preRollUnit, firstUnit): 복제 pre-roll
// - [firstUnit, endUnit): 자기 단위
struct ChunkPlan {
    usize preRollUnit = 0;
    usize firstUnit = 0;
    usize endUnit = 0;
    uint64 firstFrame = 0;
    uint64 frames = 0;
    uint64 preRollFrames = 0;
    uint64 payloadBytes = 0;
};

constexpr uint64 MaxUInt32 = NumericLimits<uint32>::Max();

usize AlignChunk(usize value) noexcept {
    return (value + AudioChunkAlignment - 1) / AudioChunkAlignment * AudioChunkAlignment;
}

// 인코더 출력이 형식 규칙에 맞는지 본다.
Result<void> CheckEncoded(const EncodedAudio& encoded,
                          const AudioPlatformDataOptions& options) noexcept {
    // rate 규칙과 codec별 불변식은 아는 codec에만 둔다(3.30.1절). PCM16·ADPCM은 pre-roll·codec
    // 머리가 없다.
    bool rateValid = encoded.sampleRate > 0;
    bool codecValid = true;
    if (encoded.codec == AudioCodecId::Opus) {
        rateValid = encoded.sampleRate == OpusPlatformSampleRate;
    } else if (encoded.codec == AudioCodecId::Pcm16 || encoded.codec == AudioCodecId::Adpcm) {
        rateValid = encoded.sampleRate >= MinPlatformSampleRate &&
                    encoded.sampleRate <= MaxPlatformSampleRate;
        codecValid = encoded.preRollFrames == 0 && encoded.codecHeader.Size() == 0;
    }

    if (encoded.codec == AudioCodecId::None ||
        static_cast<uint32>(encoded.codec) >= MaxAudioCodecs || encoded.channels == 0 ||
        encoded.channels > MaxOutputChannels || (encoded.channelMask & ~AudioChannelMaskAll) != 0 ||
        AudioChannelMaskCount(encoded.channelMask) != encoded.channels || !rateValid ||
        !codecValid || encoded.frames == 0 || encoded.units.Size() == 0 ||
        encoded.codecHeader.Size() % AudioCodecHeaderAlignment != 0 ||
        encoded.codecHeader.Size() > MaxUInt32 || options.targetChunkBytes == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const bool loopValid = options.loopEnd == 0 ? options.loopStart == 0
                                                : options.loopStart < options.loopEnd &&
                                                      options.loopEnd <= encoded.frames;
    if (!loopValid ||
        encoded.frames > NumericLimits<uint64>::Max() - encoded.encoderDelay - encoded.endPadding) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 단위: frame > 0, byte > 0, payload 안. frame 합 = delay + 유효 + padding.
    uint64 frames = 0;
    for (usize index = 0; index < encoded.units.Size(); ++index) {
        const EncodedAudioUnit& unit = encoded.units[index];
        if (unit.frames == 0 || unit.payloadBytes == 0 ||
            unit.payloadOffset > encoded.payload.Size() ||
            unit.payloadBytes > encoded.payload.Size() - unit.payloadOffset) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        frames += unit.frames;
    }

    if (frames != encoded.encoderDelay + encoded.frames + encoded.endPadding) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

// 단위를 청크로 나눈다(3.30.3절).
Result<void> PlanChunks(const EncodedAudio& encoded, const AudioPlatformDataOptions& options,
                        Array<ChunkPlan>& plans) noexcept {
    const uint64 firstTarget =
        options.firstChunkBytes != 0 ? options.firstChunkBytes : options.targetChunkBytes;
    usize unit = 0;
    uint64 codecFrame = 0;
    while (unit < encoded.units.Size()) {
        ChunkPlan plan;
        plan.firstUnit = unit;
        plan.firstFrame = codecFrame;
        const uint64 target = plans.Size() == 0 ? firstTarget : options.targetChunkBytes;
        uint64 ownBytes = 0;
        while (unit < encoded.units.Size() &&
               (ownBytes == 0 || ownBytes + encoded.units[unit].payloadBytes <= target)) {
            ownBytes += encoded.units[unit].payloadBytes;
            plan.frames += encoded.units[unit].frames;
            ++unit;
        }

        plan.endUnit = unit;
        // 바로 앞 단위들을 거꾸로 모아 pre-roll을 채운다(codec frame 0 앞으로는 넘지 않는다).
        plan.preRollUnit = plan.firstUnit;
        uint64 preRollBytes = 0;
        while (plan.preRollUnit > 0 && plan.preRollFrames < encoded.preRollFrames) {
            --plan.preRollUnit;
            plan.preRollFrames += encoded.units[plan.preRollUnit].frames;
            preRollBytes += encoded.units[plan.preRollUnit].payloadBytes;
        }

        plan.payloadBytes = preRollBytes + ownBytes;
        if (plan.frames > MaxUInt32 || plan.preRollFrames > MaxUInt32 ||
            plan.payloadBytes > MaxUInt32) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        if (auto added = plans.TryPushBack(plan); !added) {
            return added;
        }

        codecFrame += plan.frames;
    }

    if (plans.Size() > MaxUInt32) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    return Result<void>::Success();
}

Result<void> WriteInto(const EncodedAudio& encoded, const AudioPlatformDataOptions& options,
                       Array<uint8>& out) noexcept {
    if (auto checked = CheckEncoded(encoded, options); !checked) {
        return checked;
    }

    Array<ChunkPlan> plans(out.GetAllocator());
    if (auto planned = PlanChunks(encoded, options, plans); !planned) {
        return planned;
    }

    const uint32 chunkCount = static_cast<uint32>(plans.Size());
    const uint32 codecHeaderBytes = static_cast<uint32>(encoded.codecHeader.Size());
    const usize metaBytes = AudioPlatformDataMetaBytes(codecHeaderBytes, chunkCount);
    // payload 영역: 청크마다 16 정렬로 이어 붙인다.
    uint64 payloadBytes = 0;
    for (usize index = 0; index < plans.Size(); ++index) {
        payloadBytes = AlignChunk(static_cast<usize>(payloadBytes)) + plans[index].payloadBytes;
    }

    if (metaBytes == 0 || payloadBytes > NumericLimits<usize>::Max() - metaBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    auto storage = out.TryAppendUninitialized(metaBytes + static_cast<usize>(payloadBytes));
    if (!storage) {
        return Result<void>::Failure(storage.Error());
    }

    uint8* bytes = storage.Value().Data();
    platform::MemoryZero(bytes, storage.Value().Size());
    AudioPlatformDataHeader header;
    header.codec = encoded.codec;
    header.channels = encoded.channels;
    header.bitstreamVersion = encoded.bitstreamVersion;
    header.sampleRate = encoded.sampleRate;
    header.frames = encoded.frames;
    header.loopStart = options.loopStart;
    header.loopEnd = options.loopEnd;
    header.encoderDelay = encoded.encoderDelay;
    header.endPadding = encoded.endPadding;
    header.chunkCount = chunkCount;
    header.codecHeaderBytes = codecHeaderBytes;
    header.channelMask = encoded.channelMask;
    header.payloadBytes = payloadBytes;
    StoreAudioPlatformDataHeader(header, bytes);
    if (codecHeaderBytes != 0) {
        platform::MemoryCopy(bytes + AudioPlatformDataHeaderBytes, encoded.codecHeader.Data(),
                             codecHeaderBytes);
    }

    uint8* table = bytes + AudioPlatformDataHeaderBytes + codecHeaderBytes;
    uint8* payload = bytes + metaBytes;
    usize offset = 0;
    for (usize index = 0; index < plans.Size(); ++index) {
        const ChunkPlan& plan = plans[index];
        offset = AlignChunk(offset);
        // 청크 payload = 복제 pre-roll 단위 + 자기 단위.
        usize written = 0;
        for (usize unitIndex = plan.preRollUnit; unitIndex < plan.endUnit; ++unitIndex) {
            const EncodedAudioUnit& unit = encoded.units[unitIndex];
            platform::MemoryCopy(payload + offset + written,
                                 encoded.payload.Data() + unit.payloadOffset, unit.payloadBytes);
            written += unit.payloadBytes;
        }

        AudioChunkEntry chunk;
        chunk.payloadOffset = offset;
        chunk.payloadBytes = static_cast<uint32>(plan.payloadBytes);
        chunk.flags = AudioChunkFlagIndependent;
        chunk.firstFrame = plan.firstFrame;
        chunk.frameCount = static_cast<uint32>(plan.frames);
        chunk.preRollFrames = static_cast<uint32>(plan.preRollFrames);
        const Sha256Digest digest = Sha256::Hash(Span<const uint8>(payload + offset, written));
        platform::MemoryCopy(chunk.sha256, digest.bytes, AudioChunkHashBytes);
        StoreAudioChunkEntry(chunk, table + index * AudioChunkEntryBytes);
        offset += written;
    }

    return Result<void>::Success();
}

} // namespace

Result<void> WriteAudioPlatformData(const EncodedAudio& encoded,
                                    const AudioPlatformDataOptions& options,
                                    Array<uint8>& out) noexcept {
    out.Clear();
    Result<void> written = WriteInto(encoded, options, out);
    if (!written) {
        out.Clear();
    }

    return written;
}

} // namespace rocks::audio
