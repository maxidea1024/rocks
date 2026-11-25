#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Environment/PlaneReflectionCaptureComponent.generated.h>
#include <World/Environment/PlaneReflectionCaptureComponent.h>
#include <World/Environment/ReflectionCaptureComponent.generated.h>
#include <World/Environment/ReflectionSourceType.generated.h>
#include <World/SceneComponent.generated.h>
#include "../ComponentValueChecks.h"

// 평면 반사 캡처 컴포넌트다(Docs/Design/EngineRuntime 27.6절, D5).
namespace rocks {

Result<void> PlaneReflectionCaptureComponent::SetInfluenceRadiusScale(float scale) noexcept {
    if (!math::IsFinite(scale)) {
        return component_checks::Invalid();
    }

    influenceRadiusScale_ = scale;
    MarkRenderStateDirtyIfPresent();
    return Result<void>::Success();
}

double PlaneReflectionCaptureComponent::InfluenceBoundingRadius() const noexcept {
    // UE :1398 — |(scale.y, scale.z)| × 배율.
    const Vec3d scale = WorldScale3D();
    return math::Sqrt(scale.y * scale.y + scale.z * scale.z) *
           static_cast<double>(influenceRadiusScale_);
}

} // namespace rocks
