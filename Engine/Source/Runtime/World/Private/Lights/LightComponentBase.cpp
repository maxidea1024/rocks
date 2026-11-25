#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Lights/LightComponentBase.generated.h>
#include <World/Lights/LightComponent.generated.h>
#include <World/Lights/LightComponentBase.h>
#include <World/Lights/LightComponent.h>
#include <World/SceneComponent.generated.h>
#include "../ComponentValueChecks.h"
#include "LightUnitMath.h"

// 광원 컴포넌트다(Docs/Design/EngineRuntime 24.2절, D1). 데이터·순수 계산만 있고 렌더 연결은 RS
// 단계다.
namespace rocks {

Result<void> LightComponentBase::SetIntensity(float intensity) noexcept {
    // UE PostEditChangeProperty(LightComponent.cpp:781): EV가 아니면 0 이상.
    if (!math::IsFinite(intensity) || (intensity < 0.0f && IntensityUnits() != LightUnits::EV)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(intensity_, intensity, "intensity");
}

LinearColor LightComponentBase::LinearLightColor() const noexcept {
    return LinearColor::FromSRGB(lightColor_);
}

Result<void> LightComponentBase::SetLightColor(Color color) noexcept {
    return ChangeLightData(lightColor_, color, "lightColor");
}

Result<void> LightComponentBase::SetLightColor(const LinearColor& color, bool srgb) noexcept {
    if (!color.IsFinite()) {
        return component_checks::Invalid();
    }

    const Color encoded = srgb ? color.ToSRGB() : color.QuantizeRound();
    return ChangeLightData(lightColor_, encoded, "lightColor");
}

Result<void> LightComponentBase::SetAffectsWorld(bool affects) noexcept {
    if (auto allowed = CheckChangeBeforeGameInitialization("affectsWorld"); !allowed) {
        return allowed;
    }

    if (affectsWorld_ != affects) {
        affectsWorld_ = affects;
        MarkRenderStateDirtyIfPresent();
    }

    return Result<void>::Success();
}

Result<void> LightComponentBase::SetCastShadows(bool cast) noexcept {
    return ChangeLightData(castShadows_, cast, "castShadows");
}

Result<void> LightComponentBase::SetCastStaticShadows(bool cast) noexcept {
    return ChangeLightData(castStaticShadows_, cast, "castStaticShadows");
}

Result<void> LightComponentBase::SetCastDynamicShadows(bool cast) noexcept {
    return ChangeLightData(castDynamicShadows_, cast, "castDynamicShadows");
}

Result<void> LightComponentBase::SetAffectTranslucentLighting(bool affects) noexcept {
    return ChangeLightData(affectTranslucentLighting_, affects, "affectTranslucentLighting");
}

Result<void> LightComponentBase::SetTransmission(bool transmission) noexcept {
    return ChangeLightData(transmission_, transmission, "transmission");
}

Result<void> LightComponentBase::SetCastVolumetricShadow(bool cast) noexcept {
    return ChangeLightData(castVolumetricShadow_, cast, "castVolumetricShadow");
}

Result<void> LightComponentBase::SetIndirectLightingIntensity(float intensity) noexcept {
    if (!math::IsFinite(intensity)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(indirectLightingIntensity_, intensity, "indirectLightingIntensity");
}

Result<void> LightComponentBase::SetVolumetricScatteringIntensity(float intensity) noexcept {
    if (!math::IsFinite(intensity)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(volumetricScatteringIntensity_, intensity,
                           "volumetricScatteringIntensity");
}

bool LightComponentBase::HasStaticLighting() const noexcept {
    return Mobility() == ComponentMobility::Static && !GetOwner().IsNull();
}

bool LightComponentBase::HasStaticShadowing() const noexcept {
    return Mobility() != ComponentMobility::Movable && !GetOwner().IsNull();
}

} // namespace rocks
