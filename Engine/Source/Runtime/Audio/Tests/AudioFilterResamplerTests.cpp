#include "AudioTestSupport.h"

#include <Audio/LinearResampler.h>
#include <Audio/OnePoleFilter.h>
#include <Base/InitializerList.h>
#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/PlatformMath.h>
#include <TestSupport/MathTestSupport.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace audio_filter_resampler_tests_file {

using namespace rocks::audio;
using namespace rocks::audio::audiotest;
using platform::PlatformMath;

// Audio DSP1의 1극 필터·선형 리샘플러
// 시험이다(Docs/Design/AudioSystem.ko.md 3.20.4–3.20.5절, 3.20.8절).

namespace {

constexpr uint32 Rate = DefaultMixSampleRate;

OnePoleLowPass MakeLowPass(uint32 channels) noexcept {
    auto created = OnePoleLowPass::TryCreate(Rate, channels);
    if (!created) {
        platform::Fatal("Audio test low-pass was rejected");
    }

    return created.Value();
}

OnePoleHighPass MakeHighPass(uint32 channels) noexcept {
    auto created = OnePoleHighPass::TryCreate(Rate, channels);
    if (!created) {
        platform::Fatal("Audio test high-pass was rejected");
    }

    return created.Value();
}

LinearResampler MakeResampler(uint32 channels, double ratio) noexcept {
    auto created = LinearResampler::TryCreate(channels, ratio);
    if (!created) {
        platform::Fatal("Audio test resampler was rejected");
    }

    return created.Value();
}

// 정답 계수다(UE 식을 double로 독립 계산).
double ReferenceLowPassB1(double cutoff, double rate) noexcept {
    return PlatformMath::Exp(-math::Pi<double> * math::Clamp(2.0 * cutoff / rate, 0.0, 1.0));
}

double ReferenceHighPassA0(double cutoff, double rate) noexcept {
    const double clamped = math::Clamp(cutoff, 0.0, 0.5 * rate - 1.0);
    const double g = PlatformMath::Tan(math::Pi<double> * clamped / rate);
    return g / (1.0 + g);
}

// 교대 ±1(Nyquist) 신호다.
void FillNyquist(const PlanarView& view) noexcept {
    for (uint32 channel = 0; channel < view.channelCount; ++channel) {
        for (uint32 frame = 0; frame < view.frameCount; ++frame) {
            view.channels[channel][frame] = frame % 2 == 0 ? 1.0f : -1.0f;
        }
    }
}

// 채널 c의 frame i 값이 i·step + c인 선형 입력이다(작은 정수·이진 소수라 float로 정확하다).
void FillLinear(const PlanarView& view, float step) noexcept {
    for (uint32 channel = 0; channel < view.channelCount; ++channel) {
        for (uint32 frame = 0; frame < view.frameCount; ++frame) {
            view.channels[channel][frame] =
                static_cast<float>(frame) * step + static_cast<float>(channel);
        }
    }
}

// 입력 스트림 전체를 출력 블록 크기 열로 리샘플한다. 매 블록 필요한 만큼만 입력을 주고 소비 수만큼
// 입력 커서를 옮긴다(장치 콜백이 decoder를 당기는 방식과 같다). 실패하면 false.
template <typename NextBlock>
bool Drive(LinearResampler& resampler, const PlanarConstView& input, const PlanarView& output,
           NextBlock nextBlock) noexcept {
    uint32 inputCursor = 0;
    uint32 outputCursor = 0;
    while (outputCursor < output.frameCount) {
        uint32 frames = nextBlock();
        if (frames > output.frameCount - outputCursor) {
            frames = output.frameCount - outputCursor;
        }

        const auto needed = resampler.InputFramesNeeded(frames);
        if (!needed || needed.Value() > input.frameCount - inputCursor) {
            return false;
        }

        const auto consumed = resampler.Process(input.Slice(inputCursor, needed.Value()),
                                                output.Slice(outputCursor, frames));
        if (!consumed) {
            return false;
        }

        inputCursor += consumed.Value();
        outputCursor += frames;
    }

    return true;
}

} // namespace

TEST(Audio_DSP1_OnePoleRejectsInvalidSetup) {
    CHECK(OnePoleLowPass::TryCreate(MinMixSampleRate - 1, 1).Error() == ErrorCode::InvalidArgument);
    CHECK(OnePoleLowPass::TryCreate(Rate, 0).Error() == ErrorCode::InvalidArgument);
    CHECK(OnePoleHighPass::TryCreate(Rate, MaxOutputChannels + 1).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(OnePoleHighPass::TryCreate(MaxMixSampleRate + 1, 2).Error() ==
          ErrorCode::InvalidArgument);

    OnePoleLowPass lowPass = MakeLowPass(1);
    CHECK(lowPass.IsBypassed());
    REQUIRE(lowPass.SetCutoff(1000.0f, 0));
    CHECK(lowPass.SetCutoff(-1.0f, 0).Error() == ErrorCode::InvalidArgument);
    CHECK(lowPass.SetCutoff(PlatformMath::QuietNaN<float>(), 0).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(lowPass.SetCutoff(PlatformMath::Infinity<float>(), 0).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(lowPass.Cutoff() == 1000.0f && !lowPass.IsBypassed());
}

TEST(Audio_DSP1_OnePoleCoefficientsMatchReference) {
    for (const float cutoff :
         {20.0f, 250.0f, 1000.0f, 5000.0f, 12000.0f, 19999.0f, 23999.0f, 30000.0f}) {
        const double lowReference = ReferenceLowPassB1(cutoff, Rate);
        CHECK(test::Near(static_cast<double>(OnePoleLowPass::ComputeCoefficient(cutoff, Rate)),
                         lowReference, 1.0e-7));
        const double highReference = ReferenceHighPassA0(cutoff, Rate);
        CHECK(test::Near(static_cast<double>(OnePoleHighPass::ComputeCoefficient(cutoff, Rate)),
                         highReference, 1.0e-7));
    }

    // 알려진 값: 1 kHz/48 kHz LPF b1 = exp(−π/24), 정규화 1 이상은 exp(−π).
    CHECK(test::Near(OnePoleLowPass::ComputeCoefficient(1000.0f, Rate), 0.877306f, 1.0e-6f));
    CHECK(OnePoleLowPass::ComputeCoefficient(30000.0f, Rate) ==
          static_cast<float>(PlatformMath::Exp(-math::Pi<double>)));
    // HPF 차단 주파수는 fs/2 − 1 Hz로 잘려 a0가 1보다 작다(극이 −1에 놓이지 않는다).
    const float nyquistLimit = OnePoleHighPass::ComputeCoefficient(23999.0f, Rate);
    CHECK(nyquistLimit < 1.0f);
    CHECK(OnePoleHighPass::ComputeCoefficient(24000.0f, Rate) == nyquistLimit);
    CHECK(OnePoleHighPass::ComputeCoefficient(30000.0f, Rate) == nyquistLimit);
}

TEST(Audio_DSP1_OnePoleDcAndNyquistResponse) {
    SystemAllocator allocator;
    PlanarBuffer dc = MakeBuffer(2, 4096, allocator);
    PlanarBuffer nyquist = MakeBuffer(2, 4096, allocator);

    OnePoleLowPass lowPass = MakeLowPass(2);
    REQUIRE(lowPass.SetCutoff(1000.0f, 0));
    Fill(dc.View(), 1.0f);
    lowPass.Process(dc.View());
    CHECK(test::Near(dc.Channel(0)[4095], 1.0f, 1.0e-5f) &&
          test::Near(dc.Channel(1)[4095], 1.0f, 1.0e-5f));
    lowPass.ResetState();
    FillNyquist(nyquist.View());
    lowPass.Process(nyquist.View());
    // 정상 상태 진폭은 (1 − b1)/(1 + b1) ≈ 0.0653이다.
    CHECK(math::Abs(nyquist.Channel(0)[4095]) < 0.07f &&
          math::Abs(nyquist.Channel(0)[4095]) > 0.06f);

    OnePoleHighPass highPass = MakeHighPass(2);
    REQUIRE(highPass.SetCutoff(1000.0f, 0));
    Fill(dc.View(), 1.0f);
    highPass.Process(dc.View());
    CHECK(math::Abs(dc.Channel(1)[4095]) < 1.0e-5f);
    highPass.ResetState();
    FillNyquist(nyquist.View());
    highPass.Process(nyquist.View());
    CHECK(math::Abs(nyquist.Channel(1)[4095]) > 0.9f);
}

TEST(Audio_DSP1_OnePoleCoefficientInterpolationPath) {
    SystemAllocator allocator;
    OnePoleLowPass lowPass = MakeLowPass(1);
    // 첫 설정은 보간 길이가 있어도 즉시다.
    REQUIRE(lowPass.SetCutoff(1000.0f, 64));
    const float start = OnePoleLowPass::ComputeCoefficient(1000.0f, Rate);
    const float target = OnePoleLowPass::ComputeCoefficient(4000.0f, Rate);
    CHECK(lowPass.CurrentCoefficient() == start && lowPass.TargetCoefficient() == start);

    REQUIRE(lowPass.SetCutoff(4000.0f, 4));
    PlanarBuffer buffer = MakeBuffer(1, 1, allocator);
    float z = 0.0f;
    for (uint32 step = 1; step <= 6; ++step) {
        buffer.Channel(0)[0] = 1.0f;
        lowPass.Process(buffer.View());
        const double fraction = step >= 4 ? 1.0 : static_cast<double>(step) / 4.0;
        const float expected = static_cast<float>(
            static_cast<double>(start) +
            (static_cast<double>(target) - static_cast<double>(start)) * fraction);
        CHECK(lowPass.CurrentCoefficient() == expected);
        // 출력은 그 frame의 계수로 계산한 값과 bit 단위로 같다.
        z = 1.0f + expected * (z - 1.0f);
        CHECK(buffer.Channel(0)[0] == z);
    }

    CHECK(lowPass.CurrentCoefficient() == target);
}

TEST(Audio_DSP1_OnePoleHighPassInterpolationAndRetarget) {
    SystemAllocator allocator;
    OnePoleHighPass highPass = MakeHighPass(1);
    REQUIRE(highPass.SetCutoff(100.0f, 0));
    const float start = OnePoleHighPass::ComputeCoefficient(100.0f, Rate);
    const float target = OnePoleHighPass::ComputeCoefficient(2000.0f, Rate);
    REQUIRE(highPass.SetCutoff(2000.0f, 8));

    // 정답: frame k(1부터)의 계수는 start + (target − start)·k/8, 출력은 UE HPF 식이다.
    PlanarBuffer buffer = MakeBuffer(1, 1, allocator);
    float z = 0.0f;
    float lastCoefficient = start;
    for (uint32 step = 1; step <= 4; ++step) {
        const float input = step % 2 == 0 ? 0.5f : -0.25f;
        buffer.Channel(0)[0] = input;
        highPass.Process(buffer.View());
        const float a0 =
            static_cast<float>(static_cast<double>(start) +
                               (static_cast<double>(target) - static_cast<double>(start)) *
                                   (static_cast<double>(step) / 8.0));
        const float v = (input - z) * a0;
        const float lowPass = v + z;
        z = v + lowPass;
        CHECK(buffer.Channel(0)[0] == input - lowPass);
        CHECK(highPass.CurrentCoefficient() == a0);
        lastCoefficient = a0;
    }

    // ramp 중 재설정은 마지막으로 쓴 계수에서 새 ramp를 시작한다.
    const float retarget = OnePoleHighPass::ComputeCoefficient(400.0f, Rate);
    REQUIRE(highPass.SetCutoff(400.0f, 2));
    CHECK(highPass.CurrentCoefficient() == lastCoefficient &&
          highPass.TargetCoefficient() == retarget);
    buffer.Channel(0)[0] = 0.0f;
    highPass.Process(buffer.View());
    const float half = static_cast<float>(
        static_cast<double>(lastCoefficient) +
        (static_cast<double>(retarget) - static_cast<double>(lastCoefficient)) * 0.5);
    CHECK(highPass.CurrentCoefficient() == half);
    highPass.Process(buffer.View());
    CHECK(highPass.CurrentCoefficient() == retarget);

    // LPF도 같은 재설정 규칙이다.
    OnePoleLowPass lowPass = MakeLowPass(1);
    REQUIRE(lowPass.SetCutoff(500.0f, 0));
    REQUIRE(lowPass.SetCutoff(8000.0f, 4));
    lowPass.Process(buffer.View());
    const float lowMid = lowPass.CurrentCoefficient();
    REQUIRE(lowPass.SetCutoff(500.0f, 4));
    CHECK(lowPass.CurrentCoefficient() == lowMid);
}

TEST(Audio_DSP1_OnePoleHighPassStaysBoundedAtNyquistCutoff) {
    // 차단 주파수를 fs/2 이상으로 줘도 상태가 발산하지 않는다(8 kHz에서 5 kHz, 1분 분량 Nyquist
    // 톤).
    SystemAllocator allocator;
    auto created = OnePoleHighPass::TryCreate(MinMixSampleRate, 1);
    REQUIRE(created);
    OnePoleHighPass& highPass = created.Value();
    REQUIRE(highPass.SetCutoff(5000.0f, 0));
    CHECK(highPass.Cutoff() == 0.5f * static_cast<float>(MinMixSampleRate) - HighPassNyquistMargin);
    PlanarBuffer buffer = MakeBuffer(1, MaxBlockFrames, allocator);
    constexpr uint32 Blocks = MinMixSampleRate * 60 / MaxBlockFrames;
    float peak = 0.0f;
    for (uint32 block = 0; block < Blocks; ++block) {
        FillNyquist(buffer.View());
        highPass.Process(buffer.View());
        for (uint32 frame = 0; frame < MaxBlockFrames; ++frame) {
            peak = math::Max(peak, math::Abs(buffer.Channel(0)[frame]));
        }
    }

    // Nyquist는 통과대역이라 진폭 약 1을 유지한다. 상태는 정상값 |a0/(1 − a0)|(약 2,546)에 머물러
    // 자릿수 소실이 그 크기의 float 오차(1e-3 수준)로 한정된다.
    CHECK(peak <= 1.01f);
    CHECK(math::Abs(buffer.Channel(0)[MaxBlockFrames - 1]) > 0.99f);
}

TEST(Audio_DSP1_OnePoleBypassBoundaryAndReenable) {
    SystemAllocator allocator;
    RandomStream random(0xB1);
    PlanarBuffer original = MakeBuffer(2, 256, allocator);
    FillRandom(original.View(), random);
    PlanarBuffer work = MakeBuffer(2, 256, allocator);

    const auto copy = [&]() {
        for (uint32 channel = 0; channel < 2; ++channel) {
            memcpy(work.Channel(channel), original.Channel(channel), sizeof(float) * 256);
        }
    };

    OnePoleLowPass lowPass = MakeLowPass(2);
    REQUIRE(lowPass.SetCutoff(19999.0f, 0));
    CHECK(!lowPass.IsBypassed());
    copy();
    lowPass.Process(work.View());
    CHECK(!BitEqual(work.View(), original.View()));

    REQUIRE(lowPass.SetCutoff(FilterBypassFrequency, 32));
    CHECK(lowPass.IsBypassed());
    copy();
    lowPass.Process(work.View());
    CHECK(BitEqual(work.View(), original.View()));

    // bypass에서 다시 켜면 상태 0·즉시 계수라 새 필터와 같은 출력이다.
    REQUIRE(lowPass.SetCutoff(3000.0f, 32));
    copy();
    lowPass.Process(work.View());
    OnePoleLowPass fresh = MakeLowPass(2);
    REQUIRE(fresh.SetCutoff(3000.0f, 32));
    PlanarBuffer freshOutput = MakeBuffer(2, 256, allocator);
    for (uint32 channel = 0; channel < 2; ++channel) {
        memcpy(freshOutput.Channel(channel), original.Channel(channel), sizeof(float) * 256);
    }

    fresh.Process(freshOutput.View());
    CHECK(BitEqual(work.View(), freshOutput.View()));

    OnePoleHighPass highPass = MakeHighPass(2);
    CHECK(highPass.IsBypassed());
    // HPF bypass는 UE처럼 20 Hz(+ 여유) 이하다.
    REQUIRE(highPass.SetCutoff(FilterMinFrequency, 0));
    CHECK(highPass.IsBypassed());
    REQUIRE(highPass.SetCutoff(5.0f, 0));
    CHECK(highPass.IsBypassed());
    REQUIRE(highPass.SetCutoff(20.01f, 0));
    CHECK(!highPass.IsBypassed());
    // LPF bypass 경계 바로 아래는 처리한다.
    REQUIRE(lowPass.SetCutoff(19999.99f, 0));
    CHECK(!lowPass.IsBypassed());
    copy();
    REQUIRE(highPass.SetCutoff(0.0f, 0));
    highPass.Process(work.View());
    CHECK(BitEqual(work.View(), original.View()));
}

TEST(Audio_DSP1_OnePoleBlockSplitInvariant) {
    SystemAllocator allocator;
    RandomStream random(0xF17E);
    PlanarBuffer input = MakeBuffer(3, MaxBlockFrames, allocator);
    FillRandom(input.View(), random);
    PlanarBuffer whole = MakeBuffer(3, MaxBlockFrames, allocator);
    PlanarBuffer split = MakeBuffer(3, MaxBlockFrames, allocator);

    for (uint32 kind = 0; kind < 2; ++kind) {
        for (uint32 channel = 0; channel < 3; ++channel) {
            memcpy(whole.Channel(channel), input.Channel(channel), sizeof(float) * MaxBlockFrames);
            memcpy(split.Channel(channel), input.Channel(channel), sizeof(float) * MaxBlockFrames);
        }

        OnePoleLowPass lowWhole = MakeLowPass(3);
        OnePoleLowPass lowSplit = MakeLowPass(3);
        OnePoleHighPass highWhole = MakeHighPass(3);
        OnePoleHighPass highSplit = MakeHighPass(3);
        REQUIRE(lowWhole.SetCutoff(800.0f, 0) && lowSplit.SetCutoff(800.0f, 0));
        REQUIRE(lowWhole.SetCutoff(6000.0f, 1500) && lowSplit.SetCutoff(6000.0f, 1500));
        REQUIRE(highWhole.SetCutoff(200.0f, 0) && highSplit.SetCutoff(200.0f, 0));
        REQUIRE(highWhole.SetCutoff(50.0f, 2500) && highSplit.SetCutoff(50.0f, 2500));

        if (kind == 0) {
            lowWhole.Process(whole.View());
        } else {
            highWhole.Process(whole.View());
        }

        uint32 offset = 0;
        while (offset < MaxBlockFrames) {
            uint32 frames = 1 + random.NextUInt32Below(513);
            if (frames > MaxBlockFrames - offset) {
                frames = MaxBlockFrames - offset;
            }

            if (kind == 0) {
                lowSplit.Process(split.View().Slice(offset, frames));
            } else {
                highSplit.Process(split.View().Slice(offset, frames));
            }

            offset += frames;
        }

        CHECK(BitEqual(whole.View(), split.View()));
    }
}

TEST(Audio_DSP1_ResamplerIdentityAtUnitRatio) {
    SystemAllocator allocator;
    RandomStream random(0x1D);
    PlanarBuffer input = MakeBuffer(2, 512, allocator);
    FillRandom(input.View(), random);
    PlanarBuffer output = MakeBuffer(2, 512, allocator);

    LinearResampler resampler = MakeResampler(2, 1.0);
    CHECK(resampler.InputFramesNeeded(512).Value() == 512);
    const auto consumed = resampler.Process(input.View(), output.View());
    REQUIRE(consumed);
    CHECK(consumed.Value() == 512);
    CHECK(BitEqual(output.View(), input.View()));
    CHECK(resampler.Position() == 0.0);
    CHECK(resampler.InputFramesNeeded(0).Value() == 0);
}

TEST(Audio_DSP1_ResamplerHalfAndDoubleRatio) {
    SystemAllocator allocator;
    PlanarBuffer input = MakeBuffer(2, 64, allocator);
    FillLinear(input.View(), 1.0f);
    PlanarBuffer output = MakeBuffer(2, 8, allocator);

    // r = 0.5: 위치 0, 0.5, …, 3.5. 마지막 출력이 x[3]·x[4]를 읽어 5 frame이 필요하고 4 frame을
    // 소비한다.
    LinearResampler half = MakeResampler(2, 0.5);
    CHECK(half.InputFramesNeeded(8).Value() == 5);
    const auto halfConsumed = half.Process(input.View().Slice(0, 5), output.View());
    REQUIRE(halfConsumed);
    CHECK(halfConsumed.Value() == 4 && half.Position() == 0.0);
    for (uint32 frame = 0; frame < 8; ++frame) {
        CHECK(output.Channel(0)[frame] == 0.5f * static_cast<float>(frame));
        CHECK(output.Channel(1)[frame] == 0.5f * static_cast<float>(frame) + 1.0f);
    }

    // r = 2: x[0], x[2], …, x[14]. 끝 위치 16까지 소비한다.
    LinearResampler twice = MakeResampler(2, 2.0);
    CHECK(twice.InputFramesNeeded(8).Value() == 16);
    const auto twiceConsumed = twice.Process(input.View().Slice(0, 16), output.View());
    REQUIRE(twiceConsumed);
    CHECK(twiceConsumed.Value() == 16);
    for (uint32 frame = 0; frame < 8; ++frame) {
        CHECK(output.Channel(0)[frame] == 2.0f * static_cast<float>(frame));
    }

    // r = 0.75에서 블록 경계를 넘을 때 직전 frame(x[−1])을 쓴다: 위치 0, 0.75, 1.5 뒤 끝 위치 2.25
    // → 3 frame 소비, 다음 위치 −0.75.
    LinearResampler threeQuarter = MakeResampler(1, 0.75);
    PlanarBuffer mono = MakeBuffer(1, 8, allocator);
    FillLinear(mono.View(), 1.0f);
    PlanarBuffer monoOut = MakeBuffer(1, 3, allocator);
    const auto first = threeQuarter.Process(mono.View().Slice(0, 3), monoOut.View());
    REQUIRE(first);
    CHECK(first.Value() == 3 && threeQuarter.Position() == -0.75);
    const auto second = threeQuarter.Process(mono.View().Slice(3, 3), monoOut.View().Slice(0, 1));
    REQUIRE(second);
    CHECK(monoOut.Channel(0)[0] == 2.25f);
}

TEST(Audio_DSP1_ResamplerFractionalRatioTracksIdeal) {
    SystemAllocator allocator;
    constexpr uint32 Frames = 4000;
    PlanarBuffer input = MakeBuffer(1, MaxBlockFrames, allocator);
    FillLinear(input.View(), 1.0f / 1024.0f);
    PlanarBuffer output = MakeBuffer(1, Frames, allocator);

    const double ratio = 44100.0 / 48000.0;
    LinearResampler resampler = MakeResampler(1, ratio);
    REQUIRE(Drive(resampler, input.View(), output.View(), []() noexcept { return 480u; }));
    // 선형 입력이라 선형 보간이 정확하다. 차이는 고정소수점 비율 반올림(2^-33 이하/frame)과 float
    // 오차다.
    for (uint32 frame = 0; frame < Frames; ++frame) {
        const double ideal = static_cast<double>(frame) * ratio / 1024.0;
        CHECK(test::Near(static_cast<double>(output.Channel(0)[frame]), ideal, 1.0e-6, 1.0e-6));
    }
}

TEST(Audio_DSP1_ResamplerBlockSplitInvariant) {
    SystemAllocator allocator;
    RandomStream random(0x5EED);
    constexpr uint32 InputFrames = MaxBlockFrames;
    constexpr uint32 OutputFrames = 2000;
    PlanarBuffer input = MakeBuffer(2, InputFrames, allocator);
    FillRandom(input.View(), random);

    for (const double ratio : {0.37, 44100.0 / 48000.0, 1.0, 1.4142}) {
        PlanarBuffer whole = MakeBuffer(2, OutputFrames, allocator);
        PlanarBuffer split = MakeBuffer(2, OutputFrames, allocator);

        LinearResampler wholeResampler = MakeResampler(2, ratio);
        LinearResampler splitResampler = MakeResampler(2, ratio);
        REQUIRE(wholeResampler.SetFrameRatio(ratio * 1.25, 1000) &&
                splitResampler.SetFrameRatio(ratio * 1.25, 1000));
        REQUIRE(Drive(wholeResampler, input.View(), whole.View(),
                      []() noexcept { return OutputFrames; }));
        REQUIRE(Drive(splitResampler, input.View(), split.View(),
                      [&random]() noexcept { return 1 + random.NextUInt32Below(300); }));
        CHECK(BitEqual(whole.View(), split.View()));
        CHECK(wholeResampler.Position() == splitResampler.Position());
    }
}

TEST(Audio_DSP1_ResamplerRatioInterpolation) {
    SystemAllocator allocator;
    PlanarBuffer input = MakeBuffer(1, 16, allocator);
    FillLinear(input.View(), 1.0f);
    PlanarBuffer output = MakeBuffer(1, 5, allocator);

    // 1 → 2를 4 frame에: 진행량 1.25, 1.5, 1.75, 2 → 위치 0, 1.25, 2.75, 4.5, 6.5.
    LinearResampler resampler = MakeResampler(1, 1.0);
    REQUIRE(resampler.SetFrameRatio(2.0, 4));
    CHECK(resampler.FrameRatio() == 1.25 && resampler.TargetFrameRatio() == 2.0);
    const auto consumed = resampler.Process(input.View(), output.View());
    REQUIRE(consumed);
    const float expected[] = {0.0f, 1.25f, 2.75f, 4.5f, 6.5f};
    for (uint32 frame = 0; frame < 5; ++frame) {
        CHECK(output.Channel(0)[frame] == expected[frame]);
    }

    CHECK(consumed.Value() == 9 && resampler.Position() == -0.5);
    CHECK(resampler.FrameRatio() == 2.0);

    // ramp 중 재설정은 지금 진행량에서 시작한다.
    LinearResampler retarget = MakeResampler(1, 1.0);
    REQUIRE(retarget.SetFrameRatio(3.0, 4));
    REQUIRE(retarget.Process(input.View(), output.View().Slice(0, 2)));
    REQUIRE(retarget.SetFrameRatio(1.0, 2));
    CHECK(retarget.FrameRatio() == 1.5);
}

TEST(Audio_DSP1_ResamplerRejectsInvalidInput) {
    SystemAllocator allocator;
    CHECK(LinearResampler::TryCreate(0, 1.0).Error() == ErrorCode::InvalidArgument);
    CHECK(LinearResampler::TryCreate(MaxOutputChannels + 1, 1.0).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(LinearResampler::TryCreate(1, MinFrameRatio));
    CHECK(LinearResampler::TryCreate(1, MaxFrameRatio));
    CHECK(LinearResampler::TryCreate(1, 0.0099).Error() == ErrorCode::InvalidArgument);
    CHECK(LinearResampler::TryCreate(1, 100.5).Error() == ErrorCode::InvalidArgument);
    CHECK(LinearResampler::TryCreate(1, 0.0).Error() == ErrorCode::InvalidArgument);
    CHECK(LinearResampler::TryCreate(1, PlatformMath::QuietNaN<double>()).Error() ==
          ErrorCode::InvalidArgument);

    LinearResampler resampler = MakeResampler(2, 0.5);
    CHECK(resampler.SetFrameRatio(2.0, MaxRatioInterpFrames + 1).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(resampler.SetFrameRatio(100.5, 0).Error() == ErrorCode::InvalidArgument);
    CHECK(resampler.SetFrameRatio(0.0099, 4).Error() == ErrorCode::InvalidArgument);
    CHECK(resampler.SetFrameRatio(PlatformMath::Infinity<double>(), 0).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(resampler.TargetFrameRatio() == 0.5);
    CHECK(resampler.InputFramesNeeded(MaxBlockFrames + 1).Error() == ErrorCode::InvalidArgument);

    PlanarBuffer input = MakeBuffer(2, 8, allocator);
    FillLinear(input.View(), 1.0f);
    PlanarBuffer output = MakeBuffer(2, 8, allocator);
    Fill(output.View(), 7.0f);
    // 8 frame 출력에는 5 frame이 필요하다. 4 frame이면 거부하고 출력·상태를 바꾸지 않는다.
    CHECK(resampler.Process(input.View().Slice(0, 4), output.View()).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(output.Channel(0)[0] == 7.0f && resampler.Position() == 0.0);

    PlanarBuffer mono = MakeBuffer(1, 8, allocator);
    CHECK(resampler.Process(mono.View(), output.View()).Error() == ErrorCode::InvalidArgument);
    CHECK(resampler.Process(input.View(), mono.View()).Error() == ErrorCode::InvalidArgument);

    // Reset은 위치·직전 frame을 지우고 비율 ramp를 목표로 끝낸다.
    REQUIRE(resampler.SetFrameRatio(1.0, 100));
    REQUIRE(resampler.Process(input.View(), output.View().Slice(0, 3)));
    resampler.Reset();
    CHECK(resampler.Position() == 0.0 && resampler.FrameRatio() == 1.0);
}

TEST(Audio_DSP1_ResamplerZeroConsumptionKeepsPreviousFrame) {
    // r = 0.01에서 1 frame씩 내면 대부분의 호출이 입력을 소비하지 않는다. 그래도 직전
    // frame(x[−1])과 위치가 유지되어 한 번에 낸 결과와 bit 단위로 같다.
    SystemAllocator allocator;
    constexpr uint32 OutputFrames = 1200;
    PlanarBuffer input = MakeBuffer(2, 64, allocator);
    RandomStream random(0x0C);
    FillRandom(input.View(), random);
    PlanarBuffer whole = MakeBuffer(2, OutputFrames, allocator);
    PlanarBuffer single = MakeBuffer(2, OutputFrames, allocator);

    LinearResampler wholeResampler = MakeResampler(2, MinFrameRatio);
    LinearResampler singleResampler = MakeResampler(2, MinFrameRatio);
    REQUIRE(
        Drive(wholeResampler, input.View(), whole.View(), []() noexcept { return OutputFrames; }));
    uint32 zeroConsumed = 0;
    uint32 inputCursor = 0;
    for (uint32 frame = 0; frame < OutputFrames; ++frame) {
        const auto needed = singleResampler.InputFramesNeeded(1);
        REQUIRE(needed);
        const auto consumed = singleResampler.Process(
            input.View().Slice(inputCursor, needed.Value()), single.View().Slice(frame, 1));
        REQUIRE(consumed);
        zeroConsumed += consumed.Value() == 0 ? 1 : 0;
        inputCursor += consumed.Value();
    }

    CHECK(zeroConsumed > OutputFrames / 2);
    CHECK(BitEqual(whole.View(), single.View()));
    CHECK(wholeResampler.Position() == singleResampler.Position());
}

namespace {

// 출력 frame RetargetFrame에서 비율을 다시 정하며 blockFrames 단위로 리샘플한다.
bool RunWithRetarget(const PlanarConstView& input, const PlanarView& output, uint32 retargetFrame,
                     uint32 blockFrames) noexcept {
    LinearResampler resampler = MakeResampler(1, 0.8);
    if (!resampler.SetFrameRatio(1.6, 900)) {
        return false;
    }

    uint32 inputCursor = 0;
    uint32 outputCursor = 0;
    while (outputCursor < output.frameCount) {
        if (outputCursor == retargetFrame && !resampler.SetFrameRatio(0.5, 300)) {
            return false;
        }

        const uint32 remaining = output.frameCount - outputCursor;
        uint32 frames = blockFrames < remaining ? blockFrames : remaining;
        if (outputCursor < retargetFrame && retargetFrame - outputCursor < frames) {
            frames = retargetFrame - outputCursor;
        }

        const auto needed = resampler.InputFramesNeeded(frames);
        if (!needed || needed.Value() > input.frameCount - inputCursor) {
            return false;
        }

        const auto consumed = resampler.Process(input.Slice(inputCursor, needed.Value()),
                                                output.Slice(outputCursor, frames));
        if (!consumed) {
            return false;
        }

        inputCursor += consumed.Value();
        outputCursor += frames;
    }

    return true;
}

} // namespace

TEST(Audio_DSP1_ResamplerRetargetBetweenBlocksSplitInvariant) {
    // 출력 frame 기준 같은 시점에 비율을 다시 정하면 블록 크기와 무관하게 같은 PCM이다.
    SystemAllocator allocator;
    RandomStream random(0x7E7A);
    constexpr uint32 OutputFrames = 1800;
    constexpr uint32 RetargetFrame = 700;
    PlanarBuffer input = MakeBuffer(1, MaxBlockFrames, allocator);
    FillRandom(input.View(), random);
    PlanarBuffer reference = MakeBuffer(1, OutputFrames, allocator);
    PlanarBuffer split = MakeBuffer(1, OutputFrames, allocator);

    REQUIRE(RunWithRetarget(input.View(), reference.View(), RetargetFrame, OutputFrames));
    for (const uint32 blockFrames : {1u, 7u, 128u, 333u}) {
        REQUIRE(RunWithRetarget(input.View(), split.View(), RetargetFrame, blockFrames));
        CHECK(BitEqual(reference.View(), split.View()));
    }
}

TEST(Audio_DSP1_ResamplerOutputFramesBefore) {
    SystemAllocator allocator;
    // r = 0.5: 위치 0, 0.5, 1, … → 위치 3 이전 출력은 6개(0–2.5).
    LinearResampler half = MakeResampler(1, 0.5);
    CHECK(half.OutputFramesBefore(3) == 6);
    CHECK(half.OutputFramesBefore(0) == 0 && half.OutputFramesBefore(-5) == 0);
    CHECK(half.OutputFramesBefore(1) == 2);
    // 상한은 MaxBlockFrames다.
    CHECK(half.OutputFramesBefore(int64{1} << 40) == MaxBlockFrames);

    // 소비 뒤 위치가 음수(−0.75)여도 같은 규칙이다: 위치 −0.75, 0, 0.75 → 입력 1 이전 3개.
    LinearResampler threeQuarter = MakeResampler(1, 0.75);
    PlanarBuffer input = MakeBuffer(1, 8, allocator);
    FillLinear(input.View(), 1.0f);
    PlanarBuffer output = MakeBuffer(1, 3, allocator);
    REQUIRE(threeQuarter.Process(input.View().Slice(0, 3), output.View()));
    CHECK(threeQuarter.Position() == -0.75);
    CHECK(threeQuarter.OutputFramesBefore(1) == 3);

    // 비율 보간 경로를 따른다: 1 → 2(4 frame) 위치 0, 1.25, 2.75, 4.5, 6.5 → 입력 5 이전 4개.
    LinearResampler ramp = MakeResampler(1, 1.0);
    REQUIRE(ramp.SetFrameRatio(2.0, 4));
    CHECK(ramp.OutputFramesBefore(5) == 4);
    CHECK(ramp.OutputFramesBefore(7) == 5);
}

} // namespace audio_filter_resampler_tests_file
