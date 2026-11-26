#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Lights/LightComponentBase.generated.h>
#include <World/Lights/LightComponent.generated.h>
#include <World/Lights/LightComponentBase.h>
#include <World/Lights/LightComponent.h>
#include <World/Lights/LightingChannels.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/ComponentValueChecks.h>
#include "LightUnitMath.h"

// 광원 컴포넌트다(Docs/Design/EngineRuntime 24.2절, D1). 데이터·순수 계산만 있고 렌더 연결은 RS
// 단계다.
namespace rocks {

Vec3d LightComponent::Direction() const noexcept {
    // UE GetComponentTransform().GetUnitAxis(X): 월드 행렬 0행(scale 포함)을 정규화한다.
    auto axis = WorldMatrix().GetScaledAxis(0).TryNormalize();
    return axis ? axis.Value() : Vec3d(1.0, 0.0, 0.0);
}

Result<void> LightComponent::SetTemperature(float kelvin) noexcept {
    if (!math::IsFinite(kelvin)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(temperature_, kelvin, "temperature");
}

Result<void> LightComponent::SetUseTemperature(bool use) noexcept {
    return ChangeLightData(useTemperature_, use, "useTemperature");
}

Result<void> LightComponent::SetMaxDrawDistance(float distance) noexcept {
    if (!math::IsFinite(distance)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(maxDrawDistance_, distance, "maxDrawDistance");
}

Result<void> LightComponent::SetMaxDistanceFadeRange(float range) noexcept {
    if (!math::IsFinite(range)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(maxDistanceFadeRange_, range, "maxDistanceFadeRange");
}

Result<void> LightComponent::SetSpecularScale(float scale) noexcept {
    // UE PostEditChangeProperty clamp [0, 1](LightComponent.cpp:785).
    if (!component_checks::InRange(scale, 0.0f, 1.0f)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(specularScale_, scale, "specularScale");
}

Result<void> LightComponent::SetDiffuseScale(float scale) noexcept {
    if (!component_checks::InRange(scale, 0.0f, 1.0f)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(diffuseScale_, scale, "diffuseScale");
}

Result<void> LightComponent::SetShadowResolutionScale(float scale) noexcept {
    if (!component_checks::NonNegative(scale)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(shadowResolutionScale_, scale, "shadowResolutionScale");
}

Result<void> LightComponent::SetShadowBias(float bias) noexcept {
    if (!math::IsFinite(bias) ||
        (LimitsShadowBias() && !component_checks::InRange(bias, 0.0f, MaxShadowBias))) {
        return component_checks::Invalid();
    }

    return ChangeLightData(shadowBias_, bias, "shadowBias");
}

Result<void> LightComponent::SetShadowSlopeBias(float bias) noexcept {
    if (!math::IsFinite(bias) ||
        (LimitsShadowBias() && !component_checks::InRange(bias, 0.0f, MaxShadowBias))) {
        return component_checks::Invalid();
    }

    return ChangeLightData(shadowSlopeBias_, bias, "shadowSlopeBias");
}

Result<void> LightComponent::SetShadowSharpen(float sharpen) noexcept {
    if (!math::IsFinite(sharpen)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(shadowSharpen_, sharpen, "shadowSharpen");
}

Result<void> LightComponent::SetContactShadowLength(float length) noexcept {
    if (!component_checks::NonNegative(length)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(contactShadowLength_, length, "contactShadowLength");
}

Result<void> LightComponent::SetContactShadowLengthInWorldSpace(bool worldSpace) noexcept {
    return ChangeLightData(contactShadowLengthInWorldSpace_, worldSpace,
                           "contactShadowLengthInWorldSpace");
}

Result<void> LightComponent::SetCastTranslucentShadows(bool cast) noexcept {
    return ChangeLightData(castTranslucentShadows_, cast, "castTranslucentShadows");
}

Result<void> LightComponent::SetLightingChannels(const rocks::LightingChannels& channels) noexcept {
    // UE SetLightingChannels는 Mobility를 보지 않는다(LightComponent.cpp:1366).
    if (!(lightingChannels_ == channels)) {
        lightingChannels_ = channels;
        MarkRenderStateDirtyIfPresent();
    }

    return Result<void>::Success();
}

} // namespace rocks
