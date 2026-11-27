#include "../../ShaderCompiler/Tests/ShaderManagerTestSupport.h"

#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Move.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <MaterialCompiler/MaterialColorCurveSnapshot.h>
#include <MaterialCompiler/MaterialSourceProvider.h>
#include <MaterialCompiler/MaterialSourceWriter.h>
#include <MaterialSchema/MaterialContextValue.h>
#include <MaterialSchema/MaterialSchemaModule.h>
#include <MaterialSchema/Nodes/Color/ColorRampProperties.generated.h>
#include <MaterialSchema/Nodes/Color/ColorRampProperties.h>
#include <MaterialSchema/Nodes/Custom/CustomHlslProperties.generated.h>
#include <MaterialSchema/Nodes/Custom/CustomHlslProperties.h>
#include <MaterialSchema/Nodes/Noise/NoiseProperties.generated.h>
#include <MaterialSchema/Nodes/Noise/NoiseProperties.h>
#include <MaterialSchema/Nodes/Noise/VectorNoiseProperties.generated.h>
#include <MaterialSchema/Nodes/Noise/VectorNoiseProperties.h>
#include <MaterialSchema/Nodes/Parameters/ScalarParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/ScalarParameterProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleProperties.h>
#include <ShaderCompiler/ShaderCompileManager.h>
#include <ShaderCompiler/ShaderCompileOptions.h>
#include <ShaderCore/ShaderType.h>
#include <stdio.h>

#include "MaterialGraphFixture.h"

// MS2 완료 시험이다(Docs/Design/MaterialSystem 7.8절): 머티리얼 모음(구현 노드마다 하나인 노드 모음
// + 손 모음)의 생성 원문을 시험 pass 셰이더(Tests/Shaders/MaterialTestPass.rsf)의 정점·픽셀 엔트리
// × 공급 표지 퍼뮤테이션 둘로 형식 정의 전부에 실제 ShaderCompileWorker로 컴파일한다. 경고는
// 오류다. 이 호스트가 만들 수 없는 형식은 형식마다 SKIP, 워커가 없거나 도구 저장소가 준비되지
// 않았으면 컴파일 부분이 SKIP이다(모음 번역·카탈로그 완전성 검사는 늘 돈다).
using namespace rocks;
using namespace rocks::material;
namespace material_shader_compile_tests_file {

using namespace material_graph_fixture;

namespace {

namespace shader = rocks::shader;
namespace manager_test = rocks::shader_manager_test;

// 시험 pass 셰이더 폴더(저장소 뿌리 기준)·가상 루트·엔트리다.
constexpr char PassFolder[] = "Engine/Source/Developer/MaterialCompiler/Tests/Shaders";
constexpr char PassRoot[] = "/Plugin/MaterialCompilerTests";
constexpr char PassEntry[] = "/Plugin/MaterialCompilerTests/MaterialTestPass.rsf";
constexpr char VertexEntry[] = "MaterialTestVertex";
constexpr char PixelEntry[] = "MaterialTestPixel";

// 형식 정의 전부다(엔진 셰이더 컴파일 행렬과 같은 목록 — Engine/Config/ShaderFormats).
constexpr StringView Formats[] = {"D3D12_SM6", "D3D11_SM5",      "VULKAN_SM5", "VULKAN_ANDROID",
                                  "GLES31_PC", "GLES31_ANDROID", "METAL_SM5",  "METAL_IOS"};

// 공급 표지 퍼뮤테이션이다(7.8절 — 0이면 UE 기본값 경로, 1이면 공급 경로 전부).
constexpr shader::ShaderDefine SupplyNone[] = {{"MATERIAL_TEST_SUPPLY_ALL", "0"}};
constexpr shader::ShaderDefine SupplyAll[] = {{"MATERIAL_TEST_SUPPLY_ALL", "1"}};

// 노드 모음에 들어가야 하는 최소 노드 수다(구현 노드 282개 중 에셋·표면·단계 제약으로 빠지는
// 것을 뺀 값보다 작게 둔 회귀 하한).
constexpr usize MaterialCompileCorpusMinimumNodes = 150;

// 시험 텍스처·커브 id다.
constexpr Guid CorpusBrickId{0x434f5250ull, 1};
constexpr Guid CorpusSkyId{0x434f5250ull, 2};
constexpr Guid CorpusCurveId{0x434f5250ull, 3};

MaterialTextureFacts CorpusFacts(Guid asset, IrTextureKind kind) noexcept {
    MaterialTextureFacts facts;
    facts.asset = asset;
    facts.kind = kind;
    return facts;
}

const MaterialTextureFacts CorpusTextures[] = {
    CorpusFacts(CorpusBrickId, IrTextureKind::Texture2D),
    CorpusFacts(CorpusSkyId, IrTextureKind::TextureCube)};

// ColorRamp 커브 하나를 공급하는 provider다.
class CorpusSources final : public MaterialSourceProvider {
public:
    explicit CorpusSources(const MaterialColorCurveSnapshot& curve) noexcept : curve_(curve) {
    }

    const MaterialGraphSnapshot* FindFunction(Guid) const noexcept override {
        return nullptr;
    }

    const MaterialGraphSnapshot* FindMaterial(Guid) const noexcept override {
        return nullptr;
    }

    const MaterialColorCurveSnapshot* FindColorCurve(Guid asset) const noexcept override {
        return asset == CorpusCurveId ? &curve_ : nullptr;
    }

private:
    const MaterialColorCurveSnapshot& curve_;
};

// 공급 퍼뮤테이션의 pass 함수 stub 형 이름이다(MaterialContextSourceWriter ContextTypeName과 같은
// 철자 — 자원 형은 stub이 없다).
StringView StubTypeName(MaterialPinType type) noexcept {
    switch (type) {
    case MaterialPinType::Float1:
        return "float";
    case MaterialPinType::Float2:
        return "float2";
    case MaterialPinType::Float3:
        return "float3";
    case MaterialPinType::Float4:
        return "float4";
    case MaterialPinType::Double1:
        return "MaterialDouble1";
    case MaterialPinType::Double2:
        return "MaterialDouble2";
    case MaterialPinType::Double3:
        return "MaterialDouble3";
    case MaterialPinType::Double4:
        return "MaterialDouble4";
    case MaterialPinType::Int1:
        return "int";
    case MaterialPinType::UInt1:
        return "uint";
    case MaterialPinType::Bool:
        return "bool";
    case MaterialPinType::Float3x3:
        return "float3x3";
    case MaterialPinType::Float4x4:
        return "float4x4";
    default:
        return StringView();
    }
}

// 공급 표지를 모두 켠 pass가 정의해야 하는 함수형 문맥 값(MaterialPass<이름>·이전 프레임 판)의 0
// 값 stub 매크로다(7.8절 — 시험 pass는 값을 실행하지 않는다). 문맥 값 표에서 만들어 표와 늘 맞는다.
Result<void> AppendSupplyStubs(StringBuilder<>& out) noexcept {
    out << "// Test pass supply stubs (Docs/Design/MaterialSystem 7.8).\n";
    for (const MaterialContextValue& value : MaterialContextValues()) {
        const StringView type = StubTypeName(value.type);
        if (value.form != MaterialContextValueForm::Function || type.IsEmpty()) {
            continue;
        }

        for (uint32 variant = 0; variant < (value.hasPrevious ? 2u : 1u); ++variant) {
            out << "#define MaterialPass" << (variant == 1 ? StringView("Previous") : StringView())
                << value.name << "(c";
            for (uint8 argument = 0; argument < value.argumentCount; ++argument) {
                out << ", a" << argument;
            }

            out << ") ((" << type << ")0)\n";
        }
    }

    return out.Status();
}

// 모음 항목 하나다(생성 원문은 job이 가리키므로 컴파일이 끝날 때까지 산다).
struct CorpusMaterial {
    explicit CorpusMaterial(Allocator& allocator) noexcept
        : name(allocator), source(allocator), supplied(allocator) {
    }

    String name;
    MaterialGeneratedSource source;
    // 공급 퍼뮤테이션 원문이다(pass 함수 stub + 생성 원문).
    String supplied;
    // 정점 엔트리도 컴파일하는가(손 모음).
    bool vertex = false;
};

// 모음 만들기 상태다.
struct Corpus {
    explicit Corpus(TranslatorEnv& env, const MaterialSourceProvider& sources) noexcept
        : env(env), sources(sources), materials(env.allocator), skipped(env.allocator) {
    }

    // fixture를 번역·생성해 name으로 더한다. 번역이 실패하면 false(진단에 NotYetLowered가 있으면
    // notLowered를 켠다), 생성 실패는 generationFailed를 켠다.
    bool Add(GraphFixture& fixture, StringView name, const MaterialTraitsDesc& traits,
             bool vertex) noexcept {
        MaterialTranslationInput input{fixture.document, env.catalog, env.names,
                                       traits,           TestAssetId, {}};
        input.textures = CorpusTextures;
        input.sources = &sources;
        MaterialTranslation out(env.allocator);
        if (!fixture.ready || !TranslateMaterial(env.allocator, input, out).IsOk()) {
            return false;
        }

        if (!out.succeeded) {
            for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
                if (diagnostic.code == ToDiagnosticCode(MaterialDiagnosticCode::NotYetLowered)) {
                    printf("NotYetLowered in %.*s: %.*s\n", static_cast<int>(name.ByteCount()),
                           name.Data(), static_cast<int>(diagnostic.message.View().ByteCount()),
                           diagnostic.message.View().Data());
                    notLowered = true;
                }
            }

            return false;
        }

        auto material = UniquePtr<CorpusMaterial>::TryMake(env.allocator, env.allocator);
        if (!material || !material.Value()->name.TryAssign(name)) {
            return false;
        }

        material.Value()->vertex = vertex;
        if (!WriteMaterialSource(env.allocator, out.module, traits, TestAssetId, MainGraphId,
                                 material.Value()->source)
                 .IsOk() ||
            !material.Value()->source.succeeded) {
            printf("generation failed for %.*s\n", static_cast<int>(name.ByteCount()), name.Data());
            generationFailed = true;
            return false;
        }

        if (!material.Value()->supplied.TryAssign(stubs) ||
            !material.Value()->supplied.TryAppend(material.Value()->source.text.View())) {
            return false;
        }

        return materials.TryPushBack(Move(material.Value())).IsOk();
    }

    TranslatorEnv& env;
    const MaterialSourceProvider& sources;
    // 공급 퍼뮤테이션 stub 원문이다.
    StringView stubs;
    Array<UniquePtr<CorpusMaterial>> materials;
    Array<StringView> skipped;
    bool notLowered = false;
    bool generationFailed = false;
};

// 시험 probe 파라미터 id의 상위 값이다("PROB").
constexpr uint64 ProbeIdHigh = 0x50524f42ull;

// 번역기도 하위 컴파일러도 접지 못하는 값(상수 버퍼의 스칼라 파라미터)이다. sin(상수) 같은 값은
// FXC가 접어 상수 경고(0 나누기·음수 제곱근 — -WX로 오류)를 내므로 실제 pass의 입력처럼 실행 값을
// 쓴다.
Guid AddProbe(GraphFixture& fixture) noexcept {
    ScalarParameterProperties properties;
    const uint64 serial = ++fixture.nextId;
    StringBuilder<> name(fixture.env.allocator);
    name << "Probe" << serial;
    properties.parameterName = fixture.env.Intern(name.View());
    properties.parameterId = Guid{ProbeIdHigh, serial};
    properties.defaultValue = 0.37f;
    return fixture.Add("Material.ScalarParameter", fixture.env.Save(properties));
}

// probe 값을 width 성분으로 잇는다.
Guid AddVaryingWidth(GraphFixture& fixture, uint8 width) noexcept {
    Guid value = AddProbe(fixture);
    for (uint8 component = 1; component < width; ++component) {
        const Guid append = fixture.Add("Material.Append");
        fixture.Connect(value, "Result", append, "A");
        fixture.Connect(AddProbe(fixture), "Result", append, "B");
        value = append;
    }

    return value;
}

Guid AddTextureObject(GraphFixture& fixture, Guid asset) noexcept {
    TextureObjectProperties object;
    object.texture.assetId = AssetId{asset};
    return fixture.Add("Material.TextureObject", fixture.env.Save(object));
}

// 출력 keys를 모두 받는 Custom sink를 더하고 그 결과를 AO에 낸다.
Guid AddSink(GraphFixture& fixture, Span<const StringView> keys) noexcept {
    CustomHlslProperties properties(fixture.env.allocator);
    (void)properties.code.TryAssign("return 0.0;");
    (void)properties.description.TryAssign("Sink");
    properties.outputType = CustomOutputType::Float1;
    for (usize index = 0; index < keys.Size(); ++index) {
        StringBuilder<> name(fixture.env.allocator);
        name << "O" << index;
        CustomInput input;
        input.inputName = fixture.env.Intern(name.View());
        (void)properties.inputs.TryPushBack(input);
    }

    const Guid sink = fixture.Add("Material.CustomHlsl", fixture.env.Save(properties));
    fixture.Connect(sink, "Result", OutputNodeId, "AmbientOcclusion");
    return sink;
}

// 핀 타입에 맞는 시험 값을 잇는다(잇지 못하는 타입이면 false).
bool ConnectProbe(GraphFixture& fixture, Guid node, const MaterialPinDefinition& pin) noexcept {
    switch (pin.type) {
    case MaterialPinType::Numeric:
    case MaterialPinType::Any:
    case MaterialPinType::Float1:
        fixture.Connect(AddVaryingWidth(fixture, 1), "Result", node, pin.key);
        return true;
    case MaterialPinType::Float2:
        fixture.Connect(AddVaryingWidth(fixture, 2), "Result", node, pin.key);
        return true;
    case MaterialPinType::Float3:
        fixture.Connect(AddVaryingWidth(fixture, 3), "Result", node, pin.key);
        return true;
    case MaterialPinType::Float4:
        fixture.Connect(AddVaryingWidth(fixture, 4), "Result", node, pin.key);
        return true;
    case MaterialPinType::Texture2D:
    case MaterialPinType::TextureAny:
        fixture.Connect(AddTextureObject(fixture, CorpusBrickId), "Result", node, pin.key);
        return true;
    case MaterialPinType::TextureCube:
        fixture.Connect(AddTextureObject(fixture, CorpusSkyId), "Result", node, pin.key);
        return true;
    default:
        return false;
    }
}

// 노드 하나의 모음 그래프를 만든다(allInputs면 수·텍스처 입력 전부, 아니면 필수 입력만).
bool BuildNodeGraph(GraphFixture& fixture, const MaterialNodeDefinition& definition,
                    bool allInputs) noexcept {
    const Guid node = fixture.Add(definition.type);
    StringView outputs[64];
    usize outputCount = 0;
    for (const MaterialPinDefinition& pin : definition.pins) {
        if (pin.direction == nodegraph::PinDirection::Output) {
            if (outputCount < sizeof(outputs) / sizeof(outputs[0])) {
                outputs[outputCount++] = pin.key;
            }

            continue;
        }

        if (allInputs || HasPinFlag(pin.flags, MaterialPinFlags::Required)) {
            (void)ConnectProbe(fixture, node, pin);
        }
    }

    if (outputCount == 0) {
        return false;
    }

    const Guid sink = AddSink(fixture, Span<const StringView>(outputs, outputCount));
    for (usize index = 0; index < outputCount; ++index) {
        StringBuilder<> name(fixture.env.allocator);
        name << "O" << index;
        fixture.Connect(node, outputs[index], sink, name.View());
    }

    return fixture.ready;
}

// 노드 모음이 다루지 않는 노드다(sink 자체·에셋 그래프 펼침·출력 노드는 손 모음·다른 시험이 본다).
bool IsCorpusExcluded(StringView type) noexcept {
    const StringView excluded[] = {"Material.CustomHlsl", "Material.FunctionCall",
                                   "Material.FunctionOutput", "Material.LayerOutput",
                                   "Material.MaterialSample"};
    for (StringView name : excluded) {
        if (type == name) {
            return true;
        }
    }

    return false;
}

// 손 모음: 7.7절 Math 그래프(WPO·Masked·PDO·분기)다.
void BuildVertexMath(GraphFixture& fixture) noexcept {
    const Guid wave = AddVaryingWidth(fixture, 1);
    const Guid pair = fixture.Add("Material.Append");
    fixture.Connect(wave, "Result", pair, "A");
    fixture.Connect(wave, "Result", pair, "B");
    const Guid triple = fixture.Add("Material.Append");
    fixture.Connect(pair, "Result", triple, "A");
    fixture.Connect(wave, "Result", triple, "B");
    fixture.Connect(triple, "Result", OutputNodeId, "WorldPositionOffset");
    const Guid clamp = fixture.Add("Material.Clamp");
    fixture.Connect(wave, "Result", clamp, "Input");
    fixture.Connect(clamp, "Result", OutputNodeId, "OpacityMask");
    fixture.Connect(wave, "Result", OutputNodeId, "PixelDepthOffset");
    const Guid interpolator = fixture.Add("Material.VertexInterpolator");
    fixture.Connect(triple, "Result", interpolator, "Input");
    const Guid mask = fixture.Add("Material.ComponentMask", [&]() noexcept {
        ComponentMaskProperties red;
        red.r = true;
        return fixture.env.Save(red);
    }());
    fixture.Connect(interpolator, "PS", mask, "Input");
    fixture.Connect(mask, "Result", OutputNodeId, "AmbientOcclusion");
}

// 손 모음: Custom HLSL(벡터·텍스처·Double 인자, 추가 출력, 정의 — WPO와 AO에서 정점·픽셀 두 판).
void BuildCustom(GraphFixture& fixture) noexcept {
    TranslatorEnv& env = fixture.env;
    CustomHlslProperties properties(env.allocator);
    (void)properties.code.TryAssign("E = float2(A.x, T.SampleLevel(TSampler, A.xy, 0.0).r);\n"
                                    "return A * CORPUS_SCALE + MaterialDoubleDemote(P);");
    (void)properties.description.TryAssign("Corpus");
    properties.outputType = CustomOutputType::Float3;
    const StringView inputs[] = {"A", "T", "P"};
    for (StringView name : inputs) {
        CustomInput input;
        input.inputName = env.Intern(name);
        (void)properties.inputs.TryPushBack(input);
    }

    CustomOutput extra;
    extra.outputName = env.Intern("E");
    extra.outputType = CustomOutputType::Float2;
    (void)properties.additionalOutputs.TryPushBack(extra);
    CustomDefine define(env.allocator);
    (void)define.defineName.TryAssign("CORPUS_SCALE");
    (void)define.defineValue.TryAssign("0.5");
    (void)properties.additionalDefines.TryPushBack(Move(define));
    const Guid custom = fixture.Add("Material.CustomHlsl", env.Save(properties));
    fixture.Connect(AddVaryingWidth(fixture, 3), "Result", custom, "A");
    fixture.Connect(AddTextureObject(fixture, CorpusBrickId), "Result", custom, "T");
    fixture.Connect(fixture.Add("Material.WorldPosition"), "XYZ", custom, "P");
    fixture.Connect(custom, "Result", OutputNodeId, "WorldPositionOffset");
    const Guid mask = fixture.Add("Material.ComponentMask", [&]() noexcept {
        ComponentMaskProperties red;
        red.r = true;
        return env.Save(red);
    }());
    fixture.Connect(custom, "E", mask, "Input");
    fixture.Connect(mask, "Result", OutputNodeId, "AmbientOcclusion");
}

// 손 모음: Noise ALU 함수 셋 × quality 1–4, Double 위치, VectorNoise 다섯을 sink로 모은다.
void BuildNoise(GraphFixture& fixture) noexcept {
    TranslatorEnv& env = fixture.env;
    Array<Guid> nodes(env.allocator);
    const NoiseFunction functions[] = {NoiseFunction::GradientALU, NoiseFunction::ValueALU,
                                       NoiseFunction::VoronoiALU};
    for (NoiseFunction function : functions) {
        for (int32 quality = 1; quality <= 4; ++quality) {
            NoiseProperties properties;
            properties.noiseFunction = function;
            properties.quality = quality;
            properties.tiling = quality == 2;
            const Guid noise = fixture.Add("Material.Noise", env.Save(properties));
            fixture.Connect(AddVaryingWidth(fixture, 3), "Result", noise, "Position");
            (void)nodes.TryPushBack(noise);
        }
    }

    NoiseProperties wide;
    wide.noiseFunction = NoiseFunction::GradientALU;
    (void)nodes.TryPushBack(fixture.Add("Material.Noise", env.Save(wide)));
    const VectorNoiseFunction vectors[] = {
        VectorNoiseFunction::CellnoiseALU, VectorNoiseFunction::VectorALU,
        VectorNoiseFunction::GradientALU, VectorNoiseFunction::CurlALU,
        VectorNoiseFunction::VoronoiALU};
    for (VectorNoiseFunction function : vectors) {
        VectorNoiseProperties properties;
        properties.noiseFunction = function;
        const Guid noise = fixture.Add("Material.VectorNoise", env.Save(properties));
        fixture.Connect(AddVaryingWidth(fixture, 3), "Result", noise, "Position");
        (void)nodes.TryPushBack(noise);
    }

    StringView keys[32];
    for (usize index = 0; index < nodes.Size(); ++index) {
        keys[index] = "Result";
    }

    const Guid sink = AddSink(fixture, Span<const StringView>(keys, nodes.Size()));
    for (usize index = 0; index < nodes.Size(); ++index) {
        StringBuilder<> name(env.allocator);
        name << "O" << index;
        fixture.Connect(nodes[index], "Result", sink, name.View());
    }
}

// 손 모음: ColorRamp 연결 입력(키 여섯 — float4 묶음과 나머지)과 텍스처 샘플(자기·공유 샘플러).
void BuildRampAndTextures(GraphFixture& fixture) noexcept {
    TranslatorEnv& env = fixture.env;
    ColorRampProperties ramp;
    ramp.colorCurve.assetId = AssetId{CorpusCurveId};
    const Guid curve = fixture.Add("Material.ColorRamp", env.Save(ramp));
    fixture.Connect(AddVaryingWidth(fixture, 1), "Result", curve, "Input");
    TextureSampleProperties own(env.allocator);
    own.texture.assetId = AssetId{CorpusBrickId};
    const Guid ownSample = fixture.Add("Material.TextureSample", env.Save(own));
    TextureSampleProperties shared(env.allocator);
    shared.texture.assetId = AssetId{CorpusBrickId};
    shared.samplerSource = SamplerSourceMode::WrapWorldGroupSettings;
    const Guid sharedSample = fixture.Add("Material.TextureSample", env.Save(shared));
    const StringView keys[] = {"Result", "RGBA", "RGBA"};
    const Guid sink = AddSink(fixture, keys);
    fixture.Connect(curve, "Result", sink, "O0");
    fixture.Connect(ownSample, "RGBA", sink, "O1");
    fixture.Connect(sharedSample, "RGBA", sink, "O2");
}

bool IsHostUnsupported(const shader::ShaderCompileJobResult& result) noexcept {
    return result.status == shader::ShaderCompileStatus::UnsupportedCapability ||
           result.status == shader::ShaderCompileStatus::ToolError;
}

} // namespace

TEST(MaterialShaders_CompileCorpus) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    TangentCurve r(env.allocator);
    TangentCurve g(env.allocator);
    TangentCurve b(env.allocator);
    TangentCurve a(env.allocator);
    for (usize at = 0; at < 6; ++at) {
        REQUIRE(r.TryAddKey(static_cast<float>(at) * 0.2f, static_cast<float>(at % 3) * 0.4f));
    }

    REQUIRE(g.TryAddKey(0.0f, 0.5f));
    const MaterialColorCurveSnapshot curve{&r, &g, &b, &a, ColorCurveAdjustments{}};
    const CorpusSources sources(curve);
    Corpus corpus(env, sources);
    StringBuilder<> stubs(env.allocator);
    REQUIRE(AppendSupplyStubs(stubs));
    corpus.stubs = stubs.View();

    // 노드 모음이다.
    usize nodeMaterials = 0;
    for (const Span<const MaterialNodeDefinition> group : BuiltinMaterialNodeGroups()) {
        for (const MaterialNodeDefinition& definition : group) {
            if (definition.status != MaterialNodeStatus::Implemented ||
                IsCorpusExcluded(definition.type)) {
                continue;
            }

            bool added = false;
            for (bool allInputs : {true, false}) {
                if (added) {
                    break;
                }

                GraphFixture fixture(env);
                if (BuildNodeGraph(fixture, definition, allInputs)) {
                    added = corpus.Add(fixture, definition.type, Traits(), false);
                }
            }

            if (added) {
                ++nodeMaterials;
            } else {
                REQUIRE(corpus.skipped.TryPushBack(definition.type));
            }
        }
    }

    printf("  MaterialShaders_CompileCorpus: %zu node materials, %zu nodes skipped\n",
           nodeMaterials, corpus.skipped.Size());
    for (StringView type : corpus.skipped) {
        printf("    skipped %.*s\n", static_cast<int>(type.ByteCount()), type.Data());
    }

    CHECK(!corpus.notLowered);
    CHECK(nodeMaterials >= MaterialCompileCorpusMinimumNodes);

    // 손 모음이다.
    {
        GraphFixture fixture(env);
        CHECK(corpus.Add(fixture, "DefaultSurface", Traits(), true));
    }

    {
        GraphFixture fixture(env);
        BuildVertexMath(fixture);
        CHECK(corpus.Add(fixture, "VertexMath", Traits(BlendMode::Masked), true));
    }

    {
        GraphFixture fixture(env);
        BuildCustom(fixture);
        CHECK(corpus.Add(fixture, "Custom", Traits(), true));
    }

    {
        GraphFixture fixture(env);
        BuildNoise(fixture);
        CHECK(corpus.Add(fixture, "Noise", Traits(), true));
    }

    {
        GraphFixture fixture(env);
        BuildRampAndTextures(fixture);
        CHECK(corpus.Add(fixture, "RampAndTextures", Traits(), true));
    }

    CHECK(!corpus.generationFailed);

    // 컴파일이다(워커·도구 저장소가 없으면 SKIP).
    char worker[manager_test::ManagerPathBytes];
    if (!manager_test::WorkerExecutable(worker)) {
        printf("[SKIP] MaterialShaders_CompileCorpus compile: ShaderCompileWorker is not next to "
               "the test executable\n");
        return;
    }

    shader_source_test::Fixture work;
    REQUIRE(work.Make("materialcorpus"));
    if (!manager_test::ToolsReady(worker, work)) {
        printf("[SKIP] MaterialShaders_CompileCorpus compile: shader tool store is not "
               "bootstrapped\n");
        return;
    }

    char engine[manager_test::ManagerPathBytes];
    REQUIRE(shader_source_test::RepositoryShaderDirectory(engine, sizeof(engine)));
    char passes[manager_test::ManagerPathBytes];
    {
        char working[manager_test::ManagerPathBytes];
        usize bytes = 0;
        REQUIRE(platform::GetStandardDirectory(platform::StandardDirectory::Working, working,
                                               sizeof(working), bytes));
        REQUIRE(snprintf(passes, sizeof(passes), "%s/%s", working, PassFolder) > 0);
    }

    Allocator& allocator = shader_source_test::TestAllocator();
    shader::ShaderSourceTree tree(allocator);
    REQUIRE(tree.Mount("/Engine", StringView(engine)));
    REQUIRE(tree.Mount(PassRoot, StringView(passes)));
    manager_test::KeyEnv keys;
    REQUIRE(keys.registered);

    Array<UniquePtr<shader::ShaderFormatSnapshot>> snapshots(allocator);
    for (const StringView id : Formats) {
        auto snapshot = UniquePtr<shader::ShaderFormatSnapshot>::TryMake(allocator, allocator);
        REQUIRE(snapshot);
        REQUIRE(
            shader::LoadShaderFormatSnapshot(shader::ShaderFormatDirectory, id, *snapshot.Value()));
        REQUIRE(snapshots.TryPushBack(Move(snapshot.Value())));
    }

    // job: 형식 × 머티리얼 × 엔트리(노드 모음은 픽셀만) × 공급 퍼뮤테이션이다.
    Array<shader::ShaderCompileJob> jobs(allocator);
    Array<usize> owners(allocator);
    Array<usize> formatBegins(allocator);
    for (const UniquePtr<shader::ShaderFormatSnapshot>& snapshot : snapshots) {
        REQUIRE(formatBegins.TryPushBack(jobs.Size()));
        for (usize index = 0; index < corpus.materials.Size(); ++index) {
            const CorpusMaterial& material = *corpus.materials[index];
            const Span<const shader::ShaderDefine> permutations[] = {SupplyNone, SupplyAll};
            for (usize permutation = 0; permutation < 2; ++permutation) {
                const Span<const shader::ShaderDefine> defines = permutations[permutation];
                for (bool vertex : {false, true}) {
                    if (vertex && !material.vertex) {
                        continue;
                    }

                    shader::ShaderCompileJob job;
                    job.format = snapshot.Get();
                    job.entryPath = StringView(PassEntry);
                    job.entryPoint = vertex ? StringView(VertexEntry) : StringView(PixelEntry);
                    job.stage = vertex ? shader::ShaderStageVertex : shader::ShaderStagePixel;
                    job.defines = defines;
                    job.options = shader::DefaultShaderCompileOptions(snapshot->definition);
                    job.materialSource =
                        permutation == 0 ? material.source.text.View() : material.supplied.View();
                    REQUIRE(jobs.TryPushBack(job));
                    REQUIRE(owners.TryPushBack(index));
                }
            }
        }
    }

    REQUIRE(formatBegins.TryPushBack(jobs.Size()));
    shader::ShaderCompileManagerConfig config;
    char workRoot[manager_test::ManagerPathBytes];
    REQUIRE(snprintf(workRoot, sizeof(workRoot), "%s/Work", work.Path()) > 0);
    config.allocator = &allocator;
    config.tree = &tree;
    config.reflection = keys.Reflection();
    config.workerExecutable = StringView(worker);
    config.workRoot = StringView(workRoot);
    shader::ShaderCompileManager manager(config);
    REQUIRE(manager.Start());
    Array<shader::ShaderCompileJobResult> results(allocator);
    REQUIRE(manager.Compile(jobs.View(), results));
    REQUIRE(results.Size() == jobs.Size());

    for (usize format = 0; format < snapshots.Size(); ++format) {
        const StringView id = Formats[format];
        const usize begin = formatBegins[format];
        const usize end = formatBegins[format + 1];
        if (snapshots[format]->definition.stages == 0) {
            printf("[SKIP] MaterialShaders_CompileCorpus %.*s: placeholder format without stages\n",
                   static_cast<int>(id.ByteCount()), id.Data());
            continue;
        }

        usize compiled = 0;
        usize skipped = 0;
        const char* reason = "";
        usize reported = 0;
        for (usize index = begin; index < end; ++index) {
            const shader::ShaderCompileJobResult& result = results[index];
            if (result.status == shader::ShaderCompileStatus::Success) {
                ++compiled;
                continue;
            }

            if (IsHostUnsupported(result)) {
                ++skipped;
                reason = shader::ShaderCompileStatusName(result.status);
                continue;
            }

            // 실패마다 머티리얼·엔트리·퍼뮤테이션과 진단을 찍는다(형식마다 앞 여덟 개).
            if (reported++ < 8) {
                const CorpusMaterial& material = *corpus.materials[owners[index]];
                fprintf(stderr, "%.*s %.*s %.*s %.*s: %s\n%.*s\n", static_cast<int>(id.ByteCount()),
                        id.Data(), static_cast<int>(material.name.View().ByteCount()),
                        material.name.View().Data(),
                        static_cast<int>(jobs[index].entryPoint.ByteCount()),
                        jobs[index].entryPoint.Data(),
                        static_cast<int>(jobs[index].defines[0].value.ByteCount()),
                        jobs[index].defines[0].value.Data(),
                        shader::ShaderCompileStatusName(result.status),
                        static_cast<int>(result.diagnostics.ByteCount()),
                        result.diagnostics.Data());
            }

            CHECK(result.status == shader::ShaderCompileStatus::Success);
        }

        if (skipped == end - begin) {
            printf("[SKIP] MaterialShaders_CompileCorpus %.*s: this host cannot build the format "
                   "(%s)\n",
                   static_cast<int>(id.ByteCount()), id.Data(), reason);
            continue;
        }

        // 한 형식 안에서 일부만 도구 실패면 원문·도구 문제다(전부가 아니면 SKIP이 아니다).
        for (usize index = begin; skipped != 0 && index < end; ++index) {
            if (IsHostUnsupported(results[index])) {
                const CorpusMaterial& material = *corpus.materials[owners[index]];
                fprintf(stderr, "%.*s %.*s %.*s: %s\n%.*s\n", static_cast<int>(id.ByteCount()),
                        id.Data(), static_cast<int>(material.name.View().ByteCount()),
                        material.name.View().Data(),
                        static_cast<int>(jobs[index].entryPoint.ByteCount()),
                        jobs[index].entryPoint.Data(),
                        shader::ShaderCompileStatusName(results[index].status),
                        static_cast<int>(results[index].diagnostics.ByteCount()),
                        results[index].diagnostics.Data());
            }
        }

        CHECK(skipped == 0);
        printf("  MaterialShaders_CompileCorpus %.*s: %zu jobs compiled\n",
               static_cast<int>(id.ByteCount()), id.Data(), compiled);
    }
}

} // namespace material_shader_compile_tests_file
