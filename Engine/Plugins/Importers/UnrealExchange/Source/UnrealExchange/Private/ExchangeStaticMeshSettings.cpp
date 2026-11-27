#include <UnrealExchange/ExchangeStaticMeshSettings.h>

// 정적 메시 설정 번역 표다(Docs/Design/UnrealExchange 5.5절 3–5번).

namespace rocks {

namespace exchange_static_mesh_settings {

using Kind = ExchangeSettingKind;

// 행을 만든다(모든 칸을 채운다).
constexpr ExchangeSettingRow Row(StringView key, Kind kind = Kind::Same) noexcept {
    return ExchangeSettingRow{key, kind, 0, nullptr, 0, StringView(), StringView()};
}

// 출력 키가 UE 키와 다른 행이다(5.5절 4번).
constexpr ExchangeSettingRow Renamed(StringView key, StringView rocksKey) noexcept {
    return ExchangeSettingRow{key, Kind::Same, 0, nullptr, 0, StringView(), rocksKey};
}

template <usize Count>
constexpr ExchangeSettingRow StructRow(StringView key, Kind kind,
                                       const ExchangeSettingRow (&members)[Count]) noexcept {
    return ExchangeSettingRow{key, kind, 0, members, Count, StringView(), StringView()};
}

constexpr ExchangeSettingRow UnsupportedRow(StringView key, StringView defaultValue) noexcept {
    return ExchangeSettingRow{key, Kind::Unsupported, 0, nullptr, 0, defaultValue, StringView()};
}

// UE FMeshBuildSettings → MeshBuildSettings. DistanceFieldReplacementMesh는 LOD 0 값만 에셋 칸으로
// 코드가 옮긴다(5.5절 3번 — 표에서는 버린다).
constexpr ExchangeSettingRow BuildMembers[] = {
    Row("bUseMikkTSpace"),
    Row("bRecomputeNormals"),
    Row("bRecomputeTangents"),
    Row("bComputeWeightedNormals"),
    Row("bRemoveDegenerates"),
    Row("bBuildReversedIndexBuffer"),
    Row("bUseHighPrecisionTangentBasis"),
    Row("bUseFullPrecisionUVs"),
    UnsupportedRow("bUseBackwardsCompatibleF16TruncUVs", "false"),
    Row("bGenerateLightmapUVs"),
    Row("bGenerateDistanceFieldAsIfTwoSided"),
    Row("bSupportFaceRemap"),
    Row("MinLightmapResolution"),
    Row("SrcLightmapIndex"),
    Row("DstLightmapIndex"),
    Renamed("BuildScale3D", "buildScale"),
    Row("DistanceFieldResolutionScale"),
    Row("DistanceFieldReplacementMesh", Kind::Ignore),
    Row("MaxLumenMeshCards"),
};

// UE FMeshReductionSettings → MeshReductionSettings(enum 값 이름은 접두가 없다).
constexpr ExchangeSettingRow ReductionMembers[] = {
    Row("PercentTriangles"),
    Renamed("MaxNumOfTriangles", "maxTriangles"),
    Row("PercentVertices"),
    Renamed("MaxNumOfVerts", "maxVertices"),
    Row("MaxDeviation"),
    Row("PixelError"),
    Row("WeldingThreshold"),
    Row("HardAngleThreshold"),
    Renamed("BaseLODModel", "baseLod"),
    Row("SilhouetteImportance", Kind::Enum),
    Row("TextureImportance", Kind::Enum),
    Row("ShadingImportance", Kind::Enum),
    Row("bRecalculateNormals"),
    Row("bGenerateUniqueLightmapUVs"),
    Row("bKeepSymmetry"),
    Row("bVisibilityAided"),
    Row("bCullOccluded"),
    Row("TerminationCriterion", Kind::Enum),
    Row("VisibilityAggressiveness", Kind::Enum),
    Row("VertexColorImportance", Kind::Enum),
};

// UE FStaticMeshSourceModel(설정 아닌 멤버는 버린다).
constexpr ExchangeSettingRow SourceModelMembers[] = {
    StructRow("BuildSettings", Kind::Struct, BuildMembers),
    StructRow("ReductionSettings", Kind::Struct, ReductionMembers),
    Row("ScreenSize", Kind::PerPlatformFloat),
    Row("StaticMeshDescriptionBulkData", Kind::Ignore),
    Row("CacheMeshDescriptionTrianglesCount", Kind::Ignore),
    Row("CacheMeshDescriptionVerticesCount", Kind::Ignore),
    Row("SourceImportFilename", Kind::Ignore),
    Row("bImportWithBaseMesh", Kind::Ignore),
};

// UE FMeshUVChannelInfo → MeshUvDensity.
constexpr ExchangeSettingRow UvDensityMembers[] = {
    Row("bInitialized"),
    Row("bOverrideDensities"),
    Row("LocalUVDensities"),
};

// UE FStaticMaterial(5.5절 6번).
constexpr ExchangeSettingRow SlotMembers[] = {
    Row("MaterialInterface", Kind::Reference),
    Row("MaterialSlotName"),
    Row("ImportedMaterialSlotName"),
    StructRow("UVChannelData", Kind::Struct, UvDensityMembers),
    Row("OverlayMaterialInterface", Kind::Reference),
};

// UE FMeshSectionInfo → MeshSectionInfo.
constexpr ExchangeSettingRow SectionInfoMembers[] = {
    Row("MaterialIndex"),
    Row("bEnableCollision"),
    Row("bCastShadow"),
    Row("bVisibleInRayTracing"),
    Row("bAffectDistanceFieldLighting"),
    Row("bForceOpaque"),
};

// UE FMeshSectionInfoMap 맵 항목 { key, value }(5.5절 7번).
constexpr ExchangeSettingRow SectionEntryMembers[] = {
    Row("key"),
    StructRow("value", Kind::Struct, SectionInfoMembers),
};

constexpr ExchangeSettingRow SectionMapMembers[] = {
    StructRow("Map", Kind::StructArray, SectionEntryMembers),
};

// UStaticMeshSocket 프로퍼티(5.5절 8번 — PreviewStaticMesh는 편집기 미리보기라 버린다).
constexpr ExchangeSettingRow SocketPropertyMembers[] = {
    Row("SocketName"),
    Row("RelativeLocation"),
    Row("RelativeRotation"),
    Row("RelativeScale"),
    Row("Tag"),
    Row("bSocketCreatedAtImport"),
    Row("PreviewStaticMesh", Kind::Ignore),
};

// 하위 객체 덤프 { subobject, class, properties }(11.7절 13번).
constexpr ExchangeSettingRow SocketMembers[] = {
    Row("subobject"),
    Row("class", Kind::Ignore),
    StructRow("properties", Kind::Struct, SocketPropertyMembers),
};

// UE FMeshNaniteSettings → VirtualGeometrySettings(이름은 대소문자만 다르다).
constexpr ExchangeSettingRow NaniteMembers[] = {
    Row("bEnabled"),
    Row("bExplicitTangents"),
    Row("bLerpUVs"),
    Row("bSeparable"),
    Row("bVoxelNDF"),
    Row("bVoxelOpacity"),
    Row("ShapePreservation", Kind::Enum),
    Row("PositionPrecision"),
    Row("NormalPrecision"),
    Row("TangentPrecision"),
    Row("BoneWeightPrecision"),
    Row("TargetMinimumResidencyInKB"),
    Row("KeepPercentTriangles"),
    Row("TrimRelativeError"),
    Row("GenerateFallback", Kind::Enum),
    Row("FallbackTarget", Kind::Enum),
    Row("FallbackPercentTriangles"),
    Row("FallbackRelativeError"),
    Row("MaxEdgeLengthFactor"),
    Row("NumRays"),
    Row("VoxelLevel"),
    Row("RayBackUp"),
    Row("DisplacementUVChannel"),
    UnsupportedRow("DisplacementMaps", "[]"),
    UnsupportedRow("NaniteAssemblyData", R"({"Parts": [], "Nodes": []})"),
};

// 5.5절 5번 표 순서다(행 번호 = 배열 번호 — ExchangeStaticMeshSettingRow).
constexpr ExchangeSettingRow StaticMeshRows[] = {
    // 0–14: 명세로 옮김.
    StructRow("SourceModels", Kind::StructArray, SourceModelMembers),
    StructRow("StaticMaterials", Kind::StructArray, SlotMembers),
    StructRow("SectionInfoMap", Kind::Struct, SectionMapMembers),
    StructRow("Sockets", Kind::StructArray, SocketMembers),
    Row("LightMapResolution"),
    Row("LightMapCoordinateIndex"),
    Row("PositiveBoundsExtension"),
    Row("NegativeBoundsExtension"),
    Row("bAutoComputeLODScreenSize"),
    Row("LODGroup"),
    Row("MinLOD", Kind::PerPlatformFloat),
    Row("NumStreamedLODs", Kind::PerPlatformFloat),
    StructRow("NaniteSettings", Kind::Struct, NaniteMembers),
    Row("LODForCollision"),
    Row("ComplexCollisionMesh", Kind::Reference),
    // 15–26: 설정이 아님(편집기·임포트 기록·캐시).
    Row("HiResSourceModel", Kind::Ignore),
    Row("OriginalSectionInfoMap", Kind::Ignore),
    Row("ImportVersion", Kind::Ignore),
    Row("MaterialRemapIndexPerImportVersion", Kind::Ignore),
    Row("LightmapUVVersion", Kind::Ignore),
    Row("ElementToIgnoreForTexFactor", Kind::Ignore),
    Row("LightmapUVDensity", Kind::Ignore),
    Row("AssetImportData", Kind::Ignore),
    Row("ThumbnailInfo", Kind::Ignore),
    Row("EditorCameraPosition", Kind::Ignore),
    Row("bCustomizedCollision", Kind::Ignore),
    Row("ExtendedBounds", Kind::Ignore),
    // 27–47: Rocks 필드가 없다(UE 기본값 — UStaticMesh·UStreamableRenderAsset 생성자).
    UnsupportedRow("LODStreaming", R"("Default")"),
    UnsupportedRow("MinQualityLevelLOD", R"({"Default": 0, "PerQuality": []})"),
    UnsupportedRow("StaticMeshPaintSupport", R"("Default")"),
    UnsupportedRow("MeshPaintTextureCoordinateIndex", "0"),
    UnsupportedRow("MeshPaintTextureResolution", "0"),
    UnsupportedRow("DistanceFieldSelfShadowBias", "0"),
    UnsupportedRow("bGenerateMeshDistanceField", "false"),
    UnsupportedRow("bHasNavigationData", "true"),
    UnsupportedRow("bSupportUniformlyDistributedSampling", "false"),
    UnsupportedRow("bSupportPhysicalMaterialMasks", "false"),
    UnsupportedRow("bUseLegacyTangentScaling", "false"),
    UnsupportedRow(
        "RayTracingProxySettings",
        R"({"bEnabled": false, "FallbackTarget": "Auto", "FallbackPercentTriangles": 1, )"
        R"("FallbackRelativeError": 2, "FoliageOverOcclusionBias": 0})"),
    UnsupportedRow("bSupportRayTracing", "true"),
    UnsupportedRow("bDoFastBuild", "false"),
    UnsupportedRow("bAllowCPUAccess", "false"),
    UnsupportedRow("bSupportGpuUniformlyDistributedSampling", "false"),
    UnsupportedRow("AssetUserData", "[]"),
    UnsupportedRow("NumCinematicMipLevels", "0"),
    UnsupportedRow("NoRefStreamingLODBias", R"({"Default": -1, "PerQuality": []})"),
    UnsupportedRow("NeverStream", "false"),
    UnsupportedRow("bGlobalForceMipLevelsToBeResident", "false"),
};

static_assert(sizeof(StaticMeshRows) / sizeof(StaticMeshRows[0]) ==
                  ExchangeStaticMeshSettingRowCount,
              "static mesh setting rows must match ExchangeStaticMeshSettingRowCount");

} // namespace exchange_static_mesh_settings

Span<const ExchangeSettingRow> ExchangeStaticMeshSettingRows() noexcept {
    using namespace exchange_static_mesh_settings;
    return Span<const ExchangeSettingRow>(StaticMeshRows);
}

} // namespace rocks
