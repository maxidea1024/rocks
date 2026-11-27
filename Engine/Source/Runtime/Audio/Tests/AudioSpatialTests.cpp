#include <Base/InitializerList.h>
#include <Audio/DistanceAttenuation.h>
#include <Audio/Panner.h>
#include <Core/Math/MathUtility.h>
#include <Platform/Fatal.h>
#include <Platform/PlatformMath.h>
#include <TestSupport/MathTestSupport.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace audio_spatial_tests_file {

using namespace rocks::audio;
using platform::PlatformMath;

// Audio DSP1의 방위각 패너·거리 감쇠
// 시험이다(Docs/Design/AudioSystem 3.20.6–3.20.7절, 3.20.8절). 패너 정답은 UE Get3DChannelMap
// 식(스피커 표와 각도 비율)이고 감쇠 정답은 UE AttenuationEval 식을 double로 독립 계산한 값이다.

namespace {

constexpr float Tolerance = 1.0e-5f;
constexpr float InverseSqrt2 = 0.70710678118654752f;

// 수평면에서 앞(+X)으로부터 시계 방향(오른쪽 +Y) degrees 방향이다.
Vec3 HorizontalDirection(float degrees) noexcept {
    const float radians = math::DegreesToRadians(degrees);
    return Vec3(PlatformMath::Cos(radians), PlatformMath::Sin(radians), 0.0f);
}

struct Gains {
    float values[MaxOutputChannels] = {};
};

Gains GainsOf(ChannelLayout layout, float azimuth, float nonSpatialized = 0.0f,
              float lfeBleed = 0.0f) noexcept {
    Gains gains;
    const Result<void> computed = ComputeChannelGains(layout, azimuth, nonSpatialized, lfeBleed,
                                                      Span<float>(gains.values, MaxOutputChannels));
    if (!computed) {
        platform::Fatal("Audio test panning input was rejected");
    }

    return gains;
}

// 두 스피커(prev → next) 사이 비율 f의 equal-power 정답이다.
void ExpectPair(const Gains& gains, uint32 channels, uint32 prev, uint32 next,
                double fraction) noexcept {
    const double angle = fraction * math::HalfPi<double>;
    for (uint32 index = 0; index < channels; ++index) {
        double expected = 0.0;
        if (index == prev) {
            expected = PlatformMath::Cos(angle);
        } else if (index == next) {
            expected = PlatformMath::Sin(angle);
        }

        CHECK(test::Near(static_cast<double>(gains.values[index]), expected, 1.0e-5));
    }
}

DistanceAttenuationSettings Settings(DistanceAttenuationModel model, float falloff) noexcept {
    DistanceAttenuationSettings settings;
    settings.model = model;
    settings.falloffDistance = falloff;
    return settings;
}

float Attenuate(const DistanceAttenuationSettings& settings, float distance) noexcept {
    const auto result = EvaluateDistanceAttenuation(settings, distance);
    if (!result) {
        platform::Fatal("Audio test attenuation input was rejected");
    }

    return result.Value();
}

double ClampUnit(double value) noexcept {
    return math::Clamp(value, 0.0, 1.0);
}

} // namespace

TEST(Audio_DSP1_AzimuthQuadrants) {
    const Vec3 forward(1.0f, 0.0f, 0.0f);
    const Vec3 right(0.0f, 1.0f, 0.0f);
    for (const float degrees :
         {0.0f, 45.0f, 90.0f, 135.0f, 180.0f, 225.0f, 270.0f, 315.0f, 10.0f, 359.0f}) {
        const float azimuth = ComputeAzimuthDegrees(forward, right, HorizontalDirection(degrees));
        CHECK(test::Near(azimuth, degrees, 1.0e-3f));
        CHECK(azimuth >= 0.0f && azimuth < 360.0f);
    }

    // 정왼쪽(x = 0, y < 0)은 270°다(UE의 90° 경계 결함은 따르지 않는다).
    CHECK(test::Near(ComputeAzimuthDegrees(forward, right, Vec3(0.0f, -1.0f, 0.0f)), 270.0f,
                     1.0e-4f));
    // 정후면(x < 0, y = 0)은 180°다(UE는 0°로 낸다).
    CHECK(ComputeAzimuthDegrees(forward, right, Vec3(-1.0f, 0.0f, 0.0f)) == 180.0f);
    // 수직 방향은 0°다(UE는 90°). 위쪽 성분은 방위각에 영향이 없다.
    CHECK(ComputeAzimuthDegrees(forward, right, Vec3(0.0f, 0.0f, 1.0f)) == 0.0f);
    const Vec3 raised = Vec3(0.0f, 0.6f, 0.8f);
    CHECK(test::Near(ComputeAzimuthDegrees(forward, right, raised), 90.0f, 1.0e-4f));
    // 리스너를 180° 돌리면 +Y 소리는 왼쪽(270°)이다.
    CHECK(test::Near(ComputeAzimuthDegrees(Vec3(-1.0f, 0.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f),
                                           Vec3(0.0f, 1.0f, 0.0f)),
                     270.0f, 1.0e-4f));
}

TEST(Audio_DSP1_StereoPanningEqualPower) {
    // 정면·정후면은 양쪽 1/√2, 정오른쪽은 R만 1, 정왼쪽은 L만 1이다.
    for (const float azimuth : {0.0f, 180.0f}) {
        const Gains gains = GainsOf(ChannelLayout::Stereo, azimuth);
        CHECK(test::Near(gains.values[0], InverseSqrt2, Tolerance) &&
              test::Near(gains.values[1], InverseSqrt2, Tolerance));
    }

    const Gains rightSide = GainsOf(ChannelLayout::Stereo, 90.0f);
    CHECK(test::Near(rightSide.values[1], 1.0f, Tolerance) &&
          test::Near(rightSide.values[0], 0.0f, Tolerance));
    const Gains leftSide = GainsOf(ChannelLayout::Stereo, 270.0f);
    CHECK(test::Near(leftSide.values[0], 1.0f, Tolerance) &&
          test::Near(leftSide.values[1], 0.0f, Tolerance));

    // 한 바퀴 내내 L² + R² = 1이고, 오른쪽 반구(0–180°)는 R ≥ L이다.
    for (uint32 step = 0; step < 360; ++step) {
        const float azimuth = static_cast<float>(step);
        const Gains gains = GainsOf(ChannelLayout::Stereo, azimuth);
        const float power = gains.values[0] * gains.values[0] + gains.values[1] * gains.values[1];
        CHECK(test::Near(power, 1.0f, 1.0e-5f));
        if (step <= 180) {
            CHECK(gains.values[1] + Tolerance >= gains.values[0]);
        } else {
            CHECK(gains.values[0] + Tolerance >= gains.values[1]);
        }
    }

    // 45°는 FL(270°)→FR(90°) 180° 구간의 3/4 지점이다.
    ExpectPair(GainsOf(ChannelLayout::Stereo, 45.0f), 2, 0, 1, 0.75);
    // 범위 밖 방위각은 감싼다.
    ExpectPair(GainsOf(ChannelLayout::Stereo, -90.0f), 2, 1, 0, 1.0);
    ExpectPair(GainsOf(ChannelLayout::Stereo, 450.0f), 2, 0, 1, 1.0);
}

TEST(Audio_DSP1_SurroundSpeakerPairs) {
    // 5.1: FL 330·FR 30·SL 250·SR 110. 색인 FL 0, FR 1, FC 2, LFE 3, SL 4, SR 5.
    ExpectPair(GainsOf(ChannelLayout::Surround51, 0.0f), 6, 0, 1, 0.5);
    ExpectPair(GainsOf(ChannelLayout::Surround51, 70.0f), 6, 1, 5, 0.5);
    ExpectPair(GainsOf(ChannelLayout::Surround51, 180.0f), 6, 5, 4, 0.5);
    ExpectPair(GainsOf(ChannelLayout::Surround51, 290.0f), 6, 4, 0, 0.5);
    ExpectPair(GainsOf(ChannelLayout::Surround51, 30.0f), 6, 0, 1, 1.0);
    // 7.1: BR 150·BL 210 사이가 정후면이다. 색인 BL 6, BR 7.
    ExpectPair(GainsOf(ChannelLayout::Surround71, 180.0f), 8, 7, 6, 0.5);
    ExpectPair(GainsOf(ChannelLayout::Surround71, 130.0f), 8, 5, 7, 0.5);
    ExpectPair(GainsOf(ChannelLayout::Surround71, 340.0f), 8, 0, 1, 10.0 / 60.0);

    // LFE는 bleed 값이고 FC는 0이다.
    const Gains bleed = GainsOf(ChannelLayout::Surround51, 10.0f, 0.0f, 0.25f);
    CHECK(bleed.values[3] == 0.25f && bleed.values[2] == 0.0f);
}

TEST(Audio_DSP1_SurroundEqualPowerSweep) {
    // 5.1·7.1에서도 한 바퀴 내내 공간 채널 이득 제곱합이 1이고, FC·LFE(bleed 0)는 0이다.
    for (const ChannelLayout layout : {ChannelLayout::Surround51, ChannelLayout::Surround71}) {
        const uint32 channels = ChannelCount(layout);
        for (uint32 step = 0; step < 720; ++step) {
            const Gains gains = GainsOf(layout, 0.5f * static_cast<float>(step));
            float power = 0.0f;
            for (uint32 index = 0; index < channels; ++index) {
                power += gains.values[index] * gains.values[index];
            }

            CHECK(test::Near(power, 1.0f, 1.0e-5f));
            CHECK(gains.values[2] == 0.0f && gains.values[3] == 0.0f);
        }
    }
}

TEST(Audio_DSP1_NonSpatializedAmountAndCenterOnly) {
    // 비율 1이면 LFE 밖 모든 채널이 1/채널 수다.
    const Gains omni = GainsOf(ChannelLayout::Surround51, 77.0f, 1.0f, 0.5f);
    for (uint32 index = 0; index < 6; ++index) {
        CHECK(omni.values[index] == (index == 3 ? 0.5f : 1.0f / 6.0f));
    }

    // 비율 0.5, 정오른쪽: FR = Lerp(1, 0.5, 0.5), FL = Lerp(0, 0.5, 0.5).
    const Gains half = GainsOf(ChannelLayout::Stereo, 90.0f, 0.5f);
    CHECK(test::Near(half.values[1], 0.75f, Tolerance) &&
          test::Near(half.values[0], 0.25f, Tolerance));

    float mono[1] = {};
    REQUIRE(ComputeChannelGains(ChannelLayout::Mono, 123.0f, 0.0f, 0.0f, Span<float>(mono)));
    CHECK(mono[0] == 1.0f);

    float stereo[2] = {};
    REQUIRE(ComputeCenterChannelOnlyGains(ChannelLayout::Stereo, Span<float>(stereo)));
    CHECK(test::Near(stereo[0], InverseSqrt2, Tolerance) &&
          test::Near(stereo[1], InverseSqrt2, Tolerance));
    float surround[8] = {};
    REQUIRE(ComputeCenterChannelOnlyGains(ChannelLayout::Surround71, Span<float>(surround)));
    for (uint32 index = 0; index < 8; ++index) {
        CHECK(surround[index] == (index == 2 ? 1.0f : 0.0f));
    }

    REQUIRE(ComputeCenterChannelOnlyGains(ChannelLayout::Mono, Span<float>(mono)));
    CHECK(mono[0] == 1.0f);
}

TEST(Audio_DSP1_PanningRejectsInvalidInput) {
    float gains[MaxOutputChannels] = {9.0f, 9.0f, 9.0f, 9.0f, 9.0f, 9.0f, 9.0f, 9.0f};
    CHECK(ComputeChannelGains(ChannelLayout::Surround51, 0.0f, 0.0f, 0.0f, Span<float>(gains, 5))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(ComputeChannelGains(ChannelLayout::Stereo, PlatformMath::QuietNaN<float>(), 0.0f, 0.0f,
                              Span<float>(gains))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(ComputeChannelGains(ChannelLayout::Stereo, PlatformMath::Infinity<float>(), 0.0f, 0.0f,
                              Span<float>(gains))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(
        ComputeChannelGains(ChannelLayout::Stereo, 0.0f, 1.5f, 0.0f, Span<float>(gains)).Error() ==
        ErrorCode::InvalidArgument);
    CHECK(
        ComputeChannelGains(ChannelLayout::Stereo, 0.0f, -0.1f, 0.0f, Span<float>(gains)).Error() ==
        ErrorCode::InvalidArgument);
    CHECK(ComputeChannelGains(ChannelLayout::Surround51, 0.0f, 0.0f, 2.0f, Span<float>(gains))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(ComputeCenterChannelOnlyGains(ChannelLayout::Stereo, Span<float>(gains, 1)).Error() ==
          ErrorCode::InvalidArgument);
    // 거부한 호출은 out을 쓰지 않는다.
    CHECK(gains[0] == 9.0f && gains[1] == 9.0f);
}

TEST(Audio_DSP1_DistanceModelsMatchReference) {
    constexpr float Falloff = 1000.0f;
    const double falloff = Falloff;
    const double epsilon = AttenuationEpsilon;
    for (const float distance :
         {0.0f, 1.0f, 20.0f, 250.0f, 500.0f, 999.0f, 1000.0f, 1500.0f, 4000.0f}) {
        const double d = distance;
        const double linear = ClampUnit(1.0 - d / falloff);
        CHECK(test::Near(static_cast<double>(Attenuate(
                             Settings(DistanceAttenuationModel::Linear, Falloff), distance)),
                         linear, 1.0e-6));

        const double logarithmic =
            ClampUnit(0.5 * -PlatformMath::Log(math::Max(d, epsilon) / falloff));
        CHECK(test::Near(static_cast<double>(Attenuate(
                             Settings(DistanceAttenuationModel::Logarithmic, Falloff), distance)),
                         logarithmic, 1.0e-6));

        const double inverse = ClampUnit(0.02 / (math::Max(d, epsilon) / falloff));
        CHECK(test::Near(static_cast<double>(Attenuate(
                             Settings(DistanceAttenuationModel::Inverse, Falloff), distance)),
                         inverse, 1.0e-6));

        const double logReverse =
            d > falloff
                ? 0.0
                : ClampUnit(1.0 + 0.5 * PlatformMath::Log(math::Max(1.0 - d / falloff, epsilon)));
        CHECK(test::Near(static_cast<double>(Attenuate(
                             Settings(DistanceAttenuationModel::LogReverse, Falloff), distance)),
                         logReverse, 1.0e-6));

        DistanceAttenuationSettings natural =
            Settings(DistanceAttenuationModel::NaturalSound, Falloff);
        const double alpha = d / falloff;
        const double db = DefaultNaturalSoundAttenuationDb;
        natural.naturalSoundFalloffMode = NaturalSoundFalloffMode::Continues;
        CHECK(test::Near(static_cast<double>(Attenuate(natural, distance)),
                         ClampUnit(PlatformMath::Pow(10.0, alpha * db / 20.0)), 1.0e-6, 1.0e-4));
        natural.naturalSoundFalloffMode = NaturalSoundFalloffMode::Hold;
        CHECK(test::Near(static_cast<double>(Attenuate(natural, distance)),
                         PlatformMath::Pow(10.0, math::Min(alpha, 1.0) * db / 20.0), 1.0e-6,
                         1.0e-4));
        natural.naturalSoundFalloffMode = NaturalSoundFalloffMode::Silent;
        const double silent = alpha < 1.0 ? PlatformMath::Pow(10.0, alpha * db / 20.0) : 0.0;
        CHECK(
            test::Near(static_cast<double>(Attenuate(natural, distance)), silent, 1.0e-6, 1.0e-4));
    }

    // 대표값: 중간 Linear 0.5, 끝에서 Logarithmic 0·Inverse 0.02, NaturalSound 끝 −60 dB.
    CHECK(Attenuate(Settings(DistanceAttenuationModel::Linear, Falloff), 500.0f) == 0.5f);
    CHECK(Attenuate(Settings(DistanceAttenuationModel::Logarithmic, Falloff), 1000.0f) == 0.0f);
    CHECK(test::Near(Attenuate(Settings(DistanceAttenuationModel::Inverse, Falloff), 1000.0f),
                     0.02f, 1.0e-7f));
    CHECK(test::Near(Attenuate(Settings(DistanceAttenuationModel::NaturalSound, Falloff), 1000.0f),
                     0.001f, 1.0e-7f));
}

TEST(Audio_DSP1_DistanceScaleFalloffFloorAndShape) {
    // falloff < 1은 1로 본다.
    CHECK(Attenuate(Settings(DistanceAttenuationModel::Linear, 0.5f), 0.5f) == 0.5f);
    CHECK(Attenuate(Settings(DistanceAttenuationModel::Linear, 0.0f), 0.25f) == 0.75f);

    DistanceAttenuationSettings scaled = Settings(DistanceAttenuationModel::Linear, 1000.0f);
    scaled.distanceScale = 2.0f;
    CHECK(Attenuate(scaled, 250.0f) == 0.5f);
    scaled.distanceScale = 0.0f;
    CHECK(Attenuate(scaled, 5000.0f) == 1.0f);

    CHECK(SphereAttenuationDistance(500.0f, 400.0f) == 100.0f);
    CHECK(SphereAttenuationDistance(300.0f, 400.0f) == 0.0f);
    CHECK(SphereAttenuationDistance(PlatformMath::QuietNaN<float>(), 400.0f) == 0.0f);
}

TEST(Audio_DSP1_DistanceRejectsInvalidSettings) {
    DistanceAttenuationSettings settings = Settings(DistanceAttenuationModel::Custom, 1000.0f);
    CHECK(EvaluateDistanceAttenuation(settings, 10.0f).Error() == ErrorCode::Unsupported);

    settings.model = DistanceAttenuationModel::Linear;
    CHECK(EvaluateDistanceAttenuation(settings, -1.0f).Error() == ErrorCode::InvalidArgument);
    CHECK(EvaluateDistanceAttenuation(settings, PlatformMath::QuietNaN<float>()).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(EvaluateDistanceAttenuation(settings, PlatformMath::Infinity<float>()).Error() ==
          ErrorCode::InvalidArgument);
    settings.falloffDistance = -1.0f;
    CHECK(EvaluateDistanceAttenuation(settings, 1.0f).Error() == ErrorCode::InvalidArgument);
    settings.falloffDistance = 1000.0f;
    settings.distanceScale = PlatformMath::Infinity<float>();
    CHECK(EvaluateDistanceAttenuation(settings, 1.0f).Error() == ErrorCode::InvalidArgument);
    settings.distanceScale = 1.0f;
    settings.naturalSoundAttenuationDb = 1.0f;
    CHECK(EvaluateDistanceAttenuation(settings, 1.0f).Error() == ErrorCode::InvalidArgument);
    settings.naturalSoundAttenuationDb = 0.0f;
    CHECK(EvaluateDistanceAttenuation(settings, 1.0f));
}

// omni 이득 행(3.42.6절, UE AudioMixerSource.cpp:2058–2160·2254–2310): OmniDirectional 1/N,
// Direct2D mono는 FL·FR 0.5·stereo는 L → FL·R → FR, Surround2D는 5.1 측면·7.1 후면을 더한다.
// LFE·FC는 0이다.
TEST(Audio_Panner_OmniGains) {
    float gains[MaxOutputChannels] = {};
    REQUIRE(ComputeOmniGains(ChannelLayout::Stereo, AudioOmniMode::OmniDirectional, 0, 1,
                             Span<float>(gains)));
    CHECK(gains[0] == 0.5f && gains[1] == 0.5f);
    REQUIRE(
        ComputeOmniGains(ChannelLayout::Stereo, AudioOmniMode::Direct2D, 0, 1, Span<float>(gains)));
    CHECK(gains[0] == MonoUpmixLinearGain && gains[1] == MonoUpmixLinearGain);
    REQUIRE(
        ComputeOmniGains(ChannelLayout::Stereo, AudioOmniMode::Direct2D, 1, 2, Span<float>(gains)));
    CHECK(gains[0] == 0.0f && gains[1] == 1.0f);
    // 5.1: FL FR FC LFE SL SR.
    REQUIRE(ComputeOmniGains(ChannelLayout::Surround51, AudioOmniMode::Surround2D, 0, 2,
                             Span<float>(gains)));
    CHECK(gains[0] == 1.0f && gains[1] == 0.0f && gains[2] == 0.0f && gains[3] == 0.0f &&
          gains[4] == 1.0f && gains[5] == 0.0f);
    REQUIRE(ComputeOmniGains(ChannelLayout::Surround51, AudioOmniMode::Direct2D, 0, 2,
                             Span<float>(gains)));
    CHECK(gains[0] == 1.0f && gains[4] == 0.0f);
    // 7.1은 후면이다.
    REQUIRE(ComputeOmniGains(ChannelLayout::Surround71, AudioOmniMode::Surround2D, 1, 2,
                             Span<float>(gains)));
    CHECK(gains[1] == 1.0f && gains[5] == 0.0f && gains[7] == 1.0f && gains[6] == 0.0f);
    REQUIRE(
        ComputeOmniGains(ChannelLayout::Mono, AudioOmniMode::Direct2D, 0, 1, Span<float>(gains)));
    CHECK(gains[0] == 1.0f);
    CHECK(ComputeOmniGains(ChannelLayout::Stereo, AudioOmniMode::Direct2D, 2, 2, Span<float>(gains))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(ComputeOmniGains(ChannelLayout::Stereo, AudioOmniMode::Direct2D, 0, 3, Span<float>(gains))
              .Error() == ErrorCode::InvalidArgument);
}

} // namespace audio_spatial_tests_file
