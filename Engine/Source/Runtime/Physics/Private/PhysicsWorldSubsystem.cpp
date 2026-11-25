// World 물리 장면 Subsystem(Docs/Design/PhysicsModule 2.1절, 7.1.1절).
#include "JoltPhysicsScene.h"

#include <Config/ConfigContext.h>
#include <Core/Math/MathUtility.h>
#include <Physics/PhysicsSettings.generated.h>
#include <Physics/PhysicsWorldSubsystem.h>

ROCKS_DEFINE_NATIVE_TYPE_KEY(rocks::PhysicsWorldSubsystem)

namespace rocks {

namespace {

// 장면을 만드는 World 종류다(Inactive 제외 — 2.1절).
bool WantsPhysicsScene(const void* owner) noexcept {
    return static_cast<const World*>(owner)->Kind() != WorldKind::Inactive;
}

// step을 도는 World 종류다(Game·Tryout — 3.1절 PMD16).
bool SimulatesKind(WorldKind kind) noexcept {
    return kind == WorldKind::Game || kind == WorldKind::Tryout;
}

physics::JoltPhysicsScene& JoltScene(PhysicsScene& scene) noexcept {
    return static_cast<physics::JoltPhysicsScene&>(scene);
}

} // namespace

PhysicsWorldSubsystem::PhysicsWorldSubsystem(Allocator& allocator) noexcept
    : allocator_(allocator) {
}

PhysicsWorldSubsystem::~PhysicsWorldSubsystem() = default;

WorldSubsystemType PhysicsWorldSubsystem::Type() noexcept {
    WorldSubsystemType type;
    type.key = SubsystemKeyOf<PhysicsWorldSubsystem>();
    type.name = "Physics";
    type.create = [](Allocator& allocator) noexcept -> Result<UniquePtr<WorldSubsystem>> {
        auto created = UniquePtr<PhysicsWorldSubsystem>::TryMake(allocator, allocator);
        if (!created) {
            return Result<UniquePtr<WorldSubsystem>>::Failure(created.Error());
        }

        return Result<UniquePtr<WorldSubsystem>>::Success(
            UniquePtr<WorldSubsystem>(Move(created.Value())));
    };
    type.shouldCreate = &WantsPhysicsScene;
    return type;
}

Result<void> PhysicsWorldSubsystem::Initialize(World& world) noexcept {
    // 설정 문맥에 등록된 물리 설정을 쓰고, 없으면 기본값이다(CollisionSettings와 같은 방식).
    PhysicsSettings defaults;
    const PhysicsSettings* settings = &defaults;
    if (config::ConfigContext* context = world.Settings(); context != nullptr) {
        if (const void* registered = context->FindSettings(GeneratedType<PhysicsSettings>::Id());
            registered != nullptr) {
            settings = static_cast<const PhysicsSettings*>(registered);
        }
    }

    auto created = physics::JoltPhysicsScene::Create(allocator_, *settings, world.GcScheduler());
    if (!created) {
        return Result<void>::Failure(created.Error());
    }

    if (auto plugged = world.SetPhysicsScene(created.Value().Get()); !plugged) {
        return plugged;
    }

    scene_ = UniquePtr<PhysicsScene>(Move(created.Value()));
    simulates_ = SimulatesKind(world.Kind());
    return Result<void>::Success();
}

void PhysicsWorldSubsystem::Deinitialize(World& world) noexcept {
    if (scene_) {
        // 자기 장면이 꽂혀 있을 때만 뗀다.
        if (world.GetPhysicsScene() == scene_.Get()) {
            (void)world.SetPhysicsScene(nullptr);
        }

        scene_.Reset();
    }

    simulates_ = false;
}

Result<void> PhysicsWorldSubsystem::SimulateStep(double seconds) noexcept {
    if (!scene_) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    if (!simulates_) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    if (!(seconds > 0.0) || !math::IsFinite(seconds)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return JoltScene(*scene_).Step(static_cast<float>(seconds));
}

} // namespace rocks
