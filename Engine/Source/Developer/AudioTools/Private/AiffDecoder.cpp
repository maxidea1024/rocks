#include "SourceAudioInternal.h"

namespace rocks::audio {

using namespace sourcedetail;

namespace {

// FORM chunk 머리: id(4)·크기(4)·형식 id(4)다.
constexpr uint32 FormSizeOffset = 4;
constexpr uint32 FormTypeOffset = 8;
constexpr uint32 FormHeaderBytes = 12;

// COMM chunk: AIFF 18 byte(채널 2, frame 4, bits 2, rate 10), AIFC는 압축 type 4 byte가 더 있다.
constexpr uint32 CommBytes = 18;
constexpr uint32 CommCompressionBytes = 22;
constexpr uint32 CommFramesOffset = 2;
constexpr uint32 CommBitsOffset = 6;
constexpr uint32 CommRateOffset = 8;
constexpr uint32 CommCompressionOffset = 18;

// SSND chunk 머리(offset 4 + blockSize 4)다.
constexpr uint32 SsndHeaderBytes = 8;

// 80비트 IEEE extended: 지수 bias와 가수 bit 수(정수 bit 포함), 부호 bit다.
constexpr int32 ExtendedExponentBias = 16383;
constexpr int32 ExtendedMantissaBits = 63;
constexpr uint16 ExtendedSignBit = 0x8000;
constexpr uint16 ExtendedExponentMask = 0x7FFF;
constexpr uint32 ExtendedMantissaOffset = 2;
constexpr uint32 MantissaBytes = 8;
constexpr uint32 BitsPerU64 = 64;

// 받는 정수 bit 수 범위와 float bit 수다. 정수는 1–32 bit를 왼쪽 맞춤 ceil(bits/8) byte로 둔다(AIFF
// 규격).
constexpr uint32 MinIntegerBits = 1;
constexpr uint32 MaxIntegerBits = 32;
constexpr uint32 FloatBits = 32;

// 80비트 extended rate를 정수 Hz로 바꾼다. 소수가 있거나 음수·0·너무 크면 false다.
bool ReadExtendedRate(const uint8* bytes, uint32& rate) noexcept {
    const uint16 signExponent = ReadU16(bytes, true);
    if ((signExponent & ExtendedSignBit) != 0) {
        return false;
    }

    uint64 mantissa = 0;
    for (uint32 index = 0; index < MantissaBytes; ++index) {
        mantissa = (mantissa << BitsPerByte) | bytes[ExtendedMantissaOffset + index];
    }

    const int32 exponent =
        static_cast<int32>(signExponent & ExtendedExponentMask) - ExtendedExponentBias;
    // 값 = mantissa × 2^(exponent − 63). 정수 rate는 shift = 63 − exponent ∈ [0, 63]이어야 한다.
    const int32 shift = ExtendedMantissaBits - exponent;
    if (mantissa == 0 || shift < 0 || shift >= static_cast<int32>(BitsPerU64)) {
        return false;
    }

    const uint64 fraction = mantissa & ((uint64{1} << shift) - 1);
    const uint64 value = mantissa >> shift;
    if (fraction != 0 || value > MaxSourceSampleRate * uint64{2}) {
        return false;
    }

    rate = static_cast<uint32>(value);
    return true;
}

Result<void> DecodeAiffInto(Span<const uint8> file, DecodedSourceAudio& out) noexcept {
    const uint8* bytes = file.Data();
    if (file.Size() < FormHeaderBytes || !IsTag(bytes, "FORM") ||
        !(IsTag(bytes + FormTypeOffset, "AIFF") || IsTag(bytes + FormTypeOffset, "AIFC"))) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    const bool compressed = IsTag(bytes + FormTypeOffset, "AIFC");
    // FORM 크기는 파일 끝으로 자른다(WAV와 같다).
    const uint64 declaredEnd =
        static_cast<uint64>(ReadU32(bytes + FormSizeOffset, true)) + ChunkHeaderBytes;
    const usize formEnd = declaredEnd < file.Size() ? static_cast<usize>(declaredEnd) : file.Size();

    usize commOffset = 0;
    usize commSize = 0;
    usize ssndOffset = 0;
    usize ssndSize = 0;
    bool hasComm = false;
    bool hasSsnd = false;
    usize offset = FormHeaderBytes;
    while (offset + ChunkHeaderBytes <= formEnd) {
        const uint8* header = bytes + offset;
        usize size = ReadU32(header + ChunkSizeOffset, true);
        const usize body = offset + ChunkHeaderBytes;
        const bool isSsnd = IsTag(header, "SSND");
        if (size > formEnd - body) {
            // 끝을 넘는 SSND는 끝까지로 잘라 받고(아래 길이 검사가 frame 수와 맞춘다), 다른 chunk면
            // 멈추고 센다.
            if (!isSsnd) {
                out.CountIgnoredChunk();
                break;
            }

            size = formEnd - body;
        }

        if (IsTag(header, "COMM") && !hasComm) {
            commOffset = body;
            commSize = size;
            hasComm = true;
        } else if (isSsnd && !hasSsnd) {
            ssndOffset = body;
            ssndSize = size;
            hasSsnd = true;
        } else {
            // MARK·INST loop와 FVER 등은 지금 읽지 않는다(3.25.3절).
            out.CountIgnoredChunk();
        }

        offset = body + PaddedSize(size);
    }

    if (!hasComm || commSize < (compressed ? CommCompressionBytes : CommBytes)) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    const uint8* comm = bytes + commOffset;
    const uint32 channels = ReadU16(comm, true);
    const uint64 frames = ReadU32(comm + CommFramesOffset, true);
    const uint32 bits = ReadU16(comm + CommBitsOffset, true);
    uint32 sampleRate = 0;
    if (!ReadExtendedRate(comm + CommRateOffset, sampleRate)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    // 압축 type: NONE(BE 정수), sowt(LE 정수), fl32·FL32(BE float32).
    bool bigEndian = true;
    SampleEncoding encoding = SampleEncoding::SignedInt;
    if (compressed) {
        const uint8* type = comm + CommCompressionOffset;
        if (IsTag(type, "sowt")) {
            bigEndian = false;
        } else if (IsTag(type, "fl32") || IsTag(type, "FL32")) {
            encoding = SampleEncoding::Float;
        } else if (!IsTag(type, "NONE")) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }
    }

    const bool validBits = encoding == SampleEncoding::Float
                               ? bits == FloatBits
                               : (bits >= MinIntegerBits && bits <= MaxIntegerBits);
    if (!validBits) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (channels == 0 || frames == 0) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    if (channels > MaxSourceChannels || sampleRate < MinSourceSampleRate ||
        sampleRate > MaxSourceSampleRate) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    // 상한은 COMM의 frame 수로 SSND를 보기 전에 판정한다 — 큰 파일을 끝까지 읽지 않고 거절한다.
    if (ExceedsPcmLimit(channels, frames)) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    if (!hasSsnd || ssndSize < SsndHeaderBytes) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    // SSND: offset·blockSize 머리 뒤 offset byte를 건너뛴 곳부터 PCM이다.
    const uint32 dataOffset = ReadU32(bytes + ssndOffset, true);
    const uint32 bytesPerSample = (bits + BitsPerByte - 1) / BitsPerByte;
    const uint64 needed = frames * channels * bytesPerSample;
    if (dataOffset > ssndSize - SsndHeaderBytes ||
        needed > ssndSize - SsndHeaderBytes - dataOffset) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    auto pcm = out.TryAllocatePcm(channels, sampleRate, frames, FormatOf(bytesPerSample, encoding),
                                  SourceCodec::Pcm);
    if (!pcm) {
        return Result<void>::Failure(pcm.Error());
    }

    if (!DeinterleaveToFloat(bytes + ssndOffset + SsndHeaderBytes + dataOffset, channels, frames,
                             bytesPerSample, encoding, bigEndian, pcm.Value())) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    return Result<void>::Success();
}

} // namespace

Result<void> DecodeAiff(Span<const uint8> file, DecodedSourceAudio& out) noexcept {
    out.Reset();
    Result<void> decoded = DecodeAiffInto(file, out);
    // 실패하면 부분 결과를 남기지 않는다.
    if (!decoded) {
        out.Reset();
    }

    return decoded;
}

} // namespace rocks::audio
