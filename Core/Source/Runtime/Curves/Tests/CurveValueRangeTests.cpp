#include <Core/Memory/SystemAllocator.h>
#include <Curves/TangentCurve.h>
#include <Platform/PlatformMath.h>
#include <TestSupport/MathTestSupport.h>
#include <TestSupport/Test.h>

// TangentCurve 값 범위 시험이다(Docs/Design/CurveData.ko.md 7.3절, D11): 손 계산 극값(비가중 Cubic
// 넘침), 가중 구간의 단조·비단조(시간 Bezier가 되돌아가는 경우), 무작위 커브에서 촘촘한 Evaluate
// 표본의 최소·최대가 범위 안이고 범위 끝에 닿는지, 비유한 값의 거부.
using namespace rocks;

namespace {

TangentCurveKey Key(float time, float value, CurveInterp interp = CurveInterp::Linear) noexcept {
    TangentCurveKey key;
    key.time = time;
    key.value = value;
    key.interp = interp;
    return key;
}

struct Sampled {
    float minimum = 0.0f;
    float maximum = 0.0f;
};

// 키 범위를 구간마다 samples개로 나눠 Evaluate한 최소·최대다(키 시간 포함).
Sampled SampleRange(const TangentCurve& curve, int32 samples) noexcept {
    Sampled result;
    result.minimum = curve.Evaluate(curve.Keys()[0].time);
    result.maximum = result.minimum;
    const Span<const TangentCurveKey> keys = curve.Keys();
    for (usize index = 0; index + 1 < keys.Size(); ++index) {
        const double start = keys[index].time;
        const double end = keys[index + 1].time;
        for (int32 sample = 0; sample <= samples; ++sample) {
            const float time = static_cast<float>(start + (end - start) * sample / samples);
            const float value = curve.Evaluate(time);
            result.minimum = math::Min(result.minimum, value);
            result.maximum = math::Max(result.maximum, value);
        }
    }

    return result;
}

} // namespace

// Linear만이면 키 값의 범위다. 비가중 Cubic (0, 0, 출발 3)→(1, 0, 도착 −3)은 제어점 0·1·1·0이라
// y(s) = 3s(1 − s), 최대 y(0.5) = 0.75다. 키가 없으면 NotFound, 비유한 값·접선은 InvalidArgument다.
TEST(CurveValueRange_HandVectors) {
    SystemAllocator allocator;
    TangentCurve linear(allocator);
    const TangentCurveKey linearKeys[] = {Key(0.0f, 2.0f), Key(1.0f, -3.0f), Key(2.0f, 5.0f)};
    REQUIRE(linear.TrySetKeys(linearKeys));
    auto range = linear.TryGetValueRange();
    REQUIRE(range);
    CHECK(range.Value().min == -3.0f && range.Value().max == 5.0f);

    TangentCurve bump(allocator);
    TangentCurveKey first = Key(0.0f, 0.0f, CurveInterp::Cubic);
    first.leaveTangent = 3.0f;
    TangentCurveKey second = Key(1.0f, 0.0f);
    second.arriveTangent = -3.0f;
    const TangentCurveKey bumpKeys[] = {first, second};
    REQUIRE(bump.TrySetKeys(bumpKeys));
    auto bumpRange = bump.TryGetValueRange();
    REQUIRE(bumpRange);
    CHECK(bumpRange.Value().min == 0.0f && bumpRange.Value().max == 0.75f);

    REQUIRE(bump.TrySetKeyTangents(0, 0.0f, platform::PlatformMath::Infinity<float>()));
    CHECK(!bump.TryGetValueRange());
    REQUIRE(bump.TrySetKeyTangents(0, 0.0f, 3.0f));
    REQUIRE(bump.TrySetKeyValue(1, platform::PlatformMath::QuietNaN<float>()));
    CHECK(!bump.TryGetValueRange());
}

// 가중 구간에서 시간 Bezier가 되돌아가는 경우(시간 제어점 0·1.5·−0.5·1 — 출발 접선 1·도착 접선 −1,
// 가중치 1.5·√2): Evaluate는 x(s) = α의 가장 큰 근을 쓰므로 일부 매개변수만 쓰인다. 범위는 표본의
// 최소·최대를 담고 그 끝에 닿는다. 단조인 가중 구간도 같다.
TEST(CurveValueRange_WeightedMonotoneAndFolded) {
    SystemAllocator allocator;
    const float weight = 1.5f * 1.41421356f;
    for (int32 folded = 0; folded < 2; ++folded) {
        TangentCurve curve(allocator);
        TangentCurveKey first = Key(0.0f, 0.0f, CurveInterp::Cubic);
        first.tangentWeight = CurveTangentWeight::Both;
        first.leaveTangent = 1.0f;
        first.leaveTangentWeight = folded != 0 ? weight : 0.4f;
        TangentCurveKey second = Key(1.0f, 0.0f);
        second.tangentWeight = CurveTangentWeight::Both;
        second.arriveTangent = -1.0f;
        second.arriveTangentWeight = folded != 0 ? weight : 0.4f;
        const TangentCurveKey keys[] = {first, second};
        REQUIRE(curve.TrySetKeys(keys));
        auto range = curve.TryGetValueRange();
        REQUIRE(range);
        const Sampled sampled = SampleRange(curve, 200000);
        const float span = range.Value().max - range.Value().min;
        const float slack = 1.0e-4f * (1.0f + span);
        CHECK(sampled.minimum >= range.Value().min - slack);
        CHECK(sampled.maximum <= range.Value().max + slack);
        CHECK(sampled.maximum >= range.Value().max - 1.0e-2f * (1.0f + span));
        CHECK(sampled.minimum <= range.Value().min + 1.0e-2f * (1.0f + span));
    }
}

// 무작위 Cubic 커브(비가중·가중 4방식 조합, 큰 가중치로 접힌 구간 포함): 촘촘한 Evaluate 표본은
// 범위 안이고(접힘 근처 풀이 오차 여유 5e-4·(1 + 폭)) 범위 끝에 닿는다(표본 간격 여유 2e-2·(1 + 폭)
// — 쓰이지 않는 키 값을 넣지 않으므로 끝은 Evaluate가 실제로 다가가는 값이다).
TEST(CurveValueRange_RandomCurvesContainSamples) {
    SystemAllocator allocator;
    test::MathRandom random(0x7A26E);
    int32 outside = 0;
    int32 loose = 0;
    for (int32 curveIndex = 0; curveIndex < 300; ++curveIndex) {
        TangentCurve curve(allocator);
        float time = static_cast<float>(random.Range(-5.0, 5.0));
        const usize count = 2 + random.Next() % 4;
        for (usize index = 0; index < count; ++index) {
            TangentCurveKey key =
                Key(time, static_cast<float>(random.Range(-10.0, 10.0)),
                    random.Next() % 4 == 0 ? CurveInterp::Linear : CurveInterp::Cubic);
            key.tangentWeight = static_cast<CurveTangentWeight>(random.Next() % 4);
            key.arriveTangent = static_cast<float>(random.Range(-8.0, 8.0));
            key.leaveTangent = static_cast<float>(random.Range(-8.0, 8.0));
            key.arriveTangentWeight = static_cast<float>(random.Range(0.0, 3.0));
            key.leaveTangentWeight = static_cast<float>(random.Range(0.0, 3.0));
            REQUIRE(curve.TryAddKey(time, key.value));
            const usize last = curve.KeyCount() - 1;
            REQUIRE(curve.TrySetKeyInterp(last, key.interp));
            REQUIRE(curve.TrySetKeyTangentWeight(last, key.tangentWeight));
            REQUIRE(curve.TrySetKeyTangents(last, key.arriveTangent, key.leaveTangent));
            REQUIRE(curve.TrySetKeyTangentWeights(last, key.arriveTangentWeight,
                                                  key.leaveTangentWeight));
            time += static_cast<float>(random.Range(0.2, 2.0));
        }

        auto range = curve.TryGetValueRange();
        REQUIRE(range);
        const Sampled sampled = SampleRange(curve, 20000);
        const float span = range.Value().max - range.Value().min;
        // 접힘 근처의 3차 방정식 풀이 오차(TryGetValueRange 계약)만큼 여유를 둔다.
        const float slack = 5.0e-4f * (1.0f + span);
        const bool inside = sampled.minimum >= range.Value().min - slack &&
                            sampled.maximum <= range.Value().max + slack;
        outside += inside ? 0 : 1;
        const float reach = 2.0e-2f * (1.0f + span);
        loose += sampled.maximum >= range.Value().max - reach &&
                         sampled.minimum <= range.Value().min + reach
                     ? 0
                     : 1;
    }

    CHECK(outside == 0);
    CHECK(loose == 0);
}

namespace {

// 가중 구간 (0, v1)→(1, v2)의 시간 제어점 x(s) = Bezier(0, x1, x2, 1)의 접힘 점(x′의 (0, 1) 안
// 근)에서의 x 값들이다(없으면 0개).
uint32 FoldAlphas(const TangentCurveKey& k1, const TangentCurveKey& k2,
                  double (&alphas)[2]) noexcept {
    const math::CurveWeightedControls controls = math::WeightedCurveControls(k1, k2);
    const double x1 = controls.normalizedX1;
    const double x2 = controls.normalizedX2;
    const double d0 = x1;
    const double d1 = x2 - x1;
    const double d2 = 1.0 - x2;
    const double a = d0 - 2.0 * d1 + d2;
    const double b = 2.0 * (d1 - d0);
    const double c = d0;
    uint32 count = 0;
    const double discriminant = b * b - 4.0 * a * c;
    if (a == 0.0 || discriminant < 0.0) {
        return 0;
    }

    const double root = platform::PlatformMath::Sqrt(discriminant);
    const double candidates[2] = {(-b - root) / (2.0 * a), (-b + root) / (2.0 * a)};
    for (const double s : candidates) {
        if (s > 0.0 && s < 1.0) {
            const double inverse = 1.0 - s;
            alphas[count++] =
                3.0 * inverse * inverse * s * x1 + 3.0 * inverse * s * s * x2 + s * s * s;
        }
    }

    return count;
}

// 접힘 점 α 근처(10^-12 … 10^-1 떨어진 시간)의 Evaluate 최소·최대를 sampled에 더한다.
void SampleNearFolds(const TangentCurve& curve, Sampled& sampled) noexcept {
    const Span<const TangentCurveKey> keys = curve.Keys();
    for (usize index = 0; index + 1 < keys.Size(); ++index) {
        double alphas[2];
        const uint32 count = FoldAlphas(keys[index], keys[index + 1], alphas);
        const double start = keys[index].time;
        const double length = static_cast<double>(keys[index + 1].time) - start;
        for (uint32 fold = 0; fold < count; ++fold) {
            for (int32 step = 0; step <= 400; ++step) {
                const double offset =
                    platform::PlatformMath::Pow(10.0, -12.0 + 11.0 * step / 400.0);
                const double sides[2] = {alphas[fold] - offset, alphas[fold] + offset};
                for (const double alpha : sides) {
                    if (alpha >= 0.0 && alpha < 1.0) {
                        const float value =
                            curve.Evaluate(static_cast<float>(start + alpha * length));
                        sampled.minimum = math::Min(sampled.minimum, value);
                        sampled.maximum = math::Max(sampled.maximum, value);
                    }
                }
            }
        }
    }
}

TangentCurveKey WeightedKey(float time, float value, float arrive, float arriveWeight, float leave,
                            float leaveWeight) noexcept {
    TangentCurveKey key = Key(time, value, CurveInterp::Cubic);
    key.tangentWeight = CurveTangentWeight::Both;
    key.arriveTangent = arrive;
    key.arriveTangentWeight = arriveWeight;
    key.leaveTangent = leave;
    key.leaveTangentWeight = leaveWeight;
    return key;
}

} // namespace

// 얕은 접힘(CV5 리뷰 1): Evaluate의 3차 방정식 풀이는 판별식 |D| ≤ 1e-8에서 근을 겹근으로 근사해
// 수학적 매개변수 집합 밖의 매개변수를 고른다. 범위는 그 풀이가 고르는 매개변수까지 넣으므로 리뷰
// 반례의 실제 최솟값 Evaluate(0.243263736) ≈ −13.3529도 범위 안이다(고치기 전 최솟값 ≈ −13.178).
// 여유는 float 반올림만큼(1e-6·(1 + 폭) — 독립 확인의 최대 상대 초과는 2.25e-7)이다.
TEST(CurveValueRange_ShallowFoldCounterexample) {
    SystemAllocator allocator;
    TangentCurve curve(allocator);
    const TangentCurveKey keys[] = {WeightedKey(0.0f, -0.33362928f, 0.0f, 0.0f, -39.46f, 24.106f),
                                    WeightedKey(1.0f, 0.56734283f, 7.873f, 10.2616f, 0.0f, 0.0f)};
    REQUIRE(curve.TrySetKeys(keys));
    auto range = curve.TryGetValueRange();
    REQUIRE(range);
    const float span = range.Value().max - range.Value().min;
    const float slack = 1.0e-6f * (1.0f + span);
    const float value = curve.Evaluate(0.243263736f);
    CHECK(value < -13.35f);
    CHECK(value >= range.Value().min - slack && value <= range.Value().max + slack);
    Sampled sampled = SampleRange(curve, 200000);
    SampleNearFolds(curve, sampled);
    CHECK(sampled.minimum >= range.Value().min - slack);
    CHECK(sampled.maximum <= range.Value().max + slack);
}

// 무작위 얕은 접힘(시간 제어점 x1 ∈ [0.4, 0.8], x2 ∈ [−0.45, −0.1] — 판별식 띠를 겨냥한다)과 끝이
// 되돌아가는 구간(x2 > 1 — 음수 가중치): 접힘 점 근처와 촘촘한 Evaluate 표본이 범위 안이고(반올림
// 여유 1e-4·(1 + 폭)) 범위 끝에 닿는다(2e-2·(1 + 폭)).
TEST(CurveValueRange_RandomShallowFoldsContainSamples) {
    SystemAllocator allocator;
    test::MathRandom random(0xF01D);
    int32 outside = 0;
    int32 loose = 0;
    for (int32 curveIndex = 0; curveIndex < 300; ++curveIndex) {
        const bool reversedEnd = curveIndex % 4 == 3;
        const double x1 = reversedEnd ? random.Range(0.1, 0.6) : random.Range(0.4, 0.8);
        const double x2 = reversedEnd ? random.Range(1.05, 1.6) : random.Range(-0.45, -0.1);
        // 접선 각도의 cos로 가중치를 맞춘다: x1 = cos(atan(출발))·w1, x2 = 1 −
        // cos(atan(도착))·w2(길이 1).
        const float leave = static_cast<float>(random.Range(-40.0, 40.0));
        const float arrive = static_cast<float>(random.Range(-40.0, 40.0));
        const double leaveCos =
            1.0 / platform::PlatformMath::Sqrt(1.0 + static_cast<double>(leave) * leave);
        const double arriveCos =
            1.0 / platform::PlatformMath::Sqrt(1.0 + static_cast<double>(arrive) * arrive);
        const float leaveWeight = static_cast<float>(x1 / leaveCos);
        const float arriveWeight = static_cast<float>((1.0 - x2) / arriveCos);
        const float v1 = static_cast<float>(random.Range(-1.0, 1.0));
        const float v2 = static_cast<float>(random.Range(-1.0, 1.0));
        TangentCurve curve(allocator);
        const TangentCurveKey keys[] = {WeightedKey(0.0f, v1, 0.0f, 0.0f, leave, leaveWeight),
                                        WeightedKey(1.0f, v2, arrive, arriveWeight, 0.0f, 0.0f),
                                        Key(2.0f, v2)};
        REQUIRE(curve.TrySetKeys(keys));
        auto range = curve.TryGetValueRange();
        REQUIRE(range);
        Sampled sampled = SampleRange(curve, 20000);
        SampleNearFolds(curve, sampled);
        const float span = range.Value().max - range.Value().min;
        const float slack = 1.0e-4f * (1.0f + span);
        outside += sampled.minimum >= range.Value().min - slack &&
                           sampled.maximum <= range.Value().max + slack
                       ? 0
                       : 1;
        const float reach = 2.0e-2f * (1.0f + span);
        loose += sampled.maximum >= range.Value().max - reach &&
                         sampled.minimum <= range.Value().min + reach
                     ? 0
                     : 1;
    }

    CHECK(outside == 0);
    CHECK(loose == 0);
}
