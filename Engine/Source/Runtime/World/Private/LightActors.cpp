#include <Core/Math/Rotator.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/DirectionalLightComponent.generated.h>
#include <World/DirectionalLightComponent.h>
#include <World/LightActors.generated.h>
#include <World/LightActors.h>
#include <World/LightComponent.generated.h>
#include <World/PointLightComponent.generated.h>
#include <World/PointLightComponent.h>
#include <World/RectLightComponent.generated.h>
#include <World/RectLightComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/SkyLightComponent.generated.h>
#include <World/SkyLightComponent.h>
#include <World/World.h>

// 광원 Actor다(Docs/Design/EngineRuntime 24.2.5절, D1).
namespace rocks {

namespace {

// root 광원 컴포넌트를 준비한다. 레벨 로드로 T 타입(파생 포함) root가 이미 있으면 그대로 쓴다.
// 다른 타입 root가 있으면 InvalidArgument다(옛 root를 떼어 둔 채 바꾸지 않는다 — UE 광원 Actor는
// root를 생성자에서 정해 이 상태가 없다). root가 없으면 만들어 Stationary(UE 광원 Actor 생성자)로
// 두고, pitch가 있으면 상대 회전을 넣은 뒤 root로 둔다.
template <typename T>
Result<void> PrepareLightRoot(Actor& actor, const double* pitchDegrees) noexcept {
    World& world = actor.GetWorld();
    if (const ObjectHandle root = actor.RootComponent(); !root.IsNull()) {
        if (world.With<T>(root, [](T&) noexcept { return true; })) {
            return Result<void>::Success();
        }

        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto created = world.AddComponent<T>(actor.Handle());
    if (!created) {
        return Result<void>::Failure(created.Error());
    }

    auto configured = world.With<T>(created.Value(), [pitchDegrees](T& light) noexcept {
        light.SetMobility(ComponentMobility::Stationary);
        if (pitchDegrees == nullptr) {
            return Result<void>::Success();
        }

        return light.SetRelativeRotation(Rotator<double>(*pitchDegrees, 0.0, 0.0).ToQuaternion());
    });
    if (!configured) {
        return configured;
    }

    return actor.SetRootComponent(created.Value());
}

} // namespace

// --- Light ---

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

// --- 파생 ---

Result<void> PointLight::OnFrameworkPreAwake() noexcept {
    return PrepareLightRoot<PointLightComponent>(*this, nullptr);
}

Result<void> SpotLight::OnFrameworkPreAwake() noexcept {
    const double pitch = DefaultPitchDegrees;
    return PrepareLightRoot<SpotLightComponent>(*this, &pitch);
}

Result<void> RectLight::OnFrameworkPreAwake() noexcept {
    return PrepareLightRoot<RectLightComponent>(*this, nullptr);
}

Result<void> DirectionalLight::OnFrameworkPreAwake() noexcept {
    const double pitch = DefaultPitchDegrees;
    return PrepareLightRoot<DirectionalLightComponent>(*this, &pitch);
}

Result<void> SkyLight::OnFrameworkPreAwake() noexcept {
    return PrepareLightRoot<SkyLightComponent>(*this, nullptr);
}

} // namespace rocks
