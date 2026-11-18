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
