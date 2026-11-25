#include <AssetRuntime/Tests/AssetTestSupport.h>
#include <Mesh/Common/MeshReductionSettings.h>
#include <Mesh/StaticMesh/StaticMesh.generated.h>
#include <Mesh/StaticMesh/StaticMesh.h>
#include <Mesh/StaticMesh/StaticMeshValidation.h>
#include <MeshSource/Tests/MeshSourceTestSupport.h>
#include <TestSupport/Test.h>

// 정적 메시 에셋 타입 시험이다(Docs/Design/MeshAnimationAssets 5.1절·5.5.1절): UE 생성자
// 기본값(메시· 축소·가상 기하·섹션 정보·소켓·UV 밀도), 축소 활성·생성 LOD 판정(UE 기본 축소기 식),
// 패키지 왕복 (에디터·쿠킹 — EditorOnly 필드와 프로파일별 맵이 쿠킹에서 빠짐, 원본 메시 bulk·렌더
// 데이터·bounds 보존, 다시 쓰기 byte 같음).
using namespace rocks;
namespace static_mesh_tests_file {

inline TypeId StaticMeshType() noexcept {
    return GeneratedType<StaticMesh>::Id();
}

// UE 생성자 기본값(StaticMesh.cpp UStaticMesh·FMeshSectionInfo·UStaticMeshSocket·
// FMeshReductionSettings·FMeshNaniteSettings·FMeshUVChannelInfo).
TEST(StaticMesh_DefaultsMatchUe) {
    SystemAllocator allocator;
    StaticMesh mesh(allocator);
    CHECK(mesh.lightMapResolution == 4 && mesh.lightMapCoordinateIndex == 0);
    CHECK(mesh.minLod.defaultValue == 0 && mesh.numStreamedLods.defaultValue == -1);
    CHECK(mesh.minLod.perPlatform.IsEmpty() && mesh.numStreamedLods.perPlatform.IsEmpty());
    CHECK(mesh.autoComputeLodScreenSize && mesh.lodGroup.IsNone());
    CHECK(mesh.lodForCollision == 0 && !mesh.doubleSidedGeometry &&
          mesh.complexity == CollisionComplexity::Default);
    CHECK(mesh.sourceLods.IsEmpty() && mesh.sectionInfo.IsEmpty() && mesh.sockets.IsEmpty() &&
          mesh.materialSlots.Size() == 0 && mesh.renderData.lods.IsEmpty());
    CHECK(mesh.renderData.sphereRadius == 0.0 && mesh.positiveBoundsExtension.x == 0.0);
    CHECK(mesh.neverStream == 0);

    const MeshSectionInfo info;
    CHECK(info.materialIndex == 0 && info.enableCollision && info.castShadow &&
          info.visibleInRayTracing && info.affectDistanceFieldLighting && !info.forceOpaque);
    const MeshRenderSection section;
    CHECK(section.enableCollision && section.castShadow && section.visibleInRayTracing &&
          section.affectDistanceFieldLighting && !section.forceOpaque);
    MeshSocket socket(allocator);
    CHECK(socket.relativeScale.x == 1.0 && socket.relativeScale.y == 1.0 &&
          socket.relativeScale.z == 1.0 && socket.relativeLocation.z == 0.0 &&
          socket.relativeRotation.yaw == 0.0 && !socket.createdAtImport && socket.tag.IsEmpty());
    const MeshUvDensity density;
    CHECK(!density.initialized && !density.overrideDensities);
    for (float value : density.localUvDensities) {
        CHECK(value == 0.0f);
    }

    const MeshReductionSettings reduction;
    CHECK(reduction.percentTriangles == 1.0f && reduction.percentVertices == 1.0f);
    CHECK(reduction.maxTriangles == 0xFFFFFFFFu && reduction.maxVertices == 0xFFFFFFFFu);
    CHECK(reduction.maxDeviation == 0.0f && reduction.pixelError == 8.0f &&
          reduction.weldingThreshold == 0.0f && reduction.hardAngleThreshold == 80.0f &&
          reduction.baseLod == 0);
    CHECK(reduction.silhouetteImportance == MeshFeatureImportance::Normal &&
          reduction.textureImportance == MeshFeatureImportance::Normal &&
          reduction.shadingImportance == MeshFeatureImportance::Normal &&
          reduction.visibilityAggressiveness == MeshFeatureImportance::Lowest &&
          reduction.vertexColorImportance == MeshFeatureImportance::Off);
    CHECK(!reduction.recalculateNormals && !reduction.generateUniqueLightmapUVs &&
          !reduction.keepSymmetry && !reduction.visibilityAided && !reduction.cullOccluded &&
          reduction.terminationCriterion == MeshReductionTermination::Triangles);

    const VirtualGeometrySettings geometry;
    CHECK(!geometry.enabled && !geometry.explicitTangents && geometry.lerpUvs &&
          !geometry.separable && geometry.voxelNdf && !geometry.voxelOpacity);
    CHECK(geometry.shapePreservation == VirtualGeometryShapePreservation::None);
    CHECK(geometry.positionPrecision == NumericLimits<int32>::Min() &&
          geometry.normalPrecision == -1 && geometry.tangentPrecision == -1 &&
          geometry.boneWeightPrecision == -1 && geometry.targetMinimumResidencyInKb == 0);
    CHECK(geometry.keepPercentTriangles == 1.0f && geometry.trimRelativeError == 0.0f &&
          geometry.generateFallback == VirtualGeometryFallbackMode::PlatformDefault &&
          geometry.fallbackTarget == VirtualGeometryFallbackTarget::Auto &&
          geometry.fallbackPercentTriangles == 1.0f && geometry.fallbackRelativeError == 1.0f);
    CHECK(geometry.maxEdgeLengthFactor == 0.0f && geometry.numRays == 64 &&
          geometry.voxelLevel == 0 && geometry.rayBackUp == 0.0f &&
          geometry.displacementUvChannel == 0);

    StaticMeshSourceLod lod(allocator);
    CHECK(!lod.hasSource && !lod.hasBakedSource && lod.screenSize.defaultValue == 0.0f);
}

// 축소 활성(UE 기본 축소기 — 종료 기준별, 비율 < 1 − 1e-4 또는 최대 수 < 원본 수).
TEST(StaticMesh_ReductionActive) {
    MeshReductionSettings settings;
    CHECK(!IsMeshReductionActive(settings, 100, 200));
    settings.percentTriangles = 0.99995f;
    CHECK(!IsMeshReductionActive(settings, 100, 200));
    settings.percentTriangles = 0.9998f;
    CHECK(IsMeshReductionActive(settings, 100, 200));
    settings.percentTriangles = 1.0f;
    settings.maxTriangles = 200;
    CHECK(!IsMeshReductionActive(settings, 100, 200));
    settings.maxTriangles = 199;
    CHECK(IsMeshReductionActive(settings, 100, 200));
    // 삼각형 기준은 정점 칸을 보지 않는다.
    settings.maxTriangles = 0xFFFFFFFFu;
    settings.percentVertices = 0.5f;
    settings.maxVertices = 1;
    CHECK(!IsMeshReductionActive(settings, 100, 200));
    settings.terminationCriterion = MeshReductionTermination::Vertices;
    CHECK(IsMeshReductionActive(settings, 100, 200));
    settings.percentVertices = 1.0f;
    CHECK(IsMeshReductionActive(settings, 100, 200));
    settings.maxVertices = 100;
    CHECK(!IsMeshReductionActive(settings, 100, 200));
    settings.percentTriangles = 0.5f;
    CHECK(!IsMeshReductionActive(settings, 100, 200));
    settings.terminationCriterion = MeshReductionTermination::Any;
    CHECK(IsMeshReductionActive(settings, 100, 200));
    settings.percentTriangles = 1.0f;
    CHECK(!IsMeshReductionActive(settings, 100, 200));
    settings.maxVertices = 99;
    CHECK(IsMeshReductionActive(settings, 100, 200));
}

// 생성 LOD = 원본 없음 또는 축소 활성(원본 정점·삼각형 수로 판정).
TEST(StaticMesh_LodGenerated) {
    SystemAllocator allocator;
    StaticMeshSourceLod lod(allocator);
    CHECK(IsStaticMeshLodGenerated(lod));
    lod.hasSource = true;
    lod.source.vertexCount = 4;
    lod.source.triangleCount = 2;
    CHECK(!IsStaticMeshLodGenerated(lod));
    lod.reduction.maxTriangles = 1;
    CHECK(IsStaticMeshLodGenerated(lod));
    lod.reduction.maxTriangles = 2;
    CHECK(!IsStaticMeshLodGenerated(lod));
    lod.hasBakedSource = true;
    lod.hasSource = false;
    CHECK(IsStaticMeshLodGenerated(lod));
}

// 정상 렌더 데이터 하나를 채운다(정점 4, UV 한 쌍, 색 없음, 16비트 index 6개 — 구간은 배치 순서).
void FillRenderData(StaticMeshRenderData& data, BulkDataRef ref) noexcept {
    StaticMeshRenderLod lod(data.lods.GetAllocator());
    lod.screenSize = 2.0f;
    lod.vertexCount = 4;
    lod.data = ref;
    lod.positions = MeshRenderStream{3, 0, 48};
    lod.tangentBasis = MeshRenderStream{5, 48, 32};
    (void)lod.texCoords.TryPushBack(MeshRenderStream{14, 80, 32});
    lod.indexFormat = 1;
    lod.indices = MeshRenderStream{0, 112, 12};
    MeshRenderSection section;
    section.triangleCount = 2;
    section.maxVertex = 3;
    section.forceOpaque = true;
    (void)lod.sections.TryPushBack(section);
    (void)data.lods.TryPushBack(Move(lod));
    data.box = Box<double>(Vec3d(0.0, 0.0, 0.0), Vec3d(100.0, 100.0, 0.0));
    data.sphereRadius = 70.71067811865476;
}

// 에디터·쿠킹 패키지 왕복: 원본 LOD(사각형 메시 bulk)·섹션 정보·소켓·슬롯·가상 기하·플랫폼 값·
// 렌더 데이터를 쓰고 다른 Registry에 읽는다. 쿠킹은 EditorOnly(원본 LOD·섹션 정보·importedSlotName·
// 소켓 createdAtImport·가상 기하·perPlatform 맵)가 빠진다. 다시 쓴 byte가 같다.
TEST(StaticMesh_PackageRoundTripResaveIdentical) {
    const PackageKind kinds[] = {PackageKind::Editor, PackageKind::Cooked};
    for (PackageKind kind : kinds) {
        asset_test::AssetEnv env;
        REQUIRE(env.Ready());
        ObjectRegistry& registry = *env.source;
        auto package =
            NewPackage(registry, registry.OwnerAccess(), asset_test::FixedId(0x7a0), kind);
        REQUIRE(package);
        const ObjectHandle handle =
            asset_test::Make<StaticMesh>(registry, StaticMeshType(), package.Value().Handle(), "");
        StaticMesh* mesh = asset_test::Pointer<StaticMesh>(registry, handle);
        REQUIRE(mesh != nullptr);
        auto name = [&](StringView text) noexcept { return env.names.TryIntern(text).Value(); };
        MemoryBulkSource bulk(env.allocator);
        MeshSource quad(env.allocator);
        REQUIRE(mesh_source_test::BuildQuad(quad));
        StaticMeshSourceLod lod(env.allocator);
        lod.hasSource = true;
        REQUIRE(quad.Store(bulk, env.names, lod.source));
        lod.build.removeDegenerates = false;
        lod.reduction.pixelError = 4.0f;
        lod.screenSize.defaultValue = 1.0f;
        REQUIRE(lod.screenSize.perPlatform.Insert(name("Android"), 0.5f));
        REQUIRE(mesh->sourceLods.TryPushBack(Move(lod)));
        MeshSectionInfoEntry entry;
        entry.lod = 0;
        entry.section = 0;
        entry.info.castShadow = false;
        REQUIRE(mesh->sectionInfo.TryPushBack(entry));
        MeshSocket socket(env.allocator);
        socket.name = name("Muzzle");
        socket.relativeLocation = Vec3d(50.0, 0.0, 10.0);
        socket.relativeRotation = Rotator<double>(0.0, 90.0, 0.0);
        socket.createdAtImport = true;
        REQUIRE(socket.tag.TryAppend("barrel"));
        REQUIRE(mesh->sockets.TryPushBack(Move(socket)));
        REQUIRE(
            asset_test::WithBorrow<StaticMesh>(registry, handle, [&](StaticMesh& value) noexcept {
                auto added =
                    registry.AddElement(registry.OwnerAccess(), handle, value.materialSlots);
                if (!added) {
                    return false;
                }

                MeshMaterialSlot& slot = value.materialSlots[added.Value()];
                slot.slotName = name("Body");
                slot.importedSlotName = name("Body_Imported");
                slot.uvDensity.initialized = true;
                slot.uvDensity.localUvDensities[2] = 3.5f;
                return true;
            }));
        mesh->virtualGeometry.enabled = true;
        mesh->autoComputeLodScreenSize = false;
        mesh->lodGroup = name("SmallProp");
        mesh->lodForCollision = 1;
        mesh->doubleSidedGeometry = true;
        mesh->lightMapResolution = 64;
        mesh->negativeBoundsExtension = Vec3d(0.0, 0.0, 5.0);
        mesh->minLod.defaultValue = 1;
        REQUIRE(mesh->minLod.perPlatform.Insert(name("Android"), 2));
        Array<uint8> chunk(env.allocator);
        REQUIRE(chunk.TryResize(124));
        auto ref = bulk.Add(Move(chunk));
        REQUIRE(ref);
        FillRenderData(mesh->renderData, ref.Value());
        PackageWriteOptions options =
            kind == PackageKind::Cooked ? asset_test::CookedOptions() : asset_test::EditorOptions();
        options.bulkSource = &bulk;
        Array<uint8> first =
            asset_test::Write(env.allocator, registry, package.Value().Handle(), options);
        REQUIRE(!first.IsEmpty());
        const Array<uint8> again =
            asset_test::Write(env.allocator, registry, package.Value().Handle(), options);
        CHECK(asset_test::SameBytes(first.View(), again.View()));
        Array<uint8> files[1] = {asset_test::Copy(env.allocator, first.View())};
        RootRef<Object> roots[1];
        ObjectHandle handles[1];
        REQUIRE(asset_test::LoadAll(env, *env.target, files, kind, nullptr, roots, handles));
        auto found = env.target->FindObject(env.target->OwnerAccess(), handles[0], StringView());
        REQUIRE(found);
        StaticMesh* loaded = asset_test::Pointer<StaticMesh>(*env.target, found.Value());
        Package* loadedPackage = asset_test::Pointer<Package>(*env.target, handles[0]);
        REQUIRE(loaded != nullptr && loadedPackage != nullptr);
        const bool editor = kind == PackageKind::Editor;
        CHECK(loaded->sourceLods.Size() == (editor ? 1u : 0u));
        CHECK(loaded->sectionInfo.Size() == (editor ? 1u : 0u));
        CHECK(loaded->virtualGeometry.enabled == editor);
        CHECK(loaded->autoComputeLodScreenSize == !editor);
        CHECK(loaded->lodGroup.IsNone() == !editor);
        CHECK(loaded->minLod.defaultValue == 1 &&
              loaded->minLod.perPlatform.Count() == (editor ? 1u : 0u));
        CHECK(loaded->lodForCollision == 1 && loaded->doubleSidedGeometry &&
              loaded->lightMapResolution == 64 && loaded->negativeBoundsExtension.z == 5.0);
        REQUIRE(loaded->sockets.Size() == 1);
        CHECK(loaded->sockets[0].name == name("Muzzle") &&
              loaded->sockets[0].relativeRotation.yaw == 90.0 &&
              loaded->sockets[0].createdAtImport == editor &&
              loaded->sockets[0].tag.View() == StringView("barrel"));
        REQUIRE(loaded->materialSlots.Size() == 1);
        CHECK(loaded->materialSlots[0].slotName == name("Body") &&
              loaded->materialSlots[0].importedSlotName.IsNone() == !editor &&
              loaded->materialSlots[0].uvDensity.initialized &&
              loaded->materialSlots[0].uvDensity.localUvDensities[2] == 3.5f &&
              loaded->materialSlots[0].material.Handle() == ObjectHandle{});
        if (editor) {
            const StaticMeshSourceLod& read = loaded->sourceLods[0];
            CHECK(read.hasSource && !read.hasBakedSource && !read.build.removeDegenerates &&
                  read.reduction.pixelError == 4.0f && read.screenSize.defaultValue == 1.0f &&
                  read.screenSize.Resolve(name("Android")) == 0.5f);
            MeshSource readMesh(env.allocator);
            REQUIRE(readMesh.Load(read.source, *loadedPackage, env.names));
            CHECK(mesh_source_test::SameMesh(quad, readMesh));
            CHECK(!loaded->sectionInfo[0].info.castShadow);
        }

        const StaticMeshRenderData& render = loaded->renderData;
        REQUIRE(render.lods.Size() == 1);
        CHECK(render.box.max.y == 100.0 && render.sphereRadius == 70.71067811865476);
        CHECK(render.lods[0].indices.offset == 112 && render.lods[0].texCoords.Size() == 1 &&
              render.lods[0].sections[0].forceOpaque && render.lods[0].data.rawBytes == 124);
        CHECK(ValidateStaticMeshRenderData(render));
        MeshDiagnostic meshFailure;
        CHECK(ValidateStaticMesh(*loaded, *loadedPackage, env.allocator, &meshFailure));
        auto contents =
            ReadPackage(env.allocator, first.View(), asset_test::LoadOptions(kind).read);
        REQUIRE(contents);
        const Array<uint8> resaved =
            asset_test::ResaveAsRead(env.allocator, *env.target, handles[0], contents.Value());
        CHECK(asset_test::SameBytes(first.View(), resaved.View()));
    }
}

// ValidateStaticMesh: 단순 충돌 위반은 collisionFailure에 채우고 meshFailure는 건드리지 않는다.
// 렌더 데이터 위반은 meshFailure다.
TEST(StaticMesh_ValidateReportsCollisionAndRenderFailures) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& registry = *env.source;
    auto package = NewPackage(registry, registry.OwnerAccess(), asset_test::FixedId(0x7a1),
                              PackageKind::Cooked);
    REQUIRE(package);
    const ObjectHandle handle =
        asset_test::Make<StaticMesh>(registry, StaticMeshType(), package.Value().Handle(), "");
    StaticMesh* mesh = asset_test::Pointer<StaticMesh>(registry, handle);
    Package* owner = asset_test::Pointer<Package>(registry, package.Value().Handle());
    REQUIRE(mesh != nullptr && owner != nullptr);
    MeshDiagnostic meshFailure;
    CollisionDiagnostic collisionFailure;
    CHECK(ValidateStaticMesh(*mesh, *owner, env.allocator, &meshFailure, &collisionFailure));
    CollisionSphere sphere;
    sphere.radius = -1.0f;
    REQUIRE(mesh->simpleCollision.spheres.TryPushBack(sphere));
    CHECK(!ValidateStaticMesh(*mesh, *owner, env.allocator, &meshFailure, &collisionFailure));
    CHECK(collisionFailure.code == AssetDiagnosticCode::InvalidCollisionData &&
          meshFailure.code == AssetDiagnosticCode::None);
    mesh->simpleCollision.spheres.Clear();
    mesh->renderData.sphereRadius = -1.0;
    collisionFailure = CollisionDiagnostic{};
    CHECK(!ValidateStaticMesh(*mesh, *owner, env.allocator, &meshFailure, &collisionFailure));
    CHECK(meshFailure.code == AssetDiagnosticCode::CorruptPackage &&
          meshFailure.issue == MeshIssue::RenderBounds &&
          collisionFailure.code == AssetDiagnosticCode::None);
}

} // namespace static_mesh_tests_file
