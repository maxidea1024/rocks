#include <Core/Text/StringBuilder.h>
#include <MaterialCompiler/MaterialSourceWriter.h>
#include <MaterialCompiler/MaterialConstants.h>
#include <MaterialSchema/Nodes/Custom/CustomHlslProperties.generated.h>
#include <MaterialSchema/Nodes/Custom/CustomHlslProperties.h>
#include <MaterialSchema/Nodes/Parameters/StaticSwitchParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/StaticSwitchParameterProperties.h>
#include <MaterialSchema/Nodes/Texture/AntialiasedTextureMaskProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/AntialiasedTextureMaskProperties.h>
#include <MaterialSchema/Nodes/Texture/ParticleSubUvProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/ParticleSubUvProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectParameterProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleParameter2DProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleParameter2DProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleParameterCubeProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleParameterCubeProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleParameterSubUvProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleParameterSubUvProperties.h>
#include <MaterialSchema/Nodes/Texture/TexturePropertyProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TexturePropertyProperties.h>
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
constexpr Guid MovieId{0x54455854ull, 5};
constexpr Guid ClampedId{0x54455854ull, 6};
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

// 축마다 주소 모드가 다른 텍스처다(X Clamp, Y Mirror, Z Wrap).
MaterialTextureFacts ClampedFacts() noexcept {
    MaterialTextureFacts facts = Facts(ClampedId);
    facts.address[0] = TextureAddress::Clamp;
    facts.address[1] = TextureAddress::Mirror;
    facts.address[2] = TextureAddress::Wrap;
    return facts;
}

const MaterialTextureFacts TestTextures[] = {
    Facts(BrickId),
    Facts(NormalId, IrTextureKind::Texture2D, TextureCompressionSettings::Normalmap, false),
    Facts(SkyId, IrTextureKind::TextureCube),
    Facts(MaskId, IrTextureKind::Texture2D, TextureCompressionSettings::Normalmap, true),
    Facts(MovieId, IrTextureKind::External),
    ClampedFacts(),
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

// 6.10절 텍스처 표: 한 텍스처를 자기·공유 Wrap 샘플러로 샘플링하면 slot이 (출처, id, 샘플러
// 출처)마다 하나씩 둘이고(셰이더 시스템 7.2절 9항 — slot마다 출처 하나), 자기 출처 slot만 샘플러를
// 선언한다. 샘플하지 않는 값(TextureProperty 크기)은 같은 텍스처의 샘플 slot을 쓴다.
TEST(MaterialTexture_SamplerSourceSplitsSlots) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    TextureObjectProperties object;
    object.texture.assetId = AssetId{BrickId};
    const Guid texture = fixture.Add("Material.TextureObject", env.Save(object));
    TextureSampleProperties own(env.allocator);
    const Guid ownSample = fixture.Add("Material.TextureSample", env.Save(own));
    fixture.Connect(texture, "Result", ownSample, "TextureObject");
    TextureSampleProperties shared(env.allocator);
    shared.samplerSource = SamplerSourceMode::WrapWorldGroupSettings;
    const Guid sharedSample = fixture.Add("Material.TextureSample", env.Save(shared));
    fixture.Connect(texture, "Result", sharedSample, "TextureObject");
    TexturePropertyProperties property;
    property.property = ExposedTextureProperty::TextureSize;
    const Guid size = fixture.Add("Material.TextureProperty", env.Save(property));
    fixture.Connect(texture, "Result", size, "TextureObject");
    const Guid sum = fixture.Add("Material.Add");
    fixture.Connect(ownSample, "R", sum, "A");
    fixture.Connect(sharedSample, "R", sum, "B");
    fixture.Connect(sum, "Result", OutputNodeId, "AmbientOcclusion");
    const Guid mask = fixture.Add("Material.ComponentMask", [&]() noexcept {
        ComponentMaskProperties red;
        red.r = true;
        return env.Save(red);
    }());
    fixture.Connect(size, "Result", mask, "Input");
    fixture.Connect(mask, "Result", OutputNodeId, "PixelDepthOffset");
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source));
    const Span<const IrTexture> slots = out.module.Textures();
    REQUIRE(slots.Size() == 2);
    CHECK(slots[0].id == BrickId);
    CHECK(slots[1].id == BrickId);
    CHECK(slots[0].sampler == IrTextureSampler::Own);
    CHECK(slots[1].sampler == IrTextureSampler::SharedWrap);
    CHECK(HasText(source.text.View(), "SamplerState MaterialTexture0Sampler"));
    CHECK(!HasText(source.text.View(), "SamplerState MaterialTexture1Sampler"));
    CHECK(HasText(source.text.View(), "(MaterialTexture0, MaterialTexture0Sampler, "));
    CHECK(HasText(source.text.View(), "(MaterialTexture1, SharedWrapSampler, "));
    // 크기 값의 Texture는 샘플 slot(가장 작은 출처 — 자기) 번호다.
    for (const IrValue& value : out.module.Values()) {
        if (value.op == IrOp::TextureSize) {
            CHECK(out.module.Values()[value.operands[0]].immediate[3] == 0);
        }
    }

    // Custom 텍스처 인자(자기 샘플러)와 공유 Wrap 샘플은 slot 둘이다.
    GraphFixture custom(env);
    const Guid customTexture = custom.Add("Material.TextureObject", env.Save(object));
    CustomHlslProperties code(env.allocator);
    (void)code.code.TryAssign("return T.SampleLevel(TSampler, float2(0.5, 0.5), 0.0).r;");
    code.outputType = CustomOutputType::Float1;
    CustomInput argument;
    argument.inputName = env.Intern("T");
    (void)code.inputs.TryPushBack(argument);
    const Guid call = custom.Add("Material.CustomHlsl", env.Save(code));
    custom.Connect(customTexture, "Result", call, "T");
    const Guid wrapped = custom.Add("Material.TextureSample", env.Save(shared));
    custom.Connect(customTexture, "Result", wrapped, "TextureObject");
    const Guid both = custom.Add("Material.Add");
    custom.Connect(call, "Result", both, "A");
    custom.Connect(wrapped, "R", both, "B");
    custom.Connect(both, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation customOut(env.allocator);
    MaterialGeneratedSource customSource(env.allocator);
    REQUIRE(Generate(custom, customOut, customSource));
    const Span<const IrTexture> customSlots = customOut.module.Textures();
    REQUIRE(customSlots.Size() == 2);
    CHECK(customSlots[0].sampler == IrTextureSampler::Own);
    CHECK(customSlots[1].sampler == IrTextureSampler::SharedWrap);
    CHECK(HasText(customSource.text.View(), "SamplerState MaterialTexture0Sampler"));
    CHECK(HasText(customSource.text.View(), ", MaterialTexture0, MaterialTexture0Sampler"));

    // 샘플하지 않는 값만 있으면 출처 없는 slot 하나이고 샘플러를 선언하지 않는다.
    GraphFixture sizeOnly(env);
    const Guid lonelyTexture = sizeOnly.Add("Material.TextureObject", env.Save(object));
    const Guid lonelySize = sizeOnly.Add("Material.TextureProperty", env.Save(property));
    sizeOnly.Connect(lonelyTexture, "Result", lonelySize, "TextureObject");
    const Guid lonelyMask = sizeOnly.Add("Material.ComponentMask", [&]() noexcept {
        ComponentMaskProperties red;
        red.r = true;
        return env.Save(red);
    }());
    sizeOnly.Connect(lonelySize, "Result", lonelyMask, "Input");
    sizeOnly.Connect(lonelyMask, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation sizeOut(env.allocator);
    MaterialGeneratedSource sizeSource(env.allocator);
    REQUIRE(Generate(sizeOnly, sizeOut, sizeSource));
    REQUIRE(sizeOut.module.Textures().Size() == 1);
    CHECK(sizeOut.module.Textures()[0].sampler == IrTextureSampler::None);
    CHECK(!HasText(sizeSource.text.View(), "SamplerState"));
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
    // 파라미터 텍스처의 출처(12.8절 MS3d1): 주 그래프 Global, 이름은 parameterName.
    const IrParameterOrigin* origin = out.module.FindParameterOrigin(ParameterId);
    REQUIRE(origin != nullptr);
    CHECK(origin->parameterId == ParameterId);
    CHECK(origin->scope == ParameterScopeKind::Global);
    CHECK(origin->name.View() == StringView("Albedo"));

    // 같은 ParameterId의 기본 텍스처가 다르면 ParameterConflict다(이름을 넣은 문구).
    TextureSampleParameter2DProperties clamped(env.allocator);
    clamped.parameterName = env.Intern("Albedo");
    clamped.parameterId = ParameterId;
    clamped.texture.assetId = AssetId{ClampedId};
    GraphFixture differing(env);
    const Guid first = differing.Add("Material.TextureSampleParameter2D", env.Save(albedo));
    const Guid second = differing.Add("Material.TextureSampleParameter2D", env.Save(clamped));
    const Guid both = differing.Add("Material.Add");
    differing.Connect(first, "R", both, "A");
    differing.Connect(second, "G", both, "B");
    differing.Connect(both, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation conflict(env.allocator);
    REQUIRE(TranslateTextures(differing, conflict));
    CHECK(!conflict.succeeded);
    CHECK(HasMessage(conflict, "Parameter Albedo has duplicates with different default values"));
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

// TextureProperty: 입력 없음 문구, 2D 크기는 Float2 상수 블록 멤버(프로그램 leaf), Cube는 UE 문구.
TEST(MaterialTexture_TextureProperty) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture missing(env);
    const Guid lonely = missing.Add("Material.TextureProperty");
    missing.Connect(lonely, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation rejected(env.allocator);
    REQUIRE(TranslateTextures(missing, rejected));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::MissingInput, lonely));
    CHECK(HasMessage(rejected, "TextureSample> Missing input texture"));

    GraphFixture fixture(env);
    TextureObjectProperties object;
    object.texture.assetId = AssetId{BrickId};
    const Guid texture = fixture.Add("Material.TextureObject", env.Save(object));
    TexturePropertyProperties property;
    property.property = ExposedTextureProperty::TexelSize;
    const Guid size = fixture.Add("Material.TextureProperty", env.Save(property));
    fixture.Connect(texture, "Result", size, "TextureObject");
    const Guid uv = fixture.Add("Material.TextureCoordinate");
    const Guid scaled = fixture.Add("Material.Multiply");
    fixture.Connect(size, "Result", scaled, "A");
    fixture.Connect(uv, "Result", scaled, "B");
    const Guid dot = fixture.Add("Material.DotProduct");
    fixture.Connect(scaled, "Result", dot, "A");
    fixture.Connect(scaled, "Result", dot, "B");
    fixture.Connect(dot, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source));
    const IrValue* texel = nullptr;
    for (const IrValue& value : out.module.Values()) {
        texel = value.op == IrOp::TextureSize ? &value : texel;
    }

    REQUIRE(texel != nullptr);
    CHECK(texel->type == IrFloat(2));
    CHECK(texel->immediate[0] == 1);
    CHECK(texel->frequency == IrFrequency::Uniform);
    MaterialConstants constants(env.allocator);
    REQUIRE(BuildMaterialConstants(out.module, constants));
    REQUIRE(constants.members.Size() == 1);
    CHECK(out.module.Values()[constants.members[0].value].op == IrOp::TextureSize);
    REQUIRE(constants.steps.Size() == 2);
    CHECK(constants.steps[0].op == IrOp::Texture);
    CHECK(HasText(source.text.View(), "materialFloats[0].xy"));
    // 크기만 쓰는 slot도 선언은 있고 자기 샘플러는 없다.
    CHECK(HasText(source.text.View(), "Texture2D MaterialTexture0;\n"));
    CHECK(!HasText(source.text.View(), "SamplerState MaterialTexture0Sampler"));

    GraphFixture cube(env);
    TextureObjectProperties sky;
    sky.texture.assetId = AssetId{SkyId};
    const Guid cubeTexture = cube.Add("Material.TextureObject", env.Save(sky));
    const Guid cubeSize = cube.Add("Material.TextureProperty");
    cube.Connect(cubeTexture, "Result", cubeSize, "TextureObject");
    const Guid x = cube.Add("Material.DotProduct");
    cube.Connect(cubeSize, "Result", x, "A");
    cube.Connect(cubeSize, "Result", x, "B");
    cube.Connect(x, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation wrong(env.allocator);
    REQUIRE(TranslateTextures(cube, wrong));
    CHECK(HasOnlyErrorAt(wrong, MaterialDiagnosticCode::TypeMismatch, cubeSize));
    CHECK(HasMessage(wrong,
                     "Texture size only available for Texture2D, TextureVirtual, "
                     "Texture2DArray, VolumeTexture and SparseVolumeTexture, not textureCube"));
}

// RecordTextureStreamingInfo: 텍스처를 그대로 넘기고 Coordinates가 필요하다(UE 문구).
TEST(MaterialTexture_RecordTextureStreamingInfo) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    TextureObjectProperties object;
    object.texture.assetId = AssetId{BrickId};
    TextureSampleProperties properties(env.allocator);
    GraphFixture missing(env);
    const Guid texture = missing.Add("Material.TextureObject", env.Save(object));
    const Guid record = missing.Add("Material.RecordTextureStreamingInfo");
    missing.Connect(texture, "Result", record, "TextureObject");
    const Guid sample = missing.Add("Material.TextureSample", env.Save(properties));
    missing.Connect(record, "Tex", sample, "TextureObject");
    ToAo(missing, sample);
    MaterialTranslation rejected(env.allocator);
    REQUIRE(TranslateTextures(missing, rejected));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::MissingInput, record));
    CHECK(HasMessage(rejected, "Missing input UV Coordinates"));

    GraphFixture fixture(env);
    const Guid source = fixture.Add("Material.TextureObject", env.Save(object));
    const Guid passed = fixture.Add("Material.RecordTextureStreamingInfo");
    fixture.Connect(source, "Result", passed, "TextureObject");
    fixture.Connect(fixture.Add("Material.TextureCoordinate"), "Result", passed, "Coordinates");
    const Guid user = fixture.Add("Material.TextureSample", env.Save(properties));
    fixture.Connect(passed, "Tex", user, "TextureObject");
    ToAo(fixture, user);
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateTextures(fixture, out));
    REQUIRE(out.succeeded);
    CHECK(out.module.Textures().Size() == 1);
}

// ParticleSubUv: blend면 SubUV 좌표 둘의 샘플을 SubUVLerp로 섞고, 픽셀 문맥만 된다(UE 문구).
TEST(MaterialTexture_ParticleSubUv) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    ParticleSubUvProperties properties(env.allocator);
    properties.texture.assetId = AssetId{BrickId};
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.ParticleSubUv", env.Save(properties));
    ToAo(fixture, node);
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source));
    usize samples = 0;
    bool lerped = false;
    for (const IrValue& value : out.module.Values()) {
        samples += value.op == IrOp::TextureSample ? 1 : 0;
        lerped = lerped || value.op == IrOp::Lerp;
    }

    CHECK(samples == 2);
    CHECK(lerped);
    CHECK(HasText(source.text.View(), "MaterialContextParticleSubUvCoords1(c)"));
    CHECK(HasText(source.text.View(), "MaterialContextParticleSubUvLerp(c)"));

    GraphFixture vertex(env);
    const Guid inVertex = vertex.Add("Material.ParticleSubUv", env.Save(properties));
    vertex.Connect(inVertex, "RGB", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation rejected(env.allocator);
    REQUIRE(TranslateTextures(vertex, rejected));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::StageViolation, inVertex));
    CHECK(HasMessage(rejected, "Invalid node used in vertex/hull/domain shader input!"));

    GraphFixture empty(env);
    ParticleSubUvProperties none(env.allocator);
    const Guid lonely = empty.Add("Material.ParticleSubUv", env.Save(none));
    ToAo(empty, lonely);
    MaterialTranslation missing(env.allocator);
    REQUIRE(TranslateTextures(empty, missing));
    CHECK(HasMessage(missing, "Missing ParticleSubUV input texture"));
}

// TextureSampleParameterSubUv: 파라미터 slot과 SubUV 샘플 하나(blend 끔).
TEST(MaterialTexture_ParameterSubUv) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    TextureSampleParameterSubUvProperties properties(env.allocator);
    properties.parameterName = env.Intern("Flipbook");
    properties.parameterId = ParameterId;
    properties.texture.assetId = AssetId{BrickId};
    properties.blend = false;
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.TextureSampleParameterSubUv", env.Save(properties));
    ToAo(fixture, node);
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source));
    REQUIRE(out.module.Textures().Size() == 1);
    CHECK(out.module.Textures()[0].source == IrTextureSource::Parameter);
    usize samples = 0;
    for (const IrValue& value : out.module.Values()) {
        samples += value.op == IrOp::TextureSample ? 1 : 0;
    }

    CHECK(samples == 1);
}

// AntialiasedTextureMask: dot·미분·saturate로 펼친 식, 모바일 형식 오류(UE 문구), 텍스처 없음 문구.
TEST(MaterialTexture_AntialiasedTextureMask) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    AntialiasedTextureMaskProperties properties(env.allocator);
    properties.texture.assetId = AssetId{BrickId};
    properties.samplerType = SamplerType::Color;
    properties.channel = TextureColorChannel::Green;
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.AntialiasedTextureMask", env.Save(properties));
    fixture.Connect(node, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source));
    const IrResult* ao = FindResult(out, "AmbientOcclusion");
    REQUIRE(ao != nullptr);
    CHECK(ValueOf(out, ao->value).op == IrOp::Saturate);
    bool ddx = false;
    for (const IrValue& value : out.module.Values()) {
        ddx = ddx || value.op == IrOp::Ddx;
    }

    CHECK(ddx);
    const IrValue* sample = FindSample(out);
    REQUIRE(sample != nullptr);
    CHECK(sample->immediate[0] == static_cast<uint64>(SamplerType::Masks));
    CHECK(sample->immediate[1] == static_cast<uint64>(IrTextureMipMode::Implicit));
    CHECK(HasText(source.text.View(),
                  "(MaterialSample2D(MaterialTexture0, MaterialTexture0Sampler, "));

    MaterialTranslationTarget mobile;
    mobile.isMobile = true;
    mobile.isPc = false;
    MaterialTranslation rejected(env.allocator);
    REQUIRE(TranslateTextures(fixture, rejected, Traits(), mobile));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::UnsupportedCapability, node));
    CHECK(HasMessage(rejected, "Node not supported in feature level ES3_1. SM5 required."));

    GraphFixture empty(env);
    AntialiasedTextureMaskProperties none(env.allocator);
    const Guid lonely = empty.Add("Material.AntialiasedTextureMask", env.Save(none));
    empty.Connect(lonely, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation missing(env.allocator);
    REQUIRE(TranslateTextures(empty, missing));
    CHECK(HasMessage(missing, "UMaterialExpressionAntialiasedTextureMask> Missing input texture"));
}

// 가상 샘플 종류: 가상 텍스처를 끈 형식은 비가상 처리 함수로 샘플하고(UE ApplyMaterialSamplerType),
// 켠 형식은 늦은 진단이다. Param2D는 비가상 텍스처에 가상 종류면 UE 문구다.
TEST(MaterialTexture_VirtualSamplerTypes) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.TextureSample",
                                  env.Save(SampleOf(env, BrickId, SamplerType::VirtualColor)));
    ToAo(fixture, node);
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source));
    CHECK(HasText(source.text.View(), "MaterialProcessColorLookup(MaterialSample2DBias("));
    CHECK((out.consultedTarget & ToMask(MaterialTargetField::VirtualTexturing)) != 0);

    MaterialTranslationTarget virtualTarget;
    virtualTarget.virtualTexturing = true;
    MaterialTranslation pending(env.allocator);
    REQUIRE(TranslateTextures(fixture, pending, Traits(), virtualTarget));
    CHECK(HasOnlyErrorAt(pending, MaterialDiagnosticCode::PrerequisiteMissing, node));

    TextureSampleParameter2DProperties albedo(env.allocator);
    albedo.parameterName = env.Intern("Albedo");
    albedo.parameterId = ParameterId;
    albedo.texture.assetId = AssetId{BrickId};
    albedo.samplerType = SamplerType::VirtualColor;
    GraphFixture parameter(env);
    const Guid param = parameter.Add("Material.TextureSampleParameter2D", env.Save(albedo));
    ToAo(parameter, param);
    MaterialTranslation rejected(env.allocator);
    REQUIRE(TranslateTextures(parameter, rejected));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::InvalidProperty, param));
    CHECK(HasMessage(rejected, "Sampler requires VirtualTexture"));

    // 비가상 샘플은 가상 텍스처 항목을 읽지 않는다(번역 키가 갈리지 않는다).
    GraphFixture plain(env);
    ToAo(plain, plain.Add("Material.TextureSample", env.Save(SampleOf(env, BrickId))));
    MaterialTranslation normal(env.allocator);
    REQUIRE(TranslateTextures(plain, normal));
    CHECK((normal.consultedTarget & ToMask(MaterialTargetField::VirtualTexturing)) == 0);
}

// 정점 문맥 gather: mip 피연산자 없이 첫 mip을 읽고 픽셀 빈도로 고정하지 않는다(UE HT:7708–7711).
TEST(MaterialTexture_VertexGather) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    TextureSampleProperties properties = SampleOf(env, BrickId);
    properties.gatherMode = TextureGatherMode::Red;
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.TextureSample", env.Save(properties));
    fixture.Connect(node, "RGB", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source));
    const IrValue* sample = FindSample(out);
    REQUIRE(sample != nullptr);
    CHECK(sample->operandCount == 2);
    CHECK(sample->frequency != IrFrequency::Pixel);
    CHECK(HasText(source.text.View(),
                  "MaterialSample2DGatherRed(MaterialTexture0, MaterialTexture0Sampler, "));
    CHECK(!HasText(source.text.View(), "MaterialSample2DLevel("));
}

// Numeric 핀(SphericalParticleOpacity Density)에 텍스처를 이으면 문맥 값 인자에서 V1이다.
TEST(MaterialTexture_TextureIntoContextValueArgument) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    TextureObjectProperties object;
    object.texture.assetId = AssetId{BrickId};
    const Guid texture = fixture.Add("Material.TextureObject", env.Save(object));
    const Guid sphere = fixture.Add("Material.SphericalParticleOpacity");
    fixture.Connect(texture, "Result", sphere, "Density");
    fixture.Connect(sphere, "Result", OutputNodeId, "Opacity");
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateTextures(fixture, out, Traits(BlendMode::AlphaComposite)));
    CHECK(!out.succeeded);
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::TypeMismatch, sphere));
}

// AntialiasedTextureMask는 UE 전용 TextureIsValid대로 외부 텍스처를 받지 않는다.
TEST(MaterialTexture_AntialiasedTextureMaskRejectsExternal) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    AntialiasedTextureMaskProperties properties(env.allocator);
    properties.texture.assetId = AssetId{MovieId};
    properties.samplerType = SamplerType::External;
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.AntialiasedTextureMask", env.Save(properties));
    fixture.Connect(node, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateTextures(fixture, out));
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::InvalidProperty, node));
    CHECK(HasMessage(out, "Found MediaTexture, requires Texture2D"));
}

// view mip bias가 꺼지는 조건: 모바일 형식, Surface·Decal 밖 영역(하드웨어 미분 샘플이 된다).
TEST(MaterialTexture_ViewMipBiasConditions) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.TextureSample", env.Save(SampleOf(env, BrickId)));
    fixture.Connect(node, "R", OutputNodeId, "Opacity");
    MaterialTranslationTarget mobile;
    mobile.isMobile = true;
    mobile.isPc = false;
    MaterialTranslation small(env.allocator);
    REQUIRE(TranslateTextures(fixture, small, Traits(BlendMode::AlphaComposite), mobile));
    REQUIRE(small.succeeded);
    REQUIRE(FindSample(small) != nullptr);
    CHECK(FindSample(small)->immediate[1] == static_cast<uint64>(IrTextureMipMode::Implicit));

    MaterialTranslation post(env.allocator);
    REQUIRE(TranslateTextures(fixture, post,
                              Traits(BlendMode::AlphaComposite, MaterialDomain::PostProcess)));
    REQUIRE(post.succeeded);
    REQUIRE(FindSample(post) != nullptr);
    CHECK(FindSample(post)->immediate[1] == static_cast<uint64>(IrTextureMipMode::Implicit));
    CHECK((post.consultedTarget & ToMask(MaterialTargetField::IsMobile)) == 0);
}

// 큰 좌표 uv의 주소 모드: 자기 샘플러는 텍스처 사실의 축별 모드, 공유 Clamp는 모두 Clamp다.
TEST(MaterialTexture_AddressModeBits) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const auto modes = [&](SamplerSourceMode source, uint64& bits) noexcept {
        TextureSampleProperties properties = SampleOf(env, ClampedId);
        properties.samplerSource = source;
        GraphFixture fixture(env);
        const Guid node = fixture.Add("Material.TextureSample", env.Save(properties));
        fixture.Connect(fixture.Add("Material.WorldPosition"), "XY", node, "Coordinates");
        ToAo(fixture, node);
        MaterialTranslation out(env.allocator);
        if (!TranslateTextures(fixture, out) || !out.succeeded) {
            return false;
        }

        for (const IrValue& value : out.module.Values()) {
            if (value.op == IrOp::AddressDemote) {
                bits = value.immediate[0];
                return true;
            }
        }

        return false;
    };

    uint64 own = 0;
    REQUIRE(modes(SamplerSourceMode::FromTextureAsset, own));
    CHECK(own == (IrAddressModeClamp | (IrAddressModeMirror << IrAddressModeBits) |
                  (IrAddressModeWrap << (2 * IrAddressModeBits))));
    uint64 shared = 1;
    REQUIRE(modes(SamplerSourceMode::ClampWorldGroupSettings, shared));
    CHECK(shared == 0);
}

// 이름 없는 파라미터 샘플은 UE처럼 TextureSample 경로라 gather를 쓴다(ME:4022–4025).
TEST(MaterialTexture_UnnamedParameterSampleUsesGather) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    TextureSampleParameter2DProperties properties(env.allocator);
    properties.texture.assetId = AssetId{BrickId};
    properties.gatherMode = TextureGatherMode::Blue;
    GraphFixture fixture(env);
    ToAo(fixture, fixture.Add("Material.TextureSampleParameter2D", env.Save(properties)));
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source));
    CHECK(out.module.Textures()[0].source == IrTextureSource::Asset);
    CHECK(HasText(source.text.View(), "MaterialSample2DGatherBlue("));

    properties.parameterName = env.Intern("Albedo");
    properties.parameterId = ParameterId;
    GraphFixture named(env);
    ToAo(named, named.Add("Material.TextureSampleParameter2D", env.Save(properties)));
    MaterialTranslation parameter(env.allocator);
    MaterialGeneratedSource text(env.allocator);
    REQUIRE(Generate(named, parameter, text));
    CHECK(!HasText(text.text.View(), "Gather"));
}

// 잘못된 enum 값(저장 손상)은 시스템 실패가 아니라 그 노드 위치의 오류 하나다(속성 읽기가 거르거나
// lowering이 InvalidProperty로 거른다).
TEST(MaterialTexture_OutOfRangeEnum) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    TextureSampleProperties properties = SampleOf(env, BrickId);
    properties.gatherMode = static_cast<TextureGatherMode>(9);
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.TextureSample", env.Save(properties));
    ToAo(fixture, node);
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateTextures(fixture, out));
    CHECK(!out.succeeded);
    CHECK(CountErrors(out) == 1);
    for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
        CHECK(diagnostic.node == node);
    }
}

} // namespace material_texture_lowering_tests_file
