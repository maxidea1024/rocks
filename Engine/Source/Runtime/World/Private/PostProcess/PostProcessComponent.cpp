#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/PostProcess/PostProcessComponent.generated.h>
#include <World/PostProcess/PostProcessComponent.h>
#include <World/PostProcess/PostProcessSettings.generated.h>
#include <World/SceneComponent.generated.h>
#include "../ComponentValueChecks.h"

// 후처리 볼륨 컴포넌트다(Docs/Design/EngineRuntime 27.5절, D5).
namespace rocks {

Result<void> PostProcessComponent::SetSettings(const PostProcessSettings& settings) noexcept {
    if (!IsValidPostProcessSettings(settings)) {
        return component_checks::Invalid();
    }

    if (!(settings_ == settings)) {
        settings_ = settings;
        MarkRenderStateDirtyIfPresent();
    }

    return Result<void>::Success();
}

Result<void> PostProcessComponent::SetPriority(float priority) noexcept {
    if (!math::IsFinite(priority)) {
        return component_checks::Invalid();
    }

    priority_ = priority;
    return Result<void>::Success();
}

Result<void> PostProcessComponent::SetBlendRadius(float radius) noexcept {
    if (!component_checks::NonNegative(radius)) {
        return component_checks::Invalid();
    }

    blendRadius_ = radius;
    return Result<void>::Success();
}

Result<void> PostProcessComponent::SetBlendWeight(float weight) noexcept {
    if (!math::IsFinite(weight)) {
        return component_checks::Invalid();
    }

    blendWeight_ = weight;
    return Result<void>::Success();
}

} // namespace rocks
