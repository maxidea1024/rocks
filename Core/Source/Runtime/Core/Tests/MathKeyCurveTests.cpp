#include <TestSupport/Test.h>
#include <TestSupport/MathTestSupport.h>
#include <Core/Math/KeyCurveMath.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>

// KeyCurveMath의 독립 고정 벡터와 경계 시험이다(Docs/Design/CurveData.ko.md 16절). 기대값은 UE 참조
// 코드(MathKeyCurveGoldenTests.cpp)를 거치지 않고 손으로 계산했다. 계산 과정은 각 사례 옆 주석에
// 있다. float 연산 결과의 비트까지 정한 값은 float 연산 하나하나를 double로 계산해 float로
// 반올림하는 방식 (double은 float 연산 하나를 정확히 반올림한다)으로 계수를 대입해 얻었다.
namespace rocks::test {

namespace key_curve_vectors {

enum class Interp : uint8 {
    Linear = 0,
    Constant = 1,
    Cubic = 2
};
enum class Weight : uint8 {
    None = 0,
    Arrive = 1,
    Leave = 2,
    Both = 3
};

struct Key {
    float time = 0.0f;
    float value = 0.0f;
    Interp interp = Interp::Linear;
    Weight tangentWeight = Weight::None;
    float arriveTangent = 0.0f;
    float arriveTangentWeight = 0.0f;
    float leaveTangent = 0.0f;
    float leaveTangentWeight = 0.0f;
};

Key MakeKey(float time, float value, Interp interp) noexcept {
    Key key;
    key.time = time;
    key.value = value;
    key.interp = interp;
    return key;
}

uint32 FloatBits(float value) noexcept {
    return platform::BitCast<uint32>(value);
}

constexpr float FloatNaN = platform::PlatformMath::QuietNaN<float>();
constexpr float FloatInfinity = platform::PlatformMath::Infinity<float>();

} // namespace key_curve_vectors

namespace math_key_curve_tests_file {

using namespace key_curve_vectors;

TEST(KeyCurveMath_LinearAndConstantSegments) {
    // Linear (0, 0)–(2, 4), time 0.5: α = 0.25, 0 + 0.25·4 = 1.
    const Key a = MakeKey(0.0f, 0.0f, Interp::Linear);
    const Key b = MakeKey(2.0f, 4.0f, Interp::Linear);
    CHECK(math::EvalCurveSegment(a, b, 0.5f) == 1.0f);
    CHECK(math::EvalCurveSegment(a, b, 0.0f) == 0.0f);
    CHECK(math::EvalCurveSegment(a, b, 2.0f) == 4.0f);
    // 구간 밖도 같은 식(외삽): time 3 → α 1.5 → 6.
    CHECK(math::EvalCurveSegment(a, b, 3.0f) == 6.0f);

    // Constant (0, 1)–(2, 5): time < 2면 1, time = 2면 5(뒤 키 시간 비교).
    const Key c = MakeKey(0.0f, 1.0f, Interp::Constant);
    const Key d = MakeKey(2.0f, 5.0f, Interp::Linear);
    CHECK(math::EvalCurveSegment(c, d, 1.0f) == 1.0f);
    CHECK(math::EvalCurveSegment(c, d, 1.9999999f) == 1.0f);
    CHECK(math::EvalCurveSegment(c, d, 2.0f) == 5.0f);

    // 길이 0(같은 시간 키): 앞 키 값.
    const Key e = MakeKey(1.0f, 7.0f, Interp::Cubic);
    const Key f = MakeKey(1.0f, 9.0f, Interp::Linear);
    CHECK(math::EvalCurveSegment(e, f, 1.0f) == 7.0f);
    CHECK(math::EvalCurveSegment(e, f, 5.0f) == 7.0f);

    // 끝의 차가 넘치는 Linear: Rocks Lerp는 a(1 − t) + bt라 유한하다(15절 2). −3e38·0.5 + 3e38·0.5
    // = 0.
    const Key g = MakeKey(0.0f, -3.0e38f, Interp::Linear);
    const Key h = MakeKey(1.0f, 3.0e38f, Interp::Linear);
    CHECK(math::EvalCurveSegment(g, h, 0.5f) == 0.0f);

    // NaN 시간은 NaN이다.
    CHECK(platform::PlatformMath::IsNaN(math::EvalCurveSegment(a, b, FloatNaN)));
}

TEST(KeyCurveMath_UnweightedCubicHandExpansion) {
    // (0, 0, 출발 3)–(1, 3, 도착 3): p1 = 0 + (3·1)·(1/3f) = 3·0.33333334 = 1.0000000298 → float 1,
    // p2 = 3 − 1 = 2. 제어점이 0, 1, 2, 3으로 곧은 선이라 Bezier 값은 3α다.
    Key a = MakeKey(0.0f, 0.0f, Interp::Cubic);
    a.leaveTangent = 3.0f;
    Key b = MakeKey(1.0f, 3.0f, Interp::Cubic);
    b.arriveTangent = 3.0f;
    CHECK(math::EvalCurveSegment(a, b, 0.5f) == 1.5f);
    CHECK(math::EvalCurveSegment(a, b, 0.25f) == 0.75f);
    CHECK(math::BezierCurveValue(0.0f, 1.0f, 2.0f, 3.0f, 0.5f) == 1.5f);

    // (0, 0, 접선 0)–(1, 1, 접선 0): 제어점 0, 0, 1, 1 → 3α² − 2α³. α = 0.25: De Casteljau
    // p01 = 0, p12 = 0.25, p23 = 1, p012 = 0.0625, p123 = 0.4375, 결과 0.0625 + 0.25·0.375 =
    // 0.15625.
    Key c = MakeKey(0.0f, 0.0f, Interp::Cubic);
    Key d = MakeKey(1.0f, 1.0f, Interp::Cubic);
    CHECK(math::EvalCurveSegment(c, d, 0.25f) == 0.15625f);
    CHECK(math::EvalCurveSegment(c, d, 0.5f) == 0.5f);
    // 비율 0·1은 끝 값 그대로다.
    CHECK(math::EvalCurveSegment(c, d, 0.0f) == 0.0f);
    CHECK(math::EvalCurveSegment(c, d, 1.0f) == 1.0f);

    // 뒤 키가 Linear여도 앞 키가 Cubic이면 Cubic 구간이다. 뒤 키에 도착 가중(Arrive)이 있으면 가중
    // 구간이고, 가중치를 기본 거리(1/3)로 두면 비가중과 거의 같은 곡선이다.
    Key e = MakeKey(0.0f, 0.0f, Interp::Cubic);
    Key f = MakeKey(1.0f, 1.0f, Interp::Linear);
    f.tangentWeight = Weight::Arrive;
    f.arriveTangentWeight = 1.0f / 3.0f;
    CHECK(!math::IsCurveSegmentUnweighted(e, f));
    CHECK(Near(math::EvalCurveSegment(e, f, 0.25f), 0.15625f, 1.0e-6f));
}

TEST(KeyCurveMath_UnweightedCubicMatchesDoublePolynomial) {
    // 비가중 Cubic 구간을 double 멱급수 Bezier B(α) = (1−α)³p0 + 3(1−α)²α p1 + 3(1−α)α² p2 + α³p3과
    // 상대 오차로 대조한다(De Casteljau가 아닌 독립 식, 고정 seed).
    MathRandom random(0x3701);
    int32 failures = 0;
    for (int32 index = 0; index < 20000; ++index) {
        Key a = MakeKey(static_cast<float>(random.Range(-100.0, 100.0)),
                        static_cast<float>(random.Range(-50.0, 50.0)), Interp::Cubic);
        Key b = MakeKey(a.time + static_cast<float>(random.Range(0.01, 20.0)),
                        static_cast<float>(random.Range(-50.0, 50.0)), Interp::Cubic);
        a.leaveTangent = static_cast<float>(random.Range(-5.0, 5.0));
        b.arriveTangent = static_cast<float>(random.Range(-5.0, 5.0));
        const float time = a.time + static_cast<float>(random.Unit()) * (b.time - a.time);
        const double length = static_cast<double>(b.time) - a.time;
        const double alpha = (static_cast<double>(time) - a.time) / length;
        const double p0 = a.value;
        const double p1 = p0 + a.leaveTangent * length / 3.0;
        const double p3 = b.value;
        const double p2 = p3 - b.arriveTangent * length / 3.0;
        const double u = 1.0 - alpha;
        const double expected = u * u * u * p0 + 3.0 * u * u * alpha * p1 +
                                3.0 * u * alpha * alpha * p2 + alpha * alpha * alpha * p3;
        const double scale = 1.0 + platform::PlatformMath::Abs(p0) +
                             platform::PlatformMath::Abs(p1) + platform::PlatformMath::Abs(p2) +
                             platform::PlatformMath::Abs(p3);
        const double actual = math::EvalCurveSegment(a, b, time);
        if (platform::PlatformMath::Abs(actual - expected) > 1.0e-5 * scale) {
            ++failures;
        }
    }

    CHECK(failures == 0);
}

TEST(KeyCurveMath_WeightedHandSolvedCubic) {
    // 접선 0, 가중치 1/3(Both): atan(0) = 0, CurveSinCos(0) = (0, 1)이라 시간 제어점이 1/3·2/3
    // 근처로 시간 Bezier가 거의 x(s) = s다. 값은 비가중과 같은 3α² − 2α³에 가깝다.
    Key a = MakeKey(0.0f, 0.0f, Interp::Cubic);
    a.tangentWeight = Weight::Both;
    a.leaveTangentWeight = 1.0f / 3.0f;
    Key b = MakeKey(1.0f, 1.0f, Interp::Cubic);
    b.tangentWeight = Weight::Both;
    b.arriveTangentWeight = 1.0f / 3.0f;
    CHECK(Near(math::EvalCurveSegment(a, b, 0.5f), 0.5f, 1.0e-6f));
    CHECK(Near(math::EvalCurveSegment(a, b, 0.25f), 0.15625f, 1.0e-6f));

    // 가중치 1.5(Both): 시간 제어점 0, 1.5, −0.5, 1 → x(s) = 7s³ − 10.5s² + 4.5s(단조 아님).
    // α = 0.5: 7s³ − 10.5s² + 4.5s − 0.5 = (s − 0.5)(7s² − 7s + 1) → 근 0.5, (7 ± √21)/14 =
    // 0.17267, 0.82733. 셋 다 [0, 1]이라 가장 큰 근 s = 0.8273268을 쓴다. 값 제어점 0, 0, 1, 1 → y
    // = 3s² − 2s³ = 0.9208488.
    a.leaveTangentWeight = 1.5f;
    b.arriveTangentWeight = 1.5f;
    CHECK(Near(math::EvalCurveSegment(a, b, 0.5f), 0.9208488f, 2.0e-6f));

    // 길이 0이면 WeightedCurveSegment도 앞 키 값이다.
    Key c = b;
    c.value = 4.0f;
    CHECK(math::WeightedCurveSegment(b, c, 1.0f) == 1.0f);
}

TEST(KeyCurveMath_SinCosPolynomial) {
    float sine = 1.0f;
    float cosine = 0.0f;
    // 0: y = 0, 사인 다항식 × 0 = 0, 코사인 다항식 상수항 1.
    math::CurveSinCos(0.0f, sine, cosine);
    CHECK(sine == 0.0f && cosine == 1.0f);

    // 0.5: 접기 없음(y = 0.5, y² = 0.25). 계수를 float 연산 순서대로 대입하면
    //   사인 = 0.4794255495071411(0x3EF57744), 코사인 = 0.8775825500488281(0x3F60A940).
    // libm 값 sin 0.4794255386, cos 0.8775825619와 1ulp 안이다.
    math::CurveSinCos(0.5f, sine, cosine);
    CHECK(FloatBits(sine) == 0x3EF57744u);
    CHECK(FloatBits(cosine) == 0x3F60A940u);

    // 접기: 3은 π/2보다 커서 y = π − 3, 부호 −1(코사인). sin 3 = 0.14112, cos 3 = −0.98999.
    math::CurveSinCos(3.0f, sine, cosine);
    CHECK(Near(sine, 0.14112000805986721f, 2.0e-7f));
    CHECK(Near(cosine, -0.98999249660044542f, 2.0e-7f));

    // 2π 축소: 10 → quotient round(10/(2π)) = 2, y = 10 − 4π = −2.5664 → 접기.
    math::CurveSinCos(10.0f, sine, cosine);
    CHECK(Near(sine, -0.54402111088936981f, 2.0e-6f));
    CHECK(Near(cosine, -0.83907152907645244f, 2.0e-6f));

    // NaN은 NaN이다(±Inf는 UE와 같은 의미 없는 값 — golden 시험이 UE 식과 비트로 대조한다).
    math::CurveSinCos(FloatNaN, sine, cosine);
    CHECK(platform::PlatformMath::IsNaN(sine) && platform::PlatformMath::IsNaN(cosine));
}

TEST(KeyCurveMath_SolveCubicHandRoots) {
    // (x − 1)(x − 2)(x − 3) = x³ − 6x² + 11x − 6: 근 3개(acos 갈래).
    const double three[4] = {-6.0, 11.0, -6.0, 1.0};
    const math::CurveCubicRoots solved = math::SolveCurveCubic(three);
    REQUIRE(solved.count == 3);
    double sorted[3] = {solved.roots[0], solved.roots[1], solved.roots[2]};
    for (int32 pass = 0; pass < 2; ++pass) {
        for (int32 index = 0; index < 2; ++index) {
            if (sorted[index] > sorted[index + 1]) {
                const double swap = sorted[index];
                sorted[index] = sorted[index + 1];
                sorted[index + 1] = swap;
            }
        }
    }

    CHECK(Near(sorted[0], 1.0, 1.0e-12) && Near(sorted[1], 2.0, 1.0e-12) &&
          Near(sorted[2], 3.0, 1.0e-12));

    // x³ − 8 = 0: 실근 하나 2(pow 갈래).
    const double one[4] = {-8.0, 0.0, 0.0, 1.0};
    const math::CurveCubicRoots single = math::SolveCurveCubic(one);
    CHECK(single.count == 1 && Near(single.roots[0], 2.0, 1.0e-12));

    // x³ = 0: 삼중근 0.
    const double triple[4] = {0.0, 0.0, 0.0, 1.0};
    const math::CurveCubicRoots zero = math::SolveCurveCubic(triple);
    CHECK(zero.count == 1 && zero.roots[0] == 0.0);
}

TEST(KeyCurveMath_CycleTimeInt32Path) {
    // 범위 안·경계: 그대로, 주기 0.
    math::CurveCycle cycle = math::CycleCurveTime(0.0f, 2.0f, 1.0f);
    CHECK(cycle.time == 1.0f && cycle.cycleCount == 0.0 && !cycle.wide);
    cycle = math::CycleCurveTime(0.0f, 2.0f, 2.0f);
    CHECK(cycle.time == 2.0f && cycle.cycleCount == 0.0);
    cycle = math::CycleCurveTime(0.0f, 2.0f, 0.0f);
    CHECK(cycle.time == 0.0f && cycle.cycleCount == 0.0);

    // 뒤쪽 5: 몫 (2 − 5)/2 = −1.5 → floor −2 → 5 + 2·(−2) = 1, 주기 2.
    cycle = math::CycleCurveTime(0.0f, 2.0f, 5.0f);
    CHECK(cycle.time == 1.0f && cycle.cycleCount == 2.0);
    // 뒤쪽 정확한 배수: 4 → 몫 −1 → 2(max), 주기 1. 6 → 몫 −2 → 2, 주기 2.
    cycle = math::CycleCurveTime(0.0f, 2.0f, 4.0f);
    CHECK(cycle.time == 2.0f && cycle.cycleCount == 1.0);
    cycle = math::CycleCurveTime(0.0f, 2.0f, 6.0f);
    CHECK(cycle.time == 2.0f && cycle.cycleCount == 2.0);

    // 앞쪽 −0.5: 몫 −0.25 → floor −1 → −0.5 + 2 = 1.5, 주기 1. 정확한 배수 −2 → 0(min), 주기 1.
    cycle = math::CycleCurveTime(0.0f, 2.0f, -0.5f);
    CHECK(cycle.time == 1.5f && cycle.cycleCount == 1.0);
    cycle = math::CycleCurveTime(0.0f, 2.0f, -2.0f);
    CHECK(cycle.time == 0.0f && cycle.cycleCount == 1.0);
    cycle = math::CycleCurveTime(0.0f, 2.0f, -4.0f);
    CHECK(cycle.time == 0.0f && cycle.cycleCount == 2.0);

    // 앞쪽에서 반올림으로 max에 닿으면 min으로: −1e-8 + 1 = 1(float) → 0.
    cycle = math::CycleCurveTime(0.0f, 1.0f, -1.0e-8f);
    CHECK(cycle.time == 0.0f && cycle.cycleCount == 1.0);

    // 몫 경계 2^30: 그 바로 앞 −(2^30 − 64)(float)는 int32 경로이고 정수 배수라 min, 주기 2^30 −
    // 64다.
    cycle = math::CycleCurveTime(0.0f, 1.0f, -1073741760.0f);
    CHECK(!cycle.wide && cycle.time == 0.0f && cycle.cycleCount == 1073741760.0);
    // 뒤쪽 2^30 − 64는 몫 1 − (2^30 − 64)가 float −(2^30 − 64)로 반올림되어 int32 경로이고 시간
    // 0(min)이 뒤쪽 바로잡기로 max가 된다(주기는 반올림된 몫 그대로 2^30 − 64 — UE와 같다).
    cycle = math::CycleCurveTime(0.0f, 1.0f, 1073741760.0f);
    CHECK(!cycle.wide && cycle.time == 1.0f && cycle.cycleCount == 1073741760.0);
    // 몫 2^30 이상은 double 경로다(UE SSE FloorToInt가 틀리는 범위 — int32에 들어가도). 정확한
    // 배수의 뒤쪽은 max이고 주기는 하나 줄인다: 2^31 − 128 → 2147483519, 2^31 → 2147483647. 앞쪽
    // −2^31은 나머지 0이라 min, 주기 2^31.
    cycle = math::CycleCurveTime(0.0f, 1.0f, 2147483520.0f);
    CHECK(cycle.wide && cycle.time == 1.0f && cycle.cycleCount == 2147483519.0);
    cycle = math::CycleCurveTime(0.0f, 1.0f, 2147483648.0f);
    CHECK(cycle.wide && cycle.time == 1.0f && cycle.cycleCount == 2147483647.0);
    cycle = math::CycleCurveTime(0.0f, 1.0f, -2147483648.0f);
    CHECK(cycle.wide && cycle.time == 0.0f && cycle.cycleCount == 2147483648.0);
    // 리뷰 반례: (0, 0.75, 805306432)는 몫이 2^30을 넘어 double 경로다(int32 경로였다면 시간 −64).
    cycle = math::CycleCurveTime(0.0f, 0.75f, 805306432.0f);
    CHECK(cycle.wide && cycle.time >= 0.0f && cycle.time <= 0.75f);
    // double 경로는 끝 바로잡기를 하지 않는다: min = 2 − 2^-23, max = 2 + 2^-21, 앞쪽 −2052는
    // 나머지를 더한 float가 max로 반올림되어도 max 그대로다(min으로 바꾸면 Cycle·Oscillate가
    // 뒤집힌다).
    const float low = 2.0f - 1.0f / 8388608.0f;
    const float high = 2.0f + 1.0f / 2097152.0f;
    cycle = math::CycleCurveTime(low, high, -2052.0f);
    double rest = platform::PlatformMath::Fmod(-2052.0 - static_cast<double>(low),
                                               static_cast<double>(high - low));
    if (rest < 0.0) {
        rest += static_cast<double>(high - low);
    }

    CHECK(cycle.wide && cycle.time == static_cast<float>(static_cast<double>(low) + rest));

    // int32 경로는 UE와 같은 float 식이라 곱·합의 반올림으로 키 범위 끝을 시간 크기의 몇 ulp까지
    // 벗어날 수 있다(|몫|이 클수록 크다). |몫| ≤ 2^22에서 그 벗어남이 |time|·2^-21 안인지
    // 본다(무작위, 고정 seed). 길이·시간이 2^-6의 배수이고 |몫| ≤ 2^12면 곱이 정확해 언제나 범위
    // 안이다.
    MathRandom random(0x3801);
    int32 outside = 0;
    int32 exactOutside = 0;
    for (int32 index = 0; index < 20000; ++index) {
        const float minimum = static_cast<float>(random.Range(-100.0, 100.0));
        const float maximum = minimum + static_cast<float>(random.Range(0.01, 10.0));
        const float span = (maximum - minimum) * 4194304.0f;
        const float time = index % 2 == 0 ? maximum + static_cast<float>(random.Unit()) * span
                                          : minimum - static_cast<float>(random.Unit()) * span;
        const math::CurveCycle moved = math::CycleCurveTime(minimum, maximum, time);
        const float slack = platform::PlatformMath::Abs(time) / 2097152.0f;
        if (!moved.wide && (moved.time < minimum - slack || moved.time > maximum + slack)) {
            ++outside;
        }

        const float gridMinimum =
            static_cast<float>(static_cast<int32>(random.Next() % 6400) - 3200) / 64.0f;
        const float gridMaximum = gridMinimum + static_cast<float>(1 + random.Next() % 640) / 64.0f;
        const float gridSpan = (gridMaximum - gridMinimum) * 4096.0f;
        const float gridTime =
            index % 2 == 0
                ? gridMaximum +
                      static_cast<float>(static_cast<int32>(random.Next() % 65536)) / 64.0f
                : gridMinimum -
                      static_cast<float>(static_cast<int32>(random.Next() % 65536)) / 64.0f;
        const math::CurveCycle exact = math::CycleCurveTime(gridMinimum, gridMaximum, gridTime);
        if (platform::PlatformMath::Abs(gridTime) <= gridSpan &&
            (exact.wide || exact.time < gridMinimum || exact.time > gridMaximum)) {
            ++exactOutside;
        }
    }

    CHECK(exactOutside == 0);
    CHECK(outside == 0);

    // 오프셋·홀짝: 값 차 2.5 × 주기 2 = 5.
    cycle = math::CycleCurveTime(0.0f, 2.0f, 5.0f);
    CHECK(math::CurveCycleValueOffset(cycle, 2.5f) == 5.0f);
    CHECK(!math::IsOddCurveCycle(cycle));
    cycle = math::CycleCurveTime(0.0f, 2.0f, -0.5f);
    CHECK(math::IsOddCurveCycle(cycle));
}

TEST(KeyCurveMath_CycleTimeWideWithoutFatal) {
    // 뒤쪽 2^33, 길이 0.75: 몫 약 −1.1e10(int32 밖) → double 재배치. 2^33 = 0.75·11453246122 +
    // 0.5라 시간 0.5, 주기 11453246122.
    math::CurveCycle cycle = math::CycleCurveTime(0.0f, 0.75f, 8589934592.0f);
    CHECK(cycle.wide && cycle.time == 0.5f && cycle.cycleCount == 11453246122.0);
    CHECK(!math::IsOddCurveCycle(cycle));
    CHECK(math::CurveCycleValueOffset(cycle, 1.0f) == static_cast<float>(11453246122.0));

    // 앞쪽 −2^33: −2^33 + 0.75·11453246123 = 0.25 → 시간 0.25, 주기 11453246123(홀수).
    cycle = math::CycleCurveTime(0.0f, 0.75f, -8589934592.0f);
    CHECK(cycle.wide && cycle.time == 0.25f && cycle.cycleCount == 11453246123.0);
    CHECK(math::IsOddCurveCycle(cycle));

    // 2^31 + 256(float): 몫 −(2^31 + 256)이 int32 밖. 정수 배수라 뒤쪽은 max, 주기는 하나 줄인 2^31
    // + 255.
    cycle = math::CycleCurveTime(0.0f, 1.0f, 2147483904.0f);
    CHECK(cycle.wide && cycle.time == 1.0f && cycle.cycleCount == 2147483903.0);
    // 앞쪽 −(2^31 + 256): 나머지 0이라 min, 주기 2^31 + 256.
    cycle = math::CycleCurveTime(0.0f, 1.0f, -2147483904.0f);
    CHECK(cycle.wide && cycle.time == 0.0f && cycle.cycleCount == 2147483904.0);

    // 1e30, 길이 2^-20: 1e30(float)은 2^-20의 배수라 뒤쪽 max, 주기 = 1e30·2^20 − 1(double에서 1은
    // 사라진다).
    const float duration = 1.0f / 1048576.0f;
    cycle = math::CycleCurveTime(0.0f, duration, 1.0e30f);
    CHECK(cycle.wide && cycle.time == duration);
    CHECK(cycle.cycleCount == static_cast<double>(1.0e30f) * 1048576.0);
    cycle = math::CycleCurveTime(0.0f, duration, -1.0e30f);
    CHECK(cycle.wide && cycle.time == 0.0f);

    // 키 범위가 0이 아닌 곳에서 시작해도 같다: [10, 10.75], 시간 10 + 2^33 → 10.5.
    cycle = math::CycleCurveTime(10.0f, 10.75f, 8589934592.0f);
    CHECK(cycle.wide && cycle.time >= 10.0f && cycle.time <= 10.75f);
}

TEST(KeyCurveMath_CycleTimeDegenerateInputsUnchanged) {
    // 길이 0, 비유한 길이, NaN·±Inf 시간은 옮기지 않는다(주기 0).
    math::CurveCycle cycle = math::CycleCurveTime(2.0f, 2.0f, 5.0f);
    CHECK(cycle.time == 5.0f && cycle.cycleCount == 0.0 && !cycle.wide);
    cycle = math::CycleCurveTime(-3.0e38f, 3.0e38f, 3.4e38f);
    CHECK(cycle.time == 3.4e38f && cycle.cycleCount == 0.0);
    cycle = math::CycleCurveTime(0.0f, 1.0f, FloatNaN);
    CHECK(platform::PlatformMath::IsNaN(cycle.time) && cycle.cycleCount == 0.0);
    cycle = math::CycleCurveTime(0.0f, 1.0f, FloatInfinity);
    CHECK(cycle.time == FloatInfinity && cycle.cycleCount == 0.0);
    cycle = math::CycleCurveTime(0.0f, 1.0f, -FloatInfinity);
    CHECK(cycle.time == -FloatInfinity && cycle.cycleCount == 0.0);
}

TEST(KeyCurveMath_AutoTangentHandValues) {
    // (0, 0)–(1)–(2, 2): (1 − t)·((1 − 0) + (2 − 1)) / 2.
    CHECK(math::ComputeCurveTangent(0.0f, 0.0f, 1.0f, 2.0f, 2.0f, 0.0f) == 1.0f);
    CHECK(math::ComputeCurveTangent(0.0f, 0.0f, 1.0f, 2.0f, 2.0f, 0.5f) == 0.5f);
    CHECK(math::ComputeCurveTangent(0.0f, 0.0f, 1.0f, 2.0f, 2.0f, 1.0f) == 0.0f);

    // 이전·다음 시간이 같으면 하한 1e-4로 나눈다: 2 / 1e-4f.
    CHECK(math::ComputeCurveTangent(3.0f, 0.0f, 1.0f, 3.0f, 2.0f, 0.0f) == 2.0f / 1.0e-4f);

    // 괄호를 줄이지 않는다: (1e8 − 0) + (1 − 1e8) = 1e8 + (−1e8) = 0(float). next − previous로
    // 줄이면 1이다.
    CHECK(math::ComputeCurveTangent(0.0f, 0.0f, 1.0e8f, 1.0f, 1.0f, 0.0f) == 0.0f);
}
} // namespace math_key_curve_tests_file

} // namespace rocks::test
