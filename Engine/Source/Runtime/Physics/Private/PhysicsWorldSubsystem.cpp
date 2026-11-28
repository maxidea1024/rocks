// World 물리 장면 Subsystem(Docs/Design/PhysicsModule 2.1절, 7.1.1·7.1.4절).
#include "JoltPhysicsScene.h"

#include <Config/ConfigContext.h>
#include <Core/Math/MathUtility.h>
#include <Physics/PhysicsSettings.generated.h>
#include <Physics/PhysicsWorldSubsystem.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/Primitives/PrimitiveComponent.h>
#include <World/SceneComponent.generated.h>

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

// 엔진 자리 PhysicsSimulate 콜백이다(고정 스텝마다 — 문맥의 delta가 고정 스텝 길이다, 3.1절).
Result<void> SimulateFixedStep(World&, const TickContext& context, void* user) noexcept {
    return static_cast<PhysicsWorldSubsystem*>(user)->SimulateStep(context.deltaSeconds);
}

// 엔진 자리 PhysicsResults 콜백이다(Physics 단계 끝, 프레임에 한 번 — 3.1·3.4절).
Result<void> ApplyFrameResults(World& world, const TickContext&, void* user) noexcept {
    return static_cast<PhysicsWorldSubsystem*>(user)->ApplyResults(world);
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
    JoltScene(*scene_).SetSimulates(simulates_);
    // step·결과 반영은 Game·Tryout World만 건다(World는 종류로 거르지 않는다 — 3.1절 PMD16).
    if (simulates_) {
        auto staged = world.SetEngineStage(EngineStage::PhysicsSimulate, &SimulateFixedStep, this);
        if (staged) {
            staged = world.SetEngineStage(EngineStage::PhysicsResults, &ApplyFrameResults, this);
            if (!staged) {
                world.ClearEngineStage(EngineStage::PhysicsSimulate);
            }
        }

        if (!staged) {
            (void)world.SetPhysicsScene(nullptr);
            scene_.Reset();
            simulates_ = false;
            return staged;
        }
    }

    return Result<void>::Success();
}

void PhysicsWorldSubsystem::Deinitialize(World& world) noexcept {
    if (simulates_) {
        world.ClearEngineStage(EngineStage::PhysicsSimulate);
        world.ClearEngineStage(EngineStage::PhysicsResults);
    }

    if (scene_) {
        // 자기 장면이 꽂혀 있을 때만 뗀다.
        if (world.GetPhysicsScene() == scene_.Get()) {
            (void)world.SetPhysicsScene(nullptr);
        }

        scene_.Reset();
    }

    simulates_ = false;
}

Result<void> PhysicsWorldSubsystem::ApplyResults(World& world) noexcept {
    if (!scene_) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    physics::JoltPhysicsScene& scene = JoltScene(*scene_);
    // 이번 프레임의 고정 스텝이 끝났다(프레임 힘 — 7.1.17절).
    scene.EndFrame();
    auto moved = scene.TakeMovedBodies();
    ErrorCode failure = moved ? ErrorCode::None : moved.Error();
    const Span<const physics::MovedBody> bodies =
        moved ? moved.Value() : Span<const physics::MovedBody>();
    for (const physics::MovedBody& body : bodies) {
        auto applied = world.With<PrimitiveComponent>(
            body.component, [&](PrimitiveComponent& primitive) noexcept {
                return primitive.ApplyPhysicsTransform(body.location, body.rotation);
            });
        // 반영 중 다른 컴포넌트가 파괴될 수 있다 — 사라진 handle은 건너뛴다(3.4절 재진입). 이동
        // 실패만 첫 오류로 남긴다.
        const bool gone = !applied && (applied.Error() == ErrorCode::StaleHandle ||
                                       applied.Error() == ErrorCode::NotFound);
        if (!applied && !gone && failure == ErrorCode::None) {
            failure = applied.Error();
        }
    }

    // 자세 반영 뒤 깨움·잠듦 이벤트를 칸 번호 순으로 발행한다(UE OnSyncBodies — 이동 뒤
    // DispatchWakeEvents, 7.1.19절). 꺼낸 목록은 hook이 장면을 바꿔도 그대로다.
    for (const physics::WakeEvent& event : scene.TakeWakeEvents()) {
        (void)world.With<PrimitiveComponent>(event.component,
                                             [&](PrimitiveComponent& primitive) noexcept {
                                                 primitive.DispatchWakeEvents(event.awake);
                                             });
    }

    // 결과 반영 뒤 시뮬레이션 접촉 hit를 스텝 순서대로 발행한다(3.1절 (2), 7.1.12절). 결과 반영이
    // 실패해도 발행한다(쌓인 hit는 다음 프레임으로 미루지 않는다). 꺼낸 목록은 hook이 장면을 바꿔도
    // 그대로다.
    for (const PhysicsContactHit& hit : scene.TakeContactHits()) {
        PrimitiveComponent::DispatchPhysicsHit(world, hit);
    }

    return failure == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(failure);
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
