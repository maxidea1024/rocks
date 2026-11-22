#pragma once

#include <AudioTools/SourceAudio.h>

// WAV·AIFF 디코더가 함께 쓰는 byte 읽기와 표본 변환이다(Docs/Design/AudioSystem.ko.md 3.25절).
namespace rocks::audio::sourcedetail {

// 정수 표본을 [−1, 1)로 바꾸는 배율이다(2^(bits−1)).
inline constexpr double Int8Scale = 128.0;
inline constexpr double Int16Scale = 32768.0;
inline constexpr double Int24Scale = 8388608.0;
inline constexpr double Int32Scale = 2147483648.0;

// 부호 없는 8비트 WAV 표본의 중앙값이다.
inline constexpr int32 UnsignedByteBias = 128;

// byte 수별 bit 수다.
inline constexpr uint32 BitsPerByte = 8;

// RIFF·IFF chunk 머리(id 4 + 크기 4) 크기다.
inline constexpr uint32 ChunkHeaderBytes = 8;

// chunk 본문은 짝수 byte로 맞춘다(홀수 크기 뒤 pad 1 byte).
inline usize PaddedSize(usize size) noexcept {
    return size + (size & 1u);
}

// 끝 방향을 지정해 부호 없는 정수를 읽는다.
inline uint32 ReadUnsigned(const uint8* bytes, uint32 count, bool bigEndian) noexcept {
    uint32 value = 0;
    for (uint32 index = 0; index < count; ++index) {
        const uint32 shift = bigEndian ? (count - 1 - index) * BitsPerByte : index * BitsPerByte;
        value |= static_cast<uint32>(bytes[index]) << shift;
    }

    return value;
}

inline uint16 ReadU16(const uint8* bytes, bool bigEndian) noexcept {
    return static_cast<uint16>(ReadUnsigned(bytes, 2, bigEndian));
}

inline uint32 ReadU32(const uint8* bytes, bool bigEndian) noexcept {
    return ReadUnsigned(bytes, 4, bigEndian);
}

// chunk id(4 byte)가 tag와 같은지 본다.
inline bool IsTag(const uint8* bytes, const char* tag) noexcept {
    return bytes[0] == static_cast<uint8>(tag[0]) && bytes[1] == static_cast<uint8>(tag[1]) &&
           bytes[2] == static_cast<uint8>(tag[2]) && bytes[3] == static_cast<uint8>(tag[3]);
}

// 표본 부호화다.
enum class SampleEncoding : uint8 {
    UnsignedInt,
    SignedInt,
    Float
};

// 교차 표본 byte를 planar float로 바꾼다. bytesPerSample은 1·2·3·4(정수) 또는 4·8(float)이다.
void DeinterleaveToFloat(const uint8* data, uint32 channels, uint64 frames, uint32 bytesPerSample,
                         SampleEncoding encoding, bool bigEndian, float* planar) noexcept;

// 표본 형식(SourceSampleFormat)이다.
SourceSampleFormat FormatOf(uint32 bytesPerSample, SampleEncoding encoding) noexcept;

// PCM 상한(float32 기준)을 넘는지 본다.
inline bool ExceedsPcmLimit(uint32 channels, uint64 frames) noexcept {
    return frames > MaxSoundClipPcmBytes / (static_cast<uint64>(channels) * sizeof(float));
}

} // namespace rocks::audio::sourcedetail
