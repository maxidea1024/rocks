#include <Core/Math/Rotator.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Environment/DecalActor.generated.h>
#include <World/Environment/DecalActor.h>
#include <World/Environment/DecalComponent.generated.h>
#include <World/Environment/DecalComponent.h>
#include <World/SceneComponent.generated.h>
#include "../ActorRootSetup.h"

// 환경 Actor DecalActor다(Docs/Design/EngineRuntime 27.1절, D5).
namespace rocks {

Result<void> DecalActor::OnFrameworkPreAwake() noexcept {
    // UE ADecalActor(DecalActor.cpp:29–31): pitch −90°, destroyOwnerAfterFade 참(컴포넌트 기본).
    return actor_root_setup::PrepareRoot<DecalComponent>(*this, [](DecalComponent& decal) noexcept {
        return decal.SetRelativeRotation(
            Rotator<double>(DefaultPitchDegrees, 0.0, 0.0).ToQuaternion());
    });
}

} // namespace rocks
