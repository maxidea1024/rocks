#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/Primitives/ShapeComponent.generated.h>
#include <World/Primitives/ShapeComponent.h>
#include "../ComponentValueChecks.h"

// Shape 컴포넌트다(Docs/Design/EngineRuntime 24.3.6절, D2). 기하·bounds는 끝까지 동작하고 충돌은
// CR16이다.
namespace rocks {

Result<void> ShapeComponent::SetLineThickness(float thickness) noexcept {
    if (!component_checks::NonNegative(thickness)) {
        return component_checks::Invalid();
    }

    Change(lineThickness_, thickness);
    return Result<void>::Success();
}

ComponentBounds ShapeComponent::CalcBounds(const Transform& world) const noexcept {
    ComponentBounds local;
    (void)TryLocalBounds(local);
    return TransformComponentBounds(local, world.ToMatrix());
}

Result<double> ShapeComponent::TryDistanceToShape(const Vec3d& point) const noexcept {
    auto shape = TryWorldVolumeShape();
    if (!shape) {
        return Result<double>::Failure(shape.Error());
    }

    return TryDistanceToVolume(shape.Value(), point);
}

} // namespace rocks
