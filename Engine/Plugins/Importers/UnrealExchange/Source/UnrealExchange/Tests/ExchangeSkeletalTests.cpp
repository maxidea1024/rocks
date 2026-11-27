#include "ExchangeMeshTestSupport.h"

#include <Base/InitializerList.h>
#include <UnrealExchange/ExchangeSkeletalMesh.h>
#include <UnrealExchange/ExchangeSkeletalMeshSettings.h>
#include <UnrealExchange/ExchangeSkeleton.h>
#include <UnrealExchange/ExchangeSkeletonSettings.h>
#include <stdio.h>

// 교환 스켈레톤·스킨 메시 → 임포트 명세 변환 시험이다(Docs/Design/UnrealExchange 6.4절 10번 —
// UX7b1): golden 스켈레톤 둘·스킨 메시 셋의 변환(본·리타깃·커브·슬롯·LOD 설정의 이름 바꾼 행과
// enum 대응·morph 속성 적재), 경로 편집한 사본으로 가상 본·소켓·가중치 프로필·BonesToRemove·
// LODMaterialMap·Unsupported·형식 거부.
using namespace rocks;
using namespace rocks::exchange_test;
namespace exchange_skeletal_tests_file {

namespace {

constexpr StringView ColumnSkeletonPath =
    "/Game/RxGolden/SK_Column/SkeletalMeshes/SK_Column_Skeleton.SK_Column_Skeleton";
constexpr StringView ColumnPath = "/Game/RxGolden/SK_Column/SkeletalMeshes/SK_Column.SK_Column";
constexpr StringView CubeSkeletonPath =
    "/Engine/EngineMeshes/SkeletalCube_Skeleton.SkeletalCube_Skeleton";
constexpr StringView CubePath = "/Engine/EngineMeshes/SkeletalCube.SkeletalCube";
constexpr StringView SoftTargetPath = "/Game/RxGolden/SK_SoftTarget.SK_SoftTarget";
constexpr StringView GridMaterialPath =
    "/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial";
// golden SK_Column 값이다(관절 둘 기둥 — Tip은 Root 위 100 cm, LOD 0 축소 설정).
constexpr double ColumnTipHeight = 100.0;
constexpr uint32 ColumnMaxTriangles = 4;
constexpr uint32 ColumnMaxVertices = 6;
constexpr double ColumnWelding = 0.1;
constexpr double ColumnNormalsThreshold = 60.0;
constexpr double ColumnHysteresis = 0.02;
constexpr uint32 ColumnVertices = 12;

// golden 묶음 에셋 하나와 그 blob byte다.
struct GoldenAsset {
    explicit GoldenAsset(Allocator& allocator) noexcept : bytes(allocator), blobs(allocator) {
    }

    uint32 index = ExchangeNoIndex;
    Array<Array<uint8>> bytes;
    Array<Span<const uint8>> blobs;
};

struct Golden {
    explicit Golden(Allocator& allocator) noexcept
        : bundle(allocator), columnSkeleton(allocator), column(allocator), cubeSkeleton(allocator),
          cube(allocator), softTarget(allocator) {
    }

    const ExchangeAsset& Asset(const GoldenAsset& item) const noexcept {
        return bundle.Assets()[item.index];
    }

    ExchangeBundle bundle;
    GoldenAsset columnSkeleton;
    GoldenAsset column;
    GoldenAsset cubeSkeleton;
    GoldenAsset cube;
    GoldenAsset softTarget;
};

bool LoadAsset(const ExchangeBundle& bundle, StringView path, GoldenAsset& out) noexcept {
    out.index = FindAsset(bundle, path);
    return out.index != ExchangeNoIndex && LoadBlobs(bundle, out.index, out.bytes, out.blobs);
}

bool OpenGolden(Allocator& allocator, Golden& golden) noexcept {
    StringBuilder<512> directory(allocator);
    ExchangeDiagnostics diagnostics(allocator);
    return FixtureSourcePath("Golden.rxbundle", directory) &&
           ReadExchangeBundle(platform::PhysicalFileSystem(), directory.View(), {}, golden.bundle,
                              diagnostics) &&
           LoadAsset(golden.bundle, ColumnSkeletonPath, golden.columnSkeleton) &&
           LoadAsset(golden.bundle, ColumnPath, golden.column) &&
           LoadAsset(golden.bundle, CubeSkeletonPath, golden.cubeSkeleton) &&
           LoadAsset(golden.bundle, CubePath, golden.cube) &&
           LoadAsset(golden.bundle, SoftTargetPath, golden.softTarget);
}

template <class Converted>
usize CountWarnings(const Converted& converted, AssetDiagnosticCode code, uint64 detail,
                    StringView key = {}) noexcept {
    usize count = 0;
    for (const ExchangeSettingWarning& warning : converted.Warnings()) {
        count += warning.code == code && warning.detail == detail &&
                         (key.IsEmpty() || warning.key == key)
                     ? 1
                     : 0;
    }

    return count;
}

template <class Converted>
void PrintWarnings(const Converted& converted) noexcept {
    for (const ExchangeSettingWarning& warning : converted.Warnings()) {
        printf("    warning %s detail %llu key %.*s\n", AssetDiagnosticName(warning.code),
               static_cast<unsigned long long>(warning.detail),
               static_cast<int>(warning.key.ByteCount()), warning.key.Data());
    }
}

uint64 SkeletonRow(StringView key) noexcept {
    const Span<const ExchangeSettingRow> rows = ExchangeSkeletonSettingRows();
    for (usize index = 0; index < rows.Size(); ++index) {
        if (rows[index].ueKey == key) {
            return index;
        }
    }

    return ExchangeUnknownSettingDetail;
}

uint64 MeshRow(StringView key) noexcept {
    const Span<const ExchangeSettingRow> rows = ExchangeSkeletalMeshSettingRows();
    for (usize index = 0; index < rows.Size(); ++index) {
        if (rows[index].ueKey == key) {
            return index;
        }
    }

    return ExchangeUnknownSettingDetail;
}

// 편집한 스켈레톤 변환이다.
struct SkeletonAttempt {
    explicit SkeletonAttempt(Allocator& allocator) noexcept
        : edited(allocator), skeleton(allocator) {
    }

    EditedAsset edited;
    ExchangeSkeleton skeleton;
    ExchangeMeshFailure failure;
    ErrorCode error = ErrorCode::None;
};

void RunSkeleton(MeshEnv& env, Allocator& allocator, const Golden& golden, const GoldenAsset& base,
                 Span<const JsonEdit> edits, SkeletonAttempt& attempt) noexcept {
    if (!Edit(allocator, golden.Asset(base), base.blobs.View(), edits, attempt.edited)) {
        attempt.error = ErrorCode::InvalidArgument;
        return;
    }

    ExchangeSkeletonInput input{&attempt.edited.asset, nullptr, &env.objects, env.access};
    auto built = attempt.skeleton.Build(input, attempt.failure);
    attempt.error = built ? ErrorCode::None : built.Error();
}

// 편집한 스킨 메시 변환이다.
struct MeshAttempt {
    explicit MeshAttempt(Allocator& allocator) noexcept : edited(allocator), mesh(allocator) {
    }

    EditedAsset edited;
    ExchangeSkeletalMesh mesh;
    ExchangeMeshFailure failure;
    ErrorCode error = ErrorCode::None;
};

void RunMesh(MeshEnv& env, Allocator& allocator, const Golden& golden, const GoldenAsset& base,
             Span<const JsonEdit> edits, MeshAttempt& attempt,
             const OrderedMap<String, AssetId>* references = nullptr) noexcept {
    if (!Edit(allocator, golden.Asset(base), base.blobs.View(), edits, attempt.edited)) {
        attempt.error = ErrorCode::InvalidArgument;
        return;
    }

    ExchangeSkeletalMeshInput input{&attempt.edited.asset, attempt.edited.blobs.View(), references,
                                    &env.objects, env.access};
    auto built = attempt.mesh.Build(input, attempt.failure);
    attempt.error = built ? ErrorCode::None : built.Error();
}

bool SkeletonRejects(MeshEnv& env, Allocator& allocator, const Golden& golden,
                     const GoldenAsset& base, const JsonEdit& edit, AssetDiagnosticCode code,
                     uint64 detail) noexcept {
    SkeletonAttempt attempt(allocator);
    RunSkeleton(env, allocator, golden, base, Span<const JsonEdit>(&edit, 1), attempt);
    const bool ok = attempt.error == ErrorCode::CorruptData && attempt.failure.code == code &&
                    attempt.failure.detail == detail;
    if (!ok) {
        printf("    skeleton %.*s: error %d code %s detail %llu\n",
               static_cast<int>(edit.path.ByteCount()), edit.path.Data(),
               static_cast<int>(attempt.error), AssetDiagnosticName(attempt.failure.code),
               static_cast<unsigned long long>(attempt.failure.detail));
    }

    return ok;
}

bool MeshRejects(MeshEnv& env, Allocator& allocator, const Golden& golden, const GoldenAsset& base,
                 const JsonEdit& edit, AssetDiagnosticCode code, uint64 detail) noexcept {
    MeshAttempt attempt(allocator);
    RunMesh(env, allocator, golden, base, Span<const JsonEdit>(&edit, 1), attempt);
    const bool ok = attempt.error == ErrorCode::CorruptData && attempt.failure.code == code &&
                    attempt.failure.detail == detail;
    if (!ok) {
        printf("    mesh %.*s: error %d code %s detail %llu\n",
               static_cast<int>(edit.path.ByteCount()), edit.path.Data(),
               static_cast<int>(attempt.error), AssetDiagnosticName(attempt.failure.code),
               static_cast<unsigned long long>(attempt.failure.detail));
    }

    return ok;
}

} // namespace

// golden 스켈레톤 둘: 본·부모·기준 pose·리타깃 방식·커브, 경고 없음.
TEST(UnrealExchange_SkeletonGolden) {
    SystemAllocator allocator;
    {
        MeshEnv env(allocator);
        REQUIRE(env.registered);
        Golden golden(allocator);
        REQUIRE(OpenGolden(allocator, golden));
        ExchangeSkeleton column(allocator);
        ExchangeMeshFailure failure;
        ExchangeSkeletonInput input{&golden.Asset(golden.columnSkeleton), nullptr, &env.objects,
                                    env.access};
        REQUIRE(column.Build(input, failure));
        const SkeletonImportSpec& spec = column.Spec();
        REQUIRE(spec.bones.Size() == 2);
        CHECK(spec.bones[0].name == "Root" && spec.bones[0].parentIndex == NoParentBone &&
              spec.bones[1].name == "Tip" && spec.bones[1].parentIndex == 0 &&
              Near(spec.bones[1].referencePose.translation.z, ColumnTipHeight) &&
              Near(spec.bones[1].referencePose.rotation.w, 1.0) &&
              Near(spec.bones[1].referencePose.scale.x, 1.0));
        REQUIRE(spec.retargetModes.Size() == 2);
        CHECK(spec.retargetModes[1] == BoneTranslationRetarget::Animation);
        REQUIRE(spec.curves.Size() == 1);
        CHECK(spec.curves[0].name == "Bulge" && spec.curves[0].isMorphTarget &&
              !spec.curves[0].isMaterial && spec.curves[0].linkedBones.IsEmpty() &&
              spec.curves[0].maxLod == 255);
        CHECK(spec.virtualBones.IsEmpty() && spec.sockets.IsEmpty() && spec.replaceOnReimport);
        CHECK(column.Warnings().IsEmpty());
        PrintWarnings(column);

        ExchangeSkeleton cube(allocator);
        ExchangeSkeletonInput cubeInput{&golden.Asset(golden.cubeSkeleton), nullptr, &env.objects,
                                        env.access};
        REQUIRE(cube.Build(cubeInput, failure));
        CHECK(cube.Spec().bones.Size() == 2 && cube.Spec().curves.IsEmpty() &&
              cube.Warnings().IsEmpty());
        // 두 번 부를 수 없다, 스켈레톤이 아닌 에셋.
        CHECK(cube.Build(cubeInput, failure).Error() == ErrorCode::InvalidArgument &&
              failure.code == AssetDiagnosticCode::InvalidSource);
        ExchangeSkeleton wrong(allocator);
        ExchangeSkeletonInput meshInput{&golden.Asset(golden.column), nullptr, &env.objects,
                                        env.access};
        CHECK(wrong.Build(meshInput, failure).Error() == ErrorCode::InvalidArgument);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 스켈레톤 편집: 가상 본·소켓·AssetUserData·Unsupported·형식 거부.
TEST(UnrealExchange_SkeletonEdits) {
    SystemAllocator allocator;
    {
        MeshEnv env(allocator);
        REQUIRE(env.registered);
        Golden golden(allocator);
        REQUIRE(OpenGolden(allocator, golden));
        constexpr StringView VirtualBones =
            R"([{"SourceBoneName": "Root", "TargetBoneName": "Tip", "VirtualBoneName": "VB Root_Tip"}])";
        constexpr StringView Sockets =
            R"([{"subobject": "SkeletalMeshSocket_0", "class": "/Script/Engine.SkeletalMeshSocket",)"
            R"( "properties": {"SocketName": "Hand", "BoneName": "Tip",)"
            R"( "RelativeLocation": {"X": 1, "Y": 2, "Z": 3},)"
            R"( "RelativeRotation": {"Pitch": 0, "Yaw": 90, "Roll": 0},)"
            R"( "RelativeScale": {"X": 2, "Y": 2, "Z": 2}, "bForceAlwaysAnimated": false}}])";
        constexpr StringView UserData =
            R"([{"subobject": "AnimCurveMetaData_0", "class": "/Script/Engine.AnimCurveMetaData",)"
            R"( "properties": {}}, {"subobject": "Extra", "class": "/Script/Foo.Bar", "properties": {}}])";
        const JsonEdit edits[] = {
            {"settings.VirtualBones", VirtualBones},
            {"settings.Sockets", Sockets},
            {"settings.AssetUserData", UserData},
            {"settings.SlotGroups", R"([{"GroupName": "Default"}])"},
            {"settings.bUnknownThing", "1"},
            {"data.bones.1.retargetMode", R"("OrientAndScale")"},
            {"data.curves.0.linkedBones", R"(["Tip"])"},
        };
        SkeletonAttempt attempt(allocator);
        RunSkeleton(env, allocator, golden, golden.columnSkeleton, edits, attempt);
        REQUIRE(attempt.error == ErrorCode::None);
        const SkeletonImportSpec& spec = attempt.skeleton.Spec();
        REQUIRE(spec.virtualBones.Size() == 1 && spec.sockets.Size() == 1);
        CHECK(spec.virtualBones[0].sourceBone == "Root" &&
              spec.virtualBones[0].targetBone == "Tip" &&
              spec.virtualBones[0].name == "VB Root_Tip");
        const SkeletonImportSocket& socket = spec.sockets[0];
        CHECK(socket.name == "Hand" && socket.boneName == "Tip" &&
              Near(socket.relativeLocation.z, 3.0) && Near(socket.relativeRotation.yaw, 90.0) &&
              Near(socket.relativeScale.y, 2.0) && !socket.forceAlwaysAnimated);
        CHECK(spec.retargetModes[1] == BoneTranslationRetarget::OrientAndScale);
        REQUIRE(spec.curves.Size() == 1 && spec.curves[0].linkedBones.Size() == 1);
        CHECK(spec.curves[0].linkedBones[0] == "Tip");
        CHECK(attempt.skeleton.Warnings().Size() == 3);
        CHECK(CountWarnings(attempt.skeleton, AssetDiagnosticCode::UnsupportedSetting,
                            SkeletonRow("AssetUserData"), "AssetUserData") == 1);
        CHECK(CountWarnings(attempt.skeleton, AssetDiagnosticCode::UnsupportedSetting,
                            SkeletonRow("SlotGroups")) == 1);
        CHECK(CountWarnings(attempt.skeleton, AssetDiagnosticCode::UnsupportedSetting,
                            ExchangeUnknownSettingDetail) == 1);
        PrintWarnings(attempt.skeleton);

        const struct {
            JsonEdit edit;
            uint64 detail;
        } rejects[] = {
            {{"data.bones.1.retargetMode", R"("Bogus")"}, 1},
            {{"data.bones.1.retargetMode", ""}, 1},
            {{"data.bones.0.pose.rotation", "[0, 0, 1]"}, 0},
            {{"data.bones.0.parent", R"("x")"}, 0},
            {{"data.bones", "{}"}, 0},
            {{"data.curves.0.maxLod", "256"}, 0},
            {{"data.curves.0.linkedBones", "[1]"}, 0},
            {{"data.curves", "{}"}, 0},
            {{"settings.VirtualBones", R"([{"SourceBoneName": "Root"}])"},
             SkeletonRow("VirtualBones")},
            {{"settings.Sockets", R"([{"subobject": "Shared"}])"}, SkeletonRow("Sockets")},
            {{"settings.AssetUserData", "{}"}, SkeletonRow("AssetUserData")},
        };
        for (const auto& item : rejects) {
            CHECK(SkeletonRejects(env, allocator, golden, golden.columnSkeleton, item.edit,
                                  AssetDiagnosticCode::CorruptBundle, item.detail));
        }

        // 번역 값 종류 위반(배열이어야 할 자리)은 InvalidSource다.
        SkeletonAttempt translate(allocator);
        const JsonEdit notArray[] = {{"settings.VirtualBones", "{}"}};
        RunSkeleton(env, allocator, golden, golden.columnSkeleton, notArray, translate);
        CHECK(translate.error == ErrorCode::CorruptData &&
              translate.failure.code == AssetDiagnosticCode::InvalidSource);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// golden 스킨 메시 셋: 본·LOD 원본(가중치·morph 속성)·LOD 설정(이름 바꾼 행·enum 대응)·슬롯·
// 스켈레톤 참조, 물리 에셋 경고.
TEST(UnrealExchange_SkeletalMeshGolden) {
    SystemAllocator allocator;
    {
        MeshEnv env(allocator);
        REQUIRE(env.registered);
        Golden golden(allocator);
        REQUIRE(OpenGolden(allocator, golden));
        OrderedMap<String, AssetId> references(allocator);
        const AssetId gridId{Guid{0x61, 0x62}};
        const AssetId skeletonId{Guid{0x71, 0x72}};
        String grid(allocator);
        String skeletonPath(allocator);
        REQUIRE(grid.TryAssign(GridMaterialPath) && skeletonPath.TryAssign(ColumnSkeletonPath));
        REQUIRE(references.Insert(Move(grid), gridId));
        REQUIRE(references.Insert(Move(skeletonPath), skeletonId));

        ExchangeSkeletalMesh column(allocator);
        ExchangeMeshFailure failure;
        ExchangeSkeletalMeshInput input{&golden.Asset(golden.column), golden.column.blobs.View(),
                                        &references, &env.objects, env.access};
        REQUIRE(column.Build(input, failure));
        const SkeletalMeshImportSpec& spec = column.Spec();
        REQUIRE(spec.bones.Size() == 2 && spec.lods.Size() == 1);
        CHECK(spec.bones[1].name == "Tip" && spec.bones[1].parentIndex == 0);
        const SkeletalMeshImportLod& lod = spec.lods[0];
        REQUIRE(lod.source != nullptr);
        CHECK(lod.source == column.LodSource(0) && lod.bakedSource == nullptr &&
              lod.source->Count(MeshElement::Vertex) == ColumnVertices);
        CHECK(lod.source->FindAttribute(MeshElement::Vertex, "SkinWeights") != MeshNoAttribute &&
              lod.source->FindAttribute(MeshElement::Vertex, "Morph-Bulge") != MeshNoAttribute);
        CHECK(!lod.build.recomputeNormals && lod.build.recomputeTangents &&
              lod.build.useMikkTSpace && lod.build.boneInfluenceLimit == 0);
        CHECK(lod.reduction.terminationCriterion == SkeletalMeshReductionTermination::Triangles &&
              lod.reduction.reductionMethod == SkeletalMeshReductionMethod::Triangles &&
              lod.reduction.maxTriangles == ColumnMaxTriangles &&
              lod.reduction.maxVertices == ColumnMaxVertices &&
              lod.reduction.maxTrianglesFromPercent == NumericLimits<uint32>::Max() &&
              Near(lod.reduction.weldingThreshold, ColumnWelding) &&
              Near(lod.reduction.normalsThreshold, ColumnNormalsThreshold) &&
              lod.reduction.mergeCoincidentVertexBones && !lod.reduction.lockColorBoundaries &&
              lod.reduction.recalculateNormals);
        CHECK(Near(lod.screenSize, 1.0) && Near(lod.lodHysteresis, ColumnHysteresis) &&
              lod.lodMaterialMap.IsEmpty() && lod.bonesToRemove.IsEmpty() &&
              Near(lod.weightOfPrioritization, 1.0));
        REQUIRE(spec.slots.Size() == 1);
        CHECK(spec.slots[0].material == gridId && spec.slots[0].uvDensity.initialized);
        CHECK(spec.skeleton == skeletonId && spec.sockets.IsEmpty() &&
              spec.skinWeightProfiles.IsEmpty() && spec.minLod == 0 &&
              !spec.virtualGeometry.enabled && spec.replaceSettingsOnReimport);
        // 물리 에셋(범위 밖)만 Rocks 필드가 없다.
        CHECK(column.Warnings().Size() == 1 &&
              CountWarnings(column, AssetDiagnosticCode::UnsupportedSetting,
                            MeshRow("PhysicsAsset"), "PhysicsAsset") == 1);
        PrintWarnings(column);

        // 참조가 없으면 스켈레톤·머티리얼을 비운다(서비스가 보조 스켈레톤을 만든다).
        for (const GoldenAsset* item : {&golden.cube, &golden.softTarget}) {
            ExchangeSkeletalMesh mesh(allocator);
            ExchangeSkeletalMeshInput other{&golden.Asset(*item), item->blobs.View(), nullptr,
                                            &env.objects, env.access};
            REQUIRE(mesh.Build(other, failure));
            CHECK(mesh.Spec().skeleton == AssetId{} && mesh.Spec().lods.Size() == 1 &&
                  mesh.Spec().lods[0].source != nullptr && mesh.Spec().bones.Size() == 2);
            // 옛 에셋(bUseBackwardsCompatibleF16TruncUVs 참)의 LOD 설정 하나만 Rocks 필드가 없다.
            CHECK(mesh.Warnings().Size() == 1 &&
                  CountWarnings(mesh, AssetDiagnosticCode::UnsupportedSetting,
                                MeshRow("SourceModels"),
                                "bUseBackwardsCompatibleF16TruncUVs") == 1);
            PrintWarnings(mesh);
        }

        // 호출 계약: blob 수·스킨 메시가 아님·두 번 부름.
        ExchangeSkeletalMesh contract(allocator);
        ExchangeSkeletalMeshInput noBlobs{
            &golden.Asset(golden.column), {}, nullptr, &env.objects, env.access};
        CHECK(contract.Build(noBlobs, failure).Error() == ErrorCode::InvalidArgument);
        ExchangeSkeletalMesh wrong(allocator);
        ExchangeSkeletalMeshInput skeleton{&golden.Asset(golden.columnSkeleton),
                                           golden.columnSkeleton.blobs.View(), nullptr,
                                           &env.objects, env.access};
        CHECK(wrong.Build(skeleton, failure).Error() == ErrorCode::InvalidArgument);
        CHECK(column.Build(input, failure).Error() == ErrorCode::InvalidArgument);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 스킨 메시 편집: enum 대응·LOD 목록·가중치 프로필·소켓·슬롯 멤버 경고·형식 거부.
TEST(UnrealExchange_SkeletalMeshEdits) {
    SystemAllocator allocator;
    {
        MeshEnv env(allocator);
        REQUIRE(env.registered);
        Golden golden(allocator);
        REQUIRE(OpenGolden(allocator, golden));
        constexpr StringView Profiles =
            R"([{"Name": "Alt", "DefaultProfile": {"Default": true, "PerPlatform": []},)"
            R"( "DefaultProfileFromLODIndex": {"Default": 1, "PerPlatform": [{"key": "Mobile",)"
            R"( "value": 2}]}, "PerLODSourceFiles": []}])";
        constexpr StringView Sockets =
            R"([{"subobject": "S", "class": "/Script/Engine.SkeletalMeshSocket", "properties":)"
            R"( {"SocketName": "Muzzle", "BoneName": "Tip"}}])";
        const JsonEdit edits[] = {
            {"settings.SourceModels.0.ReductionSettings.TerminationCriterion",
             R"("SMTC_AbsTriangleOrVert")"},
            {"settings.SourceModels.0.ReductionSettings.ReductionMethod", R"("SMOT_MaxDeviation")"},
            {"settings.SourceModels.0.ReductionSettings.SkinningImportance", R"("SMOI_Highest")"},
            {"settings.SourceModels.0.ReductionSettings.bLockColorBounaries", "true"},
            {"settings.SourceModels.0.ReductionSettings.BaseLOD", "1"},
            {"settings.SourceModels.0.BuildSettings.ThresholdUV", "0.5"},
            {"settings.SourceModels.0.BuildSettings.bOptimizeForInstancing", "true"},
            {"settings.SourceModels.0.LODMaterialMap", "[0, -1]"},
            {"settings.SourceModels.0.BonesToRemove", R"([{"BoneName": "Tip"}])"},
            {"settings.SourceModels.0.BonesToPrioritize", R"([{"BoneName": "Root"}])"},
            {"settings.SourceModels.0.WeightOfPrioritization", "2"},
            {"settings.SourceModels.0.SkinCacheUsage", R"("Enabled")"},
            {"settings.SkinWeightProfiles", Profiles},
            {"settings.Sockets", Sockets},
            {"settings.MinLod", R"({"Default": 1, "PerPlatform": []})"},
            {"settings.PositiveBoundsExtension", R"({"X": 1, "Y": 2, "Z": 3})"},
            {"settings.PhysicsAsset", "null"},
            {"data.materials.0.Extra", "1"},
        };
        MeshAttempt attempt(allocator);
        RunMesh(env, allocator, golden, golden.column, edits, attempt);
        REQUIRE(attempt.error == ErrorCode::None);
        const SkeletalMeshImportSpec& spec = attempt.mesh.Spec();
        const SkeletalMeshImportLod& lod = spec.lods[0];
        CHECK(lod.reduction.terminationCriterion ==
                  SkeletalMeshReductionTermination::AbsoluteTrianglesOrVertices &&
              lod.reduction.reductionMethod == SkeletalMeshReductionMethod::MaxDeviation &&
              lod.reduction.skinningImportance == MeshFeatureImportance::Highest &&
              lod.reduction.lockColorBoundaries && lod.reduction.baseLod == 1 &&
              Near(lod.build.thresholdUV, 0.5));
        REQUIRE(lod.lodMaterialMap.Size() == 2 && lod.bonesToRemove.Size() == 1 &&
                lod.bonesToPrioritize.Size() == 1);
        CHECK(lod.lodMaterialMap[1] == -1 && lod.bonesToRemove[0] == "Tip" &&
              lod.bonesToPrioritize[0] == "Root" && Near(lod.weightOfPrioritization, 2.0));
        REQUIRE(spec.skinWeightProfiles.Size() == 1 && spec.sockets.Size() == 1);
        CHECK(spec.skinWeightProfiles[0].name == "Alt" &&
              spec.skinWeightProfiles[0].defaultProfile &&
              spec.skinWeightProfiles[0].defaultProfileFromLod == 1);
        CHECK(spec.sockets[0].name == "Muzzle" && spec.sockets[0].boneName == "Tip" &&
              spec.sockets[0].forceAlwaysAnimated);
        CHECK(spec.minLod == 1 && Near(spec.positiveBoundsExtension.y, 2.0));
        // 경고: Rocks 필드 없는 빌드·LOD 설정(SourceModels 행 둘), PerPlatform 프로필 값, 슬롯
        // 모르는 멤버(data.materials).
        CHECK(CountWarnings(attempt.mesh, AssetDiagnosticCode::UnsupportedSetting,
                            MeshRow("SourceModels"), "bOptimizeForInstancing") == 1);
        CHECK(CountWarnings(attempt.mesh, AssetDiagnosticCode::UnsupportedSetting,
                            MeshRow("SourceModels"), "SkinCacheUsage") == 1);
        CHECK(CountWarnings(attempt.mesh, AssetDiagnosticCode::UnsupportedSetting,
                            MeshRow("SkinWeightProfiles"), "DefaultProfileFromLODIndex") == 1);
        CHECK(CountWarnings(attempt.mesh, AssetDiagnosticCode::UnsupportedSetting,
                            ExchangeSkeletalMeshMaterialsDetail, "Extra") == 1);
        CHECK(attempt.mesh.Warnings().Size() == 4);
        PrintWarnings(attempt.mesh);

        // 생성 LOD: 원본이 없고 baked 기록이 bakedSource다(UE 렌더 데이터 — 6.4절 2번).
        StringBuilder<> record(allocator);
        REQUIRE(ToText(golden.Asset(golden.column).data.Find("lods").At(0).Find("source"), {},
                       allocator, record));
        const JsonEdit generated[] = {{"data.lods.0.hasSource", "false"},
                                      {"data.lods.0.source", "null"},
                                      {"data.lods.0.generated", "true"},
                                      {"data.lods.0.baked", record.View()}};
        MeshAttempt bakedAttempt(allocator);
        RunMesh(env, allocator, golden, golden.column, generated, bakedAttempt);
        REQUIRE(bakedAttempt.error == ErrorCode::None);
        CHECK(bakedAttempt.mesh.Spec().lods[0].source == nullptr &&
              bakedAttempt.mesh.Spec().lods[0].bakedSource != nullptr &&
              bakedAttempt.mesh.Spec().lods[0].bakedSource == bakedAttempt.mesh.LodBakedSource(0));
        // 생성 LOD가 아닌데 baked 기록이 있으면 형식 위반이다.
        const JsonEdit notGenerated = {"data.lods.0.baked", record.View()};
        CHECK(MeshRejects(env, allocator, golden, golden.column, notGenerated,
                          AssetDiagnosticCode::CorruptBundle, 0));

        // data.materials가 없는 옛 묶음: 슬롯 없이 들이고 알린다. 모르는 data 키는 모르는 키
        // 경고다.
        const JsonEdit noMaterials[] = {{"data.materials", ""}, {"data.extra", "1"}};
        MeshAttempt oldBundle(allocator);
        RunMesh(env, allocator, golden, golden.column, noMaterials, oldBundle);
        REQUIRE(oldBundle.error == ErrorCode::None);
        CHECK(oldBundle.mesh.Spec().slots.IsEmpty() &&
              CountWarnings(oldBundle.mesh, AssetDiagnosticCode::UnsupportedSetting,
                            ExchangeSkeletalMeshMaterialsDetail, "materials") == 1 &&
              CountWarnings(oldBundle.mesh, AssetDiagnosticCode::UnsupportedSetting,
                            ExchangeUnknownSettingDetail, "extra") == 1);

        // 형식 거부(CorruptBundle).
        const struct {
            JsonEdit edit;
            uint64 detail;
        } rejects[] = {
            {{"data.lods", "[]"}, 0},
            {{"data.lods.0.source", "null"}, 0},
            {{"data.lods.0.hasSource", ""}, 0},
            {{"data.lods.0.baked", "1"}, 0},
            {{"data.bones.1.pose.scale", "[1, 1]"}, 1},
            {{"settings.SourceModels.0.LODMaterialMap", R"(["a"])"}, 0},
            {{"settings.SourceModels.0.BonesToRemove", R"([{"Bone": "Tip"}])"}, 0},
            {{"settings.SourceModels.0.LODHysteresis", R"("x")"}, 0},
            {{"settings.SkinWeightProfiles", R"([{"DefaultProfile": {"Default": true}}])"},
             MeshRow("SkinWeightProfiles")},
            {{"settings.SkinWeightProfiles",
              R"([{"Name": "A", "DefaultProfile": {"Default": 1}}])"},
             MeshRow("SkinWeightProfiles")},
            {{"settings.Sockets", R"([{"subobject": "Shared"}])"}, MeshRow("Sockets")},
            {{"settings.MinLod", R"({"Default": 0.5, "PerPlatform": []})"}, MeshRow("MinLod")},
            {{"settings.NegativeBoundsExtension", "0"}, MeshRow("NegativeBoundsExtension")},
            {{"data.materials.0.MaterialSlotName", "1"}, ExchangeSkeletalMeshMaterialsDetail},
        };
        for (const auto& item : rejects) {
            CHECK(MeshRejects(env, allocator, golden, golden.column, item.edit,
                              AssetDiagnosticCode::CorruptBundle, item.detail));
        }

        // enum 대응에 없는 값·값 형식은 번역·ImportJson 거부(InvalidSource)다.
        const JsonEdit invalid[] = {
            {"settings.SourceModels.0.ReductionSettings.TerminationCriterion", R"("SMTC_Nope")"},
            {"settings.SourceModels.0.ReductionSettings.ReductionMethod", "1"},
            {"settings.SourceModels.0.BuildSettings.ThresholdUV", R"("x")"},
            {"settings.NaniteSettings.NumRays", R"("many")"},
        };
        for (const JsonEdit& edit : invalid) {
            MeshAttempt rejected(allocator);
            RunMesh(env, allocator, golden, golden.column, Span<const JsonEdit>(&edit, 1),
                    rejected);
            CHECK(rejected.error == ErrorCode::CorruptData &&
                  rejected.failure.code == AssetDiagnosticCode::InvalidSource);
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 표: 행 수·UE 키 중복 없음·명세로 옮기는 행 번호.
TEST(UnrealExchange_SkeletalSettingRows) {
    const Span<const ExchangeSettingRow> skeleton = ExchangeSkeletonSettingRows();
    const Span<const ExchangeSettingRow> mesh = ExchangeSkeletalMeshSettingRows();
    CHECK(skeleton.Size() == ExchangeSkeletonSettingRowCount &&
          mesh.Size() == ExchangeSkeletalMeshSettingRowCount);
    for (const Span<const ExchangeSettingRow> rows : {skeleton, mesh}) {
        for (usize index = 0; index < rows.Size(); ++index) {
            for (usize other = 0; other < index; ++other) {
                CHECK(rows[index].ueKey != rows[other].ueKey);
            }
        }
    }

    CHECK(skeleton[static_cast<usize>(ExchangeSkeletonSettingRow::AssetUserData)].ueKey ==
              "AssetUserData" &&
          mesh[static_cast<usize>(ExchangeSkeletalMeshSettingRow::SkinWeightProfiles)].ueKey ==
              "SkinWeightProfiles" &&
          mesh[static_cast<usize>(ExchangeSkeletalMeshSettingRow::Skeleton)].ueKey == "Skeleton");
}

} // namespace exchange_skeletal_tests_file
