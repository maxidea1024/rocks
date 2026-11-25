#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Lights/LightComponentBase.generated.h>
#include <World/Lights/LightComponent.generated.h>
#include <World/Lights/LocalLightComponent.generated.h>
#include <World/Lights/LightComponentBase.h>
#include <World/Lights/LightComponent.h>
#include <World/Lights/LocalLightComponent.h>
#include <World/SceneComponent.generated.h>
#include "../ComponentValueChecks.h"
#include "LightUnitMath.h"

// 광원 컴포넌트다(Docs/Design/EngineRuntime 24.2절, D1). 데이터·순수 계산만 있고 렌더 연결은 RS
// 단계다.
namespace rocks {

Result<void> LocalLightComponent::SetIntensityUnits(LightUnits units) noexcept {
    if (!light_math::IsKnownUnits(units) || !AcceptsIntensityUnits(units)) {
        return component_checks::Invalid();
    }

    if (auto changed = ChangeLightData(intensityUnits_, units, "intensityUnits"); !changed) {
        return changed;
    }

    ClampIntensityForUnits();
    return Result<void>::Success();
}

void LocalLightComponent::ClampIntensityForUnits() noexcept {
    // UE PostEditChangeProperty(LightComponent.cpp:781): EV가 아니면 세기를 0 이상으로 둔다.
    if (IntensityUnits() != LightUnits::EV && intensity_ < 0.0f) {
        intensity_ = 0.0f;
    }
}

Result<void> LocalLightComponent::SetAttenuationRadius(float radius) noexcept {
    // 음수 반경은 경계 구가 뒤집힌다(UE UIMin 8 — 거부는 0 미만만).
    if (!component_checks::NonNegative(radius)) {
        return component_checks::Invalid();
    }

    // UE SetAttenuationRadius는 AreDynamicDataChangesAllowed(false)다(Stationary도 막는다).
    return ChangeLightData(attenuationRadius_, radius, "attenuationRadius", true);
}

Result<void> LocalLightComponent::SetInverseExposureBlend(float blend) noexcept {
    if (!component_checks::InRange(blend, 0.0f, 1.0f)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(inverseExposureBlend_, blend, "inverseExposureBlend");
}

float LocalLightComponent::UnitsConversionFactor(LightUnits from, LightUnits to,
                                                 float cosHalfConeAngle) noexcept {
    // UE GetUnitsConversionFactor(LocalLightComponent.cpp:146–225)와 같은 식·순서다.
    cosHalfConeAngle = math::Clamp(cosHalfConeAngle, -1.0f, 1.0f - light_math::ConeCosineMargin);
    if (from == to) {
        return 1.0f;
    }

    float factor = 1.0f;
    switch (from) {
    case LightUnits::Nits:
        break;
    case LightUnits::Candelas:
    case LightUnits::EV:
        factor = light_math::SquareCentimetersPerSquareMeter;
        break;
    case LightUnits::Lumens:
        factor = light_math::SquareCentimetersPerSquareMeter *
                 light_math::InverseConeSolidAngle(cosHalfConeAngle);
        break;
    case LightUnits::Unitless:
    default:
        factor = light_math::LegacyUnitlessScale;
        break;
    }

    switch (to) {
    case LightUnits::Nits:
        break;
    case LightUnits::Candelas:
    case LightUnits::EV:
        factor *= 1.0f / light_math::SquareCentimetersPerSquareMeter;
        break;
    case LightUnits::Lumens:
        factor *= 2.0f * math::Pi<float> * (1.0f - cosHalfConeAngle) /
                  light_math::SquareCentimetersPerSquareMeter;
        break;
    case LightUnits::Unitless:
    default:
        factor *= 1.0f / light_math::LegacyUnitlessScale;
        break;
    }

    return factor;
}

Sphere<double> LocalLightComponent::BoundingSphere() const noexcept {
    return Sphere<double>(WorldLocation(), static_cast<double>(attenuationRadius_));
}

bool LocalLightComponent::AffectsBounds(const ComponentBounds& bounds) const noexcept {
    const double reach = static_cast<double>(attenuationRadius_) + bounds.sphereRadius;
    return (bounds.origin - WorldLocation()).LengthSquared() <= reach * reach;
}

} // namespace rocks
