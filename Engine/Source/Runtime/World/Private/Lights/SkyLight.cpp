#include <Core/Math/Rotator.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Lights/Light.generated.h>
#include <World/Lights/LightComponentBase.generated.h>
#include <World/Lights/SkyLight.generated.h>
#include <World/Lights/SkyLight.h>
#include <World/Lights/SkyLightComponent.generated.h>
#include <World/Lights/SkyLightComponent.h>
#include <World/ActorRootSetup.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>

// 광원 Actor SkyLight다(Docs/Design/EngineRuntime 24.2.5절, D1).
namespace rocks {

Result<void> SkyLight::OnFrameworkPreAwake() noexcept {
    // root는 Stationary SkyLightComponent다(UE ASkyLight — SkyLightComponent.cpp:1213).
    return actor_root_setup::PrepareRoot<SkyLightComponent>(
        *this, [](SkyLightComponent& light) noexcept {
            light.SetMobility(ComponentMobility::Stationary);
            return Result<void>::Success();
        });
}

} // namespace rocks
