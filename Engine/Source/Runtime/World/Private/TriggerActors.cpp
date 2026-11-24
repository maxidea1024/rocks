#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/LightingChannels.generated.h>
#include <World/PrimitiveComponent.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/ShapeComponent.generated.h>
#include <World/ShapeComponent.h>
#include <World/TriggerActors.generated.h>
#include <World/TriggerActors.h>
#include <World/World.h>

// Trigger Actor다(Docs/Design/EngineRuntime 24.3.7절, D2).
namespace rocks {

namespace {

// root shape를 준비한다. 같은 타입(파생 포함) root는 그대로 쓰고, 다른 타입 root는
// InvalidArgument다. 없으면 T를 만들어 configure(크기)·Trigger 색·profile을 넣고 root로 둔다.
template <typename T, typename Configure>
Result<void> PrepareTriggerRoot(Actor& actor, Configure&& configure) noexcept {
    World& world = actor.GetWorld();
    if (const ObjectHandle root = actor.RootComponent(); !root.IsNull()) {
        if (world.With<T>(root, [](T&) noexcept { return true; })) {
            return Result<void>::Success();
        }

        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto created = world.AddComponent<T>(actor.Handle());
    if (!created) {
        return Result<void>::Failure(created.Error());
    }

    auto configured = world.With<T>(created.Value(), [&configure](T& shape) noexcept {
        shape.SetShapeColor(TriggerBase::TriggerShapeColor);
        if (auto profile =
                shape.SetCollisionProfileName(StringView(TriggerBase::TriggerCollisionProfileName));
            !profile) {
            return profile;
        }

        return configure(shape);
    });
    if (!configured) {
        return configured;
    }

    return actor.SetRootComponent(created.Value());
}

} // namespace

Result<void> TriggerBox::OnFrameworkPreAwake() noexcept {
    return PrepareTriggerRoot<BoxComponent>(*this, [](BoxComponent& box) noexcept {
        return box.SetBoxExtent(
            Vec3d(TriggerBoxHalfExtent, TriggerBoxHalfExtent, TriggerBoxHalfExtent));
    });
}

Result<void> TriggerSphere::OnFrameworkPreAwake() noexcept {
    return PrepareTriggerRoot<SphereComponent>(*this, [](SphereComponent& sphere) noexcept {
        return sphere.SetSphereRadius(TriggerSphereRadius);
    });
}

Result<void> TriggerCapsule::OnFrameworkPreAwake() noexcept {
    return PrepareTriggerRoot<CapsuleComponent>(*this, [](CapsuleComponent& capsule) noexcept {
        return capsule.SetCapsuleSize(TriggerCapsuleRadius, TriggerCapsuleHalfHeight);
    });
}

} // namespace rocks
