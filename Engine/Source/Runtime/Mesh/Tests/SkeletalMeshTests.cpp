#include <AssetRuntime/Tests/AssetTestSupport.h>
#include <Mesh/SkeletalMesh/SkeletalMesh.generated.h>
#include <Mesh/SkeletalMesh/SkeletalMesh.h>
#include <Mesh/SkeletalMesh/SkeletalMeshValidation.h>
#include <MeshSource/Tests/MeshSourceTestSupport.h>
#include <TestSupport/Test.h>

// 스킨 메시 에셋 타입 시험이다(Docs/Design/MeshAnimationAssets 6.3절·6.7.1절): UE 기본값(메시·원본
// LOD· 가중치 프로필 정보·PerPlatformBool), 생성 LOD 판정, 패키지 왕복(에디터·쿠킹 — EditorOnly
// 원본 LOD와 프로파일별 맵이 쿠킹에서 빠짐, 본 계층·슬롯·소켓·프로필·렌더 데이터·inverse-bind bulk
// 보존, 다시 쓰기 byte 같음).
using namespace rocks;
namespace skeletal_mesh_tests_file {

inline TypeId SkeletalMeshType() noexcept {
    return GeneratedType<SkeletalMesh>::Id();
}

// UE 기본값(USkeletalMesh 생성자 — MinLod 0, FSkeletalMeshLODInfo ScreenSize
// 1·WeightOfPrioritization 1, FSkinWeightProfileInfo, FPerPlatformBool).
TEST(SkeletalMesh_DefaultsMatchUe) {
    SystemAllocator allocator;
    SkeletalMesh mesh(allocator);
    CHECK(mesh.skeleton.Handle() == ObjectHandle{} && mesh.bones.bones.IsEmpty());
    CHECK(mesh.sourceLods.IsEmpty() && mesh.materialSlots.Size() == 0 && mesh.sockets.IsEmpty() &&
          mesh.skinWeightProfiles.IsEmpty());
    CHECK(mesh.minLod.defaultValue == 0 && mesh.minLod.perPlatform.IsEmpty());
    CHECK(mesh.positiveBoundsExtension.x == 0.0 && mesh.renderData.lods.IsEmpty() &&
          mesh.renderData.inverseBindMatrices.IsEmpty());
    const SkeletalMeshSourceLod lod(allocator);
    CHECK(!lod.hasSource && !lod.hasBakedSource && lod.screenSize.defaultValue == 1.0f &&
          lod.lodHysteresis == 0.0f && lod.lodMaterialMap.IsEmpty() &&
          lod.bonesToRemove.IsEmpty() && lod.bonesToPrioritize.IsEmpty() &&
          lod.weightOfPrioritization == 1.0f);
    CHECK(!lod.build.useHighPrecisionSkinWeights && lod.build.boneInfluenceLimit == 0);
    // UE FSkeletalMeshOptimizationSettings 생성자(SkeletalMeshReductionSettings.h:179).
    const SkeletalMeshReductionSettings& reduction = lod.reduction;
    CHECK(reduction.terminationCriterion == SkeletalMeshReductionTermination::Triangles &&
          reduction.percentTriangles == 0.5f && reduction.percentVertices == 0.5f &&
          reduction.maxTriangles == 4 && reduction.maxVertices == 6 &&
          reduction.maxTrianglesFromPercent == NumericLimits<uint32>::Max() &&
          reduction.maxVerticesFromPercent == NumericLimits<uint32>::Max());
    CHECK(reduction.maxDeviationPercent == 0.5f &&
          reduction.reductionMethod == SkeletalMeshReductionMethod::Triangles &&
          reduction.skinningImportance == MeshFeatureImportance::Normal &&
          !reduction.remapMorphTargets && reduction.recalculateNormals &&
          reduction.weldingThreshold == 0.1f && reduction.normalsThreshold == 60.0f &&
          reduction.maxBonesPerVertex == 4 && !reduction.enforceBoneBoundaries &&
          reduction.mergeCoincidentVertexBones && reduction.volumeImportance == 1.0f &&
          !reduction.lockEdges && !reduction.lockColorBoundaries &&
          reduction.improveTrianglesForCloth && reduction.baseLod == 0);
    CHECK(mesh.sectionInfo.IsEmpty() && !mesh.virtualGeometry.enabled);
    const SkinWeightProfileInfo profile(allocator);
    CHECK(profile.name.IsNone() && !profile.defaultProfile.defaultValue &&
          profile.defaultProfileFromLod.defaultValue == 0);
    const SkinnedRenderLod render(allocator);
    CHECK(render.influencesPerVertex == 0 && render.boneMap.IsEmpty() &&
          render.morphTargets.IsEmpty() && render.skinWeightProfiles.IsEmpty());
}

// 생성 LOD(UE IsReductionActive): 원본이 없으면 생성, inline 축소(baseLod == LOD)면 원본 수로 판정
// — UE 구조체 기본(비율 0.5)은 축소이고 비율 1이면 아니다, 절대 기준은 수가 상한보다 클 때.
// inline이 아니면 수를 모르는 것으로 본다(비율 기준은 비율만, 절대 기준은 늘 축소).
TEST(SkeletalMesh_LodGenerated) {
    SystemAllocator allocator;
    SkeletalMeshSourceLod lod(allocator);
    CHECK(IsSkeletalMeshLodGenerated(lod, 0));
    lod.hasSource = true;
    lod.source.vertexCount = 100;
    lod.source.triangleCount = 50;
    CHECK(IsSkeletalMeshLodGenerated(lod, 0));
    lod.reduction.percentTriangles = 1.0f;
    CHECK(!IsSkeletalMeshLodGenerated(lod, 0));
    lod.reduction.maxTrianglesFromPercent = 40;
    CHECK(IsSkeletalMeshLodGenerated(lod, 0));
    lod.reduction.maxTrianglesFromPercent = NumericLimits<uint32>::Max();
    lod.reduction.terminationCriterion = SkeletalMeshReductionTermination::AbsoluteVertices;
    lod.reduction.maxVertices = 100;
    CHECK(!IsSkeletalMeshLodGenerated(lod, 0));
    lod.reduction.maxVertices = 99;
    CHECK(IsSkeletalMeshLodGenerated(lod, 0));
    // inline이 아니면(baseLod 0, LOD 1) 절대 기준은 늘 축소, 비율 기준은 비율만 본다.
    lod.reduction.maxVertices = 1000;
    CHECK(IsSkeletalMeshLodGenerated(lod, 1));
    lod.reduction.terminationCriterion = SkeletalMeshReductionTermination::TrianglesOrVertices;
    lod.reduction.percentVertices = 1.0f;
    CHECK(!IsSkeletalMeshLodGenerated(lod, 1));
}

// 에디터·쿠킹 패키지 왕복이다.
TEST(SkeletalMesh_PackageRoundTripResaveIdentical) {
    const PackageKind kinds[] = {PackageKind::Editor, PackageKind::Cooked};
    for (PackageKind kind : kinds) {
        asset_test::AssetEnv env;
        REQUIRE(env.Ready());
        ObjectRegistry& registry = *env.source;
        auto package =
            NewPackage(registry, registry.OwnerAccess(), asset_test::FixedId(0x5c0), kind);
        REQUIRE(package);
        const ObjectHandle handle = asset_test::Make<SkeletalMesh>(registry, SkeletalMeshType(),
                                                                   package.Value().Handle(), "");
        SkeletalMesh* mesh = asset_test::Pointer<SkeletalMesh>(registry, handle);
        REQUIRE(mesh != nullptr);
        auto name = [&](StringView text) noexcept { return env.names.TryIntern(text).Value(); };
        BoneInfo root(env.allocator);
        root.name = name("root");
        REQUIRE(root.exportName.TryAssign("Root"));
        BoneInfo spine(env.allocator);
        spine.name = name("spine");
        spine.parentIndex = 0;
        REQUIRE(mesh->bones.bones.TryPushBack(Move(root)));
        REQUIRE(mesh->bones.bones.TryPushBack(Move(spine)));
        REQUIRE(mesh->bones.referencePose.TryPushBack(Transform::Identity()));
        REQUIRE(mesh->bones.referencePose.TryPushBack(
            Transform(Quatd::Identity(), Vec3d(0.0, 0.0, 10.0), Vec3d::One())));
        MemoryBulkSource bulk(env.allocator);
        MeshSource quad(env.allocator);
        REQUIRE(mesh_source_test::BuildQuad(quad));
        SkeletalMeshSourceLod lod(env.allocator);
        lod.hasSource = true;
        REQUIRE(quad.Store(bulk, env.names, lod.source));
        lod.build.useHighPrecisionSkinWeights = true;
        lod.lodHysteresis = 0.02f;
        lod.reduction.percentTriangles = 0.25f;
        lod.weightOfPrioritization = 2.0f;
        REQUIRE(lod.bonesToRemove.TryPushBack(name("spine")));
        REQUIRE(lod.bonesToPrioritize.TryPushBack(name("root")));
        REQUIRE(lod.lodMaterialMap.TryPushBack(SkeletalMeshNoMaterialRemap));
        REQUIRE(lod.screenSize.perPlatform.Insert(name("Android"), 0.5f));
        REQUIRE(mesh->sourceLods.TryPushBack(Move(lod)));
        BoneSocket socket;
        socket.name = name("Hand");
        socket.boneName = name("spine");
        socket.relativeLocation = Vec3d(1.0, 2.0, 3.0);
        REQUIRE(mesh->sockets.TryPushBack(socket));
        SkinWeightProfileInfo profile(env.allocator);
        profile.name = name("Bulky");
        profile.defaultProfile.defaultValue = true;
        REQUIRE(profile.defaultProfile.perPlatform.Insert(name("Android"), false));
        REQUIRE(mesh->skinWeightProfiles.TryPushBack(Move(profile)));
        REQUIRE(asset_test::WithBorrow<SkeletalMesh>(
            registry, handle, [&](SkeletalMesh& value) noexcept {
                auto added =
                    registry.AddElement(registry.OwnerAccess(), handle, value.materialSlots);
                if (!added) {
                    return false;
                }

                value.materialSlots[added.Value()].slotName = name("Skin");
                return true;
            }));
        mesh->minLod.defaultValue = 1;
        REQUIRE(mesh->minLod.perPlatform.Insert(name("Android"), 2));
        // 쿠킹 렌더 데이터: LOD 덩어리(빈 byte 192)와 inverse-bind(본 둘).
        Array<uint8> chunk(env.allocator);
        REQUIRE(chunk.TryResize(192));
        auto chunkRef = bulk.Add(Move(chunk));
        Array<uint8> binds(env.allocator);
        REQUIRE(binds.TryResize(2 * SkinnedInverseBindMatrixBytes));
        auto bindRef = bulk.Add(Move(binds));
        REQUIRE(chunkRef && bindRef);
        SkinnedRenderLod render(env.allocator);
        render.screenSize = 1.0f;
        render.lodHysteresis = 0.02f;
        render.data = chunkRef.Value();
        render.influencesPerVertex = 4;
        REQUIRE(render.boneMap.TryPushBack(1));
        REQUIRE(render.requiredBones.TryPushBack(0));
        REQUIRE(render.requiredBones.TryPushBack(1));
        SkinnedMorphTarget morph;
        morph.name = name("Smile");
        morph.deltaCount = 2;
        morph.deltas = MeshRenderStream{0, 48, 40};
        REQUIRE(render.morphTargets.TryPushBack(morph));
        SkinWeightProfileRenderLod profileLod;
        profileLod.name = name("Bulky");
        REQUIRE(render.skinWeightProfiles.TryPushBack(profileLod));
        REQUIRE(mesh->renderData.lods.TryPushBack(Move(render)));
        mesh->renderData.inverseBindMatrices = bindRef.Value();
        mesh->renderData.sphereRadius = 10.0;
        mesh->renderData.box = Box<double>(Vec3d(-1.0, -2.0, -3.0), Vec3d(1.0, 2.0, 3.0));
        mesh->positiveBoundsExtension = Vec3d(0.0, 0.0, 4.0);
        MeshSectionInfoEntry entry;
        entry.info.castShadow = false;
        REQUIRE(mesh->sectionInfo.TryPushBack(entry));
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
        SkeletalMesh* loaded = asset_test::Pointer<SkeletalMesh>(*env.target, found.Value());
        Package* loadedPackage = asset_test::Pointer<Package>(*env.target, handles[0]);
        REQUIRE(loaded != nullptr && loadedPackage != nullptr);
        const bool editor = kind == PackageKind::Editor;
        CHECK(loaded->sourceLods.Size() == (editor ? 1u : 0u) &&
              loaded->sectionInfo.Size() == (editor ? 1u : 0u));
        CHECK(loaded->bones.bones[0].exportName.View() ==
              (editor ? StringView("Root") : StringView()));
        CHECK(loaded->positiveBoundsExtension.z == 4.0 && loaded->renderData.box.min.y == -2.0);
        REQUIRE(loaded->bones.bones.Size() == 2);
        CHECK(loaded->bones.bones[1].name == name("spine") &&
              loaded->bones.bones[1].parentIndex == 0 &&
              loaded->bones.referencePose[1].translation.z == 10.0);
        CHECK(loaded->minLod.defaultValue == 1 &&
              loaded->minLod.perPlatform.Count() == (editor ? 1u : 0u));
        REQUIRE(loaded->sockets.Size() == 1 && loaded->skinWeightProfiles.Size() == 1);
        CHECK(loaded->sockets[0].boneName == name("spine") &&
              loaded->sockets[0].relativeLocation.z == 3.0);
        CHECK(loaded->skinWeightProfiles[0].defaultProfile.defaultValue &&
              loaded->skinWeightProfiles[0].defaultProfile.perPlatform.Count() ==
                  (editor ? 1u : 0u));
        REQUIRE(loaded->materialSlots.Size() == 1);
        CHECK(loaded->materialSlots[0].slotName == name("Skin"));
        if (editor) {
            const SkeletalMeshSourceLod& read = loaded->sourceLods[0];
            CHECK(read.hasSource && read.build.useHighPrecisionSkinWeights &&
                  read.lodHysteresis == 0.02f && read.lodMaterialMap.Size() == 1 &&
                  read.lodMaterialMap[0] == SkeletalMeshNoMaterialRemap &&
                  read.screenSize.Resolve(name("Android")) == 0.5f);
            CHECK(read.reduction.percentTriangles == 0.25f && read.weightOfPrioritization == 2.0f &&
                  read.bonesToRemove.Size() == 1 && read.bonesToRemove[0] == name("spine") &&
                  read.bonesToPrioritize.Size() == 1 && !loaded->sectionInfo[0].info.castShadow);
            MeshSource readMesh(env.allocator);
            REQUIRE(readMesh.Load(read.source, *loadedPackage, env.names));
            CHECK(mesh_source_test::SameMesh(quad, readMesh));
        }

        const SkeletalMeshRenderData& renderData = loaded->renderData;
        REQUIRE(renderData.lods.Size() == 1);
        const SkinnedRenderLod& lodRead = renderData.lods[0];
        CHECK(lodRead.influencesPerVertex == 4 && lodRead.boneMap.Size() == 1 &&
              lodRead.boneMap[0] == 1 && lodRead.requiredBones.Size() == 2 &&
              lodRead.lodHysteresis == 0.02f);
        REQUIRE(lodRead.morphTargets.Size() == 1 && lodRead.skinWeightProfiles.Size() == 1);
        CHECK(lodRead.morphTargets[0].name == name("Smile") &&
              lodRead.morphTargets[0].deltaCount == 2 &&
              lodRead.morphTargets[0].deltas.byteSize == 40 &&
              lodRead.skinWeightProfiles[0].name == name("Bulky"));
        CHECK(renderData.inverseBindMatrices.rawBytes == 2 * SkinnedInverseBindMatrixBytes &&
              renderData.sphereRadius == 10.0);
        auto contents =
            ReadPackage(env.allocator, first.View(), asset_test::LoadOptions(kind).read);
        REQUIRE(contents);
        const Array<uint8> resaved =
            asset_test::ResaveAsRead(env.allocator, *env.target, handles[0], contents.Value());
        CHECK(asset_test::SameBytes(first.View(), resaved.View()));
    }
}

} // namespace skeletal_mesh_tests_file
