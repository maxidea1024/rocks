#include <AssetRuntime/Tests/AssetTestSupport.h>
#include <Base/Compiler.h>
#include <Core/Memory/SystemAllocator.h>
#include <Material/Material.generated.h>
#include <Material/Material.h>
#include <Material/MaterialBase.generated.h>
#include <Material/MaterialFunction.generated.h>
#include <Material/MaterialFunction.h>
#include <Material/MaterialFunctionInstance.generated.h>
#include <Material/MaterialFunctionInstance.h>
#include <Material/MaterialInstance.generated.h>
#include <Material/MaterialInstance.h>
#include <Material/MaterialModule.h>
#include <Material/MaterialPrerequisiteCheck.h>
#include <Material/ParameterCollection.generated.h>
#include <Material/ParameterCollection.h>
#include <TestSupport/Test.h>

// 머티리얼 에셋 클래스 시험이다(Docs/Design/MaterialSystem 4.2·4.6·19.9–19.11절, 단계 MS3b): 클래스
// 등록과 상속, traits 기본값이 UE와 같음, 필드 저장·로드 왕복(그래프·traits·사용 대상·override),
// TODO 필드 검사.
using namespace rocks;
namespace material_asset_tests_file {

namespace {

template <class T>
TypeId TypeOf() noexcept {
    return GeneratedType<T>::Id();
}

bool IsA(TypeRegistry& types, TypeId actual, TypeId expected) noexcept {
    auto result = types.IsA(actual, expected);
    return result && result.Value();
}

bool HasUse(const Array<MaterialPrerequisiteUse>& uses, material::MaterialPrerequisite prerequisite,
            StringView field) noexcept {
    for (const MaterialPrerequisiteUse& use : uses) {
        if (use.prerequisite == prerequisite && use.field == field) {
            return true;
        }
    }

    return false;
}

// 한 패키지를 저장해 다른 Registry에 로드하고 대표 에셋 handle을 돌려준다. loadedPackage는 로드한
// 패키지 handle이다(재저장에 쓴다).
ObjectHandle SaveAndLoad(asset_test::AssetEnv& env, ObjectHandle package, RootRef<Object>& root,
                         Array<uint8>& first, ObjectHandle& loadedPackage) noexcept {
    first = asset_test::Write(env.allocator, *env.source, package, asset_test::EditorOptions());
    if (first.IsEmpty()) {
        return ObjectHandle{};
    }

    Array<uint8> files[1] = {asset_test::Copy(env.allocator, first.View())};
    RootRef<Object> roots[1];
    ObjectHandle handles[1];
    if (!asset_test::LoadAll(env, *env.target, files, PackageKind::Editor, nullptr, roots,
                             handles)) {
        return ObjectHandle{};
    }

    root = Move(roots[0]);
    loadedPackage = handles[0];
    auto loaded = env.target->FindObject(env.target->OwnerAccess(), handles[0], StringView());
    return loaded ? loaded.Value() : ObjectHandle{};
}

} // namespace

// 등록: 여섯 클래스가 등록되고 상속 관계가 4.2절과 같다.
TEST(Material_RegistersAllAssetClasses) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterMaterialTypes(env.modules));
    CHECK(IsA(env.types, TypeOf<MaterialInstance>(), TypeOf<MaterialBase>()));
    CHECK(IsA(env.types, TypeOf<Material>(), TypeOf<MaterialBase>()));
    CHECK(IsA(env.types, TypeOf<MaterialFunction>(), GeneratedType<Asset>::Id()));
    CHECK(IsA(env.types, TypeOf<MaterialFunctionInstance>(), GeneratedType<Asset>::Id()));
    CHECK(IsA(env.types, TypeOf<ParameterCollection>(), GeneratedType<Asset>::Id()));
    CHECK(!IsA(env.types, TypeOf<MaterialFunction>(), TypeOf<MaterialBase>()));
    CHECK(!IsA(env.types, TypeOf<ParameterCollection>(), TypeOf<MaterialBase>()));
}

// traits 기본값이 UE UMaterial 생성자(RE/Private/Materials/Material.cpp:1137–)와 같다.
TEST(Material_DefaultTraitsMatchUnreal) {
    const material::MaterialTraitsDesc traits;
    CHECK(traits.domain == MaterialDomain::Surface);
    CHECK(traits.blendMode == BlendMode::Opaque);
    CHECK(traits.opacityMaskClipValue == 0.3333f);
    CHECK(traits.tangentSpaceNormal);
    CHECK(traits.lightmapDirectionality);
    CHECK(traits.castRayTracedShadows);
    CHECK(traits.translucencyVertexFog);
    CHECK(traits.frontLayerTranslucency);
    CHECK(traits.roughnessTracking);
    CHECK(traits.allowVariableRateShading);
    CHECK(traits.decalResponse == material::DecalResponse::ColorNormalRoughness);
    CHECK(traits.refractionMethod == material::RefractionMethod::None);
    CHECK(traits.refractionCoverageMode == material::RefractionCoverageMode::CoverageAccountedFor);
    CHECK(traits.pixelDepthOffsetMode == material::PixelDepthOffsetMode::AlongCameraVector);
    CHECK(traits.translucency.pass == material::TranslucencyPass::AfterDepthOfField);
    CHECK(traits.translucency.lightingMode ==
          material::TranslucencyLightingMode::VolumetricNonDirectional);
    CHECK(traits.translucency.shadowDensityScale == 0.5f);
    CHECK(traits.translucency.selfShadowSecondDensityScale == 10.0f);
    CHECK(traits.translucency.backscatteringExponent == 30.0f);
    CHECK(traits.translucency.shadowStartOffset == 100.0f);
    CHECK(traits.postProcess.location == material::BlendableLocation::SceneColorAfterTonemapping);
    CHECK(traits.postProcess.blendable);
    CHECK(traits.displacement.magnitude == 4.0f);
    CHECK(traits.displacement.center == 0.5f);
    CHECK(traits.displacement.fadeStartSizePixels == 4.0f);
    CHECK(traits.displacement.fadeEndSizePixels == 1.0f);
}

// Material 필드 왕복: 그래프(EditorOnly)·traits·사용 대상이 저장·로드 뒤에도 같고, 다시 쓴 byte가
// 처음과 같다(그래프 저장/재열기 왕복 — 15.2절 MS1a 기준의 이월분).
TEST(Material_FieldsRoundTrip) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterMaterialTypes(env.modules));
    ObjectRegistry& source = *env.source;
    auto package =
        NewPackage(source, source.OwnerAccess(), asset_test::FixedId(0x610), PackageKind::Editor);
    REQUIRE(package);
    const ObjectHandle handle =
        asset_test::Make<Material>(source, TypeOf<Material>(), package.Value().Handle(), "");
    REQUIRE(!handle.IsNull());

    NamePool& names = *source.Names();
    const Name staticMesh = names.TryIntern(material::StaticMeshVertexInputName).Value();
    const Name domain = names.TryIntern("Material").Value();
    const Name outputType = names.TryIntern("Material.Output").Value();
    REQUIRE(asset_test::WithBorrow<Material>(source, handle, [&](Material& material) noexcept {
        material.traits.blendMode = BlendMode::Masked;
        material.traits.opacityMaskClipValue = 0.5f;
        material.traits.twoSided = true;
        material.traits.translucency.pass = material::TranslucencyPass::AfterMotionBlur;
        material.traits.postProcess.priority = 7;
        if (!material.usage.vertexInputs.TryPushBack(staticMesh)) {
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

    RootRef<Object> root;
    Array<uint8> first(env.allocator);
    ObjectHandle loadedPackage;
    const ObjectHandle loaded =
        SaveAndLoad(env, package.Value().Handle(), root, first, loadedPackage);
    REQUIRE(!loaded.IsNull());
    Material* copy = asset_test::Pointer<Material>(*env.target, loaded);
    REQUIRE(copy != nullptr);
    CHECK(copy->traits.blendMode == BlendMode::Masked);
    CHECK(copy->traits.opacityMaskClipValue == 0.5f);
    CHECK(copy->traits.twoSided);
    CHECK(copy->traits.translucency.pass == material::TranslucencyPass::AfterMotionBlur);
    CHECK(copy->traits.postProcess.priority == 7);
    REQUIRE(copy->usage.vertexInputs.Size() == 1);
    CHECK(copy->usage.vertexInputs[0] == env.target->Names()->TryIntern("StaticMesh").Value());
#if ROCKS_WITH_EDITOR_DATA
    REQUIRE(copy->graph.graphs.Size() == 1);
    REQUIRE(copy->graph.graphs[0].nodes.Size() == 1);
    CHECK(copy->graph.graphs[0].nodes[0].id == (Guid{0x20, 2}));
    CHECK(copy->preview.cameraPitch == 30);
#endif

    auto contents =
        ReadPackage(env.allocator, first.View(), asset_test::LoadOptions(PackageKind::Editor).read);
    REQUIRE(contents);
    const Array<uint8> resaved =
        asset_test::ResaveAsRead(env.allocator, *env.target, loadedPackage, contents.Value());
    CHECK(asset_test::SameBytes(first.View(), resaved.View()));
}

// MaterialInstance 필드 왕복: 파라미터 override(키 포함)와 기본 속성 override가 남는다.
TEST(Material_InstanceOverridesRoundTrip) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterMaterialTypes(env.modules));
    ObjectRegistry& source = *env.source;
    auto package =
        NewPackage(source, source.OwnerAccess(), asset_test::FixedId(0x611), PackageKind::Editor);
    REQUIRE(package);
    const ObjectHandle handle = asset_test::Make<MaterialInstance>(
        source, TypeOf<MaterialInstance>(), package.Value().Handle(), "");
    REQUIRE(!handle.IsNull());

    REQUIRE(asset_test::WithBorrow<MaterialInstance>(
        source, handle, [&](MaterialInstance& instance) noexcept {
            material::ScalarParameterOverride roughness(source.ObjectAllocator());
            roughness.key.ownerAssetId = asset_test::FixedId(0x700);
            roughness.key.parameterId = Guid{0x30, 3};
            roughness.key.scope.kind = material::ParameterScopeKind::FunctionCall;
            if (!roughness.key.scope.callPath.TryPushBack(Guid{0x40, 4})) {
                return false;
            }

            roughness.value = 0.25f;
            material::StaticSwitchOverride useDetail(source.ObjectAllocator());
            useDetail.key.parameterId = Guid{0x31, 3};
            useDetail.value = true;
            instance.traitOverrides.blendMode = BlendMode::Translucent;
            instance.traitOverrides.twoSided = false;
            return instance.scalarOverrides.TryPushBack(Move(roughness)) &&
                   instance.staticSwitchOverrides.TryPushBack(Move(useDetail));
        }));

    RootRef<Object> root;
    Array<uint8> first(env.allocator);
    ObjectHandle loadedPackage;
    const ObjectHandle loaded =
        SaveAndLoad(env, package.Value().Handle(), root, first, loadedPackage);
    REQUIRE(!loaded.IsNull());
    MaterialInstance* copy = asset_test::Pointer<MaterialInstance>(*env.target, loaded);
    REQUIRE(copy != nullptr);
    REQUIRE(copy->scalarOverrides.Size() == 1);
    const material::ScalarParameterOverride& roughness = copy->scalarOverrides[0];
    CHECK(roughness.key.ownerAssetId == asset_test::FixedId(0x700));
    CHECK(roughness.key.parameterId == (Guid{0x30, 3}));
    CHECK(roughness.key.scope.kind == material::ParameterScopeKind::FunctionCall);
    REQUIRE(roughness.key.scope.callPath.Size() == 1);
    CHECK(roughness.key.scope.callPath[0] == (Guid{0x40, 4}));
    CHECK(roughness.value == 0.25f);
    REQUIRE(copy->staticSwitchOverrides.Size() == 1);
    CHECK(copy->staticSwitchOverrides[0].value);
    REQUIRE(copy->traitOverrides.blendMode.HasValue());
    CHECK(copy->traitOverrides.blendMode.Value() == BlendMode::Translucent);
    REQUIRE(copy->traitOverrides.twoSided.HasValue());
    CHECK(!copy->traitOverrides.twoSided.Value());
    CHECK(!copy->traitOverrides.opacityMaskClipValue.HasValue());
}

// TODO 필드 검사(MD12): 기본값이면 비어 있고, 값을 넣은 필드마다 그 선행 ID가 나온다.
TEST(Material_CollectsUsedPrerequisites) {
    SystemAllocator allocator;
    {
        Material material(allocator);
        Array<MaterialPrerequisiteUse> uses(allocator);
        REQUIRE(CollectUsedPrerequisites(material, uses));
        CHECK(uses.IsEmpty());

        material.traits.blendMode = BlendMode::AlphaHoldout;
        material.traits.displacement.magnitude = 2.0f;
        material.traits.relaxVirtualTextureRestrictions = true;
        material.physicalMaterial.assetId = asset_test::FixedId(0x800);
        material.neuralProfile.assetId = asset_test::FixedId(0x801);
        REQUIRE(CollectUsedPrerequisites(material, uses));
        CHECK(HasUse(uses, material::MaterialPrerequisite::AlphaHoldout, "traits.blendMode"));
        CHECK(HasUse(uses, material::MaterialPrerequisite::VirtualizedGeometry,
                     "traits.displacement"));
        CHECK(HasUse(uses, material::MaterialPrerequisite::VirtualTexture,
                     "traits.relaxVirtualTextureRestrictions"));
        CHECK(HasUse(uses, material::MaterialPrerequisite::PhysicalMaterial, "physicalMaterial"));
        CHECK(HasUse(uses, material::MaterialPrerequisite::NeuralProfile, "neuralProfile"));
        CHECK(uses.Size() == 5);
    }

    {
        MaterialInstance instance(allocator);
        Array<MaterialPrerequisiteUse> uses(allocator);
        REQUIRE(CollectUsedPrerequisites(instance, uses));
        CHECK(uses.IsEmpty());

        material::AssetParameterOverride font(allocator);
        REQUIRE(instance.fontOverrides.TryPushBack(Move(font)));
        instance.traitOverrides.blendMode = BlendMode::TranslucentColoredTransmittance;
        instance.traitOverrides.enableTessellation = true;
        instance.toonProfileOverride.assetId = asset_test::FixedId(0x802);
        REQUIRE(CollectUsedPrerequisites(instance, uses));
        CHECK(HasUse(uses, material::MaterialPrerequisite::Font, "fontOverrides"));
        CHECK(HasUse(uses, material::MaterialPrerequisite::ColoredTransmittance,
                     "traitOverrides.blendMode"));
        CHECK(HasUse(uses, material::MaterialPrerequisite::VirtualizedGeometry,
                     "traitOverrides.displacement"));
        CHECK(HasUse(uses, material::MaterialPrerequisite::ToonShading, "toonProfileOverride"));
        CHECK(uses.Size() == 4);
    }
}

} // namespace material_asset_tests_file
