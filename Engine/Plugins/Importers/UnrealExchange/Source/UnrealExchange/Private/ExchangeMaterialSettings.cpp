#include <Core/Text/NamePool.h>
#include <MaterialSchema/MaterialTraitsDesc.generated.h>
#include <UnrealExchange/ExchangeMaterialSettings.h>

#include "ExchangeMeshCommon.h"
#include "ExchangeMeshSettingTables.h"

// 머티리얼 설정 번역이다(Docs/Design/UnrealExchange 8.7절 7번 "구현에서 정함" — UX4b1a).

namespace rocks {

namespace exchange_material_settings {

using namespace exchange_detail;
using Kind = ExchangeSettingKind;

// 행 묶음 번호(ExchangeMaterialSettingGroup)다.
constexpr uint8 Translucency = static_cast<uint8>(ExchangeMaterialSettingGroup::Translucency);
constexpr uint8 Forward = static_cast<uint8>(ExchangeMaterialSettingGroup::Forward);
constexpr uint8 PostProcess = static_cast<uint8>(ExchangeMaterialSettingGroup::PostProcess);

// 묶음 출력 키(MaterialTraitsDesc의 하위 struct 필드 이름 — 묶음 번호 순)다.
constexpr StringView GroupKeys[] = {"translucency", "forward", "postProcess"};

// BLEND_TranslucentColoredTransmittance의 덤프 이름이다(8.8절 3번 경고).
constexpr StringView ColoredTransmittanceName = "BLEND_TranslucentColoredTransmittance";

// row의 처리 종류·묶음(·출력 키)을 바꾼 행이다.
constexpr ExchangeSettingRow Grouped(ExchangeSettingRow row, Kind kind, uint8 group,
                                     StringView rocksKey = StringView()) noexcept {
    row.kind = kind;
    row.group = group;
    if (!rocksKey.IsEmpty()) {
        row.rocksKey = rocksKey;
    }

    return row;
}

// 값 이름 대응 표를 쓰는 traits 최상위 enum 행이다.
template <usize Count>
constexpr ExchangeSettingRow Mapped(StringView key, const ExchangeSettingRow (&values)[Count],
                                    StringView rocksKey) noexcept {
    return Grouped(StructRow(key, Kind::EnumMap, values), Kind::EnumMap, 0, rocksKey);
}

// Rocks에 없는 값이다(EnumMap 멤버 — 경고 뒤 키를 버린다).
constexpr ExchangeSettingRow Unmapped(StringView ueValue) noexcept {
    return EnumValue(ueValue, StringView());
}

// UE EMaterialDomain(MaterialDomain.h) → MaterialDomain.
constexpr ExchangeSettingRow DomainValues[] = {
    EnumValue("MD_Surface", "Surface"),
    EnumValue("MD_DeferredDecal", "DeferredDecal"),
    EnumValue("MD_LightFunction", "LightFunction"),
    EnumValue("MD_Volume", "Volume"),
    EnumValue("MD_PostProcess", "PostProcess"),
    EnumValue("MD_UI", "UserInterface"),
    Unmapped("MD_RuntimeVirtualTexture"),
};

// UE EBlendMode(EngineTypes.h — 별칭 값 이름 둘 포함) → BlendMode. AlphaHoldout은 Rocks 값이
// TODO(MPQ-AlphaHoldout)라 없는 값, ColoredTransmittance는 Translucent다(8.8절 3번).
constexpr ExchangeSettingRow BlendValues[] = {
    EnumValue("BLEND_Opaque", "Opaque"),
    EnumValue("BLEND_Masked", "Masked"),
    EnumValue("BLEND_Translucent", "Translucent"),
    EnumValue("BLEND_Additive", "Additive"),
    EnumValue("BLEND_Modulate", "Modulate"),
    EnumValue("BLEND_AlphaComposite", "AlphaComposite"),
    Unmapped("BLEND_AlphaHoldout"),
    EnumValue(ColoredTransmittanceName, "Translucent"),
    EnumValue("BLEND_TranslucentGreyTransmittance", "Translucent"),
    EnumValue("BLEND_ColoredTransmittanceOnly", "Modulate"),
};

// UE EMaterialTranslucencyPass(Material.h) → TranslucencyPass.
constexpr ExchangeSettingRow TranslucencyPassValues[] = {
    EnumValue("MTP_BeforeDOF", "BeforeDepthOfField"),
    EnumValue("MTP_AfterDOF", "AfterDepthOfField"),
    EnumValue("MTP_AfterMotionBlur", "AfterMotionBlur"),
};

// UE EMaterialShadingRate(EngineTypes.h) → MaterialShadingRate.
constexpr ExchangeSettingRow ShadingRateValues[] = {
    EnumValue("MSR_1x1", "Rate1x1"), EnumValue("MSR_2x1", "Rate2x1"),
    EnumValue("MSR_1x2", "Rate1x2"), EnumValue("MSR_2x2", "Rate2x2"),
    EnumValue("MSR_4x2", "Rate4x2"), EnumValue("MSR_2x4", "Rate2x4"),
    EnumValue("MSR_4x4", "Rate4x4"),
};

// UE ERefractionMode(EngineTypes.h) → RefractionMethod.
constexpr ExchangeSettingRow RefractionMethodValues[] = {
    EnumValue("RM_IndexOfRefraction", "IndexOfRefraction"),
    EnumValue("RM_PixelNormalOffset", "PixelNormalOffset"),
    EnumValue("RM_2DOffset", "Offset2D"),
    EnumValue("RM_None", "None"),
    EnumValue("RM_IndexOfRefractionFromF0", "IndexOfRefractionFromF0"),
};

// UE EPixelDepthOffsetMode(EngineTypes.h) → PixelDepthOffsetMode.
constexpr ExchangeSettingRow PixelDepthOffsetModeValues[] = {
    EnumValue("PDOM_Legacy", "ViewDepth"),
    EnumValue("PDOM_AlongCameraVector", "AlongCameraVector"),
};

// UE EBlendableLocation(BlendableInterface.h) → BlendableLocation.
constexpr ExchangeSettingRow BlendableLocationValues[] = {
    EnumValue("BL_SceneColorBeforeDOF", "SceneColorBeforeDepthOfField"),
    EnumValue("BL_SceneColorAfterDOF", "SceneColorAfterDepthOfField"),
    EnumValue("BL_TranslucencyAfterDOF", "TranslucencyAfterDepthOfField"),
    EnumValue("BL_SSRInput", "ScreenSpaceReflectionInput"),
    EnumValue("BL_SceneColorBeforeBloom", "SceneColorBeforeBloom"),
    EnumValue("BL_ReplacingTonemapper", "ReplacingTonemapper"),
    EnumValue("BL_SceneColorAfterTonemapping", "SceneColorAfterTonemapping"),
};

// UE FIntPoint UserTextureDivisor → postProcess.userTextureDivisorX·Y(Flatten).
constexpr ExchangeSettingRow UserTextureDivisorMembers[] = {
    Renamed("X", "userTextureDivisorX"),
    Renamed("Y", "userTextureDivisorY"),
};

// UMaterial 덤프 최상위 키(덤프 순서 — MaterialSystem 19.9–19.11절 대응).
constexpr ExchangeSettingRow MaterialRows[] = {
    UnsupportedRow("PhysMaterial", "null"),
    UnsupportedRow("PhysMaterialMask", "null"),
    UnsupportedRow("PhysicalMaterialMap", "[null, null, null, null, null, null, null, null]"),
    UnsupportedRow("RenderTracePhysicalMaterialOutputs", "[]"),
    Mapped("MaterialDomain", DomainValues, "domain"),
    Mapped("BlendMode", BlendValues, "blendMode"),
    Row("DecalBlendMode", Kind::Ignore),
    Grouped(Renamed("MaterialDecalResponse", "decalResponse"), Kind::Enum, 0),
    UnsupportedRow(
        "NaniteOverrideMaterial",
        R"({"bEnableOverride": true, "OverrideMaterialEditor": null, "OverrideMaterial": null, "OverrideMaterialRef": null})"),
    UnsupportedRow("DisplacementScaling", R"({"Magnitude": 4, "Center": 0.5})"),
    UnsupportedRow("DisplacementFadeRange", R"({"StartSizePixels": 4, "EndSizePixels": 1})"),
    Row("ShadingModel", Kind::Ignore),
    Row("bCastDynamicShadowAsMasked"),
    Row("ShadingModels", Kind::Ignore),
    Row("OpacityMaskClipValue"),
    Renamed("bEnableResponsiveAA", "responsiveAntialiasing"),
    Row("bScreenSpaceReflections"),
    Row("bContactShadows"),
    Row("TwoSided"),
    Renamed("bIsThinSurface", "thinSurface"),
    Row("DitheredLODTransition"),
    Row("DitherOpacityMask"),
    Renamed("bAllowNegativeEmissiveColor", "allowNegativeEmissive"),
    Row("bHasPixelAnimation"),
    UnsupportedRow("bEnableTessellation", "false"),
    UnsupportedRow("bEnableDisplacementFade", "false"),
    Grouped(StructRow("TranslucencyPass", Kind::EnumMap, TranslucencyPassValues), Kind::EnumMap,
            Translucency, "pass"),
    Grouped(Renamed("TranslucencyLightingMode", "lightingMode"), Kind::Enum, Translucency),
    Grouped(Renamed("bEnableMobileSeparateTranslucency", "mobileSeparateTranslucency"), Kind::Same,
            Translucency),
    Renamed("NumCustomizedUVs", "customizedUvCount"),
    Grouped(Renamed("TranslucencyDirectionalLightingIntensity", "directionalLightingIntensity"),
            Kind::Same, Translucency),
    Grouped(Renamed("TranslucentShadowDensityScale", "shadowDensityScale"), Kind::Same,
            Translucency),
    Grouped(Renamed("TranslucentSelfShadowDensityScale", "selfShadowDensityScale"), Kind::Same,
            Translucency),
    Grouped(Renamed("TranslucentSelfShadowSecondDensityScale", "selfShadowSecondDensityScale"),
            Kind::Same, Translucency),
    Grouped(Renamed("TranslucentSelfShadowSecondOpacity", "selfShadowSecondOpacity"), Kind::Same,
            Translucency),
    Grouped(Renamed("TranslucentBackscatteringExponent", "backscatteringExponent"), Kind::Same,
            Translucency),
    Grouped(Renamed("TranslucentMultipleScatteringExtinction", "multipleScatteringExtinction"),
            Kind::Same, Translucency),
    Grouped(Renamed("TranslucentShadowStartOffset", "shadowStartOffset"), Kind::Same, Translucency),
    Row("bDisableDepthTest"),
    Row("bWriteOnlyAlpha"),
    Renamed("bGenerateSphericalParticleNormals", "sphericalParticleNormals"),
    Row("bTangentSpaceNormal"),
    Renamed("bUseEmissiveForDynamicAreaLighting", "emissiveForAreaLighting"),
    Renamed("bUsedAsSpecialEngineMaterial", "engineDefaultMaterial"),
    Renamed("bSubstrateRoughnessTracking", "roughnessTracking"),
    Row("bUsedWithSkeletalMesh", Kind::Ignore),
    UnsupportedRow("bUsedWithMeshDeformer", "false"),
    Row("bUsedWithEditorCompositing", Kind::Ignore),
    UnsupportedRow("bUsedWithParticleSprites", "false"),
    UnsupportedRow("bUsedWithBeamTrails", "false"),
    UnsupportedRow("bUsedWithMeshParticles", "false"),
    UnsupportedRow("bUsedWithNiagaraSprites", "false"),
    UnsupportedRow("bUsedWithNiagaraRibbons", "false"),
    UnsupportedRow("bUsedWithNiagaraMeshParticles", "false"),
    UnsupportedRow("bUsedWithGeometryCache", "false"),
    UnsupportedRow("bUsedWithStaticLighting", "false"),
    Row("bUsedWithMorphTargets", Kind::Ignore),
    UnsupportedRow("bUsedWithSplineMeshes", "false"),
    UnsupportedRow("bUsedWithInstancedStaticMeshes", "false"),
    UnsupportedRow("bUsedWithGeometryCollections", "false"),
    Row("bUsesDistortion"),
    UnsupportedRow("bUsedWithClothing", "false"),
    UnsupportedRow("bUsedWithWater", "false"),
    UnsupportedRow("bUsedWithHairStrands", "false"),
    UnsupportedRow("bUsedWithLidarPointCloud", "false"),
    UnsupportedRow("bUsedWithVirtualHeightfieldMesh", "false"),
    UnsupportedRow("bUsedWithNanite", "false"),
    UnsupportedRow("bUsedWithVoxels", "false"),
    UnsupportedRow("bUsedWithCurves", "false"),
    UnsupportedRow("bUsedWithVolumetricCloud", "false"),
    UnsupportedRow("bUsedWithHeterogeneousVolumes", "false"),
    UnsupportedRow("bUsedWithInstancedSkinnedMesh", "false"),
    Row("bUsedWithStaticMesh", Kind::Ignore),
    Renamed("bForceCompatibleWithLightFunctionAtlas", "lightFunctionAtlasCompatible"),
    Row("bAutomaticallySetUsageInEditor", Kind::Ignore),
    Row("bFullyRough"),
    Row("FloatPrecisionMode", Kind::Ignore),
    Renamed("bUseLightmapDirectionality", "lightmapDirectionality"),
    Renamed("bUseAlphaToCoverage", "alphaToCoverage"),
    Grouped(Renamed("bForwardRenderUsePreintegratedGFForSimpleIBL", "preintegratedSimpleIbl"),
            Kind::Same, Forward),
    Grouped(Renamed("bUseHQForwardReflections", "highQualityReflections"), Kind::Same, Forward),
    Grouped(Renamed("bForwardBlendsSkyLightCubemaps", "blendSkyLightCubemaps"), Kind::Same,
            Forward),
    Grouped(Renamed("bUsePlanarForwardReflections", "planarReflections"), Kind::Same, Forward),
    Row("bNormalCurvatureToRoughness"),
    Renamed("AllowTranslucentCustomDepthWrites", "translucentCustomDepthWrites"),
    Renamed("bAllowFrontLayerTranslucency", "frontLayerTranslucency"),
    Renamed("bCompatibleWithLumenCardSharing", "surfaceCacheSharing"),
    Row("Wireframe"),
    Grouped(Renamed("bAllowTranslucentLocalLightShadow", "localLightShadow"), Kind::Same,
            Translucency),
    Grouped(Renamed("TranslucentLocalLightShadowQuality", "localLightShadowQuality"), Kind::Same,
            Translucency),
    Grouped(Renamed("TranslucentDirectionalLightShadowQuality", "directionalLightShadowQuality"),
            Kind::Same, Translucency),
    Mapped("ShadingRate", ShadingRateValues, "shadingRate"),
    Row("bAllowVariableRateShading"),
    Row("EditorX", Kind::Ignore),
    Row("EditorY", Kind::Ignore),
    Row("EditorPitch", Kind::Ignore),
    Row("EditorYaw", Kind::Ignore),
    Renamed("bCanMaskedBeAssumedOpaque", "maskedAssumedOpaque"),
    Renamed("bUseMaterialAttributes", "useMaterialAttributes"),
    Row("bEnableExecWire", Kind::Ignore),
    Row("bEnableNewHLSLGenerator", Kind::Ignore),
    Row("bCastRayTracedShadows"),
    Renamed("bUseTranslucencyVertexFog", "translucencyVertexFog"),
    Row("bApplyCloudFogging"),
    Row("bIsSky"),
    Renamed("bComputeFogPerPixel", "fogPerPixel"),
    Row("bOutputTranslucentVelocity"),
    Renamed("bIsTranslucencyVelocityFromDepth", "translucencyVelocityFromDepth"),
    UnsupportedRow("bRelaxRuntimeVirtualTextureRestrictions", "false"),
    Grouped(StructRow("BlendableLocation", Kind::EnumMap, BlendableLocationValues), Kind::EnumMap,
            PostProcess, "location"),
    Grouped(Renamed("BlendableOutputAlpha", "outputAlpha"), Kind::Same, PostProcess),
    Grouped(Renamed("UserSceneTexture", "userSceneTexture"), Kind::Same, PostProcess),
    Grouped(StructRow("UserTextureDivisor", Kind::Flatten, UserTextureDivisorMembers),
            Kind::Flatten, PostProcess),
    Grouped(Renamed("ResolutionRelativeToInput", "resolutionRelativeToInput"), Kind::Same,
            PostProcess),
    Grouped(Renamed("bDisablePreExposureScale", "disablePreExposureScale"), Kind::Same,
            PostProcess),
    UnsupportedRow("bUsedWithNeuralNetworks", "false"),
    Grouped(Renamed("bEnableStencilTest", "stencilTest"), Kind::Same, PostProcess),
    Grouped(Renamed("StencilCompare", "stencilCompare"), Kind::Enum, PostProcess),
    Grouped(Renamed("StencilRefValue", "stencilRef"), Kind::Same, PostProcess),
    UnsupportedRow("NeuralProfileId", "-1"),
    Mapped("RefractionMethod", RefractionMethodValues, "refractionMethod"),
    Grouped(Renamed("RefractionCoverageMode", "refractionCoverageMode"), Kind::Enum, 0),
    Mapped("PixelDepthOffsetMode", PixelDepthOffsetModeValues, "pixelDepthOffsetMode"),
    Grouped(Renamed("BlendablePriority", "priority"), Kind::Same, PostProcess),
    Grouped(Renamed("bIsBlendable", "blendable"), Kind::Same, PostProcess),
    Row("PreshaderGap", Kind::Ignore),
    Row("RefractionDepthBias"),
    Renamed("MaxWorldPositionOffsetDisplacement", "maxWorldPositionOffset"),
    Row("bAlwaysEvaluateWorldPositionOffset"),
    Row("StateId", Kind::Ignore),
    Row("ReferencedTextureGuids", Kind::Ignore),
    Row("EditorOnlyData", Kind::Ignore),
    UnsupportedRow("SubsurfaceProfile", "null"),
    UnsupportedRow("SubsurfaceProfiles", "[]"),
    UnsupportedRow("SpecularProfiles", "[]"),
    UnsupportedRow("ToonProfiles", "[]"),
    UnsupportedRow("NeuralProfile", "null"),
    UnsupportedRow(
        "LightmassSettings",
        R"({"EmissiveBoost": 1, "DiffuseBoost": 1, "ExportResolutionScale": 1, "bCastShadowAsMasked": false, "bOverrideCastShadowAsMasked": false, "bOverrideEmissiveBoost": false, "bOverrideDiffuseBoost": false, "bOverrideExportResolutionScale": false})"),
    Row("TextureStreamingDataVersion", Kind::Ignore),
    Row("TextureStreamingData", Kind::Ignore),
    Row("ReferencedDefaultTextures", Kind::Ignore),
    UnsupportedRow("AssetUserData", "[]"),
    Row("bIncludedInBaseGame", Kind::Ignore),
    UnsupportedRow("PreviewMesh", "null"),
    Row("ThumbnailInfo", Kind::Ignore),
    Row("LayerParameterExpansion", Kind::Ignore),
    Row("ParameterOverviewExpansion", Kind::Ignore),
    Row("AssetImportData", Kind::Ignore),
    Row("LightingGuid", Kind::Ignore),
};

static_assert(sizeof(MaterialRows) / sizeof(MaterialRows[0]) == ExchangeMaterialSettingRowCount,
              "material setting row count must match ExchangeMaterialSettingRowCount");

template <ExchangeMaterialSettingRow Row>
constexpr bool RowIs(StringView key) noexcept {
    return MaterialRows[static_cast<uint32>(Row)].ueKey == key;
}

static_assert(RowIs<ExchangeMaterialSettingRow::MaterialDomain>("MaterialDomain") &&
                  RowIs<ExchangeMaterialSettingRow::BlendMode>("BlendMode") &&
                  RowIs<ExchangeMaterialSettingRow::UsedWithStaticMesh>("bUsedWithStaticMesh") &&
                  RowIs<ExchangeMaterialSettingRow::EditorPitch>("EditorPitch") &&
                  RowIs<ExchangeMaterialSettingRow::EditorYaw>("EditorYaw"),
              "ExchangeMaterialSettingRow values must index their table rows");

constexpr uint64 RowDetail(ExchangeMaterialSettingRow row) noexcept {
    return static_cast<uint64>(row);
}

// 번역 → traits ImportJson → ColoredTransmittance 경고 → 사용 대상 → 미리보기 카메라.
Result<void> Translate(ExchangeMeshContext& context, const ExchangeMaterialSettingsInput& input,
                       material::MaterialTraitsDesc& traits, material::VertexInputUsage& usage,
                       MaterialPreviewSettings& preview) noexcept {
    const JsonValue settings = input.asset->settings;
    JsonDocument translated(context.allocator);
    if (auto done = context.Translate(settings, Span<const ExchangeSettingRow>(MaterialRows),
                                      ExchangeClass::Material, translated,
                                      Span<const StringView>(GroupKeys));
        !done) {
        return done;
    }

    traits = material::MaterialTraitsDesc{};
    if (auto imported = context.ImportValue(translated.Root(), traits, 0); !imported) {
        return imported;
    }

    const JsonValue blend = settings.Find("BlendMode");
    if (input.substrate && blend.IsString() && blend.GetString() == ColoredTransmittanceName) {
        if (auto warned =
                context.Warn(AssetDiagnosticCode::UnsupportedSetting,
                             RowDetail(ExchangeMaterialSettingRow::BlendMode), "BlendMode");
            !warned) {
            return warned;
        }
    }

    // 키가 없으면 UE 기본값(true — Material.cpp UMaterial 생성자)이다.
    bool staticMesh = true;
    if (!OptBool(settings.Find("bUsedWithStaticMesh"), staticMesh)) {
        return context.Corrupt(RowDetail(ExchangeMaterialSettingRow::UsedWithStaticMesh));
    }

    usage.vertexInputs.Clear();
    if (staticMesh) {
        auto name = context.names.TryIntern(material::StaticMeshVertexInputName);
        if (!name) {
            return Result<void>::Failure(name.Error());
        }

        if (auto added = material::AddVertexInput(usage, name.Value()); !added) {
            return added;
        }
    }

    preview = MaterialPreviewSettings{};
    if (!OptInt32(settings.Find("EditorPitch"), preview.cameraPitch)) {
        return context.Corrupt(RowDetail(ExchangeMaterialSettingRow::EditorPitch));
    }

    if (!OptInt32(settings.Find("EditorYaw"), preview.cameraYaw)) {
        return context.Corrupt(RowDetail(ExchangeMaterialSettingRow::EditorYaw));
    }

    return Result<void>::Success();
}

} // namespace exchange_material_settings

Span<const ExchangeSettingRow> ExchangeMaterialSettingRows() noexcept {
    using namespace exchange_material_settings;
    return Span<const ExchangeSettingRow>(MaterialRows);
}

Span<const StringView> ExchangeMaterialSettingGroups() noexcept {
    using namespace exchange_material_settings;
    return Span<const StringView>(GroupKeys);
}

Result<void> TranslateExchangeMaterialSettings(const ExchangeMaterialSettingsInput& input,
                                               material::MaterialTraitsDesc& traits,
                                               material::VertexInputUsage& usage,
                                               MaterialPreviewSettings& preview,
                                               Array<ExchangeSettingWarning>& warnings,
                                               ExchangeMaterialFailure& failure) noexcept {
    using namespace exchange_material_settings;
    failure = ExchangeMaterialFailure{};
    if (input.asset == nullptr || input.registry == nullptr || input.registry->Names() == nullptr ||
        input.asset->exchangeClass != ExchangeClass::Material) {
        failure.code = AssetDiagnosticCode::InvalidSource;
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ExchangeMeshFailure meshFailure;
    ExchangeMeshContext context{
        meshFailure, warnings.GetAllocator(),  *input.registry, input.access,
        nullptr,     *input.registry->Names(), warnings};
    auto translated = Translate(context, input, traits, usage, preview);
    failure.code = meshFailure.code;
    failure.detail = meshFailure.detail;
    return translated;
}

} // namespace rocks
