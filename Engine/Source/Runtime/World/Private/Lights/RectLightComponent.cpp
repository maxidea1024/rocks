#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Lights/LightComponentBase.generated.h>
#include <World/Lights/LightComponent.generated.h>
#include <World/Lights/LocalLightComponent.generated.h>
#include <World/Lights/LightComponentBase.h>
#include <World/Lights/LightComponent.h>
#include <World/Lights/LocalLightComponent.h>
#include <World/Lights/RectLightComponent.generated.h>
#include <World/Lights/RectLightComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/ComponentValueChecks.h>
#include "LightUnitMath.h"

// 광원 컴포넌트다(Docs/Design/EngineRuntime 24.2절, D1). 데이터·순수 계산만 있고 렌더 연결은 RS
// 단계다.
namespace rocks {

float RectLightComponent::ComputeLightBrightness() const noexcept {
    // UE RectLightComponent.cpp:181–219(옛 EV 오식 호환 bLightRequiresBrokenEVMath는 없다).
    const float brightness = intensity_;
    switch (intensityUnits_) {
    case LightUnits::Candelas:
        return brightness * light_math::SquareCentimetersPerSquareMeter;
    case LightUnits::Nits:
        return brightness * (sourceWidth_ * sourceHeight_);
    case LightUnits::Lumens:
        return brightness * (light_math::SquareCentimetersPerSquareMeter / math::Pi<float>);
    case LightUnits::EV:
        return light_math::Ev100ToLuminance(brightness) *
               light_math::SquareCentimetersPerSquareMeter;
    case LightUnits::Unitless:
    default:
        return brightness * light_math::LegacyUnitlessScale;
    }
}

Result<void> RectLightComponent::SetSourceWidth(float width) noexcept {
    if (!component_checks::InRange(width, MinSourceSize, MaxSourceSize)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(sourceWidth_, width, "sourceWidth");
}

Result<void> RectLightComponent::SetSourceHeight(float height) noexcept {
    if (!component_checks::InRange(height, MinSourceSize, MaxSourceSize)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(sourceHeight_, height, "sourceHeight");
}

Result<void> RectLightComponent::SetBarnDoorAngle(float degrees) noexcept {
    if (!component_checks::InRange(degrees, 0.0f, MaxBarnDoorAngle)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(barnDoorAngle_, degrees, "barnDoorAngle");
}

Result<void> RectLightComponent::SetBarnDoorLength(float length) noexcept {
    if (!math::IsFinite(length) || length < MinBarnDoorLength) {
        return component_checks::Invalid();
    }

    return ChangeLightData(barnDoorLength_, length, "barnDoorLength");
}

} // namespace rocks
