#include "AudioTestSupport.h"

#include <Audio/Decibels.h>
#include <Audio/DynamicsProcessor.h>
#include <Base/InitializerList.h>
#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/PlatformMath.h>
#include <TestSupport/MathTestSupport.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace audio_dynamics_tests_file {

using namespace rocks::audio;
using namespace rocks::audio::audiotest;
using platform::PlatformMath;

// Audio DSP2 dynamics 처리기 시험이다(Docs/Design/AudioSystem 3.21절). 정답은 3.21.2절 식(UE
// FDynamicsProcessor 블록 경로)을 double로 독립 계산한 값이다.

namespace {

constexpr uint32 Rate = DefaultMixSampleRate;

DynamicsProcessor MakeProcessor(uint32 channels, const DynamicsSettings& settings,
                                Allocator& allocator) noexcept {
    auto created = DynamicsProcessor::TryCreate(Rate, channels, settings, allocator);
    if (!created) {
        platform::Fatal("Audio test dynamics settings were rejected");
    }

    return Move(created.Value());
}

double SlopeReference(DynamicsMode mode, double ratio) noexcept {
    switch (mode) {
    case DynamicsMode::Compressor:
    case DynamicsMode::UpwardsCompressor:
        return 1.0 - 1.0 / ratio;
    case DynamicsMode::Limiter:
        return 1.0;
    case DynamicsMode::Expander:
        return 1.0 / ratio - 1.0;
    case DynamicsMode::Gate:
        return -1.0;
    }

    return 0.0;
}

// 정적 곡선 정답(선형 이득)이다.
double GainReference(const DynamicsSettings& settings, double level) noexcept {
    const double threshold = settings.thresholdDb;
    const double half = 0.5 * static_cast<double>(settings.kneeBandwidthDb);
    const double slope = SlopeReference(settings.mode, settings.ratio);
    const double low = threshold - half;
    const double high = math::Min(threshold + half, 0.0);
    const bool upwards = settings.mode == DynamicsMode::UpwardsCompressor;
    const bool inKnee =
        upwards ? level < low && level > threshold + half : level > low && level < threshold + half;
    double used = slope;
    if (inKnee) {
        const double denominator = math::Abs(high - low) < 1.0e-8 ? 1.0e-8 : high - low;
        used = (level - low) / denominator * slope;
    }

    double gainDb = used * (threshold - level);
    if (upwards) {
        return PlatformMath::Pow(10.0, math::Clamp(gainDb, 0.0, 36.0) / 20.0);
    }

    return gainDb > -1.0e-8 ? 1.0 : PlatformMath::Pow(10.0, gainDb / 20.0);
}

DynamicsSettings Instant(DynamicsMode mode) noexcept {
    DynamicsSettings settings;
    settings.mode = mode;
    settings.attackMs = 0.0f;
    settings.releaseMs = 0.0f;
    settings.lookaheadMs = 0.0f;
    settings.kneeBandwidthDb = 0.0f;
    return settings;
}

} // namespace

TEST(Audio_DSP2_StaticCurveMatchesReference) {
    SystemAllocator allocator;
    for (const DynamicsMode mode :
         {DynamicsMode::Compressor, DynamicsMode::Limiter, DynamicsMode::Expander,
          DynamicsMode::Gate, DynamicsMode::UpwardsCompressor}) {
        for (const float knee : {0.0f, 10.0f}) {
            for (const float threshold : {-20.0f, -2.0f}) {
                DynamicsSettings settings;
                settings.mode = mode;
                settings.thresholdDb = threshold;
                settings.ratio = 4.0f;
                settings.kneeBandwidthDb = knee;
                const DynamicsProcessor processor = MakeProcessor(1, settings, allocator);
                for (const float level : {-90.0f, -40.0f, -26.0f, -24.0f, -21.0f, -20.0f, -19.0f,
                                          -15.0f, -14.0f, -6.0f, 0.0f, 6.0f}) {
                    const double expected = GainReference(settings, level);
                    CHECK(test::Near(static_cast<double>(processor.GainForLevelDb(level)), expected,
                                     1.0e-6, 1.0e-5));
                }
            }
        }
    }

    // 대표값: 4:1 compressor는 임계 위 8 dB에서 −6 dB, limiter는 임계로 끌어내린다, 임계 아래는
    // 정확히 1.
    DynamicsSettings compressor = Instant(DynamicsMode::Compressor);
    compressor.thresholdDb = -20.0f;
    compressor.ratio = 4.0f;
    const DynamicsProcessor compress = MakeProcessor(1, compressor, allocator);
    CHECK(test::Near(compress.GainForLevelDb(-12.0f), PlatformMath::Pow(10.0f, -6.0f / 20.0f),
                     1.0e-6f));
    CHECK(compress.GainForLevelDb(-30.0f) == 1.0f);
    // UpwardsCompressor는 36 dB를 넘지 않는다.
    DynamicsSettings upwards = Instant(DynamicsMode::UpwardsCompressor);
    upwards.ratio = 1000.0f;
    const DynamicsProcessor upward = MakeProcessor(1, upwards, allocator);
    CHECK(test::Near(upward.GainForLevelDb(-96.0f), PlatformMath::Pow(10.0f, 36.0f / 20.0f),
                     1.0e-3f));
}

TEST(Audio_DSP2_EnvelopeCoefficientsAndStepResponse) {
    SystemAllocator allocator;
    for (const bool analog : {true, false}) {
        DynamicsSettings settings;
        settings.analogMode = analog;
        settings.attackMs = 10.0f;
        settings.releaseMs = 50.0f;
        const DynamicsProcessor processor = MakeProcessor(1, settings, allocator);
        const double timeConstant = analog ? 1.00239343 : 4.60517019;
        CHECK(test::Near(static_cast<double>(processor.AttackCoefficient()),
                         PlatformMath::Exp(-1000.0 * timeConstant / (10.0 * Rate)), 1.0e-7));
        CHECK(test::Near(static_cast<double>(processor.ReleaseCoefficient()),
                         PlatformMath::Exp(-1000.0 * timeConstant / (50.0 * Rate)), 1.0e-7));
    }

    // 계단 응답: 1 → 엔벌로프가 어택 시간(480 frame)에 digital 99%, analog 63.2%. 엔벌로프는
    // 이득으로 본다: limiter 임계 −96 dB에서 이득 = 임계/엔벌로프라 엔벌로프 = 임계 선형값/이득.
    for (const bool analog : {true, false}) {
        DynamicsSettings settings = Instant(DynamicsMode::Limiter);
        settings.analogMode = analog;
        settings.attackMs = 10.0f;
        settings.thresholdDb = -96.0f;
        DynamicsProcessor processor = MakeProcessor(1, settings, allocator);
        PlanarBuffer buffer = MakeBuffer(1, 480, allocator);
        Fill(buffer.View(), 1.0f);
        processor.Process(buffer.View());
        const double floor = PlatformMath::Pow(10.0, -96.0 / 20.0);
        const double envelope = floor / static_cast<double>(buffer.Channel(0)[479]);
        const double expected =
            1.0 - PlatformMath::Pow(static_cast<double>(processor.AttackCoefficient()), 480.0);
        CHECK(test::Near(envelope, expected, 0.0, 1.0e-3));
        CHECK(test::Near(envelope, analog ? 0.632 : 0.99, 0.0, 5.0e-3));
    }
}

TEST(Audio_DSP2_MeanSquaredAndRmsSettle) {
    SystemAllocator allocator;
    for (const EnvelopePeakMode mode :
         {EnvelopePeakMode::MeanSquared, EnvelopePeakMode::RootMeanSquared}) {
        DynamicsSettings settings = Instant(DynamicsMode::Limiter);
        settings.peakMode = mode;
        settings.thresholdDb = -96.0f;
        DynamicsProcessor processor = MakeProcessor(1, settings, allocator);
        PlanarBuffer buffer = MakeBuffer(1, MaxBlockFrames, allocator);
        Fill(buffer.View(), 0.5f);
        processor.Process(buffer.View());
        // 5 ms 창이 수렴한 뒤 엔벌로프 = 0.25(평균 제곱) 또는 0.5(RMS)다.
        const double envelope =
            PlatformMath::Pow(10.0, -96.0 / 20.0) * 0.5 / buffer.Channel(0)[MaxBlockFrames - 1];
        CHECK(
            test::Near(envelope, mode == EnvelopePeakMode::MeanSquared ? 0.25 : 0.5, 0.0, 1.0e-4));
    }
}

TEST(Audio_DSP2_LookaheadDelaysSignal) {
    SystemAllocator allocator;
    for (const float lookahead : {0.0f, 1.0f, 10.0f, 2.51f, MaxLookaheadMs}) {
        DynamicsSettings settings;
        settings.lookaheadMs = lookahead;
        DynamicsProcessor processor = MakeProcessor(1, settings, allocator);
        const uint32 expected =
            static_cast<uint32>(PlatformMath::Ceil(static_cast<double>(lookahead) * Rate / 1000.0));
        CHECK(processor.LookaheadFrames() == expected);
        PlanarBuffer buffer = MakeBuffer(1, MaxBlockFrames, allocator);
        buffer.Channel(0)[0] = 1.0e-3f;
        processor.Process(buffer.View());
        // 임계(−6 dB) 아래 신호는 이득 1이라 임펄스가 지연만 된다.
        for (uint32 frame = 0; frame < MaxBlockFrames; ++frame) {
            CHECK(buffer.Channel(0)[frame] == (frame == expected ? 1.0e-3f : 0.0f));
        }

        // 첫 블록 밖의 지연(100 ms = 4,800 frame)은 다음 블록에 나온다.
        Clear(buffer.View());
        processor.Process(buffer.View());
        for (uint32 frame = 0; frame < MaxBlockFrames; ++frame) {
            CHECK(buffer.Channel(0)[frame] ==
                  (frame + MaxBlockFrames == expected ? 1.0e-3f : 0.0f));
        }
    }

    // 실행 중 길이 변경은 읽기 위치만 옮긴다: 10 → 5 ms면 이력의 5 ms 전 표본이 나온다.
    DynamicsSettings settings;
    settings.lookaheadMs = 10.0f;
    DynamicsProcessor processor = MakeProcessor(1, settings, allocator);
    PlanarBuffer first = MakeBuffer(1, 1000, allocator);
    for (uint32 frame = 0; frame < 1000; ++frame) {
        first.Channel(0)[frame] = static_cast<float>(frame) * 1.0e-6f;
    }

    processor.Process(first.View());
    settings.lookaheadMs = 5.0f;
    REQUIRE(processor.SetSettings(settings));
    PlanarBuffer second = MakeBuffer(1, 1, allocator);
    second.Channel(0)[0] = 0.0f;
    processor.Process(second.View());
    CHECK(second.Channel(0)[0] == static_cast<float>(1000 - 240) * 1.0e-6f);
}

TEST(Audio_DSP2_LimiterHoldsThreshold) {
    SystemAllocator allocator;
    DynamicsSettings settings = Instant(DynamicsMode::Limiter);
    settings.thresholdDb = -6.0f;
    DynamicsProcessor processor = MakeProcessor(2, settings, allocator);
    PlanarBuffer buffer = MakeBuffer(2, MaxBlockFrames, allocator);
    RandomStream random(0x11);
    for (uint32 channel = 0; channel < 2; ++channel) {
        for (uint32 frame = 0; frame < MaxBlockFrames; ++frame) {
            buffer.Channel(channel)[frame] = random.NextFloatInRange(-4.0f, 4.0f);
        }
    }

    PlanarBuffer original = MakeBuffer(2, MaxBlockFrames, allocator);
    for (uint32 channel = 0; channel < 2; ++channel) {
        memcpy(original.Channel(channel), buffer.Channel(channel), sizeof(float) * MaxBlockFrames);
    }

    processor.Process(buffer.View());
    const float threshold = PlatformMath::Pow(10.0f, -6.0f / 20.0f);
    for (uint32 channel = 0; channel < 2; ++channel) {
        for (uint32 frame = 0; frame < MaxBlockFrames; ++frame) {
            const float input = original.Channel(channel)[frame];
            const float output = buffer.Channel(channel)[frame];
            CHECK(math::Abs(output) <= threshold * 1.0001f);
            // 임계 아래 표본은 그대로다.
            if (math::Abs(input) < threshold * 0.999f) {
                CHECK(output == input);
            }
        }
    }
}

TEST(Audio_DSP2_LinkModes) {
    SystemAllocator allocator;
    // 왼쪽 0 dBFS, 오른쪽 −40 dBFS 상수. limiter −20 dB, 즉시 응답.
    const auto run = [&](DynamicsLinkMode link, float& left, float& right) {
        DynamicsSettings settings = Instant(DynamicsMode::Limiter);
        settings.thresholdDb = -20.0f;
        settings.linkMode = link;
        DynamicsProcessor processor = MakeProcessor(2, settings, allocator);
        PlanarBuffer buffer = MakeBuffer(2, 16, allocator);
        for (uint32 frame = 0; frame < 16; ++frame) {
            buffer.Channel(0)[frame] = 1.0f;
            buffer.Channel(1)[frame] = 0.01f;
        }

        processor.Process(buffer.View());
        left = buffer.Channel(0)[15];
        right = buffer.Channel(1)[15];
    };

    float left = 0.0f;
    float right = 0.0f;
    run(DynamicsLinkMode::Disabled, left, right);
    CHECK(test::Near(left, 0.1f, 1.0e-6f) && right == 0.01f);
    run(DynamicsLinkMode::Peak, left, right);
    CHECK(test::Near(left, 0.1f, 1.0e-6f) && test::Near(right, 0.001f, 1.0e-8f));
    // 평균 엔벌로프 0.505 → 이득 0.1/0.505.
    run(DynamicsLinkMode::Average, left, right);
    CHECK(test::Near(left, 0.1f / 0.505f, 1.0e-5f) && test::Near(right, 0.001f / 0.505f, 1.0e-7f));
}

TEST(Audio_DSP2_BlockSplitInvariant) {
    SystemAllocator allocator;
    RandomStream random(0xD2);
    for (const EnvelopePeakMode peak : {EnvelopePeakMode::Peak, EnvelopePeakMode::MeanSquared,
                                        EnvelopePeakMode::RootMeanSquared}) {
        DynamicsSettings settings;
        settings.mode = DynamicsMode::Compressor;
        settings.ratio = 3.0f;
        settings.thresholdDb = -18.0f;
        settings.attackMs = 5.0f;
        settings.releaseMs = 80.0f;
        settings.lookaheadMs = 3.0f;
        settings.peakMode = peak;
        settings.linkMode = DynamicsLinkMode::Average;
        DynamicsProcessor whole = MakeProcessor(2, settings, allocator);
        DynamicsProcessor split = MakeProcessor(2, settings, allocator);
        PlanarBuffer wholeBuffer = MakeBuffer(2, MaxBlockFrames, allocator);
        FillRandom(wholeBuffer.View(), random);
        PlanarBuffer splitBuffer = MakeBuffer(2, MaxBlockFrames, allocator);
        for (uint32 channel = 0; channel < 2; ++channel) {
            memcpy(splitBuffer.Channel(channel), wholeBuffer.Channel(channel),
                   sizeof(float) * MaxBlockFrames);
        }

        whole.Process(wholeBuffer.View());
        uint32 offset = 0;
        while (offset < MaxBlockFrames) {
            uint32 frames = 1 + random.NextUInt32Below(400);
            if (frames > MaxBlockFrames - offset) {
                frames = MaxBlockFrames - offset;
            }

            split.Process(splitBuffer.View().Slice(offset, frames));
            offset += frames;
        }

        CHECK(BitEqual(wholeBuffer.View(), splitBuffer.View()));
    }
}

TEST(Audio_DSP2_ResetClearsHistory) {
    SystemAllocator allocator;
    RandomStream random(0x3E);
    DynamicsSettings settings;
    settings.ratio = 4.0f;
    settings.peakMode = EnvelopePeakMode::RootMeanSquared;
    DynamicsProcessor used = MakeProcessor(2, settings, allocator);
    PlanarBuffer noise = MakeBuffer(2, 2048, allocator);
    FillRandom(noise.View(), random);
    used.Process(noise.View());
    used.Reset();

    DynamicsProcessor fresh = MakeProcessor(2, settings, allocator);
    PlanarBuffer a = MakeBuffer(2, 2048, allocator);
    FillRandom(a.View(), random);
    PlanarBuffer b = MakeBuffer(2, 2048, allocator);
    for (uint32 channel = 0; channel < 2; ++channel) {
        memcpy(b.Channel(channel), a.Channel(channel), sizeof(float) * 2048);
    }

    used.Process(a.View());
    fresh.Process(b.View());
    CHECK(BitEqual(a.View(), b.View()));
}

TEST(Audio_DSP2_RejectsInvalidInput) {
    SystemAllocator system;
    const DynamicsSettings valid;
    CHECK(DynamicsProcessor::TryCreate(MinMixSampleRate - 1, 1, valid, system).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(DynamicsProcessor::TryCreate(Rate, 0, valid, system).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(DynamicsProcessor::TryCreate(Rate, MaxOutputChannels + 1, valid, system).Error() ==
          ErrorCode::InvalidArgument);

    test::NFailAllocator failing(system);
    failing.FailOnAttempt(1);
    CHECK(DynamicsProcessor::TryCreate(Rate, 2, valid, failing).Error() == ErrorCode::OutOfMemory);
    failing.DisableFailure();
    CHECK(DynamicsProcessor::TryCreate(MaxMixSampleRate, MaxOutputChannels, valid, failing));

    const float nan = PlatformMath::QuietNaN<float>();
    const float infinity = PlatformMath::Infinity<float>();
    DynamicsProcessor processor = MakeProcessor(1, valid, system);
    const auto rejects = [&](auto mutate) {
        DynamicsSettings settings;
        mutate(settings);
        const bool created =
            static_cast<bool>(DynamicsProcessor::TryCreate(Rate, 1, settings, system));
        const bool set = static_cast<bool>(processor.SetSettings(settings));
        return !created && !set;
    };

    CHECK(rejects([](DynamicsSettings& s) { s.lookaheadMs = -1.0f; }));
    CHECK(rejects([](DynamicsSettings& s) { s.lookaheadMs = MaxLookaheadMs + 0.01f; }));
    CHECK(rejects([&](DynamicsSettings& s) { s.lookaheadMs = nan; }));
    CHECK(rejects([](DynamicsSettings& s) { s.attackMs = -0.5f; }));
    CHECK(rejects([&](DynamicsSettings& s) { s.releaseMs = infinity; }));
    CHECK(rejects([&](DynamicsSettings& s) { s.thresholdDb = nan; }));
    CHECK(rejects([](DynamicsSettings& s) { s.ratio = 0.0f; }));
    CHECK(rejects([](DynamicsSettings& s) { s.ratio = -2.0f; }));
    CHECK(rejects([](DynamicsSettings& s) { s.kneeBandwidthDb = -1.0f; }));
    CHECK(rejects([&](DynamicsSettings& s) { s.inputGainDb = infinity; }));
    CHECK(rejects([&](DynamicsSettings& s) { s.outputGainDb = nan; }));
    CHECK(rejects([](DynamicsSettings& s) { s.analysisWindowMs = 0.0f; }));
    CHECK(rejects([&](DynamicsSettings& s) { s.analysisWindowMs = infinity; }));
    CHECK(rejects([&](DynamicsSettings& s) { s.lookaheadMs = infinity; }));
    CHECK(rejects([&](DynamicsSettings& s) { s.attackMs = nan; }));
    CHECK(rejects([&](DynamicsSettings& s) { s.kneeBandwidthDb = infinity; }));
    CHECK(rejects([&](DynamicsSettings& s) { s.ratio = nan; }));
    CHECK(rejects([](DynamicsSettings& s) { s.ratio = MinDynamicsRatio * 0.5f; }));
    CHECK(rejects([](DynamicsSettings& s) { s.inputGainDb = MaxDynamicsGainDb + 1.0f; }));
    CHECK(rejects([](DynamicsSettings& s) { s.outputGainDb = -MaxDynamicsGainDb - 1.0f; }));
    CHECK(rejects([](DynamicsSettings& s) { s.mode = static_cast<DynamicsMode>(9); }));
    CHECK(rejects([](DynamicsSettings& s) { s.peakMode = static_cast<EnvelopePeakMode>(9); }));
    CHECK(rejects([](DynamicsSettings& s) { s.linkMode = static_cast<DynamicsLinkMode>(9); }));
    // 거부한 설정은 적용되지 않는다.
    CHECK(processor.Settings().ratio == valid.ratio && processor.LookaheadFrames() == 480);
}

TEST(Audio_DSP2_DecibelConversions) {
    CHECK(DecibelsToLinear(0.0f) == 1.0f);
    CHECK(test::Near(DecibelsToLinear(-20.0f), 0.1f, 1.0e-7f));
    CHECK(test::Near(DecibelsToLinear(6.0f), 1.9952623f, 1.0e-6f));
    CHECK(test::Near(LinearToDecibels(0.1f, DecibelsToLinear(-96.0f)), -20.0f, 1.0e-5f));
    CHECK(test::Near(LinearToDecibels(1.0f, DecibelsToLinear(-96.0f)), 0.0f, 1.0e-6f));
    // 0·음수·NaN은 하한이다.
    CHECK(test::Near(LinearToDecibels(0.0f, DecibelsToLinear(-96.0f)), -96.0f, 1.0e-4f));
    CHECK(test::Near(LinearToDecibels(-1.0f, DecibelsToLinear(-60.0f)), -60.0f, 1.0e-4f));
    CHECK(test::Near(LinearToDecibels(PlatformMath::QuietNaN<float>(), DecibelsToLinear(-60.0f)),
                     -60.0f, 1.0e-4f));
    for (const float decibels : {-90.0f, -30.0f, -6.0f, 0.0f, 12.0f}) {
        CHECK(test::Near(LinearToDecibels(DecibelsToLinear(decibels), DecibelsToLinear(-120.0f)),
                         decibels, 1.0e-4f));
    }
}

TEST(Audio_DSP2_ReleaseAndMeanSquaredTransients) {
    SystemAllocator allocator;
    const double floor = PlatformMath::Pow(10.0, -96.0 / 20.0);
    // 릴리스: 1로 채운 뒤 0.5로 내리면 엔벌로프 = 0.5 + 0.5·release^n이다(어택 즉시).
    DynamicsSettings settings = Instant(DynamicsMode::Limiter);
    settings.thresholdDb = -96.0f;
    settings.releaseMs = 10.0f;
    settings.analogMode = false;
    DynamicsProcessor release = MakeProcessor(1, settings, allocator);
    PlanarBuffer buffer = MakeBuffer(1, 480, allocator);
    Fill(buffer.View(), 1.0f);
    release.Process(buffer.View());
    Fill(buffer.View(), 0.5f);
    release.Process(buffer.View());
    const double coefficient = release.ReleaseCoefficient();
    for (const uint32 frame : {0u, 47u, 239u, 479u}) {
        const double envelope = floor * 0.5 / buffer.Channel(0)[frame];
        CHECK(test::Near(envelope, 0.5 + 0.5 * PlatformMath::Pow(coefficient, frame + 1.0), 0.0,
                         1.0e-4));
    }

    // 평균 제곱 과도 구간: k번째 표본의 값 = x²·(1 − α^k), α = exp(−1000/(fs·창)).
    for (const EnvelopePeakMode mode :
         {EnvelopePeakMode::MeanSquared, EnvelopePeakMode::RootMeanSquared}) {
        DynamicsSettings window = Instant(DynamicsMode::Limiter);
        window.thresholdDb = -96.0f;
        window.peakMode = mode;
        window.analysisWindowMs = 2.0f;
        DynamicsProcessor processor = MakeProcessor(1, window, allocator);
        Fill(buffer.View(), 0.5f);
        processor.Process(buffer.View());
        const double alpha = PlatformMath::Exp(-1000.0 / (static_cast<double>(Rate) * 2.0));
        for (const uint32 frame : {0u, 9u, 95u, 300u}) {
            const double meanSquare = 0.25 * (1.0 - PlatformMath::Pow(alpha, frame + 1.0));
            const double expected =
                mode == EnvelopePeakMode::MeanSquared ? meanSquare : PlatformMath::Sqrt(meanSquare);
            const double envelope = floor * 0.5 / buffer.Channel(0)[frame];
            CHECK(test::Near(envelope, expected, 0.0, 1.0e-4));
        }
    }
}

TEST(Audio_DSP2_LookaheadLengthenReadsOlderHistory) {
    SystemAllocator allocator;
    DynamicsSettings settings;
    settings.lookaheadMs = 5.0f;
    DynamicsProcessor processor = MakeProcessor(1, settings, allocator);
    PlanarBuffer first = MakeBuffer(1, 1000, allocator);
    for (uint32 frame = 0; frame < 1000; ++frame) {
        first.Channel(0)[frame] = static_cast<float>(frame) * 1.0e-6f;
    }

    processor.Process(first.View());
    settings.lookaheadMs = 10.0f;
    REQUIRE(processor.SetSettings(settings));
    PlanarBuffer second = MakeBuffer(1, 1, allocator);
    processor.Process(second.View());
    CHECK(second.Channel(0)[0] == static_cast<float>(1000 - 480) * 1.0e-6f);
}

TEST(Audio_DSP2_NonFiniteInputDoesNotPoisonEnvelope) {
    SystemAllocator allocator;
    DynamicsSettings settings = Instant(DynamicsMode::Limiter);
    settings.thresholdDb = -6.0f;
    settings.releaseMs = 1.0f;
    DynamicsProcessor processor = MakeProcessor(1, settings, allocator);
    PlanarBuffer buffer = MakeBuffer(1, 256, allocator);
    Fill(buffer.View(), 2.0f);
    buffer.Channel(0)[10] = PlatformMath::Infinity<float>();
    buffer.Channel(0)[11] = PlatformMath::QuietNaN<float>();
    processor.Process(buffer.View());
    // 비유한 표본 뒤에도 limiter가 계속 임계로 끌어내린다(엔벌로프가 NaN에 갇히지 않는다).
    const float threshold = DecibelsToLinear(-6.0f);
    for (uint32 frame = 20; frame < 256; ++frame) {
        CHECK(math::IsFinite(buffer.Channel(0)[frame]));
        CHECK(math::Abs(buffer.Channel(0)[frame]) <= threshold * 1.0001f);
    }
}

TEST(Audio_DSP2_LargeBlockIsProcessedInChunks) {
    SystemAllocator allocator;
    RandomStream random(0xB16);
    constexpr uint32 Frames = MaxBlockFrames * 2 + 123;
    // PlanarBuffer는 MaxBlockFrames까지라 큰 보기는 직접 만든다.
    static float whole[Frames];
    static float split[Frames];
    for (uint32 frame = 0; frame < Frames; ++frame) {
        whole[frame] = random.NextFloatInRange(-2.0f, 2.0f);
        split[frame] = whole[frame];
    }

    DynamicsSettings settings;
    settings.mode = DynamicsMode::Compressor;
    settings.ratio = 6.0f;
    settings.attackMs = 2.0f;
    DynamicsProcessor large = MakeProcessor(1, settings, allocator);
    DynamicsProcessor small = MakeProcessor(1, settings, allocator);
    PlanarView view;
    view.channelCount = 1;
    view.frameCount = Frames;
    view.channels[0] = whole;
    large.Process(view);
    for (uint32 offset = 0; offset < Frames; offset += 1000) {
        PlanarView part;
        part.channelCount = 1;
        part.frameCount = Frames - offset < 1000 ? Frames - offset : 1000;
        part.channels[0] = split + offset;
        small.Process(part);
    }

    CHECK(memcmp(whole, split, sizeof(whole)) == 0);

    // 빈(기본 생성) 처리기는 아무것도 하지 않는다.
    DynamicsProcessor empty;
    PlanarView none;
    empty.Process(none);
}

} // namespace audio_dynamics_tests_file
