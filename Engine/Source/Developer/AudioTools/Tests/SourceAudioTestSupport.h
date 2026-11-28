#pragma once

#include <AudioTools/SourceAudio.h>
#include <Core/Hash/Sha256.h>
#include <Core/Math/MathUtility.h>

// 원본 디코더 시험이 함께 쓰는 도구다(Docs/Design/AudioSystem 3.26–3.29절): 채널별 주파수
// 확인(채널 순서), Ogg page CRC 다시 계산(page 검사를 지나 packet 디코더까지 가는 손상 시험),
// 디코드 결과 SHA-256.
namespace rocks::audio::sourcetest {

inline constexpr double TwoPi = 6.283185307179586;

// frequency 성분의 에너지(Goertzel)다.
inline double GoertzelPower(const float* samples, uint64 frames, double frequency,
                            uint32 rate) noexcept {
    const double coefficient = 2.0 * math::Cos(TwoPi * frequency / rate);
    double previous = 0.0;
    double before = 0.0;
    for (uint64 index = 0; index < frames; ++index) {
        const double current = samples[index] + coefficient * previous - before;
        before = previous;
        previous = current;
    }

    return previous * previous + before * before - coefficient * previous * before;
}

// 채널 c가 frequencies 가운데 frequencies[c]를 가장 강하게 가지면 true다(채널마다 다른 sine을 넣은
// 입력의 순서 확인).
inline bool ChannelsMatchFrequencies(const DecodedSourceAudio& audio, const double* frequencies,
                                     uint32 count) noexcept {
    if (audio.ChannelCount() != count) {
        return false;
    }

    for (uint32 channel = 0; channel < count; ++channel) {
        const double expected = GoertzelPower(audio.Channel(channel), audio.FrameCount(),
                                              frequencies[channel], audio.SampleRate());
        for (uint32 other = 0; other < count; ++other) {
            if (other != channel &&
                GoertzelPower(audio.Channel(channel), audio.FrameCount(), frequencies[other],
                              audio.SampleRate()) >= expected) {
                return false;
            }
        }
    }

    return true;
}

// Ogg page의 CRC(다항식 0x04C11DB7, 반사 없음, 초기값 0, CRC 필드는 0으로 두고 계산)를 다시 적는다.
inline void SetOggPageChecksum(uint8* page, usize pageBytes) noexcept {
    constexpr usize ChecksumOffset = 22;
    constexpr usize ChecksumBytes = 4;
    constexpr uint32 Polynomial = 0x04C11DB7u;
    constexpr uint32 BitsPerByte = 8;
    constexpr uint32 TopShift = 24;
    constexpr uint32 TopBit = 0x80000000u;
    for (usize index = 0; index < ChecksumBytes; ++index) {
        page[ChecksumOffset + index] = 0;
    }

    uint32 crc = 0;
    for (usize index = 0; index < pageBytes; ++index) {
        crc ^= static_cast<uint32>(page[index]) << TopShift;
        for (uint32 bit = 0; bit < BitsPerByte; ++bit) {
            crc = (crc & TopBit) != 0 ? (crc << 1) ^ Polynomial : crc << 1;
        }
    }

    for (usize index = 0; index < ChecksumBytes; ++index) {
        page[ChecksumOffset + index] = static_cast<uint8>(crc >> (index * BitsPerByte));
    }
}

// Ogg page 본문 시작(머리 27 byte + segment 표) 위치다.
inline usize OggPageBodyOffset(const uint8* page) noexcept {
    constexpr usize PageHeaderBytes = 27;
    constexpr usize SegmentCountOffset = 26;
    return PageHeaderBytes + page[SegmentCountOffset];
}

// 모든 채널 표본(planar float32 byte)의 SHA-256 16진수다.
inline void DecodedDigest(const DecodedSourceAudio& audio,
                          char (&hex)[Sha256Digest::HexBytes + 1]) noexcept {
    Sha256 hasher;
    for (uint32 channel = 0; channel < audio.ChannelCount(); ++channel) {
        hasher.Update(Span<const uint8>(reinterpret_cast<const uint8*>(audio.Channel(channel)),
                                        static_cast<usize>(audio.FrameCount()) * sizeof(float)));
    }

    hasher.Finish().ToHex(hex);
}

} // namespace rocks::audio::sourcetest
