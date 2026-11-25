#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Lights/LightComponentBase.generated.h>
#include <World/Lights/LightComponentBase.h>
#include <World/SceneComponent.generated.h>
#include <World/Lights/SkyLightComponent.generated.h>
#include <World/Lights/SkyLightComponent.h>
#include "../ComponentValueChecks.h"
#include "LightUnitMath.h"

// 광원 컴포넌트다(Docs/Design/EngineRuntime 24.2절, D1). 데이터·순수 계산만 있고 렌더 연결은 RS
// 단계다.
namespace rocks {

Result<void> SkyLightComponent::SetRealTimeCapture(bool realTime) noexcept {
    return ChangeLightData(realTimeCapture_, realTime, "realTimeCapture");
}

Result<void> SkyLightComponent::SetSourceType(SkyLightSourceType type) noexcept {
    if (static_cast<uint8>(type) > static_cast<uint8>(SkyLightSourceType::SpecifiedCubemap)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(sourceType_, type, "sourceType");
}

Result<void> SkyLightComponent::SetSourceCubemapAngle(float degrees) noexcept {
    // UE는 범위를 자르지 않는다(UIMin 0·UIMax 360은 편집기 슬라이더 범위).
    if (!math::IsFinite(degrees)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(sourceCubemapAngle_, degrees, "sourceCubemapAngle");
}

Result<void> SkyLightComponent::SetCubemapResolution(int32 resolution) noexcept {
    if (resolution < 1) {
        return component_checks::Invalid();
    }

    // UE SanitizeCubemapSize: 2의 거듭제곱으로 올린 뒤 [8, 최대]로 자른다. 최대를 넘는 입력은 올림
    // 전에 잘라 넘침을 막는다(결과는 같다 — 최대가 2의 거듭제곱이다).
    const int32 wanted = resolution < MaxCubemapResolution ? resolution : MaxCubemapResolution;
    int32 sanitized = MinCubemapResolution;
    while (sanitized < wanted) {
        sanitized <<= 1;
    }

    return ChangeLightData(cubemapResolution_, sanitized, "cubemapResolution");
}

Result<void> SkyLightComponent::SetSkyDistanceThreshold(float distance) noexcept {
    if (!component_checks::NonNegative(distance)) {
        return component_checks::Invalid();
    }

    return ChangeLightData(skyDistanceThreshold_, distance, "skyDistanceThreshold");
}

Result<void> SkyLightComponent::SetCaptureEmissiveOnly(bool emissiveOnly) noexcept {
    return ChangeLightData(captureEmissiveOnly_, emissiveOnly, "captureEmissiveOnly");
}

Result<void> SkyLightComponent::SetLowerHemisphereIsBlack(bool black) noexcept {
    return ChangeLightData(lowerHemisphereIsBlack_, black, "lowerHemisphereIsBlack");
}

Result<void> SkyLightComponent::SetLowerHemisphereColor(const LinearColor& color) noexcept {
    if (!color.IsFinite()) {
        return component_checks::Invalid();
    }

    return ChangeLightData(lowerHemisphereColor_, color, "lowerHemisphereColor");
}

} // namespace rocks
