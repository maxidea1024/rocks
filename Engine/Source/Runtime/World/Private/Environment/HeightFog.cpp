#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Environment/HeightFog.generated.h>
#include <World/Environment/HeightFog.h>
#include <World/Environment/HeightFogComponent.generated.h>
#include <World/Environment/HeightFogComponent.h>
#include <World/Environment/HeightFogSettings.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>
#include <World/ActorRootSetup.h>

// 환경 Actor HeightFog다(Docs/Design/EngineRuntime 27.1절, D5).
namespace rocks {

Result<void> HeightFog::OnFrameworkPreAwake() noexcept {
    // root는 HeightFogComponent다(UE AExponentialHeightFog).
    return actor_root_setup::PrepareRoot<HeightFogComponent>(
        *this, [](HeightFogComponent&) noexcept { return Result<void>::Success(); });
}

Result<void> HeightFog::SetFogEnabled(bool enabled) noexcept {
    const ObjectHandle root = RootComponent();
    if (root.IsNull()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto changed = GetWorld().With<SceneComponent>(
        root, [enabled](SceneComponent& component) noexcept { component.SetVisibility(enabled); });
    return changed ? Result<void>::Success() : Result<void>::Failure(changed.Error());
}

bool HeightFog::IsFogEnabled() const noexcept {
    const ObjectHandle root = RootComponent();
    if (root.IsNull()) {
        return false;
    }

    auto visible = GetWorld().With<SceneComponent>(
        root, [](SceneComponent& component) noexcept { return component.GetVisibleFlag(); });
    return visible && visible.Value();
}

} // namespace rocks
