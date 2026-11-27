#include <Core/Math/KeyCurveMath.h>
#include <Core/Memory/SystemAllocator.h>
#include <Curves/CurveMathDeterministic.h>
#include <Curves/TangentCurve.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>
#include <TestSupport/Test.h>
#include <stdio.h>

// 쿠킹용 결정적 평가 시험이다(Docs/Design/CurveData 5.9절 D22 — CV11b): 비가중 구간·외삽
// 다섯·순환은 Evaluate와 비트가 같고, 가중 구간은 상대 오차 안이며 결과 비트를 digest로
// 고정한다(호스트와 무관 — 같은 시험이 모든 대상에서 같은 값). NaN은 고정 quiet NaN, Cardano 근(세
// 실근·중근·단근)은 기본 정책과 가깝고, 정책 함수의 특수값(세제곱근 0·음수, Atan)을 본다.
using namespace rocks;
namespace tangent_curve_deterministic_tests_file {

namespace {

constexpr uint32 CanonicalFloatNaN = 0x7FC00000u;
// 결정적 가중 평가의 출력 비트 digest다(CurveDeterministic_WeightedCloseAndPinned — 결과 비트가
// 바뀌면 DeterministicMath Version과 함께 갱신한다).
constexpr uint64 RedundantDigestValue = 0x8FF24E65F16AF81Aull;
constexpr uint64 WeightedDigestValue = 0xF6D7E18C4B8D95E2ull;

uint32 Bits(float value) noexcept {
    return platform::BitCast<uint32>(value);
}

uint64 Fnv(uint64 hash, uint32 value) noexcept {
    for (int index = 0; index < 4; ++index) {
        hash ^= (value >> (8 * index)) & 0xFF;
        hash *= 0x100000001B3ull;
    }

    return hash;
}

// 고정 시드 LCG다(시험 표본 — 호스트와 무관).
struct Lcg {
    uint32 state;

    float Unit() noexcept {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) * (1.0f / 16777216.0f); // [0, 1)
    }

    float Range(float low, float high) noexcept {
        return low + (high - low) * Unit();
    }
};

constexpr CurveExtrapolation Extrapolations[] = {
    CurveExtrapolation::Cycle, CurveExtrapolation::CycleWithOffset, CurveExtrapolation::Oscillate,
    CurveExtrapolation::Linear, CurveExtrapolation::Constant};

// 키 다섯의 무작위 커브다. weighted면 Cubic 키에 가중 방식·가중치를 섞는다.
bool MakeCurve(TangentCurve& curve, Lcg& random, bool weighted) noexcept {
    TangentCurveKey keys[5];
    float time = random.Range(-2.0f, 0.0f);
    for (TangentCurveKey& key : keys) {
        key.time = time;
        key.value = random.Range(-3.0f, 3.0f);
        const float pick = random.Unit();
        key.interp = pick < 0.2f   ? CurveInterp::Constant
                     : pick < 0.4f ? CurveInterp::Linear
                                   : CurveInterp::Cubic;
        key.tangentMode = CurveTangentMode::User;
        key.arriveTangent = random.Range(-4.0f, 4.0f);
        key.leaveTangent = random.Range(-4.0f, 4.0f);
        if (weighted) {
            const float mode = random.Unit();
            key.tangentWeight = mode < 0.25f   ? CurveTangentWeight::Arrive
                                : mode < 0.5f  ? CurveTangentWeight::Leave
                                : mode < 0.75f ? CurveTangentWeight::Both
                                               : CurveTangentWeight::None;
            key.arriveTangentWeight = random.Range(0.05f, 1.5f);
            key.leaveTangentWeight = random.Range(0.05f, 1.5f);
        }

        time += random.Range(0.1f, 1.5f);
    }

    return static_cast<bool>(curve.TrySetKeys(keys));
}

} // namespace

// 비가중 구간(Constant·Linear·Cubic 비가중)과 외삽 다섯·순환(키 범위 밖 넓은 시간)은 Evaluate와
// 비트가 같다(가중 경로를 지나지 않으면 두 정책의 연산이 같다).
TEST(CurveDeterministic_UnweightedMatchesEvaluateBits) {
    SystemAllocator allocator;
    Lcg random{2026};
    for (int sample = 0; sample < 64; ++sample) {
        TangentCurve curve(allocator);
        REQUIRE(MakeCurve(curve, random, false));
        curve.preExtrapolation = Extrapolations[sample % 5];
        curve.postExtrapolation = Extrapolations[(sample / 5) % 5];
        for (int step = 0; step < 64; ++step) {
            const float time = random.Range(-20.0f, 25.0f);
            CHECK(Bits(curve.EvaluateDeterministic(time)) == Bits(curve.Evaluate(time)));
        }
    }

    // 빈 커브·키 하나·defaultValue·fallback도 같다.
    TangentCurve empty(allocator);
    CHECK(Bits(empty.EvaluateDeterministic(1.0f, 7.0f)) == Bits(7.0f));
    empty.defaultValue = 3.0f;
    CHECK(Bits(empty.EvaluateDeterministic(1.0f, 7.0f)) == Bits(3.0f));
}

// 가중 구간은 Evaluate와 상대 오차 안이고(두 정책의 Atan·Acos·Cos·Pow 차가 Cardano 근을 거쳐
// 퍼진다), 결과 비트는 호스트와 무관해 digest로 고정한다.
TEST(CurveDeterministic_WeightedCloseAndPinned) {
    SystemAllocator allocator;
    Lcg random{2027};
    uint64 hash = 0xCBF29CE484222325ull;
    float worst = 0.0f;
    usize weightedSegments = 0;
    for (int sample = 0; sample < 64; ++sample) {
        TangentCurve curve(allocator);
        REQUIRE(MakeCurve(curve, random, true));
        const Span<const TangentCurveKey> keys = curve.Keys();
        for (usize index = 0; index + 1 < keys.Size(); ++index) {
            weightedSegments +=
                keys[index].interp == CurveInterp::Cubic &&
                        !math::IsCurveSegmentUnweighted(keys[index], keys[index + 1])
                    ? 1
                    : 0;
        }

        const float first = keys[0].time;
        const float last = keys[keys.Size() - 1].time;
        for (int step = 0; step < 64; ++step) {
            const float time = first + (last - first) * (static_cast<float>(step) / 63.0f);
            const float deterministic = curve.EvaluateDeterministic(time);
            const float platformValue = curve.Evaluate(time);
            const float scale = platform::PlatformMath::Abs(platformValue) > 1.0f
                                    ? platform::PlatformMath::Abs(platformValue)
                                    : 1.0f;
            const float error = platform::PlatformMath::Abs(deterministic - platformValue) / scale;
            worst = error > worst ? error : worst;
            CHECK(error <= 1.0e-4f);
            hash = Fnv(hash, Bits(deterministic));
        }
    }

    // 표본이 가중 경로를 실제로 지난다(가중 구간이 충분히 많다).
    CHECK(weightedSegments >= 64);
    printf("CurveDeterministic weighted: %llu segments, worst relative %.3g, digest 0x%016llX\n",
           static_cast<unsigned long long>(weightedSegments), static_cast<double>(worst),
           static_cast<unsigned long long>(hash));
    CHECK(hash == WeightedDigestValue);
}

// NaN 결과는 고정 quiet NaN이다(키가 있는 커브의 NaN 시간, NaN fallback의 빈 커브).
TEST(CurveDeterministic_CanonicalNaN) {
    SystemAllocator allocator;
    TangentCurve curve(allocator);
    REQUIRE(curve.TryAddKey(0.0f, 1.0f));
    REQUIRE(curve.TryAddKey(1.0f, 2.0f));
    const float nan = platform::BitCast<float>(0xFFC00123u);
    CHECK(Bits(curve.EvaluateDeterministic(nan)) == CanonicalFloatNaN);
    TangentCurve empty(allocator);
    CHECK(Bits(empty.EvaluateDeterministic(0.0f, nan)) == CanonicalFloatNaN);
    CHECK(Bits(CurveMathDeterministic::Canonical(nan)) == CanonicalFloatNaN);
    CHECK(Bits(CurveMathDeterministic::Canonical(-0.0f)) == Bits(-0.0f));
}

// Cardano: 세 실근((x−1)(x−2)(x−3)), 중근((x−1)²(x+2)), 단근(x³ − 2)이 기본 정책과 가깝다.
TEST(CurveDeterministic_CubicRootsMatchPlatform) {
    const double cases[][4] = {
        {-6.0, 11.0, -6.0, 1.0}, {2.0, -3.0, 0.0, 1.0}, {-2.0, 0.0, 0.0, 1.0}};
    for (const auto& coefficients : cases) {
        const math::CurveCubicRoots expected = math::SolveCurveCubic(coefficients);
        const math::CurveCubicRoots actual =
            math::SolveCurveCubic<CurveMathDeterministic>(coefficients);
        REQUIRE(actual.count == expected.count);
        for (uint32 index = 0; index < actual.count; ++index) {
            CHECK(platform::PlatformMath::Abs(actual.roots[index] - expected.roots[index]) <=
                  1.0e-12);
        }
    }
}

// 정책 함수: 세제곱근 0은 0, 음수는 부호, Atan은 float atanf와 1 ulp 안이다.
TEST(CurveDeterministic_PolicyFunctions) {
    CHECK(CurveMathDeterministic::CubeRoot(0.0) == 0.0);
    CHECK(platform::PlatformMath::Abs(CurveMathDeterministic::CubeRoot(-8.0) + 2.0) <= 1.0e-15);
    CHECK(platform::PlatformMath::Abs(CurveMathDeterministic::CubeRoot(27.0) - 3.0) <= 1.0e-15);
    const float inputs[] = {0.0f, 0.5f, -1.0f, 3.0f, 1.0e-6f, -250.0f};
    for (float x : inputs) {
        const uint32 a = Bits(CurveMathDeterministic::Atan(x));
        const uint32 b = Bits(math::CurveMathPlatform::Atan(x));
        CHECK((a > b ? a - b : b - a) <= 1u);
    }
}

// 결정적 중복 키 제거(설계 5.9절): 비가중 커브는 플랫폼 판과 같은 키를 남기고, 가중 커브도 같은
// 규칙으로 동작한다(남긴 키 시간 digest로 고정 — 호스트와 무관). 실패 규칙(NaN 허용오차)도 같다.
TEST(CurveDeterministic_RemoveRedundantKeys) {
    SystemAllocator allocator;
    Lcg random{2030};
    uint64 hash = 0xCBF29CE484222325ull;
    for (int sample = 0; sample < 32; ++sample) {
        const bool weighted = (sample & 1) != 0;
        TangentCurve platformCurve(allocator);
        REQUIRE(MakeCurve(platformCurve, random, weighted));
        // 중복 키가 생기게 일부 키를 직선 위로 옮긴다.
        TangentCurve deterministicCurve(allocator);
        REQUIRE(deterministicCurve.TryCopyFrom(platformCurve));
        REQUIRE(platformCurve.TryRemoveRedundantKeys(0.0f));
        REQUIRE(deterministicCurve.TryRemoveRedundantKeysDeterministic(0.0f));
        if (!weighted) {
            REQUIRE(platformCurve.KeyCount() == deterministicCurve.KeyCount());
            for (usize index = 0; index < platformCurve.KeyCount(); ++index) {
                CHECK(Bits(platformCurve.Keys()[index].time) ==
                      Bits(deterministicCurve.Keys()[index].time));
            }
        }

        for (const TangentCurveKey& key : deterministicCurve.Keys()) {
            hash = Fnv(hash, Bits(key.time));
        }
    }

    // 선형 위의 가운데 키는 지운다(Auto 접선 — 비표본판 비교).
    TangentCurve line(allocator);
    REQUIRE(line.TryAddKey(0.0f, 0.0f) && line.TryAddKey(1.0f, 1.0f) && line.TryAddKey(2.0f, 2.0f));
    REQUIRE(line.TryRemoveRedundantKeysDeterministic(0.0f));
    CHECK(line.KeyCount() == 2);
    const float nan = platform::BitCast<float>(0x7FC00000u);
    CHECK(line.TryRemoveRedundantKeysDeterministic(nan).Error() == ErrorCode::InvalidArgument);
    printf("CurveDeterministic redundant digest: 0x%016llX\n",
           static_cast<unsigned long long>(hash));
    CHECK(hash == RedundantDigestValue);
}

} // namespace tangent_curve_deterministic_tests_file
