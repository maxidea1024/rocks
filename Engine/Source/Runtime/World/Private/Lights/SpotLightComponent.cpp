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
#include <World/Lights/SpotLightComponent.generated.h>
#include <World/Lights/PointLightComponent.h>
#include <World/Lights/SpotLightComponent.h>
#include <World/SceneComponent.generated.h>
#include "../ComponentValueChecks.h"
#include "LightUnitMath.h"

// 광원 컴포넌트다(Docs/Design/EngineRuntime 24.2절, D1). 데이터·순수 계산만 있고 렌더 연결은 RS
// 단계다.
namespace rocks {

float SpotLightComponent::ComputeLightBrightness() const noexcept {
    return BrightnessWithUnits(light_math::InverseConeSolidAngle(CosHalfConeAngle()));
}

Result<void> SpotLightComponent::SetInnerConeAngle(float degrees) noexcept {
    if (!math::IsFinite(degrees)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(innerConeAngle_, degrees, "innerConeAngle", true);
}

Result<void> SpotLightComponent::SetOuterConeAngle(float degrees) noexcept {
    if (!math::IsFinite(degrees)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(outerConeAngle_, degrees, "outerConeAngle", true);
}

SpotLightComponent::ConeAngles SpotLightComponent::ClampedConeAngles() const noexcept {
    // UE GetClampedConeAngles(SpotLightComponent.cpp:35–41).
    const float maxRadians = math::DegreesToRadians(MaxConeAngle);
    ConeAngles angles;
    angles.innerRadians = math::Clamp(math::DegreesToRadians(innerConeAngle_), 0.0f, maxRadians);
    angles.outerRadians = math::Clamp(math::DegreesToRadians(outerConeAngle_),
                                      angles.innerRadians + MinConeAngleGapRadians,
                                      maxRadians + MinConeAngleGapRadians);
    return angles;
}

float SpotLightComponent::CosHalfConeAngle() const noexcept {
    return math::Cos(HalfConeAngle());
}

} // namespace rocks
