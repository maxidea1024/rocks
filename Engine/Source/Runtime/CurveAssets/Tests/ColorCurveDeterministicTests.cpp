#include <Core/Memory/SystemAllocator.h>
#include <CurveAssets/ColorCurveAsset.h>
#include <CurveAssets/ColorCurveEvaluate.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>
#include <TestSupport/Test.h>
#include <math.h>
#include <stdio.h>

// 쿠킹용 결정적 색 평가 시험이다(Docs/Design/CurveAssets 2.4.5절 — CA7, CAD12): 결정적 거듭제곱의 C
// powf 의미(±0 밑의 부호·무한대, 음수 밑의 정수 지수·비정수 NaN, 밑 1·지수 0·밑 −1과 ±Inf, NaN
// 고정)와 정확도, 조정 없는·항등 조정 색이 런타임 판과 비트가 같고 조정 색은 상대 오차 안이며, 결과
// NaN은 고정 quiet NaN이다.
using namespace rocks;

namespace color_curve_deterministic_tests_file {

namespace {

constexpr uint32 CanonicalFloatNaN = 0x7FC00000u;
// 결정적 조정 색 평가의 출력 비트 digest다(ColorCurveDeterministic_AdjustedPinned — 호스트와 무관,
// 결과 비트가 바뀌면 DeterministicMath Version과 함께 갱신한다).
constexpr uint64 AdjustedDigestValue = 0xD9D1B7EA4E4AA9C9ull;

uint64 Fnv(uint64 hash, uint32 value) noexcept {
    for (int index = 0; index < 4; ++index) {
        hash ^= (value >> (8 * index)) & 0xFF;
        hash *= 0x100000001B3ull;
    }

    return hash;
}

uint32 Bits(float value) noexcept {
    return platform::BitCast<uint32>(value);
}

float FromBits(uint32 bits) noexcept {
    return platform::BitCast<float>(bits);
}

uint32 UlpDistance(float left, float right) noexcept {
    const uint32 a = Bits(left);
    const uint32 b = Bits(right);
    if (((a ^ b) & 0x80000000u) != 0) {
        return (a & 0x7FFFFFFFu) + (b & 0x7FFFFFFFu);
    }

    return a > b ? a - b : b - a;
}

} // namespace

// C powf 특수값(C11 F.10.4.4)과 같은 결과다. NaN 결과는 고정 quiet NaN이다.
TEST(ColorCurveDeterministic_PowfSpecialValues) {
    const float infinity = FromBits(0x7F800000u);
    const float nan = FromBits(0xFFC00123u);
    struct Case {
        float base;
        float exponent;
        uint32 expected;
    };
    const Case cases[] = {
        {0.0f, -1.0f, 0x7F800000u},
        {-0.0f, -1.0f, 0xFF800000u},
        {-0.0f, -2.0f, 0x7F800000u},
        {-0.0f, 3.0f, 0x80000000u},
        {-0.0f, 2.0f, 0x00000000u},
        {0.0f, 0.5f, 0x00000000u},
        {-2.0f, 3.0f, Bits(-8.0f)},
        {-2.0f, 2.0f, Bits(4.0f)},
        {-2.0f, -1.0f, Bits(-0.5f)},
        {-2.0f, 0.5f, CanonicalFloatNaN},
        {-1.0f, infinity, Bits(1.0f)},
        {-1.0f, -infinity, Bits(1.0f)},
        {1.0f, nan, Bits(1.0f)},
        {nan, 0.0f, Bits(1.0f)},
        {nan, -0.0f, Bits(1.0f)},
        {nan, 2.0f, CanonicalFloatNaN},
        {2.0f, nan, CanonicalFloatNaN},
        {2.0f, infinity, 0x7F800000u},
        {0.5f, infinity, 0x00000000u},
        {2.0f, -infinity, 0x00000000u},
        {-infinity, 3.0f, 0xFF800000u},
        {-infinity, 2.0f, 0x7F800000u},
        {-infinity, -3.0f, 0x80000000u},
        {-infinity, 0.5f, 0x7F800000u},
        {-infinity, -0.5f, 0x00000000u},
        {infinity, -2.0f, 0x00000000u},
        {-0.5f, infinity, 0x00000000u},
        {-3.0f, infinity, 0x7F800000u},
        {-2.0f, 16777217.0f, Bits(powf(-2.0f, 16777217.0f))},
    };
    for (const Case& one : cases) {
        CHECK(Bits(DeterministicPowf(one.base, one.exponent)) == one.expected);
    }
}

// 정확도: 양수 밑과 음수 밑의 정수 지수에서 플랫폼 powf와 1 ulp 안이다(float로 한 번 반올림한
// double 결과 — 둘 다 거의 정확히 반올림된 값이다).
TEST(ColorCurveDeterministic_PowfAccuracy) {
    uint32 state = 2029;
    uint32 worst = 0;
    for (int sample = 0; sample < 4096; ++sample) {
        state = state * 1664525u + 1013904223u;
        const float base = static_cast<float>(state >> 8) * (1.0f / 16777216.0f) * 4.0f; // [0, 4)
        state = state * 1664525u + 1013904223u;
        const float exponent = static_cast<float>(state >> 8) * (1.0f / 16777216.0f) * 6.0f - 3.0f;
        const uint32 distance =
            UlpDistance(DeterministicPowf(base, exponent), powf(base, exponent));
        worst = distance > worst ? distance : worst;
        CHECK(distance <= 1u);
        const float integerExponent = static_cast<float>(static_cast<int>(exponent * 2.0f));
        CHECK(UlpDistance(DeterministicPowf(-base, integerExponent),
                          powf(-base, integerExponent)) <= 1u);
    }

    CHECK(worst <= 1u);
}

// 조정 없는 색과 항등 조정 색(거듭제곱을 지나지 않는다)은 런타임 판과 비트가 같고, 명도 지수·활기
// 조정 색은 상대 오차 안이다. NaN 결과는 고정 quiet NaN이다.
TEST(ColorCurveDeterministic_MatchesRuntime) {
    SystemAllocator allocator;
    ColorCurveAsset curve(allocator);
    REQUIRE(curve.r.TryAddKey(0.0f, 0.2f) && curve.r.TryAddKey(1.0f, 1.4f));
    REQUIRE(curve.g.TryAddKey(0.0f, 0.7f) && curve.g.TryAddKey(1.0f, -0.1f));
    REQUIRE(curve.b.TryAddKey(0.0f, 0.4f) && curve.b.TryAddKey(0.5f, 0.9f));
    REQUIRE(curve.a.TryAddKey(0.0f, 1.0f) && curve.a.TryAddKey(1.0f, 0.3f));
    for (int step = 0; step <= 32; ++step) {
        const float time = static_cast<float>(step) / 32.0f;
        const LinearColor raw = curve.EvaluateUnadjusted(time);
        const LinearColor rawDeterministic = curve.EvaluateUnadjustedDeterministic(time);
        CHECK(raw == rawDeterministic);
        CHECK(curve.Evaluate(time) == curve.EvaluateDeterministic(time));
    }

    curve.adjustBrightnessCurve = 0.6f;
    curve.adjustVibrance = 0.4f;
    curve.adjustHue = 25.0f;
    for (int step = 0; step <= 32; ++step) {
        const float time = static_cast<float>(step) / 32.0f;
        const LinearColor runtime = curve.Evaluate(time);
        const LinearColor cooked = curve.EvaluateDeterministic(time);
        const float channels[4][2] = {{runtime.r, cooked.r},
                                      {runtime.g, cooked.g},
                                      {runtime.b, cooked.b},
                                      {runtime.a, cooked.a}};
        for (const auto& pair : channels) {
            const float scale = platform::PlatformMath::Abs(pair[0]) > 1.0f
                                    ? platform::PlatformMath::Abs(pair[0])
                                    : 1.0f;
            CHECK(platform::PlatformMath::Abs(pair[0] - pair[1]) <= 1.0e-6f * scale);
        }
    }

    // NaN 조정(명도 배율 NaN)은 모든 RGB 성분이 고정 quiet NaN이다. 원본이 HDR인 시간(t = 1 —
    // R 1.4)이라 명도를 자르지 않는다(LDR이면 UE Clamp가 NaN을 1로 바꾼다).
    curve.adjustBrightness = FromBits(0xFFC00321u);
    const LinearColor broken = curve.EvaluateDeterministic(1.0f);
    CHECK(Bits(broken.r) == CanonicalFloatNaN && Bits(broken.g) == CanonicalFloatNaN &&
          Bits(broken.b) == CanonicalFloatNaN);
}

// 조정 경로(명도 지수·활기의 결정적 거듭제곱, HSV)의 결과 비트를 digest로 고정한다(호스트 사이
// 비트 대조 — 같은 시험이 모든 대상에서 같은 값). 조정값은 활기·명도 지수가 켜지는 조합들이다.
TEST(ColorCurveDeterministic_AdjustedPinned) {
    SystemAllocator allocator;
    ColorCurveAsset curve(allocator);
    REQUIRE(curve.r.TryAddKey(0.0f, 0.05f) && curve.r.TryAddKey(1.0f, 2.5f));
    REQUIRE(curve.g.TryAddKey(0.0f, 0.9f) && curve.g.TryAddKey(1.0f, -0.4f));
    REQUIRE(curve.b.TryAddKey(0.0f, 0.3f) && curve.b.TryAddKey(1.0f, 0.6f));
    REQUIRE(curve.a.TryAddKey(0.0f, 0.2f) && curve.a.TryAddKey(1.0f, 1.0f));
    const float brightnessCurves[] = {0.35f, 0.8f, 1.7f};
    const float vibrances[] = {0.0f, 0.3f, 0.9f};
    uint64 hash = 0xCBF29CE484222325ull;
    for (float brightnessCurve : brightnessCurves) {
        for (float vibrance : vibrances) {
            curve.adjustBrightnessCurve = brightnessCurve;
            curve.adjustVibrance = vibrance;
            curve.adjustHue = 40.0f;
            curve.adjustSaturation = 0.8f;
            for (int step = 0; step <= 64; ++step) {
                const LinearColor color =
                    curve.EvaluateDeterministic(static_cast<float>(step) / 64.0f);
                hash = Fnv(hash, Bits(color.r));
                hash = Fnv(hash, Bits(color.g));
                hash = Fnv(hash, Bits(color.b));
                hash = Fnv(hash, Bits(color.a));
            }
        }
    }

    printf("ColorCurveDeterministic adjusted digest: 0x%016llX\n",
           static_cast<unsigned long long>(hash));
    CHECK(hash == AdjustedDigestValue);
}

} // namespace color_curve_deterministic_tests_file
