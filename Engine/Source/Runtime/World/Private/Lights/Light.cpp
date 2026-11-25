#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Lights/Light.generated.h>
#include <World/Lights/Light.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>

// 광원 Actor 기반이다(Docs/Design/EngineRuntime 24.2.5절, D1).
namespace rocks {

Result<void> Light::SetLightEnabled(bool enabled) noexcept {
    const ObjectHandle root = RootComponent();
    if (root.IsNull()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto changed = GetWorld().With<SceneComponent>(
        root, [enabled](SceneComponent& component) noexcept { component.SetVisibility(enabled); });
    return changed ? Result<void>::Success() : Result<void>::Failure(changed.Error());
}

bool Light::IsLightEnabled() const noexcept {
    const ObjectHandle root = RootComponent();
    if (root.IsNull()) {
        return false;
    }

    auto visible = GetWorld().With<SceneComponent>(
        root, [](SceneComponent& component) noexcept { return component.GetVisibleFlag(); });
    return visible && visible.Value();
}

} // namespace rocks
