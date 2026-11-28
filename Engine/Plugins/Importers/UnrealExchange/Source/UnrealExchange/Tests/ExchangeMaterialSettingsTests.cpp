#include "ExchangeMeshTestSupport.h"

#include <Base/InitializerList.h>
#include <Material/MaterialModule.h>
#include <MaterialSchema/MaterialTraitsDesc.generated.h>
#include <Object/ModuleRegistration.h>
#include <Object/TypeRegistry.h>
#include <ReflectionText/TextJson.h>
#include <TestSupport/Test.h>
#include <UnrealExchange/ExchangeMaterialSettings.h>

// 머티리얼 설정 번역 시험이다(Docs/Design/UnrealExchange 8.7절 7번 "구현에서 정함" — UX4b1a): 표
// 모양(행 수·키 겹침·묶음·EnumMap 멤버), golden 머티리얼 다섯과 엔진 머티리얼(경고 없음, M_Golden
// traits = 기본값, usage·preview), 값 대응(묶음 하위 칸·Flatten·enum·Name·색), 사용 대상(키 없음 =
// UE 기본값, 스킨·morph 플래그), enum 대응 값(표와 따로 적은 기대값), enum 표의 값 전부(EnumMap
// 멤버와 접두만 다른 enum의 UE 5.8 값 이름), 경고(TODO 칸·사용 플래그·모르는 키·Flatten
// 멤버·Rocks에 없는 값·ColoredTransmittance 두 프로젝트 설정), 거부(값 종류·ImportJson·사용
// 플래그·미리보기·호출 계약). 변형 입력은 golden 문서를 경로 편집으로 다시 쓴 사본이다.
using namespace rocks;
using namespace rocks::exchange_test;
namespace exchange_material_settings_tests_file {

namespace {

constexpr StringView GoldenMaterialPath = "/Game/RxGolden/M_Golden.M_Golden";
constexpr StringView CubePath = "/Engine/BasicShapes/Cube.Cube";
constexpr StringView LightFunctionPath =
    "/Engine/EngineMaterials/DefaultLightFunctionMaterial.DefaultLightFunctionMaterial";
// 묶음 이름 수다(translucency·forward·postProcess).
constexpr usize MaterialGroupCount = 3;

// 접두만 다른 enum 행(Enum)과 그 UE 5.8 값 이름 전부다(EngineTypes.h·Material.h).
struct EnumCase {
    StringView key;
    std::initializer_list<StringView> values;
};

const EnumCase PrefixEnums[] = {
    {"MaterialDecalResponse",
     {"MDR_None", "MDR_ColorNormalRoughness", "MDR_Color", "MDR_ColorNormal", "MDR_ColorRoughness",
      "MDR_Normal", "MDR_NormalRoughness", "MDR_Roughness"}},
    {"TranslucencyLightingMode",
     {"TLM_VolumetricNonDirectional", "TLM_VolumetricDirectional",
      "TLM_VolumetricPerVertexNonDirectional", "TLM_VolumetricPerVertexDirectional", "TLM_Surface",
      "TLM_SurfacePerPixelLighting"}},
    {"RefractionCoverageMode", {"RCM_CoverageIgnored", "RCM_CoverageAccountedFor"}},
    {"StencilCompare",
     {"MSC_Less", "MSC_LessEqual", "MSC_Greater", "MSC_GreaterEqual", "MSC_Equal", "MSC_NotEqual",
      "MSC_Never", "MSC_Always"}},
};

// Material 타입(의존 모듈 MaterialSchema 포함)과 Name pool을 가진 Registry 환경이다.
struct MaterialEnv {
    explicit MaterialEnv(Allocator& allocator) noexcept
        : types(allocator), modules(types, allocator), names(allocator),
          objects(allocator, types, ContextId{0x4d54ull, 1}, Options(names)),
          access(objects.OwnerAccess()) {
        registered = static_cast<bool>(RegisterMaterialTypes(modules));
    }

    ~MaterialEnv() {
        CHECK(objects.Shutdown(access));
        CHECK(names.Shutdown());
        CHECK(modules.Unregister());
        CHECK(types.Shutdown());
    }

    static RegistryOptions Options(NamePool& pool) noexcept {
        (void)pool.Initialize();
        RegistryOptions options;
        options.names = &pool;
        return options;
    }

    TypeRegistry types;
    ModuleRegistration modules;
    NamePool names;
    ObjectRegistry objects;
    ObjectAccessContext access;
    bool registered = false;
};

bool OpenFixture(Allocator& allocator, StringView fixture, ExchangeBundle& bundle) noexcept {
    StringBuilder<512> directory(allocator);
    ExchangeDiagnostics diagnostics(allocator);
    return FixtureSourcePath(fixture, directory) &&
           ReadExchangeBundle(platform::PhysicalFileSystem(), directory.View(), {}, bundle,
                              diagnostics);
}

// 표에서 UE 키의 행 번호다(없으면 행 수).
uint64 RowOf(StringView key) noexcept {
    const Span<const ExchangeSettingRow> rows = ExchangeMaterialSettingRows();
    for (usize index = 0; index < rows.Size(); ++index) {
        if (rows[index].ueKey == key) {
            return index;
        }
    }

    return rows.Size();
}

// 편집한 사본 하나의 번역 결과다.
struct Attempt {
    explicit Attempt(Allocator& allocator) noexcept
        : edited(allocator), usage(allocator), warnings(allocator) {
    }

    EditedAsset edited;
    material::MaterialTraitsDesc traits;
    material::VertexInputUsage usage;
    MaterialPreviewSettings preview;
    Array<ExchangeSettingWarning> warnings;
    ExchangeMaterialFailure failure;
    ErrorCode error = ErrorCode::None;
    bool prepared = false;
};

void Run(MaterialEnv& env, Allocator& allocator, const ExchangeAsset& base,
         Span<const JsonEdit> edits, Attempt& attempt, bool substrate = false) noexcept {
    attempt.prepared = Edit(allocator, base, {}, edits, attempt.edited);
    if (!attempt.prepared) {
        return;
    }

    ExchangeMaterialSettingsInput input;
    input.asset = &attempt.edited.asset;
    input.substrate = substrate;
    input.registry = &env.objects;
    input.access = env.access;
    auto translated = TranslateExchangeMaterialSettings(
        input, attempt.traits, attempt.usage, attempt.preview, attempt.warnings, attempt.failure);
    attempt.error = translated ? ErrorCode::None : translated.Error();
}

// traits를 리플렉션 JSON 원문으로 쓴다(필드 전부 — 두 값의 비교용).
bool ExportTraits(MaterialEnv& env, Allocator& allocator, material::MaterialTraitsDesc& traits,
                  StringBuilder<>& out) noexcept {
    auto lease =
        env.types.AcquireValueType(TypeIdOf<material::MaterialTraitsDesc>(), TypeKind::Struct);
    if (!lease) {
        return false;
    }

    out.Clear();
    JsonWriter writer(out);
    reflection_text::TextCodecOptions options;
    return reflection_text::ExportJson(
               reflection_text::AssignmentTarget::Value(env.objects, env.access, lease.Value(),
                                                        &traits, allocator),
               writer, options) &&
           writer.Finish();
}

bool HasOnlyStaticMesh(MaterialEnv& env, const material::VertexInputUsage& usage) noexcept {
    auto name = env.names.TryFind(material::StaticMeshVertexInputName);
    return name && usage.vertexInputs.Size() == 1 && usage.vertexInputs[0] == name.Value();
}

} // namespace

// 표 모양: 행 수가 문서 수와 같고 UE 키가 겹치지 않으며, 묶음 번호는 묶음 이름 수 안이고, EnumMap
// 행은 멤버가 있고 멤버 표에 묶음이 없다.
TEST(UnrealExchange_MaterialSettingRows) {
    const Span<const ExchangeSettingRow> rows = ExchangeMaterialSettingRows();
    const Span<const StringView> groups = ExchangeMaterialSettingGroups();
    CHECK(rows.Size() == ExchangeMaterialSettingRowCount);
    REQUIRE(groups.Size() == MaterialGroupCount);
    CHECK(groups[0] == "translucency" && groups[1] == "forward" && groups[2] == "postProcess");
    for (usize index = 0; index < rows.Size(); ++index) {
        CHECK(rows[index].group <= groups.Size());
        for (usize other = 0; other < index; ++other) {
            CHECK(rows[index].ueKey != rows[other].ueKey);
        }

        const bool structured = rows[index].kind == ExchangeSettingKind::EnumMap ||
                                rows[index].kind == ExchangeSettingKind::Flatten;
        CHECK(structured == (rows[index].memberCount != 0));
        for (usize member = 0; member < rows[index].memberCount; ++member) {
            CHECK(rows[index].members[member].group == 0);
        }
    }

    CHECK(RowOf("BlendMode") == static_cast<uint64>(ExchangeMaterialSettingRow::BlendMode));
    CHECK(RowOf("bUsedWithStaticMesh") ==
          static_cast<uint64>(ExchangeMaterialSettingRow::UsedWithStaticMesh));
}

// golden 머티리얼 다섯(Golden·GoldenBaked의 M_Golden, GoldenSubstrate의 세 머티리얼 — 그 묶음의
// substrate 설정으로)과 엔진 머티리얼 DefaultLightFunctionMaterial이 경고 없이 번역되고(usage는
// 덤프의 bUsedWithStaticMesh대로), UE 새 머티리얼인 M_Golden의 traits는 Rocks 기본값과 필드 전부가
// 같으며 usage는 StaticMesh, preview 카메라는 0이다.
TEST(UnrealExchange_MaterialSettingsGolden) {
    SystemAllocator allocator;
    {
        MaterialEnv env(allocator);
        REQUIRE(env.registered);
        struct Case {
            StringView fixture;
            StringView path;
        };
        const Case cases[] = {
            {"Golden.rxbundle", GoldenMaterialPath},
            {"GoldenBaked.rxbundle", GoldenMaterialPath},
            {"GoldenSubstrate.rxbundle", "/Game/RxGoldenSubstrate/M_Golden.M_Golden"},
            {"GoldenSubstrate.rxbundle",
             "/Game/RxGoldenSubstrate/M_GoldenSubstrate.M_GoldenSubstrate"},
            {"GoldenSubstrate.rxbundle",
             "/Game/RxGoldenSubstrate/M_GoldenSubstrateAttributes.M_GoldenSubstrateAttributes"},
            {"GoldenBaked.rxbundle", LightFunctionPath},
        };
        for (const Case& item : cases) {
            ExchangeBundle bundle(allocator);
            REQUIRE(OpenFixture(allocator, item.fixture, bundle));
            const uint32 asset = FindAsset(bundle, item.path);
            REQUIRE(asset != ExchangeNoIndex);
            Attempt attempt(allocator);
            Run(env, allocator, bundle.Assets()[asset], {}, attempt, bundle.substrate);
            REQUIRE(attempt.prepared);
            CHECK(attempt.error == ErrorCode::None);
            CHECK(attempt.warnings.IsEmpty());
            const JsonValue staticMesh =
                bundle.Assets()[asset].settings.Find("bUsedWithStaticMesh");
            REQUIRE(staticMesh.IsBool());
            CHECK(staticMesh.GetBool() ? HasOnlyStaticMesh(env, attempt.usage)
                                       : attempt.usage.vertexInputs.IsEmpty());
            CHECK(attempt.preview.cameraPitch == 0 && attempt.preview.cameraYaw == 0);
            if (item.path == LightFunctionPath) {
                // UE 엔진 머티리얼: LightFunction 영역·PDOM_Legacy(M_Golden과 다른 값).
                CHECK(attempt.traits.domain == MaterialDomain::LightFunction);
                CHECK(attempt.traits.pixelDepthOffsetMode ==
                      material::PixelDepthOffsetMode::ViewDepth);
            }

            if (item.fixture == "Golden.rxbundle") {
                material::MaterialTraitsDesc defaults;
                StringBuilder<> expected(allocator);
                StringBuilder<> actual(allocator);
                REQUIRE(ExportTraits(env, allocator, defaults, expected));
                REQUIRE(ExportTraits(env, allocator, attempt.traits, actual));
                CHECK(actual.View() == expected.View());
            }
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 값 대응: 최상위 칸·묶음 하위 칸(translucency·forward·postProcess)·Flatten·이름 다른 enum·Name·색,
// 사용 플래그 거짓 → 빈 usage, 미리보기 카메라.
TEST(UnrealExchange_MaterialSettingsValues) {
    SystemAllocator allocator;
    {
        MaterialEnv env(allocator);
        REQUIRE(env.registered);
        ExchangeBundle bundle(allocator);
        REQUIRE(OpenFixture(allocator, "Golden.rxbundle", bundle));
        const uint32 asset = FindAsset(bundle, GoldenMaterialPath);
        REQUIRE(asset != ExchangeNoIndex);
        const JsonEdit edits[] = {
            {"settings.MaterialDomain", R"("MD_UI")"},
            {"settings.BlendMode", R"("BLEND_Masked")"},
            {"settings.TwoSided", "true"},
            {"settings.OpacityMaskClipValue", "0.5"},
            {"settings.bIsThinSurface", "true"},
            {"settings.NumCustomizedUVs", "3"},
            {"settings.MaterialDecalResponse", R"("MDR_Color")"},
            {"settings.ShadingRate", R"("MSR_2x4")"},
            {"settings.RefractionMethod", R"("RM_2DOffset")"},
            {"settings.PixelDepthOffsetMode", R"("PDOM_Legacy")"},
            {"settings.TranslucencyPass", R"("MTP_BeforeDOF")"},
            {"settings.TranslucencyLightingMode", R"("TLM_Surface")"},
            {"settings.TranslucentShadowStartOffset", "25"},
            {"settings.TranslucentMultipleScatteringExtinction",
             R"({"R": 0.25, "G": 0.5, "B": 0.75, "A": 1})"},
            {"settings.bUseHQForwardReflections", "true"},
            {"settings.BlendableLocation", R"("BL_SSRInput")"},
            {"settings.BlendablePriority", "7"},
            {"settings.UserSceneTexture", R"("SceneA")"},
            {"settings.UserTextureDivisor", R"({"X": 2, "Y": 3})"},
            {"settings.StencilCompare", R"("MSC_Greater")"},
            {"settings.StencilRefValue", "9"},
            {"settings.bUsedWithStaticMesh", "false"},
            {"settings.EditorPitch", "10"},
            {"settings.EditorYaw", "-20"},
        };
        Attempt attempt(allocator);
        Run(env, allocator, bundle.Assets()[asset], Span<const JsonEdit>(edits), attempt);
        REQUIRE(attempt.prepared);
        REQUIRE(attempt.error == ErrorCode::None);
        CHECK(attempt.warnings.IsEmpty());
        const material::MaterialTraitsDesc& traits = attempt.traits;
        CHECK(traits.domain == MaterialDomain::UserInterface);
        CHECK(traits.blendMode == BlendMode::Masked);
        CHECK(traits.twoSided && traits.thinSurface);
        CHECK(Near(traits.opacityMaskClipValue, 0.5));
        CHECK(traits.customizedUvCount == 3);
        CHECK(traits.decalResponse == material::DecalResponse::Color);
        CHECK(traits.shadingRate == material::MaterialShadingRate::Rate2x4);
        CHECK(traits.refractionMethod == material::RefractionMethod::Offset2D);
        CHECK(traits.pixelDepthOffsetMode == material::PixelDepthOffsetMode::ViewDepth);
        CHECK(traits.translucency.pass == material::TranslucencyPass::BeforeDepthOfField);
        CHECK(traits.translucency.lightingMode == material::TranslucencyLightingMode::Surface);
        CHECK(Near(traits.translucency.shadowStartOffset, 25.0));
        CHECK(Near(traits.translucency.multipleScatteringExtinction.r, 0.25) &&
              Near(traits.translucency.multipleScatteringExtinction.g, 0.5) &&
              Near(traits.translucency.multipleScatteringExtinction.b, 0.75));
        CHECK(traits.forward.highQualityReflections);
        CHECK(traits.postProcess.location ==
              material::BlendableLocation::ScreenSpaceReflectionInput);
        CHECK(traits.postProcess.priority == 7);
        auto scene = env.names.TryFind("SceneA");
        REQUIRE(scene);
        CHECK(traits.postProcess.userSceneTexture == scene.Value());
        CHECK(traits.postProcess.resolutionRelativeToInput.IsNone());
        CHECK(traits.postProcess.userTextureDivisorX == 2 &&
              traits.postProcess.userTextureDivisorY == 3);
        CHECK(traits.postProcess.stencilCompare == material::StencilCompare::Greater);
        CHECK(traits.postProcess.stencilRef == 9);
        CHECK(attempt.usage.vertexInputs.IsEmpty());
        CHECK(attempt.preview.cameraPitch == 10 && attempt.preview.cameraYaw == -20);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 사용 대상: bUsedWithStaticMesh 키가 없으면 UE 기본값 true(StaticMesh), 스킨 메시·morph 플래그는
// 옮기지 않고 경고도 없다(쿠킹 유도가 모은다 — 8.7절 7번).
TEST(UnrealExchange_MaterialSettingsUsage) {
    SystemAllocator allocator;
    {
        MaterialEnv env(allocator);
        REQUIRE(env.registered);
        ExchangeBundle bundle(allocator);
        REQUIRE(OpenFixture(allocator, "Golden.rxbundle", bundle));
        const uint32 asset = FindAsset(bundle, GoldenMaterialPath);
        REQUIRE(asset != ExchangeNoIndex);
        {
            const JsonEdit edits[] = {{"settings.bUsedWithStaticMesh", ""}};
            Attempt attempt(allocator);
            Run(env, allocator, bundle.Assets()[asset], Span<const JsonEdit>(edits), attempt);
            REQUIRE(attempt.prepared);
            REQUIRE(attempt.error == ErrorCode::None);
            CHECK(attempt.warnings.IsEmpty());
            CHECK(HasOnlyStaticMesh(env, attempt.usage));
        }

        {
            const JsonEdit edits[] = {
                {"settings.bUsedWithSkeletalMesh", "true"},
                {"settings.bUsedWithMorphTargets", "true"},
                {"settings.bUsedWithEditorCompositing", "true"},
            };
            Attempt attempt(allocator);
            Run(env, allocator, bundle.Assets()[asset], Span<const JsonEdit>(edits), attempt);
            REQUIRE(attempt.prepared);
            REQUIRE(attempt.error == ErrorCode::None);
            CHECK(attempt.warnings.IsEmpty());
            CHECK(HasOnlyStaticMesh(env, attempt.usage));
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// enum 대응 값(표와 따로 적은 기대값 — UE 5.8 enum 정의에서 옮겼다): 이름이 다른 enum의 UE 값마다
// 결과 Rocks 열거자다(표의 뒤바뀜을 잡는다).
TEST(UnrealExchange_MaterialSettingsEnumValues) {
    SystemAllocator allocator;
    {
        MaterialEnv env(allocator);
        REQUIRE(env.registered);
        ExchangeBundle bundle(allocator);
        REQUIRE(OpenFixture(allocator, "Golden.rxbundle", bundle));
        const uint32 asset = FindAsset(bundle, GoldenMaterialPath);
        REQUIRE(asset != ExchangeNoIndex);
        using Traits = material::MaterialTraitsDesc;
        struct Case {
            StringView key;
            StringView value;
            uint32 expected;
            uint32 (*read)(const Traits&) noexcept;
        };
        constexpr auto domain = [](const Traits& traits) noexcept {
            return static_cast<uint32>(traits.domain);
        };
        constexpr auto blend = [](const Traits& traits) noexcept {
            return static_cast<uint32>(traits.blendMode);
        };
        constexpr auto pass = [](const Traits& traits) noexcept {
            return static_cast<uint32>(traits.translucency.pass);
        };
        constexpr auto rate = [](const Traits& traits) noexcept {
            return static_cast<uint32>(traits.shadingRate);
        };
        constexpr auto refraction = [](const Traits& traits) noexcept {
            return static_cast<uint32>(traits.refractionMethod);
        };
        constexpr auto depth = [](const Traits& traits) noexcept {
            return static_cast<uint32>(traits.pixelDepthOffsetMode);
        };
        constexpr auto location = [](const Traits& traits) noexcept {
            return static_cast<uint32>(traits.postProcess.location);
        };
        using material::BlendableLocation;
        using material::MaterialShadingRate;
        using material::PixelDepthOffsetMode;
        using material::RefractionMethod;
        using material::TranslucencyPass;
        const Case cases[] = {
            {"MaterialDomain", "MD_Surface", static_cast<uint32>(MaterialDomain::Surface), domain},
            {"MaterialDomain", "MD_DeferredDecal",
             static_cast<uint32>(MaterialDomain::DeferredDecal), domain},
            {"MaterialDomain", "MD_LightFunction",
             static_cast<uint32>(MaterialDomain::LightFunction), domain},
            {"MaterialDomain", "MD_Volume", static_cast<uint32>(MaterialDomain::Volume), domain},
            {"MaterialDomain", "MD_PostProcess", static_cast<uint32>(MaterialDomain::PostProcess),
             domain},
            {"MaterialDomain", "MD_UI", static_cast<uint32>(MaterialDomain::UserInterface), domain},
            {"BlendMode", "BLEND_Opaque", static_cast<uint32>(BlendMode::Opaque), blend},
            {"BlendMode", "BLEND_Masked", static_cast<uint32>(BlendMode::Masked), blend},
            {"BlendMode", "BLEND_Translucent", static_cast<uint32>(BlendMode::Translucent), blend},
            {"BlendMode", "BLEND_Additive", static_cast<uint32>(BlendMode::Additive), blend},
            {"BlendMode", "BLEND_Modulate", static_cast<uint32>(BlendMode::Modulate), blend},
            {"BlendMode", "BLEND_AlphaComposite", static_cast<uint32>(BlendMode::AlphaComposite),
             blend},
            {"BlendMode", "BLEND_TranslucentColoredTransmittance",
             static_cast<uint32>(BlendMode::Translucent), blend},
            {"BlendMode", "BLEND_TranslucentGreyTransmittance",
             static_cast<uint32>(BlendMode::Translucent), blend},
            {"BlendMode", "BLEND_ColoredTransmittanceOnly",
             static_cast<uint32>(BlendMode::Modulate), blend},
            {"TranslucencyPass", "MTP_BeforeDOF",
             static_cast<uint32>(TranslucencyPass::BeforeDepthOfField), pass},
            {"TranslucencyPass", "MTP_AfterDOF",
             static_cast<uint32>(TranslucencyPass::AfterDepthOfField), pass},
            {"TranslucencyPass", "MTP_AfterMotionBlur",
             static_cast<uint32>(TranslucencyPass::AfterMotionBlur), pass},
            {"ShadingRate", "MSR_1x1", static_cast<uint32>(MaterialShadingRate::Rate1x1), rate},
            {"ShadingRate", "MSR_2x1", static_cast<uint32>(MaterialShadingRate::Rate2x1), rate},
            {"ShadingRate", "MSR_1x2", static_cast<uint32>(MaterialShadingRate::Rate1x2), rate},
            {"ShadingRate", "MSR_2x2", static_cast<uint32>(MaterialShadingRate::Rate2x2), rate},
            {"ShadingRate", "MSR_4x2", static_cast<uint32>(MaterialShadingRate::Rate4x2), rate},
            {"ShadingRate", "MSR_2x4", static_cast<uint32>(MaterialShadingRate::Rate2x4), rate},
            {"ShadingRate", "MSR_4x4", static_cast<uint32>(MaterialShadingRate::Rate4x4), rate},
            {"RefractionMethod", "RM_IndexOfRefraction",
             static_cast<uint32>(RefractionMethod::IndexOfRefraction), refraction},
            {"RefractionMethod", "RM_PixelNormalOffset",
             static_cast<uint32>(RefractionMethod::PixelNormalOffset), refraction},
            {"RefractionMethod", "RM_2DOffset", static_cast<uint32>(RefractionMethod::Offset2D),
             refraction},
            {"RefractionMethod", "RM_None", static_cast<uint32>(RefractionMethod::None),
             refraction},
            {"RefractionMethod", "RM_IndexOfRefractionFromF0",
             static_cast<uint32>(RefractionMethod::IndexOfRefractionFromF0), refraction},
            {"PixelDepthOffsetMode", "PDOM_Legacy",
             static_cast<uint32>(PixelDepthOffsetMode::ViewDepth), depth},
            {"PixelDepthOffsetMode", "PDOM_AlongCameraVector",
             static_cast<uint32>(PixelDepthOffsetMode::AlongCameraVector), depth},
            {"BlendableLocation", "BL_SceneColorBeforeDOF",
             static_cast<uint32>(BlendableLocation::SceneColorBeforeDepthOfField), location},
            {"BlendableLocation", "BL_SceneColorAfterDOF",
             static_cast<uint32>(BlendableLocation::SceneColorAfterDepthOfField), location},
            {"BlendableLocation", "BL_TranslucencyAfterDOF",
             static_cast<uint32>(BlendableLocation::TranslucencyAfterDepthOfField), location},
            {"BlendableLocation", "BL_SSRInput",
             static_cast<uint32>(BlendableLocation::ScreenSpaceReflectionInput), location},
            {"BlendableLocation", "BL_SceneColorBeforeBloom",
             static_cast<uint32>(BlendableLocation::SceneColorBeforeBloom), location},
            {"BlendableLocation", "BL_ReplacingTonemapper",
             static_cast<uint32>(BlendableLocation::ReplacingTonemapper), location},
            {"BlendableLocation", "BL_SceneColorAfterTonemapping",
             static_cast<uint32>(BlendableLocation::SceneColorAfterTonemapping), location},
        };
        for (const Case& item : cases) {
            StringBuilder<256> path(allocator);
            StringBuilder<256> json(allocator);
            REQUIRE(path.TryAppend("settings.") && path.TryAppend(item.key));
            REQUIRE(json.TryAppend("\"") && json.TryAppend(item.value) && json.TryAppend("\""));
            const JsonEdit edits[] = {{path.View(), json.View()}};
            Attempt attempt(allocator);
            Run(env, allocator, bundle.Assets()[asset], Span<const JsonEdit>(edits), attempt);
            REQUIRE(attempt.prepared);
            REQUIRE(attempt.error == ErrorCode::None);
            CHECK(item.read(attempt.traits) == item.expected);
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// enum 표의 값 전부: EnumMap 멤버마다 대응 이름이 Rocks enum에 있고(ImportJson 통과·경고 없음),
// Rocks에 없는 값은 그 행 번호의 경고 하나다. 접두만 다른 enum은 UE 5.8 값 이름 전부가 통과한다.
TEST(UnrealExchange_MaterialSettingsEnumTables) {
    SystemAllocator allocator;
    {
        MaterialEnv env(allocator);
        REQUIRE(env.registered);
        ExchangeBundle bundle(allocator);
        REQUIRE(OpenFixture(allocator, "Golden.rxbundle", bundle));
        const uint32 asset = FindAsset(bundle, GoldenMaterialPath);
        REQUIRE(asset != ExchangeNoIndex);
        const Span<const ExchangeSettingRow> rows = ExchangeMaterialSettingRows();
        usize mapped = 0;
        auto check = [&](StringView key, StringView value, bool unmapped) {
            StringBuilder<256> path(allocator);
            StringBuilder<256> json(allocator);
            REQUIRE(path.TryAppend("settings.") && path.TryAppend(key));
            REQUIRE(json.TryAppend("\"") && json.TryAppend(value) && json.TryAppend("\""));
            const JsonEdit edits[] = {{path.View(), json.View()}};
            Attempt attempt(allocator);
            Run(env, allocator, bundle.Assets()[asset], Span<const JsonEdit>(edits), attempt);
            REQUIRE(attempt.prepared);
            CHECK(attempt.error == ErrorCode::None);
            CHECK(attempt.warnings.Size() == (unmapped ? 1u : 0u));
            if (unmapped && attempt.warnings.Size() == 1) {
                CHECK(attempt.warnings[0].detail == RowOf(key));
            }
        };

        for (const ExchangeSettingRow& row : rows) {
            if (row.kind != ExchangeSettingKind::EnumMap) {
                continue;
            }

            for (usize member = 0; member < row.memberCount; ++member) {
                check(row.ueKey, row.members[member].ueKey, row.members[member].rocksKey.IsEmpty());
                ++mapped;
            }
        }

        // MaterialDomain 7·BlendMode 10·TranslucencyPass 3·ShadingRate 7·BlendableLocation 7·
        // RefractionMethod 5·PixelDepthOffsetMode 2.
        CHECK(mapped == 41);
        for (const EnumCase& item : PrefixEnums) {
            for (const StringView value : item.values) {
                check(item.key, value, false);
            }
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 경고: 필드 + TODO 칸(테셀레이션·물리 재질·조명 굽기·미리보기 메시)과 TODO 사용 플래그의 기본값
// 아닌 값, 모르는 키, Flatten의 모르는 멤버, Rocks에 없는 값(BLEND_AlphaHoldout — 기본 Opaque가
// 남는다), ColoredTransmittance는 Substrate 프로젝트에서만 경고하고 둘 다 Translucent다.
TEST(UnrealExchange_MaterialSettingsWarnings) {
    SystemAllocator allocator;
    {
        MaterialEnv env(allocator);
        REQUIRE(env.registered);
        ExchangeBundle bundle(allocator);
        REQUIRE(OpenFixture(allocator, "Golden.rxbundle", bundle));
        const uint32 asset = FindAsset(bundle, GoldenMaterialPath);
        REQUIRE(asset != ExchangeNoIndex);
        const ExchangeAsset& base = bundle.Assets()[asset];
        {
            const JsonEdit edits[] = {
                {"settings.PhysMaterial", R"({"objectPath": "/Game/P/PM.PM"})"},
                {"settings.bEnableTessellation", "true"},
                {"settings.bUsedWithNanite", "true"},
                {"settings.LightmassSettings.EmissiveBoost", "2"},
                {"settings.BlendMode", R"("BLEND_AlphaHoldout")"},
                {"settings.UserTextureDivisor.Z", "4"},
                {"settings.PreviewMesh", R"({"objectPath": "/Game/P/SM.SM"})"},
                {"settings.bFutureFlag", "true"},
            };
            Attempt attempt(allocator);
            Run(env, allocator, base, Span<const JsonEdit>(edits), attempt);
            REQUIRE(attempt.prepared);
            REQUIRE(attempt.error == ErrorCode::None);
            // 최상위 경고(설정 순서) 뒤에 묶음 행(postProcess의 UserTextureDivisor)의 경고가 온다.
            const uint64 expected[] = {RowOf("PhysMaterial"),        RowOf("BlendMode"),
                                       RowOf("bEnableTessellation"), RowOf("bUsedWithNanite"),
                                       RowOf("LightmassSettings"),   RowOf("PreviewMesh"),
                                       ExchangeUnknownSettingDetail, RowOf("UserTextureDivisor")};
            REQUIRE(attempt.warnings.Size() == sizeof(expected) / sizeof(expected[0]));
            for (usize index = 0; index < attempt.warnings.Size(); ++index) {
                CHECK(attempt.warnings[index].code == AssetDiagnosticCode::UnsupportedSetting);
                CHECK(attempt.warnings[index].detail == expected[index]);
            }

            CHECK(attempt.traits.blendMode == BlendMode::Opaque);
        }

        for (const bool substrate : {false, true}) {
            const JsonEdit edits[] = {
                {"settings.BlendMode", R"("BLEND_TranslucentColoredTransmittance")"},
            };
            Attempt attempt(allocator);
            Run(env, allocator, base, Span<const JsonEdit>(edits), attempt, substrate);
            REQUIRE(attempt.prepared);
            REQUIRE(attempt.error == ErrorCode::None);
            CHECK(attempt.traits.blendMode == BlendMode::Translucent);
            REQUIRE(attempt.warnings.Size() == (substrate ? 1u : 0u));
            if (substrate) {
                CHECK(attempt.warnings[0].code == AssetDiagnosticCode::UnsupportedSetting &&
                      attempt.warnings[0].detail ==
                          static_cast<uint64>(ExchangeMaterialSettingRow::BlendMode));
            }
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 거부: 번역 값 종류(EnumMap 값이 문자열 아님·표에 없는 값, Flatten 값이 객체 아님, settings가
// 객체 아님), ImportJson 거부(값 형식 — 묶음 하위 칸 포함, uint8 범위 밖), 사용 플래그·미리보기 값
// 종류(그 행 번호), 호출 계약(머티리얼이 아님·registry 없음).
TEST(UnrealExchange_MaterialSettingsRejects) {
    SystemAllocator allocator;
    {
        MaterialEnv env(allocator);
        REQUIRE(env.registered);
        ExchangeBundle bundle(allocator);
        REQUIRE(OpenFixture(allocator, "Golden.rxbundle", bundle));
        const uint32 asset = FindAsset(bundle, GoldenMaterialPath);
        REQUIRE(asset != ExchangeNoIndex);
        const ExchangeAsset& base = bundle.Assets()[asset];
        struct Case {
            JsonEdit edit;
            AssetDiagnosticCode code;
            uint64 detail;
        };
        const Case cases[] = {
            {{"settings.BlendMode", "7"}, AssetDiagnosticCode::InvalidSource, 0},
            {{"settings", "[]"}, AssetDiagnosticCode::InvalidSource, 0},
            {{"settings.OpacityMaskClipValue", R"("abc")"}, AssetDiagnosticCode::InvalidSource, 0},
            {{"settings.BlendMode", R"("BLEND_MAX")"}, AssetDiagnosticCode::InvalidSource, 0},
            {{"settings.UserTextureDivisor", "[1, 2]"}, AssetDiagnosticCode::InvalidSource, 0},
            {{"settings.TranslucentShadowStartOffset", R"("x")"},
             AssetDiagnosticCode::InvalidSource,
             0},
            {{"settings.NumCustomizedUVs", "300"}, AssetDiagnosticCode::InvalidSource, 0},
            {{"settings.StencilRefValue", "256"}, AssetDiagnosticCode::InvalidSource, 0},
            {{"settings.bUsedWithStaticMesh", R"("yes")"},
             AssetDiagnosticCode::CorruptBundle,
             static_cast<uint64>(ExchangeMaterialSettingRow::UsedWithStaticMesh)},
            {{"settings.EditorPitch", R"("x")"},
             AssetDiagnosticCode::CorruptBundle,
             static_cast<uint64>(ExchangeMaterialSettingRow::EditorPitch)},
            {{"settings.EditorYaw", "1.5"},
             AssetDiagnosticCode::CorruptBundle,
             static_cast<uint64>(ExchangeMaterialSettingRow::EditorYaw)},
        };
        for (const Case& item : cases) {
            Attempt attempt(allocator);
            Run(env, allocator, base, Span<const JsonEdit>(&item.edit, 1), attempt);
            REQUIRE(attempt.prepared);
            CHECK(attempt.error == ErrorCode::CorruptData);
            CHECK(attempt.failure.code == item.code && attempt.failure.detail == item.detail);
        }

        // 머티리얼이 아닌 에셋과 registry 없음은 호출 계약 위반이다.
        const uint32 cube = FindAsset(bundle, CubePath);
        REQUIRE(cube != ExchangeNoIndex);
        material::MaterialTraitsDesc traits;
        material::VertexInputUsage usage(allocator);
        MaterialPreviewSettings preview;
        Array<ExchangeSettingWarning> warnings(allocator);
        ExchangeMaterialFailure failure;
        ExchangeMaterialSettingsInput input;
        input.asset = &bundle.Assets()[cube];
        input.registry = &env.objects;
        input.access = env.access;
        CHECK(TranslateExchangeMaterialSettings(input, traits, usage, preview, warnings, failure)
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(failure.code == AssetDiagnosticCode::InvalidSource);
        input.asset = &base;
        input.registry = nullptr;
        CHECK(TranslateExchangeMaterialSettings(input, traits, usage, preview, warnings, failure)
                  .Error() == ErrorCode::InvalidArgument);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace exchange_material_settings_tests_file
