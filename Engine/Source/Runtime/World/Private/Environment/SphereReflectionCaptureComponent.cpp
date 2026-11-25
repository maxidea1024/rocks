#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Environment/ReflectionCaptureComponent.generated.h>
#include <World/Environment/ReflectionSourceType.generated.h>
#include <World/Environment/SphereReflectionCaptureComponent.generated.h>
#include <World/Environment/SphereReflectionCaptureComponent.h>
#include <World/SceneComponent.generated.h>
#include "../ComponentValueChecks.h"

// 구 반사 캡처 컴포넌트다(Docs/Design/EngineRuntime 27.6절, D5).
namespace rocks {

Result<void> SphereReflectionCaptureComponent::SetInfluenceRadius(float radius) noexcept {
    if (!math::IsFinite(radius)) {
        return component_checks::Invalid();
    }

    influenceRadius_ = radius;
    MarkRenderStateDirtyIfPresent();
    return Result<void>::Success();
}

} // namespace rocks
