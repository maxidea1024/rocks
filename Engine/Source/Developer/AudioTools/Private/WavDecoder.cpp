#include "SourceAudioInternal.h"

namespace rocks::audio {

using namespace sourcedetail;

namespace {

// RIFF 머리: id(4)·크기(4)·형식 id(4)다.
constexpr uint32 RiffSizeOffset = 4;
constexpr uint32 RiffFormOffset = 8;
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

// KSDATAFORMAT_SUBTYPE GUID {0000tttt-0000-0010-8000-00AA00389B71}: Data1(u32)의 하위 16비트가 tag,
// Data2(u16) = 0, Data3(u16) = 0x0010, Data4는 아래 8 byte다. Data1·2·3은 파일 끝 방향으로 읽는다
// (RIFX는 BE — libsndfile wavlike_write_guid가 그렇게 쓴다).
constexpr uint32 GuidData2Offset = 4;
constexpr uint32 GuidData3Offset = 6;
constexpr uint32 GuidData4Offset = 8;
constexpr uint32 GuidData4Bytes = 8;
constexpr uint16 GuidData3 = 0x0010;
constexpr uint8 GuidData4[GuidData4Bytes] = {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
constexpr uint32 GuidTagMask = 0xFFFF;

// smpl chunk: 머리 36 byte, loop 수 위치, loop 하나 24 byte(start·end 위치).
constexpr uint32 SmplHeaderBytes = 36;
constexpr uint32 SmplLoopCountOffset = 28;
constexpr uint32 SmplLoopBytes = 24;
constexpr uint32 SmplLoopStartOffset = 8;
constexpr uint32 SmplLoopEndOffset = 12;

// cue chunk: 개수 4 byte, cue 하나 24 byte. 위치는 dwPosition(+4)을 쓴다(UE와 같다).
constexpr uint32 CueCountBytes = 4;
constexpr uint32 CuePointBytes = 24;
constexpr uint32 CuePositionOffset = 4;

// LIST chunk 형식 id 크기와 labl 머리(id 4 byte)다.
constexpr uint32 ListTypeBytes = 4;
constexpr uint32 LabelIdBytes = 4;

// 받는 표본 bit 수다.
constexpr uint32 Bits8 = 8;
constexpr uint32 Bits16 = 16;
constexpr uint32 Bits24 = 24;
constexpr uint32 Bits32 = 32;
constexpr uint32 Bits64 = 64;

struct ChunkRange {
    usize offset = 0;
    usize size = 0;
    bool present = false;
};

// smpl loop를 읽는다. 개수가 본문보다 크면 본문에 있는 것까지만 읽는다.
Result<void> ReadSampler(const uint8* bytes, const ChunkRange& sampler, bool bigEndian,
                         uint64 frames, DecodedSourceAudio& out) noexcept {
    if (!sampler.present || sampler.size < SmplHeaderBytes) {
        return Result<void>::Success();
    }

    const uint8* smpl = bytes + sampler.offset;
    const uint32 count = ReadU32(smpl + SmplLoopCountOffset, bigEndian);
    const usize available = (sampler.size - SmplHeaderBytes) / SmplLoopBytes;
    for (uint32 index = 0; index < count && index < available; ++index) {
        const uint8* loop = smpl + SmplHeaderBytes + static_cast<usize>(index) * SmplLoopBytes;
        // 파일의 end는 포함 위치라 +1로 바꾼다. frame 범위 밖 loop는 버린다. 반복 방식(dwType)은
        // 보지 않는다.
        const uint64 start = ReadU32(loop + SmplLoopStartOffset, bigEndian);
        const uint64 end = static_cast<uint64>(ReadU32(loop + SmplLoopEndOffset, bigEndian)) + 1;
        if (start < end && end <= frames) {
            if (auto added = out.TryAddLoop(SourceLoop{start, end}); !added) {
                return added;
            }
        }
    }

    return Result<void>::Success();
}

// cue 위치를 읽는다. 개수가 본문보다 크면 본문에 있는 것까지만 읽는다.
Result<void> ReadCues(const uint8* bytes, const ChunkRange& cue, bool bigEndian, uint64 frames,
                      DecodedSourceAudio& out) noexcept {
    if (!cue.present || cue.size < CueCountBytes) {
        return Result<void>::Success();
    }

    const uint8* cues = bytes + cue.offset;
    const uint32 count = ReadU32(cues, bigEndian);
    const usize available = (cue.size - CueCountBytes) / CuePointBytes;
    for (uint32 index = 0; index < count && index < available; ++index) {
        const uint8* point = cues + CueCountBytes + static_cast<usize>(index) * CuePointBytes;
        const uint32 id = ReadU32(point, bigEndian);
        const uint64 frame = ReadU32(point + CuePositionOffset, bigEndian);
        if (frame <= frames) {
            if (auto added = out.TryAddCue(id, frame); !added) {
                return added;
            }
        }
    }

    return Result<void>::Success();
}

// LIST adtl의 labl로 cue 이름을 붙인다. 하위 chunk가 LIST 끝을 넘으면 거기서 멈추고 센다 —
// 메타데이터 오류는 PCM을 막지 않는다.
Result<void> ReadLabels(const uint8* bytes, const ChunkRange& labels, bool bigEndian,
                        DecodedSourceAudio& out) noexcept {
    if (!labels.present) {
        return Result<void>::Success();
    }

    usize position = labels.offset + ListTypeBytes;
    const usize end = labels.offset + labels.size;
    while (position + ChunkHeaderBytes <= end) {
        const uint8* sub = bytes + position;
        const usize size = ReadU32(sub + ChunkSizeOffset, bigEndian);
        const usize body = position + ChunkHeaderBytes;
        if (size > end - body) {
            out.CountIgnoredChunk();
            break;
        }

        if (IsTag(sub, "labl") && size >= LabelIdBytes) {
            const uint32 id = ReadU32(bytes + body, bigEndian);
            const char* text = reinterpret_cast<const char*>(bytes + body + LabelIdBytes);
            usize length = size - LabelIdBytes;
            // 첫 NUL(과 그 뒤)은 뗀다. NUL이 없으면 본문 전체가 이름이다.
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

    return Result<void>::Success();
}

Result<void> DecodeWavInto(Span<const uint8> file, DecodedSourceAudio& out) noexcept {
    const uint8* bytes = file.Data();
    if (file.Size() < RiffHeaderBytes || !IsTag(bytes + RiffFormOffset, "WAVE") ||
        !(IsTag(bytes, "RIFF") || IsTag(bytes, "RIFX"))) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    const bool bigEndian = IsTag(bytes, "RIFX");
    // RIFF 크기는 파일 끝으로 자른다(UE와 같다 — 스트리밍 기록기가 남긴 0xFFFFFFFF·끝 pad 누락).
    const uint64 declaredEnd =
        static_cast<uint64>(ReadU32(bytes + RiffSizeOffset, bigEndian)) + ChunkHeaderBytes;
    const usize riffEnd = declaredEnd < file.Size() ? static_cast<usize>(declaredEnd) : file.Size();

    ChunkRange format;
    ChunkRange data;
    ChunkRange sampler;
    ChunkRange cue;
    ChunkRange labels;
    usize offset = RiffHeaderBytes;
    while (offset + ChunkHeaderBytes <= riffEnd) {
        const uint8* header = bytes + offset;
        usize size = ReadU32(header + ChunkSizeOffset, bigEndian);
        const usize body = offset + ChunkHeaderBytes;
        const bool isData = IsTag(header, "data");
        if (size > riffEnd - body) {
            // 끝을 넘는 data는 끝까지로 잘라 받는다(UE와 같다). 다른 chunk가 넘치면 거기서 멈추고
            // 센다 — 그 뒤 fmt·data가 없으면 아래에서 CorruptData다.
            if (!isData) {
                out.CountIgnoredChunk();
                break;
            }

            size = riffEnd - body;
        }

        ChunkRange* target = nullptr;
        if (IsTag(header, "fmt ")) {
            target = &format;
        } else if (isData) {
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
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    const uint8* fmt = bytes + format.offset;
    uint32 tag = ReadU16(fmt, bigEndian);
    const uint32 channels = ReadU16(fmt + FmtChannelsOffset, bigEndian);
    const uint32 sampleRate = ReadU32(fmt + FmtRateOffset, bigEndian);
    const uint32 blockAlign = ReadU16(fmt + FmtBlockAlignOffset, bigEndian);
    const uint32 bits = ReadU16(fmt + FmtBitsOffset, bigEndian);
    if (tag == FormatExtensible) {
        if (format.size < FmtExtensibleBytes) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        const uint32 validBits = ReadU16(fmt + FmtValidBitsOffset, bigEndian);
        const uint8* guid = fmt + FmtSubFormatOffset;
        const uint32 data1 = ReadU32(guid, bigEndian);
        bool known = (data1 & ~GuidTagMask) == 0 &&
                     ReadU16(guid + GuidData2Offset, bigEndian) == 0 &&
                     ReadU16(guid + GuidData3Offset, bigEndian) == GuidData3;
        for (uint32 index = 0; index < GuidData4Bytes && known; ++index) {
            known = guid[GuidData4Offset + index] == GuidData4[index];
        }

        if (!known) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        if (validBits > bits) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        tag = data1 & GuidTagMask;
    }

    SampleEncoding encoding = SampleEncoding::SignedInt;
    if (tag == FormatPcm) {
        if (bits != Bits8 && bits != Bits16 && bits != Bits24 && bits != Bits32) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        encoding = bits == Bits8 ? SampleEncoding::UnsignedInt : SampleEncoding::SignedInt;
    } else if (tag == FormatIeeeFloat) {
        if (bits != Bits32 && bits != Bits64) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        encoding = SampleEncoding::Float;
    } else {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (channels == 0) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    if (channels > MaxSourceChannels || sampleRate < MinSourceSampleRate ||
        sampleRate > MaxSourceSampleRate) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    const uint32 bytesPerSample = bits / BitsPerByte;
    if (blockAlign != channels * bytesPerSample) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    // data 크기가 blockAlign의 배수가 아니면 남는 byte를 버린다(UE와 같다).
    const uint64 frames = data.size / blockAlign;
    if (frames == 0) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    auto pcm = out.TryAllocatePcm(channels, sampleRate, frames, FormatOf(bytesPerSample, encoding),
                                  SourceCodec::Pcm);
    if (!pcm) {
        return Result<void>::Failure(pcm.Error());
    }

    if (!DeinterleaveToFloat(bytes + data.offset, channels, frames, bytesPerSample, encoding,
                             bigEndian, pcm.Value())) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    if (auto loops = ReadSampler(bytes, sampler, bigEndian, frames, out); !loops) {
        return loops;
    }

    if (auto cues = ReadCues(bytes, cue, bigEndian, frames, out); !cues) {
        return cues;
    }

    return ReadLabels(bytes, labels, bigEndian, out);
}

} // namespace

Result<void> DecodeWav(Span<const uint8> file, DecodedSourceAudio& out) noexcept {
    out.Reset();
    Result<void> decoded = DecodeWavInto(file, out);
    // 실패하면 부분 결과를 남기지 않는다.
    if (!decoded) {
        out.Reset();
    }

    return decoded;
}

} // namespace rocks::audio
