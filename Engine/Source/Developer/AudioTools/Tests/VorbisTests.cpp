#include "SourceAudioTestSupport.h"
#include "VorbisTestFixtures.h"

#include <AudioTools/SourceAudio.h>
#include <Base/Compiler.h>
#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Math/MathUtility.h>
#include <Core/Math/RandomStream.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace vorbis_tests_file {

using namespace rocks::audio;
using namespace rocks::audio::vorbisfixtures;
using namespace rocks::audio::sourcetest;

// Ogg Vorbis 원본 디코드 시험이다(Docs/Design/AudioSystem.ko.md 3.28.3절).

namespace {

// lavfi sine의 기본 진폭(1/8)과 주파수다.
constexpr double SineAmplitude = 0.125;
constexpr double SineFrequency = 440.0;

// 표본 위치가 맞을 때 기대하는 최소 신호 대 잡음 에너지 비(20 dB)다. ffmpeg로 재면 약 41 dB이다.
constexpr double MinimumSignalToNoise = 100.0;

// 오른쪽 = −왼쪽 stereo에서 (L+R) 에너지가 L 에너지에 대해 넘지 않을 비율이다.
constexpr double MaximumMidToLeft = 0.01;

// VorbisOrder51의 채널별 sine 주파수(내부 순서 FL, FR, FC, LFE, SL, SR)와 길이다.
constexpr uint32 Order51Channels = 6;
constexpr double Order51Frequencies[Order51Channels] = {300.0, 500.0, 700.0, 60.0, 900.0, 1100.0};
constexpr uint64 Order51Frames = 8820;

// x86-64 Windows(UCRT libm)에서 VorbisStereo 디코드 결과(모든 채널 planar float32)의 SHA-256이다.
constexpr const char* StereoDigestX64 =
    "df80529fcc7983afcd3810f287a338aad25381eeb623e73ade462c6909028a84";

// 시험 입력의 rate·frame 수(마지막 page granule)다.
constexpr uint32 Rate44100 = 44100;
constexpr uint32 Rate22050 = 22050;
constexpr uint64 StereoFrames = 11025;
constexpr uint64 Mono22kFrames = 2205;
constexpr uint64 Mono44kFrames = 4410;
constexpr uint64 Mono44kShortFrames = 2205;

// Ogg page 머리 크기(capture "OggS"부터 segment 수까지 27 byte)다.
constexpr usize OggPageHeaderBytes = 27;

// 잡음 link 둘의 길이(마지막 granule)다.
constexpr uint64 NoiseAFrames = 33075;
constexpr uint64 NoiseBFrames = 22050;

// page 본문 안쪽을 바꿀 때 page 시작에서 건너뛰는 byte 수와, 한 byte 손상에 쓰는 값이다.
constexpr usize IntoPageBytes = 50;
constexpr uint8 CorruptionMask = 0x55;

// 앞뒤에 붙이는 쓰레기 byte 수다.
constexpr usize JunkBytes = 4;

Span<const uint8> Bytes(Span<const uint8> data) noexcept {
    return data;
}

Array<uint8> Join(Allocator& allocator, Span<const uint8> first,
                  Span<const uint8> second) noexcept {
    Array<uint8> joined(allocator);
    if (!joined.TryAppend(first) || !joined.TryAppend(second)) {
        joined.Clear();
    }

    return joined;
}

// first와 second를 잇고, at 위치 byte를 바꾼다(at이 크기 밖이면 바꾸지 않는다).
Array<uint8> JoinCorrupted(Allocator& allocator, Span<const uint8> first, Span<const uint8> second,
                           usize at) noexcept {
    Array<uint8> joined = Join(allocator, first, second);
    if (at < joined.Size()) {
        joined[at] = static_cast<uint8>(joined[at] ^ CorruptionMask);
    }

    return joined;
}

ErrorCode VorbisError(Span<const uint8> file, Allocator& allocator) noexcept {
    DecodedSourceAudio audio(allocator);
    return DecodeVorbis(file, audio).Error();
}

} // namespace

TEST(Audio_Vorbis_StereoGranuleLength) {
    SystemAllocator allocator;
    DecodedSourceAudio audio(allocator);
    REQUIRE(DecodeVorbis(Bytes(VorbisStereo), audio));
    // 길이는 마지막 granule(원본 frame 수)과 같다.
    CHECK(audio.ChannelCount() == 2 && audio.SampleRate() == Rate44100);
    CHECK(audio.FrameCount() == StereoFrames);
    CHECK(audio.Codec() == SourceCodec::Vorbis &&
          audio.SampleFormat() == SourceSampleFormat::Lossy);

    // 원본 sine과 표본 위치가 맞고, 오른쪽은 왼쪽의 부호 반전이다.
    double signal = 0.0;
    double noise = 0.0;
    double left = 0.0;
    double mid = 0.0;
    for (uint64 index = 0; index < audio.FrameCount(); ++index) {
        const double reference = SineAmplitude * math::Sin(TwoPi * SineFrequency *
                                                           static_cast<double>(index) / Rate44100);
        const double l = audio.Channel(0)[index];
        const double r = audio.Channel(1)[index];
        signal += reference * reference;
        noise += (l - reference) * (l - reference);
        left += l * l;
        mid += (l + r) * (l + r);
    }

    REQUIRE(noise > 0.0);
    CHECK(signal / noise >= MinimumSignalToNoise);
    CHECK(mid < left * MaximumMidToLeft);

    REQUIRE(DecodeVorbis(Bytes(VorbisMono22k), audio));
    CHECK(audio.ChannelCount() == 1 && audio.SampleRate() == Rate22050 &&
          audio.FrameCount() == Mono22kFrames);

    // 채널마다 다른 sine: Vorbis 5.1 순서(FL, C, FR, RL, RR, LFE)를 내부 순서로 바꾼다.
    REQUIRE(DecodeVorbis(Bytes(VorbisOrder51), audio));
    CHECK(audio.FrameCount() == Order51Frames);
    CHECK(ChannelsMatchFrequencies(audio, Order51Frequencies, Order51Channels));
}

TEST(Audio_Vorbis_DecodeHashFixed) {
    // Vorbis 디코드는 SIMD 없는 float 계산이지만 libm(sin·cos 등)을 부를 수 있어 고정값은
    // 아키텍처·OS마다 다르다(3.28.1절). x86-64는 Windows(UCRT) 값을 단언하고, 그 밖은 두 번
    // 디코드가 같은지만 본다.
    SystemAllocator allocator;
    DecodedSourceAudio audio(allocator);
    REQUIRE(DecodeVorbis(Bytes(VorbisStereo), audio));
    char hex[Sha256Digest::HexBytes + 1];
    DecodedDigest(audio, hex);
#if ROCKS_ARCH_X64
    CHECK(StringView(hex) == StringView(StereoDigestX64));
#else
    DecodedSourceAudio again(allocator);
    REQUIRE(DecodeVorbis(Bytes(VorbisStereo), again));
    char repeated[Sha256Digest::HexBytes + 1];
    DecodedDigest(again, repeated);
    CHECK(StringView(hex) == StringView(repeated));
#endif
}

TEST(Audio_Vorbis_ChainedStreams) {
    SystemAllocator allocator;
    DecodedSourceAudio audio(allocator);
    // 형식이 같은 link를 이으면 길이가 합이다.
    const Array<uint8> same = Join(allocator, Bytes(VorbisMono44k), Bytes(VorbisMono44kShort));
    REQUIRE(same.Size() > 0);
    REQUIRE(DecodeVorbis(same.View(), audio));
    CHECK(audio.SampleRate() == Rate44100 &&
          audio.FrameCount() == Mono44kFrames + Mono44kShortFrames);

    // rate·채널이 다른 link는 Unsupported.
    const Array<uint8> rateChange = Join(allocator, Bytes(VorbisMono44k), Bytes(VorbisMono22k));
    REQUIRE(rateChange.Size() > 0);
    CHECK(VorbisError(rateChange.View(), allocator) == ErrorCode::Unsupported);
    const Array<uint8> channelChange = Join(allocator, Bytes(VorbisStereo), Bytes(VorbisMono44k));
    REQUIRE(channelChange.Size() > 0);
    CHECK(VorbisError(channelChange.View(), allocator) == ErrorCode::Unsupported);
}

TEST(Audio_Vorbis_Rejects) {
    SystemAllocator allocator;
    // Ogg가 아님·빈 입력·첫 page만.
    const uint8 notOgg[] = {'f', 'L', 'a', 'C', 0, 0, 0, 0};
    CHECK(VorbisError(Bytes(notOgg), allocator) == ErrorCode::InvalidArgument);
    CHECK(VorbisError(Span<const uint8>(), allocator) == ErrorCode::InvalidArgument);
    CHECK(VorbisError(Span<const uint8>(VorbisStereo, OggPageHeaderBytes), allocator) ==
          ErrorCode::InvalidArgument);

    // audio page byte 하나가 바뀌면 page CRC가 맞지 않아 그 page가 빠진다(OV_HOLE 또는 길이
    // 불일치).
    Array<uint8> corrupted(allocator);
    REQUIRE(corrupted.TryAppend(Bytes(VorbisStereo)));
    const usize middle = VorbisStereoPageOffsets[2] + IntoPageBytes;
    corrupted[middle] = static_cast<uint8>(corrupted[middle] ^ CorruptionMask);
    CHECK(VorbisError(corrupted.View(), allocator) == ErrorCode::InvalidArgument);

    // 실패하면 앞 결과를 비운다.
    DecodedSourceAudio audio(allocator);
    REQUIRE(DecodeVorbis(Bytes(VorbisStereo), audio));
    CHECK(!DecodeVorbis(corrupted.View(), audio));
    CHECK(audio.FrameCount() == 0 && audio.ChannelCount() == 0 && audio.SampleRate() == 0);
    CHECK(audio.Codec() == SourceCodec::Pcm && audio.SampleFormat() == SourceSampleFormat::Int16);
}

TEST(Audio_Vorbis_PageIntegrity) {
    SystemAllocator allocator;
    const Span<const uint8> first = Bytes(VorbisNoiseA);
    const Span<const uint8> second = Bytes(VorbisNoiseB);
    const usize secondStart = first.Size();
    // 손상 없는 연결: link 경계의 OV_HOLE을 지나 두 link가 모두 나온다(audio page가 둘씩 있다).
    {
        const Array<uint8> chain = Join(allocator, first, second);
        DecodedSourceAudio audio(allocator);
        REQUIRE(DecodeVorbis(chain.View(), audio));
        CHECK(audio.FrameCount() == NoiseAFrames + NoiseBFrames);
    }

    // 첫 link 가운데 audio page, 첫 link 끝(EOS) page, 둘째 link BOS page 손상: CRC가 맞지 않아
    // page가 빠진다.
    for (const usize at :
         {VorbisNoiseAPageOffsets[2] + IntoPageBytes, VorbisNoiseAPageOffsets[3] + IntoPageBytes,
          secondStart + IntoPageBytes / 2}) {
        const Array<uint8> chain = JoinCorrupted(allocator, first, second, at);
        REQUIRE(chain.Size() > 0);
        CHECK(VorbisError(chain.View(), allocator) == ErrorCode::InvalidArgument);
    }

    // page 중간에서 잘림, page 경계에서 잘림(EOS 없음).
    const Array<uint8> chain = Join(allocator, first, second);
    REQUIRE(chain.Size() > 0);
    CHECK(VorbisError(chain.View().Left(chain.Size() - IntoPageBytes), allocator) ==
          ErrorCode::InvalidArgument);
    CHECK(VorbisError(chain.View().Left(secondStart + VorbisNoiseBPageOffsets[3]), allocator) ==
          ErrorCode::InvalidArgument);

    // page 하나가 통째로 빠짐(page 번호 끊김).
    {
        Array<uint8> missing(allocator);
        REQUIRE(missing.TryAppend(first.Left(VorbisNoiseAPageOffsets[2])));
        REQUIRE(missing.TryAppend(Span<const uint8>(first.Data() + VorbisNoiseAPageOffsets[3],
                                                    first.Size() - VorbisNoiseAPageOffsets[3])));
        CHECK(VorbisError(missing.View(), allocator) == ErrorCode::InvalidArgument);
    }

    // 같은 serial을 다시 이음(규격 위반), 앞·뒤 쓰레기.
    const Array<uint8> sameSerial = Join(allocator, first, first);
    REQUIRE(sameSerial.Size() > 0);
    CHECK(VorbisError(sameSerial.View(), allocator) == ErrorCode::InvalidArgument);
    const uint8 junk[JunkBytes] = {1, 2, 3, 4};
    const Array<uint8> leading = Join(allocator, Bytes(junk), first);
    const Array<uint8> trailing = Join(allocator, first, Bytes(junk));
    REQUIRE(leading.Size() > 0 && trailing.Size() > 0);
    CHECK(VorbisError(leading.View(), allocator) == ErrorCode::InvalidArgument);
    CHECK(VorbisError(trailing.View(), allocator) == ErrorCode::InvalidArgument);

    // 다중화: 첫 stream이 열린 채 둘째 stream의 BOS page가 온다.
    {
        const usize bos = VorbisNoiseAPageOffsets[1];
        Array<uint8> multiplexed(allocator);
        REQUIRE(multiplexed.TryAppend(first.Left(bos)));
        REQUIRE(multiplexed.TryAppend(second.Left(bos)));
        REQUIRE(multiplexed.TryAppend(Span<const uint8>(first.Data() + bos, first.Size() - bos)));
        REQUIRE(multiplexed.TryAppend(Span<const uint8>(second.Data() + bos, second.Size() - bos)));
        CHECK(VorbisError(multiplexed.View(), allocator) == ErrorCode::Unsupported);
    }
}

TEST(Audio_Vorbis_TruncatedAndCorruptedInputs) {
    SystemAllocator allocator;
    DecodedSourceAudio audio(allocator);
    // 모든 앞부분 길이(43 byte 간격): 받으면 길이가 원본 이하, 아니면 InvalidArgument다.
    constexpr usize PrefixStep = 43;
    for (usize size = 0; size <= sizeof(VorbisStereo); size += PrefixStep) {
        const Result<void> decoded = DecodeVorbis(Span<const uint8>(VorbisStereo, size), audio);
        if (decoded) {
            CHECK(audio.FrameCount() > 0 && audio.FrameCount() <= StereoFrames);
        } else {
            CHECK(decoded.Error() == ErrorCode::InvalidArgument);
        }
    }

    // byte를 무작위로 바꾼 입력: 멈추거나 범위 밖을 읽지 않고, 받으면 표본이 모두 유한하다.
    Array<uint8> mutated(allocator);
    REQUIRE(mutated.TryAppend(Bytes(VorbisStereo)));
    RandomStream random(0x564F5242);
    constexpr uint32 Rounds = 200;
    constexpr uint32 FlipsPerRound = 4;
    for (uint32 round = 0; round < Rounds; ++round) {
        for (usize index = 0; index < mutated.Size(); ++index) {
            mutated[index] = VorbisStereo[index];
        }

        for (uint32 flip = 0; flip < FlipsPerRound; ++flip) {
            const usize at =
                static_cast<usize>(random.NextUInt32Below(static_cast<uint32>(mutated.Size())));
            mutated[at] = static_cast<uint8>(random.NextUInt32());
        }

        const Result<void> decoded = DecodeVorbis(mutated.View(), audio);
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
                  decoded.Error() == ErrorCode::CapacityExceeded);
        }
    }
}

} // namespace vorbis_tests_file
