#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/Primitives/ShapeComponent.generated.h>
#include <World/Primitives/CapsuleComponent.generated.h>
#include <World/Primitives/ShapeComponent.h>
#include <World/Primitives/CapsuleComponent.h>
#include "../ComponentValueChecks.h"

// Shape 컴포넌트다(Docs/Design/EngineRuntime 24.3.6절, D2). 기하·bounds는 끝까지 동작하고 충돌은
// CR16이다.
namespace rocks {

float CapsuleComponent::ScaledCapsuleRadius() const noexcept {
    // UE GetScaledCapsuleRadius: min(scale.x, scale.y)(부호 그대로).
    const Vec3d scale = WorldScale3D();
    return capsuleRadius_ * static_cast<float>(math::Min(scale.x, scale.y));
}

float CapsuleComponent::ScaledCapsuleHalfHeight() const noexcept {
    return capsuleHalfHeight_ * static_cast<float>(WorldScale3D().z);
}

Result<void> CapsuleComponent::SetCapsuleSize(float radius, float halfHeight) noexcept {
    if (!component_checks::NonNegative(radius) || !component_checks::NonNegative(halfHeight)) {
        return component_checks::Invalid();
    }

    // UE: 반 높이는 반지름 이상(반구 둘이 들어간다).
    const float raisedHalfHeight = math::Max(halfHeight, radius);
    if (capsuleRadius_ != radius || capsuleHalfHeight_ != raisedHalfHeight) {
        capsuleRadius_ = radius;
        capsuleHalfHeight_ = raisedHalfHeight;
        InvalidateBounds();
        MarkRenderStateDirtyIfPresent();
    }

    return Result<void>::Success();
}

bool CapsuleComponent::TryLocalBounds(ComponentBounds& local) const noexcept {
    const double radius = static_cast<double>(capsuleRadius_);
    const double halfHeight = static_cast<double>(capsuleHalfHeight_);
    local = ComponentBounds{};
    local.boxExtent = Vec3d(radius, radius, halfHeight);
    local.sphereRadius = halfHeight;
    return true;
}

} // namespace rocks
