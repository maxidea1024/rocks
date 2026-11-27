#include "OpusTestFixtures.h"
#include "SourceAudioReference.h"
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
namespace opus_tests_file {

using namespace rocks::audio;
using namespace rocks::audio::opusfixtures;
using namespace rocks::audio::sourcetest;

// Ogg Opus 원본 디코드 시험이다(Docs/Design/AudioSystem 3.29.3절).

namespace {

// lavfi sine의 기본 진폭(1/8)과 주파수다.
constexpr double SineAmplitude = 0.125;
constexpr double SineFrequency = 440.0;

// OpusOrder51의 채널별 sine 주파수(내부 순서 FL, FR, FC, LFE, SL, SR)와 길이다.
constexpr uint32 Order51Channels = 6;
constexpr double Order51Frequencies[Order51Channels] = {300.0, 500.0, 700.0, 60.0, 900.0, 1100.0};
constexpr uint64 Order51Frames = 9600;

// OpusHead 필드 위치(본문 기준)와 바꿀 값이다.
constexpr usize HeadChannelsOffset = 9;
constexpr usize HeadFamilyOffset = 18;
constexpr uint8 AmbisonicsFamily = 2;
constexpr uint8 DiscreteFamily = 255;
constexpr uint8 NineChannels = 9;

// x86-64 Windows(UCRT libm)에서 OpusStereo 디코드 결과(모든 채널 planar float32)의 SHA-256이다.
constexpr const char* StereoDigestX64 = sourcereference::OpusStereoDigestX64;

// 표본 위치가 맞을 때 기대하는 최소 신호 대 잡음 에너지 비(15 dB)다. pre-skip 312를 덜 자르거나 더
// 자르면 440 Hz 위상이 크게 어긋나 0 dB 근처로 떨어진다.
constexpr double MinimumSignalToNoise = 31.6;

// 오른쪽 = −왼쪽 stereo에서 (L+R) 에너지가 L 에너지에 대해 넘지 않을 비율이다.
constexpr double MaximumMidToLeft = 0.01;

// 시험 입력의 형식과 길이(마지막 granule − pre-skip)다.
constexpr uint32 OpusRate = 48000;
constexpr uint64 StereoFrames = sourcereference::OpusStereoFrames;
constexpr uint64 NoiseAFrames = 72000;
constexpr uint64 NoiseBFrames = 48000;
constexpr uint64 SurroundFrames = 2400;
constexpr uint32 SurroundChannels = 6;

// page 본문 안쪽을 바꿀 때 page 시작에서 건너뛰는 byte 수와, 한 byte 손상에 쓰는 값이다.
constexpr usize IntoPageBytes = 50;
constexpr uint8 CorruptionMask = 0x55;

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

ErrorCode OpusError(Span<const uint8> file, Allocator& allocator) noexcept {
    DecodedSourceAudio audio(allocator);
    return DecodeOpus(file, audio).Error();
}

} // namespace

TEST(Audio_Opus_StereoPreSkipAndLength) {
    SystemAllocator allocator;
    DecodedSourceAudio audio(allocator);
    REQUIRE(DecodeOpus(Bytes(OpusStereo), audio));
    // 길이는 마지막 granule − pre-skip이고, 출력 rate는 48000 Hz다.
    CHECK(audio.ChannelCount() == 2 && audio.SampleRate() == OpusRate);
    CHECK(audio.FrameCount() == StereoFrames);
    CHECK(audio.Codec() == SourceCodec::Opus && audio.SampleFormat() == SourceSampleFormat::Lossy);

    double signal = 0.0;
    double noise = 0.0;
    double left = 0.0;
    double mid = 0.0;
    for (uint64 index = 0; index < audio.FrameCount(); ++index) {
        const double reference = SineAmplitude * math::Sin(TwoPi * SineFrequency *
                                                           static_cast<double>(index) / OpusRate);
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

    // 5.1(mapping family 1): 6채널.
    REQUIRE(DecodeOpus(Bytes(OpusSurround), audio));
    CHECK(audio.ChannelCount() == SurroundChannels && audio.FrameCount() == SurroundFrames);

    // 채널마다 다른 sine: Vorbis 순서(FL, FC, FR, RL, RR, LFE)로 담긴 것을 내부 순서로 바꾼다.
    REQUIRE(DecodeOpus(Bytes(OpusOrder51), audio));
    CHECK(audio.FrameCount() == Order51Frames);
    CHECK(ChannelsMatchFrequencies(audio, Order51Frequencies, Order51Channels));
}

TEST(Audio_Opus_DecodeHashFixed) {
    // libopus는 SIMD·런타임 CPU 분기 없이 빌드하지만 exp·cos는 플랫폼 libm을 부른다. 그래서
    // 고정값은 아키텍처·OS마다 다르다(3.29.1절). x86-64는 Windows(UCRT) 값을 단언하고, 그 밖은 두
    // 번 디코드가 같은지만 본다.
    SystemAllocator allocator;
    DecodedSourceAudio audio(allocator);
    REQUIRE(DecodeOpus(Bytes(OpusStereo), audio));
    char hex[Sha256Digest::HexBytes + 1];
    DecodedDigest(audio, hex);
#if ROCKS_ARCH_X64
    CHECK(StringView(hex) == StringView(StereoDigestX64));
#else
    DecodedSourceAudio again(allocator);
    REQUIRE(DecodeOpus(Bytes(OpusStereo), again));
    char repeated[Sha256Digest::HexBytes + 1];
    DecodedDigest(again, repeated);
    CHECK(StringView(hex) == StringView(repeated));
#endif
}

TEST(Audio_Opus_UnsupportedHeads) {
    SystemAllocator allocator;
    // OpusHead의 mapping family 2(ambisonics)·255(discrete)와 9채널은 Unsupported다. page CRC를
    // 다시 맞춰 page 검사를 지나게 한다.
    struct HeadChange {
        usize offset;
        uint8 value;
    };

    for (const HeadChange change : {HeadChange{HeadFamilyOffset, AmbisonicsFamily},
                                    HeadChange{HeadFamilyOffset, DiscreteFamily},
                                    HeadChange{HeadChannelsOffset, NineChannels}}) {
        Array<uint8> file(allocator);
        REQUIRE(file.TryAppend(Bytes(OpusStereo)));
        const usize body = OggPageBodyOffset(file.Data());
        file[body + change.offset] = change.value;
        SetOggPageChecksum(file.Data(), OpusStereoPageOffsets[1]);
        CHECK(OpusError(file.View(), allocator) == ErrorCode::Unsupported);
    }

    // 올바른 Ogg Vorbis는 Opus가 아니다(OP_ENOTFORMAT → CorruptData).
    CHECK(OpusError(Bytes(vorbisfixtures::VorbisStereo), allocator) == ErrorCode::CorruptData);
}

TEST(Audio_Opus_PacketCorruption) {
    // audio page 본문 byte를 바꾸고 page CRC를 다시 맞춘다: page 검사를 지나 libopus packet
    // 디코더까지 간다. 멈추거나 abort하지 않고, 받으면 길이가 원본과 같고 표본이 유한하며, 아니면
    // CorruptData다.
    SystemAllocator allocator;
    DecodedSourceAudio audio(allocator);
    Array<uint8> mutated(allocator);
    REQUIRE(mutated.TryAppend(Bytes(OpusNoiseA)));
    RandomStream random(0x4F505543);
    constexpr uint32 Rounds = 200;
    constexpr uint32 FlipsPerRound = 3;
    const usize pageStart = OpusNoiseAPageOffsets[2];
    const usize pageBytes = OpusNoiseAPageOffsets[3] - pageStart;
    const usize bodyStart = pageStart + OggPageBodyOffset(mutated.Data() + pageStart);
    const usize bodyBytes = OpusNoiseAPageOffsets[3] - bodyStart;
    for (uint32 round = 0; round < Rounds; ++round) {
        for (usize index = 0; index < mutated.Size(); ++index) {
            mutated[index] = OpusNoiseA[index];
        }

        for (uint32 flip = 0; flip < FlipsPerRound; ++flip) {
            const usize at = bodyStart + random.NextUInt32Below(static_cast<uint32>(bodyBytes));
            mutated[at] = static_cast<uint8>(random.NextUInt32());
        }

        SetOggPageChecksum(mutated.Data() + pageStart, pageBytes);
        const Result<void> decoded = DecodeOpus(mutated.View(), audio);
        if (decoded) {
            bool finite = true;
            for (uint64 index = 0; index < audio.FrameCount(); ++index) {
                finite = finite && math::IsFinite(audio.Channel(0)[index]);
            }

            CHECK(finite && audio.FrameCount() == NoiseAFrames);
        } else {
            CHECK(decoded.Error() == ErrorCode::CorruptData);
        }
    }
}

TEST(Audio_Opus_ChainedAndPageIntegrity) {
    SystemAllocator allocator;
    const Span<const uint8> first = Bytes(OpusNoiseA);
    const Span<const uint8> second = Bytes(OpusNoiseB);
    const usize secondStart = first.Size();
    {
        const Array<uint8> chain = Join(allocator, first, second);
        REQUIRE(chain.Size() > 0);
        DecodedSourceAudio audio(allocator);
        REQUIRE(DecodeOpus(chain.View(), audio));
        CHECK(audio.ChannelCount() == 1 && audio.FrameCount() == NoiseAFrames + NoiseBFrames);
    }

    // 채널 수가 다른 link는 Unsupported.
    const Array<uint8> channelChange = Join(allocator, Bytes(OpusStereo), first);
    REQUIRE(channelChange.Size() > 0);
    CHECK(OpusError(channelChange.View(), allocator) == ErrorCode::Unsupported);

    // 첫 link 가운데 audio page·EOS page, 둘째 link BOS page 손상.
    for (const usize at :
         {OpusNoiseAPageOffsets[2] + IntoPageBytes, OpusNoiseAPageOffsets[3] + IntoPageBytes,
          secondStart + IntoPageBytes / 2}) {
        Array<uint8> chain = Join(allocator, first, second);
        REQUIRE(chain.Size() > at);
        chain[at] = static_cast<uint8>(chain[at] ^ CorruptionMask);
        CHECK(OpusError(chain.View(), allocator) == ErrorCode::CorruptData);
    }

    // page 중간·경계 잘림.
    const Array<uint8> chain = Join(allocator, first, second);
    REQUIRE(chain.Size() > 0);
    CHECK(OpusError(chain.View().Left(chain.Size() - IntoPageBytes / 2), allocator) ==
          ErrorCode::CorruptData);
    CHECK(OpusError(chain.View().Left(secondStart + OpusNoiseBPageOffsets[3]), allocator) ==
          ErrorCode::CorruptData);
}

TEST(Audio_Opus_Rejects) {
    SystemAllocator allocator;
    const uint8 notOgg[] = {'f', 'L', 'a', 'C', 0, 0, 0, 0};
    CHECK(OpusError(Bytes(notOgg), allocator) == ErrorCode::CorruptData);
    CHECK(OpusError(Span<const uint8>(), allocator) == ErrorCode::CorruptData);

    // Ogg이지만 Opus가 아님: Vorbis stream은 OpusHead가 없어 CorruptData다. 여기서는 OpusHead
    // 표지를 깨고 page CRC를 다시 맞추지 않으므로 page 검사에서 걸린다 — 둘 다 CorruptData다.
    Array<uint8> notOpus(allocator);
    REQUIRE(notOpus.TryAppend(Bytes(OpusStereo)));
    constexpr usize HeadMagicOffset = 28;
    notOpus[HeadMagicOffset] = 'X';
    CHECK(OpusError(notOpus.View(), allocator) == ErrorCode::CorruptData);

    // 실패하면 앞 결과를 비운다.
    DecodedSourceAudio audio(allocator);
    REQUIRE(DecodeOpus(Bytes(OpusStereo), audio));
    CHECK(!DecodeOpus(notOpus.View(), audio));
    CHECK(audio.FrameCount() == 0 && audio.ChannelCount() == 0 && audio.SampleRate() == 0);
    CHECK(audio.Codec() == SourceCodec::Pcm && audio.SampleFormat() == SourceSampleFormat::Int16);
}

TEST(Audio_Opus_TruncatedAndCorruptedInputs) {
    SystemAllocator allocator;
    DecodedSourceAudio audio(allocator);
    // 앞부분 길이(29 byte 간격): page 끝이 아니면 page 검사로 CorruptData, page 경계면 EOS가
    // 없어 CorruptData다.
    constexpr usize PrefixStep = 29;
    for (usize size = 0; size < sizeof(OpusNoiseA); size += PrefixStep) {
        CHECK(DecodeOpus(Span<const uint8>(OpusNoiseA, size), audio).Error() ==
              ErrorCode::CorruptData);
    }

    // byte를 무작위로 바꾼 입력: 멈추거나 범위 밖을 읽지 않는다. 바뀐 byte가 page CRC로 잡히지 않는
    // 경우는 없으므로 결과는 언제나 CorruptData다(값이 같게 바뀐 경우만 성공).
    Array<uint8> mutated(allocator);
    REQUIRE(mutated.TryAppend(Bytes(OpusNoiseA)));
    RandomStream random(0x4F505553);
    constexpr uint32 Rounds = 100;
    for (uint32 round = 0; round < Rounds; ++round) {
        for (usize index = 0; index < mutated.Size(); ++index) {
            mutated[index] = OpusNoiseA[index];
        }

        const usize at =
            static_cast<usize>(random.NextUInt32Below(static_cast<uint32>(mutated.Size())));
        const uint8 value = static_cast<uint8>(random.NextUInt32());
        mutated[at] = value;
        const Result<void> decoded = DecodeOpus(mutated.View(), audio);
        if (value == OpusNoiseA[at]) {
            CHECK(decoded && audio.FrameCount() == NoiseAFrames);
        } else {
            CHECK(decoded.Error() == ErrorCode::CorruptData);
        }
    }
}

} // namespace opus_tests_file
