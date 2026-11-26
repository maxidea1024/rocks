#include <Core/Text/StringBuilder.h>
#include <MaterialCompiler/MaterialSourceWriter.h>
#include <MaterialSchema/Nodes/Parameters/StaticSwitchParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/StaticSwitchParameterProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectParameterProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleParameter2DProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleParameter2DProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleParameterCubeProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleParameterCubeProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleProperties.h>

#include "MaterialGraphFixture.h"

// MS2c3a 시험이다(Docs/Design/MaterialSystem 6.10절 텍스처): 텍스처 slot 표, 샘플 방식(view mip
// bias· 정점 문맥 Level·명시 미분·gather·decal), UE 검사 순서와 문구(입력 없음·해석 불가·UVW·Bias
// 단계· 미분 없음·gather 제한), V10 샘플 종류 검사(UE VerifySamplerType), V9 샘플러 예산, 파라미터
// 샘플의 종류 검사, 큰 좌표 uv 주소 모드, 텍스처 값의 산술 금지(V1), 생성 원문 선언과 샘플 철자를
// 본다.
using namespace rocks;
using namespace rocks::material;
namespace material_texture_lowering_tests_file {

using namespace material_graph_fixture;

namespace {

constexpr Guid BrickId{0x54455854ull, 1};
constexpr Guid NormalId{0x54455854ull, 2};
constexpr Guid SkyId{0x54455854ull, 3};
constexpr Guid MaskId{0x54455854ull, 4};
constexpr Guid MissingId{0x54455854ull, 9};
constexpr Guid ParameterId{0x50415254ull, 1};

MaterialTextureFacts
Facts(Guid asset, IrTextureKind kind = IrTextureKind::Texture2D,
      TextureCompressionSettings compression = TextureCompressionSettings::Default,
      bool srgb = true) noexcept {
    MaterialTextureFacts facts;
    facts.asset = asset;
    facts.kind = kind;
    facts.compression = compression;
    facts.srgb = srgb;
    return facts;
}

const MaterialTextureFacts TestTextures[] = {
    Facts(BrickId),
    Facts(NormalId, IrTextureKind::Texture2D, TextureCompressionSettings::Normalmap, false),
    Facts(SkyId, IrTextureKind::TextureCube),
    Facts(MaskId, IrTextureKind::Texture2D, TextureCompressionSettings::Normalmap, true),
};

bool TranslateTextures(GraphFixture& fixture, MaterialTranslation& out,
                       const MaterialTraitsDesc& traits = Traits(),
                       const MaterialTranslationTarget& target = {}) noexcept {
    MaterialTranslationInput input{fixture.document, fixture.env.catalog, fixture.env.names,
                                   traits,           TestAssetId,         target};
    input.textures = TestTextures;
    return fixture.ready && TranslateMaterial(fixture.env.allocator, input, out).IsOk();
}

bool Generate(GraphFixture& fixture, MaterialTranslation& out, MaterialGeneratedSource& source,
              const MaterialTraitsDesc& traits = Traits()) noexcept {
    return TranslateTextures(fixture, out, traits) && out.succeeded &&
           WriteMaterialSource(fixture.env.allocator, out.module, traits, TestAssetId, MainGraphId,
                               source)
               .IsOk() &&
           source.succeeded;
}

bool HasText(StringView text, StringView part) noexcept {
    for (usize at = 0; at + part.ByteCount() <= text.ByteCount(); ++at) {
        if (StringView(text.Data() + at, part.ByteCount()) == part) {
            return true;
        }
    }

    return false;
}

bool HasMessage(const MaterialTranslation& out, StringView message) noexcept {
    for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
        if (diagnostic.message.View() == message) {
            return true;
        }
    }

    return false;
}

const IrValue* FindSample(const MaterialTranslation& out) noexcept {
    for (const IrValue& value : out.module.Values()) {
        if (value.op == IrOp::TextureSample) {
            return &value;
        }
    }

    return nullptr;
}

TextureSampleProperties SampleOf(TranslatorEnv& env, Guid texture,
                                 SamplerType type = SamplerType::Color) noexcept {
    TextureSampleProperties properties(env.allocator);
    properties.texture.assetId = AssetId{texture};
    properties.samplerType = type;
    return properties;
}

// node의 R 출력을 AO에 잇는다.
void ToAo(GraphFixture& fixture, Guid node) noexcept {
    fixture.Connect(node, "R", OutputNodeId, "AmbientOcclusion");
}

} // namespace

// 보통 샘플: Texture slot 하나, Surface 픽셀의 view mip bias(Bias + MaterialTextureMipBias), 원문
// 선언과 철자.
TEST(MaterialTexture_SampleWithViewMipBias) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.TextureSample", env.Save(SampleOf(env, BrickId)));
    ToAo(fixture, node);
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source));
    REQUIRE(out.module.Textures().Size() == 1);
    CHECK(out.module.Textures()[0].source == IrTextureSource::Asset);
    CHECK(out.module.Textures()[0].id == BrickId);
    const IrValue* sample = FindSample(out);
    REQUIRE(sample != nullptr);
    CHECK(sample->immediate[1] == static_cast<uint64>(IrTextureMipMode::Bias));
    CHECK(sample->frequency == IrFrequency::Pixel);
    CHECK(HasText(source.text.View(),
                  "#define MATERIAL_NUM_TEXTURES 1\n\nTexture2D MaterialTexture0;\n"
                  "SamplerState MaterialTexture0Sampler;\n"));
    CHECK(HasText(source.text.View(),
                  "MaterialProcessColorLookup(MaterialSample2DBias(MaterialTexture0, "
                  "MaterialTexture0Sampler, "));
    CHECK(HasText(source.text.View(), "MaterialContextMaterialTextureMipBias(c)"));
}

// automaticViewMipBias가 거짓이면 하드웨어 미분 샘플이다. 입력 static Bool이 속성보다 앞선다.
TEST(MaterialTexture_ViewMipBiasOff) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    TextureSampleProperties properties = SampleOf(env, BrickId);
    properties.automaticViewMipBias = false;
    const Guid node = fixture.Add("Material.TextureSample", env.Save(properties));
    ToAo(fixture, node);
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source));
    CHECK(HasText(source.text.View(),
                  "MaterialSample2D(MaterialTexture0, MaterialTexture0Sampler, "));

    GraphFixture overridden(env);
    const Guid sample = overridden.Add("Material.TextureSample", env.Save(SampleOf(env, BrickId)));
    const Guid off = overridden.Add("Material.StaticBool");
    overridden.Connect(off, "Result", sample, "AutomaticViewMipBiasValue");
    ToAo(overridden, sample);
    MaterialTranslation result(env.allocator);
    REQUIRE(TranslateTextures(overridden, result));
    REQUIRE(result.succeeded);
    REQUIRE(FindSample(result) != nullptr);
    CHECK(FindSample(result)->immediate[1] == static_cast<uint64>(IrTextureMipMode::Implicit));
}

// 입력 없음·해석 불가 문구(UE).
TEST(MaterialTexture_MissingAndUnresolved) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture missing(env);
    TextureSampleProperties empty(env.allocator);
    const Guid lonely = missing.Add("Material.TextureSample", env.Save(empty));
    ToAo(missing, lonely);
    MaterialTranslation rejected(env.allocator);
    REQUIRE(TranslateTextures(missing, rejected));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::MissingInput, lonely));
    CHECK(HasMessage(rejected, "Missing input texture"));

    GraphFixture unknown(env);
    const Guid stray = unknown.Add("Material.TextureSample", env.Save(SampleOf(env, MissingId)));
    ToAo(unknown, stray);
    MaterialTranslation unresolved(env.allocator);
    REQUIRE(TranslateTextures(unknown, unresolved));
    CHECK(HasOnlyErrorAt(unresolved, MaterialDiagnosticCode::InvalidProperty, stray));
    SystemAllocator scratch;
    StringBuilder<> expected(scratch);
    expected << "Could not resolve referenced texture '" << AssetId{MissingId} << "'.";
    REQUIRE(expected.Status());
    CHECK(HasMessage(unresolved, expected.View()));
}

// V10: 노멀맵을 Color로 샘플하면 UE 문구, sRGB 노멀맵을 Normal로 샘플하면 sRGB 문구다. 맞는
// Normal 샘플은 MaterialUnpackNormalMap이다.
TEST(MaterialTexture_SamplerTypeCheck) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    SystemAllocator scratch;
    GraphFixture wrong(env);
    const Guid color = wrong.Add("Material.TextureSample", env.Save(SampleOf(env, NormalId)));
    ToAo(wrong, color);
    MaterialTranslation mismatch(env.allocator);
    REQUIRE(TranslateTextures(wrong, mismatch));
    CHECK(HasOnlyErrorAt(mismatch, MaterialDiagnosticCode::SamplerMismatch, color));
    StringBuilder<> first(scratch);
    first << "Sampler type is Color, should be Normal for " << AssetId{NormalId};
    REQUIRE(first.Status());
    CHECK(HasMessage(mismatch, first.View()));

    GraphFixture srgb(env);
    const Guid normal =
        srgb.Add("Material.TextureSample", env.Save(SampleOf(env, MaskId, SamplerType::Normal)));
    ToAo(srgb, normal);
    MaterialTranslation encoded(env.allocator);
    REQUIRE(TranslateTextures(srgb, encoded));
    StringBuilder<> second(scratch);
    second << "To use 'Normal' as sampler type, SRGB must be disabled for " << AssetId{MaskId};
    REQUIRE(second.Status());
    CHECK(HasMessage(encoded, second.View()));

    GraphFixture right(env);
    const Guid unpacked =
        right.Add("Material.TextureSample", env.Save(SampleOf(env, NormalId, SamplerType::Normal)));
    ToAo(right, unpacked);
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(right, out, source));
    CHECK(HasText(source.text.View(), "MaterialUnpackNormalMap(MaterialSample2DBias("));
}

// Cube는 Coordinates가 필요하다(UE 문구). 연결하면 Float3 uv의 Cube 샘플이고 view mip bias가 없다.
TEST(MaterialTexture_CubeNeedsCoordinates) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture missing(env);
    const Guid lonely = missing.Add("Material.TextureSample", env.Save(SampleOf(env, SkyId)));
    ToAo(missing, lonely);
    MaterialTranslation rejected(env.allocator);
    REQUIRE(TranslateTextures(missing, rejected));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::MissingInput, lonely));
    CHECK(HasMessage(rejected, "UVW input required for cubemap sample"));

    GraphFixture fixture(env);
    const Guid sky = fixture.Add("Material.TextureSample", env.Save(SampleOf(env, SkyId)));
    const Guid direction = fixture.Add("Material.CameraVector");
    fixture.Connect(direction, "Result", sky, "Coordinates");
    ToAo(fixture, sky);
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source));
    CHECK(HasText(source.text.View(), "TextureCube MaterialTexture0;\n"));
    CHECK(HasText(source.text.View(),
                  "MaterialSampleCube(MaterialTexture0, MaterialTexture0Sampler, "));
}

// TextureObject 입력: 객체 노드의 샘플 종류를 쓰고, 객체가 비어 있으면 "Requires valid texture"다.
TEST(MaterialTexture_TextureObjectInput) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    TextureObjectProperties object;
    object.texture.assetId = AssetId{NormalId};
    object.samplerType = SamplerType::Normal;
    const Guid texture = fixture.Add("Material.TextureObject", env.Save(object));
    TextureSampleProperties properties(env.allocator);
    const Guid sample = fixture.Add("Material.TextureSample", env.Save(properties));
    fixture.Connect(texture, "Result", sample, "TextureObject");
    ToAo(fixture, sample);
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateTextures(fixture, out));
    REQUIRE(out.succeeded);
    REQUIRE(FindSample(out) != nullptr);
    CHECK(FindSample(out)->immediate[0] == static_cast<uint64>(SamplerType::Normal));

    GraphFixture empty(env);
    TextureObjectProperties none;
    const Guid lonely = empty.Add("Material.TextureObject", env.Save(none));
    const Guid user = empty.Add("Material.TextureSample", env.Save(properties));
    empty.Connect(lonely, "Result", user, "TextureObject");
    ToAo(empty, user);
    MaterialTranslation rejected(env.allocator);
    REQUIRE(TranslateTextures(empty, rejected));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::MissingInput, lonely));
    CHECK(HasMessage(rejected, "Requires valid texture"));
}

// 공유 Wrap 출처: 자기 샘플러를 선언하지 않고 SharedWrapSampler로 샘플한다(샘플러 예산에 들지
// 않는다).
TEST(MaterialTexture_SharedSampler) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    TextureSampleProperties properties = SampleOf(env, BrickId);
    properties.samplerSource = SamplerSourceMode::WrapWorldGroupSettings;
    const Guid node = fixture.Add("Material.TextureSample", env.Save(properties));
    ToAo(fixture, node);
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source));
    CHECK(!HasText(source.text.View(), "SamplerState MaterialTexture0Sampler"));
    CHECK(HasText(source.text.View(), "(MaterialTexture0, SharedWrapSampler, "));
}

// V9: 자기 샘플러 slot이 maxSamplers를 넘으면 넘긴 노드에 V9다(같은 slot은 한 번 센다).
TEST(MaterialTexture_SamplerBudget) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid first = fixture.Add("Material.TextureSample", env.Save(SampleOf(env, BrickId)));
    const Guid again = fixture.Add("Material.TextureSample", env.Save(SampleOf(env, BrickId)));
    const Guid second = fixture.Add("Material.TextureSample",
                                    env.Save(SampleOf(env, NormalId, SamplerType::Normal)));
    const Guid sum = fixture.Add("Material.Add");
    fixture.Connect(first, "R", sum, "A");
    fixture.Connect(again, "G", sum, "B");
    const Guid total = fixture.Add("Material.Add");
    fixture.Connect(sum, "Result", total, "A");
    fixture.Connect(second, "R", total, "B");
    fixture.Connect(total, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslationTarget target;
    target.maxSamplers = 1;
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateTextures(fixture, out, Traits(), target));
    CHECK(!out.succeeded);
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::BudgetExceeded, second));
    CHECK(HasMessage(out, "Material uses too many samplers (2, limit 1)"));
    CHECK((out.consultedTarget & ToMask(MaterialTargetField::MaxSamplers)) != 0);
}

// 정점 문맥: 하드웨어 미분이 없으므로 Level(constMipValue)이고 view mip bias가 없다. MipBias는 UE
// 문구의 오류다.
TEST(MaterialTexture_VertexContext) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.TextureSample", env.Save(SampleOf(env, BrickId)));
    fixture.Connect(node, "RGB", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source));
    const IrValue* sample = FindSample(out);
    REQUIRE(sample != nullptr);
    CHECK(sample->immediate[1] == static_cast<uint64>(IrTextureMipMode::Level));
    CHECK(FloatOf(ValueOf(out, sample->operands[2]), 0) == -1.0f);
    CHECK(HasText(source.text.View(),
                  "MaterialSample2DLevel(MaterialTexture0, MaterialTexture0Sampler, "));

    GraphFixture bias(env);
    TextureSampleProperties properties = SampleOf(env, BrickId);
    properties.mipValueMode = TextureMipValueMode::MipBias;
    const Guid biased = bias.Add("Material.TextureSample", env.Save(properties));
    bias.Connect(biased, "RGB", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation rejected(env.allocator);
    REQUIRE(TranslateTextures(bias, rejected));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::StageViolation, biased));
    CHECK(HasMessage(rejected, "MipBias is only supported in the pixel shader"));
}

// 명시 미분: ddx·ddy가 필요하다(UE 문구). 둘 다 있으면 Grad이고 view mip bias는 배율을 곱한다.
TEST(MaterialTexture_Derivative) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    TextureSampleProperties properties = SampleOf(env, BrickId);
    properties.mipValueMode = TextureMipValueMode::Derivative;
    GraphFixture missing(env);
    const Guid lonely = missing.Add("Material.TextureSample", env.Save(properties));
    missing.Connect(AddConstant(missing, 0.1f), "Result", lonely, "CoordinatesDX");
    ToAo(missing, lonely);
    MaterialTranslation rejected(env.allocator);
    REQUIRE(TranslateTextures(missing, rejected));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::MissingInput, lonely));
    CHECK(HasMessage(rejected, "Missing DDY(UVs) parameter"));

    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.TextureSample", env.Save(properties));
    fixture.Connect(AddConstant(fixture, 0.1f), "Result", node, "CoordinatesDX");
    fixture.Connect(AddConstant(fixture, 0.2f), "Result", node, "CoordinatesDY");
    ToAo(fixture, node);
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source));
    const IrValue* sample = FindSample(out);
    REQUIRE(sample != nullptr);
    CHECK(sample->immediate[1] == static_cast<uint64>(IrTextureMipMode::Gradient));
    CHECK(sample->operandCount == 4);
    CHECK(HasText(source.text.View(), "MaterialSample2DGrad("));
    CHECK(HasText(source.text.View(), "MaterialContextMaterialTextureDerivativeMultiply(c)"));
}

// gather: mip 방식이 있으면 오류, 채널 함수 철자, view mip bias 없음.
TEST(MaterialTexture_Gather) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    TextureSampleProperties properties = SampleOf(env, BrickId);
    properties.gatherMode = TextureGatherMode::Green;
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.TextureSample", env.Save(properties));
    ToAo(fixture, node);
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source));
    CHECK(HasText(source.text.View(), "MaterialSample2DGatherGreen(MaterialTexture0, "));

    properties.mipValueMode = TextureMipValueMode::MipLevel;
    GraphFixture level(env);
    const Guid leveled = level.Add("Material.TextureSample", env.Save(properties));
    ToAo(level, leveled);
    MaterialTranslation rejected(env.allocator);
    REQUIRE(TranslateTextures(level, rejected));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::UnsupportedCapability, leveled));
    CHECK(HasMessage(
        rejected,
        "Gather does not support mip map overrides (implicitly accesses a specific mip)."));
}

// DeferredDecal 픽셀의 Implicit 샘플은 _Decal 철자다(view mip bias를 끈 경우).
TEST(MaterialTexture_DecalSample) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    TextureSampleProperties properties = SampleOf(env, BrickId);
    properties.automaticViewMipBias = false;
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.TextureSample", env.Save(properties));
    fixture.Connect(node, "R", OutputNodeId, "Opacity");
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source,
                     Traits(BlendMode::AlphaComposite, MaterialDomain::DeferredDecal)));
    CHECK(HasText(source.text.View(), "MaterialSample2D_Decal(MaterialTexture0, "));
}

// 큰 좌표 uv: WorldPosition.xy를 주소 모드(자기 샘플러 → 텍스처 사실 Wrap)대로 낮춘다.
TEST(MaterialTexture_LargeWorldUv) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.TextureSample", env.Save(SampleOf(env, BrickId)));
    const Guid world = fixture.Add("Material.WorldPosition");
    fixture.Connect(world, "XY", node, "Coordinates");
    ToAo(fixture, node);
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source));
    bool demoted = false;
    for (const IrValue& value : out.module.Values()) {
        demoted = demoted || (value.op == IrOp::AddressDemote && value.type == IrFloat(2) &&
                              value.immediate[0] == (1u | (1u << 2) | (1u << 4)));
    }

    CHECK(demoted);
    CHECK(HasText(source.text.View(), "MaterialDoubleApplyAddressMode("));
    CHECK(HasText(source.text.View(), ", 1u, 1u)"));
}

// 파라미터 샘플: 종류 검사(UE TextureIsValid 문구), Cube 좌표 필수, 같은 ParameterId는 한 slot.
TEST(MaterialTexture_ParameterSamples) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    TextureSampleParameter2DProperties wrongKind(env.allocator);
    wrongKind.parameterName = env.Intern("Albedo");
    wrongKind.parameterId = ParameterId;
    wrongKind.texture.assetId = AssetId{SkyId};
    GraphFixture cube(env);
    const Guid mismatch = cube.Add("Material.TextureSampleParameter2D", env.Save(wrongKind));
    ToAo(cube, mismatch);
    MaterialTranslation rejected(env.allocator);
    REQUIRE(TranslateTextures(cube, rejected));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::InvalidProperty, mismatch));
    CHECK(HasMessage(rejected, "Found TextureCube, requires Texture2D"));

    TextureSampleParameter2DProperties none(env.allocator);
    none.parameterName = env.Intern("Albedo");
    GraphFixture empty(env);
    const Guid lonely = empty.Add("Material.TextureSampleParameter2D", env.Save(none));
    ToAo(empty, lonely);
    MaterialTranslation missing(env.allocator);
    REQUIRE(TranslateTextures(empty, missing));
    CHECK(HasMessage(missing, "Found NULL, requires Texture2D"));

    TextureSampleParameterCubeProperties sky(env.allocator);
    sky.parameterName = env.Intern("Sky");
    sky.texture.assetId = AssetId{SkyId};
    GraphFixture noUv(env);
    const Guid needs = noUv.Add("Material.TextureSampleParameterCube", env.Save(sky));
    ToAo(noUv, needs);
    MaterialTranslation needsUv(env.allocator);
    REQUIRE(TranslateTextures(noUv, needsUv));
    CHECK(HasOnlyErrorAt(needsUv, MaterialDiagnosticCode::MissingInput, needs));
    CHECK(HasMessage(needsUv, "Cube sample needs UV input"));

    TextureSampleParameter2DProperties albedo(env.allocator);
    albedo.parameterName = env.Intern("Albedo");
    albedo.parameterId = ParameterId;
    albedo.texture.assetId = AssetId{BrickId};
    GraphFixture shared(env);
    const Guid a = shared.Add("Material.TextureSampleParameter2D", env.Save(albedo));
    const Guid b = shared.Add("Material.TextureSampleParameter2D", env.Save(albedo));
    const Guid sum = shared.Add("Material.Add");
    shared.Connect(a, "R", sum, "A");
    shared.Connect(b, "G", sum, "B");
    shared.Connect(sum, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateTextures(shared, out));
    REQUIRE(out.succeeded);
    REQUIRE(out.module.Textures().Size() == 1);
    CHECK(out.module.Textures()[0].source == IrTextureSource::Parameter);
    CHECK(out.module.Textures()[0].id == ParameterId);
    CHECK(out.module.Textures()[0].defaultAsset == BrickId);
}

// External 샘플 종류는 선행(MPQ-ExternalTexture)이 없어 늦은 진단이다.
TEST(MaterialTexture_ExternalIsPending) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.TextureSample",
                                  env.Save(SampleOf(env, BrickId, SamplerType::External)));
    ToAo(fixture, node);
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateTextures(fixture, out));
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::PrerequisiteMissing, node));
}

// 텍스처 값은 산술에 들어가지 못한다(V1 — static 스위치로 고른 텍스처를 곱셈에 이은 경우).
TEST(MaterialTexture_TextureValueIsNotNumeric) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    TextureObjectProperties object;
    object.texture.assetId = AssetId{BrickId};
    const Guid texture = fixture.Add("Material.TextureObject", env.Save(object));
    StaticSwitchParameterProperties choose;
    choose.parameterName = env.Intern("Pick");
    choose.defaultValue = true;
    const Guid pick = fixture.Add("Material.StaticSwitchParameter", env.Save(choose));
    fixture.Connect(texture, "Result", pick, "A");
    fixture.Connect(texture, "Result", pick, "B");
    const Guid product = fixture.Add("Material.Multiply");
    fixture.Connect(pick, "Result", product, "A");
    fixture.Connect(AddConstant(fixture, 2.0f), "Result", product, "B");
    fixture.Connect(product, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateTextures(fixture, out));
    CHECK(!out.succeeded);
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::TypeMismatch, product));
}

} // namespace material_texture_lowering_tests_file
