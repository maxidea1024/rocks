#include <TestSupport/Test.h>
#include <TestSupport/MathTestSupport.h>
#include <Core/Math/KeyCurveMath.h>
#include <Core/Tests/KeyCurveUeReference.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>

// KeyCurveMath(Docs/Design/CurveData.ko.md 5·6절)를 UE 5.8 식을 그대로 옮긴 참조 구현과 비트 단위로
// 대조한다. 참조는 UE 원본을 줄 단위로 옮긴 것이고(형식·암묵 변환·연산 순서를 UE 소스 그대로
// 적었다) 설계 문서 15절의 수정을 Options로 하나씩 켜고 끈다.
// - 수정을 모두 켠 참조 = Rocks 의미: 모든 입력에서 비트가 같아야 한다.
// - 수정을 끈 참조 = UE 그대로: 문서화한 경우(비율 0·1, 끝의 차 넘침, 근 2개, |주기 몫| ≥ 2^30,
//   길이 0·비유한 순환)만 다르다.
// 참조는 KeyCurveUeReference.h(ue58curve — 머리 주석에 참조 함수 ↔ UE 원본 파일·줄 대조표)이고
// Curves 모듈 golden 시험과 함께 쓴다. 무작위 사례는 고정 seed라 매 실행이 같다.
namespace rocks::test {

namespace key_curve_golden {

template <typename S>
bool SameBits(S a, S b) noexcept {
    if constexpr (IsSameV<S, float>) {
        return platform::BitCast<uint32>(a) == platform::BitCast<uint32>(b);
    } else {
        return platform::BitCast<uint64>(a) == platform::BitCast<uint64>(b);
    }
}

float RandomFloat(MathRandom& random, double minimum, double maximum) noexcept {
    return static_cast<float>(random.Range(minimum, maximum));
}

// 접선: 대부분 [−5, 5], 가끔 큰 값·0.
float RandomTangent(MathRandom& random) noexcept {
    const uint64 pick = random.Next() % 8;
    if (pick == 0) {
        return 0.0f;
    }

    if (pick == 1) {
        return RandomFloat(random, -1000.0, 1000.0);
    }

    return RandomFloat(random, -5.0, 5.0);
}

Key RandomKey(MathRandom& random, float time) noexcept {
    Key key;
    key.time = time;
    key.value = RandomFloat(random, -50.0, 50.0);
    key.interp = static_cast<Interp>(random.Next() % 3);
    key.tangentWeight = static_cast<Weight>(random.Next() % 4);
    key.arriveTangent = RandomTangent(random);
    key.leaveTangent = RandomTangent(random);
    key.arriveTangentWeight = RandomFloat(random, 0.0, 4.0);
    key.leaveTangentWeight = RandomFloat(random, 0.0, 4.0);
    return key;
}

// 한 구간과 평가 시간을 만든다. 구간은 가끔 아주 짧거나 길이 0이고, 시간은 대부분 구간 안이다.
struct Segment {
    Key first;
    Key second;
    float time = 0.0f;
};

Segment RandomSegment(MathRandom& random) noexcept {
    const float start = RandomFloat(random, -100.0, 100.0);
    const uint64 lengthPick = random.Next() % 10;
    float length;
    if (lengthPick == 0) {
        length = 0.0f;
    } else if (lengthPick == 1) {
        length = RandomFloat(random, 1e-5, 1e-3);
    } else {
        length = RandomFloat(random, 0.01, 20.0);
    }

    Segment segment;
    segment.first = RandomKey(random, start);
    segment.second = RandomKey(random, start + length);
    const uint64 timePick = random.Next() % 10;
    if (timePick == 0) {
        segment.time = segment.first.time;
    } else if (timePick == 1) {
        segment.time = segment.second.time;
    } else if (timePick == 2) {
        segment.time = start + RandomFloat(random, -5.0, 25.0);
    } else {
        segment.time = start + static_cast<float>(random.Unit()) * length;
    }

    return segment;
}

// 같은 입력에서 Rocks 결과와 참조 결과의 비트가 다른 사례 수다. include가 거짓인 사례는 세지
// 않는다.
template <typename Include>
int32 CountSegmentMismatches(uint64 seed, int32 cases, const ue58curve::Options& options,
                             Include include) noexcept {
    MathRandom random(seed);
    int32 mismatches = 0;
    for (int32 index = 0; index < cases; ++index) {
        const Segment segment = RandomSegment(random);
        if (!include(segment)) {
            continue;
        }

        const float rocks = math::EvalCurveSegment(segment.first, segment.second, segment.time);
        const float reference =
            ue58curve::EvalForTwoKeys(segment.first, segment.second, segment.time, options);
        if (!SameBits(rocks, reference)) {
            ++mismatches;
        }
    }

    return mismatches;
}

bool AnySegment(const Segment&) noexcept {
    return true;
}

// UE 그대로와 비트가 같아야 하는 사례인가: 길이가 0이 아니고, Lerp·Bezier에 실제로 넣는 비율(가중
// 경로는 3차 방정식 근을 float로 바꾼 값)이 0·1이 아니며, 끝의 차가 넘치지 않는다(값 범위가 작다).
// 근 2개 가중 구간은 무작위로는 거의 나오지 않고 따로 시험한다.
bool InsideUeBitRange(const Segment& segment) noexcept {
    const float difference = segment.second.time - segment.first.time;
    if (!(difference > 0.0f)) {
        return false;
    }

    float used = 0.5f;
    ue58curve::Options probe;
    probe.lastAlpha = &used;
    (void)ue58curve::EvalForTwoKeys(segment.first, segment.second, segment.time, probe);
    return used != 0.0f && used != 1.0f;
}

} // namespace key_curve_golden

namespace math_key_curve_golden_tests_file {

using namespace key_curve_golden;

TEST(KeyCurveGolden_SegmentMatchesUeFormulasBitExactly) {
    const ue58curve::Options rocksOptions;
    CHECK(CountSegmentMismatches(0x3001, 20000, rocksOptions, AnySegment) == 0);

    // 모든 구간을 가중 Cubic으로 만들어 가중 경로를 많이 지나게 한다.
    MathRandom random(0x3002);
    int32 mismatches = 0;
    int32 threeRootCases = 0;
    int32 combinations[16] = {};
    for (int32 index = 0; index < 20000; ++index) {
        Segment segment = RandomSegment(random);
        segment.first.interp = Interp::Cubic;
        // 가중 방식 4 × 4 조합을 차례로 모두 지난다(비가중 조합 포함).
        segment.first.tangentWeight = static_cast<Weight>(index % 4);
        segment.second.tangentWeight = static_cast<Weight>((index / 4) % 4);
        ++combinations[index % 16];
        const float rocks = math::EvalCurveSegment(segment.first, segment.second, segment.time);
        const float reference =
            ue58curve::EvalForTwoKeys(segment.first, segment.second, segment.time, rocksOptions);
        if (!SameBits(rocks, reference)) {
            ++mismatches;
        }

        // 시간 Bezier가 단조가 아니면(근 3개) 근 고르기 갈래를 지난다.
        const float n1 = segment.first.leaveTangentWeight;
        if (n1 > (segment.second.time - segment.first.time)) {
            ++threeRootCases;
        }
    }

    CHECK(mismatches == 0);
    CHECK(threeRootCases > 0);
    for (const int32 count : combinations) {
        CHECK(count > 0);
    }
}

// 입력 비율이 0·1이 아니어도 가중 경로의 근이 float 1.0이 되면 Bezier 비율이 1이다(리뷰 고정 사례):
// k1(t 0, v 0.3, 출발 접선 0.5, Both, 가중치 0.2), k2(t 1, v 0.0012345, 도착 접선 −2, Both, 가중치
// 3.3333333), time 0.99999994 → 근 0.99999998667 → (float) 1.0. Rocks는 끝 값(k2.value)을 그대로
// 낸다. 같은 계산을 한 수정 켬 참조와 비트가 같고 이 사례는 UE 비트 범위 밖으로 분류된다.
TEST(KeyCurveGolden_WeightedRootRoundingToOneIsEndpoint) {
    Segment segment;
    segment.first.time = 0.0f;
    segment.first.value = 0.3f;
    segment.first.interp = Interp::Cubic;
    segment.first.tangentWeight = Weight::Both;
    segment.first.leaveTangent = 0.5f;
    segment.first.leaveTangentWeight = 0.2f;
    segment.second.time = 1.0f;
    segment.second.value = 0.0012345f;
    segment.second.interp = Interp::Cubic;
    segment.second.tangentWeight = Weight::Both;
    segment.second.arriveTangent = -2.0f;
    segment.second.arriveTangentWeight = 3.3333333f;
    segment.time = 0.99999994f;
    float used = 0.5f;
    ue58curve::Options fixed;
    fixed.lastAlpha = &used;
    const float reference =
        ue58curve::EvalForTwoKeys(segment.first, segment.second, segment.time, fixed);
    CHECK(used == 1.0f);
    const float rocks = math::EvalCurveSegment(segment.first, segment.second, segment.time);
    CHECK(SameBits(rocks, reference) && rocks == segment.second.value);
    CHECK(!InsideUeBitRange(segment));
}

// 가중 경로의 비유한 입력: NaN·±Inf 시간과 접선·가중치에서 Rocks와 수정 켬 참조가 같다(NaN끼리는
// NaN이면 같다고 본다).
TEST(KeyCurveGolden_WeightedNonFiniteInputs) {
    const float nan = platform::PlatformMath::QuietNaN<float>();
    const float inf = platform::PlatformMath::Infinity<float>();
    const float times[] = {nan, inf, -inf, 0.25f};
    const float tangents[] = {nan, inf, -inf, 1.0e30f, 0.5f};
    const float weights[] = {nan, inf, 0.5f};
    const ue58curve::Options fixed;
    int32 mismatches = 0;
    for (const float time : times) {
        for (const float tangent : tangents) {
            for (const float weight : weights) {
                Key first;
                first.time = 0.0f;
                first.value = 1.0f;
                first.interp = Interp::Cubic;
                first.tangentWeight = Weight::Both;
                first.leaveTangent = tangent;
                first.leaveTangentWeight = weight;
                Key second = first;
                second.time = 1.0f;
                second.value = 2.0f;
                second.arriveTangent = -tangent;
                second.arriveTangentWeight = weight;
                const float rocks = math::EvalCurveSegment(first, second, time);
                const float reference = ue58curve::EvalForTwoKeys(first, second, time, fixed);
                const bool bothNaN = platform::PlatformMath::IsNaN(rocks) &&
                                     platform::PlatformMath::IsNaN(reference);
                if (!bothNaN && !SameBits(rocks, reference)) {
                    ++mismatches;
                }
            }
        }
    }

    CHECK(mismatches == 0);
}

TEST(KeyCurveGolden_UeLiteralDiffersOnlyInDocumentedCases) {
    ue58curve::Options literal;
    literal.exactLerpEndpoints = false;
    literal.fixUnfilledRoot = false;
    literal.unfilledRoot = 0.0;
    CHECK(CountSegmentMismatches(0x3101, 20000, literal, InsideUeBitRange) == 0);

    // 비율 0·1에서는 Lerp 끝점이 달라 UE 그대로와 갈리는 사례가 있다(15절 2).
    CHECK(CountSegmentMismatches(0x3102, 20000, literal, AnySegment) > 0);

    // 반례: 끝의 차가 넘치는 Linear 구간에서 UE는 Inf, Rocks는 유한하다.
    Segment overflow;
    overflow.first.time = 0.0f;
    overflow.first.value = -3.0e38f;
    overflow.second.time = 1.0f;
    overflow.second.value = 3.0e38f;
    const float ueOverflow =
        ue58curve::EvalForTwoKeys(overflow.first, overflow.second, 0.5f, literal);
    const float rocksOverflow = math::EvalCurveSegment(overflow.first, overflow.second, 0.5f);
    CHECK(!platform::PlatformMath::IsFinite(ueOverflow));
    CHECK(platform::PlatformMath::IsFinite(rocksOverflow) && rocksOverflow == 0.0f);
}

TEST(KeyCurveGolden_SinCosMatchesUePolynomial) {
    MathRandom random(0x3201);
    int32 mismatches = 0;
    for (int32 index = 0; index < 200000; ++index) {
        float angle;
        const uint64 pick = index % 4;
        if (pick == 0) {
            angle = static_cast<float>(random.Range(-1.5707964, 1.5707964));
        } else if (pick == 1) {
            angle = static_cast<float>(random.Range(-10.0, 10.0));
        } else if (pick == 2) {
            angle = static_cast<float>(random.Range(-1.0e6, 1.0e6));
        } else {
            angle = platform::PlatformMath::Atan(static_cast<float>(random.Range(-1.0e4, 1.0e4)));
        }

        float sine;
        float cosine;
        math::CurveSinCos(angle, sine, cosine);
        float ueSine;
        float ueCosine;
        ue58curve::SinCos(&ueSine, &ueCosine, angle);
        if (!SameBits(sine, ueSine) || !SameBits(cosine, ueCosine)) {
            ++mismatches;
        }
    }

    CHECK(mismatches == 0);

    // NaN·±Inf·아주 큰 값도 x64 정수 무효값 규칙까지 같다.
    const float specials[] = {platform::PlatformMath::QuietNaN<float>(),
                              platform::PlatformMath::Infinity<float>(),
                              -platform::PlatformMath::Infinity<float>(),
                              1.0e30f,
                              -1.0e30f,
                              0.0f,
                              -0.0f};
    for (const float angle : specials) {
        float sine;
        float cosine;
        math::CurveSinCos(angle, sine, cosine);
        float ueSine;
        float ueCosine;
        ue58curve::SinCos(&ueSine, &ueCosine, angle);
        CHECK(SameBits(sine, ueSine) && SameBits(cosine, ueCosine));
    }
}

TEST(KeyCurveGolden_SolveCubicMatchesUe) {
    MathRandom random(0x3301);
    int32 mismatches = 0;
    int32 counts[4] = {0, 0, 0, 0};
    for (int32 index = 0; index < 100000; ++index) {
        double coefficients[4];
        for (double& coefficient : coefficients) {
            coefficient = random.Range(-10.0, 10.0);
        }

        if (index % 10 == 0) {
            coefficients[3] = 0.0;
        }

        // (x − r1)(x − r2)(x − r3)로 근 3개 사례를 늘린다.
        if (index % 3 == 1) {
            const double r1 = random.Range(-2.0, 2.0);
            const double r2 = random.Range(-2.0, 2.0);
            const double r3 = random.Range(-2.0, 2.0);
            coefficients[3] = 1.0;
            coefficients[2] = -(r1 + r2 + r3);
            coefficients[1] = r1 * r2 + r2 * r3 + r1 * r3;
            coefficients[0] = -(r1 * r2 * r3);
        }

        const math::CurveCubicRoots rocks = math::SolveCurveCubic(coefficients);
        double ueCoefficients[4] = {coefficients[0], coefficients[1], coefficients[2],
                                    coefficients[3]};
        double ueRoots[3] = {0.0, 0.0, 0.0};
        const int32 ueCount = ue58curve::SolveCubic(ueCoefficients, ueRoots);
        bool same = static_cast<int32>(rocks.count) == ueCount;
        for (int32 root = 0; same && root < ueCount; ++root) {
            same = SameBits(rocks.roots[root], ueRoots[root]);
        }

        if (!same) {
            ++mismatches;
        }

        if (ueCount >= 1 && ueCount <= 3) {
            ++counts[ueCount];
        }
    }

    CHECK(mismatches == 0);
    CHECK(counts[1] > 0 && counts[3] > 0);

    // 중근: (x − 1)²(x + 2) = x³ − 3x + 2 → 근 {−2, 1}(2개).
    const double doubleRoot[4] = {2.0, -3.0, 0.0, 1.0};
    const math::CurveCubicRoots solved = math::SolveCurveCubic(doubleRoot);
    CHECK(solved.count == 2);
    CHECK(solved.roots[0] == -2.0 && solved.roots[1] == 1.0 && solved.roots[2] == 0.0);
}

TEST(KeyCurveGolden_CycleTimeMatchesUeInsideSseRange) {
    const ue58curve::Options fixed;
    ue58curve::Options literal;
    literal.fixCycle = false;
    MathRandom random(0x3401);
    int32 fixedMismatches = 0;
    int32 literalMismatches = 0;
    for (int32 index = 0; index < 100000; ++index) {
        const float minimum = static_cast<float>(random.Range(-100.0, 100.0));
        const float duration =
            static_cast<float>(index % 5 == 0 ? random.Range(1e-4, 1e-2) : random.Range(0.1, 50.0));
        const float maximum = minimum + duration;
        float time;
        const uint64 pick = random.Next() % 4;
        if (pick == 0) {
            time = minimum - static_cast<float>(random.Range(0.0, 1.0e4));
        } else if (pick == 1) {
            time = maximum + static_cast<float>(random.Range(0.0, 1.0e4));
        } else if (pick == 2) {
            // 정확한 배수 근처(주기 경계).
            time = maximum + (maximum - minimum) * static_cast<float>(random.Next() % 100);
        } else {
            time = minimum - (maximum - minimum) * static_cast<float>(random.Next() % 100);
        }

        const math::CurveCycle rocks = math::CycleCurveTime(minimum, maximum, time);
        const ue58curve::CycleResult reference =
            ue58curve::CycleTime(minimum, maximum, time, fixed);
        if (!SameBits(rocks.time, reference.time) || rocks.cycleCount != reference.cycleCount ||
            rocks.wide) {
            ++fixedMismatches;
        }

        // 길이가 0보다 크고 |몫| < 2^30이면 UE SSE 식 그대로와도 같다.
        if (maximum > minimum) {
            const ue58curve::CycleResult ue = ue58curve::CycleTime(minimum, maximum, time, literal);
            if (!SameBits(rocks.time, ue.time) || rocks.cycleCount != ue.cycleCount) {
                ++literalMismatches;
            }
        }
    }

    CHECK(fixedMismatches == 0);
    CHECK(literalMismatches == 0);
}

TEST(KeyCurveGolden_CycleTimeUeDefectsAreFixed) {
    ue58curve::Options literal;
    literal.fixCycle = false;

    // 몫 −(2^30 + 128): UE SSE FloorToInt는 2F가 int32를 넘어 −2^30을 내고 시간이 범위 밖(−128)에
    // 남는다.
    const float before = -(1073741824.0f + 128.0f);
    const ue58curve::CycleResult ueBefore = ue58curve::CycleTime(0.0f, 1.0f, before, literal);
    CHECK(ueBefore.time == -128.0f && ueBefore.cycleCount == 1073741824.0);
    const math::CurveCycle rocksBefore = math::CycleCurveTime(0.0f, 1.0f, before);
    CHECK(rocksBefore.time == 0.0f && rocksBefore.cycleCount == 1073741952.0 && rocksBefore.wide);

    // 뒤쪽 정수 시간 2^30 + 128(주기 1의 정확한 배수): Rocks는 max, UE는 128.
    const float after = 1073741824.0f + 128.0f;
    const ue58curve::CycleResult ueAfter = ue58curve::CycleTime(0.0f, 1.0f, after, literal);
    CHECK(ueAfter.time == 128.0f);
    const math::CurveCycle rocksAfter = math::CycleCurveTime(0.0f, 1.0f, after);
    CHECK(rocksAfter.time == 1.0f && rocksAfter.cycleCount == 1073741951.0 && rocksAfter.wide);

    // 길이 0: UE는 0 나눗셈(−Inf 몫 → 정수 무효값 → 주기 −2^30)이다. 시간은 5 + 0·(−2^30) = 5로
    // 남지만 주기 수 2^30이 CycleWithOffset 오프셋을 망가뜨린다. Rocks는 옮기지 않고 주기 0이다.
    const ue58curve::CycleResult ueZero = ue58curve::CycleTime(2.0f, 2.0f, 5.0f, literal);
    CHECK(ueZero.time == 5.0f && ueZero.cycleCount == 1073741824.0);
    const math::CurveCycle rocksZero = math::CycleCurveTime(2.0f, 2.0f, 5.0f);
    CHECK(rocksZero.time == 5.0f && rocksZero.cycleCount == 0.0);
}

TEST(KeyCurveGolden_WeightedTwoRootsDoesNotReadUnfilledSlot) {
    // 시간 Bezier(0, 1.5, −0.5, 1)는 s ≈ 0.311에서 극대 x ≈ 0.5945다. α를 그 극댓값에 맞추면
    // 판별식이 1e-8 안이라 근이 2개(중근 + 단근)다. 키: (0, 0)·(1, 1), 접선 0, 가중치 1.5(Both) —
    // atan(0) = 0이고 CurveSinCos(0) = (0, 1)이라 제어점 시간이 정확히 1.5와 −0.5다.
    Key first;
    first.time = 0.0f;
    first.value = 0.0f;
    first.interp = Interp::Cubic;
    first.tangentWeight = Weight::Both;
    first.leaveTangentWeight = 1.5f;
    Key second;
    second.time = 1.0f;
    second.value = 1.0f;
    second.interp = Interp::Cubic;
    second.tangentWeight = Weight::Both;
    second.arriveTangentWeight = 1.5f;

    // x(s) = 7s³ − 10.5s² + 4.5s, x′(s) = 0 → s = (21 − √63)/42.
    const double peak = (21.0 - platform::PlatformMath::Sqrt(63.0)) / 42.0;
    const double peakValue = ((7.0 * peak - 10.5) * peak + 4.5) * peak;
    float time = static_cast<float>(peakValue);
    bool found = false;
    for (int32 step = 0; step < 64 && !found; ++step) {
        double coefficients[4] = {-static_cast<double>(time), 4.5, -10.5, 7.0};
        double roots[3] = {0.0, 0.0, 0.0};
        if (ue58curve::SolveCubic(coefficients, roots) == 2) {
            found = true;
            break;
        }

        time = platform::BitCast<float>(platform::BitCast<uint32>(time) + 1u); // 다음 float(양수)
    }

    REQUIRE(found);
    const ue58curve::Options fixed;
    const float rocks = math::EvalCurveSegment(first, second, time);
    CHECK(SameBits(rocks, ue58curve::EvalForTwoKeys(first, second, time, fixed)));

    // UE는 채워지지 않은 세 번째 칸까지 본다. 그 칸이 [0, 1] 안의 더 큰 값이면 UE는 그것을 고른다.
    ue58curve::Options literal;
    literal.fixUnfilledRoot = false;
    literal.unfilledRoot = 0.999;
    const float ue = ue58curve::EvalForTwoKeys(first, second, time, literal);
    CHECK(!SameBits(rocks, ue));
}

TEST(KeyCurveGolden_WeightedDoubleTimeEqualsFloatTime) {
    // 가중 경로의 시간 칸은 UE처럼 double로 계산한 뒤 float로 저장한다(CE 101–102). 그 식은 모두
    // float 값 둘의 연산 한 번이라(c·w + t1, t2 − t1, tanX − t1) double로 계산해 float로 반올림한
    // 결과가 float 연산 결과와 늘 같다(double 53비트 ≥ 2·24 + 2 — 이중 반올림이 무해한 조건). 설계
    // 개정 2의 "float로만 계산하면 드물게 비트가 다르다"는 성립하지 않으므로, 큰 시간·짧은 구간을
    // 포함한 무작위 입력에서 두 계산이 같고 Rocks가 UE와 같은지 확인한다.
    const auto floatOnly = [](const Key& k1, const Key& k2, float time) noexcept {
        const float diff = k2.time - k1.time;
        const float alpha = (time - k1.time) / diff;
        const float x = k2.time - k1.time;
        float s;
        float c;
        math::CurveSinCos(platform::PlatformMath::Atan(k1.leaveTangent), s, c);
        const float leaveWeight = k1.leaveTangentWeight;
        const float tan1x = c * leaveWeight + k1.time;
        const float tan1y = s * leaveWeight + k1.value;
        math::CurveSinCos(platform::PlatformMath::Atan(k2.arriveTangent), s, c);
        const float arriveWeight = k2.arriveTangentWeight;
        const float tan2x = -c * arriveWeight + k2.time;
        const float tan2y = -s * arriveWeight + k2.value;
        const float n1 = (tan1x - k1.time) / x;
        const float n2 = (tan2x - k1.time) / x;
        const double a = static_cast<double>(n1);
        const double b = static_cast<double>(n2) - static_cast<double>(n1);
        const double cc = 1.0 - static_cast<double>(n2);
        const double d = b - a;
        double coefficients[4] = {0.0 - static_cast<double>(alpha), 3.0 * a, 3.0 * d, cc - b - d};
        double roots[3] = {0.0, 0.0, 0.0};
        const int32 count = ue58curve::SolveCubic(coefficients, roots);
        double parameter = roots[0];
        if (count != 1) {
            parameter = NumericLimits<double>::Lowest();
            for (int32 index = 0; index < count; ++index) {
                const double root = roots[index];
                if ((root > 0.0 || ue58curve::IsNearlyEqual(root, 0.0)) &&
                    (root < 1.0 || ue58curve::IsNearlyEqual(root, 1.0)) &&
                    (parameter < 0.0 || root > parameter)) {
                    parameter = root;
                }
            }

            if (parameter == NumericLimits<double>::Lowest()) {
                parameter = 0.0;
            }
        }

        return math::BezierCurveValue(k1.value, tan1y, tan2y, k2.value,
                                      static_cast<float>(parameter));
    };

    MathRandom random(0x3501);
    const ue58curve::Options fixed;
    int32 differing = 0;
    int32 mismatches = 0;
    for (int32 index = 0; index < 50000; ++index) {
        Key first;
        first.time = static_cast<float>(index % 2 == 0 ? random.Range(1000.0, 5000.0)
                                                       : random.Range(-1.0e7, 1.0e7));
        first.value = static_cast<float>(random.Range(-10.0, 10.0));
        first.interp = Interp::Cubic;
        first.tangentWeight = Weight::Both;
        first.leaveTangent = static_cast<float>(random.Range(-3.0, 3.0));
        first.leaveTangentWeight = static_cast<float>(random.Range(0.01, 0.3));
        Key second = first;
        second.time = first.time + static_cast<float>(index % 3 == 0 ? random.Range(1.0e-3, 1.0)
                                                                     : random.Range(1.0, 100.0));
        second.value = static_cast<float>(random.Range(-10.0, 10.0));
        second.arriveTangent = static_cast<float>(random.Range(-3.0, 3.0));
        second.arriveTangentWeight = static_cast<float>(random.Range(0.01, 0.3));
        if (!(second.time > first.time)) {
            continue;
        }

        const float time =
            first.time + static_cast<float>(random.Unit()) * (second.time - first.time);
        const float rocks = math::EvalCurveSegment(first, second, time);
        if (!SameBits(rocks, ue58curve::EvalForTwoKeys(first, second, time, fixed))) {
            ++mismatches;
        }

        if (!SameBits(rocks, floatOnly(first, second, time))) {
            ++differing;
        }
    }

    CHECK(mismatches == 0);
    CHECK(differing == 0);
}

TEST(KeyCurveGolden_AutoTangentMatchesUe) {
    MathRandom random(0x3601);
    int32 mismatches = 0;
    for (int32 index = 0; index < 100000; ++index) {
        const float previousTime = static_cast<float>(random.Range(-100.0, 100.0));
        const float currentTime = previousTime + static_cast<float>(random.Range(0.0, 10.0));
        const float nextTime = index % 50 == 0
                                   ? previousTime
                                   : currentTime + static_cast<float>(random.Range(0.0, 10.0));
        const float previousValue = static_cast<float>(random.Range(-100.0, 100.0));
        const float currentValue = static_cast<float>(random.Range(-100.0, 100.0));
        const float nextValue = static_cast<float>(random.Range(-100.0, 100.0));
        const float tension = static_cast<float>(random.Range(-1.0, 1.0));
        const float rocks = math::ComputeCurveTangent(previousTime, previousValue, currentValue,
                                                      nextTime, nextValue, tension);
        const float ue = ue58curve::ComputeCurveTangent(previousTime, previousValue, currentTime,
                                                        currentValue, nextTime, nextValue, tension);
        if (!SameBits(rocks, ue)) {
            ++mismatches;
        }
    }

    CHECK(mismatches == 0);
}
} // namespace math_key_curve_golden_tests_file

} // namespace rocks::test
