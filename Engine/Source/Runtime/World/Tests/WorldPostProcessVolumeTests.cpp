#include <Base/InitializerList.h>
#include <Core/Math/Rotator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <TestSupport/Test.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Lights/LightingChannels.generated.h>
#include <World/PostProcess/PostProcessComponent.generated.h>
#include <World/PostProcess/PostProcessComponent.h>
#include <World/PostProcess/PostProcessSettings.generated.h>
#include <World/PostProcess/PostProcessVolumeBlending.h>
#include <World/Primitives/BoxComponent.generated.h>
#include <World/Primitives/BoxComponent.h>
#include <World/Primitives/CapsuleComponent.generated.h>
#include <World/Primitives/CapsuleComponent.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/Primitives/PrimitiveTypes.generated.h>
#include <World/Primitives/ShapeComponent.generated.h>
#include <World/Primitives/SphereComponent.generated.h>
#include <World/Primitives/SphereComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>
#include <math.h>
#include <stdio.h>

// 영역 형상(CR17)과 후처리 볼륨 혼합 시험이다(Docs/Design/EngineRuntime 28.5절): Shape 월드 형상,
// EncompassesPoint 분기, 볼륨 속성, 정렬, 가중 혼합.
using namespace rocks;
namespace world_post_process_volume_tests_file {

namespace {

constexpr double VolumeTestTolerance = 1.0e-6;
constexpr double VolumeTestStep = 1.0 / 60.0;

bool NearVolumeValue(double value, double expected) noexcept {
    const bool same = fabs(value - expected) <= VolumeTestTolerance;
    if (!same) {
        printf("  value %.12g, expected %.12g\n", value, expected);
    }

    return same;
}

class VolumeTestActor final : public Actor {
    ROCKS_NATIVE_TYPE(VolumeTestActor, Actor, 0x51c8e7a204bd3f96ull, 0x8e3a6d1f27c0b549ull)

public:
    explicit VolumeTestActor(Allocator& allocator) noexcept : Actor(allocator) {
    }
};

// Shape가 아닌 primitive다(지역 bounds 반 크기 50 상자).
class VolumeTestPrimitive final : public PrimitiveComponent {
    ROCKS_NATIVE_TYPE(VolumeTestPrimitive, PrimitiveComponent, 0x2f94b1d07c6e3a85ull,
                      0xc17d5e08a93b4f26ull)

public:
    explicit VolumeTestPrimitive(Allocator& allocator) noexcept : PrimitiveComponent(allocator) {
    }

protected:
    bool TryLocalBounds(ComponentBounds& local) const noexcept override {
        local = ComponentBounds{};
        local.boxExtent = Vec3d(50.0, 50.0, 50.0);
        local.sphereRadius = local.boxExtent.Length();
        return true;
    }
};

struct VolumeFixture {
    VolumeFixture() noexcept : types(allocator), modules(types, allocator) {
        registered = RegisterWorldTypes(modules).IsOk() && Register<VolumeTestActor>() &&
                     Register<VolumeTestPrimitive>();
        WorldDesc desc;
        desc.name = "Volumes";
        desc.context = {0x564f4cull, 1};
        desc.timeStep = {VolumeTestStep, 0.25, 8};
        desc.kind = WorldKind::Game;
        auto created = UniquePtr<World>::TryMake(allocator, allocator, types, desc);
        if (created) {
            world = Move(created.Value());
        }
    }

    ~VolumeFixture() {
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

    ObjectHandle Spawn() noexcept {
        auto spawned = world->SpawnActor<VolumeTestActor>();
        return spawned ? spawned.Value() : ObjectHandle{};
    }

    template <class T>
    ObjectHandle Add(ObjectHandle actor) noexcept {
        auto added = world->AddComponent<T>(actor);
        return added ? added.Value() : ObjectHandle{};
    }

    // shape를 root로, 그 아래 볼륨을 붙인 Actor다(위치 location, 상자 반 크기 extent).
    ObjectHandle BoxVolume(const Vec3d& location, const Vec3d& extent, ObjectHandle& box) noexcept {
        const ObjectHandle actor = Spawn();
        box = Add<BoxComponent>(actor);
        const ObjectHandle volume = Add<PostProcessComponent>(actor);
        if (box.IsNull() || volume.IsNull()) {
            return {};
        }

        const bool placed =
            world
                ->With<BoxComponent>(box,
                                     [&](BoxComponent& shape) noexcept {
                                         if (auto set = shape.SetBoxExtent(extent); !set) {
                                             return set;
                                         }

                                         return shape.SetRelativeLocation(location);
                                     })
                .IsOk();
        const bool attached =
            world
                ->With<PostProcessComponent>(volume,
                                             [&](PostProcessComponent& post) noexcept {
                                                 post.SetUnbound(false);
                                                 return post.AttachTo(box);
                                             })
                .IsOk();
        return placed && attached ? volume : ObjectHandle{};
    }

    static constexpr usize MaxTokens = 2;
    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    TypeRegistrationToken tokens[MaxTokens];
    usize tokenCount = 0;
    bool registered = false;
    UniquePtr<World> world;
};

PostProcessSettings BloomSettings(float bloom) noexcept {
    PostProcessSettings settings;
    settings.overrideBloomIntensity = true;
    settings.bloomIntensity = bloom;
    return settings;
}

} // namespace

// 28.3절: Shape 컴포넌트의 월드 형상 — 중심·회전·scale 절댓값, 부모 사슬.
TEST(PostProcessVolume_ShapeWorldVolume) {
    VolumeFixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    const ObjectHandle actor = fixture.Spawn();
    const ObjectHandle parent = fixture.Add<SceneComponent>(actor);
    const ObjectHandle box = fixture.Add<BoxComponent>(actor);
    const ObjectHandle sphere = fixture.Add<SphereComponent>(actor);
    const ObjectHandle capsule = fixture.Add<CapsuleComponent>(actor);
    REQUIRE(!parent.IsNull() && !box.IsNull() && !sphere.IsNull() && !capsule.IsNull());
    REQUIRE(world.With<SceneComponent>(parent, [](SceneComponent& c) noexcept {
        return c.SetRelativeLocation(Vec3d(100.0, 0.0, 0.0));
    }));

    CHECK(world.With<BoxComponent>(box, [&](BoxComponent& c) noexcept {
        REQUIRE(c.AttachTo(parent));
        REQUIRE(c.SetBoxExtent(Vec3d(10.0, 20.0, 30.0)));
        REQUIRE(c.SetRelativeScale(Vec3d(-2.0, 1.0, 1.0)));
        REQUIRE(c.SetRelativeRotation(Rotator<double>(0.0, 90.0, 0.0).ToQuaternion()));
        auto shape = c.TryWorldVolumeShape();
        REQUIRE(shape);
        CHECK(shape.Value().kind == VolumeShapeKind::Box);
        CHECK(NearVolumeValue(shape.Value().center.x, 100.0));
        // 반 크기는 |extent × scale| = (20, 20, 30)이다(반사 부호는 버린다).
        CHECK(NearVolumeValue(shape.Value().boxHalfExtent.x, 20.0));
        CHECK(NearVolumeValue(shape.Value().boxHalfExtent.y, 20.0));
        CHECK(NearVolumeValue(shape.Value().boxHalfExtent.z, 30.0));
        // yaw 90°: 지역 X(20)가 월드 Y다.
        auto inside = c.TryDistanceToShape(Vec3d(100.0, 19.0, 0.0));
        auto beside = c.TryDistanceToShape(Vec3d(125.0, 0.0, 0.0));
        REQUIRE(inside && beside);
        CHECK(NearVolumeValue(inside.Value(), 0.0));
        CHECK(NearVolumeValue(beside.Value(), 5.0));
        CHECK(!c.TryDistanceToShape(Vec3d(nan(""), 0.0, 0.0)));
    }));

    CHECK(world.With<SphereComponent>(sphere, [](SphereComponent& c) noexcept {
        REQUIRE(c.SetSphereRadius(10.0f));
        REQUIRE(c.SetRelativeScale(Vec3d(-3.0, 2.0, 4.0)));
        auto shape = c.TryWorldVolumeShape();
        REQUIRE(shape);
        CHECK(shape.Value().kind == VolumeShapeKind::Sphere);
        // 최소 |scale| 2 × 10.
        CHECK(NearVolumeValue(shape.Value().radius, 20.0));
        auto distance = c.TryDistanceToShape(Vec3d(0.0, 0.0, 50.0));
        REQUIRE(distance);
        CHECK(NearVolumeValue(distance.Value(), 30.0));
    }));

    CHECK(world.With<CapsuleComponent>(capsule, [](CapsuleComponent& c) noexcept {
        REQUIRE(c.SetCapsuleSize(10.0f, 40.0f));
        REQUIRE(c.SetRelativeScale(Vec3d(2.0, 3.0, 0.5)));
        auto shape = c.TryWorldVolumeShape();
        REQUIRE(shape);
        CHECK(shape.Value().kind == VolumeShapeKind::Capsule);
        // UE 물리 형상: 반 높이 40 × 0.5 = 20, 반지름 clamp(10 × max(2, 3), 0.1, 20) = 20.
        CHECK(NearVolumeValue(shape.Value().radius, 20.0));
        CHECK(NearVolumeValue(shape.Value().halfHeight, 20.0));
        // scale (2, 1, 1): 반지름은 max 축(2) 배 — 컴포넌트 bounds의 min 규칙과 다르다.
        REQUIRE(c.SetRelativeScale(Vec3d(2.0, 1.0, 1.0)));
        auto wide = c.TryWorldVolumeShape();
        REQUIRE(wide);
        CHECK(NearVolumeValue(wide.Value().radius, 20.0));
        CHECK(NearVolumeValue(wide.Value().halfHeight, 40.0));
        // scale (3, 3, 1)·r 22·h 44: 반지름 66이 반 높이 44로 잘린다.
        REQUIRE(c.SetCapsuleSize(22.0f, 44.0f));
        REQUIRE(c.SetRelativeScale(Vec3d(3.0, 3.0, 1.0)));
        auto clamped = c.TryWorldVolumeShape();
        REQUIRE(clamped);
        CHECK(NearVolumeValue(clamped.Value().radius, 44.0));
        CHECK(NearVolumeValue(clamped.Value().halfHeight, 44.0));
        // 0 scale도 0.1 cm 하한이다.
        REQUIRE(c.SetRelativeScale(Vec3d(0.0, 0.0, 0.0)));
        auto tiny = c.TryWorldVolumeShape();
        REQUIRE(tiny);
        CHECK(NearVolumeValue(tiny.Value().radius, 0.1));
        CHECK(NearVolumeValue(tiny.Value().halfHeight, 0.1));
    }));

    // 부모의 회전·scale이 전파된다: 부모 yaw 90°·scale 2 아래 상자(extent 10, 20, 30).
    const ObjectHandle turnedActor = fixture.Spawn();
    const ObjectHandle turnedParent = fixture.Add<SceneComponent>(turnedActor);
    const ObjectHandle child = fixture.Add<BoxComponent>(turnedActor);
    REQUIRE(world.With<SceneComponent>(turnedParent, [](SceneComponent& c) noexcept {
        if (auto set = c.SetRelativeRotation(Rotator<double>(0.0, 90.0, 0.0).ToQuaternion());
            !set) {
            return set;
        }

        return c.SetRelativeScale(Vec3d(2.0, 2.0, 2.0));
    }));
    CHECK(world.With<BoxComponent>(child, [&](BoxComponent& c) noexcept {
        REQUIRE(c.AttachTo(turnedParent));
        REQUIRE(c.SetBoxExtent(Vec3d(10.0, 20.0, 30.0)));
        REQUIRE(c.SetRelativeLocation(Vec3d(50.0, 0.0, 0.0)));
        auto shape = c.TryWorldVolumeShape();
        REQUIRE(shape);
        // 상대 (50, 0, 0) × scale 2 → yaw 90° → 월드 (0, 100, 0).
        CHECK(NearVolumeValue(shape.Value().center.y, 100.0));
        CHECK(NearVolumeValue(shape.Value().boxHalfExtent.x, 20.0));
        CHECK(NearVolumeValue(shape.Value().boxHalfExtent.z, 60.0));
        // 지역 X 반 크기 20이 월드 Y다: (0, 100 + 25, 0)은 5 밖이다.
        auto beside = c.TryDistanceToShape(Vec3d(0.0, 125.0, 0.0));
        REQUIRE(beside);
        CHECK(NearVolumeValue(beside.Value(), 5.0));
    }));
    REQUIRE(world.Shutdown());
}

// 28.4절: EncompassesPoint 분기 다섯과 볼륨 속성.
TEST(PostProcessVolume_EncompassesPointBranches) {
    VolumeFixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());

    // 1. 경계 없음(기본 unbound).
    const ObjectHandle loose = fixture.Spawn();
    const ObjectHandle unbound = fixture.Add<PostProcessComponent>(loose);
    REQUIRE(!unbound.IsNull());
    CHECK(world.With<PostProcessComponent>(unbound, [](PostProcessComponent& c) noexcept {
        const PostProcessEncompass result = c.EncompassesPoint(Vec3d(1.0e6, 0.0, 0.0));
        CHECK(result.inside && result.distance == 0.0);
        const PostProcessVolumeProperties properties = c.VolumeProperties();
        CHECK(properties.enabled && properties.unbound);
        CHECK(properties.size == PostProcessUnboundSize);
        // unbound를 꺼도 부모가 없으면 경계가 없다.
        c.SetUnbound(false);
        ComponentBounds bounds;
        CHECK(!c.TryPostProcessBounds(bounds));
        CHECK(c.EncompassesPoint(Vec3d(1.0e6, 0.0, 0.0)).inside);
        CHECK(c.VolumeProperties().unbound);
    }));

    // 2. 부모가 primitive가 아니다(SceneComponent) — 경계 없음.
    const ObjectHandle sceneActor = fixture.Spawn();
    const ObjectHandle scene = fixture.Add<SceneComponent>(sceneActor);
    const ObjectHandle sceneVolume = fixture.Add<PostProcessComponent>(sceneActor);
    REQUIRE(world.With<PostProcessComponent>(sceneVolume, [&](PostProcessComponent& c) noexcept {
        c.SetUnbound(false);
        return c.AttachTo(scene);
    }));
    CHECK(world.With<PostProcessComponent>(sceneVolume, [](PostProcessComponent& c) noexcept {
        CHECK(c.EncompassesPoint(Vec3d(1.0e6, 0.0, 0.0)).inside);
        CHECK(c.VolumeProperties().unbound);
    }));

    // 3·4. 부모 상자(중심 원점, 반 크기 100, blendRadius 100).
    ObjectHandle box;
    const ObjectHandle volume = fixture.BoxVolume(Vec3d(0.0, 0.0, 0.0), Vec3d(100, 100, 100), box);
    REQUIRE(!volume.IsNull());
    CHECK(world.With<PostProcessComponent>(volume, [](PostProcessComponent& c) noexcept {
        ComponentBounds bounds;
        REQUIRE(c.TryPostProcessBounds(bounds));
        CHECK(NearVolumeValue(bounds.boxExtent.x, 100.0));
        const PostProcessVolumeProperties properties = c.VolumeProperties();
        CHECK(!properties.unbound && properties.enabled);
        CHECK(NearVolumeValue(properties.size, 200.0 * 200.0 * 200.0));
        // 안.
        const PostProcessEncompass center = c.EncompassesPoint(Vec3d(10.0, 0.0, 0.0));
        CHECK(center.inside && center.distance == 0.0);
        // 넓힌 상자 안·형상 밖: 거리 50.
        const PostProcessEncompass near = c.EncompassesPoint(Vec3d(150.0, 0.0, 0.0));
        CHECK(!near.inside && NearVolumeValue(near.distance, 50.0));
        // sphereRadius가 거리 이상이면 안이다.
        CHECK(c.EncompassesPoint(Vec3d(150.0, 0.0, 0.0), 50.0f).inside);
        // 넓힌 상자(100 + 100) 밖은 거리를 재지 않는다.
        const PostProcessEncompass far = c.EncompassesPoint(Vec3d(250.0, 0.0, 0.0));
        CHECK(!far.inside && far.distance == PostProcessOutsideDistance);
        // 넓힘에 sphereRadius가 더해진다(100 + 100 + 60 ≥ 250): 형상 거리 150을 재고 밖이다.
        const PostProcessEncompass reach = c.EncompassesPoint(Vec3d(250.0, 0.0, 0.0), 60.0f);
        CHECK(!reach.inside && NearVolumeValue(reach.distance, 150.0));
    }));

    // 5. Shape가 아닌 primitive 부모 — 넓힌 상자 안이면 어디서나 안(거리 0).
    const ObjectHandle primitiveActor = fixture.Spawn();
    const ObjectHandle primitive = fixture.Add<VolumeTestPrimitive>(primitiveActor);
    const ObjectHandle primitiveVolume = fixture.Add<PostProcessComponent>(primitiveActor);
    REQUIRE(
        world.With<PostProcessComponent>(primitiveVolume, [&](PostProcessComponent& c) noexcept {
            c.SetUnbound(false);
            return c.AttachTo(primitive);
        }));
    CHECK(world.With<PostProcessComponent>(primitiveVolume, [](PostProcessComponent& c) noexcept {
        const PostProcessEncompass inBox = c.EncompassesPoint(Vec3d(120.0, 0.0, 0.0));
        CHECK(inBox.inside && inBox.distance == 0.0);
        CHECK(!c.EncompassesPoint(Vec3d(300.0, 0.0, 0.0)).inside);
        CHECK(!c.VolumeProperties().unbound);
    }));

    // 구·캡슐 부모도 정확한 형상 거리다.
    const ObjectHandle sphereActor = fixture.Spawn();
    const ObjectHandle sphere = fixture.Add<SphereComponent>(sphereActor);
    const ObjectHandle sphereVolume = fixture.Add<PostProcessComponent>(sphereActor);
    REQUIRE(world.With<SphereComponent>(
        sphere, [](SphereComponent& c) noexcept { return c.SetSphereRadius(100.0f); }));
    REQUIRE(world.With<PostProcessComponent>(sphereVolume, [&](PostProcessComponent& c) noexcept {
        c.SetUnbound(false);
        return c.AttachTo(sphere);
    }));
    CHECK(world.With<PostProcessComponent>(sphereVolume, [](PostProcessComponent& c) noexcept {
        // 상자 bounds 모서리 쪽 (90, 90, 0)은 구 밖이다(|p| − 100).
        const PostProcessEncompass corner = c.EncompassesPoint(Vec3d(90.0, 90.0, 0.0));
        CHECK(!corner.inside && NearVolumeValue(corner.distance, sqrt(2.0) * 90.0 - 100.0));
        CHECK(c.EncompassesPoint(Vec3d(0.0, 0.0, 99.0)).inside);
    }));
    const ObjectHandle capsuleActor = fixture.Spawn();
    const ObjectHandle capsule = fixture.Add<CapsuleComponent>(capsuleActor);
    const ObjectHandle capsuleVolume = fixture.Add<PostProcessComponent>(capsuleActor);
    REQUIRE(world.With<CapsuleComponent>(
        capsule, [](CapsuleComponent& c) noexcept { return c.SetCapsuleSize(20.0f, 60.0f); }));
    REQUIRE(world.With<PostProcessComponent>(capsuleVolume, [&](PostProcessComponent& c) noexcept {
        c.SetUnbound(false);
        return c.AttachTo(capsule);
    }));
    CHECK(world.With<PostProcessComponent>(capsuleVolume, [](PostProcessComponent& c) noexcept {
        // 축 선분 z ∈ [−40, 40]: (30, 0, 0)은 10 밖, (0, 0, 70)은 10 밖, (0, 0, 55)는 안.
        CHECK(NearVolumeValue(c.EncompassesPoint(Vec3d(30.0, 0.0, 0.0)).distance, 10.0));
        CHECK(NearVolumeValue(c.EncompassesPoint(Vec3d(0.0, 0.0, 70.0)).distance, 10.0));
        CHECK(c.EncompassesPoint(Vec3d(0.0, 0.0, 55.0)).inside);
    }));

    // 숨기면(게임 World의 hiddenInGame) 꺼진다(UE ShouldRender).
    CHECK(world.With<PostProcessComponent>(volume, [](PostProcessComponent& c) noexcept {
        c.SetHiddenInGame(true);
        CHECK(!c.VolumeProperties().enabled);
        c.SetHiddenInGame(false);
        c.SetPostProcessEnabled(false);
        CHECK(!c.VolumeProperties().enabled);
    }));
    REQUIRE(world.Shutdown());
}

// 28.4절: 정렬(priority → unbound → size)과 볼륨 하나의 가중치.
TEST(PostProcessVolume_OrderAndWeight) {
    PostProcessVolumeProperties low;
    low.priority = 0.0f;
    PostProcessVolumeProperties high;
    high.priority = 1.0f;
    CHECK(PostProcessVolumeLess(low, high) && !PostProcessVolumeLess(high, low));
    PostProcessVolumeProperties bounded = low;
    bounded.unbound = false;
    bounded.size = 10.0;
    CHECK(PostProcessVolumeLess(low, bounded) && !PostProcessVolumeLess(bounded, low));
    PostProcessVolumeProperties bigger = bounded;
    bigger.size = 20.0;
    CHECK(PostProcessVolumeLess(bigger, bounded) && !PostProcessVolumeLess(bounded, bigger));
    CHECK(!PostProcessVolumeLess(bounded, bounded));

    PostProcessVolumeProperties properties;
    properties.enabled = true;
    properties.unbound = false;
    properties.blendRadius = 100.0f;
    properties.blendWeight = 1.0f;
    CHECK(PostProcessVolumeWeight(properties, PostProcessEncompass{true, 0.0}) == 1.0f);
    CHECK(PostProcessVolumeWeight(properties, PostProcessEncompass{false, 25.0}) == 0.75f);
    CHECK(PostProcessVolumeWeight(properties, PostProcessEncompass{false, 100.0}) == 0.0f);
    CHECK(PostProcessVolumeWeight(properties, PostProcessEncompass{false, 100.5}) == 0.0f);
    CHECK(PostProcessVolumeWeight(properties, PostProcessEncompass{false, -1.0}) == 0.0f);
    CHECK(PostProcessVolumeWeight(properties,
                                  PostProcessEncompass{false, PostProcessOutsideDistance}) == 0.0f);
    // blendWeight는 [0, 1]로 자른다.
    properties.blendWeight = 1.5f;
    CHECK(PostProcessVolumeWeight(properties, PostProcessEncompass{true, 0.0}) == 1.0f);
    properties.blendWeight = -0.5f;
    CHECK(PostProcessVolumeWeight(properties, PostProcessEncompass{true, 0.0}) == 0.0f);
    // blendRadius < 1이면 거리 비율을 곱하지 않는다(0으로 나누기 방지).
    properties.blendWeight = 0.5f;
    properties.blendRadius = 0.5f;
    CHECK(PostProcessVolumeWeight(properties, PostProcessEncompass{false, 0.25}) == 0.5f);
    CHECK(PostProcessVolumeWeight(properties, PostProcessEncompass{false, 0.75}) == 0.0f);
    // unbound는 거리를 보지 않는다. 꺼지면 0이다.
    properties.unbound = true;
    CHECK(PostProcessVolumeWeight(properties, PostProcessEncompass{false, 1.0e9}) == 0.5f);
    properties.enabled = false;
    CHECK(PostProcessVolumeWeight(properties, PostProcessEncompass{true, 0.0}) == 0.0f);
}

// 28.4절: 모으기·정렬·가중 혼합 — view 위치에 따른 결과, 등록 전 컴포넌트 제외, 우선순위.
TEST(PostProcessVolume_BlendVolumes) {
    VolumeFixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());

    // 전역(unbound) bloom 1, 상자(반 크기 100, blendRadius 100, priority 1) bloom 5.
    const ObjectHandle globalActor = fixture.Spawn();
    const ObjectHandle global = fixture.Add<PostProcessComponent>(globalActor);
    REQUIRE(world.With<PostProcessComponent>(global, [](PostProcessComponent& c) noexcept {
        return c.SetSettings(BloomSettings(1.0f));
    }));
    ObjectHandle box;
    const ObjectHandle local = fixture.BoxVolume(Vec3d(0.0, 0.0, 0.0), Vec3d(100, 100, 100), box);
    REQUIRE(!local.IsNull());
    REQUIRE(world.With<PostProcessComponent>(local, [](PostProcessComponent& c) noexcept {
        if (auto set = c.SetPriority(1.0f); !set) {
            return set;
        }

        return c.SetSettings(BloomSettings(5.0f));
    }));
    // 등록 전(지연 Spawn — Constructed) 볼륨은 모으지 않는다.
    auto deferred = world.SpawnActorDeferred<VolumeTestActor>();
    REQUIRE(deferred);
    auto pending = world.AddComponent<PostProcessComponent>(deferred.Value());
    REQUIRE(pending);
    REQUIRE(world.With<PostProcessComponent>(pending.Value(), [](PostProcessComponent& c) noexcept {
        if (auto set = c.SetPriority(9.0f); !set) {
            return set;
        }

        return c.SetSettings(BloomSettings(100.0f));
    }));

    const auto blendAt = [&](const Vec3d& view, uint32 expectedApplied) noexcept {
        PostProcessSettings settings;
        auto applied = BlendPostProcessVolumes(world, fixture.allocator, view, settings);
        CHECK(applied && applied.Value() == expectedApplied);
        return applied ? settings.bloomIntensity : -1.0f;
    };

    // 상자 안: 전역(1) 뒤 상자(5)가 가중치 1로 덮는다.
    CHECK(blendAt(Vec3d(0.0, 0.0, 0.0), 2) == 5.0f);
    // 표면에서 50: 가중치 0.5 → Lerp(1, 5, 0.5) = 3.
    CHECK(NearVolumeValue(blendAt(Vec3d(150.0, 0.0, 0.0), 2), 3.0));
    // blendRadius 밖: 전역만.
    CHECK(blendAt(Vec3d(250.0, 0.0, 0.0), 1) == 1.0f);

    // 우선순위를 뒤집으면 상자 뒤에 전역이 덮는다.
    REQUIRE(world.With<PostProcessComponent>(
        global, [](PostProcessComponent& c) noexcept { return c.SetPriority(2.0f); }));
    CHECK(blendAt(Vec3d(0.0, 0.0, 0.0), 2) == 1.0f);

    // 꺼진 볼륨은 건너뛴다.
    CHECK(world.With<PostProcessComponent>(
        global, [](PostProcessComponent& c) noexcept { c.SetPostProcessEnabled(false); }));
    CHECK(blendAt(Vec3d(0.0, 0.0, 0.0), 1) == 5.0f);

    // 지연 Spawn을 끝내면 모은다(priority 9 — 마지막).
    REQUIRE(world.FinishSpawning(deferred.Value()));
    CHECK(blendAt(Vec3d(0.0, 0.0, 0.0), 2) == 100.0f);

    // 같은 조작이면 같은 결과다(결정적).
    CHECK(blendAt(Vec3d(0.0, 0.0, 0.0), 2) == 100.0f);

    PostProcessSettings untouched;
    CHECK(!BlendPostProcessVolumes(world, fixture.allocator, Vec3d(nan(""), 0.0, 0.0), untouched));
    CHECK(untouched == PostProcessSettings{});

    // 파괴 요청된(PendingDestroy) 볼륨은 모으지 않는다(프레임 끝 정리 전에도).
    REQUIRE(world.DestroyComponent(pending.Value()));
    CHECK(blendAt(Vec3d(0.0, 0.0, 0.0), 1) == 5.0f);
    REQUIRE(world.Shutdown());
}

// 28.4절: 같은 priority면 큰 상자가 먼저(작은 상자가 뒤에 덮는다), 속성이 같으면 handle 순,
// Actor 하나에 컴포넌트가 16개를 넘어도 모은다.
TEST(PostProcessVolume_GatherOrderAndManyComponents) {
    VolumeFixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());

    ObjectHandle smallBox;
    ObjectHandle bigBox;
    const ObjectHandle small = fixture.BoxVolume(Vec3d(0.0, 0.0, 0.0), Vec3d(50, 50, 50), smallBox);
    const ObjectHandle big = fixture.BoxVolume(Vec3d(0.0, 0.0, 0.0), Vec3d(500, 500, 500), bigBox);
    REQUIRE(!small.IsNull() && !big.IsNull());
    REQUIRE(world.With<PostProcessComponent>(small, [](PostProcessComponent& c) noexcept {
        return c.SetSettings(BloomSettings(2.0f));
    }));
    REQUIRE(world.With<PostProcessComponent>(
        big, [](PostProcessComponent& c) noexcept { return c.SetSettings(BloomSettings(7.0f)); }));
    PostProcessSettings nested;
    auto applied = BlendPostProcessVolumes(world, fixture.allocator, Vec3d(0.0, 0.0, 0.0), nested);
    REQUIRE(applied);
    CHECK(applied.Value() == 2);
    CHECK(nested.bloomIntensity == 2.0f);

    // 속성이 같은 두 전역 볼륨: handle(slot, generation)이 뒤인 것이 마지막에 덮는다.
    VolumeFixture tie;
    REQUIRE(tie.registered);
    World& tieWorld = *tie.world;
    REQUIRE(tieWorld.Initialize());
    const ObjectHandle first = tie.Add<PostProcessComponent>(tie.Spawn());
    const ObjectHandle second = tie.Add<PostProcessComponent>(tie.Spawn());
    REQUIRE(!first.IsNull() && !second.IsNull());
    REQUIRE(tieWorld.With<PostProcessComponent>(first, [](PostProcessComponent& c) noexcept {
        return c.SetSettings(BloomSettings(3.0f));
    }));
    REQUIRE(tieWorld.With<PostProcessComponent>(second, [](PostProcessComponent& c) noexcept {
        return c.SetSettings(BloomSettings(4.0f));
    }));
    const bool secondLater =
        first.slot != second.slot ? first.slot < second.slot : first.generation < second.generation;
    PostProcessSettings tied;
    REQUIRE(BlendPostProcessVolumes(tieWorld, tie.allocator, Vec3d(0.0, 0.0, 0.0), tied));
    CHECK(tied.bloomIntensity == (secondLater ? 4.0f : 3.0f));
    REQUIRE(tieWorld.Shutdown());

    // 컴포넌트 20개 Actor의 마지막 컴포넌트가 볼륨이다(16개 묶음을 넘는 경로).
    const ObjectHandle crowded = fixture.Spawn();
    for (int index = 0; index < 19; ++index) {
        REQUIRE(!fixture.Add<SceneComponent>(crowded).IsNull());
    }

    const ObjectHandle last = fixture.Add<PostProcessComponent>(crowded);
    REQUIRE(world.With<PostProcessComponent>(last, [](PostProcessComponent& c) noexcept {
        if (auto set = c.SetPriority(5.0f); !set) {
            return set;
        }

        return c.SetSettings(BloomSettings(9.0f));
    }));
    PostProcessSettings crowd;
    auto crowdApplied =
        BlendPostProcessVolumes(world, fixture.allocator, Vec3d(0.0, 0.0, 0.0), crowd);
    REQUIRE(crowdApplied);
    CHECK(crowdApplied.Value() == 3);
    CHECK(crowd.bloomIntensity == 9.0f);
    REQUIRE(world.Shutdown());
}

} // namespace world_post_process_volume_tests_file
