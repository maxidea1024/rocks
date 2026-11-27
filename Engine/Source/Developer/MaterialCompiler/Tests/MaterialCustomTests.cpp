#include <Core/Text/StringBuilder.h>
#include <MaterialCompiler/MaterialSourceWriter.h>
#include <MaterialSchema/Nodes/Custom/CustomHlslProperties.generated.h>
#include <MaterialSchema/Nodes/Custom/CustomHlslProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectProperties.h>

#include "MaterialGraphFixture.h"

// MS2f5 시험이다(Docs/Design/MaterialSystem 6.13절 MS2f5): 생성 함수의 정의·호출 모양(문맥 인자,
// 이름 없는 입력 건너뜀, 텍스처 두 인자, Double 인자, 추가 출력 inout 0 초기화, return 감싸기,
// 정의·include), 입력 5개 이상(인자 사슬), V13 문구, 미연결 입력 문구, MaterialAttributes 출력
// 거부, 같은 노드 정점·픽셀 두 판, clip 선언과 픽셀 버림 표시, 코드가 IR hash에 드는지를 본다.
using namespace rocks;
using namespace rocks::material;
namespace material_custom_tests_file {

using namespace material_graph_fixture;

namespace {

constexpr Guid CustomBrickId{0x43555354ull, 1};
// 이름 hex 자릿수다(Guid high 64 bit).
constexpr uint32 CustomHexDigits = 16;
constexpr uint32 CustomHexBits = 4;

MaterialTextureFacts CustomBrickFacts() noexcept {
    MaterialTextureFacts facts;
    facts.asset = CustomBrickId;
    facts.kind = IrTextureKind::Texture2D;
    return facts;
}

const MaterialTextureFacts CustomTextures[] = {CustomBrickFacts()};

// Custom 노드 속성 묶음이다(시험마다 칸을 채운다).
struct CustomSpec {
    explicit CustomSpec(TranslatorEnv& env) noexcept : env(env), properties(env.allocator) {
        (void)properties.description.TryAssign("Custom");
    }

    CustomSpec& Code(StringView code) noexcept {
        ready = ready && properties.code.TryAssign(code).IsOk();
        return *this;
    }

    CustomSpec& Output(CustomOutputType type) noexcept {
        properties.outputType = type;
        return *this;
    }

    CustomSpec& Input(StringView name) noexcept {
        CustomInput input;
        input.inputName = name.IsEmpty() ? Name() : env.Intern(name);
        ready = ready && properties.inputs.TryPushBack(input).IsOk();
        return *this;
    }

    CustomSpec& Extra(StringView name, CustomOutputType type) noexcept {
        CustomOutput output;
        output.outputName = name.IsEmpty() ? Name() : env.Intern(name);
        output.outputType = type;
        ready = ready && properties.additionalOutputs.TryPushBack(output).IsOk();
        return *this;
    }

    CustomSpec& Define(StringView name, StringView value) noexcept {
        CustomDefine define(env.allocator);
        ready = ready && define.defineName.TryAssign(name).IsOk() &&
                define.defineValue.TryAssign(value).IsOk() &&
                properties.additionalDefines.TryPushBack(Move(define)).IsOk();
        return *this;
    }

    CustomSpec& Include(StringView path) noexcept {
        String text(env.allocator);
        ready = ready && text.TryAssign(path).IsOk() &&
                properties.includeFilePaths.TryPushBack(Move(text)).IsOk();
        return *this;
    }

    CustomSpec& Clip(CustomClipInstruction clip) noexcept {
        properties.containsClipInstruction = clip;
        return *this;
    }

    Guid Add(GraphFixture& fixture) noexcept {
        return ready ? fixture.Add("Material.CustomHlsl", env.Save(properties)) : Guid{};
    }

    TranslatorEnv& env;
    CustomHlslProperties properties;
    bool ready = true;
};

Guid AddScalar(GraphFixture& fixture, float value) noexcept {
    ConstantProperties properties;
    properties.r = value;
    return fixture.Add("Material.Constant", fixture.env.Save(properties));
}

Guid AddColor(GraphFixture& fixture) noexcept {
    Constant3Properties properties;
    properties.constant = LinearColor(1.0f, 2.0f, 3.0f, 1.0f);
    return fixture.Add("Material.Constant3", fixture.env.Save(properties));
}

// node의 pin을 Unlit emissive로 내보낸다.
void ToEmissive(GraphFixture& fixture, Guid node, StringView pin) noexcept {
    const Guid unlit = fixture.Add("Material.SurfaceUnlit");
    fixture.Connect(node, pin, unlit, "EmissiveColor");
    fixture.Connect(unlit, "Result", OutputNodeId, "Surface");
}

bool TranslateCustom(GraphFixture& fixture, MaterialTranslation& out,
                     const MaterialTraitsDesc& traits = Traits()) noexcept {
    MaterialTranslationInput input{fixture.document, fixture.env.catalog, fixture.env.names,
                                   traits,           TestAssetId,         {}};
    input.textures = CustomTextures;
    return fixture.ready && TranslateMaterial(fixture.env.allocator, input, out).IsOk();
}

bool GenerateCustom(GraphFixture& fixture, MaterialTranslation& out,
                    MaterialGeneratedSource& source) noexcept {
    return TranslateCustom(fixture, out) && out.succeeded &&
           WriteMaterialSource(fixture.env.allocator, out.module, Traits(), TestAssetId,
                               MainGraphId, source)
               .IsOk() &&
           source.succeeded;
}

bool Has(const MaterialGeneratedSource& source, StringView part) noexcept {
    return source.text.View().Contains(part, StringComparison::CaseSensitive);
}

bool HasMessage(const MaterialTranslation& out, MaterialDiagnosticCode code,
                StringView message) noexcept {
    for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
        if (diagnostic.code == ToDiagnosticCode(code) && diagnostic.message.View() == message) {
            return true;
        }
    }

    return false;
}

// MaterialCustom_<NodeId high 16 hex>_<번호>다.
void AppendCustomName(StringBuilder<>& text, Guid node, uint32 index) noexcept {
    text << "MaterialCustom_";
    for (uint32 at = 0; at < CustomHexDigits; ++at) {
        const uint64 nibble = (node.high >> ((CustomHexDigits - 1 - at) * CustomHexBits)) & 0xF;
        const char digit = "0123456789abcdef"[nibble];
        text << StringView(&digit, 1);
    }

    text << "_" << index;
}

// 모듈의 첫 CustomCall 번호다(없으면 NoIrValue).
IrValueId FirstCall(const MaterialTranslation& out) noexcept {
    for (usize at = 0; at < out.module.Values().Size(); ++at) {
        if (out.module.Values()[at].op == IrOp::CustomCall) {
            return static_cast<IrValueId>(at);
        }
    }

    return NoIrValue;
}

} // namespace

// 함수 모양: 정의·include·문맥 인자·이름 없는 입력 건너뜀·추가 출력 inout·return 감싸기와 호출의
// 0 초기화 지역 변수.
TEST(MaterialCustom_FunctionShape) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    CustomSpec spec(env);
    spec.Code("A * B")
        .Input("A")
        .Input("")
        .Input("B")
        .Extra("Extra", CustomOutputType::Float2)
        .Define("MY_SCALE", "2")
        .Include("/Engine/Custom/Helpers.rsh");
    const Guid custom = spec.Add(fixture);
    fixture.Connect(AddColor(fixture), "Result", custom, "A");
    fixture.Connect(AddScalar(fixture, 0.5f), "Result", custom, "B");
    ToEmissive(fixture, custom, "Result");
    ComponentMaskProperties red;
    red.r = true;
    const Guid mask = fixture.Add("Material.ComponentMask", env.Save(red));
    fixture.Connect(custom, "Extra", mask, "Input");
    fixture.Connect(mask, "Result", OutputNodeId, "PixelDepthOffset");
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(GenerateCustom(fixture, out, source));
    REQUIRE(out.module.CustomFunctions().Functions().Size() == 1);

    StringBuilder<> definition(env.allocator);
    definition << "#ifndef MY_SCALE\n#define MY_SCALE 2\n#endif//MY_SCALE\n"
               << "#include \"/Engine/Custom/Helpers.rsh\"\nfloat3 ";
    AppendCustomName(definition, custom, 0);
    definition
        << "(in MaterialInputs c, float3 A, float B, inout float2 Extra)\n{\nreturn A * B;\n}\n";
    REQUIRE(definition.Status());
    CHECK(Has(source, definition.View()));

    const IrValueId call = FirstCall(out);
    REQUIRE(call != NoIrValue);
    StringBuilder<> local(env.allocator);
    local << "    float2 m" << call << "_0 = 0;\n    const float3 m" << call << " = ";
    AppendCustomName(local, custom, 0);
    local << "(c, ";
    StringBuilder<> tail(env.allocator);
    tail << ", m" << call << "_0);\n";
    StringBuilder<> extra(env.allocator);
    extra << " = m" << call << "_0;\n";
    REQUIRE(local.Status());
    REQUIRE(tail.Status());
    REQUIRE(extra.Status());
    CHECK(Has(source, local.View()));
    CHECK(Has(source, tail.View()));
    CHECK(Has(source, extra.View()));
}

// return이 있으면 본문 그대로이고 \r\n은 \n이다(UE).
TEST(MaterialCustom_ReturnKept) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    CustomSpec spec(env);
    spec.Code("float3 x = 1;\r\nreturn x;");
    const Guid custom = spec.Add(fixture);
    ToEmissive(fixture, custom, "Result");
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(GenerateCustom(fixture, out, source));
    CHECK(Has(source, "(in MaterialInputs c)\n{\nfloat3 x = 1;\nreturn x;\n}\n"));
    CHECK(!Has(source, "return float3 x"));
}

// 입력 다섯은 인자 사슬 두 칸이고 원문 인자는 순서대로다.
TEST(MaterialCustom_ArgumentChain) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    CustomSpec spec(env);
    spec.Code("return I0 + I1 + I2 + I3 + I4;").Output(CustomOutputType::Float1);
    const StringView names[] = {"I0", "I1", "I2", "I3", "I4"};
    for (StringView name : names) {
        spec.Input(name);
    }

    const Guid custom = spec.Add(fixture);
    for (usize at = 0; at < 5; ++at) {
        fixture.Connect(AddScalar(fixture, static_cast<float>(at) + 0.25f), "Result", custom,
                        names[at]);
    }

    fixture.Connect(custom, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(GenerateCustom(fixture, out, source));
    usize links = 0;
    usize chained = 0;
    for (const IrValue& value : out.module.Values()) {
        if (value.op == IrOp::CustomArguments) {
            ++links;
            chained += value.immediate[1] != 0 ? 1 : 0;
        }
    }

    CHECK(links == 2);
    CHECK(chained == 1);
    CHECK(Has(source, "(in MaterialInputs c, float I0, float I1, float I2, float I3, float I4)\n"));
    CHECK(Has(source, "(c, 0.25, 1.25, 2.25, 3.25, 4.25);\n"));
}

// 텍스처 입력은 텍스처·샘플러 두 인자이고 그 slot은 자기 샘플러를 선언한다. Double 입력은
// MaterialDouble<N> 하나다.
TEST(MaterialCustom_TextureAndDoubleArguments) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    CustomSpec spec(env);
    spec.Code("return T.Sample(TSampler, float2(0, 0)).rgb;").Input("T").Input("P");
    const Guid custom = spec.Add(fixture);
    TextureObjectProperties object;
    object.texture.assetId = AssetId{CustomBrickId};
    const Guid texture = fixture.Add("Material.TextureObject", env.Save(object));
    fixture.Connect(texture, "Result", custom, "T");
    fixture.Connect(fixture.Add("Material.WorldPosition"), "XYZ", custom, "P");
    ToEmissive(fixture, custom, "Result");
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(GenerateCustom(fixture, out, source));
    CHECK(Has(source,
              "(in MaterialInputs c, Texture2D T, SamplerState TSampler, MaterialDouble3 P)"));
    CHECK(Has(source, "(c, MaterialTexture0, MaterialTexture0Sampler, m"));
    CHECK(Has(source, "SamplerState MaterialTexture0Sampler;\n"));
}

// 이름 있는 입력이 연결되지 않으면 UE 문구다(번호는 이름 없는 입력도 센다).
TEST(MaterialCustom_MissingInput) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    CustomSpec spec(env);
    spec.Code("return A;").Input("").Input("A");
    const Guid custom = spec.Add(fixture);
    ToEmissive(fixture, custom, "Result");
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateCustom(fixture, out));
    CHECK(!out.succeeded);
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::MissingInput, custom));
    CHECK(HasMessage(out, MaterialDiagnosticCode::MissingInput,
                     "Custom material Custom missing input 2 (A)"));
}

// V13: 금지 토큰(주석 안은 통과)·본문 닫기·include 경로·clip 선언·이름·정의 값.
TEST(MaterialCustom_CodeRules) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const auto rejected = [&](CustomSpec& spec, StringView message) noexcept {
        GraphFixture fixture(env);
        const Guid custom = spec.Add(fixture);
        ToEmissive(fixture, custom, "Result");
        MaterialTranslation out(env.allocator);
        return TranslateCustom(fixture, out) && !out.succeeded &&
               HasErrorAt(out, MaterialDiagnosticCode::CustomCodeRule, custom) &&
               HasMessage(out, MaterialDiagnosticCode::CustomCodeRule, message);
    };

    {
        CustomSpec spec(env);
        spec.Code("static float g = 1; return g;");
        CHECK(rejected(spec, "Custom HLSL code must not contain 'static'"));
    }

    {
        CustomSpec spec(env);
        spec.Code("#  include \"x.rsh\"\nreturn 1;");
        CHECK(rejected(spec, "Custom HLSL code must not contain '#include'"));
    }

    {
        CustomSpec spec(env);
        spec.Code("#pragma warning(disable: 1)\nreturn 1;");
        CHECK(rejected(spec, "Custom HLSL code must not contain '#pragma'"));
    }

    const StringView words[] = {"register", "cbuffer", "tbuffer", "groupshared"};
    for (StringView word : words) {
        CustomSpec spec(env);
        StringBuilder<> code(env.allocator);
        code << word << " x; return 1;";
        StringBuilder<> message(env.allocator);
        message << "Custom HLSL code must not contain '" << word << "'";
        REQUIRE(code.Status());
        REQUIRE(message.Status());
        spec.Code(code.View());
        CHECK(rejected(spec, message.View()));
    }

    {
        CustomSpec spec(env);
        spec.Code("return 1; } float3 Other() { return 2;");
        CHECK(rejected(spec, "Custom HLSL code must not close the function body"));
    }

    const StringView paths[] = {"Engine/Relative.rsh", "/Engine/../Secret.rsh",
                                "/Engine/Quote\".rsh"};
    for (StringView path : paths) {
        CustomSpec spec(env);
        spec.Code("return 1;").Include(path);
        StringBuilder<> message(env.allocator);
        message << "Custom HLSL include path is not an allowed virtual path: " << path;
        REQUIRE(message.Status());
        CHECK(rejected(spec, message.View()));
    }

    {
        CustomSpec spec(env);
        spec.Code("clip(-1); return 1;").Clip(CustomClipInstruction::No);
        CHECK(rejected(spec, "Custom HLSL uses clip or discard but containsClipInstruction is No"));
    }

    {
        CustomSpec spec(env);
        spec.Code("return 1;").Extra("not valid", CustomOutputType::Float1);
        CHECK(rejected(spec, "Custom HLSL name is not a valid identifier: not valid"));
    }

    {
        CustomSpec spec(env);
        spec.Code("return 1;").Define("LINES", "1\nstatic float g;");
        CHECK(rejected(spec, "Custom HLSL define value must be a single line: LINES"));
    }

    {
        CustomSpec spec(env);
        spec.Code("return 1;").Define("SNEAK", "static");
        CHECK(rejected(spec, "Custom HLSL code must not contain 'static'"));
    }

    {
        // 주석 안의 토큰·중괄호는 검사하지 않는다.
        GraphFixture fixture(env);
        CustomSpec spec(env);
        spec.Code("// static register }\n/* cbuffer } */ return float3(1, 1, 1);");
        const Guid custom = spec.Add(fixture);
        ToEmissive(fixture, custom, "Result");
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateCustom(fixture, out));
        CHECK(out.succeeded);
    }
}

// MaterialAttributes 출력은 지원하지 않는다(주 출력·추가 출력).
TEST(MaterialCustom_AttributesOutputRejected) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    constexpr StringView Message = "Custom HLSL MaterialAttributes output is not supported";
    {
        GraphFixture fixture(env);
        CustomSpec spec(env);
        spec.Code("return 1;").Output(CustomOutputType::MaterialAttributes);
        const Guid custom = spec.Add(fixture);
        fixture.Connect(custom, "Result", OutputNodeId, "Attributes");
        MaterialTraitsDesc traits = Traits();
        traits.useMaterialAttributes = true;
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateCustom(fixture, out, traits));
        CHECK(HasErrorAt(out, MaterialDiagnosticCode::UnsupportedCapability, custom));
        CHECK(HasMessage(out, MaterialDiagnosticCode::UnsupportedCapability, Message));
    }

    {
        GraphFixture fixture(env);
        CustomSpec spec(env);
        spec.Code("return 1;").Extra("Attributes", CustomOutputType::MaterialAttributes);
        const Guid custom = spec.Add(fixture);
        ToEmissive(fixture, custom, "Result");
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateCustom(fixture, out));
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::UnsupportedCapability, custom));
        CHECK(HasMessage(out, MaterialDiagnosticCode::UnsupportedCapability, Message));
    }
}

// 같은 노드를 정점(WPO)과 픽셀(emissive)에서 쓰면 두 판이다. 이전 프레임 정점은 정점 판을
// 다시 쓴다.
TEST(MaterialCustom_VertexAndPixelVariants) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    CustomSpec spec(env);
    spec.Code("return float3(0, 0, 1);");
    const Guid custom = spec.Add(fixture);
    ToEmissive(fixture, custom, "Result");
    fixture.Connect(custom, "Result", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(GenerateCustom(fixture, out, source));
    const Span<const IrCustomFunction> functions = out.module.CustomFunctions().Functions();
    REQUIRE(functions.Size() == 2);
    CHECK(functions[0].context != functions[1].context);
    CHECK(Has(source, "(in MaterialVertexInputs c)\n{\nreturn float3(0, 0, 1);\n}\n"));
    CHECK(Has(source, "(in MaterialInputs c)\n{\nreturn float3(0, 0, 1);\n}\n"));
}

// 픽셀 버림 표시: Yes, 또는 Search이면서 clip(·discard가 있을 때(UE HasPixelDiscard).
TEST(MaterialCustom_PixelDiscard) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const auto discards = [&](CustomClipInstruction clip, StringView code) noexcept {
        GraphFixture fixture(env);
        CustomSpec spec(env);
        spec.Code(code).Clip(clip);
        const Guid custom = spec.Add(fixture);
        ToEmissive(fixture, custom, "Result");
        MaterialTranslation out(env.allocator);
        return TranslateCustom(fixture, out) && out.succeeded &&
               out.module.UsesCustomPixelDiscard();
    };

    CHECK(discards(CustomClipInstruction::Yes, "return 1;"));
    CHECK(discards(CustomClipInstruction::Search, "if (c.pixelNormal.z < 0) discard; return 1;"));
    CHECK(discards(CustomClipInstruction::Search, "clip(-1); return 1;"));
    CHECK(!discards(CustomClipInstruction::Search, "return 1;"));
}

// 코드는 IR hash에 든다(쿠킹 키).
TEST(MaterialCustom_CodeInHash) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const auto hash = [&](StringView code, Sha256Digest& digest) noexcept {
        GraphFixture fixture(env);
        CustomSpec spec(env);
        spec.Code(code);
        const Guid custom = spec.Add(fixture);
        ToEmissive(fixture, custom, "Result");
        MaterialTranslation out(env.allocator);
        if (!TranslateCustom(fixture, out) || !out.succeeded) {
            return false;
        }

        digest = out.module.Hash();
        return true;
    };

    Sha256Digest first{};
    Sha256Digest second{};
    Sha256Digest again{};
    REQUIRE(hash("return float3(1, 0, 0);", first));
    REQUIRE(hash("return float3(0, 1, 0);", second));
    REQUIRE(hash("return float3(1, 0, 0);", again));
    CHECK(!(first == second));
    CHECK(first == again);
}

} // namespace material_custom_tests_file
