#pragma once

#include <Core/Math/Vector3.h>

// 기하 판정 시험의 독립 double oracle 도우미다. 검사 대상 구현과 다른 방식(황금분할 탐색, SDF 근
// 찾기)으로 최솟값·ray 첫 교차를 구한다. 경계 근처는 판정하지 않고 Ambiguous로 돌려준다.
namespace rocks::test {

// 볼록 함수의 최솟값을 황금분할 탐색으로 찾는다. f(m1) == f(m2)이면 [m1, m2]에 최소가 있으므로
// high를 줄여도 된다. 90회 축소 후 구간 폭은 처음의 1e-18 수준이다. argument에 최소 위치를 쓴다.
template <typename Function>
double MinimizeConvex(double low, double high, Function function, double& argument) noexcept {
    const double ratio = 0.61803398874989484820;
    double m1 = high - (high - low) * ratio;
    double m2 = low + (high - low) * ratio;
    double f1 = function(m1);
    double f2 = function(m2);
    for (int iteration = 0; iteration < 90; ++iteration) {
        if (f1 <= f2) {
            high = m2;
            m2 = m1;
            f2 = f1;
            m1 = high - (high - low) * ratio;
            f1 = function(m1);
        } else {
            low = m1;
            m1 = m2;
            f1 = f2;
            m2 = low + (high - low) * ratio;
            f2 = function(m2);
        }
    }
    argument = (low + high) * 0.5;
    return function(argument);
}

struct OracleHit {
    enum Kind {
        Miss,
        Hit,
        Inside,
        Ambiguous
    };
    Kind kind;
    double distance;
};

// signed distance 함수가 볼록이면 ray 위에서도 볼록이다. 최소값의 부호로 Hit/Miss를 정하고 첫 근을
// 이분법으로 찾는다. 경계에서 margin 이내인 경우는 판정하지 않는다(Ambiguous).
//
// 사용 예:
//   const OracleHit oracle = OracleRaycast(origin, direction, 100.0, 1.0e-9,
//                                          [&](const Vec3d& p) { return SphereSdf(p); });
template <typename Sdf>
OracleHit OracleRaycast(const Vec3d& origin, const Vec3d& direction, double maxDistance,
                        double margin, Sdf sdf) noexcept {
    auto along = [&](double t) { return sdf(origin + direction * t); };
    const double startValue = along(0.0);
    if (startValue < -margin) {
        return {OracleHit::Inside, 0.0};
    }
    if (startValue <= margin) {
        return {OracleHit::Ambiguous, 0.0};
    }

    double nearest = 0.0;
    const double minimum = MinimizeConvex(0.0, maxDistance, along, nearest);
    if (minimum > margin) {
        return {OracleHit::Miss, 0.0};
    }
    if (minimum >= -margin) {
        return {OracleHit::Ambiguous, 0.0};
    }

    double low = 0.0;
    double high = nearest;
    for (int iteration = 0; iteration < 100; ++iteration) {
        const double middle = (low + high) * 0.5;
        if (along(middle) > 0.0) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return {OracleHit::Hit, (low + high) * 0.5};
}

} // namespace rocks::test
