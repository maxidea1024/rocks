#include <AudioTools/CookResampler.h>
#include <AudioTools/SourceAudio.h>
#include <Base/Compiler.h>
#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Hash/Sha256.h>
#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace cook_resampler_tests_file {

using namespace rocks::audio;

// 쿠킹 리샘플러 시험이다(Docs/Design/AudioSystem 3.31.3절).

namespace {

constexpr double TwoPi = 6.283185307179586;
constexpr uint32 Rate8000 = 8000;
constexpr uint32 Rate44100 = 44100;
constexpr uint32 Rate48000 = 48000;
constexpr uint32 Rate96000 = 96000;
constexpr uint32 Rate192000 = 192000;

// 품질 시험: 1 kHz sine(진폭 0.5), 가장자리를 뺀 구간 SNR ≥ 90 dB(에너지 비 10^9). 가장자리는
// 필터 반폭이다. SRC_SINC_BEST_QUALITY 반폭은 낮은 쪽 rate 기준 약 143 frame이라, 출력 기준
// 256 × ceil(target / source) frame을 뺀다.
constexpr double ToneFrequency = 1000.0;
constexpr double ToneAmplitude = 0.5;
constexpr double MinimumSignalToNoise = 1.0e9;
constexpr uint32 EdgeFrames = 256;

// 대역 제한 시험: 96 kHz의 30 kHz sine을 48 kHz로 바꾸면 에너지가 −80 dB(10^−8) 아래여야 한다.
constexpr double AliasFrequency = 30000.0;
constexpr double MaximumAliasRatio = 1.0e-8;

// 채널 경로 시험: libsamplerate의 채널 수별 경로(1·2·4·6·그 밖)가 같은 신호에 같은 값을 내야 한다.
// 합산 순서가 다를 수 있어 허용 오차를 둔다.
constexpr float ChannelPathTolerance = 1.0e-6f;

// 끝 frame 시험: 원본 끝 뒤는 무음으로 본다. 그러니 DC 입력의 결과는 뒤에 무음을 필터 반폭보다
// 길게 붙인 입력의 결과 앞부분과 같아야 한다(끝 frame이 0으로 잘리면 다르다).
constexpr float DcLevel = 0.5f;
constexpr uint32 TrailingSilenceFrames = 2048;
constexpr uint32 MaxTailInputFrames = 10000;

// 원본 채널마다 다른 주파수의 sine을 담는 planar 배열이다.
class Tone {
public:
    Tone(Allocator& allocator, uint32 channels, uint32 frames, uint32 rate,
         double frequency) noexcept
        : samples_(allocator) {
        if (!samples_.TryResize(static_cast<usize>(channels) * frames)) {
            platform::Fatal("Cook resampler test signal allocation failed");
        }

        view_.channelCount = channels;
        view_.frameCount = frames;
        for (uint32 channel = 0; channel < channels; ++channel) {
            float* data = samples_.Data() + static_cast<usize>(channel) * frames;
            view_.channels[channel] = data;
            const double channelFrequency = frequency * (channel + 1);
            for (uint32 frame = 0; frame < frames; ++frame) {
                data[frame] = static_cast<float>(
                    ToneAmplitude * math::Sin(TwoPi * channelFrequency * frame / rate));
            }
        }
    }

    const PlanarConstView& View() const noexcept {
        return view_;
    }

private:
    Array<float> samples_;
    PlanarConstView view_;
};

// 모든 채널이 같은 배열을 가리키는 view다(채널 경로 비교·용량 시험용).
PlanarConstView SharedChannels(const float* data, uint32 channels, uint32 frames) noexcept {
    PlanarConstView view;
    view.channelCount = channels;
    view.frameCount = frames;
    for (uint32 channel = 0; channel < channels; ++channel) {
        view.channels[channel] = data;
    }

    return view;
}

// source → target 출력의 가장자리 frame 수다.
uint32 EdgeFor(uint32 source, uint32 target) noexcept {
    return target > source ? EdgeFrames * ((target + source - 1) / source) : EdgeFrames;
}

// 채널 channel 출력과 해석적 sine(frequency × (channel + 1))의 SNR 에너지 비다(가장자리 제외).
double ToneSignalToNoise(const DecodedAudioPcm& pcm, uint32 channel, double frequency,
                         uint32 edge) noexcept {
    double signal = 0.0;
    double noise = 0.0;
    const double channelFrequency = frequency * (channel + 1);
    for (uint64 frame = edge; frame + edge < pcm.FrameCount(); ++frame) {
        const double reference =
            ToneAmplitude * math::Sin(TwoPi * channelFrequency * frame / pcm.SampleRate());
        const double difference = pcm.Channel(channel)[frame] - reference;
        signal += reference * reference;
        noise += difference * difference;
    }

    return noise > 0.0 ? signal / noise : MinimumSignalToNoise * MinimumSignalToNoise;
}

ErrorCode ResampleError(const PlanarConstView& in, uint32 source, uint32 target,
                        Allocator& allocator) noexcept {
    DecodedAudioPcm out(allocator);
    return ResampleForCooking(in, source, target, allocator, out).Error();
}

uint64 Frames(uint64 frames, uint32 source, uint32 target) noexcept {
    auto converted = CookResampledFrames(frames, source, target);
    return converted ? converted.Value() : ~uint64{0};
}

uint64 Position(uint64 position, uint32 source, uint32 target) noexcept {
    auto converted = CookResampledPosition(position, source, target);
    return converted ? converted.Value() : ~uint64{0};
}

} // namespace

TEST(Audio_CookResampler_FrameCountsAndPositions) {
    // round-half-even(frames × target / source).
    CHECK(Frames(44100, Rate44100, Rate48000) == 48000);
    CHECK(Frames(1, Rate44100, Rate48000) == 1);
    CHECK(Frames(1, Rate96000, Rate8000) == 1);    // 0.083 → 0 → 1
    CHECK(Frames(3, Rate8000, 12000) == 4);        // 4.5 → 짝수 4
    CHECK(Frames(5, Rate8000, 12000) == 8);        // 7.5 → 짝수 8
    CHECK(Frames(1000, 22050, Rate48000) == 2177); // 2176.87
    CHECK(Position(0, Rate44100, Rate48000) == 0);
    CHECK(Position(147, Rate44100, Rate48000) == 160);
    CHECK(Position(1, Rate8000, 12000) == 2); // 1.5 → 짝수 2
    CHECK(Position(uint64{1} << 32, Rate8000, Rate192000) == (uint64{1} << 32) * 24);

    // 거부: rate 범위 밖, 2^32 초과.
    CHECK(CookResampledFrames(1, 7999, Rate48000).Error() == ErrorCode::InvalidArgument);
    CHECK(CookResampledFrames(1, Rate44100, 192001).Error() == ErrorCode::InvalidArgument);
    CHECK(CookResampledFrames((uint64{1} << 32) + 1, Rate44100, Rate48000).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(CookResampledPosition(0, 0, Rate48000).Error() == ErrorCode::InvalidArgument);
    CHECK(CookResampledPosition(~uint64{0}, Rate44100, Rate48000).Error() ==
          ErrorCode::InvalidArgument);
}

TEST(Audio_CookResampler_QualityAndAlignment) {
    // 올림·내림 모두, 블록(4096 frame) 여러 개를 거친다.
    SystemAllocator allocator;
    for (const uint32 source : {Rate8000, 22050u, 32000u, Rate44100, Rate96000, Rate192000}) {
        const Tone tone(allocator, 2, source, source, ToneFrequency);
        DecodedAudioPcm out(allocator);
        REQUIRE(ResampleForCooking(tone.View(), source, Rate48000, allocator, out));
        CHECK(out.ChannelCount() == 2 && out.SampleRate() == Rate48000);
        CHECK(out.FrameCount() == Rate48000);
        // 위상까지 맞아야 SNR이 높다(출력 frame 0 = 원본 시각 0). 채널마다 주파수가 달라 섞이면
        // 낮아진다.
        const uint32 edge = EdgeFor(source, Rate48000);
        CHECK(ToneSignalToNoise(out, 0, ToneFrequency, edge) >= MinimumSignalToNoise);
        CHECK(ToneSignalToNoise(out, 1, ToneFrequency, edge) >= MinimumSignalToNoise);
    }

    // 48 kHz가 아닌 대상도 된다.
    const Tone tone(allocator, 1, Rate48000, Rate48000, ToneFrequency);
    DecodedAudioPcm out(allocator);
    REQUIRE(ResampleForCooking(tone.View(), Rate48000, Rate44100, allocator, out));
    CHECK(out.FrameCount() == Rate44100 && out.SampleRate() == Rate44100);
    CHECK(ToneSignalToNoise(out, 0, ToneFrequency, EdgeFrames) >= MinimumSignalToNoise);
}

TEST(Audio_CookResampler_BandLimited) {
    SystemAllocator allocator;
    const Tone tone(allocator, 1, Rate96000, Rate96000, AliasFrequency);
    DecodedAudioPcm out(allocator);
    REQUIRE(ResampleForCooking(tone.View(), Rate96000, Rate48000, allocator, out));
    double input = 0.0;
    for (uint32 frame = 0; frame < tone.View().frameCount; ++frame) {
        input +=
            static_cast<double>(tone.View().channels[0][frame]) * tone.View().channels[0][frame];
    }

    double output = 0.0;
    for (uint64 frame = EdgeFrames; frame + EdgeFrames < out.FrameCount(); ++frame) {
        output += static_cast<double>(out.Channel(0)[frame]) * out.Channel(0)[frame];
    }

    // 입력 에너지를 출력 frame 수로 맞춰 비교한다.
    const double scaledInput = input * (out.FrameCount() - 2 * EdgeFrames) / tone.View().frameCount;
    CHECK(output < scaledInput * MaximumAliasRatio);
}

TEST(Audio_CookResampler_TailAndChannelPaths) {
    SystemAllocator allocator;
    // DC 신호의 마지막 frame까지 "끝 뒤 무음" 기준 값이 나온다(0으로 잘리지 않는다). 채널 수와
    // 길이(블록 경계 앞뒤)에 상관없다.
    Array<float> padded(allocator);
    REQUIRE(padded.TryResize(MaxTailInputFrames + TrailingSilenceFrames));
    for (const uint32 frames : {1u, 2u, 441u, 4095u, 4096u, 4097u, MaxTailInputFrames}) {
        for (usize index = 0; index < padded.Size(); ++index) {
            padded[index] = index < frames ? DcLevel : 0.0f;
        }

        for (const uint32 source : {Rate8000, Rate44100, Rate192000}) {
            DecodedAudioPcm reference(allocator);
            REQUIRE(
                ResampleForCooking(SharedChannels(padded.Data(), 1, frames + TrailingSilenceFrames),
                                   source, Rate48000, allocator, reference));
            for (uint32 channels = 1; channels <= MaxOutputChannels; ++channels) {
                DecodedAudioPcm out(allocator);
                REQUIRE(ResampleForCooking(SharedChannels(padded.Data(), channels, frames), source,
                                           Rate48000, allocator, out));
                REQUIRE(out.FrameCount() == Frames(frames, source, Rate48000));
                bool same = true;
                for (uint32 channel = 0; channel < channels; ++channel) {
                    same = same && out.Channel(channel)[out.FrameCount() - 1] != 0.0f;
                    for (uint64 frame = 0; frame < out.FrameCount(); ++frame) {
                        same =
                            same && math::Abs(out.Channel(channel)[frame] -
                                              reference.Channel(0)[frame]) <= ChannelPathTolerance;
                    }
                }

                CHECK(same);
            }
        }
    }

    // 채널 수별 경로가 같은 신호에 같은 결과를 낸다(채널 1과 비교).
    const Tone tone(allocator, 1, 9000, Rate44100, ToneFrequency);
    DecodedAudioPcm mono(allocator);
    REQUIRE(ResampleForCooking(tone.View(), Rate44100, Rate48000, allocator, mono));
    for (uint32 channels = 2; channels <= MaxOutputChannels; ++channels) {
        DecodedAudioPcm out(allocator);
        REQUIRE(ResampleForCooking(SharedChannels(tone.View().channels[0], channels, 9000),
                                   Rate44100, Rate48000, allocator, out));
        REQUIRE(out.FrameCount() == mono.FrameCount());
        bool same = true;
        for (uint32 channel = 0; channel < channels; ++channel) {
            for (uint64 frame = 0; frame < out.FrameCount(); ++frame) {
                same = same && math::Abs(out.Channel(channel)[frame] - mono.Channel(0)[frame]) <=
                                   ChannelPathTolerance;
            }
        }

        CHECK(same);
    }
}

TEST(Audio_CookResampler_SameRateCopiesAndEdges) {
    SystemAllocator allocator;
    const Tone tone(allocator, 2, 1000, Rate48000, ToneFrequency);
    DecodedAudioPcm out(allocator);
    REQUIRE(ResampleForCooking(tone.View(), Rate48000, Rate48000, allocator, out));
    REQUIRE(out.FrameCount() == 1000);
    bool same = true;
    for (uint32 channel = 0; channel < 2; ++channel) {
        same = same && platform::MemoryCompare(out.Channel(channel), tone.View().channels[channel],
                                               1000 * sizeof(float)) == 0;
    }

    CHECK(same);

    // 1 frame 입력도 1 frame을 낸다(크게 내릴 때도).
    const float single[1] = {DcLevel};
    for (const uint32 source : {Rate44100, Rate192000}) {
        REQUIRE(ResampleForCooking(SharedChannels(single, 1, 1), source, Rate8000, allocator, out));
        CHECK(out.FrameCount() == 1 && math::IsFinite(out.Channel(0)[0]) &&
              out.Channel(0)[0] != 0.0f);
    }

    // 거부: rate 범위 밖, 채널 0·9, frame 0, 비유한 표본.
    CHECK(ResampleError(tone.View(), 7999, Rate48000, allocator) == ErrorCode::InvalidArgument);
    CHECK(ResampleError(tone.View(), Rate44100, 192001, allocator) == ErrorCode::InvalidArgument);
    PlanarConstView empty = tone.View();
    empty.channelCount = 0;
    CHECK(ResampleError(empty, Rate44100, Rate48000, allocator) == ErrorCode::InvalidArgument);
    CHECK(ResampleError(SharedChannels(single, MaxOutputChannels + 1, 1), Rate44100, Rate48000,
                        allocator) == ErrorCode::InvalidArgument);
    PlanarConstView noFrames = tone.View();
    noFrames.frameCount = 0;
    CHECK(ResampleError(noFrames, Rate44100, Rate48000, allocator) == ErrorCode::InvalidArgument);
    Array<float> data(allocator);
    REQUIRE(data.TryResize(16));
    data[5] = platform::BitCast<float>(0x7FC00000u);
    CHECK(ResampleError(SharedChannels(data.Data(), 1, 16), Rate44100, Rate48000, allocator) ==
          ErrorCode::InvalidArgument);
    // 실패하면 out을 비운다.
    CHECK(!ResampleForCooking(SharedChannels(data.Data(), 1, 16), Rate44100, Rate48000, allocator,
                              out) &&
          out.FrameCount() == 0);

    // in이 out의 기존 버퍼를 가리키면 거부하고 out은 그대로 둔다.
    REQUIRE(ResampleForCooking(tone.View(), Rate48000, Rate48000, allocator, out));
    PlanarConstView aliased = SharedChannels(out.Channel(1), 1, 100);
    CHECK(ResampleForCooking(aliased, Rate48000, Rate44100, allocator, out).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(out.FrameCount() == 1000 &&
          platform::MemoryCompare(out.Channel(1), tone.View().channels[1], 1000 * sizeof(float)) ==
              0);
}

TEST(Audio_CookResampler_CapacityLimit) {
    // 8000 → 192000, 8채널 2,800,000 frame이면 결과가 67,200,000 frame × 32 B > 2 GiB다. 할당 전에
    // CapacityExceeded로 거부한다(입력은 채널끼리 한 배열을 같이 쓴다).
    SystemAllocator allocator;
    constexpr uint32 Frames = 2800000;
    static_assert(uint64{Frames} * (Rate192000 / Rate8000) * MaxOutputChannels * sizeof(float) >
                      MaxSoundClipPcmBytes,
                  "capacity test input must exceed the PCM limit");
    Array<float> data(allocator);
    REQUIRE(data.TryResize(Frames));
    platform::MemoryZero(data.Data(), static_cast<usize>(Frames) * sizeof(float));
    DecodedAudioPcm out(allocator);
    CHECK(ResampleForCooking(SharedChannels(data.Data(), MaxOutputChannels, Frames), Rate8000,
                             Rate192000, allocator, out)
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(out.FrameCount() == 0);
}

TEST(Audio_CookResampler_DeterministicGolden) {
    // 정수로 만든 톱니파(libm 없음)를 44100 → 48000으로 바꾼 결과의 SHA-256이다. x86-64 값을
    // 단언하고, ARM64는 두 번 같은지만 본다(3.31.3절).
    SystemAllocator allocator;
    constexpr uint32 Frames = 4410;
    constexpr uint32 SawPeriod = 1000;
    constexpr uint32 SawStep = 37;
    Array<float> data(allocator);
    REQUIRE(data.TryResize(Frames));
    for (uint32 frame = 0; frame < Frames; ++frame) {
        const int32 phase =
            static_cast<int32>(frame * SawStep % SawPeriod) - static_cast<int32>(SawPeriod / 2);
        data[frame] = static_cast<float>(phase) / static_cast<float>(SawPeriod);
    }

    const PlanarConstView in = SharedChannels(data.Data(), 1, Frames);
    DecodedAudioPcm first(allocator);
    DecodedAudioPcm second(allocator);
    REQUIRE(ResampleForCooking(in, Rate44100, Rate48000, allocator, first));
    REQUIRE(ResampleForCooking(in, Rate44100, Rate48000, allocator, second));
    const Span<const uint8> firstBytes(reinterpret_cast<const uint8*>(first.Channel(0)),
                                       static_cast<usize>(first.FrameCount()) * sizeof(float));
    const Span<const uint8> secondBytes(reinterpret_cast<const uint8*>(second.Channel(0)),
                                        static_cast<usize>(second.FrameCount()) * sizeof(float));
    CHECK(Sha256::Hash(firstBytes) == Sha256::Hash(secondBytes));
#if ROCKS_ARCH_X64
    char hex[Sha256Digest::HexBytes + 1];
    Sha256::Hash(firstBytes).ToHex(hex);
    CHECK(StringView(hex) ==
          StringView("d9c916815ba2d2b92c54b9417c6f8c6462d79dbef9540953629306fc84660073"));
#endif
}

} // namespace cook_resampler_tests_file
