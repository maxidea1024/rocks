#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Environment/HeightFogComponent.generated.h>
#include <World/Environment/HeightFogComponent.h>
#include <World/Environment/HeightFogSettings.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/ComponentValueChecks.h>

// 높이 안개 컴포넌트다(Docs/Design/EngineRuntime 27.3절, D5).
namespace rocks {

Result<void> HeightFogComponent::SetSettings(const HeightFogSettings& settings) noexcept {
    if (!IsValidHeightFogSettings(settings)) {
        return component_checks::Invalid();
    }

    if (!(settings_ == settings)) {
        settings_ = settings;
        MarkRenderStateDirtyIfPresent();
    }

    return Result<void>::Success();
}

bool HeightFogComponent::IsFogVisible() const noexcept {
    // UE AddFogIfNeeded(ExponentialHeightFogComponent.cpp:126).
    const float density = settings_.fogDensity + settings_.secondFogData.fogDensity;
    return density * DensityArtistScale > VisibilityEpsilon &&
           settings_.fogMaxOpacity > VisibilityEpsilon;
}

double HeightFogComponent::FogHeight() const noexcept {
    return WorldLocation().z;
}

double HeightFogComponent::SecondFogHeight() const noexcept {
    return FogHeight() + static_cast<double>(settings_.secondFogData.fogHeightOffset);
}

} // namespace rocks
