#include <Curves/TangentCurve.h>

#include "CurveKeyEdit.h"

// TangentCurve 값 범위다(설계 7.3절, D11 — 17.4절 C10). Evaluate가 키 범위 안에서 내는 값 집합의
// 닫힌 범위다: 구간마다 그 구간이 실제로 쓰는 값(Linear는 두 끝, Constant는 앞 키 값 — 마지막
// 구간은 끝 키 값도, Cubic은 아래 극값과 두 끝)을 더한다. 길이 0 구간은 쓰이지 않으므로(마지막 구간
// 제외) 그 앞 키 값은 넣지 않고, 가중 구간이 쓰지 않는 앞 키 값도 넣지 않는다.
// - 비가중 Cubic: 값 Bezier y(s)(s = 구간 비율)의 도함수 근에서 BezierCurveValue로 평가한다.
// - 가중 Cubic: 시간 Bezier x(s)와 값 Bezier y(s). Evaluate는 x(s) = α의 [0, 1] 안 가장 큰 근 s를
//   쓰므로, s가 쓰이는 집합 S = {s : 0 ≤ x(s) < min_{u ∈ (s, 1]} x(u)} ∪ {1} 위의 y 극값을 본다.
//   x가 단조면 S = [0, 1]이다. 아니면 x′의 근으로 나눈 조각 중 x가 증가하는 조각에서, 오른쪽
//   조각들의 최솟값보다 작은 부분만 S다(경계에서 Evaluate 값이 뛰므로 S 조각의 두 끝을 모두 넣는다
//   — 뛰는 점의 두 쪽 극한).
// 근 계산은 double이고 y는 Evaluate와 같은 BezierCurveValue(float)로 평가한다. 결과는 수학적 극값에
// 가깝고 Evaluate의 float 결과가 반올림 오차만큼 넘을 수 있다.
namespace rocks {

namespace {

struct RangeAccumulator {
    float minimum = 0.0f;
    float maximum = 0.0f;
    bool invalid = false;

    void Add(float value) noexcept {
        if (!math::IsFinite(value)) {
            invalid = true;
            return;
        }

        minimum = math::Min(minimum, value);
        maximum = math::Max(maximum, value);
    }
};

// a·s² + b·s + c = 0의 (0, 1) 안 실근이다(오름차순). 계수가 모두 0이면 근이 없다고 본다.
uint32 UnitQuadraticRoots(double a, double b, double c, double (&roots)[2]) noexcept {
    double found[2];
    uint32 count = 0;
    if (a == 0.0) {
        if (b != 0.0) {
            found[count++] = -c / b;
        }
    } else {
        const double discriminant = b * b - 4.0 * a * c;
        if (discriminant >= 0.0) {
            const double root = math::Sqrt(discriminant);
            const double q = -0.5 * (b + (b < 0.0 ? -root : root));
            if (q != 0.0) {
                found[count++] = q / a;
                found[count++] = c / q;
            } else {
                found[count++] = 0.0; // b = 0, 판별식 0 → 중근 0
            }
        }
    }

    uint32 inside = 0;
    for (uint32 index = 0; index < count; ++index) {
        if (found[index] > 0.0 && found[index] < 1.0) {
            roots[inside++] = found[index];
        }
    }

    if (inside == 2 && roots[1] < roots[0]) {
        const double swapped = roots[0];
        roots[0] = roots[1];
        roots[1] = swapped;
    }

    return inside;
}

// Bezier(p0, p1, p2, p3)의 도함수 / 3의 멱 기저 계수다: (d0 − 2d1 + d2)s² + 2(d1 − d0)s + d0.
void DerivativeCoefficients(double p0, double p1, double p2, double p3, double& a, double& b,
                            double& c) noexcept {
    const double d0 = p1 - p0;
    const double d1 = p2 - p1;
    const double d2 = p3 - p2;
    a = d0 - 2.0 * d1 + d2;
    b = 2.0 * (d1 - d0);
    c = d0;
}

double BezierDouble(double p0, double p1, double p2, double p3, double s) noexcept {
    const double inverse = 1.0 - s;
    return inverse * inverse * inverse * p0 + 3.0 * inverse * inverse * s * p1 +
           3.0 * inverse * s * s * p2 + s * s * s * p3;
}

// 값 Bezier의 [low, high] 위 극값(두 끝과 도함수 근)을 더한다.
void AddValueExtremes(RangeAccumulator& range, float p0, float p1, float p2, float p3, double low,
                      double high) noexcept {
    range.Add(math::BezierCurveValue(p0, p1, p2, p3, static_cast<float>(low)));
    range.Add(math::BezierCurveValue(p0, p1, p2, p3, static_cast<float>(high)));
    double a;
    double b;
    double c;
    DerivativeCoefficients(p0, p1, p2, p3, a, b, c);
    double roots[2];
    const uint32 count = UnitQuadraticRoots(a, b, c, roots);
    for (uint32 index = 0; index < count; ++index) {
        if (roots[index] > low && roots[index] < high) {
            range.Add(math::BezierCurveValue(p0, p1, p2, p3, static_cast<float>(roots[index])));
        }
    }
}

// 증가하는 조각 [low, high]에서 x(s) = target인 s다(x(low) < target ≤ x(high) — 이분법).
double SolveIncreasing(double x1, double x2, double target, double low, double high) noexcept {
    for (int32 iteration = 0; iteration < 100; ++iteration) {
        const double middle = 0.5 * (low + high);
        if (middle <= low || middle >= high) {
            break;
        }

        if (BezierDouble(0.0, x1, x2, 1.0, middle) < target) {
            low = middle;
        } else {
            high = middle;
        }
    }

    return 0.5 * (low + high);
}

void AddWeightedSegment(RangeAccumulator& range, const TangentCurveKey& k1,
                        const TangentCurveKey& k2) noexcept {
    const math::CurveWeightedControls controls = math::WeightedCurveControls(k1, k2);
    const double x1 = controls.normalizedX1;
    const double x2 = controls.normalizedX2;
    if (!math::IsFinite(controls.normalizedX1) || !math::IsFinite(controls.normalizedX2) ||
        !math::IsFinite(controls.valueY1) || !math::IsFinite(controls.valueY2)) {
        range.invalid = true;
        return;
    }

    // x′의 (0, 1) 안 근으로 조각을 나눈다.
    double a;
    double b;
    double c;
    DerivativeCoefficients(0.0, x1, x2, 1.0, a, b, c);
    double critical[2];
    const uint32 criticalCount = UnitQuadraticRoots(a, b, c, critical);
    double bounds[4];
    uint32 boundCount = 0;
    bounds[boundCount++] = 0.0;
    for (uint32 index = 0; index < criticalCount; ++index) {
        bounds[boundCount++] = critical[index];
    }

    bounds[boundCount++] = 1.0;

    // 오른쪽 조각부터: suffix는 지금 조각 오른쪽의 x 최솟값이다(맨 오른쪽은 제한 없음).
    double suffix = NumericLimits<double>::Max();
    for (uint32 piece = boundCount - 1; piece > 0; --piece) {
        const double low = bounds[piece - 1];
        const double high = bounds[piece];
        const double xLow = BezierDouble(0.0, x1, x2, 1.0, low);
        const double xHigh = BezierDouble(0.0, x1, x2, 1.0, high);
        if (xHigh > xLow) {
            // x(s) ≥ 0이고 x(s) < suffix인 부분이 S다.
            if (xHigh >= 0.0 && xLow < suffix) {
                const double start = xLow < 0.0 ? SolveIncreasing(x1, x2, 0.0, low, high) : low;
                const double end =
                    xHigh >= suffix ? SolveIncreasing(x1, x2, suffix, low, high) : high;
                if (start <= end) {
                    AddValueExtremes(range, k1.value, controls.valueY1, controls.valueY2, k2.value,
                                     start, end);
                }
            }

            suffix = math::Min(suffix, xLow);
        } else {
            suffix = math::Min(suffix, xHigh);
        }
    }
}

void AddUnweightedSegment(RangeAccumulator& range, const TangentCurveKey& k1,
                          const TangentCurveKey& k2, float difference) noexcept {
    // EvalCurveSegment와 같은 제어점(같은 연산 순서).
    const float oneThird = 1.0f / 3.0f;
    const float p0 = k1.value;
    const float p3 = k2.value;
    const float p1 = p0 + (k1.leaveTangent * difference * oneThird);
    const float p2 = p3 - (k2.arriveTangent * difference * oneThird);
    if (!math::IsFinite(p1) || !math::IsFinite(p2)) {
        range.invalid = true;
        return;
    }

    AddValueExtremes(range, p0, p1, p2, p3, 0.0, 1.0);
}

} // namespace

Result<Interval<float>> TangentCurve::TryGetValueRange() const noexcept {
    const Span<const TangentCurveKey> keys = keys_.View();
    if (keys.Size() == 0) {
        return Result<Interval<float>>::Failure(ErrorCode::NotFound);
    }

    RangeAccumulator range;
    range.minimum = keys[0].value;
    range.maximum = keys[0].value;
    if (keys.Size() == 1) {
        range.Add(keys[0].value);
    }

    // 구간마다 Evaluate가 쓰는 값(구간 끝은 극한 — 닫힌 범위)을 더한다. 첫 값이 들어오기 전의
    // 시작 값은 아래에서 다시 잡는다.
    bool started = keys.Size() == 1;
    auto add = [&](float value) noexcept {
        if (!started) {
            range.minimum = value;
            range.maximum = value;
            started = true;
        }

        range.Add(value);
    };

    RangeAccumulator segmentRange;
    for (usize index = 0; index + 1 < keys.Size(); ++index) {
        const TangentCurveKey& k1 = keys[index];
        const TangentCurveKey& k2 = keys[index + 1];
        const bool last = index + 2 == keys.Size();
        const float difference = k2.time - k1.time;
        if (!(difference > 0.0f)) {
            // 길이 0 구간은 쓰이지 않는다(같은 시간이면 마지막 키에서 시작하는 구간을 쓴다). 단
            // 마지막 구간이면 마지막 키 시간의 값이 앞 키 값이다(5.2절).
            if (last) {
                add(k1.value);
            }

            continue;
        }

        switch (k1.interp) {
        case CurveInterp::Constant:
            add(k1.value);
            if (last) {
                add(k2.value);
            }

            break;
        case CurveInterp::Linear:
            add(k1.value);
            add(k2.value);
            break;
        case CurveInterp::Cubic:
            segmentRange = RangeAccumulator{};
            segmentRange.minimum = k2.value;
            segmentRange.maximum = k2.value;
            segmentRange.Add(k2.value); // s = 1(구간 끝 극한)은 늘 쓰인다
            if (math::IsCurveSegmentUnweighted(k1, k2)) {
                AddUnweightedSegment(segmentRange, k1, k2, difference);
            } else {
                AddWeightedSegment(segmentRange, k1, k2);
            }

            if (segmentRange.invalid) {
                range.invalid = true;
            } else {
                add(segmentRange.minimum);
                add(segmentRange.maximum);
            }

            break;
        }
    }

    if (range.invalid) {
        return Result<Interval<float>>::Failure(ErrorCode::InvalidArgument);
    }

    return curves_detail::FiniteRange(range.minimum, range.maximum);
}

} // namespace rocks
