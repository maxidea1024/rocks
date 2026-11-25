#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Environment/SkyAtmosphereComponent.generated.h>
#include <World/Environment/SkyAtmosphereComponent.h>
#include <World/Environment/SkyAtmosphereSettings.generated.h>
#include <World/SceneComponent.generated.h>
#include "../ComponentValueChecks.h"

// 대기 컴포넌트다(Docs/Design/EngineRuntime 27.2절, D5).
namespace rocks {

Result<void> SkyAtmosphereComponent::SetSettings(const SkyAtmosphereSettings& settings) noexcept {
    if (!IsValidSkyAtmosphereSettings(settings)) {
        return component_checks::Invalid();
    }

    // UE setter는 AreDynamicDataChangesAllowed를 본다(SkyAtmosphereComponent.cpp:464).
    if (auto allowed = CheckDynamicDataChange("settings", false); !allowed) {
        return allowed;
    }

    if (!(settings_ == settings)) {
        settings_ = settings;
        MarkRenderStateDirtyIfPresent();
    }

    return Result<void>::Success();
}

Vec3d SkyAtmosphereComponent::PlanetCenterKm() const noexcept {
    // UE SkyAtmosphereCommonData.cpp:166–175.
    const Vec3d component = WorldLocation() * KilometersPerCentimeter;
    const Vec3d belowTop(0.0, 0.0, -static_cast<double>(settings_.bottomRadius));
    switch (settings_.transformMode) {
    case SkyAtmosphereTransformMode::PlanetTopAtComponentTransform:
        return belowTop + component;
    case SkyAtmosphereTransformMode::PlanetCenterAtComponentTransform:
        return component;
    case SkyAtmosphereTransformMode::PlanetTopAtAbsoluteWorldOrigin:
    default:
        return belowTop;
    }
}

} // namespace rocks
