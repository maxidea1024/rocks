#include <Core/Math/Rotator.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Lights/Light.generated.h>
#include <World/Lights/LightComponent.generated.h>
#include <World/Lights/LightComponentBase.generated.h>
#include <World/Lights/PointLight.generated.h>
#include <World/Lights/PointLight.h>
#include <World/Lights/PointLightComponent.generated.h>
#include <World/Lights/PointLightComponent.h>
#include <World/ActorRootSetup.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>

// 광원 Actor PointLight다(Docs/Design/EngineRuntime 24.2.5절, D1).
namespace rocks {

Result<void> PointLight::OnFrameworkPreAwake() noexcept {
    // root는 Stationary PointLightComponent다(UE APointLight — Light.cpp:190).
    return actor_root_setup::PrepareRoot<PointLightComponent>(
        *this, [](PointLightComponent& light) noexcept {
            light.SetMobility(ComponentMobility::Stationary);
            return Result<void>::Success();
        });
}

} // namespace rocks
