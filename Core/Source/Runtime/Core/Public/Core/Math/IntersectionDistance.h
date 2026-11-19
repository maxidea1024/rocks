#pragma once

#include <Core/Math/IntersectionCylinderDetail.h>

namespace rocks {

// 최근접점·거리 질의(TryClosestPoint, TryDistanceSquared, TrySignedDistance, TryClosestPoints)다.

// ===== 최근접점·거리 =====
// 잘못된 shape나 유한하지 않은 point는 InvalidArgument 실패다.

// 평면 위 수직 투영점이다.
template <typename T>
Result<Vector3<T>> TryClosestPoint(const Plane<T>& plane, const Vector3<T>& point) noexcept {
    if (!plane.IsValid() || !point.IsFinite()) {
        return Result<Vector3<T>>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<Vector3<T>>::Success(plane.ProjectPoint(point));
}

// Dot(normal, point) - distance다.
template <typename T>
Result<T> TrySignedDistance(const Plane<T>& plane, const Vector3<T>& point) noexcept {
    if (!plane.IsValid() || !point.IsFinite()) {
        return Result<T>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<T>::Success(plane.SignedDistance(point));
}

// 투영이 끝점 밖이면 끝점을 정확히 돌려준다. 0 길이 segment는 start다.
template <typename T>
Result<Vector3<T>> TryClosestPoint(const Segment<T>& segment, const Vector3<T>& point) noexcept {
    if (!segment.IsValid() || !point.IsFinite()) {
        return Result<Vector3<T>>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<Vector3<T>>::Success(detail::ClosestOnSegment(point, segment.start, segment.end));
}

template <typename T>
Result<T> TryDistanceSquared(const Segment<T>& segment, const Vector3<T>& point) noexcept {
    if (!segment.IsValid() || !point.IsFinite()) {
        return Result<T>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<T>::Success(detail::SegmentDistanceSquared(point, segment.start, segment.end));
}

// 축별 clamp다. box 안의 점은 그 점 자체를 정확히 돌려준다.
template <typename T>
Result<Vector3<T>> TryClosestPoint(const Box<T>& box, const Vector3<T>& point) noexcept {
    if (!box.IsValid() || !point.IsFinite()) {
        return Result<Vector3<T>>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<Vector3<T>>::Success(detail::ClampToBox(box.min, box.max, point));
}

template <typename T>
Result<T> TryDistanceSquared(const Box<T>& box, const Vector3<T>& point) noexcept {
    if (!box.IsValid() || !point.IsFinite()) {
        return Result<T>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<T>::Success(detail::BoxDistanceSquared(box.min, box.max, point));
}

// Ericson 영역 판정법과 퇴화·sliver 안전장치(detail::ClosestOnTriangle)다. 결과는 항상 삼각형 위의
// 점이다.
template <typename T>
Result<Vector3<T>> TryClosestPoint(const Triangle<T>& triangle, const Vector3<T>& point) noexcept {
    if (!triangle.IsValid() || !point.IsFinite()) {
        return Result<Vector3<T>>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<Vector3<T>>::Success(
        detail::ClosestOnTriangle(point, triangle.a, triangle.b, triangle.c));
}

template <typename T>
Result<T> TryDistanceSquared(const Triangle<T>& triangle, const Vector3<T>& point) noexcept {
    if (!triangle.IsValid() || !point.IsFinite()) {
        return Result<T>::Failure(ErrorCode::InvalidArgument);
    }
    const Vector3<T> closest = detail::ClosestOnTriangle(point, triangle.a, triangle.b, triangle.c);
    return Result<T>::Success(DistanceSquared(point, closest));
}

// local 좌표에서 clamp한 뒤 world로 되돌린다. box 안(local 판정)의 점은 그 점 자체를 정확히
// 돌려준다.
template <typename T>
Result<Vector3<T>> TryClosestPoint(const OrientedBox<T>& box, const Vector3<T>& point) noexcept {
    if (!box.IsValid() || !point.IsFinite()) {
        return Result<Vector3<T>>::Failure(ErrorCode::InvalidArgument);
    }

    const Quaternion<T> rotation = box.UnitRotation();
    const Vector3<T> local = rotation.UnrotateVector(point - box.center);
    if (detail::BoxContains(-box.halfExtents, box.halfExtents, local)) {
        return Result<Vector3<T>>::Success(point);
    }
    return Result<Vector3<T>>::Success(box.center + rotation.RotateVector(detail::ClampToBox(
                                                        -box.halfExtents, box.halfExtents, local)));
}

// local 좌표에서 계산하므로 world로 되돌리는 반올림이 거리에 섞이지 않는다.
template <typename T>
Result<T> TryDistanceSquared(const OrientedBox<T>& box, const Vector3<T>& point) noexcept {
    if (!box.IsValid() || !point.IsFinite()) {
        return Result<T>::Failure(ErrorCode::InvalidArgument);
    }
    const Vector3<T> local = box.ToLocal(point);
    return Result<T>::Success(detail::BoxDistanceSquared(-box.halfExtents, box.halfExtents, local));
}

// cylinder 위(경계 포함)의 최근접점이다. 축 성분을 두 끝면 사이로, 축에 수직인 성분을 반지름으로
// 자른다(곱집합의 성분별 투영이라 끝면 모서리 영역에서도 정확하다). 안(ContainsPoint와 같은 식)의
// 점은 그 점 자체를 정확히 돌려준다. 자른 점이 반올림으로 포함 식의 밖이 되면 안쪽 기준점과의 이분
// 탐색으로 반올림 수준 안쪽의 경계 점을 돌려주므로, radius > 0이면 결과에 대한 ContainsPoint는 항상
// Hit다. radius 0은 ContainsPoint가 Hit인 점이면 그 점 자체, 아니면 선분 위 최근접점이며 두께가
// 없어 결과의 ContainsPoint가 반올림으로 Miss일 수 있다(근사).
// 실패(InvalidArgument): 잘못된 cylinder, 유한하지 않은 point.
template <typename T>
Result<Vector3<T>> TryClosestPoint(const Cylinder<T>& cylinder, const Vector3<T>& point) noexcept {
    if (!cylinder.IsValid() || !point.IsFinite()) {
        return Result<Vector3<T>>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<Vector3<T>>::Success(detail::ClosestOnCylinder(cylinder, point));
}

// cylinder까지의 제곱 거리다. 최근접점까지 거리를 다시 재지 않고 축 방향(끝면 밖)과 수직
// 방향(반지름 밖) 초과분의 제곱합으로 계산한다. 결과가 0이면 ContainsPoint가 Hit이고 그 역도
// 같다: 안의 점은 정확히 0이고, 밖의 점은 반올림·underflow로 0이 되지 않도록 가장 작은 양의 정규
// 값(NumericLimits<T>::Min())으로 올린다(실제 거리²가 그보다 작은 밖의 점은 결과가 실제보다 크다).
// radius 0은 선분까지의 제곱 거리다. 실패(InvalidArgument): 잘못된 cylinder, 유한하지 않은 point.
template <typename T>
Result<T> TryDistanceSquared(const Cylinder<T>& cylinder, const Vector3<T>& point) noexcept {
    if (!cylinder.IsValid() || !point.IsFinite()) {
        return Result<T>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<T>::Success(detail::CylinderDistanceSquared(cylinder, point));
}

// 두 segment의 최근접점이다. 평행·0 길이 segment도 처리하며 인자 순서를 바꾸면 s/t와 점만 바뀌고
// 거리는 비트 단위로 같다. UE SegmentDistToSegmentSafe의 절대 허용 오차(1e-4) 대신 정확한 0 판정을
// 쓴다.
template <typename T>
Result<SegmentClosestPoints<T>> TryClosestPoints(const Segment<T>& first,
                                                 const Segment<T>& second) noexcept {
    if (!first.IsValid() || !second.IsValid()) {
        return Result<SegmentClosestPoints<T>>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<SegmentClosestPoints<T>>::Success(
        detail::SymmetricClosestSegmentSegment(first, second));
}

} // namespace rocks
