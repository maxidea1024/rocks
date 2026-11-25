#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Environment/BoxReflectionCaptureComponent.generated.h>
#include <World/Environment/BoxReflectionCaptureComponent.h>
#include <World/Environment/ReflectionCaptureComponent.generated.h>
#include <World/Environment/ReflectionSourceType.generated.h>
#include <World/SceneComponent.generated.h>
#include "../ComponentValueChecks.h"

// 상자 반사 캡처 컴포넌트다(Docs/Design/EngineRuntime 27.6절, D5).
namespace rocks {

Result<void> BoxReflectionCaptureComponent::SetBoxTransitionDistance(float distance) noexcept {
    if (!math::IsFinite(distance)) {
        return component_checks::Invalid();
    }

    boxTransitionDistance_ = distance;
    MarkRenderStateDirtyIfPresent();
    return Result<void>::Success();
}

double BoxReflectionCaptureComponent::InfluenceBoundingRadius() const noexcept {
    // UE GetInfluenceBoundingRadius(ReflectionCaptureComponent.cpp:1364): 상자 크기가 scale이다.
    return WorldScale3D().Length();
}

} // namespace rocks
