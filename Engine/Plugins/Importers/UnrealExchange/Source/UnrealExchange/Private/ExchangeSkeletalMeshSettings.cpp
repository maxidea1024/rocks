#include <UnrealExchange/ExchangeSkeletalMeshSettings.h>

#include "ExchangeMeshSettingTables.h"

// 스킨 메시 설정 번역 표다(Docs/Design/UnrealExchange 6.4절 2–4번).

namespace rocks {

namespace exchange_skeletal_mesh_settings {

using namespace exchange_detail;
using Kind = ExchangeSettingKind;

// UE FSkeletalMeshBuildSettings → SkinnedMeshBuildSettings(이름은 b 접두만 다르다).
constexpr ExchangeSettingRow BuildMembers[] = {
    Row("bRecomputeNormals"),
    Row("bRecomputeTangents"),
    Row("bUseMikkTSpace"),
    Row("bComputeWeightedNormals"),
    Row("bRemoveDegenerates"),
    Row("bUseHighPrecisionTangentBasis"),
    Row("bUseHighPrecisionSkinWeights"),
    Row("bUseFullPrecisionUVs"),
    UnsupportedRow("bUseBackwardsCompatibleF16TruncUVs", "false"),
    UnsupportedRow("bOptimizeForInstancing", "false"),
    Row("ThresholdPosition"),
    Row("ThresholdTangentNormal"),
    Row("ThresholdUV"),
    Row("MorphThresholdPosition"),
    Row("BoneInfluenceLimit"),
};

// UE ESkeletalMeshTerminationCriterion →
// SkeletalMeshReductionTermination(SkeletalMeshReductionSettings.h).
constexpr ExchangeSettingRow TerminationValues[] = {
    EnumValue("SMTC_NumOfTriangles", "Triangles"),
    EnumValue("SMTC_NumOfVerts", "Vertices"),
    EnumValue("SMTC_TriangleOrVert", "TrianglesOrVertices"),
    EnumValue("SMTC_AbsNumOfTriangles", "AbsoluteTriangles"),
    EnumValue("SMTC_AbsNumOfVerts", "AbsoluteVertices"),
    EnumValue("SMTC_AbsTriangleOrVert", "AbsoluteTrianglesOrVertices"),
};

// UE ESkeletalMeshOptimizationType → SkeletalMeshReductionMethod.
constexpr ExchangeSettingRow MethodValues[] = {
    EnumValue("SMOT_NumOfTriangles", "Triangles"),
    EnumValue("SMOT_MaxDeviation", "MaxDeviation"),
    EnumValue("SMOT_TriangleOrDeviation", "TrianglesOrDeviation"),
};

// UE FSkeletalMeshOptimizationSettings → SkeletalMeshReductionSettings(6.4절 3번).
constexpr ExchangeSettingRow ReductionMembers[] = {
    StructRow("TerminationCriterion", Kind::EnumMap, TerminationValues),
    Renamed("NumOfTrianglesPercentage", "percentTriangles"),
    Renamed("NumOfVertPercentage", "percentVertices"),
    Renamed("MaxNumOfTriangles", "maxTriangles"),
    Renamed("MaxNumOfVerts", "maxVertices"),
    Renamed("MaxNumOfTrianglesPercentage", "maxTrianglesFromPercent"),
    Renamed("MaxNumOfVertsPercentage", "maxVerticesFromPercent"),
    Renamed("MaxDeviationPercentage", "maxDeviationPercent"),
    StructRow("ReductionMethod", Kind::EnumMap, MethodValues),
    Row("SilhouetteImportance", Kind::Enum),
    Row("TextureImportance", Kind::Enum),
    Row("ShadingImportance", Kind::Enum),
    Row("SkinningImportance", Kind::Enum),
    Row("bRemapMorphTargets"),
    Renamed("bRecalcNormals", "recalculateNormals"),
    Row("WeldingThreshold"),
    Row("NormalsThreshold"),
    Row("MaxBonesPerVertex"),
    Row("bEnforceBoneBoundaries"),
    Renamed("bMergeCoincidentVertBones", "mergeCoincidentVertexBones"),
    Row("VolumeImportance"),
    Row("bLockEdges"),
    // UE 프로퍼티 이름의 철자 그대로다(Bounaries).
    Renamed("bLockColorBounaries", "lockColorBoundaries"),
    Row("bImproveTrianglesForCloth"),
    Renamed("BaseLOD", "baseLod"),
};

// UE FSkeletalMeshSourceModel(5.8 — LOD 정보를 합쳤다, 6.3절 13번).
constexpr ExchangeSettingRow SourceModelMembers[] = {
    StructRow("BuildSettings", Kind::Struct, BuildMembers),
    StructRow("ReductionSettings", Kind::Struct, ReductionMembers),
    Row("ScreenSize", Kind::PerPlatformFloat),
    Row("LODHysteresis"),
    Row("LODMaterialMap"),
    Row("BonesToRemove"),
    Row("BonesToPrioritize"),
    Row("WeightOfPrioritization"),
    Row("TriangleCount", Kind::Ignore),
    Row("VertexCount", Kind::Ignore),
    Row("Bounds", Kind::Ignore),
    Row("CachedSkinWeightProfileNames", Kind::Ignore),
    Row("CachedMorphTargetNames", Kind::Ignore),
    Row("CachedHasVertexColors", Kind::Ignore),
    Row("CachedVertexColorGuid", Kind::Ignore),
    Row("MeshDescriptionBulkData", Kind::Ignore),
    Row("SourceImportFilename", Kind::Ignore),
    Row("bImportWithBaseMesh", Kind::Ignore),
    Row("bHasBeenSimplified", Kind::Ignore),
    Row("ImportedMorphTargetSourceFilename", Kind::Ignore),
    UnsupportedRow("SectionsToPrioritize", "[]"),
    UnsupportedRow("BakePose", "null"),
    UnsupportedRow("BakePoseOverride", "null"),
    UnsupportedRow("SkinCacheUsage", R"("Auto")"),
    UnsupportedRow("MorphTargetPositionErrorTolerance", "20"),
    UnsupportedRow("VertexAttributes", "[]"),
    UnsupportedRow("bHasPerLODVertexColors", "false"),
    UnsupportedRow("bAllowCPUAccess", "false"),
    UnsupportedRow("bBuildHalfEdgeBuffers", "false"),
    UnsupportedRow("bAllowMeshDeformer", "true"),
    UnsupportedRow("bSupportUniformlyDistributedSampling", "false"),
};

// UE FSkinWeightProfileInfo(PerPlatform 값은 코드가 읽는다 — bool 기본값이라 PerPlatformFloat가
// 아니다).
constexpr ExchangeSettingRow ProfileMembers[] = {
    Row("Name"),
    Row("DefaultProfile"),
    Row("DefaultProfileFromLODIndex"),
    Row("PerLODSourceFiles", Kind::Ignore),
};

// UE FSkeletalMeshSamplingInfo: 사용자 영역만 본다(BuiltData는 파생).
constexpr ExchangeSettingRow SamplingMembers[] = {
    UnsupportedRow("Regions", "[]"),
    Row("BuiltData", Kind::Ignore),
};

// 6.4절 4번 표 순서다(행 번호 = 배열 번호 — ExchangeSkeletalMeshSettingRow).
constexpr ExchangeSettingRow SkeletalMeshRows[] = {
    // 0–7: 명세로 옮김.
    StructRow("SourceModels", Kind::StructArray, SourceModelMembers),
    Row("Skeleton", Kind::Reference),
    Row("PositiveBoundsExtension"),
    Row("NegativeBoundsExtension"),
    Row("MinLod", Kind::PerPlatformFloat),
    StructRow("NaniteSettings", Kind::Struct, NaniteMembers),
    StructRow("Sockets", Kind::StructArray, BoneSocketMembers),
    StructRow("SkinWeightProfiles", Kind::StructArray, ProfileMembers),
    // 8–23: 설정이 아님(원본에서 다시 계산·편집기 표시·임포트 기록).
    Row("MorphTargets", Kind::Ignore),
    Row("bHasVertexColors", Kind::Ignore),
    Row("VertexColorGuid", Kind::Ignore),
    Row("AssetImportData", Kind::Ignore),
    Row("ThumbnailInfo", Kind::Ignore),
    Row("bHasCustomDefaultEditorCamera", Kind::Ignore),
    Row("DefaultEditorCameraLocation", Kind::Ignore),
    Row("DefaultEditorCameraRotation", Kind::Ignore),
    Row("DefaultEditorCameraLookAt", Kind::Ignore),
    Row("DefaultEditorCameraOrthoZoom", Kind::Ignore),
    Row("PreviewAttachedAssetContainer", Kind::Ignore),
    Row("FloorOffset", Kind::Ignore),
    Row("BoneDrawSize", Kind::Ignore),
    Row("ForwardAxis", Kind::Ignore),
    Row("SkelMirrorAxis", Kind::Ignore),
    Row("SkelMirrorFlipAxis", Kind::Ignore),
    // 24–53: Rocks 필드가 없다(UE 기본값 — USkeletalMesh·UStreamableRenderAsset 생성자).
    UnsupportedRow("SkelMirrorTable", "[]"),
    UnsupportedRow("MinQualityLevelLOD", R"({"Default": 0, "PerQuality": []})"),
    UnsupportedRow("DisableBelowMinLodStripping", R"({"Default": false, "PerPlatform": []})"),
    UnsupportedRow("bOverrideLODStreamingSettings", "false"),
    UnsupportedRow("bSupportLODStreaming", R"({"Default": false, "PerPlatform": []})"),
    UnsupportedRow("MaxNumStreamedLODs", R"({"Default": 0, "PerPlatform": []})"),
    UnsupportedRow("MaxNumOptionalLODs", R"({"Default": 0, "PerPlatform": []})"),
    UnsupportedRow("LODSettings", "null"),
    UnsupportedRow("DefaultAnimatingRig", "null"),
    UnsupportedRow("bEnablePerPolyCollision", "false"),
    UnsupportedRow("PhysicsAsset", "null"),
    UnsupportedRow("ShadowPhysicsAsset", "null"),
    UnsupportedRow("NodeMappingData", "[]"),
    UnsupportedRow("bSupportRayTracing", "true"),
    UnsupportedRow("RayTracingMinLOD", "0"),
    UnsupportedRow("ClothLODBiasMode", R"("MappingsToSameLOD")"),
    UnsupportedRow("PostProcessAnimBlueprint", "null"),
    UnsupportedRow("PostProcessAnimBPLODThreshold", "-1"),
    UnsupportedRow("MeshClothingAssets", "[]"),
    StructRow("SamplingInfo", Kind::Struct, SamplingMembers),
    UnsupportedRow("AssetUserData", "[]"),
    UnsupportedRow("AssetUserDataEditorOnly", "[]"),
    UnsupportedRow("DefaultMeshDeformer", "null"),
    UnsupportedRow("TargetMeshDeformers", "null"),
    UnsupportedRow("OverlayMaterial", "null"),
    UnsupportedRow("OverlayMaterialMaxDrawDistance", "0"),
    UnsupportedRow("NumCinematicMipLevels", "0"),
    UnsupportedRow("NoRefStreamingLODBias", R"({"Default": -1, "PerQuality": []})"),
    UnsupportedRow("NeverStream", "false"),
    UnsupportedRow("bGlobalForceMipLevelsToBeResident", "false"),
};

static_assert(sizeof(SkeletalMeshRows) / sizeof(SkeletalMeshRows[0]) ==
                  ExchangeSkeletalMeshSettingRowCount,
              "skeletal mesh setting rows must match ExchangeSkeletalMeshSettingRowCount");

// data 객체: materials만 슬롯으로 번역한다(6.3절 13번).
constexpr ExchangeSettingRow DataRows[] = {
    Row("bones", Kind::Ignore),
    Row("lods", Kind::Ignore),
    StructRow("materials", Kind::StructArray, SlotMembers),
};

} // namespace exchange_skeletal_mesh_settings

Span<const ExchangeSettingRow> ExchangeSkeletalMeshSettingRows() noexcept {
    using namespace exchange_skeletal_mesh_settings;
    return Span<const ExchangeSettingRow>(SkeletalMeshRows);
}

Span<const ExchangeSettingRow> ExchangeSkeletalMeshDataRows() noexcept {
    using namespace exchange_skeletal_mesh_settings;
    return Span<const ExchangeSettingRow>(DataRows);
}

} // namespace rocks
