#include <Base/InitializerList.h>
#include <Core/Math/Rotator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <TestSupport/Test.h>
#include <World/Actor.generated.h>
#include <World/Camera/CameraComponent.generated.h>
#include <World/Camera/CameraComponent.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/SceneComponent.h>
#include <World/World.h>
#include <math.h>
#include <stdio.h>

// 컴포넌트 기반 보강 CM0b·CM0c 시험이다(Docs/Design/EngineRuntime 23.3·23.4절): 부착 규칙·절대
// Transform·socket·Mobility·가시성·bounds·Transform 통지, CameraComponent.
using namespace rocks;
namespace world_scene_foundation_tests_file {

namespace {

constexpr double Tolerance = 1.0e-6;

bool Near(double left, double right) noexcept {
    return fabs(left - right) <= Tolerance;
}

bool NearVector(const Vec3d& value, double x, double y, double z) noexcept {
    const bool same = Near(value.x, x) && Near(value.y, y) && Near(value.z, z);
    if (!same) {
        printf("  vector (%g, %g, %g), expected (%g, %g, %g)\n", value.x, value.y, value.z, x, y,
               z);
    }

    return same;
}

class EmptyActor final : public Actor {
    ROCKS_NATIVE_TYPE(EmptyActor, Actor, 0x3d8f0a6c2e4b1957ull, 0xa7e2c4b9d01f3658ull)

public:
    explicit EmptyActor(Allocator& allocator) noexcept : Actor(allocator) {
    }
};

// socket "Muzzle"(위로 50 cm)을 가진 SceneComponent다.
class SocketScene final : public SceneComponent {
    ROCKS_NATIVE_TYPE(SocketScene, SceneComponent, 0x8c41e7a2f05d3b96ull, 0x1f6b9d3e7a0c2845ull)

public:
    explicit SocketScene(Allocator& allocator) noexcept : SceneComponent(allocator) {
    }

    bool HasSocket(StringView socket) const noexcept override {
        return socket == StringView("Muzzle");
    }

    Result<Transform> GetSocketTransform(StringView socket,
                                         TransformSpace space) const noexcept override {
        if (!HasSocket(socket)) {
            return SceneComponent::GetSocketTransform(socket, space);
        }

        const Transform local(Quatd::Identity(), Vec3d(0.0, 0.0, 50.0), Vec3d::One());
        if (space == TransformSpace::Component) {
            return Result<Transform>::Success(local);
        }

        auto world = TryWorldTransform();
        if (!world) {
            return world;
        }

        return Transform::TryMultiply(local, world.Value());
    }
};

// bounds 계산·Transform 통지 횟수를 세는 SceneComponent다.
class CountingScene final : public SceneComponent {
    ROCKS_NATIVE_TYPE(CountingScene, SceneComponent, 0x5a2e9c71b3d04f68ull, 0xc8f13a6e9b2d0574ull)

public:
    explicit CountingScene(Allocator& allocator) noexcept : SceneComponent(allocator) {
    }

    ComponentBounds CalcBounds(const Transform& world) const noexcept override {
        ++calcCount;
        ComponentBounds bounds;
        bounds.origin = world.translation;
        bounds.boxExtent = Vec3d(10.0, 10.0, 10.0);
        bounds.sphereRadius = 17.320508075688775;
        return bounds;
    }

    mutable uint32 calcCount = 0;
    uint32 updateCount = 0;
    uint32 visibilityCount = 0;

protected:
    void OnTransformUpdated() noexcept override {
        ++updateCount;
    }

    void OnVisibilityChanged() noexcept override {
        ++visibilityCount;
    }
};

// OnAwake에서 Static root를 만들고 그 안에서 옮기는 Actor다(초기화 전 이동은 된다).
class StaticRootActor final : public Actor {
    ROCKS_NATIVE_TYPE(StaticRootActor, Actor, 0x9e07b4c2d6a1f385ull, 0x4d3c8a1f7e2b5096ull)

public:
    explicit StaticRootActor(Allocator& allocator) noexcept : Actor(allocator) {
    }

    Result<void> OnAwake() noexcept override {
        auto root = GetWorld().AddComponent<SceneComponent>(Handle());
        if (!root) {
            return Result<void>::Failure(root.Error());
        }

        auto moved =
            GetWorld().With<SceneComponent>(root.Value(), [](SceneComponent& scene) noexcept {
                scene.SetMobility(ComponentMobility::Static);
                return scene.SetRelativeLocation(Vec3d(5.0, 0.0, 0.0));
            });
        if (!moved) {
            return moved;
        }

        return SetRootComponent(root.Value());
    }
};

struct Fixture {
    explicit Fixture(WorldKind kind = WorldKind::Game) noexcept
        : types(allocator), modules(types, allocator) {
        registered = RegisterWorldTypes(modules).IsOk() && Register<EmptyActor>() &&
                     Register<SocketScene>() && Register<CountingScene>() &&
                     Register<StaticRootActor>();
        WorldDesc desc;
        desc.name = "Test";
        desc.context = {0x434d3062ull, 1};
        desc.timeStep = {1.0 / 60.0, 0.25, 8};
        desc.kind = kind;
        auto created = UniquePtr<World>::TryMake(allocator, allocator, types, desc);
        if (created) {
            world = Move(created.Value());
        }
    }

    ~Fixture() {
        world.Reset();
        while (tokenCount != 0) {
            CHECK(types.Unregister(tokens[--tokenCount]).IsOk());
        }

        CHECK(modules.Unregister().IsOk());
    }

    template <class T>
    bool Register() noexcept {
        auto token = RegisterNativeClass<T>(types);
        if (!token || tokenCount == MaxTokens) {
            return false;
        }

        tokens[tokenCount++] = token.Value();
        return true;
    }

    template <class T = SceneComponent>
    ObjectHandle AddScene(ObjectHandle actor, const Transform& relative) noexcept {
        auto scene = world->AddComponent<T>(actor);
        if (!scene) {
            return {};
        }

        auto set = world->With<SceneComponent>(scene.Value(), [&](SceneComponent& value) noexcept {
            return value.SetRelativeTransform(relative);
        });
        return set ? scene.Value() : ObjectHandle{};
    }

    Transform WorldOf(ObjectHandle scene) noexcept {
        auto transform = world->With<SceneComponent>(
            scene, [](SceneComponent& value) noexcept { return value.TryWorldTransform(); });
        return transform ? transform.Value() : Transform::Identity();
    }

    static constexpr uint32 MaxTokens = 6;
    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    TypeRegistrationToken tokens[MaxTokens];
    uint32 tokenCount = 0;
    bool registered = false;
    UniquePtr<World> world;
};

// 부모: (100, 0, 0), yaw 90, scale (2, 1, 1). yaw 90은 +X → +Y, +Y → −X다(UE 왼손 yaw).
Transform ParentTransform() noexcept {
    return Transform(Rotator<double>(0.0, 90.0, 0.0).ToQuaternion(), Vec3d(100.0, 0.0, 0.0),
                     Vec3d(2.0, 1.0, 1.0));
}

} // namespace

// 23.3절 ERD29: 부착 규칙 셋(KeepRelative·KeepWorld·Snap 두 가지) × 비균일 scale·회전 부모.
TEST(SceneFoundation_AttachmentRulesWithNonUniformParent) {
    struct Case {
        AttachmentRules rules;
        double x, y, z; // 기대 월드 위치
        double scaleX;  // 기대 월드 scale x
        bool rotatedLikeParent;
    };
    const Case cases[] = {
        // 상대 (0, 50, 0)이 부모 아래로: scale (0, 50, 0) → yaw 90 (−50, 0, 0) → + (100, 0, 0).
        {AttachmentRules::KeepRelativeTransform(), 50.0, 0.0, 0.0, 2.0, true},
        {AttachmentRules::KeepWorldTransform(), 0.0, 50.0, 0.0, 1.0, false},
        {AttachmentRules::SnapToTargetIncludingScale(), 100.0, 0.0, 0.0, 2.0, true},
        {AttachmentRules::SnapToTargetNotIncludingScale(), 100.0, 0.0, 0.0, 1.0, true},
    };
    for (const Case& item : cases) {
        Fixture fixture;
        REQUIRE(fixture.registered);
        World& world = *fixture.world;
        REQUIRE(world.Initialize());
        auto actor = world.SpawnActor<EmptyActor>();
        REQUIRE(actor);
        const ObjectHandle parent = fixture.AddScene(actor.Value(), ParentTransform());
        const ObjectHandle child = fixture.AddScene(
            actor.Value(), Transform(Quatd::Identity(), Vec3d(0.0, 50.0, 0.0), Vec3d::One()));
        REQUIRE(!parent.IsNull() && !child.IsNull());
        REQUIRE(world.With<SceneComponent>(child, [&](SceneComponent& value) noexcept {
            return value.AttachTo(parent, item.rules);
        }));
        const Transform result = fixture.WorldOf(child);
        CHECK(NearVector(result.translation, item.x, item.y, item.z));
        if (!Near(result.scale.x, item.scaleX)) {
            printf("  rules %d/%d/%d scale (%g, %g, %g)\n", static_cast<int>(item.rules.location),
                   static_cast<int>(item.rules.rotation), static_cast<int>(item.rules.scale),
                   result.scale.x, result.scale.y, result.scale.z);
        }

        CHECK(Near(result.scale.x, item.scaleX));
        const Vec3d forward = result.rotation.RotateVector(Vec3d(1.0, 0.0, 0.0));
        CHECK(item.rotatedLikeParent ? NearVector(forward, 0.0, 1.0, 0.0)
                                     : NearVector(forward, 1.0, 0.0, 0.0));
        REQUIRE(world.Shutdown());
    }
}

// 23.3절: KeepWorld로 떼면 월드 Transform이 그대로다. 절대 위치는 부모를 무시한다.
TEST(SceneFoundation_DetachKeepWorldAndAbsoluteLocation) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto actor = world.SpawnActor<EmptyActor>();
    REQUIRE(actor);
    const ObjectHandle parent = fixture.AddScene(actor.Value(), ParentTransform());
    const ObjectHandle child = fixture.AddScene(
        actor.Value(), Transform(Quatd::Identity(), Vec3d(10.0, 0.0, 0.0), Vec3d::One()));
    REQUIRE(world.With<SceneComponent>(
        child, [&](SceneComponent& value) noexcept { return value.AttachTo(parent); }));
    // 상대 (10, 0, 0): scale x2 → (20, 0, 0) → yaw 90 → (0, 20, 0) → + 부모.
    CHECK(NearVector(fixture.WorldOf(child).translation, 100.0, 20.0, 0.0));
    REQUIRE(world.With<SceneComponent>(child, [](SceneComponent& value) noexcept {
        return value.SetAbsolute(true, false, false);
    }));
    CHECK(NearVector(fixture.WorldOf(child).translation, 10.0, 0.0, 0.0));
    REQUIRE(world.With<SceneComponent>(child, [](SceneComponent& value) noexcept {
        auto cleared = value.SetAbsolute(false, false, false);
        return cleared ? value.Detach(AttachmentRule::KeepWorld) : Move(cleared);
    }));
    CHECK(NearVector(fixture.WorldOf(child).translation, 100.0, 20.0, 0.0));
    auto parentGone = world.With<SceneComponent>(
        child, [](SceneComponent& value) noexcept { return value.Parent().IsNull(); });
    CHECK(parentGone.Value());
    REQUIRE(world.Shutdown());
}

// 23.3절 socket: socket이 있으면 그 Transform이 부모 앞에 들어간다. 없는 socket은 UE처럼 받아 두고
// 부모 자신을 쓴다.
TEST(SceneFoundation_SocketAttachment) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto actor = world.SpawnActor<EmptyActor>();
    REQUIRE(actor);
    const ObjectHandle gun = fixture.AddScene<SocketScene>(actor.Value(), ParentTransform());
    const ObjectHandle flash = fixture.AddScene(actor.Value(), Transform::Identity());
    REQUIRE(!gun.IsNull() && !flash.IsNull());
    REQUIRE(world.With<SceneComponent>(flash, [&](SceneComponent& value) noexcept {
        return value.AttachTo(gun, AttachmentRules::KeepRelativeTransform(), "Muzzle");
    }));
    CHECK(NearVector(fixture.WorldOf(flash).translation, 100.0, 0.0, 50.0));
    auto socket = world.With<SceneComponent>(flash, [](SceneComponent& value) noexcept {
        return value.AttachSocketName() == StringView("Muzzle");
    });
    CHECK(socket.Value());
    REQUIRE(world.With<SceneComponent>(flash, [&](SceneComponent& value) noexcept {
        return value.AttachTo(gun, AttachmentRules::KeepRelativeTransform(), "Missing");
    }));
    CHECK(NearVector(fixture.WorldOf(flash).translation, 100.0, 0.0, 0.0));
    REQUIRE(world.Shutdown());
}

// 23.3절 ERD30: Static·Stationary는 게임 World에서 Actor 초기화 뒤 옮길 수 없다(OnAwake 안은 된다).
// 편집 World는 옮길 수 있다.
TEST(SceneFoundation_StaticMobilityRejectsMovesInGameWorlds) {
    for (WorldKind kind : {WorldKind::Game, WorldKind::Editor}) {
        Fixture fixture(kind);
        REQUIRE(fixture.registered);
        World& world = *fixture.world;
        REQUIRE(world.Initialize());
        auto actor = world.SpawnActor<StaticRootActor>();
        REQUIRE(actor);
        auto root = world.With<Actor>(actor.Value(),
                                      [](Actor& value) noexcept { return value.RootComponent(); });
        REQUIRE(root && !root.Value().IsNull());
        CHECK(NearVector(fixture.WorldOf(root.Value()).translation, 5.0, 0.0, 0.0));
        auto moved = world.With<SceneComponent>(root.Value(), [](SceneComponent& value) noexcept {
            return value.SetRelativeLocation(Vec3d(9.0, 0.0, 0.0));
        });
        const bool game = kind == WorldKind::Game;
        CHECK(game ? moved.Error() == ErrorCode::InvalidArgument : moved.IsOk());
        auto other = fixture.AddScene(actor.Value(), Transform::Identity());
        auto attached = world.With<SceneComponent>(
            root.Value(), [&](SceneComponent& value) noexcept { return value.AttachTo(other); });
        // Static은 어느 World에서든 Static 부모에만 붙는다(UE SceneComponent.cpp:2381). 게임
        // World는 Mobility 검사가 먼저 거부한다.
        CHECK(attached.Error() == ErrorCode::InvalidArgument);
        // Movable로 바꾸면 된다.
        auto freed = world.With<SceneComponent>(root.Value(), [](SceneComponent& value) noexcept {
            value.SetMobility(ComponentMobility::Movable);
            return value.SetRelativeLocation(Vec3d(9.0, 0.0, 0.0));
        });
        CHECK(freed.IsOk());
        REQUIRE(world.Shutdown());
    }
}

// 23.3절 가시성: visible 전파, hiddenInGame은 게임 World에서만 숨긴다. 바뀔 때만 hook.
TEST(SceneFoundation_VisibilityPropagationAndHiddenInGame) {
    for (WorldKind kind : {WorldKind::Game, WorldKind::Editor}) {
        Fixture fixture(kind);
        REQUIRE(fixture.registered);
        World& world = *fixture.world;
        REQUIRE(world.Initialize());
        auto actor = world.SpawnActor<EmptyActor>();
        REQUIRE(actor);
        const ObjectHandle parent =
            fixture.AddScene<CountingScene>(actor.Value(), Transform::Identity());
        const ObjectHandle child =
            fixture.AddScene<CountingScene>(actor.Value(), Transform::Identity());
        REQUIRE(world.With<SceneComponent>(
            child, [&](SceneComponent& value) noexcept { return value.AttachTo(parent); }));
        auto visible = [&](ObjectHandle scene) noexcept {
            return world
                .With<SceneComponent>(
                    scene, [](SceneComponent& value) noexcept { return value.IsVisible(); })
                .Value();
        };
        REQUIRE(world.With<SceneComponent>(parent, [](SceneComponent& value) noexcept {
            value.SetVisibility(false, true);
            value.SetVisibility(false, true);
        }));
        CHECK(!visible(parent) && !visible(child));
        auto hooks = world.With<CountingScene>(
            child, [](CountingScene& value) noexcept { return value.visibilityCount; });
        CHECK(hooks.Value() == 1);
        REQUIRE(world.With<SceneComponent>(parent, [](SceneComponent& value) noexcept {
            value.SetVisibility(true, true);
            value.SetHiddenInGame(true, false);
        }));
        CHECK(visible(parent) == (kind != WorldKind::Game));
        CHECK(visible(child));
        REQUIRE(world.Shutdown());
    }
}

// 23.3절 bounds 캐시·Transform 통지: 바뀐 뒤 첫 Bounds()에서만 다시 계산하고, 부모 이동은
// 자신·자손에 한 번씩 통지한다. 속도는 쓴 값 그대로다.
TEST(SceneFoundation_BoundsCacheTransformNotificationAndVelocity) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto actor = world.SpawnActor<EmptyActor>();
    REQUIRE(actor);
    const ObjectHandle parent =
        fixture.AddScene<CountingScene>(actor.Value(), Transform::Identity());
    const ObjectHandle child = fixture.AddScene<CountingScene>(
        actor.Value(), Transform(Quatd::Identity(), Vec3d(0.0, 0.0, 10.0), Vec3d::One()));
    REQUIRE(world.With<SceneComponent>(
        child, [&](SceneComponent& value) noexcept { return value.AttachTo(parent); }));
    auto counts = [&](ObjectHandle scene) noexcept {
        return world
            .With<CountingScene>(scene,
                                 [](CountingScene& value) noexcept {
                                     return value.updateCount * 100 + value.calcCount;
                                 })
            .Value();
    };
    const uint32 parentBefore = counts(parent);
    const uint32 childBefore = counts(child);
    auto bounds = world.With<CountingScene>(child, [](CountingScene& value) noexcept {
        const ComponentBounds first = value.Bounds();
        const ComponentBounds second = value.Bounds();
        return first.origin.z == 10.0 && second.boxExtent.x == 10.0;
    });
    CHECK(bounds.Value());
    CHECK(counts(child) == childBefore + 1);
    REQUIRE(world.With<SceneComponent>(parent, [](SceneComponent& value) noexcept {
        return value.SetRelativeLocation(Vec3d(0.0, 0.0, 5.0));
    }));
    CHECK(counts(parent) == parentBefore + 100);
    CHECK(counts(child) == childBefore + 101);
    auto moved = world.With<CountingScene>(
        child, [](CountingScene& value) noexcept { return value.Bounds().origin.z; });
    CHECK(Near(moved.Value(), 15.0));
    auto velocity = world.With<SceneComponent>(child, [](SceneComponent& value) noexcept {
        value.SetComponentVelocity(Vec3d(0.0, 0.0, -980.0));
        return value.ComponentVelocity().z;
    });
    CHECK(velocity.Value() == -980.0);
    REQUIRE(world.Shutdown());
}

// 23.4절: CameraComponent 시야 값 검증과 GetCameraView(월드 Transform·필드).
TEST(SceneFoundation_CameraComponentView) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto actor = world.SpawnActor<EmptyActor>();
    REQUIRE(actor);
    const ObjectHandle root = fixture.AddScene(actor.Value(), ParentTransform());
    auto camera = world.AddComponent<CameraComponent>(actor.Value());
    REQUIRE(camera);
    auto configured =
        world.With<CameraComponent>(camera.Value(), [&](CameraComponent& value) noexcept {
            CHECK(value.FieldOfView() == CameraComponent::DefaultFieldOfView);
            CHECK(value.SetFieldOfView(4.0).Error() == ErrorCode::InvalidArgument);
            CHECK(value.SetFieldOfView(171.0).Error() == ErrorCode::InvalidArgument);
            CHECK(value.SetOrthoWidth(0.0).Error() == ErrorCode::InvalidArgument);
            CHECK(value.SetAspectRatio(-1.0).Error() == ErrorCode::InvalidArgument);
            value.SetUsePawnControlRotation(true);
            value.SetProjection(CameraProjection::Orthographic);
            auto fov = value.SetFieldOfView(100.0);
            auto attached = value.AttachTo(root);
            auto lifted = value.SetRelativeLocation(Vec3d(0.0, 0.0, 60.0));
            return fov.IsOk() && attached.IsOk() && lifted.IsOk();
        });
    CHECK(configured.Value());
    auto view = world.With<CameraComponent>(camera.Value(), [](CameraComponent& value) noexcept {
        CameraViewInfo info;
        value.GetCameraView(1.0 / 60.0, info);
        return info;
    });
    REQUIRE(view);
    CHECK(NearVector(view.Value().location, 100.0, 0.0, 60.0));
    CHECK(view.Value().fieldOfView == 100.0 && view.Value().usePawnControlRotation);
    CHECK(view.Value().projection == CameraProjection::Orthographic);
    CHECK(NearVector(view.Value().rotation.RotateVector(Vec3d(1.0, 0.0, 0.0)), 0.0, 1.0, 0.0));
    REQUIRE(world.Shutdown());
}

// 리뷰 보강(23.3절): 축에 맞지 않은 비균일 부모(yaw 45, scale (2, 1, 1)) 아래에서도 KeepWorld가
// 되고(정확한 역변환이 shear로 실패하면 UE 식 — 위치는 그대로), 절대 회전은 부모와 무관하다. 절대
// 위치 자식의 KeepWorld는 월드 위치를 지킨다.
TEST(SceneFoundation_SkewedParentKeepWorldAndAbsoluteRotation) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto actor = world.SpawnActor<EmptyActor>();
    REQUIRE(actor);
    const Transform skewed(Rotator<double>(0.0, 45.0, 0.0).ToQuaternion(), Vec3d(100.0, 0.0, 0.0),
                           Vec3d(2.0, 1.0, 1.0));
    const ObjectHandle parent = fixture.AddScene(actor.Value(), skewed);
    const ObjectHandle child =
        fixture.AddScene(actor.Value(), Transform(Rotator<double>(0.0, 30.0, 0.0).ToQuaternion(),
                                                  Vec3d(0.0, 50.0, 0.0), Vec3d::One()));
    REQUIRE(!parent.IsNull() && !child.IsNull());
    REQUIRE(world.With<SceneComponent>(child, [&](SceneComponent& value) noexcept {
        return value.AttachTo(parent, AttachmentRules::KeepWorldTransform());
    }));
    // 부모가 비균일이고 축이 어긋나 월드 행렬에 shear가 있다(TRS 분해 불가) — 위치는 행렬에서
    // 읽는다.
    auto location = world.With<SceneComponent>(
        child, [](SceneComponent& value) noexcept { return value.WorldLocation(); });
    CHECK(NearVector(location.Value(), 0.0, 50.0, 0.0));

    // 절대 회전: 곱이 shear로 분해되지 않아도 상대 회전(yaw 30 — 위 KeepWorld가 다시 계산한 값과
    // 무관하게 새로 준다)이 월드 회전이다.
    REQUIRE(world.With<SceneComponent>(child, [](SceneComponent& value) noexcept -> Result<void> {
        if (auto absolute = value.SetAbsolute(false, true, false); !absolute) {
            return absolute;
        }

        return value.SetRelativeRotation(Rotator<double>(0.0, 30.0, 0.0).ToQuaternion());
    }));
    const Vec3d forward = fixture.WorldOf(child).rotation.RotateVector(Vec3d(1.0, 0.0, 0.0));
    const double half = 0.8660254037844387;
    CHECK(NearVector(forward, half, 0.5, 0.0));

    // 절대 위치 자식의 KeepWorld 부착은 월드 위치를 지킨다(UE rule switch).
    const ObjectHandle pinned = fixture.AddScene(
        actor.Value(), Transform(Quatd::Identity(), Vec3d(10.0, 0.0, 0.0), Vec3d::One()));
    REQUIRE(world.With<SceneComponent>(pinned, [&](SceneComponent& value) noexcept -> Result<void> {
        if (auto absolute = value.SetAbsolute(true, false, false); !absolute) {
            return absolute;
        }

        return value.AttachTo(parent, AttachmentRules::KeepWorldTransform());
    }));
    CHECK(NearVector(fixture.WorldOf(pinned).translation, 10.0, 0.0, 0.0));
    CHECK(world
              .With<SceneComponent>(pinned,
                                    [](SceneComponent& value) noexcept {
                                        return value.Detach(AttachmentRule::SnapToTarget);
                                    })
              .Error() == ErrorCode::InvalidArgument);
    REQUIRE(world.Shutdown());
}
} // namespace world_scene_foundation_tests_file
