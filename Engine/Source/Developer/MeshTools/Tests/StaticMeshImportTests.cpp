#include "../../../Runtime/MeshSource/Tests/MeshSourceTestSupport.h"

#include <AssetRuntime/MemoryBulkSource.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Material/Material.generated.h>
#include <Mesh/StaticMesh/StaticMesh.generated.h>
#include <MeshTools/StaticMeshImport.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/ObjectRegistryOptions.h>
#include <TestSupport/Test.h>

// 정적 메시 임포트 서비스 시험이다(Docs/Design/MeshAnimationAssets 5.5.3절): 필드
// 채우기(원본·baked· 볼록 bulk가 출력 bulk 번호로 옮겨짐 — 번호를 일부러 어긋나게 둔다,
// 슬롯·참조·섹션 정보·소켓·플랫폼 값, renderData 비움), 새 임포트는 옛 값을 남기지 않음, 재임포트는
// 사용자 편집 보존(슬롯 머티리얼· 편집기 소켓·설정, replaceSettingsOnReimport), 검증 실패의
// 코드·detail·순서(저장 전 실패라 bulk를 넘기지 않음), bulk 원본 Load의 ref 판정.
using namespace rocks;
namespace static_mesh_import_tests_file {

namespace {

// 출력 bulk 번호를 서비스 내부 번호와 어긋나게 하는 앞 entry 수다.
constexpr uint32 DummyBulkEntries = 7;

constexpr AssetId MaterialAsset{Guid{0x4d41540000000000ull, 1}};
constexpr AssetId SecondMaterialAsset{Guid{0x4d41540000000000ull, 2}};
constexpr AssetId CollisionAsset{Guid{0x434f4c0000000000ull, 3}};

// 가짜 임포트 문맥이다(출력은 미리 만든 메시, bulk는 메모리, OpenAsset은 표).
class FakeContext final : public AssetImportContext {
public:
    FakeContext(ObjectRegistry& registry, ObjectHandle output, Allocator& allocator,
                bool reimport = false) noexcept
        : bulk(allocator), registry_(registry), access_(registry.OwnerAccess()), output_(output),
          reimport_(reimport) {
        for (uint32 index = 0; index < DummyBulkEntries; ++index) {
            const uint8 filler = static_cast<uint8>(index);
            dummyAdded = bulk.AddCopy(Span<const uint8>(&filler, 1)).IsOk() && dummyAdded;
        }
    }

    Span<const AssetImportSource> Sources() const noexcept override {
        return {};
    }

    const void* Options() const noexcept override {
        return nullptr;
    }

    bool IsReimport() const noexcept override {
        return reimport_;
    }

    ObjectRegistry& Registry() noexcept override {
        return registry_;
    }

    const ObjectAccessContext& Access() const noexcept override {
        return access_;
    }

    Result<ObjectHandle> Output(StringView, TypeId) noexcept override {
        return Result<ObjectHandle>::Success(output_);
    }

    Result<BulkDataRef> AddBulk(ObjectHandle target, Array<uint8>&& raw) noexcept override {
        if (target != output_) {
            return Result<BulkDataRef>::Failure(ErrorCode::InvalidArgument);
        }

        ++addedBulks;
        return bulk.Add(Move(raw));
    }

    Result<void> Warn(AssetDiagnosticCode, uint64) noexcept override {
        return Result<void>::Success();
    }

    // 첫 진단만 남긴다(서비스는 실패마다 Fail을 한 번 부른다 — 두 번째는 결함).
    Result<void> Fail(AssetDiagnosticCode code, ErrorCode error, uint64 detail) noexcept override {
        ++failCount;
        if (failCount == 1) {
            failCode = code;
            failDetail = detail;
        }

        return Result<void>::Failure(error);
    }

    Result<void> AddTag(StringView, StringView) noexcept override {
        return Result<void>::Success();
    }

    Span<const ImportTag> PreviousTags() const noexcept override {
        return {};
    }

    // 표에 없는 id는 실제 문맥처럼 MissingDependency 진단을 남기고 NotFound다.
    Result<ObjectHandle> OpenAsset(AssetId asset) noexcept override {
        for (usize index = 0; index < count; ++index) {
            if (ids[index] == asset) {
                return Result<ObjectHandle>::Success(handles[index]);
            }
        }

        (void)Fail(AssetDiagnosticCode::MissingDependency, ErrorCode::NotFound, 0);
        return Result<ObjectHandle>::Failure(ErrorCode::NotFound);
    }

    usize GroupSize() const noexcept override {
        return 1;
    }

    usize GroupIndex() const noexcept override {
        return 0;
    }

    AssetId MemberAsset(usize) const noexcept override {
        return {};
    }

    void Add(AssetId id, ObjectHandle handle) noexcept {
        ids[count] = id;
        handles[count] = handle;
        ++count;
    }

    MemoryBulkSource bulk;
    bool dummyAdded = true;
    uint32 addedBulks = 0;
    uint32 failCount = 0;
    AssetDiagnosticCode failCode = AssetDiagnosticCode::None;
    uint64 failDetail = 0;
    AssetId ids[4];
    ObjectHandle handles[4];
    usize count = 0;

private:
    ObjectRegistry& registry_;
    ObjectAccessContext access_;
    ObjectHandle output_;
    bool reimport_;
};

struct ImportScene {
    ImportScene() noexcept : types(allocator), registration(types, allocator), names(allocator) {
        ready = RegisterMeshTypes(registration).IsOk() && names.Initialize().IsOk();
        RegistryOptions options;
        options.names = &names;
        auto made = UniquePtr<ObjectRegistry>::TryMake(allocator, allocator, types,
                                                       ContextId{0x4d455349ull, 1}, options);
        if (made) {
            registry = Move(made.Value());
        }

        ready = ready && registry.Get() != nullptr && Make<StaticMesh>(mesh, meshRoot) &&
                Make<StaticMesh>(other, otherRoot) && Make<Material>(material, materialRoot) &&
                Make<Material>(second, secondRoot);
    }

    ~ImportScene() noexcept {
        meshRoot.Reset();
        otherRoot.Reset();
        materialRoot.Reset();
        secondRoot.Reset();
        registry.Reset();
        CHECK(registration.Unregister());
    }

    template <class T, class R>
    bool Make(ObjectHandle& handle, R& root) noexcept {
        auto made = registry->NewObject<T>(registry->OwnerAccess(), TypeIdOf<T>());
        if (!made) {
            return false;
        }

        handle = made.Value().Handle();
        root = Move(made.Value());
        return true;
    }

    StaticMesh* Mesh() noexcept {
        auto borrowed = registry->ResolveForWrite<StaticMesh>(registry->OwnerAccess(), mesh);
        if (!borrowed) {
            return nullptr;
        }

        auto value = borrowed.Value().Get();
        return value ? value.Value() : nullptr;
    }

    void AddReferences(FakeContext& context) noexcept {
        context.Add(MaterialAsset, material);
        context.Add(SecondMaterialAsset, second);
        context.Add(CollisionAsset, other);
    }

    Name Intern(StringView text) noexcept {
        auto name = names.TryIntern(text);
        return name ? name.Value() : Name{};
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration registration;
    NamePool names;
    UniquePtr<ObjectRegistry> registry;
    ObjectHandle mesh{};
    ObjectHandle other{};
    ObjectHandle material{};
    ObjectHandle second{};
    RootRef<StaticMesh> meshRoot;
    RootRef<StaticMesh> otherRoot;
    RootRef<Material> materialRoot;
    RootRef<Material> secondRoot;
    bool ready = false;
};

// 정육면체 볼록 형상(점 8개, index 없음)이다.
ConvexGeometry Cube(Allocator& allocator) noexcept {
    ConvexGeometry geometry(allocator);
    for (uint32 corner = 0; corner < 8; ++corner) {
        CHECK(geometry.vertices.TryPushBack(Vec3d((corner & 1) != 0 ? 5.0 : -5.0,
                                                  (corner & 2) != 0 ? 5.0 : -5.0,
                                                  (corner & 4) != 0 ? 5.0 : -5.0)));
    }

    return geometry;
}

// 단일 LOD 명세다.
StaticMeshImportSpec Basic(const StaticMeshImportLod& lod) noexcept {
    StaticMeshImportSpec spec;
    spec.lods = Span<const StaticMeshImportLod>(&lod, 1);
    return spec;
}

} // namespace

// 필드 채우기와 bulk 왕복: 원본·baked·볼록 bulk가 출력 번호로 옮겨지고(앞 entry로 번호를 어긋나게
// 해도 출력 bulk에서 Load가 같은 메시), 슬롯·참조·섹션 정보·소켓·충돌·플랫폼 값이 채워지고
// renderData는 빈다.
TEST(StaticMeshImport_FillsFields) {
    ImportScene scene;
    REQUIRE(scene.ready);
    FakeContext context(*scene.registry, scene.mesh, scene.allocator);
    REQUIRE(context.dummyAdded);
    scene.AddReferences(context);
    MeshSource quad(scene.allocator);
    REQUIRE(mesh_source_test::BuildQuad(quad));
    MeshSource shared(scene.allocator);
    REQUIRE(mesh_source_test::BuildSharedMesh(shared));
    const StaticMeshImportFloat screens[1] = {{"Android", 0.25f}};
    StaticMeshImportLod lods[2];
    lods[0].source = &quad;
    lods[0].screenSize = 1.0f;
    lods[0].screenSizePerPlatform = Span<const StaticMeshImportFloat>(screens);
    lods[1].bakedSource = &shared;
    lods[1].reduction.percentTriangles = 0.5f;
    StaticMeshImportSlot slot;
    slot.slotName = "Body";
    slot.importedSlotName = "Body_Imported";
    slot.material = MaterialAsset;
    MeshSectionInfoEntry entry;
    entry.lod = 1;
    entry.section = 0;
    entry.info.castShadow = false;
    StaticMeshImportSocket socket;
    socket.name = "Muzzle";
    socket.tag = "barrel";
    SimpleCollision collision(scene.allocator);
    CollisionSphere sphere;
    sphere.radius = 3.0f;
    REQUIRE(collision.spheres.TryPushBack(sphere));
    REQUIRE(collision.convexes.TryPushBack(CollisionConvex{}));
    const ConvexGeometry convexes[1] = {Cube(scene.allocator)};
    const StaticMeshImportInt minLods[1] = {{"Android", 1}};
    StaticMeshImportSpec spec;
    spec.lods = Span<const StaticMeshImportLod>(lods);
    spec.slots = Span<const StaticMeshImportSlot>(&slot, 1);
    spec.sectionInfo = Span<const MeshSectionInfoEntry>(&entry, 1);
    spec.sockets = Span<const StaticMeshImportSocket>(&socket, 1);
    spec.collision = &collision;
    spec.convexes = Span<const ConvexGeometry>(convexes);
    spec.complexCollisionMesh = CollisionAsset;
    spec.lodGroup = "SmallProp";
    spec.minLodPerPlatform = Span<const StaticMeshImportInt>(minLods);
    spec.lightMapResolution = 32;
    MeshDiagnostic meshFailure;
    REQUIRE(ImportStaticMesh(context, scene.mesh, spec, &meshFailure));
    CHECK(meshFailure.code == AssetDiagnosticCode::None && context.failCount == 0);
    StaticMesh* mesh = scene.Mesh();
    REQUIRE(mesh != nullptr);
    REQUIRE(mesh->sourceLods.Size() == 2);
    const Name android = scene.Intern("android");
    CHECK(mesh->sourceLods[0].hasSource && !mesh->sourceLods[0].hasBakedSource &&
          mesh->sourceLods[0].screenSize.Resolve(android) == 0.25f &&
          mesh->sourceLods[0].screenSize.defaultValue == 1.0f);
    CHECK(!mesh->sourceLods[1].hasSource && mesh->sourceLods[1].hasBakedSource &&
          IsStaticMeshLodGenerated(mesh->sourceLods[1]));
    CHECK(mesh->sourceLods[0].source.topology.bulkId >= DummyBulkEntries);
    MeshSource readSource(scene.allocator);
    REQUIRE(readSource.Load(mesh->sourceLods[0].source, context.bulk, scene.names));
    CHECK(mesh_source_test::SameMesh(quad, readSource));
    MeshSource readBaked(scene.allocator);
    REQUIRE(readBaked.Load(mesh->sourceLods[1].bakedSource, context.bulk, scene.names));
    CHECK(mesh_source_test::SameMesh(shared, readBaked));
    REQUIRE(mesh->materialSlots.Size() == 1);
    CHECK(mesh->materialSlots[0].material.Handle() == scene.material &&
          mesh->materialSlots[0].overlayMaterial.Handle() == ObjectHandle{} &&
          mesh->materialSlots[0].slotName == scene.Intern("Body") &&
          mesh->materialSlots[0].importedSlotName == scene.Intern("Body_Imported"));
    CHECK(mesh->complexCollisionMesh.Handle() == scene.other);
    CHECK(mesh->sectionInfo.Size() == 1 && !mesh->sectionInfo[0].info.castShadow);
    REQUIRE(mesh->sockets.Size() == 1);
    CHECK(mesh->sockets[0].tag.View() == StringView("barrel") && mesh->sockets[0].createdAtImport);
    REQUIRE(mesh->simpleCollision.convexes.Size() == 1);
    const CollisionConvex& convex = mesh->simpleCollision.convexes[0];
    CHECK(convex.vertices.bulkId >= DummyBulkEntries);
    auto points = context.bulk.ReadBulk(convex.vertices);
    REQUIRE(points);
    CHECK(points.Value().Size() == 8 * sizeof(Vec3d) &&
          mesh->simpleCollision.spheres[0].radius == 3.0f);
    CHECK(mesh->minLod.Resolve(android) == 1 && mesh->lightMapResolution == 32 &&
          mesh->renderData.lods.IsEmpty() && mesh->lodGroup == scene.Intern("SmallProp"));
}

// 새 임포트(재임포트 아님)는 옛 슬롯·소켓·참조·설정을 남기지 않고 renderData를 비운다.
TEST(StaticMeshImport_NewImportReplaces) {
    ImportScene scene;
    REQUIRE(scene.ready);
    FakeContext context(*scene.registry, scene.mesh, scene.allocator);
    scene.AddReferences(context);
    MeshSource quad(scene.allocator);
    REQUIRE(mesh_source_test::BuildQuad(quad));
    StaticMeshImportLod lod;
    lod.source = &quad;
    StaticMeshImportSlot slots[2];
    slots[0].material = MaterialAsset;
    StaticMeshImportSocket socket;
    StaticMeshImportSpec spec = Basic(lod);
    spec.slots = Span<const StaticMeshImportSlot>(slots);
    spec.sockets = Span<const StaticMeshImportSocket>(&socket, 1);
    spec.complexCollisionMesh = CollisionAsset;
    spec.lightMapResolution = 8;
    spec.lodGroup = "SmallProp";
    REQUIRE(ImportStaticMesh(context, scene.mesh, spec));
    REQUIRE(scene.Mesh() != nullptr);
    CHECK(scene.Mesh()->materialSlots.Size() == 2 && scene.Mesh()->sockets.Size() == 1);
    scene.Mesh()->renderData.sphereRadius = 5.0;
    REQUIRE(ImportStaticMesh(context, scene.mesh, Basic(lod)));
    StaticMesh* mesh = scene.Mesh();
    CHECK(mesh->materialSlots.Size() == 0 && mesh->sockets.IsEmpty() &&
          mesh->sourceLods.Size() == 1);
    CHECK(mesh->complexCollisionMesh.Handle() == ObjectHandle{} &&
          mesh->lightMapResolution == StaticMeshDefaultLightMapResolution &&
          mesh->renderData.sphereRadius == 0.0 && mesh->lodGroup.IsNone());
}

// 재임포트: importedSlotName이 같은 옛 슬롯의 이름·머티리얼, 편집기 소켓, 설정을 보존한다.
// 명세가 머티리얼을 주면 명세가 이기고, replaceSettingsOnReimport면 설정도 명세 값이다.
TEST(StaticMeshImport_ReimportPreservesEdits) {
    ImportScene scene;
    REQUIRE(scene.ready);
    MeshSource quad(scene.allocator);
    REQUIRE(mesh_source_test::BuildQuad(quad));
    StaticMeshImportLod lod;
    lod.source = &quad;
    StaticMeshImportSlot slot;
    slot.slotName = "Body";
    slot.importedSlotName = "Body_Imported";
    slot.material = MaterialAsset;
    StaticMeshImportSocket socket;
    socket.name = "Imported";
    MeshSectionInfoEntry entry;
    entry.info.castShadow = false;
    StaticMeshImportSpec spec = Basic(lod);
    spec.slots = Span<const StaticMeshImportSlot>(&slot, 1);
    spec.sockets = Span<const StaticMeshImportSocket>(&socket, 1);
    spec.sectionInfo = Span<const MeshSectionInfoEntry>(&entry, 1);
    {
        FakeContext context(*scene.registry, scene.mesh, scene.allocator);
        scene.AddReferences(context);
        REQUIRE(ImportStaticMesh(context, scene.mesh, spec));
    }

    // 사용자 편집: 슬롯 이름, 편집기 소켓, 설정. 쿠킹 결과도 남아 있다고 둔다.
    StaticMesh* mesh = scene.Mesh();
    REQUIRE(mesh != nullptr);
    mesh->materialSlots[0].slotName = scene.Intern("Renamed");
    MeshSocket editor(scene.allocator);
    editor.name = scene.Intern("Editor");
    REQUIRE(mesh->sockets.TryPushBack(Move(editor)));
    mesh->lightMapResolution = 128;
    mesh->sourceLods[0].build.useFullPrecisionUVs = true;
    mesh->renderData.sphereRadius = 5.0;

    StaticMeshImportSlot reslot;
    reslot.importedSlotName = "Body_Imported";
    StaticMeshImportSocket resocket;
    resocket.name = "Imported2";
    StaticMeshImportSpec again = Basic(lod);
    again.slots = Span<const StaticMeshImportSlot>(&reslot, 1);
    again.sockets = Span<const StaticMeshImportSocket>(&resocket, 1);
    {
        FakeContext context(*scene.registry, scene.mesh, scene.allocator, true);
        scene.AddReferences(context);
        REQUIRE(ImportStaticMesh(context, scene.mesh, again));
    }

    mesh = scene.Mesh();
    REQUIRE(mesh->materialSlots.Size() == 1 && mesh->sockets.Size() == 2);
    CHECK(mesh->materialSlots[0].material.Handle() == scene.material &&
          mesh->materialSlots[0].slotName == scene.Intern("Renamed"));
    CHECK(mesh->sockets[0].name == scene.Intern("Imported2") &&
          mesh->sockets[1].name == scene.Intern("Editor") && !mesh->sockets[1].createdAtImport);
    CHECK(mesh->lightMapResolution == 128 && mesh->sourceLods[0].build.useFullPrecisionUVs);
    // 명세 섹션 정보가 비면 옛 값을 유지하고, renderData는 비운다.
    CHECK(mesh->sectionInfo.Size() == 1 && !mesh->sectionInfo[0].info.castShadow &&
          mesh->renderData.sphereRadius == 0.0);

    // 명세 머티리얼이 이기고, replaceSettingsOnReimport는 설정을 명세 값으로 바꾼다.
    reslot.material = SecondMaterialAsset;
    again.replaceSettingsOnReimport = true;
    {
        FakeContext context(*scene.registry, scene.mesh, scene.allocator, true);
        scene.AddReferences(context);
        REQUIRE(ImportStaticMesh(context, scene.mesh, again));
    }

    mesh = scene.Mesh();
    CHECK(mesh->materialSlots[0].material.Handle() == scene.second &&
          mesh->materialSlots[0].slotName == scene.Intern("Renamed"));
    CHECK(mesh->lightMapResolution == StaticMeshDefaultLightMapResolution &&
          !mesh->sourceLods[0].build.useFullPrecisionUVs);
}

// 검증 실패의 코드·detail·순서. 모두 저장 전에 실패하므로 bulk를 넘기지 않는다.
TEST(StaticMeshImport_Rejects) {
    ImportScene scene;
    REQUIRE(scene.ready);
    MeshSource quad(scene.allocator);
    REQUIRE(mesh_source_test::BuildQuad(quad));
    StaticMeshImportLod lods[StaticMeshMaxLods + 1];
    for (StaticMeshImportLod& lod : lods) {
        lod.source = &quad;
    }

    auto expect = [&](const StaticMeshImportSpec& spec, AssetDiagnosticCode code, uint64 detail,
                      ObjectHandle output) noexcept {
        FakeContext context(*scene.registry, output, scene.allocator);
        scene.AddReferences(context);
        const bool failed = !ImportStaticMesh(context, output, spec);
        return failed && context.failCount == 1 && context.failCode == code &&
               context.failDetail == detail && context.addedBulks == 0;
    };

    const ObjectHandle mesh = scene.mesh;

    // LOD 수, 원본 없는 LOD.
    StaticMeshImportSpec none;
    CHECK(expect(none, AssetDiagnosticCode::InvalidSetting, 0, mesh));
    StaticMeshImportSpec many;
    many.lods = Span<const StaticMeshImportLod>(lods);
    CHECK(expect(many, AssetDiagnosticCode::InvalidSetting, StaticMeshMaxLods + 1, mesh));
    StaticMeshImportLod twoLods[2];
    twoLods[0].source = &quad;
    StaticMeshImportSpec sourceless;
    sourceless.lods = Span<const StaticMeshImportLod>(twoLods);
    CHECK(expect(sourceless, AssetDiagnosticCode::InvalidSetting, 1, mesh));

    // 원본 검증(LOD 1 baked의 비유한 위치) — detail = LOD, meshFailure에 세부.
    MeshSource broken(scene.allocator);
    REQUIRE(mesh_source_test::BuildQuad(broken));
    broken.Positions()[0].x = mesh_source_test::TestQuietNaN();
    StaticMeshImportLod brokenLods[2];
    brokenLods[0].source = &quad;
    brokenLods[1].bakedSource = &broken;
    StaticMeshImportSpec invalidMesh;
    invalidMesh.lods = Span<const StaticMeshImportLod>(brokenLods);
    CHECK(expect(invalidMesh, AssetDiagnosticCode::InvalidMeshData, 1, mesh));
    {
        FakeContext context(*scene.registry, mesh, scene.allocator);
        MeshDiagnostic failure;
        CHECK(!ImportStaticMesh(context, mesh, invalidMesh, &failure));
        CHECK(failure.code == AssetDiagnosticCode::InvalidMeshData &&
              failure.issue == MeshIssue::NonFinitePosition);
    }

    // 섹션 정보: 원본 검증이 먼저(위반 둘이면 InvalidMeshData), 순서, 음수 section, LOD 범위.
    MeshSectionInfoEntry entries[2];
    entries[0].section = 1;
    entries[1].section = 0;
    invalidMesh.sectionInfo = Span<const MeshSectionInfoEntry>(entries);
    CHECK(expect(invalidMesh, AssetDiagnosticCode::InvalidMeshData, 1, mesh));
    StaticMeshImportSpec sections = Basic(lods[0]);
    sections.sectionInfo = Span<const MeshSectionInfoEntry>(entries);
    CHECK(expect(sections, AssetDiagnosticCode::InvalidSetting, 1, mesh));
    MeshSectionInfoEntry single;
    single.section = -1;
    sections.sectionInfo = Span<const MeshSectionInfoEntry>(&single, 1);
    CHECK(expect(sections, AssetDiagnosticCode::InvalidSetting, 0, mesh));
    single.section = 0;
    single.lod = 1;
    CHECK(expect(sections, AssetDiagnosticCode::InvalidSetting, 0, mesh));
    single.lod = -1;
    CHECK(expect(sections, AssetDiagnosticCode::InvalidSetting, 0, mesh));

    // 이름(잘못된 UTF-8)·프로파일 키(빈 키, Name 비교로 겹침).
    StaticMeshImportSlot badName;
    badName.slotName = StringView("\xFF\xFE", 2);
    StaticMeshImportSpec names = Basic(lods[0]);
    names.slots = Span<const StaticMeshImportSlot>(&badName, 1);
    CHECK(expect(names, AssetDiagnosticCode::InvalidSetting, 0, mesh));
    const StaticMeshImportInt emptyKey[1] = {{"", 1}};
    StaticMeshImportSpec profiles = Basic(lods[0]);
    profiles.minLodPerPlatform = Span<const StaticMeshImportInt>(emptyKey);
    CHECK(expect(profiles, AssetDiagnosticCode::InvalidSetting, 0, mesh));
    const StaticMeshImportInt duplicate[2] = {{"Android", 1}, {"ANDROID", 2}};
    profiles.minLodPerPlatform = Span<const StaticMeshImportInt>(duplicate);
    CHECK(expect(profiles, AssetDiagnosticCode::InvalidSetting, 0, mesh));

    // 충돌: 볼록 수 불일치, 잘못된 볼록 형상(점 3개), 잘못된 구(반지름 음수).
    SimpleCollision collision(scene.allocator);
    REQUIRE(collision.convexes.TryPushBack(CollisionConvex{}));
    StaticMeshImportSpec convex = Basic(lods[0]);
    convex.collision = &collision;
    CHECK(expect(convex, AssetDiagnosticCode::InvalidCollisionData, 0, mesh));
    ConvexGeometry thin(scene.allocator);
    REQUIRE(thin.vertices.TryPushBack(Vec3d(0.0, 0.0, 0.0)));
    REQUIRE(thin.vertices.TryPushBack(Vec3d(1.0, 0.0, 0.0)));
    REQUIRE(thin.vertices.TryPushBack(Vec3d(0.0, 1.0, 0.0)));
    const ConvexGeometry thins[1] = {Move(thin)};
    convex.convexes = Span<const ConvexGeometry>(thins);
    CHECK(expect(convex, AssetDiagnosticCode::InvalidCollisionData, 0, mesh));
    SimpleCollision spheres(scene.allocator);
    CollisionSphere sphere;
    sphere.radius = -1.0f;
    REQUIRE(spheres.spheres.TryPushBack(sphere));
    StaticMeshImportSpec badSphere = Basic(lods[0]);
    badSphere.collision = &spheres;
    CHECK(expect(badSphere, AssetDiagnosticCode::InvalidCollisionData, 0, mesh));

    // 참조: 슬롯 0 overlay에 메시(위치 1), 슬롯 하나 뒤 distance field 자리에 머티리얼(위치 3),
    // 없는 id(OpenAsset 진단 그대로), 출력이 StaticMesh가 아님.
    StaticMeshImportSlot overlay;
    overlay.overlayMaterial = CollisionAsset;
    StaticMeshImportSpec references = Basic(lods[0]);
    references.slots = Span<const StaticMeshImportSlot>(&overlay, 1);
    CHECK(expect(references, AssetDiagnosticCode::TypeMismatch, 1, mesh));
    overlay.overlayMaterial = AssetId{};
    references.distanceFieldReplacementMesh = MaterialAsset;
    CHECK(expect(references, AssetDiagnosticCode::TypeMismatch, 3, mesh));
    references.distanceFieldReplacementMesh = AssetId{Guid{0x1234ull, 0x5678ull}};
    CHECK(expect(references, AssetDiagnosticCode::MissingDependency, 0, mesh));
    CHECK(expect(Basic(lods[0]), AssetDiagnosticCode::TypeMismatch, 0, scene.material));
}

// MeshSource::Load(bulk 원본): 크기·SHA-256 불일치는 CorruptPackage(BulkUnreadable)·TypeMismatch,
// 빈 ref는 InvalidArgument, 없는 id는 원본의 오류(NotFound)다.
TEST(StaticMeshImport_LoadFromBulkSourceChecksRefs) {
    ImportScene scene;
    REQUIRE(scene.ready);
    MemoryBulkSource bulk(scene.allocator);
    MeshSource quad(scene.allocator);
    REQUIRE(mesh_source_test::BuildQuad(quad));
    MeshSourceData data(scene.allocator);
    REQUIRE(quad.Store(bulk, scene.names, data));
    MeshSource read(scene.allocator);
    REQUIRE(read.Load(data, bulk, scene.names));
    auto fails = [&](void (*mutate)(MeshSourceData&) noexcept, ErrorCode error) noexcept {
        MeshSourceData copy(scene.allocator);
        copy.vertexCount = data.vertexCount;
        copy.cornerCount = data.cornerCount;
        copy.edgeCount = data.edgeCount;
        copy.polygonCount = data.polygonCount;
        copy.triangleCount = data.triangleCount;
        copy.polygonGroupCount = data.polygonGroupCount;
        copy.topology = data.topology;
        CHECK(copy.attributes.TryAppend(data.attributes.View()));
        mutate(copy);
        MeshSource out(scene.allocator);
        MeshDiagnostic failure;
        auto loaded = out.Load(copy, bulk, scene.names, &failure);
        return !loaded && loaded.Error() == error &&
               failure.code == AssetDiagnosticCode::CorruptPackage &&
               failure.issue == MeshIssue::BulkUnreadable;
    };
    CHECK(fails([](MeshSourceData& d) noexcept { d.topology.rawBytes += 1; },
                ErrorCode::TypeMismatch));
    CHECK(fails([](MeshSourceData& d) noexcept { d.topology.sha256.bytes[0] ^= 1; },
                ErrorCode::TypeMismatch));
    CHECK(fails([](MeshSourceData& d) noexcept { d.topology = BulkDataRef{}; },
                ErrorCode::InvalidArgument));
    CHECK(fails([](MeshSourceData& d) noexcept { d.topology.bulkId = 999; }, ErrorCode::NotFound));
}

} // namespace static_mesh_import_tests_file
