#include <Base/InitializerList.h>
#include <Core/Math/Rotator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Meta/TypeSchema.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Collision/BodySettings.generated.h>
#include <Collision/CollisionSettings.generated.h>
#include <Collision/CollisionTypes.generated.h>
#include <TestSupport/Test.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/Lights/LightingChannels.generated.h>
#include <World/Primitives/PrimitiveTypes.generated.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/Primitives/PrimitiveTypes.h>
#include <World/Primitives/PrimitiveComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/Primitives/ShapeComponent.generated.h>
#include <World/Primitives/BoxComponent.generated.h>
#include <World/Primitives/SphereComponent.generated.h>
#include <World/Primitives/CapsuleComponent.generated.h>
#include <World/Primitives/ShapeComponent.h>
#include <World/Primitives/BoxComponent.h>
#include <World/Primitives/SphereComponent.h>
#include <World/Primitives/CapsuleComponent.h>
#include <World/Primitives/TriggerBase.generated.h>
#include <World/Primitives/TriggerBox.generated.h>
#include <World/Primitives/TriggerSphere.generated.h>
#include <World/Primitives/TriggerCapsule.generated.h>
#include <World/Primitives/TriggerBase.h>
#include <World/Primitives/TriggerBox.h>
#include <World/Primitives/TriggerSphere.h>
#include <World/Primitives/TriggerCapsule.h>
#include <World/World.h>
#include <math.h>
#include <stdio.h>

// 컴포넌트 데이터 먼저 D2 시험이다(Docs/Design/EngineRuntime 24.3.9절): PrimitiveComponent·Shape
// 기본값, setter 검증(Mobility 무관), 충돌 profile, Shape 기하·bounds(UE TransformBy 대조), Trigger
// Actor, 리플렉션·저장 왕복.
using namespace rocks;
namespace world_primitive_component_tests_file {

namespace {

constexpr double Tolerance = 1.0e-4;

bool Near(double value, double expected) noexcept {
    const bool same = fabs(value - expected) <= Tolerance;
    if (!same) {
        printf("  value %.9g, expected %.9g\n", value, expected);
    }

    return same;
}

bool NearVector(const Vec3d& value, double x, double y, double z) noexcept {
    return Near(value.x, x) && Near(value.y, y) && Near(value.z, z);
}

ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

template <class T>
T* Get(Borrow<T>& borrow) noexcept {
    auto pointer = borrow.Get();
    return pointer ? pointer.Value() : nullptr;
}

class EmptyActor final : public Actor {
    ROCKS_NATIVE_TYPE(EmptyActor, Actor, 0x2b7e91c4d05a3f68ull, 0x9c04f3a1e7b25d86ull)

public:
    explicit EmptyActor(Allocator& allocator) noexcept : Actor(allocator) {
    }
};

struct Fixture {
    explicit Fixture(WorldKind kind = WorldKind::Game) noexcept
        : types(allocator), modules(types, allocator) {
        registered = RegisterWorldTypes(modules).IsOk() && Register<EmptyActor>();
        WorldDesc desc;
        desc.name = "Primitives";
        desc.context = {0x5052494dull, 1};
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

    // 빈 Actor(처음 부를 때 만든다)에 T를 붙인다.
    template <class T>
    ObjectHandle Add(ObjectHandle& actor) noexcept {
        if (actor.IsNull()) {
            auto spawned = world->SpawnActor<EmptyActor>();
            if (!spawned) {
                return {};
            }

            actor = spawned.Value();
        }

        auto added = world->AddComponent<T>(actor);
        return added ? added.Value() : ObjectHandle{};
    }

    ObjectHandle RootOf(ObjectHandle actor) noexcept {
        auto root =
            world->With<Actor>(actor, [](Actor& value) noexcept { return value.RootComponent(); });
        return root ? root.Value() : ObjectHandle{};
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

} // namespace

// 24.3.3·24.3.6절: Primitive·Shape 기본값(UE 생성자). 추상 타입은 만들 수 없다.
TEST(Primitive_DefaultsMatchUe) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    ObjectHandle actor;
    const ObjectHandle box = fixture.Add<BoxComponent>(actor);
    const ObjectHandle sphere = fixture.Add<SphereComponent>(actor);
    const ObjectHandle capsule = fixture.Add<CapsuleComponent>(actor);
    REQUIRE(!box.IsNull() && !sphere.IsNull() && !capsule.IsNull());

    auto boxChecked = world.With<BoxComponent>(box, [](BoxComponent& shape) noexcept {
        // PrimitiveComponent(UE PrimitiveComponent.cpp:331–439).
        CHECK(shape.MinDrawDistance() == 0.0f && shape.DesiredMaxDrawDistance() == 0.0f);
        CHECK(!shape.NeverDistanceCull() && shape.AllowsCullDistanceVolume());
        CHECK(!shape.CastsShadow());
        CHECK(shape.CastsStaticShadow() && shape.CastsContactShadow());
        CHECK(!shape.CastsFarShadow() && !shape.CastsInsetShadow() && !shape.CastsHiddenShadow());
        CHECK(!shape.CastsShadowAsTwoSided() && !shape.IsSelfShadowOnly());
        CHECK(!shape.CastsCinematicShadow());
        CHECK(!shape.CastsVolumetricTranslucentShadow());
        CHECK(shape.ReceivesMobileCsmShadows() &&
              !shape.UsesSingleSampleShadowFromStationaryLights());
        CHECK(shape.AffectsDynamicIndirectLighting() && shape.AffectsDistanceFieldLighting());
        CHECK(shape.ReceivesDecals() && shape.RendersInMainPass() && shape.RendersInDepthPass());
        CHECK(shape.IsVisibleInReflectionCaptures() && shape.IsVisibleInRealTimeSkyCaptures());
        CHECK(!shape.IsVisibleInSceneCaptureOnly() && !shape.IsHiddenInSceneCapture());
        CHECK(!shape.IsOwnerNoSee() && !shape.IsOnlyOwnerSee() && !shape.IsHoldout());
        CHECK(!shape.TreatsAsBackgroundForOcclusion() && !shape.UsesAsOccluder());
        CHECK(!shape.RendersCustomDepth() && shape.CustomDepthStencilValue() == 0);
        CHECK(shape.CustomDepthStencilWriteMask() == RendererStencilMask::Default);
        CHECK(shape.TranslucencySortPriority() == 0 &&
              shape.TranslucencySortDistanceOffset() == 0.0f);
        CHECK(shape.LightingChannels() == LightingChannels{});
        CHECK(shape.BoundsScale() == 1.0f && shape.GeneratesOverlapEvents());
        CHECK(shape.CanCharacterStepUpOn() == CanBeCharacterBase::Yes);
        CHECK(shape.Mobility() == ComponentMobility::Movable);
        // ShapeComponent(UE ShapeComponent.cpp:45–67).
        CHECK(!shape.CastsDynamicShadow());
        CHECK(shape.IsHiddenInGame());
        CHECK(shape.CollisionProfileName() == StringView("OverlapAllDynamic"));
        CHECK(shape.ShapeColor() == Color(223, 149, 157, 255));
        CHECK(shape.LineThickness() == 0.0f && !shape.IsDynamicObstacle());
        CHECK(NearVector(shape.UnscaledBoxExtent(), 32.0, 32.0, 32.0));
    });
    CHECK(boxChecked);
    auto sphereChecked = world.With<SphereComponent>(sphere, [](SphereComponent& shape) noexcept {
        CHECK(shape.UnscaledSphereRadius() == 32.0f);
        CHECK(shape.ShapeColor() == Color(255, 0, 0, 255));
    });
    CHECK(sphereChecked);
    auto capsuleChecked =
        world.With<CapsuleComponent>(capsule, [](CapsuleComponent& shape) noexcept {
            CHECK(shape.UnscaledCapsuleRadius() == 22.0f);
            CHECK(shape.UnscaledCapsuleHalfHeight() == 44.0f);
            CHECK(shape.UnscaledCapsuleHalfHeightWithoutHemisphere() == 22.0f);
            CHECK(shape.ShapeColor() == Color(223, 149, 157, 255));
        });
    CHECK(capsuleChecked);

    CHECK(!world.AddComponent(actor, ObjectTypeIdOf<PrimitiveComponent>()));
    CHECK(!world.AddComponent(actor, ObjectTypeIdOf<ShapeComponent>()));
    CHECK(!world.SpawnActor(ObjectTypeIdOf<TriggerBase>()));
    REQUIRE(world.Shutdown());
}

// 24.3.5절: 값 검증과 값 유지, Mobility를 보지 않음(게임 World 초기화 뒤 Static에서도 된다).
TEST(Primitive_SettersValidateWithoutMobility) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    ObjectHandle actor;
    const ObjectHandle box = fixture.Add<BoxComponent>(actor);
    REQUIRE(!box.IsNull());
    const float nan = NAN;
    const float infinity = INFINITY;
    auto checked = world.With<BoxComponent>(box, [&](BoxComponent& shape) noexcept {
        shape.SetMobility(ComponentMobility::Static);
        // 게임 World 초기화 뒤 Static이다(이동은 거부된다 — 아래 setter는 그래도 된다).
        CHECK(ErrorOf(shape.SetRelativeLocation(Vec3d(1.0, 0.0, 0.0))) ==
              ErrorCode::InvalidArgument);
        // 거리.
        CHECK(ErrorOf(shape.SetMinDrawDistance(-1.0f)) == ErrorCode::InvalidArgument);
        CHECK(ErrorOf(shape.SetDesiredMaxDrawDistance(nan)) == ErrorCode::InvalidArgument);
        CHECK(shape.MinDrawDistance() == 0.0f && shape.DesiredMaxDrawDistance() == 0.0f);
        CHECK(shape.SetMinDrawDistance(100.0f) && shape.SetDesiredMaxDrawDistance(5000.0f));
        CHECK(shape.EffectiveMaxDrawDistance() == 5000.0f);
        shape.SetNeverDistanceCull(true);
        CHECK(shape.EffectiveMaxDrawDistance() == 0.0f);
        CHECK(shape.DesiredMaxDrawDistance() == 5000.0f);
        // stencil·enum.
        CHECK(ErrorOf(shape.SetCustomDepthStencilValue(256)) == ErrorCode::InvalidArgument);
        CHECK(ErrorOf(shape.SetCustomDepthStencilValue(-1)) == ErrorCode::InvalidArgument);
        CHECK(shape.SetCustomDepthStencilValue(255) && shape.CustomDepthStencilValue() == 255);
        CHECK(ErrorOf(shape.SetCustomDepthStencilWriteMask(static_cast<RendererStencilMask>(10))) ==
              ErrorCode::InvalidArgument);
        CHECK(shape.SetCustomDepthStencilWriteMask(RendererStencilMask::Mask128));
        CHECK(ErrorOf(shape.SetCanCharacterStepUpOn(static_cast<CanBeCharacterBase>(3))) ==
              ErrorCode::InvalidArgument);
        CHECK(shape.CanCharacterStepUpOn() == CanBeCharacterBase::Yes);
        CHECK(shape.SetCanCharacterStepUpOn(CanBeCharacterBase::Owner));
        // 배율·정렬.
        CHECK(ErrorOf(shape.SetBoundsScale(0.0f)) == ErrorCode::InvalidArgument);
        CHECK(ErrorOf(shape.SetBoundsScale(-2.0f)) == ErrorCode::InvalidArgument);
        CHECK(shape.BoundsScale() == 1.0f);
        CHECK(shape.SetBoundsScale(2.5f) && shape.BoundsScale() == 2.5f);
        CHECK(ErrorOf(shape.SetTranslucencySortDistanceOffset(infinity)) ==
              ErrorCode::InvalidArgument);
        CHECK(shape.SetTranslucencySortDistanceOffset(-50.0f));
        CHECK(ErrorOf(shape.SetLineThickness(-1.0f)) == ErrorCode::InvalidArgument);
        CHECK(shape.SetLineThickness(2.0f) && shape.LineThickness() == 2.0f);
        // bool·채널 setter는 Static에서도 된다(UE는 Mobility를 보지 않는다).
        shape.SetCastShadow(true);
        shape.SetOwnerNoSee(true);
        shape.SetRenderCustomDepth(true);
        shape.SetGenerateOverlapEvents(false);
        LightingChannels channels;
        channels.channel1 = true;
        shape.SetLightingChannels(channels);
        CHECK(shape.CastsShadow() && shape.IsOwnerNoSee() && shape.RendersCustomDepth());
        CHECK(!shape.GeneratesOverlapEvents() && shape.LightingChannels() == channels);
        // Shape 크기도 Mobility를 보지 않는다.
        CHECK(shape.SetBoxExtent(Vec3d(10.0, 20.0, 30.0)));
    });
    CHECK(checked);
    REQUIRE(world.Shutdown());
}

// 24.3.4절: 충돌 profile 이름 — 클래스 기본, override, 기본과 같은 이름은 빈 값.
TEST(Primitive_CollisionProfileName) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    ObjectHandle actor;
    const ObjectHandle sphere = fixture.Add<SphereComponent>(actor);
    REQUIRE(!sphere.IsNull());
    auto checked = world.With<SphereComponent>(sphere, [](SphereComponent& shape) noexcept {
        CHECK(shape.DefaultCollisionProfileName() == StringView("OverlapAllDynamic"));
        CHECK(shape.CollisionProfileName() == StringView("OverlapAllDynamic"));
        CHECK(shape.SetCollisionProfileName(StringView("BlockAllDynamic")));
        CHECK(shape.CollisionProfileName() == StringView("BlockAllDynamic"));
        CHECK(shape.SetCollisionProfileName(StringView("BlockAllDynamic")));
        CHECK(shape.SetCollisionProfileName(StringView("OverlapAllDynamic")));
        CHECK(shape.CollisionProfileName() == StringView("OverlapAllDynamic"));
        // 클래스 기본과의 비교는 ASCII 대소문자를 무시한다(UE FName).
        CHECK(shape.SetCollisionProfileName(StringView("Custom")));
        CHECK(shape.SetCollisionProfileName(StringView("overlapalldynamic")));
        CHECK(shape.CollisionProfileName() == StringView("OverlapAllDynamic"));
        // 다른 이름은 철자 그대로 둔다.
        CHECK(shape.SetCollisionProfileName(StringView("blockall")));
        CHECK(shape.CollisionProfileName() == StringView("blockall"));
        // 빈 이름도 기본으로 돌아간다.
        CHECK(shape.SetCollisionProfileName(StringView("Custom")));
        CHECK(shape.SetCollisionProfileName(StringView()));
        CHECK(shape.CollisionProfileName() == StringView("OverlapAllDynamic"));
    });
    CHECK(checked);
    REQUIRE(world.Shutdown());
}

// 24.3.6절: Shape 크기 검증, scaled 값, bounds(UE TransformBy — 회전·비균일 scale 손계산).
TEST(Primitive_ShapeGeometryAndBounds) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    ObjectHandle actor;
    const ObjectHandle box = fixture.Add<BoxComponent>(actor);
    const ObjectHandle sphere = fixture.Add<SphereComponent>(actor);
    const ObjectHandle capsule = fixture.Add<CapsuleComponent>(actor);
    REQUIRE(!box.IsNull() && !sphere.IsNull() && !capsule.IsNull());
    const double sqrt3 = sqrt(3.0);

    auto boxChecked = world.With<BoxComponent>(box, [&](BoxComponent& shape) noexcept {
        CHECK(ErrorOf(shape.SetBoxExtent(Vec3d(-1.0, 1.0, 1.0))) == ErrorCode::InvalidArgument);
        CHECK(ErrorOf(shape.SetBoxExtent(Vec3d(1.0, NAN, 1.0))) == ErrorCode::InvalidArgument);
        CHECK(NearVector(shape.UnscaledBoxExtent(), 32.0, 32.0, 32.0));
        // 위치만: (100, 0, 0) 중심, extent 32, 반지름 32√3.
        CHECK(shape.SetRelativeLocation(Vec3d(100.0, 0.0, 0.0)));
        ComponentBounds bounds = shape.Bounds();
        CHECK(NearVector(bounds.origin, 100.0, 0.0, 0.0));
        CHECK(NearVector(bounds.boxExtent, 32.0, 32.0, 32.0));
        CHECK(Near(bounds.sphereRadius, 32.0 * sqrt3));
        // yaw 45° + scale (2, 1, 1): 행0 = 2(cos45, sin45, 0), 행1 = (−sin45, cos45, 0), 행2 = +Z.
        // extent = 32(√2 + √½, √2 + √½, 1). 열 길이² = (2.5, 2.5, 1) → 반지름 = min(32√3·√2.5,
        // |extent|) = 32√3·√2.5(행 길이 최대 2를 쓰면 |extent|가 된다 — UE는 열 길이다).
        CHECK(shape.SetRelativeRotation(Rotator<double>(0.0, 45.0, 0.0).ToQuaternion()));
        CHECK(shape.SetRelativeScale(Vec3d(2.0, 1.0, 1.0)));
        bounds = shape.Bounds();
        const double spread = 32.0 * (sqrt(2.0) + sqrt(0.5));
        CHECK(NearVector(bounds.origin, 100.0, 0.0, 0.0));
        CHECK(NearVector(bounds.boxExtent, spread, spread, 32.0));
        CHECK(Near(bounds.sphereRadius, 32.0 * sqrt3 * sqrt(2.5)));
        CHECK(NearVector(shape.ScaledBoxExtent(), 64.0, 32.0, 32.0));
        // 0 scale 축 + 회전(TRS로 분해되지 않는다): 월드 행렬로 계산해 회전을 잃지 않는다.
        // scale (1, 1, 0), yaw 45° → 행0 = (√½, √½, 0), 행1 = (−√½, √½, 0), 행2 = 0.
        CHECK(shape.SetRelativeScale(Vec3d(1.0, 1.0, 0.0)));
        bounds = shape.Bounds();
        CHECK(NearVector(bounds.boxExtent, 64.0 * sqrt(0.5), 64.0 * sqrt(0.5), 0.0));
        // 음수 scale은 그 축에 부호가 남는다(UE GetScale3D — 행렬 분해는 X에 둔다).
        CHECK(shape.SetRelativeRotation(Quatd::Identity()));
        CHECK(shape.SetRelativeScale(Vec3d(1.0, 1.0, -1.0)));
        CHECK(NearVector(shape.ScaledBoxExtent(), 32.0, 32.0, -32.0));
        // 크기만 바꿔도 bounds를 다시 계산한다(Transform 변경 없이).
        CHECK(shape.SetRelativeScale(Vec3d::One()));
        CHECK(NearVector(shape.Bounds().boxExtent, 32.0, 32.0, 32.0));
        CHECK(shape.SetBoxExtent(Vec3d(1.0, 2.0, 3.0)));
        bounds = shape.Bounds();
        CHECK(NearVector(bounds.boxExtent, 1.0, 2.0, 3.0));
        CHECK(Near(bounds.sphereRadius, sqrt(14.0)));
        // Shape는 boundsScale을 곱하지 않는다(UE와 같다).
        CHECK(shape.SetBoundsScale(3.0f));
        CHECK(NearVector(shape.Bounds().boxExtent, 1.0, 2.0, 3.0));
    });
    CHECK(boxChecked);

    auto sphereChecked = world.With<SphereComponent>(sphere, [&](SphereComponent& shape) noexcept {
        CHECK(ErrorOf(shape.SetSphereRadius(-1.0f)) == ErrorCode::InvalidArgument);
        CHECK(shape.UnscaledSphereRadius() == 32.0f);
        // 최소 축 scale(절댓값): (2, −3, 4) → 2.
        CHECK(shape.SetRelativeScale(Vec3d(2.0, -3.0, 4.0)));
        CHECK(Near(shape.ScaledSphereRadius(), 64.0));
        CHECK(shape.SetRelativeScale(Vec3d::One()));
        CHECK(Near(shape.Bounds().sphereRadius, 32.0));
        CHECK(shape.SetSphereRadius(10.0f));
        const ComponentBounds bounds = shape.Bounds();
        CHECK(NearVector(bounds.boxExtent, 10.0, 10.0, 10.0));
        CHECK(Near(bounds.sphereRadius, 10.0));
    });
    CHECK(sphereChecked);

    auto capsuleChecked =
        world.With<CapsuleComponent>(capsule, [&](CapsuleComponent& shape) noexcept {
            // 반 높이는 반지름 이상으로 올린다(UE SetCapsuleSize).
            CHECK(shape.SetCapsuleSize(30.0f, 10.0f));
            CHECK(shape.UnscaledCapsuleRadius() == 30.0f &&
                  shape.UnscaledCapsuleHalfHeight() == 30.0f);
            CHECK(shape.SetCapsuleHalfHeight(50.0f) && shape.UnscaledCapsuleHalfHeight() == 50.0f);
            CHECK(shape.SetCapsuleRadius(60.0f) && shape.UnscaledCapsuleHalfHeight() == 60.0f);
            CHECK(ErrorOf(shape.SetCapsuleSize(-1.0f, 10.0f)) == ErrorCode::InvalidArgument);
            CHECK(ErrorOf(shape.SetCapsuleSize(10.0f, INFINITY)) == ErrorCode::InvalidArgument);
            CHECK(shape.UnscaledCapsuleRadius() == 60.0f);
            CHECK(shape.SetCapsuleSize(22.0f, 44.0f));
            // scaled: 반지름 × min(x, y), 반 높이 × z.
            CHECK(shape.SetRelativeScale(Vec3d(2.0, 3.0, 4.0)));
            CHECK(Near(shape.ScaledCapsuleRadius(), 44.0));
            CHECK(Near(shape.ScaledCapsuleHalfHeight(), 176.0));
            CHECK(Near(shape.ScaledCapsuleHalfHeightWithoutHemisphere(), 132.0));
            // 음수 scale은 그 축에 남는다(UE: 반지름 min(x, y) 부호 그대로, 반 높이 × z).
            CHECK(shape.SetRelativeScale(Vec3d(1.0, 1.0, -1.0)));
            CHECK(Near(shape.ScaledCapsuleRadius(), 22.0));
            CHECK(Near(shape.ScaledCapsuleHalfHeight(), -44.0));
            // pitch 90°: 지역 Z가 월드 X 쪽으로 눕는다 → extent (44, 22, 22), 반지름 44.
            CHECK(shape.SetRelativeScale(Vec3d::One()));
            CHECK(shape.SetRelativeRotation(Rotator<double>(90.0, 0.0, 0.0).ToQuaternion()));
            ComponentBounds bounds = shape.Bounds();
            CHECK(NearVector(bounds.boxExtent, 44.0, 22.0, 22.0));
            CHECK(Near(bounds.sphereRadius, 44.0));
            CHECK(shape.SetRelativeRotation(Quatd::Identity()));
            CHECK(NearVector(shape.Bounds().boxExtent, 22.0, 22.0, 44.0));
            // 크기만 바꿔도 다시 계산한다.
            CHECK(shape.SetCapsuleSize(10.0f, 30.0f));
            bounds = shape.Bounds();
            CHECK(NearVector(bounds.boxExtent, 10.0, 10.0, 30.0));
            CHECK(Near(bounds.sphereRadius, 30.0));
        });
    CHECK(capsuleChecked);
    REQUIRE(world.Shutdown());
}

// TransformComponentBounds는 shear 행렬에도 UE 식 그대로다(행 벡터 규약).
TEST(Primitive_TransformComponentBoundsMatchesUeFormula) {
    ComponentBounds local;
    local.origin = Vec3d(1.0, 2.0, 3.0);
    local.boxExtent = Vec3d(1.0, 1.0, 1.0);
    local.sphereRadius = 2.0;
    const Mat4d matrix = Mat4d::FromRows(Vec4d(1.0, 1.0, 0.0, 0.0), Vec4d(0.0, 2.0, 0.0, 0.0),
                                         Vec4d(0.0, 0.0, 3.0, 0.0), Vec4d(10.0, 0.0, 0.0, 1.0));
    const ComponentBounds world = TransformComponentBounds(local, matrix);
    // 중심 = 1·행0 + 2·행1 + 3·행2 + 행3 = (11, 5, 9).
    CHECK(NearVector(world.origin, 11.0, 5.0, 9.0));
    // extent = |행0| + |행1| + |행2| = (1, 3, 3).
    CHECK(NearVector(world.boxExtent, 1.0, 3.0, 3.0));
    // 열 길이² = (1, 5, 9) → 3 × 2 = 6, |extent| = √19 → √19(여기서는 |extent| 쪽이 작다 — 열·행
    // 구분은 Primitive_ShapeGeometryAndBounds의 yaw 45° 경우가 본다).
    CHECK(Near(world.sphereRadius, sqrt(19.0)));
}

// 24.3.7절: Trigger Actor root·크기·색·profile, 가시성·Mobility, root 재사용·거부.
TEST(Primitive_TriggerActors) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto box = world.SpawnActor<TriggerBox>();
    auto sphere = world.SpawnActor<TriggerSphere>();
    auto capsule = world.SpawnActor<TriggerCapsule>();
    REQUIRE(box && sphere && capsule);
    for (ObjectHandle actor : {box.Value(), sphere.Value(), capsule.Value()}) {
        auto checked =
            world.With<ShapeComponent>(fixture.RootOf(actor), [](ShapeComponent& shape) noexcept {
                CHECK(shape.ShapeColor() == TriggerBase::TriggerShapeColor);
                CHECK(shape.CollisionProfileName() == StringView("Trigger"));
                CHECK(shape.IsHiddenInGame());
                CHECK(shape.Mobility() == ComponentMobility::Movable);
            });
        CHECK(checked);
        auto handle = world.With<TriggerBase>(actor, [](TriggerBase& trigger) noexcept {
            return trigger.CollisionComponentHandle() == trigger.RootComponent();
        });
        CHECK(handle && handle.Value());
    }

    CHECK(world.With<BoxComponent>(fixture.RootOf(box.Value()), [](BoxComponent& shape) noexcept {
        CHECK(NearVector(shape.UnscaledBoxExtent(), 40.0, 40.0, 40.0));
    }));
    CHECK(world.With<SphereComponent>(
        fixture.RootOf(sphere.Value()),
        [](SphereComponent& shape) noexcept { CHECK(shape.UnscaledSphereRadius() == 40.0f); }));
    CHECK(world.With<CapsuleComponent>(fixture.RootOf(capsule.Value()),
                                       [](CapsuleComponent& shape) noexcept {
                                           CHECK(shape.UnscaledCapsuleRadius() == 40.0f);
                                           CHECK(shape.UnscaledCapsuleHalfHeight() == 80.0f);
                                       }));

    // 같은 타입 root는 재사용한다(다시 설정하지 않는다 — 기본 크기 그대로).
    auto reused = world.SpawnActorDeferred<TriggerSphere>();
    REQUIRE(reused);
    auto preset = world.AddComponent<SphereComponent>(reused.Value());
    REQUIRE(preset);
    REQUIRE(world.With<Actor>(reused.Value(), [&](Actor& value) noexcept {
        return value.SetRootComponent(preset.Value());
    }));
    REQUIRE(world.FinishSpawning(reused.Value()));
    CHECK(fixture.RootOf(reused.Value()) == preset.Value());
    CHECK(world.With<SphereComponent>(preset.Value(), [](SphereComponent& shape) noexcept {
        CHECK(shape.UnscaledSphereRadius() == 32.0f);
    }));

    // 다른 타입 root면 Spawn이 실패한다.
    auto mismatched = world.SpawnActorDeferred<TriggerBox>();
    REQUIRE(mismatched);
    auto other = world.AddComponent<SphereComponent>(mismatched.Value());
    REQUIRE(other);
    REQUIRE(world.With<Actor>(mismatched.Value(), [&](Actor& value) noexcept {
        return value.SetRootComponent(other.Value());
    }));
    CHECK(ErrorOf(world.FinishSpawning(mismatched.Value())) == ErrorCode::InvalidArgument);
    REQUIRE(world.Shutdown());
}

// 24.3.9절 6: 리플렉션 형태와 Level 그래프 저장 왕복(profile 문자열·enum·Vec3d·Color).
TEST(Primitive_ReflectionAndSaveRoundTrip) {
    Fixture fixture(WorldKind::Editor);
    REQUIRE(fixture.registered);
    struct Expected {
        TypeId id;
        TypeId base;
        usize fields;
        bool abstract;
    };

    // 필드 수는 상속분을 포함한다(SceneComponent 12, Actor 8 — 태그 ER 31.4절·물리 이벤트 PH4e
    // 포함). Primitive가 50(bodySettings 포함 — CD2, 물리 이벤트 PH4e·PH5d), Shape가 3, Box·Sphere
    // 가 1, Capsule이 2를 더한다.
    const Expected expected[] = {
        {ObjectTypeIdOf<PrimitiveComponent>(), ObjectTypeIdOf<SceneComponent>(), 62, true},
        {ObjectTypeIdOf<ShapeComponent>(), ObjectTypeIdOf<PrimitiveComponent>(), 65, true},
        {ObjectTypeIdOf<BoxComponent>(), ObjectTypeIdOf<ShapeComponent>(), 66, false},
        {ObjectTypeIdOf<SphereComponent>(), ObjectTypeIdOf<ShapeComponent>(), 66, false},
        {ObjectTypeIdOf<CapsuleComponent>(), ObjectTypeIdOf<ShapeComponent>(), 67, false},
        {ObjectTypeIdOf<TriggerBase>(), ObjectTypeIdOf<Actor>(), 8, true},
        {ObjectTypeIdOf<TriggerBox>(), ObjectTypeIdOf<TriggerBase>(), 8, false},
        {ObjectTypeIdOf<TriggerSphere>(), ObjectTypeIdOf<TriggerBase>(), 8, false},
        {ObjectTypeIdOf<TriggerCapsule>(), ObjectTypeIdOf<TriggerBase>(), 8, false},
    };
    for (const Expected& entry : expected) {
        auto lease = fixture.types.Acquire(entry.id);
        REQUIRE(lease);
        const TypeSchema& schema = lease.Value().Schema();
        CHECK(HasFlag(schema.flags, TypeFlags::WorldOnly));
        CHECK(HasFlag(schema.flags, TypeFlags::Abstract) == entry.abstract);
        CHECK(schema.baseType == entry.base);
        CHECK(schema.fields.Size() == entry.fields);
    }

    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto level = world.CreateLevel("Zones");
    REQUIRE(level);
    SpawnParams params;
    params.level = level.Value();
    auto zone = world.SpawnActor<TriggerBox>(params);
    REQUIRE(zone);
    auto edited =
        world.With<BoxComponent>(fixture.RootOf(zone.Value()), [](BoxComponent& shape) noexcept {
            shape.SetCastShadow(true);
            shape.SetShapeColor(Color(1, 2, 3, 4));
            if (auto set = shape.SetCustomDepthStencilWriteMask(RendererStencilMask::Mask8); !set) {
                return set;
            }

            if (auto set = shape.SetCanCharacterStepUpOn(CanBeCharacterBase::No); !set) {
                return set;
            }

            if (auto set = shape.SetDesiredMaxDrawDistance(1234.0f); !set) {
                return set;
            }

            return shape.SetBoxExtent(Vec3d(5.0, 6.0, 7.0));
        });
    REQUIRE(edited);
    // 기본 profile(빈 이름)을 따르는 Shape도 저장한다.
    auto defaulted = world.SpawnActor<Actor>(params);
    REQUIRE(defaulted);
    auto defaultSphere = world.AddComponent<SphereComponent>(defaulted.Value());
    REQUIRE(defaultSphere);
    REQUIRE(world.With<Actor>(defaulted.Value(), [&](Actor& value) noexcept {
        return value.SetRootComponent(defaultSphere.Value());
    }));
    // 사용자 지정 충돌 값을 가진 Shape도 저장한다(저장하려면 리플렉션 Actor여야 한다).
    auto plain = world.SpawnActor<Actor>(params);
    REQUIRE(plain);
    auto plainSphere = world.AddComponent<SphereComponent>(plain.Value());
    REQUIRE(plainSphere);
    REQUIRE(world.With<Actor>(plain.Value(), [&](Actor& value) noexcept {
        return value.SetRootComponent(plainSphere.Value());
    }));
    // bodySettings 저장 왕복(CD2): "Custom"으로 바꾸는 setter로 사용자 지정 칸을 채운다.
    REQUIRE(world.With<SphereComponent>(plainSphere.Value(), [](SphereComponent& shape) noexcept {
        if (auto set = shape.SetCollisionObjectType(CollisionChannel::Pawn); !set) {
            return set;
        }

        if (auto set = shape.SetCollisionEnabled(CollisionEnabled::QueryOnly); !set) {
            return set;
        }

        return shape.SetCollisionResponseToChannel(CollisionChannel::Camera,
                                                   CollisionResponse::Ignore);
    }));

    Array<uint8> saved(fixture.allocator);
    {
        auto bytes =
            SaveObjectGraph(fixture.allocator, world.Objects(), world.Access(), level.Value());
        REQUIRE(bytes);
        saved = Move(bytes.Value());
    }

    {
        ObjectRegistry loaded(fixture.allocator, fixture.types, {0x5a4f4e45ull, 2});
        const auto access = loaded.OwnerAccess();
        auto root = LoadObjectGraph(fixture.allocator, loaded, fixture.types, access, saved.View());
        REQUIRE(root);
        ObjectHandle loadedZone;
        ObjectHandle loadedPlain;
        ObjectHandle loadedDefault;
        {
            auto borrowed = loaded.Resolve<Level>(access, root.Value().Handle());
            REQUIRE(borrowed);
            Level* pointer = Get(borrowed.Value());
            REQUIRE(pointer != nullptr && pointer->Actors().Size() == 3);
            // Level actors 순서는 보장하지 않는다(7.2절) — 타입과 profile 이름으로 고른다.
            for (usize index = 0; index < 3; ++index) {
                const ObjectHandle candidate = pointer->Actors()[index];
                if (loaded.Resolve<TriggerBox>(access, candidate)) {
                    loadedZone = candidate;
                    continue;
                }

                auto actor = loaded.Resolve<Actor>(access, candidate);
                Actor* actorPointer = actor ? Get(actor.Value()) : nullptr;
                REQUIRE(actorPointer != nullptr);
                auto sphereBorrow =
                    loaded.Resolve<SphereComponent>(access, actorPointer->RootComponent());
                REQUIRE(sphereBorrow);
                SphereComponent* sphereShape = Get(sphereBorrow.Value());
                REQUIRE(sphereShape != nullptr);
                if (sphereShape->CollisionProfileName() == CustomCollisionProfileName) {
                    loadedPlain = candidate;
                } else {
                    loadedDefault = candidate;
                    CHECK(sphereShape->CollisionProfileName() == StringView("OverlapAllDynamic"));
                }
            }
        }

        REQUIRE(!loadedZone.IsNull() && !loadedPlain.IsNull() && !loadedDefault.IsNull());
        {
            auto actor = loaded.Resolve<Actor>(access, loadedPlain);
            REQUIRE(actor);
            Actor* pointer = Get(actor.Value());
            REQUIRE(pointer != nullptr);
            auto sphereBorrow = loaded.Resolve<SphereComponent>(access, pointer->RootComponent());
            REQUIRE(sphereBorrow);
            SphereComponent* sphereShape = Get(sphereBorrow.Value());
            REQUIRE(sphereShape != nullptr);
            CHECK(sphereShape->CollisionProfileName() == CustomCollisionProfileName);
            const BodySettings& body = sphereShape->GetBodySettings();
            CHECK(body.objectType == CollisionChannel::Pawn);
            CHECK(body.collisionEnabled == CollisionEnabled::QueryOnly);
            REQUIRE(body.customResponses.Size() == 1);
            CHECK(body.customResponses[0].channel.View() == StringView("Camera"));
            CHECK(body.customResponses[0].response == CollisionResponse::Ignore);
            CHECK(!body.simulatePhysics && body.enableGravity);
            // 그래프로 로드한 객체는 World 장부 밖이다: 해석·Custom setter는 InvalidArgument, 이름
            // setter는 이름만 바꾸고 body는 그대로다.
            CHECK(sphereShape->TryResolveCollision().Error() == ErrorCode::InvalidArgument);
            CHECK(sphereShape->SetCollisionObjectType(CollisionChannel::Vehicle).Error() ==
                  ErrorCode::InvalidArgument);
            CHECK(sphereShape->SetCollisionProfileName("BlockAll"));
            CHECK(sphereShape->CollisionProfileName() == StringView("BlockAll"));
            CHECK(body.objectType == CollisionChannel::Pawn);
        }

        ObjectHandle loadedShape;
        {
            auto borrowed = loaded.Resolve<TriggerBox>(access, loadedZone);
            REQUIRE(borrowed);
            TriggerBox* pointer = Get(borrowed.Value());
            REQUIRE(pointer != nullptr && pointer->Components().Size() == 1);
            loadedShape = pointer->RootComponent();
        }

        auto borrowed = loaded.Resolve<BoxComponent>(access, loadedShape);
        REQUIRE(borrowed);
        BoxComponent* shape = Get(borrowed.Value());
        REQUIRE(shape != nullptr);
        CHECK(shape->CastsShadow());
        CHECK(shape->ShapeColor() == Color(1, 2, 3, 4));
        CHECK(shape->CustomDepthStencilWriteMask() == RendererStencilMask::Mask8);
        CHECK(shape->CanCharacterStepUpOn() == CanBeCharacterBase::No);
        CHECK(shape->DesiredMaxDrawDistance() == 1234.0f);
        CHECK(NearVector(shape->UnscaledBoxExtent(), 5.0, 6.0, 7.0));
        CHECK(shape->CollisionProfileName() == StringView("Trigger"));
        CHECK(shape->IsHiddenInGame());
    }

    REQUIRE(world.Shutdown());
}

} // namespace world_primitive_component_tests_file
