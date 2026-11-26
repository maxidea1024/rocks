#include "Mp3TestFixtures.h"
#include "SourceAudioReference.h"

#include <AudioTools/SourceAudio.h>
#include <Base/Compiler.h>
#include <Core/Containers/Array.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Math/MathUtility.h>
#include <Core/Math/RandomStream.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace mp3_tests_file {

using namespace rocks::audio;
using namespace rocks::audio::mp3fixtures;

// MP3 원본 디코드 시험이다(Docs/Design/AudioSystem 3.26.3절). 입력은 LAME·ffmpeg로
// 만든 작은 파일이다(Mp3TestFixtures.h).

namespace {

// lavfi sine의 기본 진폭(1/8)과 주파수다.
constexpr double SineAmplitude = 0.125;
constexpr double SineFrequency = 440.0;
constexpr double TwoPi = 6.283185307179586;

// 표본 위치가 맞을 때 기대하는 최소 신호 대 잡음 에너지 비(15 dB = 10^1.5)다. ffmpeg로 재면
// 약 26 dB이고, 디코더 지연 529 표본만큼 어긋나면 440 Hz의 위상이 반 주기쯤 틀어져 0 dB 아래로
// 떨어진다.
constexpr double MinimumSignalToNoise = 31.6;

// 오른쪽 = −왼쪽 stereo에서 (L+R) 에너지가 L 에너지에 대해 넘지 않을 비율과, 왼쪽 에너지 하한이다.
constexpr double MaximumMidToLeft = 0.01;
constexpr double MinimumLeftEnergy = 1.0;

// Layer III frame당 채널 표본 수다.
constexpr uint64 Mpeg1FrameSamples = 1152;
constexpr uint64 Mpeg2FrameSamples = 576;

// 시험 MP3의 rate·frame 수다.
constexpr uint32 Rate44100 = 44100;
constexpr uint32 Rate16000 = 16000;
constexpr uint64 MonoFrames = sourcereference::Mp3MonoLameFrames;
constexpr uint64 MonoLameAudioFrames = 11;
constexpr uint64 StereoAudioFrames = 5;
constexpr uint64 Mpeg2MonoAudioFrames = 11;

// Mp3MonoLame의 LAME tag 값: delay 576 + 디코더 지연 529다. 앞 ID3v2 크기는 머리 10 + 본문 101이다.
constexpr uint64 MonoLameSkip = 576 + 529;
constexpr usize MonoId3v2Bytes = 111;

// x86-64(SSE2)에서 Mp3MonoLame 디코드 결과(planar float32 byte)의 SHA-256이다.
constexpr const char* MonoLameDigestX64 = sourcereference::Mp3MonoLameDigestX64;

// MPEG-1 Layer III 머리 해석에 쓰는 값이다(시험 입력의 frame 위치 계산용).
constexpr uint32 Mpeg1BitratesKbps[16] = {0,   32,  40,  48,  56,  64,  80,  96,
                                          112, 128, 160, 192, 224, 256, 320, 0};
constexpr uint32 Mpeg1Rates[3] = {44100, 48000, 32000};
constexpr uint32 Mpeg1Layer3BytesFactor = 144;
constexpr uint32 BitsPerKilobit = 1000;
constexpr usize HeaderBytes = 4;
constexpr usize CrcBytes = 2;

// LAME 확장에서 delay·padding 3 byte가 있는 위치(확장 시작 + 21)다.
constexpr usize LameDelayPaddingOffset = 21;

Span<const uint8> Bytes(Span<const uint8> data) noexcept {
    return data;
}

// MPEG-1 Layer III frame 크기(byte)다.
usize Mpeg1FrameBytes(const uint8* header) noexcept {
    const uint32 bitrate = Mpeg1BitratesKbps[header[2] >> 4] * BitsPerKilobit;
    const uint32 rate = Mpeg1Rates[(header[2] >> 2) & 3];
    return Mpeg1Layer3BytesFactor * bitrate / rate + ((header[2] >> 1) & 1);
}

// begin에서 시작해 index번째 MPEG-1 frame의 시작 위치다.
usize Mpeg1FrameOffset(Span<const uint8> file, usize begin, uint32 index) noexcept {
    usize offset = begin;
    for (uint32 frame = 0; frame < index; ++frame) {
        offset += Mpeg1FrameBytes(file.Data() + offset);
    }

    return offset;
}

// 두 입력을 이어 붙인다.
Array<uint8> Join(Allocator& allocator, Span<const uint8> first,
                  Span<const uint8> second) noexcept {
    Array<uint8> joined(allocator);
    if (!joined.TryAppend(first) || !joined.TryAppend(second)) {
        joined.Clear();
    }

    return joined;
}

// Info tag 위치를 찾는다. 없으면 file.Size()다.
usize FindInfoTag(Span<const uint8> file) noexcept {
    for (usize index = 0; index + 4 <= file.Size(); ++index) {
        if (file[index] == 'I' && file[index + 1] == 'n' && file[index + 2] == 'f' &&
            file[index + 3] == 'o') {
            return index;
        }
    }

    return file.Size();
}

// Info tag에서 LAME 확장 시작 위치다(flags에 따라 frame 수 4, byte 수 4, TOC 100, scale 4가 앞에
// 있다).
usize LameExtensionOffset(Span<const uint8> file, usize tag) noexcept {
    const uint8 flags = file[tag + 7];
    usize offset = tag + 8;
    offset += (flags & 1) != 0 ? 4 : 0;
    offset += (flags & 2) != 0 ? 4 : 0;
    offset += (flags & 4) != 0 ? 100 : 0;
    offset += (flags & 8) != 0 ? 4 : 0;
    return offset;
}

ErrorCode Mp3Error(Span<const uint8> file, Allocator& allocator) noexcept {
    DecodedSourceAudio audio(allocator);
    return DecodeMp3(file, audio).Error();
}

} // namespace

TEST(Audio_Mp3_MonoLameGapless) {
    SystemAllocator allocator;
    DecodedSourceAudio audio(allocator);
    REQUIRE(DecodeMp3(Bytes(Mp3MonoLame), audio));
    // ID3v2·ID3v1을 건너뛰고, Xing/LAME tag frame은 디코드하지 않고, delay·padding을 잘라 원본
    // 길이와 같다.
    CHECK(audio.ChannelCount() == 1 && audio.SampleRate() == Rate44100);
    CHECK(audio.FrameCount() == MonoFrames);
    CHECK(audio.Codec() == SourceCodec::Mp3 && audio.SampleFormat() == SourceSampleFormat::Lossy);
    CHECK(audio.Loops().Size() == 0 && audio.Cues().Size() == 0);

    // 원본 sine과 표본 위치가 맞아야 SNR이 높다(앞 delay를 덜 자르거나 더 자르면 위상이 어긋나
    // 낮아진다).
    double signal = 0.0;
    double noise = 0.0;
    const float* samples = audio.Channel(0);
    for (uint64 index = 0; index < audio.FrameCount(); ++index) {
        const double reference = SineAmplitude * math::Sin(TwoPi * SineFrequency *
                                                           static_cast<double>(index) / Rate44100);
        const double difference = static_cast<double>(samples[index]) - reference;
        signal += reference * reference;
        noise += difference * difference;
    }

    REQUIRE(noise > 0.0);
    CHECK(signal / noise >= MinimumSignalToNoise);
}

TEST(Audio_Mp3_DecodeHashFixed) {
    // 디코드 결과는 아키텍처마다 고정이다(SIMD를 빌드 때 고르고 런타임 CPU 탐지가 없다 — 3.10.1절).
    // 기기가 바뀌어도 같은 원본이 같은 bulk를 내는지 float bit의 SHA-256으로 단언한다.
    SystemAllocator allocator;
    DecodedSourceAudio audio(allocator);
    REQUIRE(DecodeMp3(Bytes(Mp3MonoLame), audio));
    const Sha256Digest digest =
        Sha256::Hash(Span<const uint8>(reinterpret_cast<const uint8*>(audio.Channel(0)),
                                       static_cast<usize>(audio.FrameCount()) * sizeof(float)));
    char hex[Sha256Digest::HexBytes + 1];
    digest.ToHex(hex);
#if ROCKS_ARCH_X64
    CHECK(StringView(hex) == StringView(MonoLameDigestX64));
#else
    // ARM64(NEON) 고정값은 macOS 기기에서 잰 뒤 넣는다(3.26.3절). 그때까지는 두 번 디코드한 결과가
    // 같은지만 본다.
    DecodedSourceAudio again(allocator);
    REQUIRE(DecodeMp3(Bytes(Mp3MonoLame), again));
    const Sha256Digest repeated =
        Sha256::Hash(Span<const uint8>(reinterpret_cast<const uint8*>(again.Channel(0)),
                                       static_cast<usize>(again.FrameCount()) * sizeof(float)));
    CHECK(repeated == digest);
#endif
}

TEST(Audio_Mp3_StereoAndMpeg2WithoutTag) {
    SystemAllocator allocator;
    DecodedSourceAudio audio(allocator);
    REQUIRE(DecodeMp3(Bytes(Mp3StereoNoTag), audio));
    // tag가 없으면 자르지 않는다: frame 수 × 1152.
    CHECK(audio.ChannelCount() == 2 && audio.SampleRate() == Rate44100);
    CHECK(audio.FrameCount() == StereoAudioFrames * Mpeg1FrameSamples);
    // 오른쪽은 왼쪽의 부호 반전이다.
    double left = 0.0;
    double sum = 0.0;
    for (uint64 index = 0; index < audio.FrameCount(); ++index) {
        const double l = audio.Channel(0)[index];
        const double r = audio.Channel(1)[index];
        left += l * l;
        sum += (l + r) * (l + r);
    }

    CHECK(left > MinimumLeftEnergy);
    CHECK(sum < left * MaximumMidToLeft);

    // MPEG-2(16000 Hz)는 frame당 576 표본이다.
    REQUIRE(DecodeMp3(Bytes(Mp3Mpeg2Mono16k), audio));
    CHECK(audio.ChannelCount() == 1 && audio.SampleRate() == Rate16000);
    CHECK(audio.FrameCount() == Mpeg2MonoAudioFrames * Mpeg2FrameSamples);
}

TEST(Audio_Mp3_TagVariants) {
    SystemAllocator allocator;
    DecodedSourceAudio audio(allocator);
    Array<uint8> file(allocator);
    REQUIRE(file.TryAppend(Bytes(Mp3MonoLame)));
    const usize tag = FindInfoTag(file.View());
    REQUIRE(tag < file.Size());
    const usize extension = LameExtensionOffset(file.View(), tag);
    const usize delayPadding = extension + LameDelayPaddingOffset;
    const uint64 decoded = MonoLameAudioFrames * Mpeg1FrameSamples;

    // padding이 디코더 지연 529보다 작으면 0으로 본다: 앞 delay만 자른다.
    file[delayPadding + 1] = static_cast<uint8>(file[delayPadding + 1] & 0xF0);
    file[delayPadding + 2] = 100;
    REQUIRE(DecodeMp3(file.View(), audio));
    CHECK(audio.FrameCount() == decoded - MonoLameSkip);

    // LAME 확장이 없으면(첫 byte 0) delay·padding이 0이다: tag frame만 빼고 자르지 않는다.
    file[extension] = 0;
    REQUIRE(DecodeMp3(file.View(), audio));
    CHECK(audio.FrameCount() == decoded);

    // frame 수 flag가 없는 tag(−1)도 tag frame은 빼고 자르지 않는다.
    file[tag + 7] = 0;
    REQUIRE(DecodeMp3(file.View(), audio));
    CHECK(audio.FrameCount() == decoded);
}

TEST(Audio_Mp3_TruncatedKeepsGaplessStart) {
    // tag frame + audio frame 6개로 자른 파일: 길이 = min(디코드 − delay, tag 길이)라 앞만 자르고
    // 끝은 남긴다.
    SystemAllocator allocator;
    DecodedSourceAudio audio(allocator);
    const Span<const uint8> file = Bytes(Mp3MonoLame);
    constexpr uint32 KeptAudioFrames = 6;
    const usize end = Mpeg1FrameOffset(file, MonoId3v2Bytes, KeptAudioFrames + 1);
    REQUIRE(DecodeMp3(file.Left(end), audio));
    CHECK(audio.FrameCount() == KeptAudioFrames * Mpeg1FrameSamples - MonoLameSkip);
}

TEST(Audio_Mp3_SilenceForUndecodableFrame) {
    // 세 번째 frame의 main_data_begin을 최대(511)로 키우면 bit reservoir가 모자라 표본을 내지
    // 못한다. 길이를 지키려고 그 frame 자리는 무음이다.
    SystemAllocator allocator;
    Array<uint8> file(allocator);
    REQUIRE(file.TryAppend(Bytes(Mp3StereoNoTag)));
    constexpr uint32 BrokenFrame = 2;
    const usize frame = Mpeg1FrameOffset(file.View(), 0, BrokenFrame);
    const bool hasCrc = (file[frame + 1] & 1) == 0;
    const usize sideInfo = frame + HeaderBytes + (hasCrc ? CrcBytes : 0);
    file[sideInfo] = 0xFF;
    file[sideInfo + 1] = static_cast<uint8>(file[sideInfo + 1] | 0x80);

    DecodedSourceAudio audio(allocator);
    REQUIRE(DecodeMp3(file.View(), audio));
    CHECK(audio.FrameCount() == StereoAudioFrames * Mpeg1FrameSamples);
    bool silent = true;
    for (uint32 channel = 0; channel < audio.ChannelCount(); ++channel) {
        for (uint64 index = BrokenFrame * Mpeg1FrameSamples;
             index < (BrokenFrame + 1) * Mpeg1FrameSamples; ++index) {
            silent = silent && audio.Channel(channel)[index] == 0.0f;
        }
    }

    CHECK(silent);
}

TEST(Audio_Mp3_Rejects) {
    SystemAllocator allocator;
    // Layer II, 중간에 rate·채널이 바뀌는 stream은 Unsupported.
    CHECK(Mp3Error(Bytes(Mp2Layer2), allocator) == ErrorCode::Unsupported);
    const Array<uint8> rateChange = Join(allocator, Bytes(Mp3Mpeg2Mono16k), Bytes(Mp3Mpeg2Mono22k));
    REQUIRE(rateChange.Size() > 0);
    CHECK(Mp3Error(rateChange.View(), allocator) == ErrorCode::Unsupported);
    const Array<uint8> channelChange = Join(allocator, Bytes(Mp3StereoNoTag), Bytes(Mp3MonoNoTag));
    REQUIRE(channelChange.Size() > 0);
    CHECK(Mp3Error(channelChange.View(), allocator) == ErrorCode::Unsupported);

    // frame이 없으면 InvalidArgument: 빈 입력, 0 byte 묶음, ID3v2 tag만, 머리 4 byte만.
    CHECK(Mp3Error(Span<const uint8>(), allocator) == ErrorCode::InvalidArgument);
    const uint8 zeros[4096] = {};
    CHECK(Mp3Error(Bytes(zeros), allocator) == ErrorCode::InvalidArgument);
    CHECK(Mp3Error(Span<const uint8>(Mp3MonoLame, MonoId3v2Bytes), allocator) ==
          ErrorCode::InvalidArgument);
    CHECK(Mp3Error(Span<const uint8>(Mp3StereoNoTag, HeaderBytes), allocator) ==
          ErrorCode::InvalidArgument);

    // 실패하면 앞 결과를 비운다.
    DecodedSourceAudio audio(allocator);
    REQUIRE(DecodeMp3(Bytes(Mp3MonoLame), audio));
    CHECK(!DecodeMp3(Bytes(Mp2Layer2), audio));
    CHECK(audio.FrameCount() == 0 && audio.ChannelCount() == 0 && audio.SampleRate() == 0);
    CHECK(audio.Codec() == SourceCodec::Pcm && audio.SampleFormat() == SourceSampleFormat::Int16);
}

TEST(Audio_Mp3_CapacityBeforeDecode) {
    // MPEG-2.5 8 kHz 8 kbps mono frame(72 byte, 576 표본)을 PCM 상한(float32 2 GiB = 536,870,912
    // 표본)을 넘게 이어 붙인다(약 67 MB). 1차 훑기(머리만)로 CapacityExceeded이고 디코드 버퍼를
    // 잡지 않는다.
    constexpr usize FrameBytes = 72;
    constexpr uint8 Header[HeaderBytes] = {0xFF, 0xE3, 0x18, 0xC0};
    const uint64 limitFrames = MaxSoundClipPcmBytes / sizeof(float);
    const usize frames = static_cast<usize>(limitFrames / Mpeg2FrameSamples + 1);
    SystemAllocator allocator;
    Array<uint8> file(allocator);
    REQUIRE(file.TryResize(frames * FrameBytes));
    for (usize frame = 0; frame < frames; ++frame) {
        uint8* bytes = file.Data() + frame * FrameBytes;
        for (usize index = 0; index < FrameBytes; ++index) {
            bytes[index] = index < HeaderBytes ? Header[index] : 0;
        }
    }

    CHECK(Mp3Error(file.View(), allocator) == ErrorCode::CapacityExceeded);
}

TEST(Audio_Mp3_TruncatedAndCorruptedInputs) {
    SystemAllocator allocator;
    DecodedSourceAudio audio(allocator);
    // 모든 앞부분 길이(37 byte 간격): 받으면 길이가 원본 이하, 아니면 InvalidArgument다.
    constexpr usize PrefixStep = 37;
    for (usize size = 0; size <= sizeof(Mp3MonoLame); size += PrefixStep) {
        const Result<void> decoded = DecodeMp3(Span<const uint8>(Mp3MonoLame, size), audio);
        if (decoded) {
            CHECK(audio.FrameCount() > 0 && audio.FrameCount() <= MonoFrames);
        } else {
            CHECK(decoded.Error() == ErrorCode::InvalidArgument);
        }
    }

    // byte를 무작위로 바꾼 입력: 멈추거나 범위 밖을 읽지 않고, 받으면 표본이 모두 유한하다.
    Array<uint8> mutated(allocator);
    REQUIRE(mutated.TryAppend(Bytes(Mp3MonoLame)));
    RandomStream random(0x4D503321);
    constexpr uint32 Rounds = 200;
    constexpr uint32 FlipsPerRound = 8;
    for (uint32 round = 0; round < Rounds; ++round) {
        for (usize index = 0; index < mutated.Size(); ++index) {
            mutated[index] = Mp3MonoLame[index];
        }

        for (uint32 flip = 0; flip < FlipsPerRound; ++flip) {
            const usize at =
                static_cast<usize>(random.NextUInt32Below(static_cast<uint32>(mutated.Size())));
            mutated[at] = static_cast<uint8>(random.NextUInt32());
        }

        const Result<void> decoded = DecodeMp3(mutated.View(), audio);
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
                  decoded.Error() == ErrorCode::Unsupported);
        }
    }
}

} // namespace mp3_tests_file
