#include "../../AssetDatabase/Tests/AssetDatabaseTestSupport.h"
#include "SkinnedMeshTestSupport.h"

#include <AssetNugget/NuggetCache.h>
#include <AssetNugget/NuggetCacheSettings.h>
#include <AssetNugget/NuggetScc.h>
#include <AssetRuntime/MemoryBulkSource.h>
#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/PackageLoad.h>
#include <AssetRuntime/PackageReader.h>
#include <Base/Endian.h>
#include <Base/InitializerList.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/NamePool.h>
#include <GraphicsTypes/VertexFormat.h>
#include <Mesh/SkeletalMesh/SkeletalMesh.generated.h>
#include <Mesh/SkeletalMesh/SkeletalMeshValidation.h>
#include <MeshTools/SkeletalMeshCook.h>
#include <MeshTools/StaticMeshCook.h>
#include <Object/ConstructionContext.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/ObjectRegistryOptions.h>
#include <Platform/File.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

// 스킨 메시 쿠킹 처리기 시험이다(Docs/Design/MeshAnimationAssets 6.7.2절): 쿠킹 패키지(EditorOnly
// 없음·ValidateSkeletalMesh·LOD bulk 값 검증 통과·inverse-bind bulk 값·화면 크기·LOD 전환 여유),
// 대상 프로파일 값과 LOD 머티리얼 맵 굽기, 서버 프로파일(stream 없음, 본 목록·inverse-bind 남음),
// LOD 입력(원본 없는 LOD 빼기·축소 활성 원본 빌드·baked), 실패(LOD 없음·LOD 수 초과·본 계층 위반)와
// 에디터 객체 되돌리기, 너겟(미적중 → 적중 → verify → 손상 적중), 너겟 키(본·맵·스킨 설정은 바꾸고
// 화면 크기·전환 여유·minLod는 그대로), 너겟 머리 왕복·손상, 처리기 등록.
using namespace rocks;
namespace skeletal_mesh_cook_tests_file {

using graphics::VertexFormat;
using rocks::asset_database_test::TempProject;
using namespace rocks::skinned_mesh_test;

namespace {

// 시험 메시의 본 수다(사슬 — 정점은 모두 본 1에 붙는다).
constexpr uint32 TestBones = 2;

AssetId MeshId(uint64 seed) noexcept {
    return AssetId{Guid{0x534b4d4553480000ull, seed}};
}

struct CookOut {
    explicit CookOut(Allocator& allocator) noexcept : sink(allocator), warnings(allocator) {
    }

    MemoryPackageSink sink;
    AssetDiagnostic failure;
    Array<CookDiagnostic> warnings;
};

struct Loaded {
    RootRef<Object> root;
    ObjectHandle package{};
    ObjectHandle mesh{};
};

// 스킨 원본(사각형, 모든 정점 본 1)을 만든다.
bool BuildSkinnedQuad(MeshSource& quad) noexcept {
    return mesh_source_test::BuildQuad(quad) && RigidWeights(quad, 1) != MeshNoAttribute;
}

// 스킨 사각형에 가중치 프로필 Cloth(모든 정점 본 0), 위치 morph Smile(정점 1 z + 5), 법선만 있는
// morph Blink(코너 2 법선 변화)를 더한다.
bool BuildRiggedQuad(MeshSource& quad) noexcept {
    if (!BuildSkinnedQuad(quad)) {
        return false;
    }

    const uint32 cloth = AddWeights(quad, "SkinWeights-Cloth");
    for (uint32 vertex = 0; cloth != MeshNoAttribute && vertex < 4; ++vertex) {
        const BoneInfluence influence{0, BoneWeightTotal};
        if (!SetWeights(quad, cloth, vertex, Span<const BoneInfluence>(&influence, 1))) {
            return false;
        }
    }

    const uint32 smile = AddSparse(quad, MeshElement::Vertex, "Morph-Smile");
    const uint32 blink = AddSparse(quad, MeshElement::Corner, "MorphNormal-Blink");
    return cloth != MeshNoAttribute && smile != MeshNoAttribute && blink != MeshNoAttribute &&
           quad.SetSparseValue(smile, 1, Vec3(0.0f, 0.0f, 5.0f)).IsOk() &&
           quad.SetSparseValue(blink, 2, Vec3(0.0f, 0.5f, 0.0f)).IsOk();
}

// Mesh 타입을 등록한 패키지 정책 Registry 둘(에디터·쿠킹 로드)과 에디터 패키지 하나다.
struct CookScene {
    explicit CookScene(StringView backend = "D3D12") noexcept
        : types(allocator), registration(types, allocator), names(allocator), sourceBulk(allocator),
          profile(allocator), entry(allocator) {
        ready = RegisterMeshTypes(registration).IsOk() && names.Initialize().IsOk();
        RegistryOptions options;
        options.names = &names;
        options.policy.packageNativeType = NativeTypeKey<Package>();
        options.policy.rejectWorldOnly = true;
        auto made = UniquePtr<ObjectRegistry>::TryMake(allocator, allocator, types,
                                                       ContextId{0x534b4d4bull, 1}, options);
        auto other = UniquePtr<ObjectRegistry>::TryMake(allocator, allocator, types,
                                                        ContextId{0x534b4d4bull, 2}, options);
        if (made && other) {
            editor = Move(made.Value());
            cooked = Move(other.Value());
        }

        ready = ready && editor.Get() != nullptr && cooked.Get() != nullptr &&
                static_cast<bool>(profile.id.TryAssign("Windows"));
        if (!backend.IsEmpty()) {
            String name(allocator);
            ready = ready && static_cast<bool>(name.TryAssign(backend)) &&
                    static_cast<bool>(profile.graphicsBackends.TryPushBack(Move(name)));
        }
    }

    ~CookScene() noexcept {
        package.Reset();
        editor.Reset();
        cooked.Reset();
        CHECK(registration.Unregister());
    }

    ObjectAccessContext EditorAccess() const noexcept {
        return editor->OwnerAccess();
    }

    // 새 에디터 패키지 id에 원본 LOD lodCount개(스킨 사각형, 축소 없음)·슬롯 slots개·본
    // TestBones개의 스킨 메시를 만든다.
    bool Make(uint64 id, uint32 lodCount = 1, uint32 slots = 1) noexcept {
        package.Reset();
        asset = MeshId(id);
        auto made = NewPackage(*editor, EditorAccess(), asset, PackageKind::Editor);
        if (!made) {
            return false;
        }

        package = Move(made.Value());
        auto created = editor->NewObject<SkeletalMesh>(EditorAccess(), TypeIdOf<SkeletalMesh>(),
                                                       package.Handle(), "");
        if (!created) {
            return false;
        }

        mesh = created.Value().Handle();
        MeshSource quad(allocator);
        if (!BuildSkinnedQuad(quad)) {
            return false;
        }

        return Edit([&](SkeletalMesh& value) noexcept {
            for (uint32 slot = 0; slot < slots; ++slot) {
                if (!editor->AddElement(EditorAccess(), mesh, value.materialSlots)) {
                    return false;
                }
            }

            if (!ChainBones(names, TestBones, value.bones)) {
                return false;
            }

            for (uint32 index = 0; index < lodCount; ++index) {
                SkeletalMeshSourceLod lod(allocator);
                lod.hasSource = true;
                lod.reduction.percentTriangles = 1.0f;
                lod.reduction.percentVertices = 1.0f;
                if (!quad.Store(sourceBulk, names, lod.source) ||
                    !value.sourceLods.TryPushBack(Move(lod))) {
                    return false;
                }
            }

            return true;
        });
    }

    template <class Fn>
    bool Edit(Fn&& fn) noexcept {
        auto borrowed = editor->ResolveForWrite<SkeletalMesh>(EditorAccess(), mesh);
        if (!borrowed) {
            return false;
        }

        auto value = borrowed.Value().Get();
        return value && fn(*value.Value());
    }

    // 에디터 객체가 쿠킹 전 값 그대로인지다(renderData 비어 있음·minLod).
    bool EditorUntouched(int32 minLod = 0) noexcept {
        return Edit([&](SkeletalMesh& value) noexcept {
            return value.renderData.lods.IsEmpty() && value.renderData.sphereRadius == 0.0 &&
                   value.renderData.inverseBindMatrices.IsEmpty() &&
                   value.minLod.defaultValue == minLod;
        });
    }

    Result<void> Cook(const SkeletalMeshCookProcessor& processor, CookOut& out,
                      NuggetCache* nuggets = nullptr, bool verify = false) noexcept {
        entry.id = asset;
        CookContext context;
        context.allocator = &allocator;
        context.registry = editor.Get();
        context.access = EditorAccess();
        context.package = package.Handle();
        context.entry = &entry;
        context.profile = &profile;
        context.cookerVersion = 1;
        context.bulkSource = &sourceBulk;
        context.nuggets = nuggets;
        context.verifyNuggets = verify;
        context.sink = &out.sink;
        context.failure = &out.failure;
        context.warnings = &out.warnings;
        return processor.Cook(context);
    }

    bool Load(Span<const uint8> bytes, Loaded& out) noexcept {
        Array<uint8> copy(allocator);
        if (!copy.TryAppend(bytes)) {
            return false;
        }

        PackageLoadOptions options;
        options.read.expectedKind = PackageKind::Cooked;
        auto made = UniquePtr<PackageLoadState>::TryMake(allocator, allocator, *cooked, types,
                                                         cooked->OwnerAccess(), options);
        if (!made) {
            return false;
        }

        UniquePtr<PackageLoadState> state = Move(made.Value());
        if (!state->Prepare(Move(copy), nullptr)) {
            return false;
        }

        ConstructionContext transaction(*cooked, cooked->OwnerAccess());
        PackageLoadState* pointers[1] = {state.Get()};
        ObjectHandle handles[1] = {};
        RootRef<Object> roots[1];
        if (!InstantiatePackages(transaction, Span<PackageLoadState* const>(pointers, 1), nullptr,
                                 Span<ObjectHandle>(handles), nullptr) ||
            !transaction.Commit(Span<ObjectHandle>(handles), Span<RootRef<Object>>(roots))) {
            return false;
        }

        out.root = Move(roots[0]);
        out.package = handles[0];
        auto child = cooked->FindChild(cooked->OwnerAccess(), out.package, "");
        out.mesh = child ? child.Value() : ObjectHandle{};
        return !out.mesh.IsNull();
    }

    const SkeletalMesh* LoadedMesh(const Loaded& loaded) noexcept {
        auto borrowed = cooked->Resolve<SkeletalMesh>(cooked->OwnerAccess(), loaded.mesh);
        if (!borrowed) {
            return nullptr;
        }

        auto value = borrowed.Value().Get();
        return value ? value.Value() : nullptr;
    }

    const Package* LoadedPackage(const Loaded& loaded) noexcept {
        auto borrowed = cooked->Resolve<Package>(cooked->OwnerAccess(), loaded.package);
        if (!borrowed) {
            return nullptr;
        }

        auto value = borrowed.Value().Get();
        return value ? value.Value() : nullptr;
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration registration;
    NamePool names;
    UniquePtr<ObjectRegistry> editor;
    UniquePtr<ObjectRegistry> cooked;
    RootRef<Package> package;
    ObjectHandle mesh{};
    AssetId asset{};
    MemoryBulkSource sourceBulk;
    TargetProfile profile;
    AssetIndexEntry entry;
    bool ready = false;
};

// 쿠킹·로드해 메시를 돌려준다(실패하면 null). 로드한 메시는 ValidateSkeletalMesh와 LOD마다
// ValidateSkinnedRenderLodBulk를 통과해야 하고, inverse-bind bulk는 본 수 × 64 byte다.
const SkeletalMesh* CookAndLoad(CookScene& scene, const SkeletalMeshCookProcessor& processor,
                                CookOut& out, Loaded& loaded) noexcept {
    if (!scene.Cook(processor, out) || !scene.Load(out.sink.Bytes(), loaded)) {
        return nullptr;
    }

    const SkeletalMesh* mesh = scene.LoadedMesh(loaded);
    const Package* package = scene.LoadedPackage(loaded);
    if (mesh == nullptr || package == nullptr || !ValidateSkeletalMesh(*mesh, scene.allocator)) {
        return nullptr;
    }

    for (usize index = 0; index < mesh->renderData.lods.Size(); ++index) {
        const SkinnedRenderLod& lod = mesh->renderData.lods[index];
        Span<const uint8> bytes;
        if (!lod.data.IsEmpty()) {
            auto read = package->BulkBytes(lod.data);
            if (!read) {
                return nullptr;
            }

            bytes = read.Value();
        }

        if (!ValidateSkinnedRenderLodBulk(lod, static_cast<uint32>(index), bytes)) {
            return nullptr;
        }
    }

    auto binds = package->BulkBytes(mesh->renderData.inverseBindMatrices);
    if (!binds ||
        binds.Value().Size() != mesh->bones.bones.Size() * SkinnedInverseBindMatrixBytes) {
        return nullptr;
    }

    return mesh;
}

usize CountWarnings(const CookOut& out, AssetDiagnosticCode code) noexcept {
    usize count = 0;
    for (const CookDiagnostic& warning : out.warnings) {
        count += warning.code == code && warning.warning ? 1 : 0;
    }

    return count;
}

bool SameBytes(Span<const uint8> a, Span<const uint8> b) noexcept {
    return a.Size() == b.Size() && platform::MemoryEquals(a.Data(), b.Data(), a.Size());
}

// 시험 캐시(임시 프로젝트의 Intermediate/AssetNuggets)다.
struct NuggetScene {
    NuggetScene(Allocator& sceneAllocator, const char* tag) noexcept
        : allocator(sceneAllocator), project(tag), settings(sceneAllocator), root(sceneAllocator) {
        ready = project.made &&
                static_cast<bool>(settings.localPath.TryAssign(NuggetDefaultLocalPath)) &&
                static_cast<bool>(ResolveNuggetLocalPath(settings, project.Root(), root)) &&
                Reopen();
    }

    bool Reopen() noexcept {
        cache.Reset();
        auto made = UniquePtr<NuggetCache>::TryMake(allocator, allocator,
                                                    platform::PhysicalFileSystem(), clock);
        if (!made) {
            return false;
        }

        cache = Move(made.Value());
        return static_cast<bool>(cache->Open(settings, project.Root()));
    }

    // key 항목을 values로 바꿔 쓴다(지우고 새 캐시로 Put — 같은 키 다른 값의 적중).
    bool Replace(const Sha256Digest& key, AssetId member, Array<NuggetValue>& values) noexcept {
        char hex[Sha256Digest::HexBytes + 1];
        key.ToHex(hex);
        String path(allocator);
        if (!path.TryAppend(root.View()) || !path.TryAppend("/") ||
            !path.TryAppend(StringView(hex, 2)) || !path.TryAppend("/") ||
            !path.TryAppend(StringView(hex, Sha256Digest::HexBytes)) || !path.TryAppend(".rnug") ||
            !platform::PhysicalFileSystem().Delete(path.View().Data(), path.View().ByteCount()) ||
            !Reopen()) {
            return false;
        }

        NuggetCachePut put;
        put.bucket = NuggetBucketMesh;
        put.key = key;
        put.values = values.View();
        const AssetId members[1] = {member};
        const NuggetCachePutResult stored = cache->PutScc(Span<const AssetId>(members), put);
        return stored.status == NuggetStatus::Stored && Reopen();
    }

    Allocator& allocator;
    TempProject project;
    NuggetCacheSettings settings;
    String root;
    NuggetSystemClock clock;
    UniquePtr<NuggetCache> cache;
    bool ready = false;
};

} // namespace

// 쿠킹 패키지: LOD 하나(영향 4칸 UByte4·UByte4Unorm, bone map = 본 1, 필요 본 전체),
// EditorOnly(원본 LOD) 없음, 화면 크기 = LOD 정보 기본 1.0(자동 계산 없음), LOD 전환 여유 그대로,
// inverse-bind bulk가 본마다 기준 pose 역행렬(float — 본 0은 PoseFor(0)의 역), 에디터 객체 되돌림.
TEST(SkeletalMeshCook_CookedPackage) {
    CookScene scene;
    REQUIRE(scene.ready);
    REQUIRE(scene.Make(1));
    REQUIRE(scene.Edit([](SkeletalMesh& value) noexcept {
        value.sourceLods[0].lodHysteresis = 0.25f;
        return true;
    }));
    SkeletalMeshCookProcessor processor;
    CookOut out(scene.allocator);
    Loaded loaded;
    const SkeletalMesh* mesh = CookAndLoad(scene, processor, out, loaded);
    REQUIRE(mesh != nullptr);
    CHECK(processor.Builds() == 1 && out.warnings.IsEmpty());
    CHECK(mesh->sourceLods.IsEmpty() && mesh->bones.bones.Size() == TestBones);
    REQUIRE(mesh->renderData.lods.Size() == 1);
    const SkinnedRenderLod& lod = mesh->renderData.lods[0];
    CHECK(lod.screenSize == SkeletalMeshSourceLod::DefaultScreenSize &&
          lod.lodHysteresis == 0.25f && lod.vertexCount == 4 && lod.sections.Size() == 1);
    CHECK(lod.influencesPerVertex == MeshCookedInfluencesLow &&
          lod.boneIndices.vertexFormat == graphics::VertexFormatNumber(VertexFormat::UByte4) &&
          lod.boneWeights.vertexFormat == graphics::VertexFormatNumber(VertexFormat::UByte4Unorm));
    CHECK(lod.boneMap.Size() == 1 && lod.boneMap[0] == 1 && lod.requiredBones.Size() == TestBones);
    CHECK(mesh->renderData.box.max.x == 100.0);
    // inverse-bind: 본 0의 행렬 × 기준 pose 행렬 = 단위(float 오차 안).
    const Package* package = scene.LoadedPackage(loaded);
    REQUIRE(package != nullptr);
    auto binds = package->BulkBytes(mesh->renderData.inverseBindMatrices);
    REQUIRE(binds);
    Mat4d inverse;
    for (usize at = 0; at < 16; ++at) {
        inverse.m[at] = platform::BitCast<float>(
            LoadLittleEndian<uint32>(binds.Value().Data() + at * sizeof(float)));
    }

    const Mat4d product = inverse * animation_test::PoseFor(0).ToMatrix();
    CHECK(product.Equals(Mat4d{}, 1.0e-4));
    CHECK(scene.EditorUntouched());
}

// 대상 프로파일 값과 LOD 머티리얼 맵: 화면 크기·minLod는 프로파일 값(minLod는 쿠킹 LOD 번호),
// LOD 1의 맵 {1}은 그 섹션 materialIndex로 굽고 LOD 0은 그대로다.
TEST(SkeletalMeshCook_ProfileValuesAndMaterialMap) {
    CookScene scene;
    REQUIRE(scene.ready);
    REQUIRE(scene.Make(2, 2, 2));
    const Name windows = scene.names.TryIntern("Windows").Value();
    REQUIRE(scene.Edit([&](SkeletalMesh& value) noexcept {
        value.sourceLods[1].screenSize.defaultValue = 0.5f;
        return value.sourceLods[1].screenSize.perPlatform.Insert(windows, 0.3f).IsOk() &&
               value.minLod.perPlatform.Insert(windows, 1).IsOk() &&
               value.sourceLods[1].lodMaterialMap.TryPushBack(1).IsOk();
    }));
    SkeletalMeshCookProcessor processor;
    CookOut out(scene.allocator);
    Loaded loaded;
    const SkeletalMesh* mesh = CookAndLoad(scene, processor, out, loaded);
    REQUIRE(mesh != nullptr);
    REQUIRE(mesh->renderData.lods.Size() == 2);
    CHECK(mesh->renderData.lods[0].screenSize == 1.0f &&
          mesh->renderData.lods[1].screenSize == 0.3f);
    CHECK(mesh->renderData.lods[0].sections[0].materialIndex == 0 &&
          mesh->renderData.lods[1].sections[0].materialIndex == 1);
    CHECK(mesh->minLod.defaultValue == 1 && mesh->minLod.perPlatform.IsEmpty());
    CHECK(scene.EditorUntouched());
}

// 서버 프로파일(backend 없음): stream·data·영향 없음, 섹션·정점 수·bounds·bone map·필요 본·
// inverse-bind 남음.
TEST(SkeletalMeshCook_ServerProfile) {
    CookScene scene("");
    REQUIRE(scene.ready);
    REQUIRE(scene.Make(3));
    SkeletalMeshCookProcessor processor;
    CookOut out(scene.allocator);
    Loaded loaded;
    const SkeletalMesh* mesh = CookAndLoad(scene, processor, out, loaded);
    REQUIRE(mesh != nullptr);
    REQUIRE(mesh->renderData.lods.Size() == 1);
    const SkinnedRenderLod& lod = mesh->renderData.lods[0];
    CHECK(lod.data.IsEmpty() && lod.positions.byteSize == 0 && lod.indices.byteSize == 0 &&
          lod.boneIndices.byteSize == 0 && lod.influencesPerVertex == 0 &&
          lod.texCoords.IsEmpty() && lod.vertexCount == 4 && lod.sections.Size() == 1);
    CHECK(lod.boneMap.Size() == 1 && lod.requiredBones.Size() == TestBones);
    CHECK(!mesh->renderData.inverseBindMatrices.IsEmpty() && mesh->renderData.box.max.y == 100.0);
}

// LOD 입력: 원본 없는 LOD는 빠지고(경고, minLod 옮김), 축소 활성 원본은 축소 없이 빌드(경고), 생성
// LOD의 baked 원본은 그것으로 빌드(경고 없음).
TEST(SkeletalMeshCook_LodInputs) {
    CookScene scene;
    REQUIRE(scene.ready);
    REQUIRE(scene.Make(4, 3));
    REQUIRE(scene.Edit([](SkeletalMesh& value) noexcept {
        value.sourceLods[1].hasSource = false;
        value.sourceLods[2].reduction.percentTriangles = 0.5f;
        value.minLod.defaultValue = 2;
        return true;
    }));
    SkeletalMeshCookProcessor processor;
    CookOut out(scene.allocator);
    Loaded loaded;
    const SkeletalMesh* mesh = CookAndLoad(scene, processor, out, loaded);
    REQUIRE(mesh != nullptr);
    CHECK(mesh->renderData.lods.Size() == 2 && processor.Builds() == 2);
    CHECK(CountWarnings(out, AssetDiagnosticCode::UnsupportedSetting) == 2);
    CHECK(mesh->minLod.defaultValue == 1);
    CHECK(scene.EditorUntouched(2));

    CookScene baked;
    REQUIRE(baked.ready);
    REQUIRE(baked.Make(5, 1));
    REQUIRE(baked.Edit([&](SkeletalMesh& value) noexcept {
        SkeletalMeshSourceLod& lod = value.sourceLods[0];
        lod.hasSource = false;
        lod.hasBakedSource = true;
        MeshSource quad(baked.allocator);
        return BuildSkinnedQuad(quad) &&
               quad.Store(baked.sourceBulk, baked.names, lod.bakedSource).IsOk();
    }));
    CookOut bakedOut(baked.allocator);
    Loaded bakedLoaded;
    const SkeletalMesh* bakedMesh = CookAndLoad(baked, processor, bakedOut, bakedLoaded);
    REQUIRE(bakedMesh != nullptr);
    CHECK(bakedMesh->renderData.lods.Size() == 1 && bakedOut.warnings.IsEmpty());
}

// 실패: 쓸 LOD 없음 InvalidSource, 원본 LOD 9개 InvalidSetting, 본 계층 위반(부모가 자기보다 뒤)
// InvalidSkeletonData(detail = 본). 에디터 객체는 그대로다.
TEST(SkeletalMeshCook_Failures) {
    CookScene scene;
    REQUIRE(scene.ready);
    SkeletalMeshCookProcessor processor;
    REQUIRE(scene.Make(6, 1));
    REQUIRE(scene.Edit([](SkeletalMesh& value) noexcept {
        value.sourceLods[0].hasSource = false;
        return true;
    }));
    CookOut none(scene.allocator);
    CHECK(!scene.Cook(processor, none) && none.failure.code == AssetDiagnosticCode::InvalidSource);
    CHECK(scene.EditorUntouched());
    REQUIRE(scene.Make(7, SkeletalMeshMaxLods + 1));
    CookOut many(scene.allocator);
    CHECK(!scene.Cook(processor, many) && many.failure.code == AssetDiagnosticCode::InvalidSetting);
    CHECK(scene.EditorUntouched());
    REQUIRE(scene.Make(8, 1));
    REQUIRE(scene.Edit([](SkeletalMesh& value) noexcept {
        value.bones.bones[0].parentIndex = 1;
        return true;
    }));
    CookOut bones(scene.allocator);
    CHECK(!scene.Cook(processor, bones) &&
          bones.failure.code == AssetDiagnosticCode::InvalidSkeletonData);
    CHECK(processor.Builds() == 0 && scene.EditorUntouched());
}

// 너겟: 미적중 → Put → 적중(빌드 없음, 같은 패키지 byte) → verify(다시 빌드, 일치) → 손상 적중(값
// 머리만)은 미적중으로 다시 빌드한다. 서버 프로파일도 적중한다(머리·inverse-bind 둘).
TEST(SkeletalMeshCook_Nuggets) {
    CookScene scene;
    REQUIRE(scene.ready);
    NuggetScene nuggets(scene.allocator, "skeletal-mesh-cook-nuggets");
    REQUIRE(nuggets.ready);
    SkeletalMeshCookProcessor processor;
    REQUIRE(scene.Make(9, 2));
    CookOut first(scene.allocator);
    REQUIRE(scene.Cook(processor, first, nuggets.cache.Get()));
    CHECK(processor.Builds() == 2);
    const Sha256Digest key = processor.LastNuggetKey();
    CookOut second(scene.allocator);
    REQUIRE(scene.Cook(processor, second, nuggets.cache.Get()));
    CHECK(processor.Builds() == 2);
    CHECK(SameBytes(first.sink.Bytes(), second.sink.Bytes()));
    CookOut verify(scene.allocator);
    REQUIRE(scene.Cook(processor, verify, nuggets.cache.Get(), true));
    CHECK(processor.Builds() == 4);
    CHECK(scene.EditorUntouched());

    Array<NuggetValue> broken(scene.allocator);
    NuggetValue head(scene.allocator);
    head.id = NuggetSccValueId(0, 0);
    REQUIRE(head.bytes.TryAppend(Span<const uint8>(reinterpret_cast<const uint8*>("bad"), 3)));
    REQUIRE(broken.TryPushBack(Move(head)));
    REQUIRE(nuggets.Replace(key, scene.asset, broken));
    CookOut rebuilt(scene.allocator);
    REQUIRE(scene.Cook(processor, rebuilt, nuggets.cache.Get()));
    CHECK(processor.Builds() == 6);
    CHECK(SameBytes(rebuilt.sink.Bytes(), first.sink.Bytes()));

    scene.profile.graphicsBackends.Clear();
    CookOut serverMiss(scene.allocator);
    REQUIRE(scene.Cook(processor, serverMiss, nuggets.cache.Get()));
    CHECK(processor.LastNuggetKey() != key && processor.Builds() == 8);
    CookOut serverHit(scene.allocator);
    REQUIRE(scene.Cook(processor, serverHit, nuggets.cache.Get()));
    CHECK(processor.Builds() == 8 && SameBytes(serverMiss.sink.Bytes(), serverHit.sink.Bytes()));
}

// 프로필·morph: 이름은 원본 속성 이름에서 접두를 뗀 것(Cloth, Smile, 법선만 있는 Blink)이고 적중
// 왕복(같은 패키지 byte)·verify가 이름을 지킨다. 서버 프로파일은 이름만 남긴다(stream 비움,
// deltaCount 0).
TEST(SkeletalMeshCook_ProfilesAndMorphs) {
    for (const StringView backend : {StringView("D3D12"), StringView("")}) {
        CookScene scene(backend);
        REQUIRE(scene.ready);
        NuggetScene nuggets(scene.allocator, backend.IsEmpty() ? "skeletal-mesh-cook-rig-server"
                                                               : "skeletal-mesh-cook-rig");
        REQUIRE(nuggets.ready);
        REQUIRE(scene.Make(11, 1));
        REQUIRE(scene.Edit([&](SkeletalMesh& value) noexcept {
            MeshSource quad(scene.allocator);
            return BuildRiggedQuad(quad) &&
                   quad.Store(scene.sourceBulk, scene.names, value.sourceLods[0].source).IsOk();
        }));
        SkeletalMeshCookProcessor processor;
        CookOut miss(scene.allocator);
        REQUIRE(scene.Cook(processor, miss, nuggets.cache.Get()));
        CookOut hit(scene.allocator);
        REQUIRE(scene.Cook(processor, hit, nuggets.cache.Get()));
        CHECK(processor.Builds() == 1 && SameBytes(miss.sink.Bytes(), hit.sink.Bytes()));
        CookOut verify(scene.allocator);
        REQUIRE(scene.Cook(processor, verify, nuggets.cache.Get(), true));
        CHECK(processor.Builds() == 2);
        Loaded loaded;
        REQUIRE(scene.Load(hit.sink.Bytes(), loaded));
        const SkeletalMesh* mesh = scene.LoadedMesh(loaded);
        REQUIRE(mesh != nullptr && mesh->renderData.lods.Size() == 1);
        const SkinnedRenderLod& lod = mesh->renderData.lods[0];
        REQUIRE(lod.skinWeightProfiles.Size() == 1 && lod.morphTargets.Size() == 2);
        CHECK(lod.skinWeightProfiles[0].name == scene.names.TryFind("Cloth").Value() &&
              lod.morphTargets[0].name == scene.names.TryFind("Smile").Value() &&
              lod.morphTargets[1].name == scene.names.TryFind("Blink").Value());
        if (backend.IsEmpty()) {
            CHECK(lod.skinWeightProfiles[0].boneIndices.byteSize == 0 &&
                  lod.skinWeightProfiles[0].boneWeights.byteSize == 0 &&
                  lod.morphTargets[0].deltaCount == 0 && lod.morphTargets[1].deltas.byteSize == 0);
        } else {
            CHECK(lod.skinWeightProfiles[0].boneWeights.byteSize == lod.boneWeights.byteSize &&
                  lod.morphTargets[0].deltaCount != 0 && lod.morphTargets[1].deltaCount != 0);
        }

        CHECK(ValidateSkeletalMesh(*mesh, scene.allocator));
    }
}

// 적중 거절: part 하나 빠짐, inverse-bind byte 수 틀림, LOD bulk의 영향 번호가 bone map 밖이면
// 미적중으로 다시 빌드한다(각 경우 좋은 값에서 시작). 검증을 통과하는 손상(위치 값)은 적중으로 받고
// verify가 NuggetMismatch로 잡는다. 적중의 본 계층 검사는 본을 바꾸면 키가 바뀌어 시험으로 닿지
// 않는다(쿠킹 패키지가 본을 담으므로 적중에서도 보는 방어다).
TEST(SkeletalMeshCook_HitRejections) {
    CookScene scene;
    REQUIRE(scene.ready);
    NuggetScene nuggets(scene.allocator, "skeletal-mesh-cook-rejects");
    REQUIRE(nuggets.ready);
    SkeletalMeshCookProcessor processor;
    REQUIRE(scene.Make(12, 1));
    CookOut first(scene.allocator);
    REQUIRE(scene.Cook(processor, first, nuggets.cache.Get()));
    const Sha256Digest key = processor.LastNuggetKey();
    Loaded loaded;
    REQUIRE(scene.Load(first.sink.Bytes(), loaded));
    const SkeletalMesh* mesh = scene.LoadedMesh(loaded);
    REQUIRE(mesh != nullptr);
    const uint64 boneIndexAt = mesh->renderData.lods[0].boneIndices.offset;
    NuggetCacheGetResult good = nuggets.cache->GetSccMember(NuggetBucketMesh, key, scene.asset);
    REQUIRE(good.status == NuggetStatus::Hit && good.values.Size() == 3);
    // 좋은 값의 사본에 mutate를 적용해 바꿔 쓴다.
    auto replaceWith = [&](auto&& mutate) noexcept {
        Array<NuggetValue> values(scene.allocator);
        for (const NuggetValue& source : good.values) {
            NuggetValue value(scene.allocator);
            value.id = source.id;
            CHECK(value.bytes.TryAppend(source.bytes.View()));
            CHECK(values.TryPushBack(Move(value)));
        }

        mutate(values);
        return nuggets.Replace(key, scene.asset, values);
    };
    auto rebuilds = [&](auto&& mutate) noexcept {
        if (!replaceWith(mutate)) {
            return false;
        }

        const uint64 before = processor.Builds();
        CookOut out(scene.allocator);
        return scene.Cook(processor, out, nuggets.cache.Get()).IsOk() &&
               processor.Builds() == before + 1 && SameBytes(out.sink.Bytes(), first.sink.Bytes());
    };
    CHECK(rebuilds([](Array<NuggetValue>& values) noexcept { (void)values.TryPopBack(); }));
    CHECK(rebuilds([](Array<NuggetValue>& values) noexcept {
        (void)values[1].bytes.TryResize(values[1].bytes.Size() - 4);
    }));
    CHECK(rebuilds([&](Array<NuggetValue>& values) noexcept {
        values[2].bytes[static_cast<usize>(boneIndexAt)] = 0xFF;
    }));

    // 위치 값 손상: 적중으로 받고(빌드 없음), verify는 NuggetMismatch로 실패한다.
    REQUIRE(replaceWith([](Array<NuggetValue>& values) noexcept {
        StoreLittleEndian<uint32>(values[2].bytes.Data(), platform::BitCast<uint32>(12345.0f));
    }));
    const uint64 before = processor.Builds();
    CookOut trusted(scene.allocator);
    REQUIRE(scene.Cook(processor, trusted, nuggets.cache.Get()));
    CHECK(processor.Builds() == before && !SameBytes(trusted.sink.Bytes(), first.sink.Bytes()));
    CookOut verify(scene.allocator);
    CHECK(!scene.Cook(processor, verify, nuggets.cache.Get(), true) &&
          verify.failure.code == AssetDiagnosticCode::NuggetMismatch);
    CHECK(scene.EditorUntouched());
}

// 너겟 키: 메시 본(pose)·LOD 머티리얼 맵·스킨 설정은 키를 바꾸고, 화면 크기·LOD 전환 여유·minLod·
// bounds 확장은 바꾸지 않는다(너겟 밖 값). 스켈레톤 참조는 키가 읽지 않는다(6.7.2절 3).
TEST(SkeletalMeshCook_NuggetKeys) {
    CookScene scene;
    REQUIRE(scene.ready);
    NuggetScene nuggets(scene.allocator, "skeletal-mesh-cook-keys");
    REQUIRE(nuggets.ready);
    SkeletalMeshCookProcessor processor;
    REQUIRE(scene.Make(10, 1));
    auto keyAfter = [&](auto&& edit) noexcept {
        CHECK(scene.Edit(edit));
        CookOut out(scene.allocator);
        CHECK(scene.Cook(processor, out, nuggets.cache.Get()));
        return processor.LastNuggetKey();
    };
    const Sha256Digest base = keyAfter([](SkeletalMesh&) noexcept { return true; });
    CHECK(keyAfter([](SkeletalMesh& value) noexcept {
              value.sourceLods[0].screenSize.defaultValue = 0.5f;
              value.sourceLods[0].lodHysteresis = 0.1f;
              value.minLod.defaultValue = 1;
              value.positiveBoundsExtension = Vec3d(1.0, 2.0, 3.0);
              return true;
          }) == base);
    const Sha256Digest poseKey = keyAfter([](SkeletalMesh& value) noexcept {
        value.bones.referencePose[1].translation.x += 1.0;
        return true;
    });
    CHECK(poseKey != base);
    const Sha256Digest mapKey = keyAfter([](SkeletalMesh& value) noexcept {
        return value.sourceLods[0].lodMaterialMap.TryPushBack(SkeletalMeshNoMaterialRemap).IsOk();
    });
    CHECK(mapKey != poseKey);
    const Sha256Digest skinKey = keyAfter([](SkeletalMesh& value) noexcept {
        value.sourceLods[0].build.useHighPrecisionSkinWeights = true;
        return true;
    });
    CHECK(skinKey != mapKey);
    // 16비트 가중치 빌드는 UShort4Unorm으로 쿠킹된다.
    CookOut out(scene.allocator);
    Loaded loaded;
    const SkeletalMesh* mesh = CookAndLoad(scene, processor, out, loaded);
    REQUIRE(mesh != nullptr);
    CHECK(mesh->renderData.lods[0].boneWeights.vertexFormat ==
          graphics::VertexFormatNumber(VertexFormat::UShort4Unorm));
}

// 너겟 머리 왕복(프로필·morph 이름 원문·stream, bone map·필요 본, 영향 칸)과 손상(버전·LOD 수·남는
// byte·빠진 byte·이름 길이).
TEST(SkeletalMeshCook_NuggetHeader) {
    SystemAllocator allocator;
    NamePool names(allocator);
    REQUIRE(names.Initialize());
    SkeletalMeshRenderData data(allocator);
    SkinnedRenderLod lod(allocator);
    lod.vertexCount = 4;
    lod.data.rawBytes = 256;
    lod.positions = MeshRenderStream{3, 0, 48};
    lod.influencesPerVertex = MeshCookedInfluencesHigh;
    lod.boneIndices = MeshRenderStream{6, 112, 32};
    REQUIRE(lod.texCoords.TryPushBack(MeshRenderStream{14, 80, 32}));
    REQUIRE(lod.boneMap.TryPushBack(3) && lod.boneMap.TryPushBack(9));
    REQUIRE(lod.requiredBones.TryPushBack(0));
    SkinWeightProfileRenderLod profile;
    profile.name = names.TryIntern("Cloth").Value();
    profile.boneWeights = MeshRenderStream{7, 160, 16};
    REQUIRE(lod.skinWeightProfiles.TryPushBack(profile));
    SkinnedMorphTarget morph;
    morph.name = names.TryIntern("Smile").Value();
    morph.deltaCount = 2;
    morph.deltas = MeshRenderStream{MeshRenderStreamNone, 176, 40};
    REQUIRE(lod.morphTargets.TryPushBack(morph));
    MeshRenderSection section;
    section.triangleCount = 2;
    section.forceOpaque = true;
    REQUIRE(lod.sections.TryPushBack(section));
    REQUIRE(data.lods.TryPushBack(Move(lod)));
    data.box = Box<double>(Vec3d(-1.0, -2.0, -3.0), Vec3d(1.0, 2.0, 3.0));
    data.sphereRadius = 4.0;
    const MeshCookWarning warnings[1] = {{AssetDiagnosticCode::InfluencesTrimmed, 0}};
    Array<uint8> header(allocator);
    REQUIRE(
        WriteSkeletalMeshNuggetHeader(data, Span<const MeshCookWarning>(warnings), names, header));
    // 읽기는 다른 pool에 intern한다(이름 원문으로 옮겨진다).
    NamePool other(allocator);
    REQUIRE(other.Initialize());
    SkeletalMeshRenderData read(allocator);
    Array<MeshCookWarning> readWarnings(allocator);
    REQUIRE(ReadSkeletalMeshNuggetHeader(header.View(), other, read, readWarnings));
    REQUIRE(read.lods.Size() == 1 && readWarnings.Size() == 1);
    CHECK(readWarnings[0].code == AssetDiagnosticCode::InfluencesTrimmed);
    const SkinnedRenderLod& back = read.lods[0];
    CHECK(read.box.min.z == -3.0 && read.sphereRadius == 4.0 && back.vertexCount == 4 &&
          back.data.rawBytes == 256 && back.texCoords.Size() == 1 &&
          back.influencesPerVertex == MeshCookedInfluencesHigh && back.boneIndices.offset == 112 &&
          back.sections[0].forceOpaque);
    CHECK(back.boneMap.Size() == 2 && back.boneMap[1] == 9 && back.requiredBones.Size() == 1);
    REQUIRE(back.skinWeightProfiles.Size() == 1 && back.morphTargets.Size() == 1);
    CHECK(back.skinWeightProfiles[0].name == other.TryFind("Cloth").Value() &&
          back.skinWeightProfiles[0].boneWeights.offset == 160);
    CHECK(back.morphTargets[0].name == other.TryFind("Smile").Value() &&
          back.morphTargets[0].deltaCount == 2 && back.morphTargets[0].deltas.byteSize == 40);

    auto rejects = [&](auto&& mutate) noexcept {
        Array<uint8> broken(allocator);
        CHECK(broken.TryAppend(header.View()));
        mutate(broken);
        SkeletalMeshRenderData out(allocator);
        Array<MeshCookWarning> outWarnings(allocator);
        return ReadSkeletalMeshNuggetHeader(broken.View(), other, out, outWarnings).Error() ==
               ErrorCode::InvalidArgument;
    };
    CHECK(rejects([](Array<uint8>& b) noexcept { b[0] = 9; }));
    CHECK(rejects([](Array<uint8>& b) noexcept { b[4] = 9; }));
    CHECK(rejects([](Array<uint8>& b) noexcept { (void)b.TryPushBack(0); }));
    CHECK(rejects([](Array<uint8>& b) noexcept { (void)b.TryPopBack(); }));
    // 모든 길이로 자르면 실패하고, byte마다 0xFF로 덮어도 성공 또는 InvalidArgument다(넘겨 읽기
    // 없음).
    for (usize size = 0; size < header.Size(); ++size) {
        SkeletalMeshRenderData out(allocator);
        Array<MeshCookWarning> outWarnings(allocator);
        CHECK(ReadSkeletalMeshNuggetHeader(Span<const uint8>(header.Data(), size), other, out,
                                           outWarnings)
                  .Error() == ErrorCode::InvalidArgument);
    }

    for (usize at = 0; at < header.Size(); ++at) {
        Array<uint8> broken(allocator);
        REQUIRE(broken.TryAppend(header.View()));
        broken[at] = 0xFF;
        SkeletalMeshRenderData out(allocator);
        Array<MeshCookWarning> outWarnings(allocator);
        const Result<void> read =
            ReadSkeletalMeshNuggetHeader(broken.View(), other, out, outWarnings);
        CHECK(read.IsOk() || read.Error() == ErrorCode::InvalidArgument);
    }

    // morph 이름 byte 수(끝에서 이름 5 + 수 4 + stream 20 + 앞 4)를 크게 한다.
    CHECK(rejects([](Array<uint8>& b) noexcept {
        StoreLittleEndian<uint32>(b.Data() + b.Size() - 20 - 4 - 5 - 4, 0xffffu);
    }));
    // LOD 수 초과는 쓰기에서 막는다.
    SkeletalMeshRenderData many(allocator);
    for (uint32 index = 0; index <= SkeletalMeshMaxLods; ++index) {
        REQUIRE(many.lods.TryPushBack(SkinnedRenderLod(allocator)));
    }

    CHECK(WriteSkeletalMeshNuggetHeader(many, {}, names, header).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(header.IsEmpty());
}

// 처리기 등록과 정체성(정적 처리기와 다름).
TEST(SkeletalMeshCook_Registration) {
    CookScene scene;
    REQUIRE(scene.ready);
    DefaultCookProcessor fallback;
    CookProcessorTable table(scene.allocator, scene.types, fallback);
    SkeletalMeshCookProcessor processor;
    CHECK(RegisterSkeletalMeshCookProcessors(nullptr, table).Error() == ErrorCode::InvalidArgument);
    REQUIRE(RegisterSkeletalMeshCookProcessors(&processor, table));
    auto found = table.Find(TypeIdOf<SkeletalMesh>());
    REQUIRE(found);
    CHECK(found.Value() == &processor);
    CHECK(processor.Identity().processor != StaticMeshCookProcessorId.processor &&
          SkeletalMeshNuggetProcessorId.version != StaticMeshNuggetProcessorId.version);
}

} // namespace skeletal_mesh_cook_tests_file
