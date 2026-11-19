#pragma once

#include <Core/Math/IntersectionCylinderDetail.h>

namespace rocks {

// shape를 덮는 축 정렬 경계(TryBounds)다. 반올림을 거쳐도 shape 전체를 포함한다.

// ===== 경계 =====

// cylinder를 덮는 축 정렬 box다. 반올림을 거쳐도 cylinder 전체(수학적 점 집합)를 항상 포함하며,
// 공간 인덱스 등록 경계로 쓰여 컬링 누락을 막는다.
//
// 계산: 축 d = end - start, 축별 반폭 e_i = r·√(d_j² + d_k²) / |d|(i, j, k는 서로 다른 축)이고
// 결과는 [min(start_i, end_i) - e_i, max(start_i, end_i) + e_i]다. √(1 - a_i²) 꼴은 축이 좌표축과
// 거의 평행할 때 a_i가 1로 반올림되어 반폭이 0이 되므로 쓰지 않는다(예: start (0, 0, 0), end
// (1000, 1e-6, 0), r 1e4이면 실제 x 반폭은 1e-5다). 포함 보장:
// - √(d_j² + d_k²)와 |d|는 가장 큰 성분으로 먼저 나눠 계산해 성분 제곱의 underflow·overflow가 없다.
//   d 성분의 뺄셈 오차는 각 성분의 상대 ε/2라 큰 좌표에서도 비율 오차가 ε 규모다.
// - 비율 계산의 상대 반올림 오차(합 약 6ε)는 반폭에 1 + 8ε(detail::CylinderBoundsRelativeMargin)를
//   곱해 덮는다. 축 비율이 비정규 범위로 작아지면 하한(detail::CylinderBoundsRatioFloor)으로
//   올려 비정규 절대 오차가 radius배로 커지지 않게 한다.
// - 반폭에 한 ulp(math::NextUp)를 더해 마지막 곱의 비정규 절대 오차를 덮고, 끝점 ± 반폭은
//   math::NextDown·NextUp으로 한 ulp 바깥쪽으로 보정해 덧셈의 반 ulp를 덮는다(원점에서 1e9 cm면
//   덧셈의 반 ulp만 약 6e-8 cm다).
// 반폭이 정확히 0인 축(축이 그 좌표축과 평행)은 끝점 좌표를 그대로 쓴다. radius 0은 두 끝점의
// box(정확)다. 결과는 꽉 맞는 경계보다 축마다 상대 약 10ε와 몇 ulp만큼 넓다.
//
// 실패(InvalidArgument): 잘못된 cylinder, 경계 좌표 overflow(계약 범위 밖의 큰 좌표·반지름).
//
// 사용 예:
//   Result<Box<double>> bounds = TryBounds(pillar);
//   if (bounds) { index.Insert(id, bounds.Value()); }
template <typename T>
Result<Box<T>> TryBounds(const Cylinder<T>& cylinder) noexcept {
    if (!cylinder.IsValid()) {
        return Result<Box<T>>::Failure(ErrorCode::InvalidArgument);
    }

    const T zero = static_cast<T>(0);
    const T startValues[3] = {cylinder.start.x, cylinder.start.y, cylinder.start.z};
    const T endValues[3] = {cylinder.end.x, cylinder.end.y, cylinder.end.z};
    T low[3];
    T high[3];
    for (int axis = 0; axis < 3; ++axis) {
        low[axis] = math::Min(startValues[axis], endValues[axis]);
        high[axis] = math::Max(startValues[axis], endValues[axis]);
    }

    if (cylinder.radius > zero) {
        // IsValid가 축 정규화 성공을 보장하므로 delta는 유한한 영 아닌 벡터다.
        const Vector3<T> delta = cylinder.end - cylinder.start;
        const T deltaValues[3] = {math::Abs(delta.x), math::Abs(delta.y), math::Abs(delta.z)};
        const T largest = delta.MaxAbsComponent();
        const T scaledLength = (delta / largest).Length();
        for (int axis = 0; axis < 3; ++axis) {
            const T across0 = deltaValues[(axis + 1) % 3];
            const T across1 = deltaValues[(axis + 2) % 3];
            const T across = math::Max(across0, across1);
            if (!(across > zero)) {
                continue;
            }

            // 반폭 = r · ρ · √(w0² + w1²) / |delta / largest|
            // (ρ = across / largest, w = 성분 / across).
            const T w0 = across0 / across;
            const T w1 = across1 / across;
            const T ratio = math::Max(across / largest, detail::CylinderBoundsRatioFloor<T>);
            const T unitHalfExtent = math::Sqrt(w0 * w0 + w1 * w1) / scaledLength * ratio *
                                     detail::CylinderBoundsRelativeMargin<T>;
            const T halfExtent = math::NextUp(cylinder.radius * unitHalfExtent);
            low[axis] = math::NextDown(low[axis] - halfExtent);
            high[axis] = math::NextUp(high[axis] + halfExtent);
        }
    }

    const Box<T> bounds(Vector3<T>(low[0], low[1], low[2]), Vector3<T>(high[0], high[1], high[2]));
    if (!bounds.IsValid()) {
        return Result<Box<T>>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<Box<T>>::Success(bounds);
}

} // namespace rocks
