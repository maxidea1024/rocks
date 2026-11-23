#include "FlacTestFixtures.h"

#include <AudioTools/SourceAudio.h>
#include <Core/Containers/Array.h>
#include <Core/Math/MathUtility.h>
#include <Core/Math/RandomStream.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace flac_tests_file {

using namespace rocks::audio;
using namespace rocks::audio::flacfixtures;

// FLAC 원본 디코드 시험이다(Docs/Design/AudioSystem.ko.md 3.27.3절). 무손실이므로 디코드 결과가
// 원본 정수 패턴과 정확히 같아야 한다.

namespace {

// 시험 패턴(gen_flac_wavs.py와 같은 식): x = frame × 2654435761 + channel × 40503 (mod 2^32)의 위
// bits를 부호 있는 값으로 쓴다.
constexpr uint32 PatternMultiplier = 2654435761u;
constexpr uint32 PatternChannelStep = 40503u;
constexpr uint32 BitsPerWord = 32;

// 시험 입력의 형식이다.
constexpr uint32 Stereo16Rate = 44100;
constexpr uint64 Stereo16Frames = 1000;
constexpr uint32 Mono24Rate = 96000;
constexpr uint64 Mono24Frames = 600;
constexpr uint32 Mono16Rate = 48000;
constexpr uint64 Mono16Frames = 500;
constexpr uint32 Bits16 = 16;
constexpr uint32 Bits24 = 24;

// ffmpeg가 메타데이터 뒤에 8 KiB PADDING 블록을 넣으므로 audio frame은 파일 끝쪽에 있다. 손상
// 시험은 끝에서 이만큼 안쪽만 바꾼다(FlacStereo16의 audio는 약 2.3 KB).
constexpr usize AudioTailBytes = 2048;

// STREAMINFO 필드 위치다("fLaC" 4 + block 머리 4 뒤 본문 기준): block·frame 크기 10 byte 뒤 rate 20
// bit, 채널 3 bit, bit 수 5 bit, 길이 36 bit, MD5 16 byte.
constexpr usize StreamInfoRateOffset = 18;
constexpr usize StreamInfoBitsOffset = 20;
constexpr usize StreamInfoLengthOffset = 21;
constexpr usize StreamInfoMd5Offset = 26;
constexpr usize Md5Bytes = 16;

// rate 4000 Hz(20 bit 0x00FA0)를 rate 필드 세 byte에 넣는 값이다(셋째 byte는 아래 4 bit만 남긴다).
constexpr uint8 Rate4000High = 0x00;
constexpr uint8 Rate4000Middle = 0xFA;
constexpr uint8 RateLowNibbleMask = 0x0F;

// bit 수 필드(bits − 1, 5 bit)는 byte 20의 bit 0과 byte 21의 위 4 bit다. 24비트(23 = 1 0111)로
// 바꾼다.
constexpr uint8 BitsHighBit = 0x01;
constexpr uint8 Bits24LowNibble = 0x70;

// subframe 머리의 첫 bit는 0이어야 한다(1이면 손상).
constexpr uint8 SubframePaddingBit = 0x80;

// 한 byte 손상에 쓰는 값이다.
constexpr uint8 CorruptionMask = 0x55;

int32 PatternSample(uint64 frame, uint32 channel, uint32 bits) noexcept {
    const uint32 x = static_cast<uint32>(frame) * PatternMultiplier + channel * PatternChannelStep;
    return static_cast<int32>(x >> (BitsPerWord - bits)) - static_cast<int32>(1u << (bits - 1));
}

Span<const uint8> Bytes(Span<const uint8> data) noexcept {
    return data;
}

// 디코드 결과가 패턴과 표본 하나까지 같은지 본다.
bool MatchesPattern(const DecodedSourceAudio& audio, uint32 bits) noexcept {
    const double scale = static_cast<double>(1u << (bits - 1));
    for (uint32 channel = 0; channel < audio.ChannelCount(); ++channel) {
        for (uint64 frame = 0; frame < audio.FrameCount(); ++frame) {
            const float expected = static_cast<float>(PatternSample(frame, channel, bits) / scale);
            if (audio.Channel(channel)[frame] != expected) {
                return false;
            }
        }
    }

    return true;
}

ErrorCode FlacError(Span<const uint8> file, Allocator& allocator) noexcept {
    DecodedSourceAudio audio(allocator);
    return DecodeFlac(file, audio).Error();
}

} // namespace

TEST(Audio_Flac_DecodesExactly) {
    SystemAllocator allocator;
    DecodedSourceAudio audio(allocator);
    REQUIRE(DecodeFlac(Bytes(FlacStereo16), audio));
    CHECK(audio.ChannelCount() == 2 && audio.SampleRate() == Stereo16Rate);
    CHECK(audio.FrameCount() == Stereo16Frames);
    CHECK(audio.Codec() == SourceCodec::Flac && audio.SampleFormat() == SourceSampleFormat::Int16);
    CHECK(MatchesPattern(audio, Bits16));

    REQUIRE(DecodeFlac(Bytes(FlacMono24), audio));
    CHECK(audio.ChannelCount() == 1 && audio.SampleRate() == Mono24Rate);
    CHECK(audio.FrameCount() == Mono24Frames && audio.SampleFormat() == SourceSampleFormat::Int24);
    CHECK(MatchesPattern(audio, Bits24));

    // 파이프로 쓴 파일은 STREAMINFO 길이·MD5가 0이다: 끝까지 디코드한 길이를 쓴다.
    REQUIRE(DecodeFlac(Bytes(FlacMono16Unknown), audio));
    CHECK(audio.SampleRate() == Mono16Rate && audio.FrameCount() == Mono16Frames);
    CHECK(MatchesPattern(audio, Bits16));
}

TEST(Audio_Flac_ToleratesTags) {
    SystemAllocator allocator;
    DecodedSourceAudio audio(allocator);
    // 앞 ID3v2(libFLAC이 건너뛴다)와 끝 ID3v1(frame 사이 쓰레기 — sync를 잃어도 받는다).
    constexpr uint8 Id3v2[] = {'I', 'D', '3', 3, 0, 0, 0, 0, 0, 4, 'T', 'E', 'S', 'T'};
    constexpr usize Id3v1Bytes = 128;
    Array<uint8> file(allocator);
    REQUIRE(file.TryAppend(Bytes(Id3v2)));
    REQUIRE(file.TryAppend(Bytes(FlacStereo16)));
    const uint8 tag[3] = {'T', 'A', 'G'};
    REQUIRE(file.TryAppend(Bytes(tag)));
    for (usize index = sizeof(tag); index < Id3v1Bytes; ++index) {
        REQUIRE(file.TryPushBack(static_cast<uint8>(index)));
    }

    REQUIRE(DecodeFlac(file.View(), audio));
    CHECK(audio.FrameCount() == Stereo16Frames && MatchesPattern(audio, Bits16));
    // "fLaC" 앞 쓰레기(첫 frame 앞의 LOST_SYNC)는 받는다. ID3v2 바로 뒤의 쓰레기는 libFLAC 1.5.0이
    // 메타데이터를 찾지 못해 실패한다(3.27.2절 한계) — 그래서 ID3 없이 0 byte만 앞에 둔다.
    constexpr usize PaddingBytes = 16;
    Array<uint8> padded(allocator);
    for (usize index = 0; index < PaddingBytes; ++index) {
        REQUIRE(padded.TryPushBack(0));
    }

    REQUIRE(padded.TryAppend(Bytes(FlacStereo16)));
    REQUIRE(DecodeFlac(padded.View(), audio));
    CHECK(audio.FrameCount() == Stereo16Frames && MatchesPattern(audio, Bits16));
}

TEST(Audio_Flac_Rejects) {
    SystemAllocator allocator;
    // FLAC이 아님·빈 입력·"fLaC"만.
    const uint8 notFlac[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
    CHECK(FlacError(Bytes(notFlac), allocator) == ErrorCode::InvalidArgument);
    CHECK(FlacError(Span<const uint8>(), allocator) == ErrorCode::InvalidArgument);
    CHECK(FlacError(Span<const uint8>(FlacStereo16, 4), allocator) == ErrorCode::InvalidArgument);

    // audio data 한 byte가 바뀌면 frame CRC가 맞지 않는다.
    Array<uint8> corrupted(allocator);
    REQUIRE(corrupted.TryAppend(Bytes(FlacStereo16)));
    const usize middle = corrupted.Size() - AudioTailBytes / 2;
    corrupted[middle] = static_cast<uint8>(corrupted[middle] ^ CorruptionMask);
    CHECK(FlacError(corrupted.View(), allocator) == ErrorCode::InvalidArgument);

    // 길이를 아는 파일이 마지막 frame 안에서 잘리면 STREAMINFO 길이·MD5와 맞지 않는다.
    constexpr usize CutBytes = 100;
    CHECK(FlacError(Span<const uint8>(FlacStereo16, sizeof(FlacStereo16) - CutBytes), allocator) ==
          ErrorCode::InvalidArgument);

    // STREAMINFO rate가 범위 밖(4000 Hz)이면 Unsupported.
    Array<uint8> lowRate(allocator);
    REQUIRE(lowRate.TryAppend(Bytes(FlacStereo16)));
    lowRate[StreamInfoRateOffset] = Rate4000High;
    lowRate[StreamInfoRateOffset + 1] = Rate4000Middle;
    lowRate[StreamInfoRateOffset + 2] =
        static_cast<uint8>(lowRate[StreamInfoRateOffset + 2] & RateLowNibbleMask);
    CHECK(FlacError(lowRate.View(), allocator) == ErrorCode::Unsupported);

    // 실패하면 앞 결과를 비운다.
    DecodedSourceAudio audio(allocator);
    REQUIRE(DecodeFlac(Bytes(FlacStereo16), audio));
    CHECK(!DecodeFlac(corrupted.View(), audio));
    CHECK(audio.FrameCount() == 0 && audio.ChannelCount() == 0 && audio.SampleRate() == 0);
    CHECK(audio.Codec() == SourceCodec::Pcm && audio.SampleFormat() == SourceSampleFormat::Int16);
}

TEST(Audio_Flac_StreamInfoChecks) {
    SystemAllocator allocator;
    // MD5만 바꾸면 frame은 모두 맞지만 finish가 MD5 불일치로 실패한다.
    {
        Array<uint8> file(allocator);
        REQUIRE(file.TryAppend(Bytes(FlacStereo16)));
        file[StreamInfoMd5Offset] = static_cast<uint8>(file[StreamInfoMd5Offset] ^ CorruptionMask);
        CHECK(FlacError(file.View(), allocator) == ErrorCode::InvalidArgument);
    }

    // MD5를 0으로 하고 마지막 frame을 frame 경계에서 떼면 STREAMINFO 길이와 맞지 않는다.
    {
        Array<uint8> file(allocator);
        REQUIRE(file.TryAppend(Span<const uint8>(FlacStereo16, FlacStereo16FrameOffsets[3])));
        for (usize index = 0; index < Md5Bytes; ++index) {
            file[StreamInfoMd5Offset + index] = 0;
        }

        CHECK(FlacError(file.View(), allocator) == ErrorCode::InvalidArgument);
    }

    // STREAMINFO 길이가 실제보다 짧으면(MD5 0) 그 길이를 넘는 frame에서 실패한다.
    {
        Array<uint8> file(allocator);
        REQUIRE(file.TryAppend(Bytes(FlacStereo16)));
        for (usize index = 0; index < Md5Bytes; ++index) {
            file[StreamInfoMd5Offset + index] = 0;
        }

        // 길이 36 bit 중 아래 32 bit(byte 22–25)를 500으로 둔다(위 4 bit는 이미 0).
        constexpr uint32 ShortLength = 500;
        constexpr uint32 BitsPerByte = 8;
        for (uint32 index = 0; index < 4; ++index) {
            file[StreamInfoLengthOffset + 1 + index] =
                static_cast<uint8>(ShortLength >> ((3 - index) * BitsPerByte));
        }

        CHECK(FlacError(file.View(), allocator) == ErrorCode::InvalidArgument);
    }

    // STREAMINFO 길이가 PCM 상한을 넘으면 디코드 전에 CapacityExceeded다(36 bit 최대값).
    {
        Array<uint8> file(allocator);
        REQUIRE(file.TryAppend(Bytes(FlacStereo16)));
        file[StreamInfoLengthOffset] =
            static_cast<uint8>(file[StreamInfoLengthOffset] | RateLowNibbleMask);
        for (usize index = 1; index <= 4; ++index) {
            file[StreamInfoLengthOffset + index] = 0xFF;
        }

        CHECK(FlacError(file.View(), allocator) == ErrorCode::CapacityExceeded);
    }

    // STREAMINFO bit 수를 24로 바꾸면 frame 머리(16비트)와 형식이 달라 Unsupported다.
    {
        Array<uint8> file(allocator);
        REQUIRE(file.TryAppend(Bytes(FlacStereo16)));
        file[StreamInfoBitsOffset] = static_cast<uint8>(file[StreamInfoBitsOffset] | BitsHighBit);
        file[StreamInfoBitsOffset + 1] = static_cast<uint8>(
            (file[StreamInfoBitsOffset + 1] & RateLowNibbleMask) | Bits24LowNibble);
        CHECK(FlacError(file.View(), allocator) == ErrorCode::Unsupported);
    }

    // 길이·MD5가 없는 파일에서 가운데 frame의 subframe 머리가 깨지면 libFLAC은 LOST_SYNC를 내고 그
    // 자리를 무음 frame으로 채운다. 무손실 원본을 조용히 바꾸지 않도록 InvalidArgument다.
    {
        Array<uint8> file(allocator);
        REQUIRE(file.TryAppend(Bytes(FlacMono16Unknown)));
        const usize subframe = FlacMono16UnknownSubframeOffsets[1];
        file[subframe] = static_cast<uint8>(file[subframe] | SubframePaddingBit);
        CHECK(FlacError(file.View(), allocator) == ErrorCode::InvalidArgument);
    }

    // 길이·MD5가 없는 파일을 frame 경계에서 자르면 알아챌 수단이 없어 짧게 받는다(3.27.2절 한계).
    {
        DecodedSourceAudio audio(allocator);
        REQUIRE(DecodeFlac(Span<const uint8>(FlacMono16Unknown, FlacMono16UnknownFrameOffsets[3]),
                           audio));
        CHECK(audio.FrameCount() < Mono16Frames && MatchesPattern(audio, Bits16));
    }
}

TEST(Audio_Flac_TruncatedAndCorruptedInputs) {
    SystemAllocator allocator;
    DecodedSourceAudio audio(allocator);
    // 길이를 모르는 파일의 모든 앞부분(41 byte 간격): 받으면 패턴의 앞부분과 같고, 아니면
    // InvalidArgument다.
    constexpr usize PrefixStep = 41;
    for (usize size = 0; size <= sizeof(FlacMono16Unknown); size += PrefixStep) {
        const Result<void> decoded = DecodeFlac(Span<const uint8>(FlacMono16Unknown, size), audio);
        if (decoded) {
            CHECK(audio.FrameCount() <= Mono16Frames && MatchesPattern(audio, Bits16));
        } else {
            CHECK(decoded.Error() == ErrorCode::InvalidArgument);
        }
    }

    // audio 영역 byte를 무작위로 바꾼 입력: 멈추거나 범위 밖을 읽지 않고, 받으면 표본이 모두
    // 유한하다.
    Array<uint8> mutated(allocator);
    REQUIRE(mutated.TryAppend(Bytes(FlacStereo16)));
    RandomStream random(0x464C4143);
    constexpr uint32 Rounds = 200;
    constexpr uint32 FlipsPerRound = 4;
    for (uint32 round = 0; round < Rounds; ++round) {
        for (usize index = 0; index < mutated.Size(); ++index) {
            mutated[index] = FlacStereo16[index];
        }

        for (uint32 flip = 0; flip < FlipsPerRound; ++flip) {
            const usize at =
                mutated.Size() - AudioTailBytes +
                static_cast<usize>(random.NextUInt32Below(static_cast<uint32>(AudioTailBytes)));
            mutated[at] = static_cast<uint8>(random.NextUInt32());
        }

        const Result<void> decoded = DecodeFlac(mutated.View(), audio);
        if (decoded) {
            bool finite = true;
            for (uint32 channel = 0; channel < audio.ChannelCount(); ++channel) {
                for (uint64 index = 0; index < audio.FrameCount(); ++index) {
                    finite = finite && math::IsFinite(audio.Channel(channel)[index]);
                }
            }

            CHECK(finite);
        } else {
            CHECK(decoded.Error() == ErrorCode::InvalidArgument ||
                  decoded.Error() == ErrorCode::Unsupported ||
                  decoded.Error() == ErrorCode::CapacityExceeded ||
                  decoded.Error() == ErrorCode::OutOfMemory);
        }
    }
}

} // namespace flac_tests_file
