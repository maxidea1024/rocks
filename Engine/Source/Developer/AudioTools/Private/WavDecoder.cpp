#include "SourceAudioInternal.h"

namespace rocks::audio {

using namespace sourcedetail;

namespace {

// RIFF 머리(id·크기·형식) 크기다.
constexpr uint32 RiffHeaderBytes = 12;

// fmt chunk의 최소·확장 크기와 필드 위치다.
constexpr uint32 FmtMinimumBytes = 16;
constexpr uint32 FmtExtensibleBytes = 40;
constexpr uint32 FmtChannelsOffset = 2;
constexpr uint32 FmtRateOffset = 4;
constexpr uint32 FmtBlockAlignOffset = 12;
constexpr uint32 FmtBitsOffset = 14;
constexpr uint32 FmtValidBitsOffset = 18;
constexpr uint32 FmtSubFormatOffset = 24;

// 부호화 tag다(mmreg.h).
constexpr uint16 FormatPcm = 0x0001;
constexpr uint16 FormatIeeeFloat = 0x0003;
constexpr uint16 FormatExtensible = 0xFFFE;

// KSDATAFORMAT_SUBTYPE의 tag 뒤 14 byte(00000000-0010-8000-00AA00389B71, GUID 메모리 배치)다.
constexpr uint8 SubFormatTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                     0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

// smpl chunk: 머리 36 byte, loop 수 위치, loop 하나 24 byte(start·end 위치).
constexpr uint32 SmplHeaderBytes = 36;
constexpr uint32 SmplLoopCountOffset = 28;
constexpr uint32 SmplLoopBytes = 24;
constexpr uint32 SmplLoopStartOffset = 8;
constexpr uint32 SmplLoopEndOffset = 12;

// cue chunk: 개수 4 byte, cue 하나 24 byte(id·sampleOffset 위치).
constexpr uint32 CueCountBytes = 4;
constexpr uint32 CuePointBytes = 24;
constexpr uint32 CueSampleOffset = 20;

// LIST chunk 형식 id 크기와 labl 머리(id 4 byte)다.
constexpr uint32 ListTypeBytes = 4;
constexpr uint32 LabelIdBytes = 4;

struct ChunkRange {
    usize offset = 0;
    usize size = 0;
    bool present = false;
};

} // namespace

Result<void> DecodeWav(Span<const uint8> file, DecodedSourceAudio& out) noexcept {
    out.Reset();
    const uint8* bytes = file.Data();
    if (file.Size() < RiffHeaderBytes || !IsTag(bytes + 8, "WAVE") ||
        !(IsTag(bytes, "RIFF") || IsTag(bytes, "RIFX"))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const bool bigEndian = IsTag(bytes, "RIFX");
    const uint64 riffEnd = static_cast<uint64>(ReadU32(bytes + 4, bigEndian)) + ChunkHeaderBytes;
    if (riffEnd > file.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ChunkRange format;
    ChunkRange data;
    ChunkRange sampler;
    ChunkRange cue;
    ChunkRange labels;
    usize offset = RiffHeaderBytes;
    while (offset + ChunkHeaderBytes <= riffEnd) {
        const uint8* header = bytes + offset;
        const usize size = ReadU32(header + 4, bigEndian);
        const usize body = offset + ChunkHeaderBytes;
        if (size > riffEnd - body) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        ChunkRange* target = nullptr;
        if (IsTag(header, "fmt ")) {
            target = &format;
        } else if (IsTag(header, "data")) {
            target = &data;
        } else if (IsTag(header, "smpl")) {
            target = &sampler;
        } else if (IsTag(header, "cue ")) {
            target = &cue;
        } else if (IsTag(header, "LIST") && size >= ListTypeBytes && IsTag(bytes + body, "adtl")) {
            target = &labels;
        }

        // 모르는 chunk와 두 번째 이후의 같은 chunk는 건너뛰고 센다.
        if (target == nullptr || target->present) {
            out.CountIgnoredChunk();
        } else {
            target->offset = body;
            target->size = size;
            target->present = true;
        }

        offset = body + PaddedSize(size);
    }

    if (!format.present || !data.present || format.size < FmtMinimumBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const uint8* fmt = bytes + format.offset;
    uint16 tag = ReadU16(fmt, bigEndian);
    const uint32 channels = ReadU16(fmt + FmtChannelsOffset, bigEndian);
    const uint32 sampleRate = ReadU32(fmt + FmtRateOffset, bigEndian);
    const uint32 blockAlign = ReadU16(fmt + FmtBlockAlignOffset, bigEndian);
    const uint32 bits = ReadU16(fmt + FmtBitsOffset, bigEndian);
    if (tag == FormatExtensible) {
        if (format.size < FmtExtensibleBytes) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const uint32 validBits = ReadU16(fmt + FmtValidBitsOffset, bigEndian);
        const uint8* subFormat = fmt + FmtSubFormatOffset;
        for (uint32 index = 0; index < sizeof(SubFormatTail); ++index) {
            if (subFormat[2 + index] != SubFormatTail[index]) {
                return Result<void>::Failure(ErrorCode::Unsupported);
            }
        }

        if (validBits > bits) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // GUID의 첫 두 byte는 LE 부호화 tag다(RIFX도 GUID는 메모리 배치 그대로).
        tag = static_cast<uint16>(subFormat[0] | (subFormat[1] << BitsPerByte));
    }

    SampleEncoding encoding = SampleEncoding::SignedInt;
    if (tag == FormatPcm) {
        if (bits != 8 && bits != 16 && bits != 24 && bits != 32) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        encoding = bits == 8 ? SampleEncoding::UnsignedInt : SampleEncoding::SignedInt;
    } else if (tag == FormatIeeeFloat) {
        if (bits != 32 && bits != 64) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        encoding = SampleEncoding::Float;
    } else {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (channels == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (channels > MaxSourceChannels || sampleRate < MinSourceSampleRate ||
        sampleRate > MaxSourceSampleRate) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    const uint32 bytesPerSample = bits / BitsPerByte;
    if (blockAlign != channels * bytesPerSample) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // data 크기가 blockAlign의 배수가 아니면 남는 byte를 버린다(UE와 같다).
    const uint64 frames = data.size / blockAlign;
    if (frames == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto pcm = out.TryAllocatePcm(channels, sampleRate, frames, FormatOf(bytesPerSample, encoding),
                                  SourceCodec::Pcm);
    if (!pcm) {
        return Result<void>::Failure(pcm.Error());
    }

    DeinterleaveToFloat(bytes + data.offset, channels, frames, bytesPerSample, encoding, bigEndian,
                        pcm.Value());

    // smpl loop: 파일의 end는 포함 위치라 +1로 바꾼다. frame 범위 밖 loop는 버린다.
    if (sampler.present && sampler.size >= SmplHeaderBytes) {
        const uint8* smpl = bytes + sampler.offset;
        const uint32 count = ReadU32(smpl + SmplLoopCountOffset, bigEndian);
        const usize available = (sampler.size - SmplHeaderBytes) / SmplLoopBytes;
        for (uint32 index = 0; index < count && index < available; ++index) {
            const uint8* loop = smpl + SmplHeaderBytes + static_cast<usize>(index) * SmplLoopBytes;
            const uint64 start = ReadU32(loop + SmplLoopStartOffset, bigEndian);
            const uint64 end =
                static_cast<uint64>(ReadU32(loop + SmplLoopEndOffset, bigEndian)) + 1;
            if (start < end && end <= frames) {
                if (auto added = out.TryAddLoop(SourceLoop{start, end}); !added) {
                    return added;
                }
            }
        }
    }

    // cue 위치(sampleOffset)와 LIST adtl labl 이름.
    if (cue.present && cue.size >= CueCountBytes) {
        const uint8* cues = bytes + cue.offset;
        const uint32 count = ReadU32(cues, bigEndian);
        const usize available = (cue.size - CueCountBytes) / CuePointBytes;
        for (uint32 index = 0; index < count && index < available; ++index) {
            const uint8* point = cues + CueCountBytes + static_cast<usize>(index) * CuePointBytes;
            const uint32 id = ReadU32(point, bigEndian);
            const uint64 frame = ReadU32(point + CueSampleOffset, bigEndian);
            if (frame <= frames) {
                if (auto added = out.TryAddCue(id, frame); !added) {
                    return added;
                }
            }
        }
    }

    if (labels.present) {
        usize position = labels.offset + ListTypeBytes;
        const usize end = labels.offset + labels.size;
        while (position + ChunkHeaderBytes <= end) {
            const uint8* sub = bytes + position;
            const usize size = ReadU32(sub + 4, bigEndian);
            const usize body = position + ChunkHeaderBytes;
            if (size > end - body) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            if (IsTag(sub, "labl") && size >= LabelIdBytes) {
                const uint32 id = ReadU32(bytes + body, bigEndian);
                const char* text = reinterpret_cast<const char*>(bytes + body + LabelIdBytes);
                usize length = size - LabelIdBytes;
                // 끝의 NUL(과 그 뒤)은 뗀다.
                for (usize index = 0; index < length; ++index) {
                    if (text[index] == '\0') {
                        length = index;
                        break;
                    }
                }

                if (auto named = out.TryNameCue(id, StringView(text, length)); !named) {
                    return named;
                }
            }

            position = body + PaddedSize(size);
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::audio
