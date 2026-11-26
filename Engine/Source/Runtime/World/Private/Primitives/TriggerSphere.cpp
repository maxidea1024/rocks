#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/Primitives/ShapeComponent.generated.h>
#include <World/Primitives/SphereComponent.generated.h>
#include <World/Primitives/SphereComponent.h>
#include <World/Primitives/TriggerBase.generated.h>
#include <World/Primitives/TriggerSphere.generated.h>
#include <World/Primitives/TriggerSphere.h>
#include <World/ActorRootSetup.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>

// Trigger Actor TriggerSphere다(Docs/Design/EngineRuntime 24.3.7절, D2).
namespace rocks {

Result<void> TriggerSphere::OnFrameworkPreAwake() noexcept {
    // Trigger 색·profile·크기(UE TriggerActors.cpp:20–21·:179).
    return actor_root_setup::PrepareRoot<SphereComponent>(
        *this, [](SphereComponent& shape) noexcept {
            shape.SetShapeColor(TriggerShapeColor);
            if (auto profile =
                    shape.SetCollisionProfileName(StringView(TriggerCollisionProfileName));
                !profile) {
                return profile;
            }

            return shape.SetSphereRadius(TriggerSphereRadius);
        });
}

} // namespace rocks
