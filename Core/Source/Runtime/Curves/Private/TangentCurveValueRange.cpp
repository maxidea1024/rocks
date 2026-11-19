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
    bool any = false;
    bool invalid = false;

    void Add(float value) noexcept {
        if (!math::IsFinite(value)) {
            invalid = true;
            return;
        }

        if (!any) {
            minimum = value;
            maximum = value;
            any = true;
            return;
        }

        minimum = math::Min(minimum, value);
        maximum = math::Max(maximum, value);
    }

    void Merge(const RangeAccumulator& other) noexcept {
        invalid = invalid || other.invalid;
        if (other.any) {
            Add(other.minimum);
            Add(other.maximum);
        }
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

// Evaluate의 근 고르기(math::WeightedCurveSegment — UE 순서: [0, 1] ± 1e-8 안에서 고른 값이
// 음수이거나 더 큰 근)다. 없으면 0이다.
double PickCurveRoot(const double* roots, uint32 count) noexcept {
    if (count == 1) {
        return roots[0];
    }

    constexpr double Tolerance = 1.e-8;
    constexpr double Unset = NumericLimits<double>::Lowest();
    double parameter = Unset;
    for (uint32 index = 0; index < count; ++index) {
        const double root = roots[index];
        const bool aboveZero = root > 0.0 || math::Abs(root - 0.0) <= Tolerance;
        const bool belowOne = root < 1.0 || math::Abs(root - 1.0) <= Tolerance;
        if (aboveZero && belowOne && (parameter < 0.0 || root > parameter)) {
            parameter = root;
        }
    }

    return parameter == Unset ? 0.0 : parameter;
}

// math::SolveCurveCubic의 Cbrt다(UE CE 10–79).
double CubeRoot(double value) noexcept {
    return value > 0.0 ? platform::PlatformMath::Pow(value, 1.0 / 3.0)
                       : (value < 0.0 ? -platform::PlatformMath::Pow(-value, 1.0 / 3.0) : 0.0);
}

// Evaluate의 3차 방정식 풀이가 판별식 |D| ≤ 1e-8에서 근을 겹근으로 근사하는 α 띠(CE 32–35 — 얕게
// 접힌 구간은 실근 셋인 α 구간 전체가 이 띠에 든다)에서 실제로 고르는 매개변수의 y 값을 넣는다.
// 시간 다항식 x(s) − α의 계수는 α가 상수항에만 들어가므로 풀이의 P는 α와 무관하고 Q는 α에 대해
// 일차다(Q = (K − α/den)/2). 띠 안에서 근 후보는 2U − A/3·−U − A/3(U = ∛(−Q)), |Q| ≤ 1e-8이면
// −A/3 하나이고 모두 Q에 대해 단조라, 후보가 0·1을 지나는 Q와 Q = 0·±1e-8로 띠를 나누면 조각마다
// 고르는 식이 하나다. 조각마다 고른 매개변수가 지나는 구간의 y 극값을 넣는다(띠 밖의 풀이는 정확해
// 해석적 매개변수 집합 S가 덮는다).
void AddSolverBand(RangeAccumulator& range, double x1, double x2, float y0, float y1, float y2,
                   float y3) noexcept {
    // math::WeightedCurveSegment와 같은 계수(UE BezierToPower, CE 81–91).
    const double powerA = x1 - 0.0;
    const double powerB = x2 - x1;
    const double powerC = 1.0 - x2;
    const double powerD = powerB - powerA;
    const double cubic = powerC - powerB - powerD;
    const double den = cubic != 0.0 ? cubic : 1e-8 * 1e-8;
    const double a = (3.0 * powerD) / den;
    const double b = (3.0 * powerA) / den;
    const double squareOfA = a * a;
    const double p = (1.0 / 3) * ((-1.0 / 3) * squareOfA + b);
    const double k = ((2.0 / 27) * a) * squareOfA - ((1.0 / 3) * a) * b;
    const double cubeOfP = (p * p) * p;
    constexpr double Threshold = 1e-8;
    const double outer = Threshold - cubeOfP;
    if (!(outer >= 0.0)) {
        return;
    }

    const double sub = (1.0 / 3) * a;
    auto qOfAlpha = [&](double alpha) noexcept { return 0.5 * (k - alpha / den); };
    const double qAtZero = qOfAlpha(0.0);
    const double qAtOne = qOfAlpha(1.0);
    const double qLow = math::Min(qAtZero, qAtOne);
    const double qHigh = math::Max(qAtZero, qAtOne);
    const double outerQ = math::Sqrt(outer);
    const double inner = -Threshold - cubeOfP;
    const double innerQ = inner > 0.0 ? math::Sqrt(inner) : 0.0;

    // 띠의 Q 구간(둘 또는 하나)을 [qLow, qHigh]와 겹친다.
    double bands[2][2] = {{-outerQ, -innerQ}, {innerQ, outerQ}};
    const uint32 bandCount = inner > 0.0 ? 2 : 1;
    if (bandCount == 1) {
        bands[0][1] = outerQ;
    }

    // 고르는 식이 바뀔 수 있는 Q: 0, ±1e-8, 후보가 0·1인 Q.
    double cuts[10];
    uint32 cutCount = 0;
    cuts[cutCount++] = 0.0;
    cuts[cutCount++] = Threshold;
    cuts[cutCount++] = -Threshold;
    const double ends[2] = {0.0, 1.0};
    for (const double end : ends) {
        const double first = (end + sub) * 0.5; // 2U − A/3 = end
        const double second = -(end + sub);     // −U − A/3 = end
        cuts[cutCount++] = -(first * first * first);
        cuts[cutCount++] = -(second * second * second);
    }

    auto parameterAt = [&](double q) noexcept {
        if (math::Abs(q) <= Threshold) {
            return -sub;
        }

        const double u = CubeRoot(-q);
        const double roots[2] = {2.0 * u - sub, -u - sub};
        return PickCurveRoot(roots, 2);
    };

    for (uint32 band = 0; band < bandCount; ++band) {
        const double low = math::Max(bands[band][0], qLow);
        const double high = math::Min(bands[band][1], qHigh);
        if (!(low <= high)) {
            continue;
        }

        // [low, high]를 cuts로 나눈 조각마다 양 끝과 가운데의 매개변수를 본다.
        double points[12];
        uint32 pointCount = 0;
        points[pointCount++] = low;
        for (uint32 index = 0; index < cutCount; ++index) {
            if (cuts[index] > low && cuts[index] < high) {
                points[pointCount++] = cuts[index];
            }
        }

        points[pointCount++] = high;
        for (uint32 outerIndex = 1; outerIndex < pointCount; ++outerIndex) {
            for (uint32 index = outerIndex; index > 0 && points[index - 1] > points[index];
                 --index) {
                const double swapped = points[index];
                points[index] = points[index - 1];
                points[index - 1] = swapped;
            }
        }

        for (uint32 index = 0; index + 1 < pointCount; ++index) {
            const double start = parameterAt(points[index]);
            const double middle = parameterAt(0.5 * (points[index] + points[index + 1]));
            const double end = parameterAt(points[index + 1]);
            const double lowS = math::Min(start, math::Min(middle, end));
            const double highS = math::Max(start, math::Max(middle, end));
            range.Add(math::BezierCurveValue(y0, y1, y2, y3, static_cast<float>(lowS)));
            range.Add(math::BezierCurveValue(y0, y1, y2, y3, static_cast<float>(highS)));
            double coefficientA;
            double coefficientB;
            double coefficientC;
            DerivativeCoefficients(y0, y1, y2, y3, coefficientA, coefficientB, coefficientC);
            double roots[2];
            const uint32 rootCount =
                UnitQuadraticRoots(coefficientA, coefficientB, coefficientC, roots);
            for (uint32 root = 0; root < rootCount; ++root) {
                if (roots[root] > lowS && roots[root] < highS) {
                    range.Add(
                        math::BezierCurveValue(y0, y1, y2, y3, static_cast<float>(roots[root])));
                }
            }
        }
    }
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

    AddSolverBand(range, x1, x2, k1.value, controls.valueY1, controls.valueY2, k2.value);
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
    if (keys.Size() == 1) {
        range.Add(keys[0].value);
    }

    // 구간마다 Evaluate가 쓰는 값(구간 끝은 극한 — 닫힌 범위)을 더한다.
    for (usize index = 0; index + 1 < keys.Size(); ++index) {
        const TangentCurveKey& k1 = keys[index];
        const TangentCurveKey& k2 = keys[index + 1];
        const bool last = index + 2 == keys.Size();
        const float difference = k2.time - k1.time;
        if (!(difference > 0.0f)) {
            // 길이 0 구간은 쓰이지 않는다(같은 시간이면 마지막 키에서 시작하는 구간을 쓴다). 단
            // 마지막 구간이면 마지막 키 시간의 값이 앞 키 값이다(5.2절).
            if (last) {
                range.Add(k1.value);
            }

            continue;
        }

        switch (k1.interp) {
        case CurveInterp::Constant:
            range.Add(k1.value);
            if (last) {
                range.Add(k2.value);
            }

            break;
        case CurveInterp::Linear:
            range.Add(k1.value);
            range.Add(k2.value);
            break;
        case CurveInterp::Cubic: {
            RangeAccumulator segment;
            if (math::IsCurveSegmentUnweighted(k1, k2)) {
                AddUnweightedSegment(segment, k1, k2, difference);
            } else {
                // 마지막 키 시간(α = 1)은 마지막 구간에서만 이 구간을 쓴다(s = 1 → 끝 키 값). 그
                // 밖의 끝 극한은 x가 끝에서 증가할 때만 쓰이고 매개변수 집합 S가 넣는다.
                if (last) {
                    segment.Add(k2.value);
                }

                AddWeightedSegment(segment, k1, k2);
            }

            range.Merge(segment);
            break;
        }
        }
    }

    if (range.invalid || !range.any) {
        return Result<Interval<float>>::Failure(ErrorCode::InvalidArgument);
    }

    return curves_detail::FiniteRange(range.minimum, range.maximum);
}

} // namespace rocks
