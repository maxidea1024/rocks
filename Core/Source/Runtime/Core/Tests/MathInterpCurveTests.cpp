#include <TestSupport/Test.h>
#include <TestSupport/MathTestSupport.h>
#include <Core/Math/InterpCurve.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>

// InterpCurve의 의미 결정(Docs/Implementation/InterpCurve.ko.md 3절) 경계 시험이다. UE 식 대조와
// double reference는 MathInterpCurveGoldenTests.cpp에 있다.
namespace rocks::test {

namespace {

constexpr float FloatNaN = platform::PlatformMath::QuietNaN<float>();
constexpr float FloatInfinity = platform::PlatformMath::Infinity<float>();

// x 바로 아래의 float이다(유한한 양수·음수·0).
float NextBelow(float value) noexcept {
    if (value == 0.0f) {
        return -platform::BitCast<float>(1u);
    }

    const uint32 bits = platform::BitCast<uint32>(value);
    return platform::BitCast<float>(value > 0.0f ? bits - 1u : bits + 1u);
}

InterpCurvePoint<float> UserKey(float input, float output, float arrive, float leave,
                                InterpCurveMode mode = InterpCurveMode::CurveUser) noexcept {
    return InterpCurvePoint<float>(input, output, arrive, leave, mode);
}

} // namespace

TEST(InterpCurve_EmptyCurveUsesFallbackAndNotFound) {
    SystemAllocator allocator;
    InterpCurveFloat curve(allocator);
    CHECK(curve.Eval(1.0f) == 0.0f);
    CHECK(curve.Eval(1.0f, 7.0f) == 7.0f);
    CHECK(curve.EvalDerivative(1.0f, 3.0f) == 3.0f);
    CHECK(curve.EvalSecondDerivative(1.0f, 4.0f) == 4.0f);
    // 빈 커브의 fallback은 NaN 입력보다 먼저다(평가할 키가 없다).
    CHECK(curve.Eval(FloatNaN, 2.0f) == 2.0f);
    CHECK(curve.FindSegmentIndex(0.0f) == InterpCurveFloat::NoSegment);
    CHECK(curve.SegmentCount() == 0);
    auto interval = curve.TryGetKeyInterval();
    CHECK(!interval && interval.Error() == ErrorCode::NotFound);
    auto nearest = curve.TryFindNearest(1.0f);
    CHECK(!nearest && nearest.Error() == ErrorCode::NotFound);
    curve.AutoSetTangents();
    REQUIRE(curve.TrySetLoopKey(5.0f));
    CHECK(!curve.IsLooped());

    InterpCurveQuat rotations(allocator);
    CHECK(rotations.Eval(0.5f) == Quat::Identity());
    InterpCurveVector positions(allocator);
    CHECK(positions.Eval(0.5f) == Vec3());
}

TEST(InterpCurve_SingleKeyIsConstantEverywhere) {
    SystemAllocator allocator;
    InterpCurveFloat curve(allocator);
    REQUIRE(curve.TryAddPoint(UserKey(2.0f, 5.0f, 3.0f, 4.0f)));
    const float inputs[] = {-FloatInfinity, -10.0f, 2.0f, 3.0f, 10.0f, FloatInfinity};
    for (float input : inputs) {
        CHECK(curve.Eval(input) == 5.0f);
        CHECK(curve.EvalSecondDerivative(input) == 0.0f);
    }

    // 앞쪽은 leave, 키부터는 arrive 접선이다(UE 규약).
    CHECK(curve.EvalDerivative(1.0f) == 4.0f);
    CHECK(curve.EvalDerivative(2.0f) == 3.0f);
    CHECK(curve.EvalDerivative(9.0f) == 3.0f);
    CHECK(curve.SegmentCount() == 0);

    auto nearest = curve.TryFindNearest(1.0f);
    REQUIRE(nearest);
    CHECK(nearest.Value().input == 2.0f && nearest.Value().distanceSquared == 16.0f &&
          nearest.Value().segment == 0);

    // 루프를 켜면 키 자신으로 돌아가는 길이 2의 곡선 구간이 생긴다.
    REQUIRE(curve.TrySetLoopKey(4.0f));
    CHECK(curve.IsLooped() && curve.LoopKeyOffset() == 2.0f && curve.SegmentCount() == 1);
    // Hermite(5, leave 4·2, 5, arrive 3·2, 0.5) = 0.5·5 + 0.125·8 − 0.125·6 + 0.5·5 = 5.25
    CHECK(curve.Eval(3.0f) == 5.25f);
    CHECK(curve.Eval(4.0f) == 5.0f);
    CHECK(curve.Eval(100.0f) == 5.0f);
}

TEST(InterpCurve_KeysStaySortedAndDuplicatesInsertBeforeEqualKeys) {
    SystemAllocator allocator;
    InterpCurveFloat curve(allocator);
    auto third = curve.TryAddPoint(3.0f, 30.0f);
    auto first = curve.TryAddPoint(1.0f, 10.0f);
    auto second = curve.TryAddPoint(2.0f, 20.0f);
    REQUIRE(third && first && second);
    CHECK(third.Value() == 0 && first.Value() == 0 && second.Value() == 1);

    // 같은 입력값의 새 키는 기존 키 앞에 들어간다(UE AddPoint의 InVal < 비교).
    auto duplicate = curve.TryAddPoint(2.0f, 25.0f);
    REQUIRE(duplicate);
    CHECK(duplicate.Value() == 1);
    REQUIRE(curve.PointCount() == 4);
    CHECK(curve.GetPoint(1).outValue == 25.0f && curve.GetPoint(2).outValue == 20.0f);

    // 정확한 입력에서는 마지막 중복 키(먼저 넣은 키), 왼쪽 극한은 새 키 쪽이다(계단).
    CHECK(curve.FindSegmentIndex(2.0f) == 2);
    CHECK(curve.Eval(2.0f) == 20.0f);
    CHECK(Near(curve.Eval(NextBelow(2.0f)), 25.0f, 1.0e-5f));
    CHECK(curve.Eval(1.5f) == 17.5f);
    CHECK(curve.EvalDerivative(1.5f) == 15.0f);
    CHECK(curve.Eval(2.5f) == 25.0f);
    auto interval = curve.TryGetKeyInterval();
    REQUIRE(interval);
    CHECK(interval.Value().min == 1.0f && interval.Value().max == 3.0f);

    // 비유한 키는 거부되고 커브는 그대로다.
    auto nanKey = curve.TryAddPoint(FloatNaN, 0.0f);
    CHECK(!nanKey && nanKey.Error() == ErrorCode::InvalidArgument);
    CHECK(!curve.TryAddPoint(FloatInfinity, 0.0f));
    CHECK(!curve.TryAddPoint(-FloatInfinity, 0.0f));
    CHECK(curve.PointCount() == 4);
    CHECK(!curve.TryRemovePoint(4));
    CHECK(curve.PointCount() == 4);
}

TEST(InterpCurve_MovePointKeepsAttributesAndOrder) {
    SystemAllocator allocator;
    InterpCurveFloat curve(allocator);
    REQUIRE(curve.TryAddPoint(UserKey(1.0f, 10.0f, 1.0f, 2.0f, InterpCurveMode::CurveBreak)));
    REQUIRE(curve.TryAddPoint(2.0f, 20.0f));
    REQUIRE(curve.TryAddPoint(2.0f, 21.0f));
    REQUIRE(curve.TryAddPoint(3.0f, 30.0f));

    // 1 → 2.5: 남은 키 2, 2 뒤로 간다.
    auto moved = curve.TryMovePoint(0, 2.5f);
    REQUIRE(moved);
    CHECK(moved.Value() == 2);
    const InterpCurvePoint<float>& point = curve.GetPoint(2);
    CHECK(point.inValue == 2.5f && point.outValue == 10.0f && point.arriveTangent == 1.0f &&
          point.leaveTangent == 2.0f && point.mode == InterpCurveMode::CurveBreak);
    CHECK(curve.GetPoint(0).outValue == 21.0f && curve.GetPoint(3).outValue == 30.0f);

    // 2.5 → 2: 같은 입력값 키들 앞(0번)으로 간다(AddPoint와 같은 규칙).
    auto back = curve.TryMovePoint(2, 2.0f);
    REQUIRE(back);
    CHECK(back.Value() == 0 && curve.GetPoint(0).outValue == 10.0f);
    CHECK(curve.GetPoint(1).outValue == 21.0f && curve.GetPoint(2).outValue == 20.0f);

    // 실패는 상태를 바꾸지 않는다.
    CHECK(!curve.TryMovePoint(4, 0.0f));
    CHECK(!curve.TryMovePoint(0, FloatNaN));
    CHECK(curve.GetPoint(0).inValue == 2.0f && curve.GetPoint(0).outValue == 10.0f);

    // 마지막으로 옮기기.
    auto last = curve.TryMovePoint(0, 9.0f);
    REQUIRE(last);
    CHECK(last.Value() == 3 && curve.GetPoint(3).outValue == 10.0f);
    for (usize index = 1; index < curve.PointCount(); ++index) {
        CHECK(curve.GetPoint(index - 1).inValue <= curve.GetPoint(index).inValue);
    }
}

TEST(InterpCurve_OutOfRangeHoldsEndValuesAndReturnsEndTangents) {
    SystemAllocator allocator;
    InterpCurveFloat curve(allocator);
    REQUIRE(curve.TryAddPoint(UserKey(0.0f, 0.0f, 1.0f, 2.0f)));
    REQUIRE(curve.TryAddPoint(UserKey(1.0f, 1.0f, 3.0f, 4.0f)));
    CHECK(curve.Eval(-1.0f) == 0.0f && curve.Eval(-FloatInfinity) == 0.0f);
    CHECK(curve.Eval(5.0f) == 1.0f && curve.Eval(FloatInfinity) == 1.0f);
    CHECK(curve.EvalDerivative(-1.0f) == 2.0f);
    CHECK(curve.EvalDerivative(1.0f) == 3.0f);
    CHECK(curve.EvalDerivative(5.0f) == 3.0f);
    CHECK(curve.EvalSecondDerivative(-1.0f) == 0.0f && curve.EvalSecondDerivative(5.0f) == 0.0f);
    // 구간 끝 값은 정확하다.
    CHECK(curve.Eval(0.0f) == 0.0f);
    CHECK(curve.Eval(1.0f) == 1.0f);

    // NaN 입력은 NaN으로 드러난다(UE는 Default = 0을 반환한다).
    CHECK(math::IsNaN(curve.Eval(FloatNaN)));
    CHECK(math::IsNaN(curve.EvalDerivative(FloatNaN)));
    CHECK(math::IsNaN(curve.EvalSecondDerivative(FloatNaN)));
    CHECK(curve.FindSegmentIndex(FloatNaN) == InterpCurveFloat::NoSegment);

    InterpCurveVector positions(allocator);
    REQUIRE(positions.TryAddPoint(0.0f, Vec3(1.0f, 2.0f, 3.0f)));
    const Vec3 nanVector = positions.Eval(FloatNaN);
    CHECK(math::IsNaN(nanVector.x) && math::IsNaN(nanVector.y) && math::IsNaN(nanVector.z));
    InterpCurveQuat rotations(allocator);
    REQUIRE(rotations.TryAddPoint(0.0f, Quat::Identity()));
    CHECK(!rotations.Eval(FloatNaN).IsFinite());
    InterpCurveLinearColor colors(allocator);
    REQUIRE(colors.TryAddPoint(0.0f, LinearColor::White()));
    CHECK(colors.Eval(FloatNaN).ContainsNaN());
}

TEST(InterpCurve_LoopSegmentAndBoundaries) {
    SystemAllocator allocator;
    InterpCurveFloat curve(allocator);
    REQUIRE(curve.TryAddPoint(0.0f, 0.0f));
    REQUIRE(curve.TryAddPoint(1.0f, 10.0f));
    REQUIRE(curve.TrySetLoopKey(3.0f));
    CHECK(curve.IsLooped() && curve.LoopKeyOffset() == 2.0f && curve.SegmentCount() == 2);
    CHECK(curve.Eval(1.0f) == 10.0f);
    CHECK(curve.Eval(2.0f) == 5.0f);
    CHECK(curve.EvalDerivative(2.0f) == -5.0f);
    CHECK(curve.FindSegmentIndex(2.5f) == 1);
    CHECK(curve.Eval(NextBelow(3.0f)) > 0.0f && curve.Eval(NextBelow(3.0f)) < 1.0e-5f);
    // 루프 끝 이상과 첫 키 앞은 첫 키 값이다. 입력을 감지 않는다.
    CHECK(curve.Eval(3.0f) == 0.0f);
    CHECK(curve.Eval(100.0f) == 0.0f);
    CHECK(curve.Eval(-1.0f) == 0.0f);
    CHECK(curve.EvalDerivative(3.0f) == curve.GetPoint(0).arriveTangent);
    CHECK(curve.EvalSecondDerivative(3.0f) == 0.0f);
    // 감는 것은 호출자 몫이다.
    CHECK(curve.Eval(math::Wrap(4.5f, 0.0f, 3.0f)) == 7.5f);

    // 비유한 루프 키는 실패하고 상태를 바꾸지 않는다.
    CHECK(!curve.TrySetLoopKey(FloatNaN));
    CHECK(!curve.TrySetLoopKey(FloatInfinity));
    CHECK(curve.IsLooped() && curve.LoopKeyOffset() == 2.0f);

    // offset은 마지막 키 기준이라 키를 더하면 루프 끝도 움직인다.
    REQUIRE(curve.TryAddPoint(2.0f, 20.0f));
    CHECK(curve.Eval(3.0f) == 10.0f);
    CHECK(curve.Eval(4.0f) == 0.0f);

    // 마지막 키 이하의 루프 키는 루프를 끈다(UE).
    REQUIRE(curve.TrySetLoopKey(2.0f));
    CHECK(!curve.IsLooped() && curve.Eval(3.0f) == 20.0f);
    REQUIRE(curve.TrySetLoopKey(5.0f));
    CHECK(curve.IsLooped());
    curve.ClearLoopKey();
    CHECK(!curve.IsLooped() && curve.SegmentCount() == 2);
}

TEST(InterpCurve_LoopAutoTangentsWrapNeighbors) {
    SystemAllocator allocator;
    InterpCurveFloat curve(allocator);
    REQUIRE(curve.TryAddPoint(0.0f, 0.0f));
    REQUIRE(curve.TryAddPoint(1.0f, 1.0f));
    REQUIRE(curve.TryAddPoint(2.0f, 0.0f));
    for (usize index = 0; index < curve.PointCount(); ++index) {
        curve.SetMode(index, InterpCurveMode::CurveAuto);
    }

    REQUIRE(curve.TrySetLoopKey(3.0f));

    // 첫 키의 이전 키는 시간 −1의 마지막 키, 마지막 키의 다음 키는 시간 3의 첫 키다.
    curve.AutoSetTangents(0.0f, false);
    CHECK(curve.GetPoint(0).leaveTangent == 0.5f && curve.GetPoint(0).arriveTangent == 0.5f);
    CHECK(curve.GetPoint(1).leaveTangent == 0.0f);
    CHECK(curve.GetPoint(2).leaveTangent == -0.5f);
    // 이음매에서 1차 미분이 이어진다.
    CHECK(Near(curve.EvalDerivative(NextBelow(3.0f)), 0.5f, 1.0e-5f));
    CHECK(curve.EvalDerivative(0.0f) == 0.5f);

    // stationaryEndpoints = true면 루프에서도 첫 키만 0이 된다(UE).
    curve.AutoSetTangents(0.0f, true);
    CHECK(curve.GetPoint(0).leaveTangent == 0.0f && curve.GetPoint(0).arriveTangent == 0.0f);
    CHECK(curve.GetPoint(2).leaveTangent == -0.5f);

    // tension은 (1 − tension)을 곱한다.
    curve.AutoSetTangents(0.5f, false);
    CHECK(curve.GetPoint(0).leaveTangent == 0.25f && curve.GetPoint(2).arriveTangent == -0.25f);
}

TEST(InterpCurve_ConstantLinearAndBreakModes) {
    SystemAllocator allocator;
    InterpCurveFloat curve(allocator);
    REQUIRE(curve.TryAddPoint(UserKey(0.0f, 1.0f, 9.0f, 9.0f, InterpCurveMode::Constant)));
    REQUIRE(curve.TryAddPoint(UserKey(2.0f, 5.0f, 1.0f, -1.0f, InterpCurveMode::CurveBreak)));
    REQUIRE(curve.TryAddPoint(UserKey(3.0f, 4.0f, -1.0f, 0.0f, InterpCurveMode::CurveUser)));
    REQUIRE(curve.TryAddPoint(4.0f, 8.0f));

    // Constant: 다음 키 직전까지 유지, 미분 0, 접선 무시.
    CHECK(curve.Eval(NextBelow(2.0f)) == 1.0f);
    CHECK(curve.EvalDerivative(1.0f) == 0.0f && curve.EvalSecondDerivative(1.0f) == 0.0f);
    CHECK(curve.Eval(2.0f) == 5.0f);

    // CurveBreak: 2에서 출발 접선 −1. 2 → 3 구간 왼쪽 끝 미분은 leave.
    CHECK(curve.EvalDerivative(2.0f) == -1.0f);
    // 3에서 도착 −1, 출발 0(CurveUser이지만 값이 달라도 그대로 쓴다).
    CHECK(Near(curve.EvalDerivative(NextBelow(3.0f)), -1.0f, 1.0e-4f));
    CHECK(curve.EvalDerivative(3.0f) == 0.0f);
    // 3 → 4 구간은 CurveUser 키에서 시작하므로 곡선이고 끝 키(Linear)의 도착 접선 0을 쓴다.
    CHECK(curve.Eval(3.5f) == math::CurveHermite(4.0f, 0.0f, 8.0f, 0.0f, 0.5f));

    // Linear 구간: 값은 math::Lerp, 1차 미분은 기울기, 2차 미분 0.
    InterpCurveFloat line(allocator);
    REQUIRE(line.TryAddPoint(0.0f, 2.0f));
    REQUIRE(line.TryAddPoint(4.0f, 10.0f));
    CHECK(line.Eval(1.0f) == 4.0f && line.EvalDerivative(1.0f) == 2.0f);
    CHECK(line.EvalSecondDerivative(1.0f) == 0.0f);
}

TEST(InterpCurve_LinearEndpointIsExactAtRoundedAlphaOne) {
    // x − prev가 반올림으로 구간 길이와 같아지면 비율이 정확히 1이다. math::Lerp는 끝 값을 정확히
    // 반환한다(UE A + 1·(B − A)는 반올림이 남을 수 있다 — 설계 문서 4절 6).
    SystemAllocator allocator;
    InterpCurveFloat curve(allocator);
    REQUIRE(curve.TryAddPoint(-1000.0f, 0.1f));
    REQUIRE(curve.TryAddPoint(1.0f, 0.7f));
    const float justBelow = NextBelow(1.0f);
    REQUIRE((justBelow - -1000.0f) / 1001.0f == 1.0f);
    CHECK(curve.Eval(justBelow) == 0.7f);
}

TEST(InterpCurve_AutoClampedDoesNotOvershootMonotoneKeys) {
    SystemAllocator allocator;
    const float values[] = {0.0f, 0.9f, 1.0f};
    InterpCurveFloat automatic(allocator);
    InterpCurveFloat clamped(allocator);
    for (usize index = 0; index < 3; ++index) {
        const float input = static_cast<float>(index);
        REQUIRE(automatic.TryAddPoint(input, values[index]));
        REQUIRE(clamped.TryAddPoint(input, values[index]));
        automatic.SetMode(index, InterpCurveMode::CurveAuto);
        clamped.SetMode(index, InterpCurveMode::CurveAutoClamped);
    }

    automatic.AutoSetTangents();
    clamped.AutoSetTangents();

    // CurveAuto는 1을 넘는다(반례 기록). Clamped는 단조이며 키 범위 안이다(float 반올림 여유 1e-6).
    CHECK(automatic.Eval(1.5f) > 1.0f);
    float previous = clamped.Eval(0.0f);
    for (int32 step = 1; step <= 400; ++step) {
        const float value = clamped.Eval(static_cast<float>(step) * 0.005f);
        CHECK(value >= previous - 1.0e-6f && value <= 1.0f);
        previous = value;
    }

    CHECK(clamped.GetPoint(1).leaveTangent < automatic.GetPoint(1).leaveTangent);

    // 극값 키의 clamped 접선은 0이다. 이전 키도 곡선 키여야 한다(Linear 뒤의 자동 키는 앞 키의
    // 접선을 복사한다 — UE와 같다).
    InterpCurveFloat crest(allocator);
    REQUIRE(crest.TryAddPoint(0.0f, 0.0f));
    REQUIRE(crest.TryAddPoint(1.0f, 1.0f));
    REQUIRE(crest.TryAddPoint(2.0f, 0.0f));
    for (usize index = 0; index < 3; ++index) {
        crest.SetMode(index, InterpCurveMode::CurveAutoClamped);
    }

    crest.AutoSetTangents();
    CHECK(crest.GetPoint(1).leaveTangent == 0.0f);
    crest.SetMode(0, InterpCurveMode::Linear);
    crest.AutoSetTangents();
    CHECK(crest.GetPoint(1).leaveTangent == crest.GetPoint(0).leaveTangent);
    CHECK(crest.GetPoint(1).leaveTangent == 1.0f);

    // 색·벡터도 성분별로 clamp한다(UE는 FLinearColor에서 clamp를 무시한다).
    InterpCurveLinearColor colors(allocator);
    InterpCurveVector4 vectors(allocator);
    for (usize index = 0; index < 3; ++index) {
        const float input = static_cast<float>(index);
        REQUIRE(colors.TryAddPoint(input, LinearColor(values[index], 0.0f, 0.0f, 1.0f)));
        REQUIRE(vectors.TryAddPoint(input, Vec4(0.0f, values[index], 0.0f, 0.0f)));
        colors.SetMode(index, InterpCurveMode::CurveAutoClamped);
        vectors.SetMode(index, InterpCurveMode::CurveAutoClamped);
    }

    colors.AutoSetTangents();
    vectors.AutoSetTangents();
    CHECK(colors.GetPoint(1).leaveTangent.r == clamped.GetPoint(1).leaveTangent);
    CHECK(vectors.GetPoint(1).leaveTangent.y == clamped.GetPoint(1).leaveTangent);
    CHECK(colors.Eval(1.5f).r <= 1.0f);
}

TEST(InterpCurve_TangentContinuityAcrossModes) {
    SystemAllocator allocator;
    InterpCurveDouble curve(allocator);
    // 간격 2: UE 방식(값 차)이면 Linear → 곡선 이음매의 미분이 2배로 끊긴다.
    const double values[] = {0.0, 4.0, 2.0, 6.0, 5.0};
    const InterpCurveMode modes[] = {InterpCurveMode::Linear, InterpCurveMode::CurveAuto,
                                     InterpCurveMode::CurveAutoClamped, InterpCurveMode::CurveAuto,
                                     InterpCurveMode::Linear};
    for (usize index = 0; index < 5; ++index) {
        REQUIRE(curve.TryAddPoint(2.0 * static_cast<double>(index), values[index]));
        curve.SetMode(index, modes[index]);
    }

    curve.AutoSetTangents(0.0, false);
    CHECK(curve.GetPoint(0).leaveTangent == 2.0);

    // 모든 안쪽 키에서 왼쪽 극한과 오른쪽 값의 1차 미분이 같다.
    for (usize index = 1; index < 4; ++index) {
        const double key = curve.GetPoint(index).inValue;
        const double left = curve.EvalDerivative(key - 1.0e-9);
        const double right = curve.EvalDerivative(key);
        CHECK(Near(left, right, 1.0e-6));
    }

    // 1차·2차 미분은 중앙 차분과 맞는다. 키에서는 2차 미분이 끊기므로 차분 폭 안의 표본은 뺀다.
    MathRandom random(0x5eedc0de12345678ull);
    for (int32 sample = 0; sample < 2000; ++sample) {
        const double input = random.Range(0.001, 7.999);
        const double step = 1.0e-5;
        const bool nearKey = math::Abs(input - math::Round(input / 2.0) * 2.0) < 2.0 * step;
        if (nearKey) {
            continue;
        }

        const double forward = curve.Eval(input + step);
        const double backward = curve.Eval(input - step);
        const double difference = (forward - backward) / (2.0 * step);
        CHECK(Near(curve.EvalDerivative(input), difference, 1.0e-6, 1.0e-6));
        const double slopeDifference =
            (curve.EvalDerivative(input + step) - curve.EvalDerivative(input - step)) /
            (2.0 * step);
        CHECK(Near(curve.EvalSecondDerivative(input), slopeDifference, 1.0e-5, 1.0e-5));
    }
}

TEST(InterpCurve_CopyAndReset) {
    SystemAllocator allocator;
    InterpCurveVector2 source(allocator);
    REQUIRE(source.TryAddPoint(0.0f, Vec2(1.0f, 2.0f)));
    REQUIRE(source.TryAddPoint(1.0f, Vec2(3.0f, 4.0f)));
    REQUIRE(source.TrySetLoopKey(2.0f));
    InterpCurveVector2 copy(allocator);
    REQUIRE(copy.TryCopyFrom(source));
    REQUIRE(copy.PointCount() == 2);
    CHECK(copy.GetPoint(1) == source.GetPoint(1));
    CHECK(copy.IsLooped() && copy.LoopKeyOffset() == 1.0f);
    CHECK(copy.Eval(1.5f) == source.Eval(1.5f));
    CHECK(copy == source);
    REQUIRE(copy.TryCopyFrom(copy));
    CHECK(copy.PointCount() == 2);

    // 같음: 루프가 꺼져 있으면 offset은 보지 않고(UE와 같다), 키 필드 하나만 달라도 다르다.
    copy.SetMode(0, InterpCurveMode::CurveUser);
    CHECK(!(copy == source));
    copy.SetMode(0, InterpCurveMode::Linear);
    REQUIRE(copy.TrySetLoopKey(3.0f));
    CHECK(!(copy == source));
    copy.ClearLoopKey();
    source.ClearLoopKey();
    CHECK(copy == source);

    // Reset은 키만 지우고 루프 설정은 남긴다(UE Reset과 같다).
    REQUIRE(copy.TrySetLoopKey(2.0f));
    copy.Reset();
    CHECK(copy.IsEmpty() && copy.IsLooped());
    CHECK(copy.Eval(0.5f, Vec2(9.0f, 9.0f)) == Vec2(9.0f, 9.0f));

    InterpCurveVector2 moved(Move(source));
    CHECK(moved.PointCount() == 2 && !moved.IsLooped() &&
          moved.GetPoint(1).outValue == Vec2(3.0f, 4.0f));
}

TEST(InterpCurve_QuaternionSquadPassesKeysAndStaysUnit) {
    SystemAllocator allocator;
    InterpCurveQuatd curve(allocator);
    const Quatd keys[] = {
        Quatd::Identity(),
        Quatd::FromUnitAxisAngle(Vec3d::UnitZ(), math::HalfPi<double>),
        Quatd::FromUnitAxisAngle(Vec3d::UnitZ(), math::HalfPi<double>) *
            Quatd::FromUnitAxisAngle(Vec3d::UnitX(), math::HalfPi<double>),
        Quatd::FromUnitAxisAngle(Vec3d(0.6, 0.0, 0.8), 2.5),
    };

    for (usize index = 0; index < 4; ++index) {
        REQUIRE(curve.TryAddPoint(static_cast<double>(index), keys[index]));
        curve.SetMode(index, InterpCurveMode::CurveAuto);
    }

    curve.AutoSetTangents(0.0, false);

    for (usize index = 0; index < 4; ++index) {
        CHECK(curve.Eval(static_cast<double>(index)) == keys[index]);
    }

    // 단위 길이, 키에서 연속(왼쪽 극한도 같은 회전), 이웃 표본 사이 각이 작다.
    Quatd previous = curve.Eval(0.0);
    for (int32 step = 1; step <= 3000; ++step) {
        const Quatd value = curve.Eval(static_cast<double>(step) * 0.001);
        CHECK(value.IsNormalized(1.0e-12));
        CHECK(AngularDistance(previous, value) < 0.01);
        previous = value;
    }

    for (usize index = 1; index < 4; ++index) {
        const double key = static_cast<double>(index);
        CHECK(AngularDistance(curve.Eval(key - 1.0e-12), keys[index]) < 1.0e-9);
    }

    // 키를 −q로 넣어도 같은 회전 곡선이다(반구 맞춤).
    InterpCurveQuatd flipped(allocator);
    for (usize index = 0; index < 4; ++index) {
        const Quatd key =
            index == 2 ? Quatd(-keys[2].x, -keys[2].y, -keys[2].z, -keys[2].w) : keys[index];
        REQUIRE(flipped.TryAddPoint(static_cast<double>(index), key));
        flipped.SetMode(index, InterpCurveMode::CurveAuto);
    }

    flipped.AutoSetTangents(0.0, false);
    for (int32 step = 0; step <= 300; ++step) {
        const double input = static_cast<double>(step) * 0.01;
        CHECK(AngularDistance(flipped.Eval(input), curve.Eval(input)) < 1.0e-9);
    }

    // tension 1과 고정 끝점은 제어점이 키 자신이다. Linear는 Slerp다.
    curve.AutoSetTangents(1.0, true);
    for (usize index = 0; index < 4; ++index) {
        CHECK(AngularDistance(curve.GetPoint(index).leaveTangent, keys[index]) < 1.0e-12);
    }

    curve.SetMode(1, InterpCurveMode::Linear);
    CHECK(AngularDistance(curve.Eval(1.25), Slerp(keys[1], keys[2], 0.25)) < 1.0e-12);
    curve.SetMode(1, InterpCurveMode::Constant);
    CHECK(curve.Eval(1.75) == keys[1]);
}

TEST(InterpCurve_QuaternionLogExpAndSquadHelpers) {
    MathRandom random(0x0a11ce5eedull);
    for (int32 sample = 0; sample < 2000; ++sample) {
        const Vec3d axis(random.Range(-1.0, 1.0), random.Range(-1.0, 1.0), random.Range(-1.0, 1.0));
        auto unitAxis = axis.TryNormalize(1.0e-3);
        if (!unitAxis) {
            continue;
        }

        const double angle = random.Range(0.0, 3.1);
        const Quatd rotation = Quatd::FromUnitAxisAngle(unitAxis.Value(), angle);
        const Quatd logarithm = QuaternionLog(rotation);
        CHECK(logarithm.w == 0.0);
        CHECK(Near(Vec3d(logarithm.x, logarithm.y, logarithm.z).Length(), angle * 0.5, 1.0e-12));
        CHECK(AngularDistance(QuaternionExp(logarithm), rotation) < 1.0e-12);
    }

    CHECK(QuaternionLog(Quatd::Identity()) == Quatd(0.0, 0.0, 0.0, 0.0));
    CHECK(QuaternionExp(Quatd(0.0, 0.0, 0.0, 0.0)) == Quatd::Identity());

    // 이웃이 대칭이면 제어점은 키 자신이다(log 합이 0).
    const Quatd center = Quatd::FromUnitAxisAngle(Vec3d::UnitY(), 0.3);
    const Quatd before = center * Quatd::FromUnitAxisAngle(Vec3d::UnitZ(), -0.4);
    const Quatd after = center * Quatd::FromUnitAxisAngle(Vec3d::UnitZ(), 0.4);
    CHECK(AngularDistance(SquadControlPoint(before, center, after, 0.0), center) < 1.0e-12);
    // 같은 축 위의 키는 SQUAD가 slerp와 같다.
    const Quatd a = Quatd::FromUnitAxisAngle(Vec3d::UnitZ(), 0.2);
    const Quatd b = Quatd::FromUnitAxisAngle(Vec3d::UnitZ(), 1.4);
    CHECK(AngularDistance(Squad(a, a, b, b, 0.3), Slerp(a, b, 0.3)) < 1.0e-12);
    CHECK(Squad(a, a, b, b, 0.0) == a);
    CHECK(SlerpFullPath(a, b, 1.0) == b);
}

TEST(InterpCurve_FindNearestSegmentRules) {
    SystemAllocator allocator;
    InterpCurveVector2 curve(allocator);
    REQUIRE(curve.TryAddPoint(0.0f, Vec2(0.0f, 0.0f)));
    REQUIRE(curve.TryAddPoint(1.0f, Vec2(4.0f, 0.0f)));
    REQUIRE(curve.TryAddPoint(3.0f, Vec2(4.0f, 0.0f)));
    REQUIRE(curve.TryAddPoint(4.0f, Vec2(4.0f, 2.0f)));
    curve.SetMode(2, InterpCurveMode::Constant);

    // Linear: 사영 비율 0.25 → 입력 0.25.
    const InterpCurveVector2::NearestPoint onLine = curve.FindNearestOnSegment(Vec2(1.0f, 1.0f), 0);
    CHECK(onLine.input == 0.25f && onLine.distanceSquared == 1.0f && onLine.segment == 0);
    // 길이 0 Linear 구간(1 → 3은 같은 값)은 끝 키를 고른다(UE와 같다).
    const InterpCurveVector2::NearestPoint degenerate =
        curve.FindNearestOnSegment(Vec2(5.0f, 0.0f), 1);
    CHECK(degenerate.input == 3.0f && degenerate.distanceSquared == 1.0f);
    // Constant: 가까운 끝 키, 같으면 다음 키.
    const InterpCurveVector2::NearestPoint constantStart =
        curve.FindNearestOnSegment(Vec2(4.0f, -1.0f), 2);
    CHECK(constantStart.input == 3.0f && constantStart.distanceSquared == 1.0f);
    const InterpCurveVector2::NearestPoint constantTie =
        curve.FindNearestOnSegment(Vec2(4.0f, 1.0f), 2);
    CHECK(constantTie.input == 4.0f && constantTie.distanceSquared == 1.0f);

    // 전체 탐색: 동률은 앞 구간이다(점 (4, 0)은 구간 0의 끝과 구간 1·2에 모두 거리 0).
    auto best = curve.TryFindNearest(Vec2(4.0f, 0.0f));
    REQUIRE(best);
    CHECK(best.Value().segment == 0 && best.Value().input == 1.0f &&
          best.Value().distanceSquared == 0.0f);

    // 루프 구간도 본다.
    REQUIRE(curve.TrySetLoopKey(6.0f));
    auto looped = curve.TryFindNearest(Vec2(2.0f, 1.0f));
    REQUIRE(looped);
    CHECK(looped.Value().segment == 3);
    CHECK(Near(looped.Value().input, 5.0f, 1.0e-6f));

    // NaN 위치는 NaN 거리로 드러난다.
    auto invalid = curve.TryFindNearest(Vec2(FloatNaN, 0.0f));
    REQUIRE(invalid);
    CHECK(math::IsNaN(invalid.Value().distanceSquared));

    // 스칼라 커브도 거리(차의 제곱)로 찾는다.
    InterpCurveDouble scalar(allocator);
    REQUIRE(scalar.TryAddPoint(0.0, 0.0));
    REQUIRE(scalar.TryAddPoint(2.0, 4.0));
    auto scalarNearest = scalar.TryFindNearest(3.0);
    REQUIRE(scalarNearest);
    CHECK(scalarNearest.Value().input == 1.5 && scalarNearest.Value().distanceSquared == 0.0);
}

TEST(PolynomialRoots_FindsRootsInsideOpenRange) {
    // (x − 0.25)(x − 0.5)(x − 1): 계수가 이진수로 정확해 1에서 값이 정확히 0이다. 열린 구간 (0, 1)
    // 안의 근 둘만 반환하고 끝의 1은 제외한다. (계수를 0.06·0.56처럼 반올림하면 표현된 식의 근이 1
    // 바로 아래로 옮겨 구간 안 근이 된다 — 시험 작성 때 확인한 반례.)
    const double cubic[4] = {-0.125, 0.875, -1.75, 1.0};
    double roots[3] = {};
    const usize count = math::FindPolynomialRootsInRange<3>(cubic, 0.0, 1.0, roots);
    REQUIRE(count == 2);
    CHECK(Near(roots[0], 0.25, 1.0e-15) && Near(roots[1], 0.5, 1.0e-15));

    // 중근 (x − 0.5)²: 임계점에서 값이 정확히 0이면 근이다.
    const double square[3] = {0.25, -1.0, 1.0};
    double squareRoots[2] = {};
    REQUIRE(math::FindPolynomialRootsInRange<2>(square, 0.0, 1.0, squareRoots) == 1);
    CHECK(squareRoots[0] == 0.5);

    // 근 다섯 개: (x−0.1)(x−0.3)(x−0.5)(x−0.7)(x−0.9)의 전개 계수.
    const double quintic[6] = {-0.00945, 0.1689, -0.95, 2.3, -2.5, 1.0};
    double quinticRoots[5] = {};
    REQUIRE(math::FindPolynomialRootsInRange<5>(quintic, 0.0, 1.0, quinticRoots) == 5);
    const double expected[5] = {0.1, 0.3, 0.5, 0.7, 0.9};
    for (usize index = 0; index < 5; ++index) {
        CHECK(Near(quinticRoots[index], expected[index], 1.0e-10));
    }

    double critical[4] = {};
    CHECK(math::FindPolynomialCriticalPointsInRange<5>(quintic, 0.0, 1.0, critical) == 4);

    // 근이 없거나 모두 0인 식, 1차식.
    const double positive[3] = {1.0, 0.0, 1.0};
    double none[2] = {};
    CHECK(math::FindPolynomialRootsInRange<2>(positive, -2.0, 2.0, none) == 0);
    const double zero[3] = {0.0, 0.0, 0.0};
    CHECK(math::FindPolynomialRootsInRange<2>(zero, -2.0, 2.0, none) == 0);
    const double line[2] = {-1.0, 4.0};
    double lineRoot[1] = {};
    REQUIRE(math::FindPolynomialRootsInRange<1>(line, 0.0, 1.0, lineRoot) == 1);
    CHECK(lineRoot[0] == 0.25);
    CHECK(math::EvaluatePolynomial<3>(cubic, 0.25) == 0.0);
    CHECK(math::EvaluatePolynomial<3>(cubic, 1.0) == 0.0);
}

} // namespace rocks::test
