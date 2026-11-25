#include <Core/Math/ConvexHull.h>

#include <Core/Math/IntersectionDistance.h>
#include <Core/Math/MathUtility.h>
#include <Core/Math/Shapes.h>

// 볼록 껍질 거리다(Docs/Design/EngineRuntime 28.2절).
namespace rocks {

namespace {

// 볼록 껍질의 최소 꼭짓점 수(사면체)와 삼각형 하나의 색인 수다.
constexpr usize ConvexHullMinimumVertices = 4;
constexpr usize ConvexTriangleIndexCount = 3;
// 중심이 면 평면에서 이 비율(× 껍질 크기)보다 가까우면 평평하다고 본다.
constexpr double ConvexFlatTolerance = 1.0e-9;

Result<double> InvalidConvex() noexcept {
    return Result<double>::Failure(ErrorCode::InvalidArgument);
}

} // namespace

Result<double> TryDistanceToConvexHull(const ConvexHullView& hull, const Vec3d& point) noexcept {
    const usize vertexCount = hull.vertices.Size();
    const usize indexCount = hull.triangleIndices.Size();
    if (!point.IsFinite() || vertexCount < ConvexHullMinimumVertices || indexCount == 0 ||
        indexCount % ConvexTriangleIndexCount != 0) {
        return InvalidConvex();
    }

    // 삼각형이 쓰는 꼭짓점(색인 자리마다)의 평균은 볼록체 안이다 — 평면 방향을 이쪽이 음이 되게
    // 맞춘다. 쓰이지 않는 꼭짓점은 넣지 않는다.
    Vec3d centroid;
    for (usize index = 0; index < indexCount; ++index) {
        const uint32 vertex = hull.triangleIndices[index];
        if (vertex >= vertexCount || !hull.vertices[vertex].IsFinite()) {
            return InvalidConvex();
        }

        centroid += hull.vertices[vertex];
    }

    centroid = centroid / static_cast<double>(indexCount);
    if (!centroid.IsFinite()) {
        return InvalidConvex();
    }

    // 평평함 판정의 길이 기준(중심에서 가장 먼 꼭짓점)이다.
    double hullScale = 0.0;
    for (usize index = 0; index < indexCount; ++index) {
        hullScale =
            math::Max(hullScale, (hull.vertices[hull.triangleIndices[index]] - centroid).Length());
    }

    bool inside = true;
    bool anyPlane = false;
    double nearestSquared = 0.0;
    bool anyTriangle = false;
    for (usize first = 0; first < indexCount; first += ConvexTriangleIndexCount) {
        const uint32 ia = hull.triangleIndices[first];
        const uint32 ib = hull.triangleIndices[first + 1];
        const uint32 ic = hull.triangleIndices[first + 2];
        const Triangle<double> triangle(hull.vertices[ia], hull.vertices[ib], hull.vertices[ic]);
        auto squared = TryDistanceSquared(triangle, point);
        if (!squared) {
            return Result<double>::Failure(squared.Error());
        }

        if (!anyTriangle || squared.Value() < nearestSquared) {
            nearestSquared = squared.Value();
            anyTriangle = true;
        }

        // 퇴화 삼각형은 평면이 없어 안 판정에서 뺀다(거리에는 넣었다).
        auto plane = Plane<double>::TryFromPoints(triangle.a, triangle.b, triangle.c);
        if (!plane) {
            continue;
        }

        anyPlane = true;
        const double centroidSide = plane.Value().SignedDistance(centroid);
        // 중심이 면 평면 위(오차 안)면 부피가 없거나 볼록이 아니다 — 방향을 정할 수 없다.
        if (math::Abs(centroidSide) <= ConvexFlatTolerance * hullScale) {
            return InvalidConvex();
        }

        const double pointSide = plane.Value().SignedDistance(point);
        // 바깥 쪽(중심과 반대 부호)으로 0보다 나가 있으면 밖이다.
        if ((centroidSide < 0.0 && pointSide > 0.0) || (centroidSide > 0.0 && pointSide < 0.0)) {
            inside = false;
        }
    }

    if (!anyPlane) {
        return InvalidConvex();
    }

    return Result<double>::Success(inside ? 0.0 : math::Sqrt(nearestSquared));
}

} // namespace rocks
