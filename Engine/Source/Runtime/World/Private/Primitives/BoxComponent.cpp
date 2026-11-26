#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/Primitives/ShapeComponent.generated.h>
#include <World/Primitives/BoxComponent.generated.h>
#include <World/Primitives/ShapeComponent.h>
#include <World/Primitives/BoxComponent.h>
#include "../ComponentValueChecks.h"

// Shape 컴포넌트다(Docs/Design/EngineRuntime 24.3.6절, D2). 기하·bounds는 끝까지 동작하고 충돌은
// CR16이다.
namespace rocks {

Vec3d BoxComponent::ScaledBoxExtent() const noexcept {
    return boxExtent_ * WorldScale3D();
}

Result<void> BoxComponent::SetBoxExtent(const Vec3d& extent) noexcept {
    if (!component_checks::NonNegative(extent.x) || !component_checks::NonNegative(extent.y) ||
        !component_checks::NonNegative(extent.z)) {
        return component_checks::Invalid();
    }

    if (!(boxExtent_ == extent)) {
        boxExtent_ = extent;
        InvalidateBounds();
        MarkRenderStateDirtyIfPresent();
    }

    return Result<void>::Success();
}

bool BoxComponent::TryLocalBounds(ComponentBounds& local) const noexcept {
    // UE FBoxSphereBounds(FBox(-E, E)): 중심 0, extent E, 반지름 |E|.
    local = ComponentBounds{};
    local.boxExtent = boxExtent_;
    local.sphereRadius = boxExtent_.Length();
    return true;
}

Result<VolumeShape> BoxComponent::TryWorldVolumeShape() const noexcept {
    const Vec3d extent = ScaledBoxExtent();
    return VolumeShape::TryBox(
        WorldLocation(), WorldQuaternion(),
        Vec3d(math::Abs(extent.x), math::Abs(extent.y), math::Abs(extent.z)));
}

bool BoxComponent::BuildBodyGeometry(BodyGeometry& geometry) const noexcept {
    geometry.kind = BodyGeometryKind::Box;
    geometry.halfExtent = boxExtent_;
    return true;
}

} // namespace rocks
