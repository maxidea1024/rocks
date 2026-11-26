#include <Base/InitializerList.h>
#include <Collision/BodySettings.generated.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Math/Rotator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Material/Material.generated.h>
#include <Mesh/StaticMesh/StaticMesh.generated.h>
#include <Meta/TypeSchema.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <TestSupport/Test.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>
#include <WorldIntegration/Actors/StaticMeshActor.generated.h>
#include <WorldIntegration/Actors/StaticMeshActor.h>
#include <WorldIntegration/Components/MeshComponent.generated.h>
#include <WorldIntegration/Components/MeshComponent.h>
#include <WorldIntegration/Components/StaticMeshComponent.generated.h>
#include <WorldIntegration/Components/StaticMeshComponent.h>
#include <WorldIntegration/WorldIntegrationTypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// 메시 컴포넌트 데이터 D2b 시험이다(Docs/Design/EngineRuntime 30.7절): 기본값, 머티리얼 setter,
// StaticMeshComponent setter와 SetStaticMesh Mobility, 로드 정보 bounds(UE CalcBounds 손계산),
// StaticMeshActor, 리플렉션 형태와 저장 왕복(soft 참조).
using namespace rocks;
namespace static_mesh_component_tests_file {

namespace {

constexpr double Tolerance = 1.0e-4;
constexpr AssetId ChairAsset{Guid{0x4348414952000000ull, 1}};
constexpr AssetId RedAsset{Guid{0x5245440000000000ull, 2}};
constexpr AssetId GlowAsset{Guid{0x474c4f5700000000ull, 3}};

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

template <class T>
SoftObjectRef<T> Soft(Allocator& allocator, AssetId asset, StringView path = {}) noexcept {
    SoftObjectRef<T> value(allocator);
    CHECK(value.TryAssign(asset, path));
    return value;
}

// StaticComponentDataChanged Warning 수를 센다(Manual 모드 LogSystem에 붙여 Pump로 받는다).
class DiagnosticCounter final : public RuntimeLogSink {
public:
    Result<void> Write(const LogEvent& event) noexcept override {
        const char* needle = "(StaticComponentDataChanged)";
        const usize needleBytes = strlen(needle);
        const StringView text = event.message;
        for (usize start = 0;
             event.level == LogLevel::Warning && start + needleBytes <= text.ByteCount(); ++start) {
            if (memcmp(text.Data() + start, needle, needleBytes) == 0) {
                ++dataChanged;
                break;
            }
        }

        return Result<void>::Success();
    }

    uint32 dataChanged = 0;
};

class EmptyActor final : public Actor {
    ROCKS_NATIVE_TYPE(EmptyActor, Actor, 0x5e3a91c7d24b0f68ull, 0x8c1d4e7a2b9f3065ull)

public:
    explicit EmptyActor(Allocator& allocator) noexcept : Actor(allocator) {
    }
};

struct Fixture {
    explicit Fixture(WorldKind kind = WorldKind::Game) noexcept
        : types(allocator), modules(types, allocator) {
        registered = RegisterWorldIntegrationTypes(modules).IsOk();
        if (registered) {
            auto token = RegisterNativeClass<EmptyActor>(types);
            registered = token.IsOk();
            if (token) {
                emptyToken = token.Value();
            }
        }

        WorldDesc desc;
        desc.name = "Meshes";
        desc.context = {0x4d455348ull, 1};
        desc.timeStep = {1.0 / 60.0, 0.25, 8};
        desc.kind = kind;
        auto created = UniquePtr<World>::TryMake(allocator, allocator, types, desc);
        if (created) {
            world = Move(created.Value());
        }
    }

    ~Fixture() {
        world.Reset();
        if (registered) {
            CHECK(types.Unregister(emptyToken).IsOk());
        }

        CHECK(modules.Unregister().IsOk());
    }

    ObjectHandle AddMesh(ObjectHandle& actor) noexcept {
        if (actor.IsNull()) {
            auto spawned = world->SpawnActor<EmptyActor>();
            if (!spawned) {
                return {};
            }

            actor = spawned.Value();
        }

        auto added = world->AddComponent<StaticMeshComponent>(actor);
        return added ? added.Value() : ObjectHandle{};
    }

    ObjectHandle RootOf(ObjectHandle actor) noexcept {
        auto root =
            world->With<Actor>(actor, [](Actor& value) noexcept { return value.RootComponent(); });
        return root ? root.Value() : ObjectHandle{};
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    TypeRegistrationToken emptyToken{};
    bool registered = false;
    UniquePtr<World> world;
};

// 에셋 Registry의 StaticMesh다(로드 정보 원본 — 30.3절). 에셋 객체는 World 접근 범위(With) 밖에서
// Borrow를 쥐고 읽는다(다른 Registry).
struct LoadedMesh {
    explicit LoadedMesh(Fixture& fixture) noexcept
        : registry(fixture.allocator, fixture.types, {0x4c4f4144ull, 1}) {
        auto made = registry.NewObject<StaticMesh>(registry.OwnerAccess(), TypeIdOf<StaticMesh>());
        if (made) {
            handle = made.Value().Handle();
            root = Move(made.Value());
        }
    }

    ~LoadedMesh() noexcept {
        root.Reset();
        CHECK(registry.Shutdown(registry.OwnerAccess()));
    }

    // 쓰기 Borrow다(부르는 쪽이 쓰는 동안 쥔다).
    Result<Borrow<StaticMesh>> Write() noexcept {
        return registry.ResolveForWrite<StaticMesh>(registry.OwnerAccess(), handle);
    }

    // 머티리얼 슬롯 count개를 더한다(StructRefArray 변경은 owner Borrow를 쥔 채 한다).
    bool AddSlots(usize count) noexcept {
        auto borrowed = Write();
        StaticMesh* mesh = borrowed ? Get(borrowed.Value()) : nullptr;
        for (usize index = 0; mesh != nullptr && index < count; ++index) {
            if (!registry.AddElement(registry.OwnerAccess(), handle, mesh->materialSlots)) {
                return false;
            }
        }

        return mesh != nullptr;
    }

    ObjectRegistry registry;
    ObjectHandle handle{};
    RootRef<StaticMesh> root;
};

} // namespace

// 30.2·30.3절 표: 기본값(UE 생성자) — 메시 계열 그림자·occluder 켬, profile "BlockAllDynamic".
TEST(MeshComponent_DefaultsMatchUe) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    ObjectHandle actor;
    const ObjectHandle mesh = fixture.AddMesh(actor);
    REQUIRE(!mesh.IsNull());
    CHECK(world.With<StaticMeshComponent>(mesh, [](StaticMeshComponent& component) noexcept {
        // MeshComponent(UE MeshComponent.cpp:27).
        CHECK(component.CastsShadow() && component.UsesAsOccluder());
        CHECK(component.NumOverrideMaterials() == 0 && component.OverlayMaterial().IsNull());
        CHECK(component.OverlayMaterialMaxDrawDistance() == 0.0f);
        CHECK(component.NumSlotOverlayMaterials() == 0 && component.NumMaterials() == 0);
        CHECK(component.OverrideMaterial(0) == nullptr &&
              component.SlotOverlayMaterial(0) == nullptr);
        // StaticMeshComponent(UE StaticMeshComponent.cpp:200–231).
        CHECK(component.CollisionProfileName() == StringView("BlockAllDynamic"));
        CHECK(component.GetStaticMesh().IsNull() && !component.HasLoadedMeshInfo());
        CHECK(component.ForcedLodModel() == 0 && component.MinLod() == 0 &&
              !component.OverridesMinLod());
        CHECK(component.WireframeColorOverride() == Color(255, 255, 255, 255) &&
              !component.OverridesWireframeColor());
        CHECK(component.WorldPositionOffsetDisableDistance() == 0);
        CHECK(component.EvaluatesWorldPositionOffset() &&
              component.WorldPositionOffsetWritesVelocity() &&
              !component.EvaluatesWorldPositionOffsetInRayTracing());
        CHECK(!component.ForcesNaniteForMasked() && !component.DisallowsNanite() &&
              !component.ForcesDisableNanite() &&
              component.NanitePixelProgrammableDistance() == 0.0f);
        CHECK(!component.OverridesLightMapRes() && component.OverriddenLightMapRes() == 64);
        CHECK(!component.CastsDistanceFieldIndirectShadow() &&
              component.DistanceFieldIndirectShadowMinVisibility() == 0.1f);
        CHECK(!component.OverridesDistanceFieldSelfShadowBias() &&
              component.DistanceFieldSelfShadowBias() == 0.0f);
        CHECK(component.UsesSubDivisions() && component.SubDivisionStepSize() == 32);
        CHECK(!component.UsesDefaultCollision() && !component.SortsTriangles() &&
              !component.ReversesCulling() && !component.IgnoresInstanceForTextureStreaming());
        CHECK(component.StreamingDistanceMultiplier() == 1.0f);
        CHECK(component.Mobility() == ComponentMobility::Movable);
    }));
    REQUIRE(world.Shutdown());
}

// 30.2절: 머티리얼 setter — null로 늘리기, 같은 값 무변경, 범위 밖 index, 잘못된 soft 값 유지,
// overlay 같은 값·null로 되돌리기, 슬롯 overlay(로드 정보 없음 = 슬롯 상한까지, 있음 = 늘릴 때만
// 슬롯 수 미만 — 있는 원소는 그대로 쓴다), 거리 검증. Mobility를 보지 않는다(Static에서도 된다).
TEST(MeshComponent_MaterialSetters) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    LoadedMesh loaded(fixture);
    REQUIRE(loaded.AddSlots(2));
    auto assetBorrow = loaded.Write();
    REQUIRE(assetBorrow);
    StaticMesh* asset = Get(assetBorrow.Value());
    REQUIRE(asset != nullptr);
    ObjectHandle actor;
    const ObjectHandle mesh = fixture.AddMesh(actor);
    REQUIRE(!mesh.IsNull());
    Allocator& allocator = fixture.allocator;
    CHECK(world.With<StaticMeshComponent>(mesh, [&](StaticMeshComponent& component) noexcept {
        // 로드 정보를 붙이려면 메시를 먼저 정한다(Static이 되기 전 — 게임 초기화 뒤 Static은
        // 막힌다).
        REQUIRE(component.SetStaticMesh(Soft<StaticMesh>(allocator, ChairAsset)));
        component.SetMobility(ComponentMobility::Static);
        const SoftObjectRef<MaterialBase> red = Soft<MaterialBase>(allocator, RedAsset);
        REQUIRE(component.SetMaterial(2, red));
        REQUIRE(component.NumOverrideMaterials() == 3);
        CHECK(component.OverrideMaterial(0)->IsNull() && component.OverrideMaterial(1)->IsNull());
        CHECK(*component.OverrideMaterial(2) == red && component.OverrideMaterial(3) == nullptr);
        CHECK(component.OverrideMaterial(-1) == nullptr);
        CHECK(component.SetMaterial(2, red) && component.NumOverrideMaterials() == 3);
        CHECK(ErrorOf(component.SetMaterial(-1, red)) == ErrorCode::InvalidArgument);
        CHECK(ErrorOf(component.SetMaterial(MeshComponent::MaxMaterialSlots, red)) ==
              ErrorCode::InvalidArgument);
        // asset이 0인데 경로가 있는 값은 거부하고 배열을 늘리지 않는다.
        SoftObjectRef<MaterialBase> broken(allocator);
        CHECK(broken.objectPath.TryAssign("Orphan"));
        CHECK(ErrorOf(component.SetMaterial(5, broken)) == ErrorCode::InvalidArgument);
        CHECK(component.NumOverrideMaterials() == 3);
        // null로 되돌리기.
        CHECK(component.SetMaterial(2, SoftObjectRef<MaterialBase>(allocator)));
        CHECK(component.OverrideMaterial(2)->IsNull());

        const SoftObjectRef<MaterialBase> glow = Soft<MaterialBase>(allocator, GlowAsset, "Glow");
        CHECK(component.SetOverlayMaterial(glow) && component.OverlayMaterial() == glow);
        CHECK(component.SetOverlayMaterial(glow) && component.OverlayMaterial() == glow);
        CHECK(ErrorOf(component.SetOverlayMaterial(broken)) == ErrorCode::InvalidArgument);
        CHECK(component.OverlayMaterial() == glow);
        CHECK(component.SetOverlayMaterial(SoftObjectRef<MaterialBase>(allocator)));
        CHECK(component.OverlayMaterial().IsNull());

        // 로드 정보가 없으면 메시 슬롯 상한까지 늘린다.
        CHECK(component.SetSlotOverlayMaterial(4, glow) &&
              component.NumSlotOverlayMaterials() == 5);
        CHECK(ErrorOf(component.SetSlotOverlayMaterial(-1, glow)) == ErrorCode::InvalidArgument);
        CHECK(ErrorOf(component.SetSlotOverlayMaterial(MeshComponent::MaxMaterialSlots, glow)) ==
              ErrorCode::InvalidArgument);
        CHECK(ErrorOf(component.SetSlotOverlayMaterial(7, broken)) == ErrorCode::InvalidArgument);
        CHECK(component.NumSlotOverlayMaterials() == 5);
        CHECK(component.SlotOverlayMaterial(-1) == nullptr &&
              component.SlotOverlayMaterial(5) == nullptr);
        // 로드 정보(슬롯 2개)가 있으면: 있는 원소(2–4)는 그대로 쓰고, 늘릴 때만 슬롯 수 미만이다.
        REQUIRE(component.ApplyLoadedMeshInfo(component.GetStaticMesh(), *asset));
        CHECK(component.NumMaterials() == 2);
        CHECK(component.SetSlotOverlayMaterial(1, glow));
        CHECK(component.SetSlotOverlayMaterial(3, glow));
        CHECK(ErrorOf(component.SetSlotOverlayMaterial(5, glow)) == ErrorCode::InvalidArgument);
        CHECK(component.NumSlotOverlayMaterials() == 5);
        CHECK(*component.SlotOverlayMaterial(1) == glow &&
              component.SlotOverlayMaterial(0)->IsNull());

        CHECK(component.SetOverlayMaterialMaxDrawDistance(1500.0f));
        CHECK(ErrorOf(component.SetOverlayMaterialMaxDrawDistance(-1.0f)) ==
              ErrorCode::InvalidArgument);
        CHECK(ErrorOf(component.SetOverlayMaterialMaxDrawDistance(nanf(""))) ==
              ErrorCode::InvalidArgument);
        CHECK(component.OverlayMaterialMaxDrawDistance() == 1500.0f);
    }));
    REQUIRE(world.Shutdown());
}

// 30.3절: setter 검증(음수·비유한·[0, 1]·UE 편집 뒤 clamp 범위 — lightmap [4, 4096]의 4의 배수,
// step [1, 128])과 값 유지, profile setter가 useDefaultCollision을 끈다.
TEST(MeshComponent_StaticMeshSettersValidate) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    ObjectHandle actor;
    const ObjectHandle mesh = fixture.AddMesh(actor);
    REQUIRE(!mesh.IsNull());
    CHECK(world.With<StaticMeshComponent>(mesh, [](StaticMeshComponent& component) noexcept {
        const ErrorCode invalid = ErrorCode::InvalidArgument;
        CHECK(component.SetForcedLodModel(3) && component.ForcedLodModel() == 3);
        CHECK(ErrorOf(component.SetForcedLodModel(-1)) == invalid);
        CHECK(component.ForcedLodModel() == 3);
        CHECK(component.SetMinLod(1) && ErrorOf(component.SetMinLod(-2)) == invalid);
        CHECK(component.MinLod() == 1);
        CHECK(component.SetWorldPositionOffsetDisableDistance(5000));
        CHECK(ErrorOf(component.SetWorldPositionOffsetDisableDistance(-1)) == invalid);
        CHECK(component.WorldPositionOffsetDisableDistance() == 5000);
        CHECK(component.SetNanitePixelProgrammableDistance(100.0f));
        CHECK(ErrorOf(component.SetNanitePixelProgrammableDistance(-0.5f)) == invalid);
        CHECK(ErrorOf(component.SetNanitePixelProgrammableDistance(INFINITY)) == invalid);
        CHECK(component.NanitePixelProgrammableDistance() == 100.0f);
        // lightmap 해상도(UE Max((v + 3) & ~3, 4)·ClampMax 4096 — Rocks는 거부).
        CHECK(component.SetOverriddenLightMapRes(4) && component.SetOverriddenLightMapRes(4096));
        CHECK(component.SetOverriddenLightMapRes(128));
        for (int32 bad : {0, 2, 5, 130, 4100, 8192, -4}) {
            CHECK(ErrorOf(component.SetOverriddenLightMapRes(bad)) == invalid);
        }

        CHECK(component.OverriddenLightMapRes() == 128);
        CHECK(component.SetDistanceFieldIndirectShadowMinVisibility(1.0f));
        CHECK(ErrorOf(component.SetDistanceFieldIndirectShadowMinVisibility(1.01f)) == invalid);
        CHECK(ErrorOf(component.SetDistanceFieldIndirectShadowMinVisibility(-0.01f)) == invalid);
        CHECK(ErrorOf(component.SetDistanceFieldIndirectShadowMinVisibility(nanf(""))) == invalid);
        CHECK(component.DistanceFieldIndirectShadowMinVisibility() == 1.0f);
        CHECK(component.SetDistanceFieldSelfShadowBias(2.0f));
        CHECK(ErrorOf(component.SetDistanceFieldSelfShadowBias(-1.0f)) == invalid);
        CHECK(ErrorOf(component.SetDistanceFieldSelfShadowBias(nanf(""))) == invalid);
        CHECK(component.DistanceFieldSelfShadowBias() == 2.0f);
        // 세분 step(UE Clamp(1, 128)).
        CHECK(component.SetSubDivisionStepSize(1) && component.SetSubDivisionStepSize(128));
        CHECK(ErrorOf(component.SetSubDivisionStepSize(0)) == invalid);
        CHECK(ErrorOf(component.SetSubDivisionStepSize(129)) == invalid);
        CHECK(component.SubDivisionStepSize() == 128);
        CHECK(component.SetStreamingDistanceMultiplier(0.0f));
        CHECK(ErrorOf(component.SetStreamingDistanceMultiplier(-1.0f)) == invalid);
        CHECK(ErrorOf(component.SetStreamingDistanceMultiplier(nanf(""))) == invalid);
        CHECK(component.StreamingDistanceMultiplier() == 0.0f);
        component.SetReverseCulling(true);
        component.SetOverrideMinLod(true);
        component.SetDisallowNanite(true);
        component.SetWireframeColorOverride(Color(1, 2, 3, 4));
        CHECK(component.ReversesCulling() && component.OverridesMinLod() &&
              component.DisallowsNanite());
        CHECK(component.WireframeColorOverride() == Color(1, 2, 3, 4));
        // profile을 바꾸면 메시 기본 충돌을 끈다(UE SetCollisionProfileName —
        // StaticMeshComponent.cpp:2621).
        component.SetUseDefaultCollision(true);
        CHECK(component.UsesDefaultCollision());
        CHECK(component.SetCollisionProfileName(StringView("OverlapAll")));
        CHECK(!component.UsesDefaultCollision());
    }));
    REQUIRE(world.Shutdown());
}

// 30.3절 Mobility: 게임·Tryout World 초기화 뒤 Static은 SetStaticMesh를 거부(진단과 함께)하고
// Stationary는 막지 않으며(UE AreDynamicDataChangesAllowed(true)) 다른 setter도 막지 않는다. 편집
// World와 Movable은 된다.
TEST(MeshComponent_SetStaticMeshMobility) {
    for (WorldKind kind : {WorldKind::Game, WorldKind::Tryout, WorldKind::Editor}) {
        // sink는 LogSystem보다 먼저 선언한다(LogSystem이 먼저 소멸해야 한다).
        DiagnosticCounter diagnostics;
        SystemAllocator logAllocator;
        LogSystemConfig logConfig;
        logConfig.mode = LogMode::Manual;
        logConfig.globalMinimum = LogLevel::Debug;
        LogSystem logs(logAllocator, logConfig);
        REQUIRE(logs.Start() && logs.AddSink(diagnostics));
        LogInstallScope installed(logs);
        Fixture fixture(kind);
        REQUIRE(fixture.registered);
        World& world = *fixture.world;
        REQUIRE(world.Initialize());
        const bool game = kind != WorldKind::Editor;
        ObjectHandle actor;
        const ObjectHandle mesh = fixture.AddMesh(actor);
        REQUIRE(!mesh.IsNull());
        Allocator& allocator = fixture.allocator;
        CHECK(world.With<StaticMeshComponent>(mesh, [&](StaticMeshComponent& component) noexcept {
            const SoftObjectRef<StaticMesh> chair = Soft<StaticMesh>(allocator, ChairAsset);
            CHECK(component.SetStaticMesh(chair));
            // Stationary는 메시를 바꿀 수 있다.
            component.SetMobility(ComponentMobility::Stationary);
            const SoftObjectRef<StaticMesh> desk = Soft<StaticMesh>(allocator, ChairAsset, "Desk");
            CHECK(component.SetStaticMesh(desk));
            CHECK(component.SetStaticMesh(chair));
            // 같은 값은 Static이어도 성공(무변경)이다.
            component.SetMobility(ComponentMobility::Static);
            CHECK(component.SetStaticMesh(chair));
            const SoftObjectRef<StaticMesh> other =
                Soft<StaticMesh>(allocator, ChairAsset, "Other");
            CHECK(ErrorOf(component.SetStaticMesh(other)) ==
                  (game ? ErrorCode::InvalidArgument : ErrorCode::None));
            CHECK(component.GetStaticMesh() == (game ? chair : other));
            // 다른 setter는 Mobility를 보지 않는다(UE SetForcedLodModel 등).
            CHECK(component.SetForcedLodModel(2));
            CHECK(component.SetMaterial(0, Soft<MaterialBase>(allocator, RedAsset)));
            // 값 검증이 Mobility보다 먼저다(진단을 남기지 않는다).
            SoftObjectRef<StaticMesh> broken(allocator);
            CHECK(broken.objectPath.TryAssign("Orphan"));
            CHECK(ErrorOf(component.SetStaticMesh(broken)) == ErrorCode::InvalidArgument);
        }));
        (void)logs.Pump();
        CHECK(diagnostics.dataChanged == (game ? 1u : 0u));
        REQUIRE(world.Shutdown());
    }
}

// 30.3절 bounds: 로드 정보 없음 = 위치의 점, ApplyLoadedMeshInfo(확장 bounds) × boundsScale을
// 월드로 옮긴 값(UE CalcBounds 손계산), CalcBounds(다른 Transform), 요청 참조가 다르면 거부,
// SetStaticMesh와 ClearLoadedMeshInfo가 정보를 지우고 캐시를 버림.
TEST(MeshComponent_BoundsFromLoadedMeshInfo) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    LoadedMesh loaded(fixture);
    auto assetBorrow = loaded.Write();
    REQUIRE(assetBorrow);
    StaticMesh* asset = Get(assetBorrow.Value());
    REQUIRE(asset != nullptr);
    // 렌더 bounds 상자 (0, −10, 0)–(20, 10, 40), 반지름 25 + 확장 +Z 10 → 상자 (0, −10, 0)–(20, 10,
    // 50), 중심 (10, 0, 25), extent (10, 10, 25), 반지름 √825(UE CalculateExtendedBounds).
    asset->renderData.box.min = Vec3d(0.0, -10.0, 0.0);
    asset->renderData.box.max = Vec3d(20.0, 10.0, 40.0);
    asset->renderData.sphereRadius = 25.0;
    asset->positiveBoundsExtension = Vec3d(0.0, 0.0, 10.0);
    const double radius = sqrt(825.0);
    ObjectHandle actor;
    const ObjectHandle mesh = fixture.AddMesh(actor);
    REQUIRE(!mesh.IsNull());
    Allocator& allocator = fixture.allocator;
    CHECK(world.With<StaticMeshComponent>(mesh, [&](StaticMeshComponent& component) noexcept {
        REQUIRE(component.SetRelativeLocation(Vec3d(100.0, 0.0, 0.0)));
        REQUIRE(component.SetRelativeScale(Vec3d(2.0, 2.0, 2.0)));
        // 정보 없음: 위치의 점(UE 메시 없음 결과).
        const ComponentBounds empty = component.Bounds();
        CHECK(NearVector(empty.origin, 100.0, 0.0, 0.0));
        CHECK(NearVector(empty.boxExtent, 0.0, 0.0, 0.0) && empty.sphereRadius == 0.0);

        // 메시가 null이거나 요청 참조가 지금 메시와 다르면 거부한다(늦게 끝난 옛 요청).
        const SoftObjectRef<StaticMesh> chair = Soft<StaticMesh>(allocator, ChairAsset);
        CHECK(ErrorOf(component.ApplyLoadedMeshInfo(chair, *asset)) == ErrorCode::InvalidArgument);
        REQUIRE(component.SetStaticMesh(chair));
        CHECK(ErrorOf(component.ApplyLoadedMeshInfo(SoftObjectRef<StaticMesh>(allocator),
                                                    *asset)) == ErrorCode::InvalidArgument);
        CHECK(ErrorOf(component.ApplyLoadedMeshInfo(Soft<StaticMesh>(allocator, ChairAsset, "Old"),
                                                    *asset)) == ErrorCode::InvalidArgument);
        CHECK(!component.HasLoadedMeshInfo());
        REQUIRE(component.ApplyLoadedMeshInfo(chair, *asset));
        CHECK(component.HasLoadedMeshInfo() && component.LoadedMeshMaterialSlotCount() == 0);
        // 균일 scale 2: 중심 (100, 0, 0) + 2·(10, 0, 25), extent 2·(10, 10, 25), 반지름 min(2R,
        // |extent|).
        const ComponentBounds scaled = component.Bounds();
        CHECK(NearVector(scaled.origin, 120.0, 0.0, 50.0));
        CHECK(NearVector(scaled.boxExtent, 20.0, 20.0, 50.0));
        CHECK(Near(scaled.sphereRadius, 2.0 * radius));
        // boundsScale 1.5는 extent·반지름에 곱한다(중심은 그대로).
        REQUIRE(component.SetBoundsScale(1.5f));
        const ComponentBounds bigger = component.Bounds();
        CHECK(NearVector(bigger.origin, 120.0, 0.0, 50.0));
        CHECK(NearVector(bigger.boxExtent, 30.0, 30.0, 75.0));
        CHECK(Near(bigger.sphereRadius, 3.0 * radius));
        // CalcBounds(Transform)는 같은 지역 bounds를 그 Transform으로 옮긴다(위치 (0, 0, 7), scale
        // 1).
        const Transform other(Quatd::Identity(), Vec3d(0.0, 0.0, 7.0), Vec3d::One());
        const ComponentBounds moved = component.CalcBounds(other);
        CHECK(NearVector(moved.origin, 10.0, 0.0, 32.0));
        CHECK(NearVector(moved.boxExtent, 15.0, 15.0, 37.5));
        CHECK(Near(moved.sphereRadius, 1.5 * radius));
        // 회전 Yaw 90°·비균일 scale(2, 1, 1): 지역 X → 월드 +Y(UE 왼손 좌표), extent는 행 절댓값
        // 합.
        REQUIRE(component.SetBoundsScale(1.0f));
        REQUIRE(component.SetRelativeScale(Vec3d(2.0, 1.0, 1.0)));
        REQUIRE(component.SetRelativeRotation(Rotator<double>(0.0, 90.0, 0.0).ToQuaternion()));
        const ComponentBounds turned = component.Bounds();
        CHECK(NearVector(turned.origin, 100.0, 20.0, 25.0));
        CHECK(NearVector(turned.boxExtent, 10.0, 20.0, 25.0));
        // 구: min(R × 최대 열 길이 2, |extent|) — |(10, 20, 25)| = √1125 < 2R.
        CHECK(Near(turned.sphereRadius, sqrt(1125.0)));

        // 메시를 바꾸면 정보를 지우고 위치의 점으로 돌아간다.
        REQUIRE(component.SetStaticMesh(Soft<StaticMesh>(allocator, ChairAsset, "Other")));
        CHECK(!component.HasLoadedMeshInfo() && component.NumMaterials() == 0);
        CHECK(component.Bounds().sphereRadius == 0.0);
        // Clear도 캐시를 버린다(Apply 뒤 Bounds로 캐시를 채운 다음 Clear).
        REQUIRE(component.ApplyLoadedMeshInfo(component.GetStaticMesh(), *asset));
        CHECK(component.Bounds().sphereRadius > 0.0);
        component.ClearLoadedMeshInfo();
        CHECK(!component.HasLoadedMeshInfo() && component.Bounds().sphereRadius == 0.0);
    }));
    REQUIRE(world.Shutdown());
}

// 30.4절: StaticMeshActor root·profile "BlockAll"·Static·overlap 꺼짐·useDefaultCollision, 게임
// 초기화 뒤 메시 교체 거부, root 재사용·다른 타입 거부.
TEST(MeshComponent_StaticMeshActor) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto placed = world.SpawnActor<StaticMeshActor>();
    REQUIRE(placed);
    const ObjectHandle root = fixture.RootOf(placed.Value());
    CHECK(world.With<StaticMeshComponent>(root, [&](StaticMeshComponent& mesh) noexcept {
        CHECK(mesh.CollisionProfileName() == StringView("BlockAll"));
        CHECK(mesh.Mobility() == ComponentMobility::Static);
        CHECK(!mesh.GeneratesOverlapEvents() && mesh.UsesDefaultCollision());
        CHECK(ErrorOf(mesh.SetStaticMesh(Soft<StaticMesh>(fixture.allocator, ChairAsset))) ==
              ErrorCode::InvalidArgument);
    }));
    auto handle = world.With<StaticMeshActor>(placed.Value(), [&](StaticMeshActor& value) noexcept {
        return value.StaticMeshComponentHandle() == root;
    });
    CHECK(handle && handle.Value());

    // 게임 초기화 전(Deferred Spawn 중)에는 Static 메시도 정할 수 있다.
    auto deferred = world.SpawnActorDeferred<StaticMeshActor>();
    REQUIRE(deferred);
    auto preset = world.AddComponent<StaticMeshComponent>(deferred.Value());
    REQUIRE(preset);
    REQUIRE(
        world.With<StaticMeshComponent>(preset.Value(), [&](StaticMeshComponent& mesh) noexcept {
            mesh.SetMobility(ComponentMobility::Static);
            return mesh.SetStaticMesh(Soft<StaticMesh>(fixture.allocator, ChairAsset));
        }));
    REQUIRE(world.With<Actor>(deferred.Value(), [&](Actor& value) noexcept {
        return value.SetRootComponent(preset.Value());
    }));
    REQUIRE(world.FinishSpawning(deferred.Value()));
    // 같은 타입 root는 재사용한다(다시 설정하지 않는다 — 클래스 기본 profile 그대로).
    CHECK(fixture.RootOf(deferred.Value()) == preset.Value());
    CHECK(world.With<StaticMeshComponent>(preset.Value(), [](StaticMeshComponent& mesh) noexcept {
        CHECK(mesh.CollisionProfileName() == StringView("BlockAllDynamic"));
        CHECK(mesh.GetStaticMesh().asset == ChairAsset);
    }));

    // 다른 타입 root면 Spawn이 실패한다.
    auto mismatched = world.SpawnActorDeferred<StaticMeshActor>();
    REQUIRE(mismatched);
    auto other = world.AddComponent<SceneComponent>(mismatched.Value());
    REQUIRE(other);
    REQUIRE(world.With<Actor>(mismatched.Value(), [&](Actor& value) noexcept {
        return value.SetRootComponent(other.Value());
    }));
    CHECK(ErrorOf(world.FinishSpawning(mismatched.Value())) == ErrorCode::InvalidArgument);
    REQUIRE(world.Shutdown());
}

// 30.7절 6: 리플렉션 형태(Abstract·base·필드 수)와 Level 그래프 저장 왕복(soft 참조 AssetId·경로,
// override 배열의 null 원소, 전체·슬롯 overlay). 로드 정보는 저장하지 않는다.
TEST(MeshComponent_ReflectionAndSaveRoundTrip) {
    Fixture fixture(WorldKind::Editor);
    REQUIRE(fixture.registered);
    struct Expected {
        TypeId id;
        TypeId base;
        usize fields;
        bool abstract;
    };

    // 필드 수는 상속분을 포함한다(Primitive 53, Actor 4). MeshComponent가 4, StaticMeshComponent가
    // 27을 더한다.
    const Expected expected[] = {
        {ObjectTypeIdOf<MeshComponent>(), ObjectTypeIdOf<PrimitiveComponent>(), 57, true},
        {ObjectTypeIdOf<StaticMeshComponent>(), ObjectTypeIdOf<MeshComponent>(), 84, false},
        {ObjectTypeIdOf<StaticMeshActor>(), ObjectTypeIdOf<Actor>(), 4, false},
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
    LoadedMesh loaded(fixture);
    auto assetBorrow = loaded.Write();
    REQUIRE(assetBorrow);
    StaticMesh* asset = Get(assetBorrow.Value());
    REQUIRE(asset != nullptr);
    auto level = world.CreateLevel("Props");
    REQUIRE(level);
    SpawnParams params;
    params.level = level.Value();
    auto placed = world.SpawnActor<StaticMeshActor>(params);
    REQUIRE(placed);
    Allocator& allocator = fixture.allocator;
    REQUIRE(world.With<StaticMeshComponent>(
        fixture.RootOf(placed.Value()), [&](StaticMeshComponent& mesh) noexcept {
            if (auto set = mesh.SetStaticMesh(Soft<StaticMesh>(allocator, ChairAsset, "Chair_LOD"));
                !set) {
                return set;
            }

            if (auto set = mesh.SetMaterial(1, Soft<MaterialBase>(allocator, RedAsset)); !set) {
                return set;
            }

            if (auto set = mesh.SetOverlayMaterial(Soft<MaterialBase>(allocator, GlowAsset, "Rim"));
                !set) {
                return set;
            }

            if (auto set = mesh.SetSlotOverlayMaterial(0, Soft<MaterialBase>(allocator, GlowAsset));
                !set) {
                return set;
            }

            if (auto set = mesh.SetOverlayMaterialMaxDrawDistance(800.0f); !set) {
                return set;
            }

            mesh.SetReverseCulling(true);
            if (auto applied = mesh.ApplyLoadedMeshInfo(mesh.GetStaticMesh(), *asset); !applied) {
                return applied;
            }

            return mesh.SetForcedLodModel(2);
        }));

    Array<uint8> saved(allocator);
    {
        auto bytes = SaveObjectGraph(allocator, world.Objects(), world.Access(), level.Value());
        REQUIRE(bytes);
        saved = Move(bytes.Value());
    }

    {
        ObjectRegistry restored(allocator, fixture.types, {0x50524f50ull, 2});
        const auto access = restored.OwnerAccess();
        auto root = LoadObjectGraph(allocator, restored, fixture.types, access, saved.View());
        REQUIRE(root);
        ObjectHandle meshHandle;
        {
            auto borrowed = restored.Resolve<Level>(access, root.Value().Handle());
            REQUIRE(borrowed);
            Level* pointer = Get(borrowed.Value());
            REQUIRE(pointer != nullptr && pointer->Actors().Size() == 1);
            auto actor = restored.Resolve<StaticMeshActor>(access, pointer->Actors()[0]);
            REQUIRE(actor);
            StaticMeshActor* actorPointer = Get(actor.Value());
            REQUIRE(actorPointer != nullptr);
            meshHandle = actorPointer->StaticMeshComponentHandle();
        }

        auto borrowed = restored.Resolve<StaticMeshComponent>(access, meshHandle);
        REQUIRE(borrowed);
        StaticMeshComponent* mesh = Get(borrowed.Value());
        REQUIRE(mesh != nullptr);
        CHECK(mesh->GetStaticMesh().asset == ChairAsset &&
              mesh->GetStaticMesh().objectPath.View() == StringView("Chair_LOD"));
        REQUIRE(mesh->NumOverrideMaterials() == 2);
        CHECK(mesh->OverrideMaterial(0)->IsNull() && mesh->OverrideMaterial(1)->asset == RedAsset);
        CHECK(mesh->OverlayMaterial().asset == GlowAsset &&
              mesh->OverlayMaterial().objectPath.View() == StringView("Rim"));
        CHECK(mesh->NumSlotOverlayMaterials() == 1 &&
              mesh->SlotOverlayMaterial(0)->asset == GlowAsset);
        CHECK(mesh->OverlayMaterialMaxDrawDistance() == 800.0f);
        CHECK(mesh->ReversesCulling() && mesh->ForcedLodModel() == 2);
        CHECK(mesh->Mobility() == ComponentMobility::Static);
        CHECK(mesh->CollisionProfileName() == StringView("BlockAll"));
        CHECK(mesh->UsesDefaultCollision());
        CHECK(!mesh->HasLoadedMeshInfo());
    }

    REQUIRE(world.Shutdown());
}

// 할당 실패의 값 유지는 시험하지 않는다: World 컴포넌트 시험에는 실패 주입 할당기 경로가 없다(같은
// 규칙의 D1–D5 시험도 같다). 실패 보장은 AssignSlot이 할당을 먼저 마치는 순서로 지킨다.

} // namespace static_mesh_component_tests_file
