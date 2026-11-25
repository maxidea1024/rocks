#include <Core/Math/Rotator.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Lights/DirectionalLight.generated.h>
#include <World/Lights/DirectionalLight.h>
#include <World/Lights/DirectionalLightComponent.generated.h>
#include <World/Lights/DirectionalLightComponent.h>
#include <World/Lights/Light.generated.h>
#include <World/Lights/LightComponent.generated.h>
#include <World/Lights/LightComponentBase.generated.h>
#include "../ActorRootSetup.h"
#include <World/SceneComponent.generated.h>
#include <World/World.h>

// 광원 Actor DirectionalLight다(Docs/Design/EngineRuntime 24.2.5절, D1).
namespace rocks {

Result<void> DirectionalLight::OnFrameworkPreAwake() noexcept {
    // root는 Stationary DirectionalLightComponent다(UE ADirectionalLight — Light.cpp:244–245).
    return actor_root_setup::PrepareRoot<DirectionalLightComponent>(
        *this, [](DirectionalLightComponent& light) noexcept {
            light.SetMobility(ComponentMobility::Stationary);
            if (auto rotated = light.SetRelativeRotation(
                    Rotator<double>(DefaultPitchDegrees, 0.0, 0.0).ToQuaternion());
                !rotated) {
                return rotated;
            }

            return Result<void>::Success();
        });
}

} // namespace rocks
