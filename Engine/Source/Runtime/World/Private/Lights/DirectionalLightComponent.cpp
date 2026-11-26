#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Lights/DirectionalLightComponent.generated.h>
#include <World/Lights/DirectionalLightComponent.h>
#include <World/Lights/LightComponentBase.generated.h>
#include <World/Lights/LightComponent.generated.h>
#include <World/Lights/LightComponentBase.h>
#include <World/Lights/LightComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/ComponentValueChecks.h>
#include "LightUnitMath.h"

// 광원 컴포넌트다(Docs/Design/EngineRuntime 24.2절, D1). 데이터·순수 계산만 있고 렌더 연결은 RS
// 단계다.
namespace rocks {

Result<void> DirectionalLightComponent::SetLightSourceAngle(float degrees) noexcept {
    if (!component_checks::NonNegative(degrees)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(lightSourceAngle_, degrees, "lightSourceAngle");
}

Result<void> DirectionalLightComponent::SetLightSourceSoftAngle(float degrees) noexcept {
    if (!component_checks::NonNegative(degrees)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(lightSourceSoftAngle_, degrees, "lightSourceSoftAngle");
}

Result<void> DirectionalLightComponent::SetShadowAmount(float amount) noexcept {
    if (!component_checks::InRange(amount, 0.0f, 1.0f)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(shadowAmount_, amount, "shadowAmount");
}

Result<void>
DirectionalLightComponent::SetDynamicShadowDistanceMovableLight(float distance) noexcept {
    if (!component_checks::NonNegative(distance)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(dynamicShadowDistanceMovableLight_, distance,
                           "dynamicShadowDistanceMovableLight");
}

Result<void>
DirectionalLightComponent::SetDynamicShadowDistanceStationaryLight(float distance) noexcept {
    if (!component_checks::NonNegative(distance)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(dynamicShadowDistanceStationaryLight_, distance,
                           "dynamicShadowDistanceStationaryLight");
}

Result<void> DirectionalLightComponent::SetDynamicShadowCascades(int32 cascades) noexcept {
    if (cascades < 0 || cascades > MaxShadowCascades) {
        return component_checks::Invalid();
    }

    return ChangeLightData(dynamicShadowCascades_, cascades, "dynamicShadowCascades");
}

Result<void> DirectionalLightComponent::SetCascadeDistributionExponent(float exponent) noexcept {
    if (!component_checks::InRange(exponent, MinCascadeDistributionExponent,
                                   MaxCascadeDistributionExponent)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(cascadeDistributionExponent_, exponent, "cascadeDistributionExponent");
}

Result<void> DirectionalLightComponent::SetCascadeTransitionFraction(float fraction) noexcept {
    if (!component_checks::InRange(fraction, MinCascadeTransitionFraction,
                                   MaxCascadeTransitionFraction)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(cascadeTransitionFraction_, fraction, "cascadeTransitionFraction");
}

Result<void> DirectionalLightComponent::SetShadowDistanceFadeoutFraction(float fraction) noexcept {
    if (!component_checks::InRange(fraction, 0.0f, 1.0f)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(shadowDistanceFadeoutFraction_, fraction,
                           "shadowDistanceFadeoutFraction");
}

Result<void> DirectionalLightComponent::SetFarShadowCascadeCount(int32 count) noexcept {
    if (count < 0 || count > MaxShadowCascades) {
        return component_checks::Invalid();
    }

    return ChangeLightData(farShadowCascadeCount_, count, "farShadowCascadeCount");
}

Result<void> DirectionalLightComponent::SetFarShadowDistance(float distance) noexcept {
    if (!component_checks::NonNegative(distance)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(farShadowDistance_, distance, "farShadowDistance");
}

Result<void> DirectionalLightComponent::SetForwardShadingPriority(int32 priority) noexcept {
    if (priority < 0) {
        return component_checks::Invalid();
    }

    return ChangeLightData(forwardShadingPriority_, priority, "forwardShadingPriority");
}

Result<void> DirectionalLightComponent::SetAtmosphereSunLight(bool sun) noexcept {
    return ChangeLightData(atmosphereSunLight_, sun, "atmosphereSunLight");
}

Result<void> DirectionalLightComponent::SetAtmosphereSunLightIndex(int32 index) noexcept {
    if (index < 0 || index > MaxAtmosphereSunLightIndex) {
        return component_checks::Invalid();
    }

    return ChangeLightData(atmosphereSunLightIndex_, index, "atmosphereSunLightIndex");
}

Result<void>
DirectionalLightComponent::SetAtmosphereSunDiskColorScale(const LinearColor& scale) noexcept {
    if (!scale.IsFinite()) {
        return component_checks::Invalid();
    }

    return ChangeLightData(atmosphereSunDiskColorScale_, scale, "atmosphereSunDiskColorScale");
}

} // namespace rocks
