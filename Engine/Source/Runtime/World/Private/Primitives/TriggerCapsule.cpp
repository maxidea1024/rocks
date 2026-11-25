#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Primitives/CapsuleComponent.generated.h>
#include <World/Primitives/CapsuleComponent.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/Primitives/ShapeComponent.generated.h>
#include <World/Primitives/TriggerBase.generated.h>
#include <World/Primitives/TriggerCapsule.generated.h>
#include <World/Primitives/TriggerCapsule.h>
#include "../ActorRootSetup.h"
#include <World/SceneComponent.generated.h>
#include <World/World.h>

// Trigger Actor TriggerCapsule다(Docs/Design/EngineRuntime 24.3.7절, D2).
namespace rocks {

Result<void> TriggerCapsule::OnFrameworkPreAwake() noexcept {
    // Trigger 색·profile·크기(UE TriggerActors.cpp:20–21·:28).
    return actor_root_setup::PrepareRoot<CapsuleComponent>(
        *this, [](CapsuleComponent& shape) noexcept {
            shape.SetShapeColor(TriggerShapeColor);
            if (auto profile =
                    shape.SetCollisionProfileName(StringView(TriggerCollisionProfileName));
                !profile) {
                return profile;
            }

            return shape.SetCapsuleSize(TriggerCapsuleRadius, TriggerCapsuleHalfHeight);
        });
}

} // namespace rocks
