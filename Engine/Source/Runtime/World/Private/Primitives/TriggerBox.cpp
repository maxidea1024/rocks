#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Primitives/BoxComponent.generated.h>
#include <World/Primitives/BoxComponent.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/Primitives/ShapeComponent.generated.h>
#include <World/Primitives/TriggerBase.generated.h>
#include <World/Primitives/TriggerBox.generated.h>
#include <World/Primitives/TriggerBox.h>
#include <World/ActorRootSetup.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>

// Trigger Actor TriggerBox다(Docs/Design/EngineRuntime 24.3.7절, D2).
namespace rocks {

Result<void> TriggerBox::OnFrameworkPreAwake() noexcept {
    // Trigger 색·profile·크기(UE TriggerActors.cpp:20–21·:117).
    return actor_root_setup::PrepareRoot<BoxComponent>(*this, [](BoxComponent& shape) noexcept {
        shape.SetShapeColor(TriggerShapeColor);
        if (auto profile = shape.SetCollisionProfileName(StringView(TriggerCollisionProfileName));
            !profile) {
            return profile;
        }

        return shape.SetBoxExtent(
            Vec3d(TriggerBoxHalfExtent, TriggerBoxHalfExtent, TriggerBoxHalfExtent));
    });
}

} // namespace rocks
