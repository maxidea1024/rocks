#include "../../AssetDatabase/Tests/AssetDatabaseTestSupport.h"
#include "../../../Runtime/MeshSource/Tests/MeshSourceTestSupport.h"

#include <AssetNugget/NuggetCache.h>
#include <AssetNugget/NuggetCacheSettings.h>
#include <AssetNugget/NuggetScc.h>
#include <AssetRuntime/MemoryBulkSource.h>
#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/PackageLoad.h>
#include <AssetRuntime/PackageReader.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/NamePool.h>
#include <GraphicsTypes/VertexFormat.h>
#include <Mesh/StaticMesh/StaticMesh.generated.h>
#include <MeshTools/StaticMeshCook.h>
#include <Object/ConstructionContext.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/ObjectRegistryOptions.h>
#include <Platform/File.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

// 정적 메시 쿠킹 처리기 시험이다(Docs/Design/MeshAnimationAssets 5.5.2절): 쿠킹 패키지(EditorOnly
// 없음·renderData 검증 통과·LOD 덩어리 bulk의 index·자동 화면 크기·대상 프로파일 값), 서버 프로파일
// (stream 없음, 섹션·bounds 남음), GL 색 형식, LOD 입력(원본 없는 LOD 빼기·경고·번호 당기기, 축소
// 활성 원본 빌드·경고, baked 원본), 실패(LOD 없음·LOD 수 초과·타입)와 에디터 객체 되돌리기, 너겟
// (미적중 → Put → 적중·verify·손상 적중 미적중), 너겟 머리 왕복·손상, 자동 화면 크기, 처리기 등록.
using namespace rocks;
namespace static_mesh_cook_tests_file {

using graphics::VertexFormat;
using rocks::asset_database_test::TempProject;

namespace {

AssetId MeshId(uint64 seed) noexcept {
    return AssetId{Guid{0x4d45534843000000ull, seed}};
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
                                                       ContextId{0x4d45534bull, 1}, options);
        auto other = UniquePtr<ObjectRegistry>::TryMake(allocator, allocator, types,
                                                        ContextId{0x4d45534bull, 2}, options);
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

    // 새 에디터 패키지 id에 원본 LOD lodCount개(원본 있음 — 사각형)의 정적 메시를 만든다.
    bool Make(uint64 id, uint32 lodCount = 1) noexcept {
        package.Reset();
        asset = MeshId(id);
        auto made = NewPackage(*editor, EditorAccess(), asset, PackageKind::Editor);
        if (!made) {
            return false;
        }

        package = Move(made.Value());
        auto created = editor->NewObject<StaticMesh>(EditorAccess(), TypeIdOf<StaticMesh>(),
                                                     package.Handle(), "");
        if (!created) {
            return false;
        }

        mesh = created.Value().Handle();
        MeshSource quad(allocator);
        if (!mesh_source_test::BuildQuad(quad)) {
            return false;
        }

        return Edit([&](StaticMesh& value) noexcept {
            // 슬롯 하나(importedSlotName None — 묶음의 None 이름과 맞는다, 대체 경고 없음).
            if (!editor->AddElement(EditorAccess(), mesh, value.materialSlots)) {
                return false;
            }

            for (uint32 index = 0; index < lodCount; ++index) {
                StaticMeshSourceLod lod(allocator);
                lod.hasSource = true;
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
        auto borrowed = editor->ResolveForWrite<StaticMesh>(EditorAccess(), mesh);
        if (!borrowed) {
            return false;
        }

        auto value = borrowed.Value().Get();
        return value && fn(*value.Value());
    }

    // 에디터 객체가 쿠킹 전 값 그대로인지다(renderData 비어 있음·플랫폼 값).
    bool EditorUntouched(int32 minLod = 0, int32 lodForCollision = 0) noexcept {
        return Edit([&](StaticMesh& value) noexcept {
            return value.renderData.lods.IsEmpty() && value.renderData.sphereRadius == 0.0 &&
                   value.minLod.defaultValue == minLod &&
                   value.numStreamedLods.defaultValue == StaticMeshGroupStreamedLods &&
                   value.lodForCollision == lodForCollision;
        });
    }

    Result<void> Cook(const StaticMeshCookProcessor& processor, CookOut& out,
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

    const StaticMesh* LoadedMesh(const Loaded& loaded) noexcept {
        auto borrowed = cooked->Resolve<StaticMesh>(cooked->OwnerAccess(), loaded.mesh);
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

// 쿠킹·로드해 메시를 돌려준다(실패하면 null). 로드한 메시는 ValidateStaticMesh를 통과해야 한다.
const StaticMesh* CookAndLoad(CookScene& scene, const StaticMeshCookProcessor& processor,
                              CookOut& out, Loaded& loaded) noexcept {
    if (!scene.Cook(processor, out) || !scene.Load(out.sink.Bytes(), loaded)) {
        return nullptr;
    }

    const StaticMesh* mesh = scene.LoadedMesh(loaded);
    const Package* package = scene.LoadedPackage(loaded);
    if (mesh == nullptr || package == nullptr ||
        !ValidateStaticMesh(*mesh, *package, scene.allocator)) {
        return nullptr;
    }

    for (usize index = 0; index < mesh->renderData.lods.Size(); ++index) {
        const StaticMeshRenderLod& lod = mesh->renderData.lods[index];
        if (lod.data.IsEmpty()) {
            continue;
        }

        auto bytes = package->BulkBytes(lod.data);
        if (!bytes || !ValidateStaticMeshIndices(lod, static_cast<uint32>(index), bytes.Value())) {
            return nullptr;
        }
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

// 쿠킹 패키지: renderData LOD 하나(검증·index 통과, LOD 덩어리 bulk 4 KiB 정렬), EditorOnly(원본
// LOD) 없음, 자동 화면 크기 2.0, bounds는 사각형(확장 없음), 에디터 객체 되돌림.
TEST(StaticMeshCook_CookedPackage) {
    CookScene scene;
    REQUIRE(scene.ready);
    REQUIRE(scene.Make(1));
    REQUIRE(scene.Edit([](StaticMesh& value) noexcept {
        value.positiveBoundsExtension = Vec3d(0.0, 0.0, 10.0);
        return true;
    }));
    StaticMeshCookProcessor processor;
    CookOut out(scene.allocator);
    Loaded loaded;
    const StaticMesh* mesh = CookAndLoad(scene, processor, out, loaded);
    REQUIRE(mesh != nullptr);
    CHECK(processor.Builds() == 1 && out.warnings.IsEmpty());
    CHECK(mesh->sourceLods.IsEmpty());
    REQUIRE(mesh->renderData.lods.Size() == 1);
    const StaticMeshRenderLod& lod = mesh->renderData.lods[0];
    CHECK(lod.screenSize == StaticMeshLod0ScreenSize && lod.vertexCount == 4 &&
          lod.sections.Size() == 1 && lod.sections[0].triangleCount == 2);
    CHECK(lod.positions.vertexFormat == graphics::VertexFormatNumber(VertexFormat::Float3) &&
          lod.colors.vertexFormat == MeshRenderStreamNone && !lod.data.IsEmpty());
    CHECK(mesh->renderData.box.max.x == 100.0 && mesh->renderData.box.max.z == 0.0);
    CHECK(StaticMeshExtendedBounds(*mesh).box.max.z == 10.0);
    CHECK(scene.EditorUntouched());
}

// 대상 프로파일 값: minLod·numStreamedLods는 프로파일 값, 화면 크기(자동 끔)도 프로파일 값.
TEST(StaticMeshCook_ProfileValues) {
    CookScene scene;
    REQUIRE(scene.ready);
    REQUIRE(scene.Make(2, 2));
    const Name windows = scene.names.TryIntern("Windows").Value();
    REQUIRE(scene.Edit([&](StaticMesh& value) noexcept {
        value.autoComputeLodScreenSize = false;
        value.sourceLods[0].screenSize.defaultValue = 1.0f;
        value.sourceLods[1].screenSize.defaultValue = 0.5f;
        value.minLod.defaultValue = 0;
        return value.sourceLods[1].screenSize.perPlatform.Insert(windows, 0.3f).IsOk() &&
               value.minLod.perPlatform.Insert(windows, 1).IsOk() &&
               value.numStreamedLods.perPlatform.Insert(windows, 1).IsOk();
    }));
    StaticMeshCookProcessor processor;
    CookOut out(scene.allocator);
    Loaded loaded;
    const StaticMesh* mesh = CookAndLoad(scene, processor, out, loaded);
    REQUIRE(mesh != nullptr);
    REQUIRE(mesh->renderData.lods.Size() == 2);
    CHECK(mesh->renderData.lods[0].screenSize == 1.0f &&
          mesh->renderData.lods[1].screenSize == 0.3f);
    CHECK(mesh->minLod.defaultValue == 1 && mesh->minLod.perPlatform.IsEmpty() &&
          mesh->numStreamedLods.defaultValue == 1);
    CHECK(scene.EditorUntouched());
}

// 서버 프로파일(backend 없음): stream·data 없음, 섹션·정점 수·bounds 남음.
TEST(StaticMeshCook_ServerProfile) {
    CookScene scene("");
    REQUIRE(scene.ready);
    REQUIRE(scene.Make(3));
    StaticMeshCookProcessor processor;
    CookOut out(scene.allocator);
    Loaded loaded;
    const StaticMesh* mesh = CookAndLoad(scene, processor, out, loaded);
    REQUIRE(mesh != nullptr);
    REQUIRE(mesh->renderData.lods.Size() == 1);
    const StaticMeshRenderLod& lod = mesh->renderData.lods[0];
    CHECK(lod.data.IsEmpty() && lod.positions.byteSize == 0 && lod.indices.byteSize == 0 &&
          lod.texCoords.IsEmpty() && lod.vertexCount == 4 && lod.sections.Size() == 1);
    CHECK(mesh->renderData.box.max.y == 100.0);
}

// GL 프로파일은 색을 UByte4Unorm으로 쓴다(색 있는 메시 — 흰색 아님).
TEST(StaticMeshCook_GlColorFormat) {
    CookScene scene("OpenGL");
    REQUIRE(scene.ready);
    REQUIRE(scene.Make(4));
    REQUIRE(scene.Edit([&](StaticMesh& value) noexcept {
        MeshSource quad(scene.allocator);
        if (!mesh_source_test::BuildQuad(quad)) {
            return false;
        }

        auto color = quad.AddAttribute(MeshElement::Corner, MeshAttributeName::Color,
                                       MeshValueType::Vec4, 1);
        if (!color) {
            return false;
        }

        const float values[4] = {0.5f, 0.25f, 0.0f, 1.0f};
        Span<float> colors = quad.FloatValues(color.Value(), 0);
        if (colors.Size() != 16) {
            return false;
        }

        for (usize at = 0; at < colors.Size(); ++at) {
            colors[at] = values[at % 4];
        }

        return quad.Store(scene.sourceBulk, scene.names, value.sourceLods[0].source).IsOk();
    }));
    StaticMeshCookProcessor processor;
    CookOut out(scene.allocator);
    Loaded loaded;
    const StaticMesh* mesh = CookAndLoad(scene, processor, out, loaded);
    REQUIRE(mesh != nullptr);
    CHECK(mesh->renderData.lods[0].colors.vertexFormat ==
          graphics::VertexFormatNumber(VertexFormat::UByte4Unorm));
    CHECK(StaticMeshColorFormat(scene.profile) == MeshColorFormat::UByte4Unorm);
}

// LOD 입력: 원본·baked 없는 LOD는 빠지고(경고, 번호 당김, minLod·lodForCollision 옮김), 축소 활성
// 원본은 축소 없이 빌드(경고), baked 원본은 크기·재계산 없이 빌드.
TEST(StaticMeshCook_LodInputs) {
    CookScene scene;
    REQUIRE(scene.ready);
    REQUIRE(scene.Make(5, 3));
    REQUIRE(scene.Edit([](StaticMesh& value) noexcept {
        value.sourceLods[1].hasSource = false;
        value.sourceLods[2].reduction.percentTriangles = 0.5f;
        value.minLod.defaultValue = 2;
        value.lodForCollision = 5;
        return true;
    }));
    StaticMeshCookProcessor processor;
    CookOut out(scene.allocator);
    Loaded loaded;
    const StaticMesh* mesh = CookAndLoad(scene, processor, out, loaded);
    REQUIRE(mesh != nullptr);
    CHECK(mesh->renderData.lods.Size() == 2 && processor.Builds() == 2);
    CHECK(CountWarnings(out, AssetDiagnosticCode::UnsupportedSetting) == 2);
    // 원본 LOD 2 → 쿠킹 LOD 1, 범위 밖 5 → 마지막.
    CHECK(mesh->minLod.defaultValue == 1 && mesh->lodForCollision == 1);
    // 자동 화면 크기: 2.0, 0.75.
    CHECK(mesh->renderData.lods[1].screenSize == 0.75f);
    CHECK(scene.EditorUntouched(2, 5));

    // baked: 생성 LOD에 baked가 있으면 그것을 쓴다(크기 1 — 원본 buildScale을 무시).
    CookScene baked;
    REQUIRE(baked.ready);
    REQUIRE(baked.Make(6, 1));
    REQUIRE(baked.Edit([&](StaticMesh& value) noexcept {
        StaticMeshSourceLod& lod = value.sourceLods[0];
        lod.hasSource = false;
        lod.hasBakedSource = true;
        lod.build.buildScale = Vec3d(2.0, 2.0, 2.0);
        MeshSource quad(baked.allocator);
        return mesh_source_test::BuildQuad(quad) &&
               quad.Store(baked.sourceBulk, baked.names, lod.bakedSource).IsOk();
    }));
    CookOut bakedOut(baked.allocator);
    Loaded bakedLoaded;
    const StaticMesh* bakedMesh = CookAndLoad(baked, processor, bakedOut, bakedLoaded);
    REQUIRE(bakedMesh != nullptr);
    CHECK(bakedMesh->renderData.box.max.x == 100.0 && bakedOut.warnings.IsEmpty());
}

// 실패: 쓸 LOD 없음 InvalidSource, 원본 LOD 9개 InvalidSetting. 에디터 객체는 그대로다.
TEST(StaticMeshCook_Failures) {
    CookScene scene;
    REQUIRE(scene.ready);
    StaticMeshCookProcessor processor;
    REQUIRE(scene.Make(7, 1));
    REQUIRE(scene.Edit([](StaticMesh& value) noexcept {
        value.sourceLods[0].hasSource = false;
        return true;
    }));
    CookOut none(scene.allocator);
    auto failed = scene.Cook(processor, none);
    CHECK(!failed && none.failure.code == AssetDiagnosticCode::InvalidSource);
    CHECK(scene.EditorUntouched());
    REQUIRE(scene.Make(8, StaticMeshMaxLods + 1));
    CookOut many(scene.allocator);
    auto tooMany = scene.Cook(processor, many);
    CHECK(!tooMany && many.failure.code == AssetDiagnosticCode::InvalidSetting);
    CHECK(scene.EditorUntouched());
}

// 너겟: 미적중 → Put → 적중(빌드 없음, 같은 패키지 byte) → verify(다시 빌드, 일치) → 손상 적중은
// 미적중으로 다시 빌드한다.
TEST(StaticMeshCook_Nuggets) {
    CookScene scene;
    REQUIRE(scene.ready);
    NuggetScene nuggets(scene.allocator, "static-mesh-cook-nuggets");
    REQUIRE(nuggets.ready);
    StaticMeshCookProcessor processor;
    REQUIRE(scene.Make(9, 2));
    CookOut first(scene.allocator);
    REQUIRE(scene.Cook(processor, first, nuggets.cache.Get()));
    CHECK(processor.Builds() == 2);
    const Sha256Digest key = processor.LastNuggetKey();
    CookOut second(scene.allocator);
    REQUIRE(scene.Cook(processor, second, nuggets.cache.Get()));
    CHECK(processor.Builds() == 2);
    CHECK(first.sink.Bytes().Size() == second.sink.Bytes().Size() &&
          platform::MemoryEquals(first.sink.Bytes().Data(), second.sink.Bytes().Data(),
                                 first.sink.Bytes().Size()));
    CookOut verify(scene.allocator);
    REQUIRE(scene.Cook(processor, verify, nuggets.cache.Get(), true));
    CHECK(processor.Builds() == 4);
    CHECK(scene.EditorUntouched());

    // 손상 적중(머리만 있고 LOD byte 없음)은 받지 않는다.
    Array<NuggetValue> broken(scene.allocator);
    NuggetValue head(scene.allocator);
    head.id = NuggetSccValueId(0, 0);
    REQUIRE(head.bytes.TryAppend(Span<const uint8>(reinterpret_cast<const uint8*>("bad"), 3)));
    REQUIRE(broken.TryPushBack(Move(head)));
    REQUIRE(nuggets.Replace(key, scene.asset, broken));
    CookOut rebuilt(scene.allocator);
    REQUIRE(scene.Cook(processor, rebuilt, nuggets.cache.Get()));
    CHECK(processor.Builds() == 6);
    CHECK(rebuilt.sink.Bytes().Size() == first.sink.Bytes().Size());
}

// 너겟 머리 왕복과 손상(버전·LOD 수·남는 byte·flag 값).
TEST(StaticMeshCook_NuggetHeader) {
    SystemAllocator allocator;
    StaticMeshRenderData data(allocator);
    StaticMeshRenderLod lod(allocator);
    lod.vertexCount = 4;
    lod.data.rawBytes = 128;
    lod.positions = MeshRenderStream{3, 0, 48};
    REQUIRE(lod.texCoords.TryPushBack(MeshRenderStream{14, 80, 32}));
    MeshRenderSection section;
    section.triangleCount = 2;
    section.maxVertex = 3;
    section.forceOpaque = true;
    REQUIRE(lod.sections.TryPushBack(section));
    REQUIRE(data.lods.TryPushBack(Move(lod)));
    data.box = Box<double>(Vec3d(-1.0, -2.0, -3.0), Vec3d(1.0, 2.0, 3.0));
    data.sphereRadius = 4.0;
    const MeshCookWarning warnings[1] = {{AssetDiagnosticCode::MaterialSlotFallback, 0}};
    Array<uint8> header(allocator);
    REQUIRE(WriteStaticMeshNuggetHeader(data, Span<const MeshCookWarning>(warnings), header));
    StaticMeshRenderData read(allocator);
    Array<MeshCookWarning> readWarnings(allocator);
    REQUIRE(ReadStaticMeshNuggetHeader(header.View(), read, readWarnings));
    REQUIRE(read.lods.Size() == 1 && readWarnings.Size() == 1);
    CHECK(readWarnings[0].code == AssetDiagnosticCode::MaterialSlotFallback);
    CHECK(read.box.min.z == -3.0 && read.sphereRadius == 4.0 && read.lods[0].vertexCount == 4 &&
          read.lods[0].data.rawBytes == 128 && read.lods[0].texCoords.Size() == 1 &&
          read.lods[0].texCoords[0].offset == 80 && read.lods[0].sections[0].forceOpaque);
    // 손상: 버전, LOD 수(9), 마지막 byte 더함·뺌, 섹션 flag 2.
    auto rejects = [&](void (*mutate)(Array<uint8>&) noexcept) noexcept {
        Array<uint8> broken(allocator);
        CHECK(broken.TryAppend(header.View()));
        mutate(broken);
        StaticMeshRenderData out(allocator);
        Array<MeshCookWarning> outWarnings(allocator);
        return ReadStaticMeshNuggetHeader(broken.View(), out, outWarnings).Error() ==
               ErrorCode::CorruptData;
    };
    CHECK(rejects([](Array<uint8>& b) noexcept { b[0] = 9; }));
    CHECK(rejects([](Array<uint8>& b) noexcept { b[4] = 9; }));
    CHECK(rejects([](Array<uint8>& b) noexcept { (void)b.TryPushBack(0); }));
    CHECK(rejects([](Array<uint8>& b) noexcept { (void)b.TryPopBack(); }));
    CHECK(rejects([](Array<uint8>& b) noexcept { b[b.Size() - 1] = 2; }));
}

// 자동 화면 크기(UE 식 — 2.0, 0.75^i)와 색·장치 판정, 처리기 등록.
TEST(StaticMeshCook_HelpersAndRegistration) {
    float sizes[4] = {};
    StaticMeshAutoScreenSizes(Span<float>(sizes, 4));
    CHECK(sizes[0] == 2.0f && sizes[1] == 0.75f && sizes[2] == 0.5625f && sizes[3] == 0.421875f);
    CookScene scene;
    REQUIRE(scene.ready);
    CHECK(StaticMeshProfileHasDevice(scene.profile) &&
          StaticMeshColorFormat(scene.profile) == MeshColorFormat::Bgra8Unorm);
    DefaultCookProcessor fallback;
    CookProcessorTable table(scene.allocator, scene.types, fallback);
    StaticMeshCookProcessor processor;
    CHECK(RegisterStaticMeshCookProcessors(nullptr, table).Error() == ErrorCode::InvalidArgument);
    REQUIRE(RegisterStaticMeshCookProcessors(&processor, table));
    auto found = table.Find(TypeIdOf<StaticMesh>());
    REQUIRE(found);
    CHECK(found.Value() == &processor);
}

// 너겟 키: 슬롯 이름·섹션 정보·빌드 설정·장치 유무는 키를 바꾸고, 화면 크기·minLod·bounds 확장은
// 바꾸지 않는다. 적중에서도 빌드 경고(슬롯 대체)가 미적중과 같은 순서로 나온다. 서버 프로파일도
// 적중한다(값은 머리 하나).
TEST(StaticMeshCook_NuggetKeysAndWarnings) {
    CookScene scene;
    REQUIRE(scene.ready);
    NuggetScene nuggets(scene.allocator, "static-mesh-cook-keys");
    REQUIRE(nuggets.ready);
    StaticMeshCookProcessor processor;
    REQUIRE(scene.Make(10, 1));
    const Name fallback = scene.names.TryIntern("Missing").Value();
    REQUIRE(scene.Edit([&](StaticMesh& value) noexcept {
        value.materialSlots[0].importedSlotName = fallback;
        return true;
    }));
    CookOut miss(scene.allocator);
    REQUIRE(scene.Cook(processor, miss, nuggets.cache.Get()));
    const Sha256Digest base = processor.LastNuggetKey();
    CHECK(CountWarnings(miss, AssetDiagnosticCode::MaterialSlotFallback) == 1);
    CookOut hit(scene.allocator);
    REQUIRE(scene.Cook(processor, hit, nuggets.cache.Get()));
    CHECK(processor.Builds() == 1);
    REQUIRE(hit.warnings.Size() == miss.warnings.Size());
    for (usize index = 0; index < hit.warnings.Size(); ++index) {
        CHECK(hit.warnings[index].code == miss.warnings[index].code &&
              hit.warnings[index].setting == miss.warnings[index].setting);
    }

    auto keyAfter = [&](auto&& edit) noexcept {
        CHECK(scene.Edit(edit));
        CookOut out(scene.allocator);
        CHECK(scene.Cook(processor, out, nuggets.cache.Get()));
        return processor.LastNuggetKey();
    };
    // 바꾸지 않는 값.
    CHECK(keyAfter([](StaticMesh& value) noexcept {
              value.sourceLods[0].screenSize.defaultValue = 0.5f;
              value.autoComputeLodScreenSize = false;
              value.minLod.defaultValue = 1;
              value.positiveBoundsExtension = Vec3d(1.0, 2.0, 3.0);
              return true;
          }) == base);
    // 바꾸는 값.
    const Sha256Digest slotKey = keyAfter([](StaticMesh& value) noexcept {
        value.materialSlots[0].importedSlotName = Name{};
        return true;
    });
    CHECK(slotKey != base);
    const Sha256Digest sectionKey = keyAfter([](StaticMesh& value) noexcept {
        MeshSectionInfoEntry entry;
        entry.info.forceOpaque = true;
        return value.sectionInfo.TryPushBack(entry).IsOk();
    });
    CHECK(sectionKey != slotKey);
    const Sha256Digest settingKey = keyAfter([](StaticMesh& value) noexcept {
        value.sourceLods[0].build.useFullPrecisionUVs = true;
        return true;
    });
    CHECK(settingKey != sectionKey);
    scene.profile.graphicsBackends.Clear();
    const Sha256Digest serverKey = keyAfter([](StaticMesh&) noexcept { return true; });
    CHECK(serverKey != settingKey);
    const uint64 builds = processor.Builds();
    CookOut serverHit(scene.allocator);
    REQUIRE(scene.Cook(processor, serverHit, nuggets.cache.Get()));
    CHECK(processor.Builds() == builds);
    Loaded loaded;
    REQUIRE(scene.Load(serverHit.sink.Bytes(), loaded));
    const StaticMesh* mesh = scene.LoadedMesh(loaded);
    REQUIRE(mesh != nullptr && mesh->renderData.lods.Size() == 1);
    CHECK(mesh->renderData.lods[0].data.IsEmpty() &&
          mesh->renderData.lods[0].sections[0].forceOpaque);
}

} // namespace static_mesh_cook_tests_file
