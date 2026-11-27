#include <AssetRuntime/AssetTypeHooks.h>
#include <AssetRuntime/Tests/AssetTestSupport.h>
#include <Base/Compiler.h>
#include <Core/Memory/SystemAllocator.h>
#include <Material/Material.generated.h>
#include <Material/Material.h>
#include <Material/MaterialAssetHooks.h>
#include <Material/MaterialBase.generated.h>
#include <Material/MaterialFunction.generated.h>
#include <Material/MaterialFunction.h>
#include <Material/MaterialFunctionBase.generated.h>
#include <Material/MaterialFunctionInstance.generated.h>
#include <Material/MaterialFunctionInstance.h>
#include <Material/MaterialInstance.generated.h>
#include <Material/MaterialInstance.h>
#include <Material/MaterialModule.h>
#include <Material/MaterialPrerequisiteCheck.h>
#include <Material/ParameterCollection.generated.h>
#include <Material/ParameterCollection.h>
#include <MaterialSchema/Nodes/Color/ColorRampProperties.generated.h>
#include <MaterialSchema/Nodes/Color/ColorRampProperties.h>
#include <MaterialSchema/Nodes/Functions/FunctionCallProperties.generated.h>
#include <MaterialSchema/Nodes/Functions/FunctionCallProperties.h>
#include <MaterialSchema/Nodes/Functions/MaterialSampleProperties.generated.h>
#include <MaterialSchema/Nodes/Functions/MaterialSampleProperties.h>
#include <MaterialSchema/Nodes/MaterialAttributes/AttributeLayersProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/AttributeLayersProperties.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackLayer.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackLayer.h>
#include <ObjectSerialization/StructRecord.h>
#include <TestSupport/Test.h>
#include <Texture/Texture.generated.h>
#include <Texture/Texture.h>

// 머티리얼 에셋 클래스 시험이다(Docs/Design/MaterialSystem 4.2·4.6·4.6.1·19.9–19.11절, 단계 MS3b):
// 클래스 등록과 상속, 기본값이 UE와 같음, 필드 저장·로드·재저장 왕복(그래프·traits·사용 대상·
// override·Optional·레이어 스택·함수·함수 인스턴스·컬렉션), 텍스처·컬렉션 참조의 Hard 의존, 레이어
// 스택·함수 인스턴스의 EditorOnly 의존(쿠킹 패키지엔 항목 없음)과 Build hook의 패키지 판정, TODO
// 필드 검사의 필드별 양성·음성.
using namespace rocks;
namespace material_asset_tests_file {

namespace {

using material::MaterialPrerequisite;

template <class T>
TypeId TypeOf() noexcept {
    return GeneratedType<T>::Id();
}

bool IsA(TypeRegistry& types, TypeId actual, TypeId expected) noexcept {
    auto result = types.IsA(actual, expected);
    return result && result.Value();
}

// 패키지 하나와 그 대표 에셋을 만든다.
template <class T>
ObjectHandle MakeAsset(asset_test::AssetEnv& env, uint64 id, RootRef<Package>& package,
                       PackageKind kind = PackageKind::Editor) noexcept {
    ObjectRegistry& source = *env.source;
    auto made = NewPackage(source, source.OwnerAccess(), asset_test::FixedId(id), kind);
    if (!made) {
        return ObjectHandle{};
    }

    package = Move(made.Value());
    return asset_test::Make<T>(source, TypeOf<T>(), package.Handle(), "");
}

// 한 번에 로드하는 파일의 최대 수다.
constexpr usize MaxLoadedFiles = 3;

// 저장한 파일들을 다른 Registry에 한 번에 로드한다.
struct Loaded {
    RootRef<Object> roots[MaxLoadedFiles];
    ObjectHandle packages[MaxLoadedFiles];
    ObjectHandle assets[MaxLoadedFiles];
};

bool LoadFiles(asset_test::AssetEnv& env, Span<const Array<uint8>> saved, Loaded& out) noexcept {
    if (saved.Size() > MaxLoadedFiles) {
        return false;
    }

    Array<uint8> files[MaxLoadedFiles] = {Array<uint8>(env.allocator), Array<uint8>(env.allocator),
                                          Array<uint8>(env.allocator)};
    for (usize index = 0; index < saved.Size(); ++index) {
        files[index] = asset_test::Copy(env.allocator, saved[index].View());
    }

    if (!asset_test::LoadAll(env, *env.target, Span<Array<uint8>>(files, saved.Size()),
                             PackageKind::Editor, nullptr,
                             Span<RootRef<Object>>(out.roots, saved.Size()),
                             Span<ObjectHandle>(out.packages, saved.Size()))) {
        return false;
    }

    for (usize index = 0; index < saved.Size(); ++index) {
        auto asset =
            env.target->FindObject(env.target->OwnerAccess(), out.packages[index], StringView());
        if (!asset) {
            return false;
        }

        out.assets[index] = asset.Value();
    }

    return true;
}

// 로드한 패키지를 다시 써서 처음 byte와 같은가.
bool ResaveSame(asset_test::AssetEnv& env, const Array<uint8>& first,
                ObjectHandle package) noexcept {
    auto contents =
        ReadPackage(env.allocator, first.View(), asset_test::LoadOptions(PackageKind::Editor).read);
    if (!contents) {
        return false;
    }

    const Array<uint8> resaved =
        asset_test::ResaveAsRead(env.allocator, *env.target, package, contents.Value());
    return asset_test::SameBytes(first.View(), resaved.View());
}

bool HasDependency(asset_test::AssetEnv& env, const Array<uint8>& file, AssetId asset,
                   PackageDependencyKind kind) noexcept {
    auto contents =
        ReadPackage(env.allocator, file.View(), asset_test::LoadOptions(PackageKind::Editor).read);
    if (!contents) {
        return false;
    }

    for (const PackageDependencyEntry& entry : contents.Value().dependencies) {
        if (entry.asset == asset && entry.kind == kind) {
            return true;
        }
    }

    return false;
}

// 쿠킹 패키지가 asset으로 가는 의존 항목을 (종류와 상관없이) 하나라도 가졌는가. 읽기 실패는 참으로
// 돌려 시험이 통과하지 않게 한다.
bool CookedMentions(asset_test::AssetEnv& env, const Array<uint8>& file, AssetId asset) noexcept {
    auto contents =
        ReadPackage(env.allocator, file.View(), asset_test::LoadOptions(PackageKind::Cooked).read);
    if (!contents) {
        return true;
    }

    for (const PackageDependencyEntry& entry : contents.Value().dependencies) {
        if (entry.asset == asset) {
            return true;
        }
    }

    return false;
}

bool CookedHasHard(asset_test::AssetEnv& env, const Array<uint8>& file, AssetId asset) noexcept {
    auto contents =
        ReadPackage(env.allocator, file.View(), asset_test::LoadOptions(PackageKind::Cooked).read);
    if (!contents) {
        return false;
    }

    for (const PackageDependencyEntry& entry : contents.Value().dependencies) {
        if (entry.asset == asset && entry.kind == PackageDependencyKind::Hard) {
            return true;
        }
    }

    return false;
}

material::ParameterKey Key(Allocator& allocator, uint64 parameter) noexcept {
    material::ParameterKey key(allocator);
    key.ownerAssetId = asset_test::FixedId(0x700);
    key.parameterId = Guid{parameter, 1};
    return key;
}

// 선행 TODO 필드 검사: 결과가 정확히 (선행, 필드) 하나인가.
bool ExactlyOne(const Array<MaterialPrerequisiteUse>& uses, MaterialPrerequisite prerequisite,
                StringView field) noexcept {
    return uses.Size() == 1 && uses[0].prerequisite == prerequisite && uses[0].field == field;
}

} // namespace

// 등록: 클래스가 등록되고 상속 관계가 4.2·4.6.1절과 같다(함수와 함수 인스턴스는
// MaterialFunctionBase).
TEST(Material_RegistersAllAssetClasses) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterMaterialTypes(env.modules));
    CHECK(IsA(env.types, TypeOf<MaterialInstance>(), TypeOf<MaterialBase>()));
    CHECK(IsA(env.types, TypeOf<Material>(), TypeOf<MaterialBase>()));
    CHECK(IsA(env.types, TypeOf<MaterialFunction>(), TypeOf<MaterialFunctionBase>()));
    CHECK(IsA(env.types, TypeOf<MaterialFunctionInstance>(), TypeOf<MaterialFunctionBase>()));
    CHECK(IsA(env.types, TypeOf<MaterialFunctionBase>(), GeneratedType<Asset>::Id()));
    CHECK(IsA(env.types, TypeOf<ParameterCollection>(), GeneratedType<Asset>::Id()));
    CHECK(!IsA(env.types, TypeOf<MaterialFunction>(), TypeOf<MaterialBase>()));
    CHECK(!IsA(env.types, TypeOf<ParameterCollection>(), TypeOf<MaterialBase>()));

    // 추상 기반은 만들 수 없다.
    RootRef<Package> package;
    CHECK(MakeAsset<MaterialFunctionBase>(env, 0x620, package).IsNull());
}

// traits 기본값이 UE UMaterial 생성자(RE/Private/Materials/Material.cpp:1137–)·헤더 초기값과 같다.
// UE에서 생성자가 정하지 않는 값은 0·false다.
TEST(Material_DefaultTraitsMatchUnreal) {
    const material::MaterialTraitsDesc traits;
    CHECK(traits.domain == MaterialDomain::Surface);
    CHECK(traits.blendMode == BlendMode::Opaque);
    CHECK(traits.opacityMaskClipValue == 0.3333f);
    CHECK(!traits.twoSided && !traits.thinSurface && !traits.useMaterialAttributes);
    CHECK(traits.tangentSpaceNormal);
    CHECK(!traits.allowNegativeEmissive && !traits.fullyRough &&
          !traits.normalCurvatureToRoughness);
    CHECK(traits.roughnessTracking);
    CHECK(!traits.engineDefaultMaterial && !traits.maskedAssumedOpaque &&
          !traits.ditherOpacityMask);
    CHECK(!traits.ditheredLodTransition && !traits.castDynamicShadowAsMasked);
    CHECK(traits.castRayTracedShadows);
    CHECK(traits.customizedUvCount == 0);
    CHECK(traits.pixelDepthOffsetMode == material::PixelDepthOffsetMode::AlongCameraVector);
    CHECK(traits.maxWorldPositionOffset == 0.0f && !traits.alwaysEvaluateWorldPositionOffset);
    CHECK(!traits.hasPixelAnimation && !traits.outputTranslucentVelocity &&
          !traits.translucencyVelocityFromDepth);
    CHECK(traits.decalResponse == material::DecalResponse::ColorNormalRoughness);
    CHECK(!traits.responsiveAntialiasing && !traits.screenSpaceReflections &&
          !traits.contactShadows);
    CHECK(!traits.disableDepthTest && !traits.writeOnlyAlpha && !traits.sphericalParticleNormals);
    CHECK(!traits.emissiveForAreaLighting);
    CHECK(traits.lightmapDirectionality);
    CHECK(!traits.usesDistortion);
    CHECK(traits.refractionMethod == material::RefractionMethod::None);
    CHECK(traits.refractionCoverageMode == material::RefractionCoverageMode::CoverageAccountedFor);
    CHECK(traits.refractionDepthBias == 0.0f);
    CHECK(!traits.lightFunctionAtlasCompatible && !traits.alphaToCoverage &&
          !traits.translucentCustomDepthWrites);
    CHECK(traits.frontLayerTranslucency);
    CHECK(!traits.surfaceCacheSharing && !traits.wireframe);
    CHECK(traits.shadingRate == material::MaterialShadingRate::Rate1x1);
    CHECK(traits.allowVariableRateShading);
    CHECK(traits.translucencyVertexFog);
    CHECK(!traits.applyCloudFogging && !traits.isSky && !traits.fogPerPixel);
    CHECK(!traits.relaxVirtualTextureRestrictions);

    const material::MaterialDisplacementTraits& displacement = traits.displacement;
    CHECK(!displacement.enableTessellation && !displacement.enableFade);
    CHECK(displacement.magnitude == 4.0f && displacement.center == 0.5f);
    CHECK(displacement.fadeStartSizePixels == 4.0f && displacement.fadeEndSizePixels == 1.0f);

    const material::MaterialTranslucencyTraits& translucency = traits.translucency;
    CHECK(translucency.pass == material::TranslucencyPass::AfterDepthOfField);
    CHECK(translucency.lightingMode ==
          material::TranslucencyLightingMode::VolumetricNonDirectional);
    CHECK(!translucency.mobileSeparateTranslucency);
    CHECK(translucency.directionalLightingIntensity == 1.0f);
    CHECK(translucency.shadowDensityScale == 0.5f);
    CHECK(translucency.selfShadowDensityScale == 2.0f);
    CHECK(translucency.selfShadowSecondDensityScale == 10.0f);
    CHECK(translucency.selfShadowSecondOpacity == 0.0f);
    CHECK(translucency.backscatteringExponent == 30.0f);
    CHECK(translucency.multipleScatteringExtinction == LinearColor(1.0f, 0.833f, 0.588f, 1.0f));
    CHECK(translucency.shadowStartOffset == 100.0f);
    CHECK(!translucency.localLightShadow);
    CHECK(translucency.localLightShadowQuality == 0.0f &&
          translucency.directionalLightShadowQuality == 0.0f);

    const material::MaterialForwardTraits& forward = traits.forward;
    CHECK(!forward.preintegratedSimpleIbl && !forward.highQualityReflections &&
          !forward.blendSkyLightCubemaps && !forward.planarReflections);

    const material::MaterialPostProcessTraits& post = traits.postProcess;
    CHECK(post.location == material::BlendableLocation::SceneColorAfterTonemapping);
    CHECK(post.priority == 0 && post.blendable && !post.outputAlpha);
    CHECK(post.userSceneTexture.IsNone() && post.resolutionRelativeToInput.IsNone());
    CHECK(post.userTextureDivisorX == 0 && post.userTextureDivisorY == 0);
    CHECK(!post.disablePreExposureScale && !post.stencilTest);
    CHECK(post.stencilCompare == material::StencilCompare::Less && post.stencilRef == 0);
}

// 나머지 값 타입 기본값이 UE와 같다(조명 굽기 FLightmassMaterialInterfaceSettings, 텍스처 정보
// FMaterialTextureInfo, 레이어 줄).
TEST(Material_DefaultValueTypesMatchUnreal) {
    const MaterialLightBakeSettings bake;
    CHECK(!bake.castShadowAsMasked);
    CHECK(bake.emissiveBoost == 1.0f && bake.diffuseBoost == 1.0f &&
          bake.exportResolutionScale == 1.0f);

    const material::MaterialTextureSamplingScale scale;
    CHECK(scale.uvChannel == material::NoUvChannel);
    CHECK(scale.samplingScale == 0.0f);

    SystemAllocator allocator;
    const MaterialLayerEntry entry(allocator);
    CHECK(entry.visible && !entry.restrictToLayerRelatives && !entry.restrictToBlendRelatives);
    CHECK(entry.linkState == MaterialLayerLinkState::NotFromParent);
    CHECK(entry.layer.Handle().IsNull() && entry.blend.Handle().IsNull());

    const MaterialLightBakeOverrides overrides{};
    CHECK(!overrides.castShadowAsMasked.HasValue() && !overrides.emissiveBoost.HasValue());
}

// 사용 대상 도우미: 중복 없이 선언 순서로 더하고 None은 거부한다.
TEST(Material_VertexInputUsageHelpers) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    NamePool& names = *env.source->Names();
    const Name staticMesh = names.TryIntern(material::StaticMeshVertexInputName).Value();
    const Name skinned = names.TryIntern("SkinnedMesh").Value();
    material::VertexInputUsage usage(env.allocator);
    CHECK(material::AddVertexInput(usage, skinned));
    CHECK(material::AddVertexInput(usage, staticMesh));
    CHECK(material::AddVertexInput(usage, skinned));
    REQUIRE(usage.vertexInputs.Size() == 2);
    CHECK(usage.vertexInputs[0] == skinned && usage.vertexInputs[1] == staticMesh);
    CHECK(material::ContainsVertexInput(usage, staticMesh));
    CHECK(!material::ContainsVertexInput(usage, Name()));
    CHECK(material::AddVertexInput(usage, Name()).Error() == ErrorCode::InvalidArgument);
}

// Material 필드 왕복: 그래프(EditorOnly)·traits·사용 대상·물리 재질 표가 저장·로드 뒤에도 같고,
// 다시 쓴 byte가 처음과 같다(그래프 저장/재열기 왕복 — 15.2절 MS1a 기준의 이월분).
TEST(Material_FieldsRoundTrip) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterMaterialTypes(env.modules));
    ObjectRegistry& source = *env.source;
    RootRef<Package> package;
    const ObjectHandle handle = MakeAsset<Material>(env, 0x610, package);
    REQUIRE(!handle.IsNull());

    NamePool& names = *source.Names();
    const Name staticMesh = names.TryIntern(material::StaticMeshVertexInputName).Value();
    const Name outputType = names.TryIntern("Material.Output").Value();
    const Name domain = names.TryIntern("Material").Value();
    REQUIRE(asset_test::WithBorrow<Material>(source, handle, [&](Material& material) noexcept {
        material.traits.blendMode = BlendMode::Masked;
        material.traits.opacityMaskClipValue = 0.5f;
        material.traits.twoSided = true;
        material.traits.translucency.pass = material::TranslucencyPass::AfterMotionBlur;
        material.traits.postProcess.priority = 7;
        material.physicalMaterialMap[3].assetId = asset_test::FixedId(0x803);
        if (!material::AddVertexInput(material.usage, staticMesh)) {
            return false;
        }

#if ROCKS_WITH_EDITOR_DATA
        material.graph.domain = domain;
        nodegraph::Graph main(source.ObjectAllocator());
        main.id = Guid{0x10, 1};
        nodegraph::GraphNode output(source.ObjectAllocator());
        output.id = Guid{0x20, 2};
        output.type = outputType;
        output.typeSchemaVersion = 1;
        if (!main.nodes.TryPushBack(Move(output))) {
            return false;
        }

        material.graph.mainGraph = main.id;
        if (!material.graph.graphs.TryPushBack(Move(main))) {
            return false;
        }

        material.preview.cameraPitch = 30;
#endif
        return true;
    }));

    const Array<uint8> files[1] = {
        asset_test::Write(env.allocator, source, package.Handle(), asset_test::EditorOptions())};
    REQUIRE(!files[0].IsEmpty());
    Loaded loaded;
    REQUIRE(LoadFiles(env, files, loaded));
    Material* copy = asset_test::Pointer<Material>(*env.target, loaded.assets[0]);
    REQUIRE(copy != nullptr);
    CHECK(copy->traits.blendMode == BlendMode::Masked);
    CHECK(copy->traits.opacityMaskClipValue == 0.5f);
    CHECK(copy->traits.twoSided);
    CHECK(copy->traits.translucency.pass == material::TranslucencyPass::AfterMotionBlur);
    CHECK(copy->traits.postProcess.priority == 7);
    CHECK(copy->physicalMaterialMap[3].assetId == asset_test::FixedId(0x803));
    CHECK(copy->physicalMaterialMap[2].IsNull());
    REQUIRE(copy->usage.vertexInputs.Size() == 1);
    CHECK(copy->usage.vertexInputs[0] == env.target->Names()->TryIntern("StaticMesh").Value());
#if ROCKS_WITH_EDITOR_DATA
    REQUIRE(copy->graph.graphs.Size() == 1);
    REQUIRE(copy->graph.graphs[0].nodes.Size() == 1);
    CHECK(copy->graph.graphs[0].nodes[0].id == (Guid{0x20, 2}));
    CHECK(copy->preview.cameraPitch == 30);
#endif
    CHECK(ResaveSame(env, files[0], loaded.packages[0]));
}

// MaterialInstance 필드 왕복: 파라미터 override(키 포함)·static enum·글꼴 page·기본 속성 override
// (Optional enum·float·int, 사용 대상 added/removed)·조명 굽기 override·Optional null 덮기가 남고,
// 재저장 byte가 같다.
TEST(Material_InstanceOverridesRoundTrip) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterMaterialTypes(env.modules));
    ObjectRegistry& source = *env.source;
    RootRef<Package> package;
    const ObjectHandle handle = MakeAsset<MaterialInstance>(env, 0x611, package);
    REQUIRE(!handle.IsNull());
    const Name skinned = source.Names()->TryIntern("SkinnedMesh").Value();
    const Name staticMesh = source.Names()->TryIntern("StaticMesh").Value();

    REQUIRE(asset_test::WithBorrow<MaterialInstance>(
        source, handle, [&](MaterialInstance& instance) noexcept {
            Allocator& allocator = source.ObjectAllocator();
            material::ScalarParameterOverride roughness(allocator);
            roughness.key = Key(allocator, 0x30);
            roughness.key.scope.kind = material::ParameterScopeKind::FunctionCall;
            if (!roughness.key.scope.callPath.TryPushBack(Guid{0x40, 4})) {
                return false;
            }

            roughness.value = 0.25f;
            material::StaticEnumOverride preset(allocator);
            preset.key = Key(allocator, 0x32);
            preset.value = 3;
            material::FontParameterOverride font(allocator);
            font.key = Key(allocator, 0x33);
            font.page = 2;
            instance.traitOverrides.blendMode = BlendMode::Translucent;
            instance.traitOverrides.shadingModel = material::ShadingModelPreset::PreintegratedSkin;
            instance.traitOverrides.blendableLocation =
                material::BlendableLocation::ReplacingTonemapper;
            instance.traitOverrides.maxWorldPositionOffset = 12.5f;
            instance.traitOverrides.blendablePriority = -3;
            instance.lightBakeOverrides.diffuseBoost = 2.0f;
            // 없음으로 덮기(값이 있는 null 참조).
            instance.physicalMaterialOverride = material::MaterialAssetReference{};
#if ROCKS_WITH_EDITOR_DATA
            // 레이어 스택의 참조 아닌 칸(함수 참조 왕복은 다른 시험이 본다).
            instance.overridesLayerStack = true;
            auto layerIndex =
                source.AddElement(source.OwnerAccess(), handle, instance.layerStack.layers);
            if (!layerIndex) {
                return false;
            }

            MaterialLayerEntry& entry = instance.layerStack.layers[layerIndex.Value()];
            entry.layerId = Guid{0x61, 6};
            entry.visible = false;
            entry.restrictToLayerRelatives = true;
            entry.restrictToBlendRelatives = true;
            entry.linkState = MaterialLayerLinkState::UnlinkedFromParent;
            if (!entry.displayName.TryAssign("Moss") ||
                !instance.layerStack.deletedParentLayerIds.TryPushBack(Guid{0x62, 6})) {
                return false;
            }
#endif
            return instance.scalarOverrides.TryPushBack(Move(roughness)) &&
                   instance.staticEnumOverrides.TryPushBack(Move(preset)) &&
                   instance.fontOverrides.TryPushBack(Move(font)) &&
                   instance.traitOverrides.addedVertexInputs.TryPushBack(skinned) &&
                   instance.traitOverrides.removedVertexInputs.TryPushBack(staticMesh);
        }));

    const Array<uint8> files[1] = {
        asset_test::Write(env.allocator, source, package.Handle(), asset_test::EditorOptions())};
    REQUIRE(!files[0].IsEmpty());
    Loaded loaded;
    REQUIRE(LoadFiles(env, files, loaded));
    MaterialInstance* copy = asset_test::Pointer<MaterialInstance>(*env.target, loaded.assets[0]);
    REQUIRE(copy != nullptr);
    REQUIRE(copy->scalarOverrides.Size() == 1);
    const material::ScalarParameterOverride& roughness = copy->scalarOverrides[0];
    CHECK(roughness.key.ownerAssetId == asset_test::FixedId(0x700));
    CHECK(roughness.key.parameterId == (Guid{0x30, 1}));
    CHECK(roughness.key.scope.kind == material::ParameterScopeKind::FunctionCall);
    REQUIRE(roughness.key.scope.callPath.Size() == 1);
    CHECK(roughness.key.scope.callPath[0] == (Guid{0x40, 4}));
    CHECK(roughness.value == 0.25f);
    REQUIRE(copy->staticEnumOverrides.Size() == 1);
    CHECK(copy->staticEnumOverrides[0].value == 3);
    REQUIRE(copy->fontOverrides.Size() == 1);
    CHECK(copy->fontOverrides[0].page == 2);
    const material::MaterialTraitOverrides& traits = copy->traitOverrides;
    CHECK(traits.blendMode.HasValue() && traits.blendMode.Value() == BlendMode::Translucent);
    CHECK(traits.shadingModel.HasValue() &&
          traits.shadingModel.Value() == material::ShadingModelPreset::PreintegratedSkin);
    CHECK(traits.blendableLocation.HasValue() &&
          traits.blendableLocation.Value() == material::BlendableLocation::ReplacingTonemapper);
    CHECK(traits.maxWorldPositionOffset.HasValue() &&
          traits.maxWorldPositionOffset.Value() == 12.5f);
    CHECK(traits.blendablePriority.HasValue() && traits.blendablePriority.Value() == -3);
    CHECK(!traits.opacityMaskClipValue.HasValue() && !traits.twoSided.HasValue());
    REQUIRE(traits.addedVertexInputs.Size() == 1 && traits.removedVertexInputs.Size() == 1);
    CHECK(copy->lightBakeOverrides.diffuseBoost.HasValue() &&
          copy->lightBakeOverrides.diffuseBoost.Value() == 2.0f);
    CHECK(!copy->lightBakeOverrides.emissiveBoost.HasValue());
    CHECK(copy->physicalMaterialOverride.HasValue() &&
          copy->physicalMaterialOverride.Value().IsNull());
    CHECK(copy->physicalMaterialMask.IsNull());
#if ROCKS_WITH_EDITOR_DATA
    CHECK(copy->overridesLayerStack);
    REQUIRE(copy->layerStack.layers.Size() == 1);
    const MaterialLayerEntry& layer = copy->layerStack.layers[0];
    CHECK(layer.layerId == (Guid{0x61, 6}));
    CHECK(!layer.visible && layer.restrictToLayerRelatives && layer.restrictToBlendRelatives);
    CHECK(layer.linkState == MaterialLayerLinkState::UnlinkedFromParent);
    CHECK(layer.displayName.View() == StringView("Moss"));
    CHECK(layer.layer.Handle().IsNull() && layer.blend.Handle().IsNull());
    REQUIRE(copy->layerStack.deletedParentLayerIds.Size() == 1);
    CHECK(copy->layerStack.deletedParentLayerIds[0] == (Guid{0x62, 6}));
#endif
    CHECK(ResaveSame(env, files[0], loaded.packages[0]));
}

// 텍스처 override는 다른 패키지 텍스처의 Hard 의존이다. 두 패키지를 함께 로드하면 참조가 이어진다.
TEST(Material_InstanceTextureOverrideIsHardDependency) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterMaterialTypes(env.modules));
    ObjectRegistry& source = *env.source;
    const ObjectAccessContext access = source.OwnerAccess();
    RootRef<Package> texturePackage;
    const ObjectHandle texture = MakeAsset<Texture2D>(env, 0x630, texturePackage);
    RootRef<Package> instancePackage;
    const ObjectHandle instance = MakeAsset<MaterialInstance>(env, 0x631, instancePackage);
    REQUIRE(!texture.IsNull() && !instance.IsNull());

    REQUIRE(asset_test::WithBorrow<MaterialInstance>(
        source, instance, [&](MaterialInstance& value) noexcept {
            auto index = source.AddElement(access, instance, value.textureOverrides);
            if (!index) {
                return false;
            }

            TextureParameterOverride& element = value.textureOverrides[index.Value()];
            element.key.parameterId = Guid{0x50, 5};
            return source
                .SetElementReference(access, instance, value.textureOverrides, index.Value(),
                                     element.value, texture)
                .IsOk();
        }));

    const Array<uint8> files[2] = {asset_test::Write(env.allocator, source, texturePackage.Handle(),
                                                     asset_test::EditorOptions()),
                                   asset_test::Write(env.allocator, source,
                                                     instancePackage.Handle(),
                                                     asset_test::EditorOptions())};
    REQUIRE(!files[0].IsEmpty() && !files[1].IsEmpty());
    CHECK(HasDependency(env, files[1], asset_test::FixedId(0x630), PackageDependencyKind::Hard));

    Loaded loaded;
    REQUIRE(LoadFiles(env, files, loaded));
    MaterialInstance* copy = asset_test::Pointer<MaterialInstance>(*env.target, loaded.assets[1]);
    REQUIRE(copy != nullptr);
    REQUIRE(copy->textureOverrides.Size() == 1);
    CHECK(copy->textureOverrides[0].key.parameterId == (Guid{0x50, 5}));
    CHECK(copy->textureOverrides[0].value.Handle() == loaded.assets[0]);
    CHECK(ResaveSame(env, files[1], loaded.packages[1]));
}

// 레이어 스택과 함수 인스턴스 부모는 EditorOnly 의존(쿠킹 Hard 간선 없음)이고, Build 의존은 hook이
// 선언한다(4.6.1절).
TEST(Material_FunctionReferencesAreEditorOnlyAndBuild) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterMaterialTypes(env.modules));
    ObjectRegistry& source = *env.source;
    const ObjectAccessContext access = source.OwnerAccess();
    RootRef<Package> functionPackage;
    const ObjectHandle function = MakeAsset<MaterialFunction>(env, 0x640, functionPackage);
    RootRef<Package> parentPackage;
    const ObjectHandle parent = MakeAsset<Material>(env, 0x641, parentPackage);
    RootRef<Package> instancePackage;
    const ObjectHandle instance = MakeAsset<MaterialInstance>(env, 0x642, instancePackage);
    RootRef<Package> functionInstancePackage;
    const ObjectHandle functionInstance =
        MakeAsset<MaterialFunctionInstance>(env, 0x643, functionInstancePackage);
    REQUIRE(!function.IsNull() && !parent.IsNull() && !instance.IsNull() &&
            !functionInstance.IsNull());

    REQUIRE(asset_test::WithBorrow<MaterialInstance>(
        source, instance, [&](MaterialInstance& value) noexcept {
            if (!source.SetReference(access, instance, value.parent, parent)) {
                return false;
            }

#if ROCKS_WITH_EDITOR_DATA
            value.overridesLayerStack = true;
            auto index = source.AddElement(access, instance, value.layerStack.layers);
            if (!index) {
                return false;
            }

            MaterialLayerEntry& entry = value.layerStack.layers[index.Value()];
            entry.layerId = Guid{0x60, 6};
            entry.linkState = MaterialLayerLinkState::LinkedToParent;
            return source
                .SetElementReference(access, instance, value.layerStack.layers, index.Value(),
                                     entry.layer, function)
                .IsOk();
#else
            return true;
#endif
        }));
#if ROCKS_WITH_EDITOR_DATA
    REQUIRE(asset_test::WithBorrow<MaterialFunctionInstance>(
        source, functionInstance, [&](MaterialFunctionInstance& value) noexcept {
            return static_cast<bool>(
                source.SetReference(access, functionInstance, value.parent, function));
        }));
#endif

    const Array<uint8> instanceFile = asset_test::Write(
        env.allocator, source, instancePackage.Handle(), asset_test::EditorOptions());
    REQUIRE(!instanceFile.IsEmpty());
    CHECK(
        HasDependency(env, instanceFile, asset_test::FixedId(0x641), PackageDependencyKind::Hard));
#if ROCKS_WITH_EDITOR_DATA
    CHECK(HasDependency(env, instanceFile, asset_test::FixedId(0x640),
                        PackageDependencyKind::EditorOnly));
    CHECK(
        !HasDependency(env, instanceFile, asset_test::FixedId(0x640), PackageDependencyKind::Hard));
#endif

    AssetTypeHookTable table(env.allocator, env.types);
    Array<AssetTypeHookRegistration> registrations(env.allocator);
    REQUIRE(RegisterMaterialAssetHooks(table, registrations));
    CHECK(RegisterMaterialAssetHooks(table, registrations).Error() == ErrorCode::AlreadyExists);
#if ROCKS_WITH_EDITOR_DATA
    CHECK(registrations.Size() == 4);
#else
    CHECK(registrations.Size() == 2);
#endif

    AssetDependencySink instanceSink(env.allocator);
    REQUIRE(table.CollectBuildDependencies(source, access, instance, TypeOf<MaterialInstance>(),
                                           asset_test::FixedId(0x642), instanceSink));
    bool parentDeclared = false;
    bool functionDeclared = false;
    for (const PackageDependencyEntry& entry : instanceSink.Dependencies()) {
        CHECK(entry.kind == PackageDependencyKind::Build);
        parentDeclared = parentDeclared || entry.asset == asset_test::FixedId(0x641);
        functionDeclared = functionDeclared || entry.asset == asset_test::FixedId(0x640);
    }

    CHECK(parentDeclared);
#if ROCKS_WITH_EDITOR_DATA
    CHECK(functionDeclared);
    AssetDependencySink functionSink(env.allocator);
    REQUIRE(table.CollectBuildDependencies(source, access, functionInstance,
                                           TypeOf<MaterialFunctionInstance>(),
                                           asset_test::FixedId(0x643), functionSink));
    REQUIRE(functionSink.Dependencies().Size() == 1);
    CHECK(functionSink.Dependencies()[0].asset == asset_test::FixedId(0x640));
#else
    (void)functionDeclared;
#endif
    registrations.Clear();
}

#if ROCKS_WITH_EDITOR_DATA
// 그래프 노드 하나를 붙인다(속성은 리플렉션 Record).
template <class Properties>
bool AddGraphNode(asset_test::AssetEnv& env, nodegraph::GraphDocument& document, Guid id,
                  StringView type, const Properties& properties) noexcept {
    ObjectRegistry& source = *env.source;
    auto lease = env.types.AcquireValueType(GeneratedType<Properties>::Id(), TypeKind::Struct);
    if (!lease) {
        return false;
    }

    auto saved =
        SaveStructRecord(env.allocator, source, source.OwnerAccess(), lease.Value(), &properties);
    auto name = source.Names()->TryIntern(type);
    if (!saved || !name) {
        return false;
    }

    if (document.graphs.IsEmpty()) {
        nodegraph::Graph main(source.ObjectAllocator());
        main.id = Guid{0x10, 1};
        document.mainGraph = main.id;
        if (!document.graphs.TryPushBack(Move(main))) {
            return false;
        }
    }

    nodegraph::GraphNode node(source.ObjectAllocator());
    node.id = id;
    node.type = name.Value();
    node.typeSchemaVersion = 1;
    return node.properties.bytes.TryAppend(saved.Value().View()).IsOk() &&
           document.graphs[0].nodes.TryPushBack(Move(node)).IsOk();
}

// 그래프 안 FunctionCall·MaterialSample·레이어 노드 참조는 Build 의존이다(6.12절 MS2e2·MS2e3 —
// 자기·null 참조는 뺀다).
TEST(Material_GraphReferencesAreBuildDependencies) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterMaterialTypes(env.modules));
    ObjectRegistry& source = *env.source;
    const ObjectAccessContext access = source.OwnerAccess();
    RootRef<Package> materialPackage;
    const ObjectHandle material = MakeAsset<Material>(env, 0x6a0, materialPackage);
    RootRef<Package> functionPackage;
    const ObjectHandle function = MakeAsset<MaterialFunction>(env, 0x6a1, functionPackage);
    REQUIRE(!material.IsNull() && !function.IsNull());

    REQUIRE(asset_test::WithBorrow<Material>(source, material, [&](Material& value) noexcept {
        material::FunctionCallProperties call(source.ObjectAllocator());
        call.materialFunction.assetId = asset_test::FixedId(0x6a1);
        material::MaterialSampleProperties sample;
        sample.materialReference.assetId = asset_test::FixedId(0x6a2);
        material::MaterialSampleProperties self;
        self.materialReference.assetId = asset_test::FixedId(0x6a0);
        material::FunctionCallProperties empty(source.ObjectAllocator());
        material::AttributeLayersProperties layers(source.ObjectAllocator());
        material::LayerStackLayer layer(source.ObjectAllocator());
        layer.layerId = Guid{0x40, 1};
        layer.layer.assetId = asset_test::FixedId(0x6a4);
        layer.blend.assetId = asset_test::FixedId(0x6a5);
        value.graph.domain = source.Names()->TryIntern("Material").Value();
        return layers.defaultLayers.layers.TryPushBack(Move(layer)).IsOk() &&
               AddGraphNode(env, value.graph, Guid{0x20, 1}, "Material.FunctionCall", call) &&
               AddGraphNode(env, value.graph, Guid{0x20, 2}, "Material.MaterialSample", sample) &&
               AddGraphNode(env, value.graph, Guid{0x20, 3}, "Material.MaterialSample", self) &&
               AddGraphNode(env, value.graph, Guid{0x20, 4}, "Material.FunctionCall", empty) &&
               AddGraphNode(env, value.graph, Guid{0x20, 5}, "Material.AttributeLayers", layers);
    }));
    REQUIRE(asset_test::WithBorrow<MaterialFunction>(
        source, function, [&](MaterialFunction& value) noexcept {
            material::FunctionCallProperties call(source.ObjectAllocator());
            call.materialFunction.assetId = asset_test::FixedId(0x6a3);
            material::ColorRampProperties ramp;
            ramp.colorCurve.assetId = asset_test::FixedId(0x6a6);
            value.graph.domain = source.Names()->TryIntern("MaterialFunction").Value();
            return AddGraphNode(env, value.graph, Guid{0x30, 1}, "Material.FunctionCall", call) &&
                   AddGraphNode(env, value.graph, Guid{0x30, 2}, "Material.ColorRamp", ramp);
        }));

    AssetTypeHookTable table(env.allocator, env.types);
    Array<AssetTypeHookRegistration> registrations(env.allocator);
    REQUIRE(RegisterMaterialAssetHooks(table, registrations));
    AssetDependencySink materialSink(env.allocator);
    REQUIRE(table.CollectBuildDependencies(source, access, material, TypeOf<Material>(),
                                           asset_test::FixedId(0x6a0), materialSink));
    REQUIRE(materialSink.Dependencies().Size() == 4);
    CHECK(materialSink.Dependencies()[0].asset == asset_test::FixedId(0x6a1));
    CHECK(materialSink.Dependencies()[1].asset == asset_test::FixedId(0x6a2));
    CHECK(materialSink.Dependencies()[2].asset == asset_test::FixedId(0x6a4));
    CHECK(materialSink.Dependencies()[3].asset == asset_test::FixedId(0x6a5));
    for (const PackageDependencyEntry& entry : materialSink.Dependencies()) {
        CHECK(entry.kind == PackageDependencyKind::Build);
    }

    AssetDependencySink functionSink(env.allocator);
    REQUIRE(table.CollectBuildDependencies(source, access, function, TypeOf<MaterialFunction>(),
                                           asset_test::FixedId(0x6a1), functionSink));
    REQUIRE(functionSink.Dependencies().Size() == 2);
    CHECK(functionSink.Dependencies()[0].asset == asset_test::FixedId(0x6a3));
    // ColorRamp 색 커브도 번역이 읽는 Build 의존이다(6.13절 MS2f4).
    CHECK(functionSink.Dependencies()[1].asset == asset_test::FixedId(0x6a6));
    registrations.Clear();
}
#endif

// 함수·컬렉션 왕복: EditorOnly 필드·쓰임·상속 칸이 남고 재저장 byte가 같다.
TEST(Material_FunctionAndCollectionRoundTrip) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterMaterialTypes(env.modules));
    ObjectRegistry& source = *env.source;
    RootRef<Package> functionPackage;
    const ObjectHandle function = MakeAsset<MaterialFunction>(env, 0x650, functionPackage);
    RootRef<Package> collectionPackage;
    const ObjectHandle collection = MakeAsset<ParameterCollection>(env, 0x651, collectionPackage);
    REQUIRE(!function.IsNull() && !collection.IsNull());
    const Name wetness = source.Names()->TryIntern("Wetness").Value();

    REQUIRE(asset_test::WithBorrow<MaterialFunction>(
        source, function, [&](MaterialFunction& value) noexcept {
            value.usage = material::MaterialFunctionUsage::LayerBlend;
#if ROCKS_WITH_EDITOR_DATA
            value.exposeToLibrary = true;
            return value.userExposedCaption.TryAssign("Blend Layers") &&
                   value.description.TryAssign("Blends two layers.");
#else
            return true;
#endif
        }));
    REQUIRE(asset_test::WithBorrow<ParameterCollection>(
        source, collection, [&](ParameterCollection& value) noexcept {
            material::ScalarCollectionParameter scalar;
            scalar.id = Guid{0x70, 7};
            scalar.name = wetness;
            scalar.defaultValue = 0.5f;
            material::ScalarCollectionBaseOverride inherited;
            inherited.parameterId = Guid{0x71, 7};
            inherited.value = 0.75f;
            return value.scalars.TryPushBack(scalar) &&
                   value.scalarBaseOverrides.TryPushBack(inherited);
        }));

    const Array<uint8> files[2] = {
        asset_test::Write(env.allocator, source, functionPackage.Handle(),
                          asset_test::EditorOptions()),
        asset_test::Write(env.allocator, source, collectionPackage.Handle(),
                          asset_test::EditorOptions())};
    REQUIRE(!files[0].IsEmpty() && !files[1].IsEmpty());
    Loaded loaded;
    REQUIRE(LoadFiles(env, files, loaded));
    MaterialFunction* functionCopy =
        asset_test::Pointer<MaterialFunction>(*env.target, loaded.assets[0]);
    ParameterCollection* collectionCopy =
        asset_test::Pointer<ParameterCollection>(*env.target, loaded.assets[1]);
    REQUIRE(functionCopy != nullptr && collectionCopy != nullptr);
    CHECK(functionCopy->usage == material::MaterialFunctionUsage::LayerBlend);
#if ROCKS_WITH_EDITOR_DATA
    CHECK(functionCopy->exposeToLibrary);
    CHECK(functionCopy->userExposedCaption.View() == StringView("Blend Layers"));
#endif
    REQUIRE(collectionCopy->scalars.Size() == 1);
    CHECK(collectionCopy->scalars[0].defaultValue == 0.5f);
    REQUIRE(collectionCopy->scalarBaseOverrides.Size() == 1);
    CHECK(collectionCopy->scalarBaseOverrides[0].value == 0.75f);
    CHECK(collectionCopy->base.Handle().IsNull());
    CHECK(ResaveSame(env, files[0], loaded.packages[0]));
    CHECK(ResaveSame(env, files[1], loaded.packages[1]));
}

#if ROCKS_WITH_EDITOR_DATA
// MaterialFunctionInstance 왕복: 부모(다른 패키지 함수 — EditorOnly 의존)·static enum·글꼴 page·
// 컬렉션 override·scalar override가 남고, 로드하면 부모 참조가 이어지며, 재저장 byte가 같다.
TEST(Material_FunctionInstanceRoundTrip) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterMaterialTypes(env.modules));
    ObjectRegistry& source = *env.source;
    const ObjectAccessContext access = source.OwnerAccess();
    RootRef<Package> functionPackage;
    const ObjectHandle function = MakeAsset<MaterialFunction>(env, 0x670, functionPackage);
    RootRef<Package> instancePackage;
    const ObjectHandle instance = MakeAsset<MaterialFunctionInstance>(env, 0x671, instancePackage);
    REQUIRE(!function.IsNull() && !instance.IsNull());

    REQUIRE(asset_test::WithBorrow<MaterialFunctionInstance>(
        source, instance, [&](MaterialFunctionInstance& value) noexcept {
            Allocator& allocator = source.ObjectAllocator();
            if (!source.SetReference(access, instance, value.parent, function)) {
                return false;
            }

            material::ScalarParameterOverride scalar(allocator);
            scalar.key = Key(allocator, 0x34);
            scalar.value = 0.125f;
            material::StaticEnumOverride preset(allocator);
            preset.key = Key(allocator, 0x35);
            preset.value = 5;
            material::FontParameterOverride font(allocator);
            font.key = Key(allocator, 0x36);
            font.font.assetId = asset_test::FixedId(0x8F0);
            font.page = 4;
            material::AssetParameterOverride collection(allocator);
            collection.key = Key(allocator, 0x37);
            collection.value.assetId = asset_test::FixedId(0x8F1);
            return value.scalarOverrides.TryPushBack(Move(scalar)) &&
                   value.staticEnumOverrides.TryPushBack(Move(preset)) &&
                   value.fontOverrides.TryPushBack(Move(font)) &&
                   value.collectionOverrides.TryPushBack(Move(collection));
        }));

    const Array<uint8> files[2] = {
        asset_test::Write(env.allocator, source, functionPackage.Handle(),
                          asset_test::EditorOptions()),
        asset_test::Write(env.allocator, source, instancePackage.Handle(),
                          asset_test::EditorOptions())};
    REQUIRE(!files[0].IsEmpty() && !files[1].IsEmpty());
    CHECK(HasDependency(env, files[1], asset_test::FixedId(0x670),
                        PackageDependencyKind::EditorOnly));
    CHECK(!HasDependency(env, files[1], asset_test::FixedId(0x670), PackageDependencyKind::Hard));

    Loaded loaded;
    REQUIRE(LoadFiles(env, files, loaded));
    MaterialFunctionInstance* copy =
        asset_test::Pointer<MaterialFunctionInstance>(*env.target, loaded.assets[1]);
    REQUIRE(copy != nullptr);
    CHECK(copy->parent.Handle() == loaded.assets[0]);
    REQUIRE(copy->scalarOverrides.Size() == 1);
    CHECK(copy->scalarOverrides[0].key.parameterId == (Guid{0x34, 1}));
    CHECK(copy->scalarOverrides[0].value == 0.125f);
    REQUIRE(copy->staticEnumOverrides.Size() == 1);
    CHECK(copy->staticEnumOverrides[0].value == 5);
    REQUIRE(copy->fontOverrides.Size() == 1);
    CHECK(copy->fontOverrides[0].font.assetId == asset_test::FixedId(0x8F0));
    CHECK(copy->fontOverrides[0].page == 4);
    REQUIRE(copy->collectionOverrides.Size() == 1);
    CHECK(copy->collectionOverrides[0].value.assetId == asset_test::FixedId(0x8F1));
    CHECK(ResaveSame(env, files[1], loaded.packages[1]));
}
#endif

// 파라미터 컬렉션 참조: 머티리얼의 parameterCollections와 컬렉션 base는 Hard 의존이고, 세 패키지를
// 함께 로드하면 참조가 이어진다. 상속 기본값 덮기(scalar·vector)가 남는다.
TEST(Material_ParameterCollectionReferencesAreHard) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterMaterialTypes(env.modules));
    ObjectRegistry& source = *env.source;
    const ObjectAccessContext access = source.OwnerAccess();
    RootRef<Package> basePackage;
    const ObjectHandle base = MakeAsset<ParameterCollection>(env, 0x680, basePackage);
    RootRef<Package> childPackage;
    const ObjectHandle child = MakeAsset<ParameterCollection>(env, 0x681, childPackage);
    RootRef<Package> materialPackage;
    const ObjectHandle material = MakeAsset<Material>(env, 0x682, materialPackage);
    REQUIRE(!base.IsNull() && !child.IsNull() && !material.IsNull());

    REQUIRE(asset_test::WithBorrow<ParameterCollection>(
        source, base, [&](ParameterCollection& value) noexcept {
            material::VectorCollectionParameter tint;
            tint.id = Guid{0x72, 7};
            tint.defaultValue = LinearColor(1.0f, 0.0f, 0.0f, 1.0f);
            return value.vectors.TryPushBack(tint);
        }));
    REQUIRE(asset_test::WithBorrow<ParameterCollection>(
        source, child, [&](ParameterCollection& value) noexcept {
            material::VectorCollectionBaseOverride inherited;
            inherited.parameterId = Guid{0x72, 7};
            inherited.value = LinearColor(0.0f, 0.5f, 1.0f, 1.0f);
            return source.SetReference(access, child, value.base, base).IsOk() &&
                   value.vectorBaseOverrides.TryPushBack(inherited);
        }));
    REQUIRE(asset_test::WithBorrow<Material>(source, material, [&](Material& value) noexcept {
        return source.AddReference(access, material, value.parameterCollections, child).IsOk();
    }));

    const Array<uint8> files[3] = {
        asset_test::Write(env.allocator, source, basePackage.Handle(), asset_test::EditorOptions()),
        asset_test::Write(env.allocator, source, childPackage.Handle(),
                          asset_test::EditorOptions()),
        asset_test::Write(env.allocator, source, materialPackage.Handle(),
                          asset_test::EditorOptions())};
    REQUIRE(!files[0].IsEmpty() && !files[1].IsEmpty() && !files[2].IsEmpty());
    CHECK(HasDependency(env, files[1], asset_test::FixedId(0x680), PackageDependencyKind::Hard));
    CHECK(HasDependency(env, files[2], asset_test::FixedId(0x681), PackageDependencyKind::Hard));

    Loaded loaded;
    REQUIRE(LoadFiles(env, files, loaded));
    ParameterCollection* childCopy =
        asset_test::Pointer<ParameterCollection>(*env.target, loaded.assets[1]);
    Material* materialCopy = asset_test::Pointer<Material>(*env.target, loaded.assets[2]);
    REQUIRE(childCopy != nullptr && materialCopy != nullptr);
    CHECK(childCopy->base.Handle() == loaded.assets[0]);
    REQUIRE(childCopy->vectorBaseOverrides.Size() == 1);
    CHECK(childCopy->vectorBaseOverrides[0].parameterId == (Guid{0x72, 7}));
    CHECK(childCopy->vectorBaseOverrides[0].value == LinearColor(0.0f, 0.5f, 1.0f, 1.0f));
    REQUIRE(materialCopy->parameterCollections.Size() == 1);
    CHECK(materialCopy->parameterCollections[0] == loaded.assets[1]);
    CHECK(ResaveSame(env, files[1], loaded.packages[1]));
    CHECK(ResaveSame(env, files[2], loaded.packages[2]));
}

#if ROCKS_WITH_EDITOR_DATA
// 쿠킹 패키지: 레이어 스택의 함수 참조와 함수 인스턴스의 모든 참조(부모·텍스처 override)는
// EditorOnly라 쿠킹 패키지에 의존 항목이 아예 없다. 부모 머티리얼은 쿠킹에도 Hard다(4.6.1절).
TEST(Material_CookedPackagesDropEditorOnlyReferences) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterMaterialTypes(env.modules));
    ObjectRegistry& source = *env.source;
    const ObjectAccessContext access = source.OwnerAccess();
    const PackageKind cooked = PackageKind::Cooked;
    RootRef<Package> functionPackage;
    const ObjectHandle function = MakeAsset<MaterialFunction>(env, 0x690, functionPackage, cooked);
    RootRef<Package> parentPackage;
    const ObjectHandle parent = MakeAsset<Material>(env, 0x691, parentPackage, cooked);
    RootRef<Package> instancePackage;
    const ObjectHandle instance = MakeAsset<MaterialInstance>(env, 0x692, instancePackage, cooked);
    RootRef<Package> functionInstancePackage;
    const ObjectHandle functionInstance =
        MakeAsset<MaterialFunctionInstance>(env, 0x693, functionInstancePackage, cooked);
    RootRef<Package> texturePackage;
    const ObjectHandle texture = MakeAsset<Texture2D>(env, 0x694, texturePackage, cooked);
    REQUIRE(!function.IsNull() && !parent.IsNull() && !instance.IsNull() &&
            !functionInstance.IsNull() && !texture.IsNull());

    REQUIRE(asset_test::WithBorrow<MaterialInstance>(
        source, instance, [&](MaterialInstance& value) noexcept {
            if (!source.SetReference(access, instance, value.parent, parent)) {
                return false;
            }

            value.overridesLayerStack = true;
            auto index = source.AddElement(access, instance, value.layerStack.layers);
            return index && source
                                .SetElementReference(
                                    access, instance, value.layerStack.layers, index.Value(),
                                    value.layerStack.layers[index.Value()].layer, function)
                                .IsOk();
        }));
    REQUIRE(asset_test::WithBorrow<MaterialFunctionInstance>(
        source, functionInstance, [&](MaterialFunctionInstance& value) noexcept {
            if (!source.SetReference(access, functionInstance, value.parent, function)) {
                return false;
            }

            auto index = source.AddElement(access, functionInstance, value.textureOverrides);
            return index && source
                                .SetElementReference(
                                    access, functionInstance, value.textureOverrides, index.Value(),
                                    value.textureOverrides[index.Value()].value, texture)
                                .IsOk();
        }));

    const Array<uint8> instanceFile = asset_test::Write(
        env.allocator, source, instancePackage.Handle(), asset_test::CookedOptions());
    const Array<uint8> functionInstanceFile = asset_test::Write(
        env.allocator, source, functionInstancePackage.Handle(), asset_test::CookedOptions());
    REQUIRE(!instanceFile.IsEmpty() && !functionInstanceFile.IsEmpty());
    CHECK(CookedHasHard(env, instanceFile, asset_test::FixedId(0x691)));
    CHECK(!CookedMentions(env, instanceFile, asset_test::FixedId(0x690)));
    CHECK(!CookedMentions(env, functionInstanceFile, asset_test::FixedId(0x690)));
    CHECK(!CookedMentions(env, functionInstanceFile, asset_test::FixedId(0x694)));
}

// Build hook의 패키지 판정(4.6.1절): 다른 패키지의 하위 객체(Outer 2단)는 그 패키지 id로, 자기
// 패키지 안 객체와 null 참조는 건너뛰고, 패키지 없는 객체는 InvalidArgument로 실패한다.
TEST(Material_BuildHookResolvesOutermostPackage) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterMaterialTypes(env.modules));
    ObjectRegistry& source = *env.source;
    const ObjectAccessContext access = source.OwnerAccess();
    RootRef<Package> functionPackage;
    const ObjectHandle function = MakeAsset<MaterialFunction>(env, 0x6A0, functionPackage);
    RootRef<Package> instancePackage;
    const ObjectHandle instance = MakeAsset<MaterialInstance>(env, 0x6A1, instancePackage);
    REQUIRE(!function.IsNull() && !instance.IsNull());
    const ObjectHandle remoteInner =
        asset_test::Make<MaterialFunction>(source, TypeOf<MaterialFunction>(), function, "Inner");
    const ObjectHandle localInner =
        asset_test::Make<MaterialFunction>(source, TypeOf<MaterialFunction>(), instance, "Local");
    REQUIRE(!remoteInner.IsNull() && !localInner.IsNull());

    REQUIRE(asset_test::WithBorrow<MaterialInstance>(
        source, instance, [&](MaterialInstance& value) noexcept {
            value.overridesLayerStack = true;
            const ObjectHandle targets[] = {remoteInner, localInner};
            for (const ObjectHandle target : targets) {
                auto index = source.AddElement(access, instance, value.layerStack.layers);
                if (!index || !source
                                   .SetElementReference(
                                       access, instance, value.layerStack.layers, index.Value(),
                                       value.layerStack.layers[index.Value()].layer, target)
                                   .IsOk()) {
                    return false;
                }
            }

            return true;
        }));

    AssetTypeHookTable table(env.allocator, env.types);
    Array<AssetTypeHookRegistration> registrations(env.allocator);
    REQUIRE(RegisterMaterialAssetHooks(table, registrations));

    // 부모 null·혼합 함수 null·자기 패키지 레이어는 건너뛰고 다른 패키지 하위 객체만 남는다.
    AssetDependencySink instanceSink(env.allocator);
    REQUIRE(table.CollectBuildDependencies(source, access, instance, TypeOf<MaterialInstance>(),
                                           asset_test::FixedId(0x6A1), instanceSink));
    REQUIRE(instanceSink.Dependencies().Size() == 1);
    CHECK(instanceSink.Dependencies()[0].asset == asset_test::FixedId(0x6A0));
    CHECK(instanceSink.Dependencies()[0].kind == PackageDependencyKind::Build);

    // 복제처럼 저장 id가 원본과 달라도 자기 패키지 판정은 객체 소속으로 한다.
    AssetDependencySink copySink(env.allocator);
    REQUIRE(table.CollectBuildDependencies(source, access, instance, TypeOf<MaterialInstance>(),
                                           asset_test::FixedId(0x6AF), copySink));
    REQUIRE(copySink.Dependencies().Size() == 1);
    CHECK(copySink.Dependencies()[0].asset == asset_test::FixedId(0x6A0));

    // 패키지 정책 Registry는 패키지 없는 객체를 만들지 못한다(ObjectIdentity.h RegistryPolicy).
    // 정책 없는 Registry에서 패키지 없는 대상을 가리키면 hook은 건너뛰지 않고 InvalidArgument로
    // 실패한다.
    RegistryOptions looseOptions;
    looseOptions.names = &env.names;
    auto looseRegistry = UniquePtr<ObjectRegistry>::TryMake(
        env.allocator, env.allocator, env.types, ContextId{0x4d41544c4f4f5345ull, 3}, looseOptions);
    REQUIRE(looseRegistry);
    ObjectRegistry& loose = *looseRegistry.Value();
    const ObjectAccessContext looseAccess = loose.OwnerAccess();
    auto looseFunction = loose.NewObject<MaterialFunction>(looseAccess, TypeOf<MaterialFunction>(),
                                                           ObjectHandle{}, "LooseFunction");
    auto looseInstance = loose.NewObject<MaterialFunctionInstance>(
        looseAccess, TypeOf<MaterialFunctionInstance>(), ObjectHandle{}, "LooseInstance");
    REQUIRE(looseFunction && looseInstance);
    const ObjectHandle looseInstanceHandle = looseInstance.Value().Handle();
    const ObjectHandle looseFunctionHandle = looseFunction.Value().Handle();
    REQUIRE(asset_test::WithBorrow<MaterialFunctionInstance>(
        loose, looseInstanceHandle, [&](MaterialFunctionInstance& value) noexcept {
            return loose
                .SetReference(looseAccess, looseInstanceHandle, value.parent, looseFunctionHandle)
                .IsOk();
        }));

    AssetDependencySink looseSink(env.allocator);
    CHECK(table
              .CollectBuildDependencies(loose, looseAccess, looseInstanceHandle,
                                        TypeOf<MaterialFunctionInstance>(),
                                        asset_test::FixedId(0x6A2), looseSink)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(looseSink.Dependencies().IsEmpty());
    registrations.Clear();
}
#endif

// 선행 TODO 필드 검사(MD12): 기본값이면 비어 있고, 필드 하나만 바꾸면 그 (선행, 필드) 하나만
// 나온다.
TEST(Material_CollectsUsedPrerequisitesPerField) {
    SystemAllocator allocator;
    {
        Material material(allocator);
        Array<MaterialPrerequisiteUse> uses(allocator);
        REQUIRE(CollectUsedPrerequisites(material, uses));
        CHECK(uses.IsEmpty());
    }

    struct MaterialCase {
        void (*mutate)(Material&) noexcept;
        MaterialPrerequisite prerequisite;
        StringView field;
    };

    const MaterialCase materialCases[] = {
        {[](Material& m) noexcept { m.traits.blendMode = BlendMode::AlphaHoldout; },
         MaterialPrerequisite::AlphaHoldout, "traits.blendMode"},
        {[](Material& m) noexcept {
             m.traits.blendMode = BlendMode::TranslucentColoredTransmittance;
         },
         MaterialPrerequisite::ColoredTransmittance, "traits.blendMode"},
        {[](Material& m) noexcept { m.traits.displacement.enableFade = true; },
         MaterialPrerequisite::VirtualizedGeometry, "traits.displacement"},
        {[](Material& m) noexcept { m.traits.relaxVirtualTextureRestrictions = true; },
         MaterialPrerequisite::VirtualTexture, "traits.relaxVirtualTextureRestrictions"},
        {[](Material& m) noexcept { m.physicalMaterial.assetId = asset_test::FixedId(1); },
         MaterialPrerequisite::PhysicalMaterial, "physicalMaterial"},
        {[](Material& m) noexcept { m.physicalMaterialMask.assetId = asset_test::FixedId(1); },
         MaterialPrerequisite::PhysicalMaterial, "physicalMaterialMask"},
        {[](Material& m) noexcept {
             (void)m.renderTracePhysicalMaterials.TryPushBack(material::MaterialAssetReference{});
         },
         MaterialPrerequisite::PhysicalMaterial, "renderTracePhysicalMaterials"},
        {[](Material& m) noexcept { m.physicalMaterialMap[7].assetId = asset_test::FixedId(1); },
         MaterialPrerequisite::PhysicalMaterial, "physicalMaterialMap"},
        {[](Material& m) noexcept {
             m.virtualizedGeometryOverride.assetId = asset_test::FixedId(1);
         },
         MaterialPrerequisite::VirtualizedGeometry, "virtualizedGeometryOverride"},
        {[](Material& m) noexcept {
             (void)m.subsurfaceProfiles.TryPushBack(material::MaterialAssetReference{});
         },
         MaterialPrerequisite::SubsurfaceProfile, "subsurfaceProfiles"},
        {[](Material& m) noexcept {
             (void)m.specularProfiles.TryPushBack(material::MaterialAssetReference{});
         },
         MaterialPrerequisite::SpecularProfile, "specularProfiles"},
        {[](Material& m) noexcept {
             (void)m.toonProfiles.TryPushBack(material::MaterialAssetReference{});
         },
         MaterialPrerequisite::ToonShading, "toonProfiles"},
        {[](Material& m) noexcept { m.neuralProfile.assetId = asset_test::FixedId(1); },
         MaterialPrerequisite::NeuralProfile, "neuralProfile"},
        {[](Material& m) noexcept { m.lightBakeSettings.diffuseBoost = 2.0f; },
         MaterialPrerequisite::LightBake, "lightBakeSettings"},
    };

    for (const MaterialCase& testCase : materialCases) {
        Material material(allocator);
        testCase.mutate(material);
        Array<MaterialPrerequisiteUse> uses(allocator);
        REQUIRE(CollectUsedPrerequisites(material, uses));
        CHECK(ExactlyOne(uses, testCase.prerequisite, testCase.field));
    }

    {
        MaterialInstance instance(allocator);
        Array<MaterialPrerequisiteUse> uses(allocator);
        REQUIRE(CollectUsedPrerequisites(instance, uses));
        CHECK(uses.IsEmpty());
    }

    struct InstanceCase {
        void (*mutate)(MaterialInstance&, Allocator&) noexcept;
        MaterialPrerequisite prerequisite;
        StringView field;
    };

    const InstanceCase instanceCases[] = {
        {[](MaterialInstance& i, Allocator& a) noexcept {
             (void)i.textureCollectionOverrides.TryPushBack(material::AssetParameterOverride(a));
         },
         MaterialPrerequisite::TextureCollection, "textureCollectionOverrides"},
        {[](MaterialInstance& i, Allocator& a) noexcept {
             (void)i.virtualTextureOverrides.TryPushBack(material::AssetParameterOverride(a));
         },
         MaterialPrerequisite::VirtualTexture, "virtualTextureOverrides"},
        {[](MaterialInstance& i, Allocator& a) noexcept {
             (void)i.sparseVolumeTextureOverrides.TryPushBack(material::AssetParameterOverride(a));
         },
         MaterialPrerequisite::SparseVolumeTexture, "sparseVolumeTextureOverrides"},
        {[](MaterialInstance& i, Allocator& a) noexcept {
             (void)i.fontOverrides.TryPushBack(material::FontParameterOverride(a));
         },
         MaterialPrerequisite::Font, "fontOverrides"},
        {[](MaterialInstance& i, Allocator&) noexcept {
             i.physicalMaterialOverride = material::MaterialAssetReference{};
         },
         MaterialPrerequisite::PhysicalMaterial, "physicalMaterialOverride"},
        {[](MaterialInstance& i, Allocator&) noexcept {
             i.physicalMaterialMask.assetId = asset_test::FixedId(1);
         },
         MaterialPrerequisite::PhysicalMaterial, "physicalMaterialMask"},
        {[](MaterialInstance& i, Allocator&) noexcept {
             i.physicalMaterialMap[0].assetId = asset_test::FixedId(1);
         },
         MaterialPrerequisite::PhysicalMaterial, "physicalMaterialMap"},
        {[](MaterialInstance& i, Allocator&) noexcept {
             i.virtualizedGeometryOverride = material::MaterialAssetReference{};
         },
         MaterialPrerequisite::VirtualizedGeometry, "virtualizedGeometryOverride"},
        {[](MaterialInstance& i, Allocator&) noexcept {
             i.traitOverrides.displacementCenter = 0.1f;
         },
         MaterialPrerequisite::VirtualizedGeometry, "traitOverrides.displacement"},
        {[](MaterialInstance& i, Allocator&) noexcept {
             i.subsurfaceProfileOverride = material::MaterialAssetReference{};
         },
         MaterialPrerequisite::SubsurfaceProfile, "subsurfaceProfileOverride"},
        {[](MaterialInstance& i, Allocator&) noexcept {
             i.specularProfileOverride = material::MaterialAssetReference{};
         },
         MaterialPrerequisite::SpecularProfile, "specularProfileOverride"},
        {[](MaterialInstance& i, Allocator&) noexcept {
             i.toonProfileOverride = material::MaterialAssetReference{};
         },
         MaterialPrerequisite::ToonShading, "toonProfileOverride"},
        {[](MaterialInstance& i, Allocator&) noexcept {
             i.lightBakeOverrides.emissiveBoost = 3.0f;
         },
         MaterialPrerequisite::LightBake, "lightBakeOverrides"},
        {[](MaterialInstance& i, Allocator&) noexcept {
             i.traitOverrides.blendMode = BlendMode::AlphaHoldout;
         },
         MaterialPrerequisite::AlphaHoldout, "traitOverrides.blendMode"},
    };

    for (const InstanceCase& testCase : instanceCases) {
        MaterialInstance instance(allocator);
        testCase.mutate(instance, allocator);
        Array<MaterialPrerequisiteUse> uses(allocator);
        REQUIRE(CollectUsedPrerequisites(instance, uses));
        CHECK(ExactlyOne(uses, testCase.prerequisite, testCase.field));
    }

    // 상태가 TODO가 아닌 override(블렌드 모드 Translucent)는 쓰여도 진단이 없다.
    {
        MaterialInstance instance(allocator);
        instance.traitOverrides.blendMode = BlendMode::Translucent;
        Array<MaterialPrerequisiteUse> uses(allocator);
        REQUIRE(CollectUsedPrerequisites(instance, uses));
        CHECK(uses.IsEmpty());
    }

    {
        MaterialFunctionInstance instance(allocator);
        Array<MaterialPrerequisiteUse> uses(allocator);
        REQUIRE(CollectUsedPrerequisites(instance, uses));
        CHECK(uses.IsEmpty());
    }

#if ROCKS_WITH_EDITOR_DATA
    struct FunctionInstanceCase {
        void (*mutate)(MaterialFunctionInstance&, Allocator&) noexcept;
        MaterialPrerequisite prerequisite;
        StringView field;
    };

    const FunctionInstanceCase functionCases[] = {
        {[](MaterialFunctionInstance& i, Allocator& a) noexcept {
             (void)i.textureCollectionOverrides.TryPushBack(material::AssetParameterOverride(a));
         },
         MaterialPrerequisite::TextureCollection, "textureCollectionOverrides"},
        {[](MaterialFunctionInstance& i, Allocator& a) noexcept {
             (void)i.virtualTextureOverrides.TryPushBack(material::AssetParameterOverride(a));
         },
         MaterialPrerequisite::VirtualTexture, "virtualTextureOverrides"},
        {[](MaterialFunctionInstance& i, Allocator& a) noexcept {
             (void)i.sparseVolumeTextureOverrides.TryPushBack(material::AssetParameterOverride(a));
         },
         MaterialPrerequisite::SparseVolumeTexture, "sparseVolumeTextureOverrides"},
        {[](MaterialFunctionInstance& i, Allocator& a) noexcept {
             (void)i.fontOverrides.TryPushBack(material::FontParameterOverride(a));
         },
         MaterialPrerequisite::Font, "fontOverrides"},
    };

    for (const FunctionInstanceCase& testCase : functionCases) {
        MaterialFunctionInstance instance(allocator);
        testCase.mutate(instance, allocator);
        Array<MaterialPrerequisiteUse> uses(allocator);
        REQUIRE(CollectUsedPrerequisites(instance, uses));
        CHECK(ExactlyOne(uses, testCase.prerequisite, testCase.field));
    }

    // 상태가 TODO가 아닌 override(컬렉션·static enum)는 진단이 없다.
    {
        MaterialFunctionInstance instance(allocator);
        REQUIRE(
            instance.collectionOverrides.TryPushBack(material::AssetParameterOverride(allocator)));
        REQUIRE(instance.staticEnumOverrides.TryPushBack(material::StaticEnumOverride(allocator)));
        Array<MaterialPrerequisiteUse> uses(allocator);
        REQUIRE(CollectUsedPrerequisites(instance, uses));
        CHECK(uses.IsEmpty());
    }
#endif

    // 물리 재질 표는 마지막 칸 하나만 채워도 잡힌다(인스턴스).
    {
        MaterialInstance instance(allocator);
        instance.physicalMaterialMap[MaterialPhysicalMaterialMapSize - 1].assetId =
            asset_test::FixedId(1);
        Array<MaterialPrerequisiteUse> uses(allocator);
        REQUIRE(CollectUsedPrerequisites(instance, uses));
        CHECK(ExactlyOne(uses, MaterialPrerequisite::PhysicalMaterial, "physicalMaterialMap"));
    }
}

} // namespace material_asset_tests_file
