#include <Collision/BodySettings.generated.h>
#include <World/Actor.generated.h>
#include <World/ActorRootSetup.h>
#include <World/Component.generated.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>
#include <WorldIntegration/Actors/StaticMeshActor.generated.h>
#include <WorldIntegration/Actors/StaticMeshActor.h>
#include <WorldIntegration/Components/MeshComponent.generated.h>
#include <WorldIntegration/Components/StaticMeshComponent.generated.h>
#include <WorldIntegration/Components/StaticMeshComponent.h>

// 정적 메시 Actor다(Docs/Design/EngineRuntime 30.4절, D2b).
namespace rocks {

Result<void> StaticMeshActor::OnFrameworkPreAwake() noexcept {
    // UE StaticMeshActor.cpp:32–36의 root 설정이다.
    return actor_root_setup::PrepareRoot<StaticMeshComponent>(
        *this, [](StaticMeshComponent& mesh) noexcept {
            if (auto profile =
                    mesh.SetCollisionProfileName(StringView(StaticMeshActorCollisionProfileName));
                !profile) {
                return profile;
            }

            mesh.SetMobility(ComponentMobility::Static);
            mesh.SetGenerateOverlapEvents(false);
            mesh.SetUseDefaultCollision(true);
            return Result<void>::Success();
        });
}

} // namespace rocks
