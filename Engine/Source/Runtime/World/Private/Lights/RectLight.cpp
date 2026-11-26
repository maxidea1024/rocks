#include <Core/Math/Rotator.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Lights/Light.generated.h>
#include <World/Lights/LightComponent.generated.h>
#include <World/Lights/LightComponentBase.generated.h>
#include <World/Lights/RectLight.generated.h>
#include <World/Lights/RectLight.h>
#include <World/Lights/RectLightComponent.generated.h>
#include <World/Lights/RectLightComponent.h>
#include <World/ActorRootSetup.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>

// 광원 Actor RectLight다(Docs/Design/EngineRuntime 24.2.5절, D1).
namespace rocks {

Result<void> RectLight::OnFrameworkPreAwake() noexcept {
    // root는 Stationary RectLightComponent다(UE ARectLight — RectLight.cpp:12).
    return actor_root_setup::PrepareRoot<RectLightComponent>(
        *this, [](RectLightComponent& light) noexcept {
            light.SetMobility(ComponentMobility::Stationary);
            return Result<void>::Success();
        });
}

} // namespace rocks
