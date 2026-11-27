#include <Collision/CollisionShape.h>
#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/Primitives/ShapeComponent.generated.h>
#include <World/Primitives/SphereComponent.generated.h>
#include <World/Primitives/ShapeComponent.h>
#include <World/Primitives/SphereComponent.h>
#include <World/ComponentValueChecks.h>

// Shape 컴포넌트다(Docs/Design/EngineRuntime 24.3.6절, D2). 기하·bounds는 끝까지 동작하고 충돌은
// CR16이다.
namespace rocks {

float SphereComponent::ScaledSphereRadius() const noexcept {
    const Vec3d scale = WorldScale3D();
    const double minimum =
        math::Min(math::Min(math::Abs(scale.x), math::Abs(scale.y)), math::Abs(scale.z));
    return sphereRadius_ * static_cast<float>(minimum);
}

Result<void> SphereComponent::SetSphereRadius(float radius) noexcept {
    if (!component_checks::NonNegative(radius)) {
        return component_checks::Invalid();
    }

    if (sphereRadius_ != radius) {
        sphereRadius_ = radius;
        InvalidateBounds();
        MarkRenderStateDirtyIfPresent();
        UpdateBodyGeometry();
    }

    return Result<void>::Success();
}

bool SphereComponent::TryLocalBounds(ComponentBounds& local) const noexcept {
    const double radius = static_cast<double>(sphereRadius_);
    local = ComponentBounds{};
    local.boxExtent = Vec3d(radius, radius, radius);
    local.sphereRadius = radius;
    return true;
}

CollisionShape SphereComponent::GetCollisionShape(double inflation) const noexcept {
    return CollisionShape::Sphere(static_cast<double>(ScaledSphereRadius())).Inflate(inflation);
}

Result<VolumeShape> SphereComponent::TryWorldVolumeShape() const noexcept {
    return VolumeShape::TrySphere(WorldLocation(), static_cast<double>(ScaledSphereRadius()));
}

bool SphereComponent::BuildBodyGeometry(BodyGeometry& geometry) const noexcept {
    geometry.kind = BodyGeometryKind::Sphere;
    geometry.radius = static_cast<double>(sphereRadius_);
    return true;
}

} // namespace rocks
