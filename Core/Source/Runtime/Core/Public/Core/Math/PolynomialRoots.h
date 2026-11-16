#pragma once

#include <Core/Math/MathUtility.h>

namespace rocks::math {

// 다항식 c[0] + c[1]·x + ... + c[Degree]·x^Degree의 값이다(Horner, double).
template <usize Degree>
constexpr double EvaluatePolynomial(const double (&coefficients)[Degree + 1], double x) noexcept {
    double value = coefficients[Degree];
    for (usize index = Degree; index != 0; --index) {
        value = value * x + coefficients[index - 1];
    }

    return value;
}

namespace detail {

// [low, high]에서 부호가 바뀌는 한 근을 이분법으로 좁힌다. lowValue와 highValue는 반대 부호이며
// 0이 아니다. 중점이 더 이상 구간을 줄이지 못할 때(인접한 double)까지 반복하므로 결과는 입력에만
// 의존하는 결정적 값이다. 반복 상한은 double 지수 범위를 모두 지나는 횟수보다 크다.
template <usize Degree>
double BisectPolynomialRoot(const double (&coefficients)[Degree + 1], double low, double high,
                            double lowValue) noexcept {
    constexpr int32 MaximumSteps = 2200;
    for (int32 step = 0; step < MaximumSteps; ++step) {
        const double middle = low + (high - low) * 0.5;
        if (!(middle > low && middle < high)) {
            break;
        }

        const double middleValue = EvaluatePolynomial<Degree>(coefficients, middle);
        if (middleValue == 0.0) {
            return middle;
        }

        if ((middleValue < 0.0) == (lowValue < 0.0)) {
            low = middle;
            lowValue = middleValue;
        } else {
            high = middle;
        }
    }

    return low + (high - low) * 0.5;
}

// 도함수의 근(오름차순, 열린 구간 안)으로 나눈 단조 구간마다 근을 찾는다. 단조 구간 안에는 근이
// 많아야 하나다. 임계점에서 값이 정확히 0이면(접하는 근) 그 점을 근으로 넣는다.
template <usize Degree>
usize FindRootsBetweenCriticalPoints(const double (&coefficients)[Degree + 1], double rangeStart,
                                     double rangeEnd, const double* criticalPoints,
                                     usize criticalCount, double* roots) noexcept {
    usize rootCount = 0;
    double low = rangeStart;
    double lowValue = EvaluatePolynomial<Degree>(coefficients, low);
    for (usize index = 0; index <= criticalCount; ++index) {
        const bool atEnd = index == criticalCount;
        const double high = atEnd ? rangeEnd : criticalPoints[index];
        const double highValue = EvaluatePolynomial<Degree>(coefficients, high);
        const bool signChange =
            (lowValue < 0.0 && highValue > 0.0) || (lowValue > 0.0 && highValue < 0.0);
        if (signChange) {
            roots[rootCount] = BisectPolynomialRoot<Degree>(coefficients, low, high, lowValue);
            ++rootCount;
        }

        if (!atEnd && highValue == 0.0) {
            roots[rootCount] = high;
            ++rootCount;
        }

        low = high;
        lowValue = highValue;
    }

    return rootCount;
}

} // namespace detail

// 다항식 c[0] + c[1]·x + ... + c[Degree]·x^Degree의 실근 중 열린 구간 (rangeStart, rangeEnd)
// 안의 것을 오름차순으로 roots에 쓰고 개수를 반환한다(UE 5.8 TPolynomialRootSolver 대응. 구간 끝의
// 근은 반환하지 않는 것도 같다). Degree는 1 이상이다.
//
// 방법: 1차 도함수부터 원래 식까지 차수를 올리며, 한 단계 낮은 도함수의 근으로 나눈 단조 구간마다
// 부호 변화가 있으면 이분법으로 근을 찾는다. 임계점에서 값이 정확히 0이면 그 점도 근이다(중근).
// 정확히 0이 아닌 "거의 닿는" 극값은 근으로 보지 않는다. 그런 점이 필요한 호출자(최근접 탐색)는
// FindPolynomialCriticalPointsInRange의 임계점도 후보로 함께 비교한다.
//
// 전제: rangeStart < rangeEnd이고 모두 유한하다(위반은 Fatal). 계수에 NaN이 있으면 부호 판정이 모두
// 거짓이 되어 근이 없다. 모든 계수가 0인 식은 근을 반환하지 않는다. 결과는 같은 입력에서 항상 같다.
// 근은 double로 표현된 계수의 식에 대한 것이다. 계수를 반올림하면 구간 끝의 근이 끝 바로 안쪽으로
// 옮겨 반환될 수 있다(예: 0.06·0.56으로 쓴 (x−0.2)(x−0.3)(x−1)은 1 바로 아래 근도 낸다).
//
// 사용 예:
//   const double cubic[4] = {-0.125, 0.875, -1.75, 1.0};  // (x-0.25)(x-0.5)(x-1.0)
//   double roots[3];
//   const usize count = math::FindPolynomialRootsInRange<3>(cubic, 0.0, 1.0, roots);  // 0.25, 0.5
template <usize Degree>
usize FindPolynomialRootsInRange(const double (&coefficients)[Degree + 1], double rangeStart,
                                 double rangeEnd, double (&roots)[Degree]) noexcept {
    static_assert(Degree >= 1, "FindPolynomialRootsInRange requires degree 1 or higher");
    ROCKS_ASSERT(IsFinite(rangeStart) && IsFinite(rangeEnd) && rangeStart < rangeEnd);

    // derivatives[k]는 k번째 도함수의 계수다(차수 Degree - k). 근은 상수배에 무관하므로 도함수를
    // 그대로 쓴다.
    double derivatives[Degree + 1][Degree + 1] = {};
    for (usize index = 0; index <= Degree; ++index) {
        derivatives[0][index] = coefficients[index];
    }

    for (usize order = 1; order <= Degree; ++order) {
        for (usize index = 0; index + order <= Degree; ++index) {
            derivatives[order][index] =
                derivatives[order - 1][index + 1] * static_cast<double>(index + 1);
        }
    }

    // 1차식(Degree - 1번째 도함수)의 근에서 시작한다.
    double levelRoots[Degree] = {};
    usize levelCount = 0;
    {
        const double constant = derivatives[Degree - 1][0];
        const double slope = derivatives[Degree - 1][1];
        if (slope != 0.0) {
            const double root = -constant / slope;
            if (root > rangeStart && root < rangeEnd) {
                levelRoots[0] = root;
                levelCount = 1;
            }
        }
    }

    for (usize level = Degree - 1; level != 0; --level) {
        // level - 1번째 도함수(차수 Degree - level + 1)의 근을 level번째 도함수의 근으로 나눈
        // 구간에서 찾는다. 차수별 정적 배열 크기를 맞추려고 원래 차수 크기의 계수 배열에 0을 채워
        // 쓴다.
        double current[Degree + 1] = {};
        for (usize index = 0; index <= Degree; ++index) {
            current[index] = derivatives[level - 1][index];
        }

        double found[Degree] = {};
        const usize foundCount = detail::FindRootsBetweenCriticalPoints<Degree>(
            current, rangeStart, rangeEnd, levelRoots, levelCount, found);
        for (usize index = 0; index < foundCount; ++index) {
            levelRoots[index] = found[index];
        }

        levelCount = foundCount;
    }

    for (usize index = 0; index < levelCount; ++index) {
        roots[index] = levelRoots[index];
    }

    return levelCount;
}

// 다항식의 임계점(1차 도함수의 근) 중 열린 구간 안의 것을 오름차순으로 쓰고 개수를 반환한다.
// 전제와 결정성은 FindPolynomialRootsInRange와 같다. Degree는 2 이상이다.
template <usize Degree>
usize FindPolynomialCriticalPointsInRange(const double (&coefficients)[Degree + 1],
                                          double rangeStart, double rangeEnd,
                                          double (&criticalPoints)[Degree - 1]) noexcept {
    static_assert(Degree >= 2, "FindPolynomialCriticalPointsInRange requires degree 2 or higher");
    double derivative[Degree] = {};
    for (usize index = 0; index < Degree; ++index) {
        derivative[index] = coefficients[index + 1] * static_cast<double>(index + 1);
    }

    return FindPolynomialRootsInRange<Degree - 1>(derivative, rangeStart, rangeEnd, criticalPoints);
}

} // namespace rocks::math
