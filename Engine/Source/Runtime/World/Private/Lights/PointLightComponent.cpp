#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Lights/LightComponentBase.generated.h>
#include <World/Lights/LightComponent.generated.h>
#include <World/Lights/LocalLightComponent.generated.h>
#include <World/Lights/LightComponentBase.h>
#include <World/Lights/LightComponent.h>
#include <World/Lights/LocalLightComponent.h>
#include <World/Lights/PointLightComponent.generated.h>
#include <World/Lights/PointLightComponent.h>
#include <World/SceneComponent.generated.h>
#include "../ComponentValueChecks.h"
#include "LightUnitMath.h"

// 광원 컴포넌트다(Docs/Design/EngineRuntime 24.2절, D1). 데이터·순수 계산만 있고 렌더 연결은 RS
// 단계다.
namespace rocks {

float PointLightComponent::BrightnessWithUnits(float lumensFactor) const noexcept {
    // UE PointLightComponent.cpp:127–157(Spot은 lumensFactor만 다르다).
    float brightness = intensity_;
    if (!useInverseSquaredFalloff_) {
        return brightness;
    }

    switch (intensityUnits_) {
    case LightUnits::Candelas:
        return brightness * light_math::SquareCentimetersPerSquareMeter;
    case LightUnits::Nits:
        // 캡슐 면적(구 + 원통, cm²)이다.
        return brightness *
               (4.0f * math::Pi<float> * sourceRadius_ * (sourceRadius_ + 0.5f * sourceLength_));
    case LightUnits::Lumens:
        return brightness * light_math::SquareCentimetersPerSquareMeter * lumensFactor;
    case LightUnits::EV:
        return light_math::Ev100ToLuminance(brightness) *
               light_math::SquareCentimetersPerSquareMeter;
    case LightUnits::Unitless:
    default:
        return brightness * light_math::LegacyUnitlessScale;
    }
}

float PointLightComponent::ComputeLightBrightness() const noexcept {
    return BrightnessWithUnits(1.0f / (4.0f * math::Pi<float>));
}

Result<void> PointLightComponent::SetUseInverseSquaredFalloff(bool use) noexcept {
    if (auto allowed = CheckDynamicDataChange("useInverseSquaredFalloff", false); !allowed) {
        return allowed;
    }

    if (useInverseSquaredFalloff_ == use) {
        return Result<void>::Success();
    }

    // UE: 1/r²가 꺼지면 단위가 Unitless로 돌아간다(PointLightComponent.cpp:230·270).
    useInverseSquaredFalloff_ = use;
    if (!use) {
        intensityUnits_ = LightUnits::Unitless;
        ClampIntensityForUnits();
    }

    MarkRenderStateDirtyIfPresent();
    return Result<void>::Success();
}

Result<void> PointLightComponent::SetLightFalloffExponent(float exponent) noexcept {
    if (!math::IsFinite(exponent) || exponent < MinLightFalloffExponent) {
        return component_checks::Invalid();
    }

    return ChangeLightData(lightFalloffExponent_, exponent, "lightFalloffExponent");
}

Result<void> PointLightComponent::SetSourceRadius(float radius) noexcept {
    if (!component_checks::InRange(radius, 0.0f, MaxSourceSize)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(sourceRadius_, radius, "sourceRadius");
}

Result<void> PointLightComponent::SetSoftSourceRadius(float radius) noexcept {
    if (!component_checks::InRange(radius, 0.0f, MaxSourceSize)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(softSourceRadius_, radius, "softSourceRadius");
}

Result<void> PointLightComponent::SetSourceLength(float length) noexcept {
    if (!component_checks::InRange(length, 0.0f, MaxSourceSize)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(sourceLength_, length, "sourceLength");
}

} // namespace rocks
