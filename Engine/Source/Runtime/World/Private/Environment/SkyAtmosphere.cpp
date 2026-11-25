#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Environment/SkyAtmosphere.generated.h>
#include <World/Environment/SkyAtmosphere.h>
#include <World/Environment/SkyAtmosphereComponent.generated.h>
#include <World/Environment/SkyAtmosphereComponent.h>
#include <World/Environment/SkyAtmosphereSettings.generated.h>
#include <World/SceneComponent.generated.h>
#include "../ActorRootSetup.h"

// 환경 Actor SkyAtmosphere다(Docs/Design/EngineRuntime 27.1절, D5).
namespace rocks {

Result<void> SkyAtmosphere::OnFrameworkPreAwake() noexcept {
    // root는 SkyAtmosphereComponent다(UE ASkyAtmosphere — SkyAtmosphereComponent.cpp).
    return actor_root_setup::PrepareRoot<SkyAtmosphereComponent>(
        *this, [](SkyAtmosphereComponent&) noexcept { return Result<void>::Success(); });
}

} // namespace rocks
