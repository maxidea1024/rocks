#include <Core/Math/VolumeShape.h>

#include <Core/Math/IntersectionDistance.h>
#include <Core/Math/MathUtility.h>
#include <Core/Math/Shapes.h>

// 영역 형상 거리다(Docs/Design/EngineRuntime 28.2절).
namespace rocks {

namespace {

Result<double> InvalidVolume() noexcept {
    return Result<double>::Failure(ErrorCode::InvalidArgument);
}

bool NonNegativeFinite(double value) noexcept {
    return math::IsFinite(value) && value >= 0.0;
}

bool ValidVolumeShape(const VolumeShape& shape) noexcept {
    if (!shape.center.IsFinite() || !shape.rotation.IsNormalized(ShapeUnitTolerance<double>)) {
        return false;
    }

    switch (shape.kind) {
    case VolumeShapeKind::Box:
        return NonNegativeFinite(shape.boxHalfExtent.x) &&
               NonNegativeFinite(shape.boxHalfExtent.y) && NonNegativeFinite(shape.boxHalfExtent.z);
    case VolumeShapeKind::Sphere:
        return NonNegativeFinite(shape.radius);
    case VolumeShapeKind::Capsule:
        return NonNegativeFinite(shape.radius) && NonNegativeFinite(shape.halfHeight) &&
               shape.halfHeight >= shape.radius;
    }

    return false;
}

} // namespace

Result<VolumeShape> VolumeShape::TryBox(const Vec3d& center, const Quatd& rotation,
                                        const Vec3d& halfExtent) noexcept {
    auto unit = rotation.TryNormalize();
    if (!unit || !center.IsFinite() || !NonNegativeFinite(halfExtent.x) ||
        !NonNegativeFinite(halfExtent.y) || !NonNegativeFinite(halfExtent.z)) {
        return Result<VolumeShape>::Failure(ErrorCode::InvalidArgument);
    }

    VolumeShape shape;
    shape.kind = VolumeShapeKind::Box;
    shape.center = center;
    shape.rotation = unit.Value();
    shape.boxHalfExtent = halfExtent;
    return Result<VolumeShape>::Success(shape);
}

Result<VolumeShape> VolumeShape::TrySphere(const Vec3d& center, double radius) noexcept {
    if (!center.IsFinite() || !NonNegativeFinite(radius)) {
        return Result<VolumeShape>::Failure(ErrorCode::InvalidArgument);
    }

    VolumeShape shape;
    shape.kind = VolumeShapeKind::Sphere;
    shape.center = center;
    shape.radius = radius;
    return Result<VolumeShape>::Success(shape);
}

Result<VolumeShape> VolumeShape::TryCapsule(const Vec3d& center, const Quatd& rotation,
                                            double radius, double halfHeight) noexcept {
    auto unit = rotation.TryNormalize();
    if (!unit || !center.IsFinite() || !NonNegativeFinite(radius) ||
        !NonNegativeFinite(halfHeight)) {
        return Result<VolumeShape>::Failure(ErrorCode::InvalidArgument);
    }

    VolumeShape shape;
    shape.kind = VolumeShapeKind::Capsule;
    shape.center = center;
    shape.rotation = unit.Value();
    shape.radius = radius;
    shape.halfHeight = halfHeight < radius ? radius : halfHeight;
    return Result<VolumeShape>::Success(shape);
}

Result<double> TryDistanceToVolume(const VolumeShape& shape, const Vec3d& point) noexcept {
    if (!point.IsFinite() || !ValidVolumeShape(shape)) {
        return InvalidVolume();
    }

    switch (shape.kind) {
    case VolumeShapeKind::Box: {
        const OrientedBox<double> box(shape.center, shape.rotation, shape.boxHalfExtent);
        auto squared = TryDistanceSquared(box, point);
        if (!squared) {
            return Result<double>::Failure(squared.Error());
        }

        return Result<double>::Success(math::Sqrt(squared.Value()));
    }

    case VolumeShapeKind::Sphere: {
        const double outside = (point - shape.center).Length() - shape.radius;
        return Result<double>::Success(outside > 0.0 ? outside : 0.0);
    }

    case VolumeShapeKind::Capsule: {
        // 축 선분은 center ± 회전Z × (halfHeight − radius)다.
        const Vec3d axis =
            shape.rotation.RotateVector(Vec3d(0.0, 0.0, shape.halfHeight - shape.radius));
        const Segment<double> segment(shape.center - axis, shape.center + axis);
        auto squared = TryDistanceSquared(segment, point);
        if (!squared) {
            return Result<double>::Failure(squared.Error());
        }

        const double outside = math::Sqrt(squared.Value()) - shape.radius;
        return Result<double>::Success(outside > 0.0 ? outside : 0.0);
    }
    }

    return InvalidVolume();
}

} // namespace rocks
