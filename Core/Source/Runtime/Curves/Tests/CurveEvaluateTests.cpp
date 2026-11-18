#include <Core/Memory/SystemAllocator.h>
#include <Curves/PlainCurve.h>
#include <Curves/TangentCurve.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>
#include <TestSupport/Test.h>

// 커브 평가의 독립 고정 벡터다(Docs/Design/CurveData.ko.md 5절·16절 — UE 참조 코드를 거치지 않은 손
// 계산 기대값). 계산 과정은 사례마다 주석에 적었다. 빈 커브·키 하나·같은 시간 키, 외삽 5종,
// Constant·Linear·Cubic 구간, 가중 접선이 비가중과 같은 곡선이 되는 사례, PlainCurve 경계 차이,
// NaN·±Inf, double 재배치로 가는 큰 순환 시간.
using namespace rocks;

namespace {

bool SameBits(float a, float b) noexcept {
    return platform::BitCast<uint32>(a) == platform::BitCast<uint32>(b);
}

bool Near(float a, float b, float tolerance) noexcept {
    return platform::PlatformMath::Abs(a - b) <= tolerance;
}

TangentCurveKey Key(float time, float value, CurveInterp interp = CurveInterp::Linear) noexcept {
    TangentCurveKey key;
    key.time = time;
    key.value = value;
    key.interp = interp;
    return key;
}

} // namespace

// 키가 없으면 기본값, 없으면 fallback이다. UE와 같이 0을 더하므로 기본값 −0은 +0이 된다. 키가
// 하나면 외삽·기본값과 무관하게 그 키 값이고, NaN 시간은 키가 있으면 NaN이다(UE는 마지막 키 값 —
// 15절 4).
TEST(CurveEvaluate_EmptyAndSingleKey) {
    SystemAllocator allocator;
    TangentCurve curve(allocator);
    CHECK(curve.Evaluate(3.0f, 7.0f) == 7.0f);
    curve.defaultValue = 2.5f;
    CHECK(curve.Evaluate(3.0f, 7.0f) == 2.5f);
    curve.defaultValue = -0.0f;
    CHECK(SameBits(curve.Evaluate(1.0f), 0.0f));
    CHECK(SameBits(curve.Evaluate(platform::PlatformMath::QuietNaN<float>()), 0.0f));

    REQUIRE(curve.TryAddKey(1.0f, 4.0f));
    const CurveExtrapolation modes[] = {
        CurveExtrapolation::Cycle, CurveExtrapolation::CycleWithOffset,
        CurveExtrapolation::Oscillate, CurveExtrapolation::Linear, CurveExtrapolation::Constant};
    for (const CurveExtrapolation mode : modes) {
        curve.preExtrapolation = mode;
        curve.postExtrapolation = mode;
        CHECK(curve.Evaluate(-5.0f) == 4.0f && curve.Evaluate(1.0f) == 4.0f &&
              curve.Evaluate(9.0f) == 4.0f);
        CHECK(platform::PlatformMath::IsNaN(
            curve.Evaluate(platform::PlatformMath::QuietNaN<float>())));
    }
}

// 키 (0, 0)·(1, 1)·(2, 4) Linear의 외삽 5종(손 계산):
// - Linear 앞 t = −1: 기울기 (1 − 0)/(1 − 0) = 1 → −1. 뒤 t = 3: 기울기 (1 − 4)/(1 − 2) = 3 → 4 + 3
// = 7.
// - Constant: 앞 0, 뒤 4.
// - Cycle 뒤 t = 2.5: 몫 (2 − 2.5)/2 = −0.25 → 주기 −1 → t = 2.5 − 2 = 0.5 → 0.5. 앞 t = −0.5: 몫
//   −0.25 → t = −0.5 + 2 = 1.5 → Lerp(1, 4, 0.5) = 2.5.
// - CycleWithOffset 뒤 t = 2.5: 0.5 + (4 − 0)·1 = 4.5. 앞 t = −0.5: 2.5 + (0 − 4)·1 = −1.5.
// - Oscillate 뒤 t = 2.5: 주기 1(홀수) → t = 0 + (2 − 0.5) = 1.5 → 2.5. t = 4.5: 몫 −1.25 → 주기 2
//   (짝수) → t = 4.5 − 4 = 0.5 → 0.5.
// - 정확히 n 주기(t = 6): 몫 −2 → t = 6 − 4 = 2(max) → 4. Cycle 앞 t = −2: 몫 −1 → t = 0 = min이고
//   원래 시간이 min보다 작으므로 그대로 min → 0.
TEST(CurveEvaluate_ExtrapolationVectors) {
    SystemAllocator allocator;
    TangentCurve curve(allocator);
    const TangentCurveKey keys[] = {Key(0.0f, 0.0f), Key(1.0f, 1.0f), Key(2.0f, 4.0f)};
    REQUIRE(curve.TrySetKeys(keys));

    curve.preExtrapolation = CurveExtrapolation::Linear;
    curve.postExtrapolation = CurveExtrapolation::Linear;
    CHECK(curve.Evaluate(-1.0f) == -1.0f && curve.Evaluate(3.0f) == 7.0f);
    curve.preExtrapolation = CurveExtrapolation::Constant;
    curve.postExtrapolation = CurveExtrapolation::Constant;
    CHECK(curve.Evaluate(-1.0f) == 0.0f && curve.Evaluate(3.0f) == 4.0f);
    curve.preExtrapolation = CurveExtrapolation::Cycle;
    curve.postExtrapolation = CurveExtrapolation::Cycle;
    CHECK(curve.Evaluate(2.5f) == 0.5f && curve.Evaluate(-0.5f) == 2.5f);
    CHECK(curve.Evaluate(6.0f) == 4.0f && curve.Evaluate(-2.0f) == 0.0f);
    curve.preExtrapolation = CurveExtrapolation::CycleWithOffset;
    curve.postExtrapolation = CurveExtrapolation::CycleWithOffset;
    CHECK(curve.Evaluate(2.5f) == 4.5f && curve.Evaluate(-0.5f) == -1.5f);
    curve.preExtrapolation = CurveExtrapolation::Oscillate;
    curve.postExtrapolation = CurveExtrapolation::Oscillate;
    CHECK(curve.Evaluate(2.5f) == 2.5f && curve.Evaluate(4.5f) == 0.5f);

    // Linear 외삽의 두 키 시간 차가 1e-8 이하면 끝 키 값이다.
    TangentCurve flat(allocator);
    const TangentCurveKey same[] = {Key(1.0f, 3.0f), Key(1.0f, 5.0f), Key(2.0f, 9.0f)};
    REQUIRE(flat.TrySetKeys(same));
    flat.preExtrapolation = CurveExtrapolation::Linear;
    CHECK(flat.Evaluate(0.0f) == 3.0f);
}

// 구간 보간(손 계산):
// - Constant (0, 1)→(2, 5): t = 1.999 → 1, 마지막 키 시간 t = 2 → time < 2가 거짓이라 5.
// - Cubic 비가중 (0, 0, 출발 0)→(1, 1, 도착 0): 제어점 0·0·1·1, De Casteljau α = 0.25:
//   p01 = 0, p12 = 0.25, p23 = 1, p012 = 0.0625, p123 = 0.4375, 결과 0.0625 + 0.25·0.375 = 0.15625.
// - 가중 Both(가중치 = 길이의 1/3, 접선 0)는 시간 제어점이 1/3·2/3이라 x(s) = s — 비가중과 같은
//   곡선이다(3차 방정식 근의 반올림만큼 다르다).
TEST(CurveEvaluate_SegmentVectors) {
    SystemAllocator allocator;
    TangentCurve step(allocator);
    const TangentCurveKey steps[] = {Key(0.0f, 1.0f, CurveInterp::Constant), Key(2.0f, 5.0f)};
    REQUIRE(step.TrySetKeys(steps));
    CHECK(step.Evaluate(1.999f) == 1.0f && step.Evaluate(2.0f) == 5.0f);

    TangentCurve cubic(allocator);
    const TangentCurveKey smooth[] = {Key(0.0f, 0.0f, CurveInterp::Cubic), Key(1.0f, 1.0f)};
    REQUIRE(cubic.TrySetKeys(smooth));
    CHECK(cubic.Evaluate(0.25f) == 0.15625f && cubic.Evaluate(0.5f) == 0.5f);

    TangentCurve weighted(allocator);
    TangentCurveKey first = Key(0.0f, 0.0f, CurveInterp::Cubic);
    first.tangentWeight = CurveTangentWeight::Both;
    first.leaveTangentWeight = 1.0f / 3.0f;
    TangentCurveKey second = Key(1.0f, 1.0f);
    second.tangentWeight = CurveTangentWeight::Both;
    second.arriveTangentWeight = 1.0f / 3.0f;
    const TangentCurveKey pair[] = {first, second};
    REQUIRE(weighted.TrySetKeys(pair));
    CHECK(Near(weighted.Evaluate(0.25f), 0.15625f, 1.0e-6f));
    CHECK(Near(weighted.Evaluate(0.75f), cubic.Evaluate(0.75f), 1.0e-6f));
}

// 같은 시간 키: 그 시간의 마지막 키에서 시작하는 구간을 쓴다. 마지막 키 시간이 중복이면 마지막
// 구간은 길이 0이라 keys[N−2] 값이고, 조금 뒤는 뒤 외삽(마지막 키 값)이다.
// PlainCurve는 첫 키 시간 이하가 앞 갈래(첫 키 값), 마지막 키 시간 이상이 뒤 갈래다(설계 5.7절).
TEST(CurveEvaluate_SameTimeKeysAndPlainBounds) {
    SystemAllocator allocator;
    TangentCurve tangent(allocator);
    const TangentCurveKey firstSame[] = {Key(0.0f, 1.0f), Key(0.0f, 2.0f), Key(1.0f, 3.0f)};
    REQUIRE(tangent.TrySetKeys(firstSame));
    CHECK(tangent.Evaluate(0.0f) == 2.0f && tangent.Evaluate(0.5f) == 2.5f);

    PlainCurve plain(allocator);
    const PlainCurveKey plainSame[] = {{0.0f, 1.0f}, {0.0f, 2.0f}, {1.0f, 3.0f}};
    REQUIRE(plain.TrySetKeys(plainSame));
    CHECK(plain.Evaluate(0.0f) == 1.0f && plain.Evaluate(0.5f) == 2.5f);
    plain.postExtrapolation = CurveExtrapolation::Linear;
    CHECK(plain.Evaluate(1.0f) == 3.0f);
    plain.interp = PlainCurveInterp::Constant;
    CHECK(plain.Evaluate(0.5f) == 2.0f && plain.Evaluate(0.999f) == 2.0f);

    TangentCurve lastSame(allocator);
    const TangentCurveKey ends[] = {Key(0.0f, 0.0f), Key(1.0f, 5.0f), Key(1.0f, 7.0f)};
    REQUIRE(lastSame.TrySetKeys(ends));
    CHECK(lastSame.Evaluate(1.0f) == 5.0f && lastSame.Evaluate(1.0001f) == 7.0f);
}

// ±Inf: 순환 외삽은 재배치 없이 끝 키 값, Linear는 산술 결과(±Inf). 길이 0(키 시간이 모두 같음)
// 순환은 재배치하지 않는다. 주기 몫이 2^30 이상인 시간은 double로 재배치한다(Fatal 없음): 키 (0,
// 0)· (0.75, 3), 시간 2^33 → 나머지 0.5(2^33 = 0.75·11453246122 + 0.5) → 3·(0.5/0.75) = 2.
TEST(CurveEvaluate_NonFiniteAndHugeTimes) {
    SystemAllocator allocator;
    const float infinity = platform::PlatformMath::Infinity<float>();
    TangentCurve curve(allocator);
    const TangentCurveKey keys[] = {Key(0.0f, 0.0f), Key(0.75f, 3.0f)};
    REQUIRE(curve.TrySetKeys(keys));
    curve.preExtrapolation = CurveExtrapolation::Cycle;
    curve.postExtrapolation = CurveExtrapolation::Oscillate;
    CHECK(curve.Evaluate(infinity) == 3.0f && curve.Evaluate(-infinity) == 0.0f);
    curve.preExtrapolation = CurveExtrapolation::Linear;
    curve.postExtrapolation = CurveExtrapolation::Linear;
    CHECK(curve.Evaluate(infinity) == infinity && curve.Evaluate(-infinity) == -infinity);

    curve.postExtrapolation = CurveExtrapolation::Cycle;
    CHECK(Near(curve.Evaluate(8589934592.0f), 2.0f, 1.0e-6f));
    curve.postExtrapolation = CurveExtrapolation::CycleWithOffset;
    // 주기 11453246122 × 값 차 3 + 2 ≈ 2^33·4 — double 곱의 float 반올림.
    CHECK(Near(curve.Evaluate(8589934592.0f), 34359738368.0f, 4096.0f));

    TangentCurve zero(allocator);
    const TangentCurveKey stacked[] = {Key(2.0f, 1.0f), Key(2.0f, 6.0f)};
    REQUIRE(zero.TrySetKeys(stacked));
    zero.preExtrapolation = CurveExtrapolation::Cycle;
    zero.postExtrapolation = CurveExtrapolation::CycleWithOffset;
    CHECK(zero.Evaluate(-3.0f) == 1.0f && zero.Evaluate(9.0f) == 6.0f);
}

// 자동 접선(손 계산, 설계 6절): Cubic Auto 키 (0, 0)·(1, 1)·(2, 4) — 가운데 접선은
// ((1 − 0) + (4 − 1))/(2 − 0) = 2, tension 0.5면 1, 첫 키 출발·마지막 키 도착은 0. Constant 키 뒤의
// User 키는 출발 0, 앞 키가 Cubic이 아니므로 도착도 0이다. 가중치는 바뀌지 않는다.
TEST(CurveEvaluate_AutoTangentVectors) {
    SystemAllocator allocator;
    TangentCurve curve(allocator);
    TangentCurveKey keys[] = {Key(0.0f, 0.0f, CurveInterp::Cubic),
                              Key(1.0f, 1.0f, CurveInterp::Cubic),
                              Key(2.0f, 4.0f, CurveInterp::Cubic)};
    keys[0].leaveTangent = 9.0f;
    keys[2].arriveTangent = 9.0f;
    keys[1].arriveTangentWeight = 0.7f;
    REQUIRE(curve.TrySetKeys(keys));
    curve.AutoSetTangents();
    CHECK(curve.Keys()[0].leaveTangent == 0.0f && curve.Keys()[2].arriveTangent == 0.0f);
    CHECK(curve.Keys()[1].arriveTangent == 2.0f && curve.Keys()[1].leaveTangent == 2.0f);
    CHECK(curve.Keys()[1].arriveTangentWeight == 0.7f);
    curve.AutoSetTangents(0.5f);
    CHECK(curve.Keys()[1].arriveTangent == 1.0f);

    TangentCurve user(allocator);
    TangentCurveKey userKeys[] = {Key(0.0f, 0.0f, CurveInterp::Constant),
                                  Key(1.0f, 1.0f, CurveInterp::Cubic), Key(2.0f, 2.0f)};
    userKeys[1].tangentMode = CurveTangentMode::User;
    userKeys[1].arriveTangent = 3.0f;
    userKeys[1].leaveTangent = 3.0f;
    REQUIRE(user.TrySetKeys(userKeys));
    user.AutoSetTangents();
    CHECK(user.Keys()[1].arriveTangent == 0.0f && user.Keys()[1].leaveTangent == 0.0f);
}
