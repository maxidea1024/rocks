#pragma once

// Physics 모듈 body 시험의 공용 고정 장치다(PhysicsBodyTests·PhysicsFilterTests — World·컴포넌트
// 공개 API만 쓴다).
#include <Collision/CollisionSettings.generated.h>
#include <Collision/CollisionSettings.h>
#include <Config/Tests/ConfigTestSupport.h>
#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Physics/PhysicsWorldSubsystem.h>
#include <TestSupport/Test.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Physics/PhysicsScene.h>
#include <World/Primitives/BoxComponent.generated.h>
#include <World/Primitives/BoxComponent.h>
#include <World/Primitives/CapsuleComponent.generated.h>
#include <World/Primitives/CapsuleComponent.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/Primitives/ShapeComponent.generated.h>
#include <World/Primitives/SphereComponent.generated.h>
#include <World/Primitives/SphereComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>

namespace physics_test_support {

using namespace rocks;

constexpr double StepSeconds = 1.0 / 60.0;
constexpr int FallSteps = 30;
constexpr int SettleSteps = 180;
constexpr double GravityZ = -980.0; // cm/s²(PhysicsSettings 기본값)

constexpr double VelocityTolerance = 0.05; // cm/s
constexpr double PositionTolerance = 0.01; // cm
// 쌓여 정지한 형상의 높이 허용 오차: Jolt mPenetrationSlop 2 cm + 여유.
constexpr double RestHeightTolerance = 3.0; // cm

constexpr double FloorHalfExtent = 1000.0;
constexpr double FloorHalfThickness = 50.0;
constexpr double SphereRadius = 20.0;
constexpr double DropHeight = 200.0;
// 같은 World에 둔 형상끼리 서로 닿지 않게 벌리는 x 간격(cm)이다.
constexpr double SideOffset = 200.0;
// 고정 스텝 한 번에 떨어질 수 있는 거리보다 넉넉한 위치 허용 오차(cm — g·dt² ≈ 0.27 cm).
constexpr double OneStepFallTolerance = 1.0;

class PhysicsTestActor final : public Actor {
    ROCKS_NATIVE_TYPE(PhysicsTestActor, Actor, 0x4b8e2d6f91c03a57ull, 0x2f6c1e8a05b97d43ull)

public:
    explicit PhysicsTestActor(Allocator& allocator) noexcept : Actor(allocator) {
    }
};

// 물리 World 고정 장치다. collisionLayer가 없으면 설정 문맥이 없어 충돌 profile 표가 비고, 형상은
// 사용자 지정 값 QueryAndPhysics·WorldStatic으로 해석된다. collisionLayer는 엔진 층 JSON 텍스트다
// (`{ "Collision": { "profiles": [...] } }` — profile 표를 만든다).
struct BodyWorldFixture {
    explicit BodyWorldFixture(bool withPhysics = true, WorldKind kind = WorldKind::Game,
                              const WorldSubsystemType* after = nullptr,
                              const char* collisionLayer = nullptr) noexcept
        : types(allocator), modules(types, allocator) {
        registered = RegisterWorldTypes(modules).IsOk();
        if (collisionLayer != nullptr) {
            registered = registered &&
                         RegisterCollisionTypes(settingsFixture.registration).IsOk() &&
                         settingsFixture.Context().RegisterSettings<CollisionSettings>().IsOk() &&
                         settingsFixture.Context()
                             .AddLayerText(config::ConfigLayer::Engine, "Engine.json", "Engine",
                                           "<engine>", StringView(collisionLayer))
                             .IsOk() &&
                         settingsFixture.Context().Sync().IsOk();
        }

        auto actorToken = RegisterNativeClass<PhysicsTestActor>(types);
        registered = registered && actorToken.IsOk();
        if (actorToken) {
            token = actorToken.Value();
        }

        subsystems[0] = PhysicsWorldSubsystem::Type();
        usize subsystemCount = 1;
        if (after != nullptr) {
            subsystems[subsystemCount++] = *after;
        }

        WorldDesc desc;
        desc.name = "PhysicsBodies";
        desc.context = {0x50485953424f4459ull, 1};
        desc.timeStep = {StepSeconds, 0.25, 8};
        desc.kind = kind;
        desc.settings = collisionLayer != nullptr ? &settingsFixture.Context() : nullptr;
        if (withPhysics) {
            desc.subsystems = Span<const WorldSubsystemType>(subsystems, subsystemCount);
        }

        auto created = UniquePtr<World>::TryMake(allocator, allocator, types, desc);
        if (created) {
            world = Move(created.Value());
        }
    }

    ~BodyWorldFixture() {
        if (world) {
            (void)world->Shutdown();
        }

        world.Reset();
        if (registered) {
            CHECK(types.Unregister(token).IsOk());
        }

        CHECK(modules.Unregister().IsOk());
    }

    // 형상 컴포넌트 하나를 가진 Actor를 만든다. configure는 등록(body 생성) 전에 부른다.
    template <class T, class Configure>
    ObjectHandle SpawnShape(Configure&& configure) noexcept {
        return SpawnShapeOn<PhysicsTestActor, T>(static_cast<Configure&&>(configure));
    }

    // SpawnShape와 같지만 Actor 클래스를 고른다(ActorT는 시험이 등록한 native 클래스).
    template <class ActorT, class T, class Configure>
    ObjectHandle SpawnShapeOn(Configure&& configure) noexcept {
        auto actor = world->SpawnActorDeferred<ActorT>();
        if (!actor) {
            return {};
        }

        auto component = world->AddComponent<T>(actor.Value());
        if (!component) {
            return {};
        }

        (void)world->With<T>(component.Value(), [&](T& shape) noexcept {
            configure(shape);
            return Result<void>::Success();
        });
        if (!world->FinishSpawning(actor.Value())) {
            return {};
        }

        return component.Value();
    }

    // 부모(ParentT)와 그에 붙은 자식(ChildT)을 가진 Actor를 만든다. configure는 등록 전에 부른다.
    template <class ParentT, class ChildT, class ConfigureParent, class ConfigureChild>
    bool SpawnAttached(ObjectHandle& parentOut, ObjectHandle& childOut,
                       ConfigureParent&& configureParent,
                       ConfigureChild&& configureChild) noexcept {
        auto actor = world->SpawnActorDeferred<PhysicsTestActor>();
        if (!actor) {
            return false;
        }

        auto parent = world->AddComponent<ParentT>(actor.Value());
        auto child = world->AddComponent<ChildT>(actor.Value());
        if (!parent || !child) {
            return false;
        }

        (void)world->With<ParentT>(parent.Value(), [&](ParentT& shape) noexcept {
            configureParent(shape);
            return Result<void>::Success();
        });
        auto attached = world->With<ChildT>(child.Value(), [&](ChildT& shape) noexcept {
            configureChild(shape);
            return shape.AttachTo(parent.Value());
        });
        if (!attached || !world->FinishSpawning(actor.Value())) {
            return false;
        }

        parentOut = parent.Value();
        childOut = child.Value();
        return true;
    }

    ObjectHandle Parent(ObjectHandle component) noexcept {
        ObjectHandle parent;
        (void)world->With<SceneComponent>(component, [&](SceneComponent& scene) noexcept {
            parent = scene.Parent();
            return Result<void>::Success();
        });
        return parent;
    }

    // 윗면이 z = 0인 정적 바닥(BoxComponent)이다.
    ObjectHandle SpawnFloor() noexcept {
        return SpawnShape<BoxComponent>([](BoxComponent& floor) noexcept {
            floor.SetMobility(ComponentMobility::Static);
            (void)floor.SetBoxExtent(Vec3d(FloorHalfExtent, FloorHalfExtent, FloorHalfThickness));
            (void)floor.SetRelativeLocation(Vec3d(0.0, 0.0, -FloorHalfThickness));
        });
    }

    PhysicsBodyHandle Body(ObjectHandle component) noexcept {
        PhysicsBodyHandle handle;
        (void)world->With<PrimitiveComponent>(component,
                                              [&](PrimitiveComponent& primitive) noexcept {
                                                  handle = primitive.BodyHandle();
                                                  return Result<void>::Success();
                                              });
        return handle;
    }

    BodyKinematics Kinematics(ObjectHandle component) noexcept {
        auto read = world->GetPhysicsScene()->ReadKinematics(Body(component));
        CHECK(read);
        return read ? read.Value() : BodyKinematics{};
    }

    // 컴포넌트 월드 위치·회전이다.
    Transform ComponentPose(ObjectHandle component) noexcept {
        Transform pose;
        (void)world->With<PrimitiveComponent>(component,
                                              [&](PrimitiveComponent& primitive) noexcept {
                                                  pose.translation = primitive.WorldLocation();
                                                  pose.rotation = primitive.WorldQuaternion();
                                                  return Result<void>::Success();
                                              });
        return pose;
    }

    void Step(int count) noexcept {
        for (int index = 0; index < count; ++index) {
            CHECK(world->Tick(StepSeconds));
        }
    }

    uint32 BodyCount() noexcept {
        return world->GetPhysicsScene()->Stats().bodies;
    }

    // 설정 문맥은 World보다 오래 산다(먼저 선언).
    test::config::ConfigFixture settingsFixture;
    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    TypeRegistrationToken token;
    bool registered = false;
    WorldSubsystemType subsystems[2];
    UniquePtr<World> world;
};

inline bool Near(double value, double expected, double tolerance) noexcept {
    return math::Abs(value - expected) <= tolerance;
}

// 컴포넌트 하나에 setter를 부른다(결과는 시험이 본다).
template <class T, class Call>
bool Apply(World& world, ObjectHandle component, Call&& call) noexcept {
    return world
        .With<T>(component,
                 [&](T& value) noexcept {
                     call(value);
                     return Result<void>::Success();
                 })
        .IsOk();
}

// UE 기본 선형 감쇠(BodySettings DefaultLinearDamping 0.01/s)를 넣은 Jolt 적분의 n 스텝 뒤 낙하
// 속도다 (MotionProperties.inl:133-143 — 중력을 더한 뒤 v·(1 − c·dt)). Jolt 기본 감쇠 0.05였다면
// 다르다.
inline double DampedFallVelocity(int steps) noexcept {
    const double keep = 1.0 - static_cast<double>(DefaultLinearDamping) * StepSeconds;
    double velocity = 0.0;
    for (int step = 0; step < steps; ++step) {
        velocity = (velocity + GravityZ * StepSeconds) * keep;
    }

    return velocity;
}

} // namespace physics_test_support
