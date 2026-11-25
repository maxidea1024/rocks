#include <Core/Math/Rotator.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Lights/Light.generated.h>
#include <World/Lights/LightComponent.generated.h>
#include <World/Lights/LightComponentBase.generated.h>
#include <World/Lights/SpotLight.generated.h>
#include <World/Lights/SpotLight.h>
#include <World/Lights/SpotLightComponent.generated.h>
#include <World/Lights/SpotLightComponent.h>
#include "../ActorRootSetup.h"
#include <World/SceneComponent.generated.h>
#include <World/World.h>

// 광원 Actor SpotLight다(Docs/Design/EngineRuntime 24.2.5절, D1).
namespace rocks {

Result<void> SpotLight::OnFrameworkPreAwake() noexcept {
    // root는 Stationary SpotLightComponent다(UE ASpotLight — SpotLight.cpp:27–28).
    return actor_root_setup::PrepareRoot<SpotLightComponent>(
        *this, [](SpotLightComponent& light) noexcept {
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
