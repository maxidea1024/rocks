#include "../../ShaderCompiler/Tests/ShaderManagerTestSupport.h"

#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Move.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <MaterialCompiler/MaterialColorCurveSnapshot.h>
#include <MaterialCompiler/MaterialContextSource.h>
#include <MaterialCompiler/MaterialSourceProvider.h>
#include <MaterialCompiler/MaterialSourceWriter.h>
#include <MaterialSchema/MaterialContextFeature.h>
#include <MaterialSchema/MaterialContextValue.h>
#include <MaterialSchema/MaterialSchemaModule.h>
#include <MaterialSchema/Nodes/Color/ColorRampProperties.generated.h>
#include <MaterialSchema/Nodes/Color/ColorRampProperties.h>
#include <MaterialSchema/Nodes/Constants/ViewPropertyProperties.generated.h>
#include <MaterialSchema/Nodes/Constants/ViewPropertyProperties.h>
#include <MaterialSchema/Nodes/Custom/CustomHlslProperties.generated.h>
#include <MaterialSchema/Nodes/Custom/CustomHlslProperties.h>
#include <MaterialSchema/Nodes/Noise/NoiseProperties.generated.h>
#include <MaterialSchema/Nodes/Noise/NoiseProperties.h>
#include <MaterialSchema/Nodes/Noise/VectorNoiseProperties.generated.h>
#include <MaterialSchema/Nodes/Noise/VectorNoiseProperties.h>
#include <MaterialSchema/Nodes/Parameters/CurveAtlasRowParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/CurveAtlasRowParameterProperties.h>
#include <MaterialSchema/Nodes/Parameters/ScalarParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/ScalarParameterProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleProperties.h>
#include <Platform/File.h>
#include <ShaderCompiler/ShaderCompileManager.h>
#include <ShaderCompiler/ShaderCompileOptions.h>
#include <ShaderCore/ShaderType.h>
#include <stdio.h>

#include "MaterialGraphFixture.h"
#include "MaterialMathGraph.h"

// MS2 완료 시험이다(Docs/Design/MaterialSystem 7.8절): 머티리얼 모음(구현 노드마다 만드는 노드 모음
// + 손 모음)의 생성 원문을 시험 pass 셰이더(Tests/Shaders/MaterialTestPass.rsf)의 엔트리 × 공급
// 퍼뮤테이션 둘(공급 없음·공급 전부)로 형식 정의 전부에 실제 ShaderCompileWorker로 컴파일한다. 공급
// 전부는 경고를 오류로 하고, 공급 없음은 머티리얼 job 정책대로 경고를 오류로 하지 않는다
// (warningsAsErrors false — 경고는 진단). 이 호스트가 만들 수 없는 형식은 형식마다 SKIP, 워커가
// 없거나 도구 저장소가 준비되지 않았으면 컴파일 부분이 SKIP이다(모음 번역·카탈로그 완전성 검사는 늘
// 돈다).
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
// 실패한 job의 생성 원문을 쓰는 폴더다(작업 디렉터리 기준).
constexpr char FailureFolder[] = "out/MaterialCorpusFailures";

// 노드 모음에 들어가야 하는 최소 노드 수다(측정 192(2026-11-28)에서 작은 여유를 뺀 회귀 하한
// — 7.8절).
constexpr usize MaterialCompileCorpusMinimumNodes = 185;
// 형식마다 진단을 찍는 실패 job 수다.
constexpr usize FailureReportLimit = 8;
// sink 하나가 받는 출력 수 상한이다(Operator 연산 전부를 받는다).
constexpr usize MaxSinkInputs = 128;
// probe 스칼라 파라미터의 기본값이다(0·1이 아닌 값 — 특수값 접기를 피한다).
constexpr float ProbeDefaultValue = 0.37f;
// ColorRamp 시험 커브의 빨강 키 수(float4 묶음 하나 + 나머지 구간)와 키 간격·값 모양(값은 주기
// 셋으로 오르내린다)이다.
constexpr usize CorpusCurveRedKeys = 6;
constexpr float CorpusCurveKeySpacing = 0.2f;
constexpr usize CorpusCurveValuePeriod = 3;
constexpr float CorpusCurveValueStep = 0.4f;
constexpr float CorpusCurveGreenValue = 0.5f;
// 형 조사용 첫 번역의 sink 본문이다.
constexpr StringView ProvisionalSinkCode = "return 0.0;";

// Operator·ViewProperty 값 수다(MathNodeEnums·ConstantsNodeEnums의 마지막 값 + 1).
constexpr uint8 OperatorKindCount = static_cast<uint8>(OperatorKind::Smoothstep) + 1;
constexpr uint8 ExposedViewPropertyCount = static_cast<uint8>(ExposedViewProperty::NearPlane) + 1;
// 시험 입력 모양이 없는 Operator 연산이다(7.8절 — 행렬 입력, LWC 타일은 NotYetLowered).
constexpr OperatorKind OperatorKindsWithoutTestInputs[] = {
    OperatorKind::LWCTile, OperatorKind::Transpose, OperatorKind::MatrixMultiply};
// Operator 시험 노드의 입력 수다(셋째 입력이 필요한 Clamp·Lerp·Select·Smoothstep까지).
constexpr usize OperatorTestInputs = 3;
constexpr StringView OperatorInputKeys[OperatorTestInputs] = {"A", "B", "C"};

// 시험 텍스처·커브 id다.
constexpr Guid CorpusBrickId{0x434f5250ull, 1};
constexpr Guid CorpusSkyId{0x434f5250ull, 2};
constexpr Guid CorpusCurveId{0x434f5250ull, 3};
constexpr Guid CorpusAtlasId{0x434f5250ull, 4};
// atlas 행 목록이다(ColorRamp 커브가 행 1 — CurveAtlasRowParameter가 찾는다).
const Guid CorpusAtlasRows[] = {Guid{}, CorpusCurveId};
// 시험 probe 파라미터 id의 상위 값이다("PROB").
constexpr uint64 ProbeIdHigh = 0x50524f42ull;

MaterialTextureFacts CorpusFacts(Guid asset, IrTextureKind kind) noexcept {
    MaterialTextureFacts facts;
    facts.asset = asset;
    facts.kind = kind;
    return facts;
}

const MaterialTextureFacts CorpusTextures[] = {
    CorpusFacts(CorpusBrickId, IrTextureKind::Texture2D),
    CorpusFacts(CorpusSkyId, IrTextureKind::TextureCube),
    CorpusFacts(CorpusAtlasId, IrTextureKind::Texture2D)};

// ColorRamp 커브 하나와 atlas 하나를 공급하는 provider다.
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

    const MaterialCurveAtlasSnapshot* FindCurveAtlas(Guid asset) const noexcept override {
        return asset == CorpusAtlasId ? &atlas_ : nullptr;
    }

private:
    const MaterialColorCurveSnapshot& curve_;
    MaterialCurveAtlasSnapshot atlas_{CorpusAtlasRows};
};

// 공급 전부 퍼뮤테이션이 정의해야 하는 함수형 문맥 값(MaterialPass<이름>·이전 프레임 판)의 0 값
// stub 매크로다(7.8절 — 시험 pass는 값을 실행하지 않는다). 문맥 값 표에서 만들어 표와 늘 맞는다.
Result<void> AppendSupplyStubs(StringBuilder<>& out) noexcept {
    out << "// Test pass supply stubs (Docs/Design/MaterialSystem 7.8).\n";
    for (const MaterialContextValue& value : MaterialContextValues()) {
        const StringView type = MaterialContextTypeName(value.type);
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

usize CountLines(StringView text) noexcept {
    usize lines = 0;
    for (usize at = 0; at < text.ByteCount(); ++at) {
        lines += text.Data()[at] == '\n' ? 1 : 0;
    }

    return lines;
}

// 그래프 입력 값이다(7.8절 probe). 픽셀이면 보간기를 거친 픽셀 값(그래프마다 Float4 하나)에
// 입력마다 다른 스칼라 파라미터를 더하고, 아니면 파라미터만(uniform)이다. 파라미터만이면 노드
// 연산이 상수 식 프로그램으로 올라가 픽셀 셰이더에 남지 않는다(6.10절).
class ProbeSource {
public:
    ProbeSource(GraphFixture& fixture, bool pixel) noexcept : fixture_(fixture), pixel_(pixel) {
    }

    // width 성분 값이다.
    Guid Add(uint8 width) noexcept {
        const Guid uniform = AddParameters(width);
        if (!pixel_) {
            return uniform;
        }

        if (!hasBase_) {
            base_ = fixture_.Add("Material.VertexInterpolator");
            fixture_.Connect(AddParameters(4), "Result", base_, "Input");
            hasBase_ = true;
        }

        ComponentMaskProperties mask;
        mask.r = true;
        mask.g = width > 1;
        mask.b = width > 2;
        mask.a = width > 3;
        const Guid masked = fixture_.Add("Material.ComponentMask", fixture_.env.Save(mask));
        fixture_.Connect(base_, "PS", masked, "Input");
        const Guid sum = fixture_.Add("Material.Add");
        fixture_.Connect(masked, "Result", sum, "A");
        fixture_.Connect(uniform, "Result", sum, "B");
        return sum;
    }

private:
    // 스칼라 파라미터 width개를 Append로 잇는다(번역기도 하위 컴파일러도 접지 못한다).
    Guid AddParameters(uint8 width) noexcept {
        Guid value = AddParameter();
        for (uint8 component = 1; component < width; ++component) {
            const Guid append = fixture_.Add("Material.Append");
            fixture_.Connect(value, "Result", append, "A");
            fixture_.Connect(AddParameter(), "Result", append, "B");
            value = append;
        }

        return value;
    }

    Guid AddParameter() noexcept {
        ScalarParameterProperties properties;
        const uint64 serial = ++fixture_.nextId;
        StringBuilder<> name(fixture_.env.allocator);
        name << "Probe" << serial;
        properties.parameterName = fixture_.env.Intern(name.View());
        properties.parameterId = Guid{ProbeIdHigh, serial};
        properties.defaultValue = ProbeDefaultValue;
        return fixture_.Add("Material.ScalarParameter", fixture_.env.Save(properties));
    }

    GraphFixture& fixture_;
    bool pixel_;
    bool hasBase_ = false;
    Guid base_{};
};

Guid AddTextureObject(GraphFixture& fixture, Guid asset) noexcept {
    TextureObjectProperties object;
    object.texture.assetId = AssetId{asset};
    return fixture.Add("Material.TextureObject", fixture.env.Save(object));
}

// 핀 타입에 맞는 시험 값을 잇는다(잇지 못하는 타입이면 false).
bool ConnectProbe(GraphFixture& fixture, ProbeSource& probes, Guid node,
                  const MaterialPinDefinition& pin) noexcept {
    switch (pin.type) {
    case MaterialPinType::Numeric:
    case MaterialPinType::Any:
    case MaterialPinType::Float1:
        fixture.Connect(probes.Add(1), "Result", node, pin.key);
        return true;
    case MaterialPinType::Float2:
        fixture.Connect(probes.Add(2), "Result", node, pin.key);
        return true;
    case MaterialPinType::Float3:
        fixture.Connect(probes.Add(3), "Result", node, pin.key);
        return true;
    case MaterialPinType::Float4:
        fixture.Connect(probes.Add(4), "Result", node, pin.key);
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

// sink가 받는 출력 목록이다.
class SinkList {
public:
    // node의 출력 pin을 더한다. Numeric·Any는 ComponentMask R을 거치고, 행렬·표면·속성 집합·shading
    // model·정적·자원 없는 타입은 받지 않는다(Custom 인자가 될 수 없다). 받으면 true.
    bool Add(GraphFixture& fixture, Guid node, StringView pin, MaterialPinType type) noexcept {
        switch (type) {
        case MaterialPinType::Numeric:
        case MaterialPinType::Any: {
            ComponentMaskProperties red;
            red.r = true;
            const Guid mask = fixture.Add("Material.ComponentMask", fixture.env.Save(red));
            fixture.Connect(node, pin, mask, "Input");
            return Push(mask, "Result");
        }
        case MaterialPinType::Float1:
        case MaterialPinType::Float2:
        case MaterialPinType::Float3:
        case MaterialPinType::Float4:
        case MaterialPinType::Int1:
        case MaterialPinType::Int2:
        case MaterialPinType::Int3:
        case MaterialPinType::Int4:
        case MaterialPinType::UInt1:
        case MaterialPinType::UInt2:
        case MaterialPinType::UInt3:
        case MaterialPinType::UInt4:
        case MaterialPinType::Bool:
        case MaterialPinType::Double1:
        case MaterialPinType::Double2:
        case MaterialPinType::Double3:
        case MaterialPinType::Double4:
        case MaterialPinType::Texture2D:
        case MaterialPinType::Texture2DArray:
        case MaterialPinType::TextureCube:
        case MaterialPinType::TextureCubeArray:
        case MaterialPinType::Texture3D:
            return Push(node, pin);
        default:
            return false;
        }
    }

    // Custom sink를 더해 목록을 모두 잇고 결과를 AO(픽셀) 또는 WPO(정점)에 낸다.
    Guid Connect(GraphFixture& fixture, StringView code, bool vertex) const noexcept {
        CustomHlslProperties properties(fixture.env.allocator);
        (void)properties.code.TryAssign(code);
        (void)properties.description.TryAssign("Sink");
        properties.outputType = vertex ? CustomOutputType::Float3 : CustomOutputType::Float1;
        for (usize index = 0; index < count_; ++index) {
            StringBuilder<> name(fixture.env.allocator);
            name << "O" << index;
            CustomInput input;
            input.inputName = fixture.env.Intern(name.View());
            (void)properties.inputs.TryPushBack(input);
        }

        const Guid sink = fixture.Add("Material.CustomHlsl", fixture.env.Save(properties));
        for (usize index = 0; index < count_; ++index) {
            StringBuilder<> name(fixture.env.allocator);
            name << "O" << index;
            fixture.Connect(items_[index].node, items_[index].pin, sink, name.View());
        }

        fixture.Connect(sink, "Result", OutputNodeId,
                        vertex ? StringView("WorldPositionOffset")
                               : StringView("AmbientOcclusion"));
        return sink;
    }

    usize Count() const noexcept {
        return count_;
    }

    // 목록이 상한을 넘었는가(넘은 출력은 버렸다 — 시험 구성 오류).
    bool Overflowed() const noexcept {
        return overflowed_;
    }

private:
    struct Item {
        Guid node;
        StringView pin;
    };

    bool Push(Guid node, StringView pin) noexcept {
        if (count_ == MaxSinkInputs) {
            overflowed_ = true;
            return false;
        }

        items_[count_++] = Item{node, pin};
        return true;
    }

    Item items_[MaxSinkInputs]{};
    usize count_ = 0;
    bool overflowed_ = false;
};

// sink 본문을 번역된 인자 형으로 만든다(7.8절 — 인자를 모두 읽어 하위 컴파일러가 계산을 지우지
// 않게): 수는 첫 성분(정수·Bool은 Custom이 float로 넘긴다), Double은 낮춘 첫 성분, 텍스처는 가운데
// 샘플링이다.
Result<void> AppendSinkCode(const IrCustomTable& table, const IrCustomFunction& function,
                            StringBuilder<>& out) noexcept {
    out << "float total = 0.0;\n";
    for (const IrCustomItem& argument : table.Arguments(function)) {
        const StringView name = table.Text(argument.name);
        const IrType type = argument.type;
        out << "total += ";
        if (type.IsTexture()) {
            StringView coordinate;
            switch (type.texture) {
            case IrTextureKind::Texture2D:
                coordinate = "float2(0.5, 0.5)";
                break;
            case IrTextureKind::TextureCube:
            case IrTextureKind::Texture2DArray:
            case IrTextureKind::Texture3D:
                coordinate = "float3(0.5, 0.5, 0.5)";
                break;
            case IrTextureKind::TextureCubeArray:
                coordinate = "float4(0.5, 0.5, 0.5, 0.0)";
                break;
            default:
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            out << name << ".SampleLevel(" << name << "Sampler, " << coordinate << ", 0.0).r";
        } else if (type.scalar == IrScalar::Double) {
            out << "MaterialDoubleDemote(" << name << ")" << (type.components > 1 ? ".x" : "");
        } else {
            out << name << (type.components > 1 ? ".x" : "");
        }

        out << ";\n";
    }

    // 반환 형은 sink의 출력 형이다(픽셀 Float1 → AO, 정점 Float3 → WPO).
    out << (function.returnType.components == 3 ? "return total.xxx;" : "return total;");
    return out.Status();
}

// 모음 그래프 만들기다. sink 본문 code로 그래프를 만들고 sink가 있으면 그 id를 sink에 쓴다(같은
// 만들기는 같은 id를 낸다 — fixture id는 순서대로다).
class CorpusRecipe {
public:
    virtual bool Build(GraphFixture& fixture, StringView code, Guid& sink,
                       bool& hasSink) const noexcept = 0;

protected:
    ~CorpusRecipe() = default;
};

// 모음 항목 하나다(생성 원문은 job이 가리키므로 컴파일이 끝날 때까지 산다).
struct CorpusMaterial {
    explicit CorpusMaterial(Allocator& allocator) noexcept
        : name(allocator), source(allocator), supplied(allocator) {
    }

    String name;
    MaterialGeneratedSource source;
    // 공급 전부 퍼뮤테이션 원문이다(pass 함수 stub + 생성 원문).
    String supplied;
    // 정점 엔트리도 컴파일하는가(손 모음·정점 판 노드).
    bool vertex = false;
};

// 모음에 더한 결과다.
enum class CorpusOutcome : uint8 {
    Added,
    // 이미 더한 같은 원문이다(필수 입력만 이은 판이 전부 이은 판과 같을 때).
    Duplicate,
    // 번역 진단으로 실패했다(firstDiagnostic).
    NotTranslated,
    // 번역기 하드 실패·생성 실패·할당 실패다(시험 실패).
    Broken
};

// 모음 만들기 상태다.
struct Corpus {
    Corpus(TranslatorEnv& env, const MaterialSourceProvider& sources) noexcept
        : env(env), sources(sources), materials(env.allocator), firstDiagnostic(env.allocator) {
    }

    // 번역한다. 진단으로 실패하면 false(첫 진단을 firstDiagnostic에, NotYetLowered면 notLowered),
    // 번역기 하드 실패면 false와 broken이다.
    bool Translate(GraphFixture& fixture, const MaterialTraitsDesc& traits, StringView name,
                   MaterialTranslation& out) noexcept {
        MaterialTranslationInput input{fixture.document, env.catalog, env.names,
                                       traits,           TestAssetId, {}};
        input.textures = CorpusTextures;
        input.sources = &sources;
        firstDiagnostic.Clear();
        if (!fixture.ready || !TranslateMaterial(env.allocator, input, out).IsOk()) {
            printf("translator hard failure in %.*s\n", static_cast<int>(name.ByteCount()),
                   name.Data());
            broken = true;
            return false;
        }

        if (out.succeeded) {
            return true;
        }

        for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
            if (firstDiagnostic.IsEmpty()) {
                (void)firstDiagnostic.TryAssign(diagnostic.message.View());
            }

            if (diagnostic.code == ToDiagnosticCode(MaterialDiagnosticCode::NotYetLowered)) {
                printf("NotYetLowered in %.*s: %.*s\n", static_cast<int>(name.ByteCount()),
                       name.Data(), static_cast<int>(diagnostic.message.View().ByteCount()),
                       diagnostic.message.View().Data());
                notLowered = true;
            }
        }

        return false;
    }

    // recipe를 번역·생성해 name으로 더한다. sink가 있으면 임시 본문으로 한 번 번역해 인자 형을 읽고
    // 그 형의 본문으로 다시 만든다. sameAs와 원문이 같으면 Duplicate다.
    CorpusOutcome Add(const CorpusRecipe& recipe, StringView name, const MaterialTraitsDesc& traits,
                      bool vertex, const CorpusMaterial* sameAs) noexcept {
        StringBuilder<> code(env.allocator);
        {
            GraphFixture probe(env);
            Guid sink{};
            bool hasSink = false;
            // 만들지 못하면(sink가 읽을 출력이 없거나 핀이 없다) 번역하지 않은 것으로 센다.
            if (!recipe.Build(probe, ProvisionalSinkCode, sink, hasSink)) {
                (void)firstDiagnostic.TryAssign("test graph has no output the sink can read");
                return CorpusOutcome::NotTranslated;
            }

            MaterialTranslation out(env.allocator);
            if (!Translate(probe, traits, name, out)) {
                return broken ? CorpusOutcome::Broken : CorpusOutcome::NotTranslated;
            }

            const IrCustomTable& table = out.module.CustomFunctions();
            for (const IrCustomFunction& function : table.Functions()) {
                if (hasSink && function.node == sink && code.View().IsEmpty()) {
                    if (!AppendSinkCode(table, function, code)) {
                        broken = true;
                        return CorpusOutcome::Broken;
                    }
                }
            }

            // sink 함수가 없으면 본문을 형으로 만들 수 없다(임시 본문은 계산을 지운다 — 7.8절).
            if (hasSink && code.View().IsEmpty()) {
                printf("sink function missing in %.*s\n", static_cast<int>(name.ByteCount()),
                       name.Data());
                broken = true;
                return CorpusOutcome::Broken;
            }
        }

        GraphFixture fixture(env);
        Guid sink{};
        bool hasSink = false;
        const StringView body = code.View();
        MaterialTranslation out(env.allocator);
        if (!recipe.Build(fixture, body, sink, hasSink) || !Translate(fixture, traits, name, out)) {
            // 첫 번역이 된 그래프는 본문만 다르므로 두 번째 실패는 시험 구성 오류다.
            broken = true;
            return CorpusOutcome::Broken;
        }

        auto material = UniquePtr<CorpusMaterial>::TryMake(env.allocator, env.allocator);
        if (!material || !material.Value()->name.TryAssign(name)) {
            broken = true;
            return CorpusOutcome::Broken;
        }

        CorpusMaterial& item = *material.Value();
        item.vertex = vertex;
        if (!WriteMaterialSource(env.allocator, out.module, traits, TestAssetId, MainGraphId,
                                 item.source)
                 .IsOk() ||
            !item.source.succeeded) {
            printf("generation failed for %.*s\n", static_cast<int>(name.ByteCount()), name.Data());
            for (const nodegraph::GraphDiagnostic& diagnostic : item.source.diagnostics.items) {
                printf("    %.*s\n", static_cast<int>(diagnostic.message.View().ByteCount()),
                       diagnostic.message.View().Data());
            }

            broken = true;
            return CorpusOutcome::Broken;
        }

        if (sameAs != nullptr && sameAs->source.text.View() == item.source.text.View()) {
            return CorpusOutcome::Duplicate;
        }

        if (!item.supplied.TryAssign(stubs) || !item.supplied.TryAppend(item.source.text.View()) ||
            !materials.TryPushBack(Move(material.Value()))) {
            broken = true;
            return CorpusOutcome::Broken;
        }

        return CorpusOutcome::Added;
    }

    TranslatorEnv& env;
    const MaterialSourceProvider& sources;
    // 공급 전부 퍼뮤테이션 stub 원문이다.
    StringView stubs;
    Array<UniquePtr<CorpusMaterial>> materials;
    // 마지막 번역 실패의 첫 진단이다.
    String firstDiagnostic;
    bool notLowered = false;
    bool broken = false;
};

// 노드 하나의 모음 그래프다(allInputs면 수·텍스처 입력 전부, 아니면 필수 입력만; vertex면 sink를
// WPO에 잇고 uniform probe).
class NodeRecipe final : public CorpusRecipe {
public:
    NodeRecipe(const MaterialNodeDefinition& definition, bool allInputs, bool vertex) noexcept
        : definition_(definition), allInputs_(allInputs), vertex_(vertex) {
    }

    bool Build(GraphFixture& fixture, StringView code, Guid& sink,
               bool& hasSink) const noexcept override {
        ProbeSource probes(fixture, !vertex_);
        const Guid node = fixture.Add(definition_.type);
        SinkList outputs;
        for (const MaterialPinDefinition& pin : definition_.pins) {
            if (pin.direction == nodegraph::PinDirection::Output) {
                (void)outputs.Add(fixture, node, pin.key, pin.type);
                continue;
            }

            if (allInputs_ || HasPinFlag(pin.flags, MaterialPinFlags::Required)) {
                (void)ConnectProbe(fixture, probes, node, pin);
            }
        }

        if (outputs.Count() == 0 || outputs.Overflowed()) {
            return false;
        }

        sink = outputs.Connect(fixture, code, vertex_);
        hasSink = true;
        return fixture.ready;
    }

private:
    const MaterialNodeDefinition& definition_;
    bool allInputs_;
    bool vertex_;
};

// 노드 모음이 다루지 않는 노드다(sink 자체·에셋 그래프 펼침은 손 모음·다른 시험이 본다).
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

// 함수 하나로 만드는 손 모음 그래프다.
class HandRecipe final : public CorpusRecipe {
public:
    using BuildFunction = bool (*)(GraphFixture&, StringView, Guid&, bool&, const void*) noexcept;

    HandRecipe(BuildFunction build, const void* state = nullptr) noexcept
        : build_(build), state_(state) {
    }

    bool Build(GraphFixture& fixture, StringView code, Guid& sink,
               bool& hasSink) const noexcept override {
        return build_(fixture, code, sink, hasSink, state_) && fixture.ready;
    }

private:
    BuildFunction build_;
    const void* state_;
};

bool BuildDefaultSurface(GraphFixture&, StringView, Guid&, bool&, const void*) noexcept {
    return true;
}

// 7.7절 Math 골든 그래프(골든 시험과 같은 모양)다. 입력은 TexCoord 0 R + probe다(정점·픽셀 둘 다
// 실행 값 — uniform이면 cos·Append가 상수 식 프로그램으로 올라가고, WPO가 있어 보간기는 못 쓴다).
bool BuildMathGolden(GraphFixture& fixture, StringView, Guid&, bool&, const void*) noexcept {
    ProbeSource uniform(fixture, false);
    ComponentMaskProperties red;
    red.r = true;
    const Guid coordinate = fixture.Add("Material.ComponentMask", fixture.env.Save(red));
    fixture.Connect(fixture.Add("Material.TextureCoordinate"), "Result", coordinate, "Input");
    const Guid wave = fixture.Add("Material.Add");
    fixture.Connect(coordinate, "Result", wave, "A");
    fixture.Connect(uniform.Add(1), "Result", wave, "B");
    BuildMathGraph(fixture, wave);
    return true;
}

// 정점 결과: WPO(이전 프레임 판도)·customized UV 0(픽셀 TexCoord 0이 읽는다)·보간기다.
bool BuildVertexResults(GraphFixture& fixture, StringView, Guid&, bool&, const void*) noexcept {
    ProbeSource uniform(fixture, false);
    const Guid offset = fixture.Add("Material.Add");
    fixture.Connect(uniform.Add(3), "Result", offset, "A");
    fixture.Connect(fixture.Add("Material.WorldPosition"), "XYZ", offset, "B");
    fixture.Connect(offset, "Result", OutputNodeId, "WorldPositionOffset");
    fixture.Connect(uniform.Add(2), "Result", OutputNodeId, "CustomizedUv0");
    ComponentMaskProperties red;
    red.r = true;
    const Guid uv = fixture.Add("Material.ComponentMask", fixture.env.Save(red));
    fixture.Connect(fixture.Add("Material.TextureCoordinate"), "Result", uv, "Input");
    fixture.Connect(uv, "Result", OutputNodeId, "PixelDepthOffset");
    const Guid interpolator = fixture.Add("Material.VertexInterpolator");
    fixture.Connect(offset, "Result", interpolator, "Input");
    const Guid mask = fixture.Add("Material.ComponentMask", fixture.env.Save(red));
    fixture.Connect(interpolator, "PS", mask, "Input");
    fixture.Connect(mask, "Result", OutputNodeId, "AmbientOcclusion");
    return true;
}

// Custom HLSL(벡터·텍스처·Double 인자, 추가 출력, 정의 — WPO와 AO에서 정점·픽셀 두 판).
bool BuildCustom(GraphFixture& fixture, StringView, Guid&, bool&, const void*) noexcept {
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
    // 정점 판에서 쓰므로 uniform probe에 WorldPosition(정점 값)을 더한다.
    ProbeSource uniform(fixture, false);
    fixture.Connect(uniform.Add(3), "Result", custom, "A");
    fixture.Connect(AddTextureObject(fixture, CorpusBrickId), "Result", custom, "T");
    fixture.Connect(fixture.Add("Material.WorldPosition"), "XYZ", custom, "P");
    fixture.Connect(custom, "Result", OutputNodeId, "WorldPositionOffset");
    ComponentMaskProperties red;
    red.r = true;
    const Guid mask = fixture.Add("Material.ComponentMask", env.Save(red));
    fixture.Connect(custom, "E", mask, "Input");
    fixture.Connect(mask, "Result", OutputNodeId, "AmbientOcclusion");
    return true;
}

// Noise ALU 함수 셋 × quality 1–4, Double 위치(입력 없음 — 절대 world 위치), VectorNoise 다섯이다.
bool BuildNoise(GraphFixture& fixture, StringView code, Guid& sink, bool& hasSink,
                const void*) noexcept {
    TranslatorEnv& env = fixture.env;
    ProbeSource probes(fixture, true);
    SinkList outputs;
    const NoiseFunction functions[] = {NoiseFunction::GradientALU, NoiseFunction::ValueALU,
                                       NoiseFunction::VoronoiALU};
    for (NoiseFunction function : functions) {
        for (int32 quality = 1; quality <= 4; ++quality) {
            NoiseProperties properties;
            properties.noiseFunction = function;
            properties.quality = quality;
            properties.tiling = quality == 2;
            const Guid noise = fixture.Add("Material.Noise", env.Save(properties));
            fixture.Connect(probes.Add(3), "Result", noise, "Position");
            (void)outputs.Add(fixture, noise, "Result", MaterialPinType::Float1);
        }
    }

    NoiseProperties wide;
    wide.noiseFunction = NoiseFunction::GradientALU;
    (void)outputs.Add(fixture, fixture.Add("Material.Noise", env.Save(wide)), "Result",
                      MaterialPinType::Float1);
    const VectorNoiseFunction vectors[] = {
        VectorNoiseFunction::CellnoiseALU, VectorNoiseFunction::VectorALU,
        VectorNoiseFunction::GradientALU, VectorNoiseFunction::CurlALU,
        VectorNoiseFunction::VoronoiALU};
    for (VectorNoiseFunction function : vectors) {
        VectorNoiseProperties properties;
        properties.noiseFunction = function;
        const Guid noise = fixture.Add("Material.VectorNoise", env.Save(properties));
        fixture.Connect(probes.Add(3), "Result", noise, "Position");
        (void)outputs.Add(fixture, noise, "Result", MaterialPinType::Numeric);
    }

    sink = outputs.Connect(fixture, code, false);
    hasSink = true;
    return !outputs.Overflowed();
}

// ColorRamp 연결 입력(커브 공급)과 텍스처 샘플(자기·공유 샘플러)이다.
bool BuildRampAndTextures(GraphFixture& fixture, StringView code, Guid& sink, bool& hasSink,
                          const void*) noexcept {
    TranslatorEnv& env = fixture.env;
    ProbeSource probes(fixture, true);
    ColorRampProperties ramp;
    ramp.colorCurve.assetId = AssetId{CorpusCurveId};
    const Guid curve = fixture.Add("Material.ColorRamp", env.Save(ramp));
    fixture.Connect(probes.Add(1), "Result", curve, "Input");
    TextureSampleProperties own(env.allocator);
    own.texture.assetId = AssetId{CorpusBrickId};
    const Guid ownSample = fixture.Add("Material.TextureSample", env.Save(own));
    fixture.Connect(probes.Add(2), "Result", ownSample, "Coordinates");
    TextureSampleProperties shared(env.allocator);
    shared.texture.assetId = AssetId{CorpusBrickId};
    shared.samplerSource = SamplerSourceMode::WrapWorldGroupSettings;
    const Guid sharedSample = fixture.Add("Material.TextureSample", env.Save(shared));
    SinkList outputs;
    (void)outputs.Add(fixture, curve, "Result", MaterialPinType::Float4);
    (void)outputs.Add(fixture, ownSample, "RGBA", MaterialPinType::Float4);
    (void)outputs.Add(fixture, sharedSample, "RGBA", MaterialPinType::Float4);
    sink = outputs.Connect(fixture, code, false);
    hasSink = true;
    return true;
}

// CurveAtlasRowParameter(행 파라미터·실행 값 시간 — 공유 Clamp 샘플)의 출력 전부다.
bool BuildCurveAtlasRow(GraphFixture& fixture, StringView code, Guid& sink, bool& hasSink,
                        const void*) noexcept {
    ProbeSource probes(fixture, true);
    CurveAtlasRowParameterProperties properties;
    properties.parameterName = fixture.env.Intern("AtlasRow");
    properties.parameterId = Guid{ProbeIdHigh, ++fixture.nextId};
    properties.curve.assetId = AssetId{CorpusCurveId};
    properties.atlas.assetId = AssetId{CorpusAtlasId};
    const Guid row = fixture.Add("Material.CurveAtlasRowParameter", fixture.env.Save(properties));
    fixture.Connect(probes.Add(1), "Result", row, "InputTime");
    SinkList outputs;
    (void)outputs.Add(fixture, row, "Result", MaterialPinType::Float3);
    const StringView channels[] = {"R", "G", "B", "A"};
    for (StringView key : channels) {
        (void)outputs.Add(fixture, row, key, MaterialPinType::Float1);
    }

    sink = outputs.Connect(fixture, code, false);
    hasSink = true;
    return true;
}

// Operator 연산 하나의 입력 모양이다(번역되는 첫 모양을 고른다).
enum class OperatorInputs : uint8 {
    // 실수 Float3 셋.
    Float = 0,
    // 부호 없는 정수 스칼라 셋(비트 연산).
    UInt = 1,
    // Bool 셋(논리 연산 — 실행 값 비교 결과).
    Bool = 2,
    // 번역되는 모양이 없다.
    None = 3
};

// Operator 그래프 상태다(연산마다 입력 모양 — 하나만 시험하면 only).
struct OperatorPlan {
    OperatorInputs inputs[OperatorKindCount]{};
    bool single = false;
    uint8 only = 0;
};

Guid AddOperator(GraphFixture& fixture, ProbeSource& probes, OperatorKind kind,
                 OperatorInputs inputs) noexcept {
    OperatorProperties properties(fixture.env.allocator);
    properties.operatorKind = kind;
    for (usize index = 0; index < OperatorTestInputs; ++index) {
        (void)properties.dynamicInputs.TryPushBack(OperatorInput{});
    }

    const Guid node = fixture.Add("Material.Operator", fixture.env.Save(properties));
    for (StringView key : OperatorInputKeys) {
        if (inputs == OperatorInputs::UInt) {
            const Guid toUInt = fixture.Add("Material.FloatToUInt");
            fixture.Connect(probes.Add(1), "Result", toUInt, "Input");
            fixture.Connect(toUInt, "Result", node, key);
        } else if (inputs == OperatorInputs::Bool) {
            OperatorProperties less(fixture.env.allocator);
            less.operatorKind = OperatorKind::LessThan;
            (void)less.dynamicInputs.TryPushBack(OperatorInput{});
            (void)less.dynamicInputs.TryPushBack(OperatorInput{});
            const Guid test = fixture.Add("Material.Operator", fixture.env.Save(less));
            fixture.Connect(probes.Add(1), "Result", test, "A");
            fixture.Connect(probes.Add(1), "Result", test, "B");
            fixture.Connect(test, "Result", node, key);
        } else {
            fixture.Connect(probes.Add(3), "Result", node, key);
        }
    }

    return node;
}

// Operator 연산 전부(번역되는 모양이 있는 것)를 sink 하나로 모은다.
bool BuildOperators(GraphFixture& fixture, StringView code, Guid& sink, bool& hasSink,
                    const void* state) noexcept {
    const OperatorPlan& plan = *static_cast<const OperatorPlan*>(state);
    ProbeSource probes(fixture, true);
    SinkList outputs;
    for (uint8 kind = 0; kind < OperatorKindCount; ++kind) {
        if ((plan.single && kind != plan.only) || plan.inputs[kind] == OperatorInputs::None) {
            continue;
        }

        const Guid node =
            AddOperator(fixture, probes, static_cast<OperatorKind>(kind), plan.inputs[kind]);
        (void)outputs.Add(fixture, node, "Result", MaterialPinType::Numeric);
    }

    sink = outputs.Connect(fixture, code, false);
    hasSink = true;
    return outputs.Count() > 0 && !outputs.Overflowed();
}

// ViewProperty 속성 전부의 Property·InvProperty다.
bool BuildViewProperties(GraphFixture& fixture, StringView code, Guid& sink, bool& hasSink,
                         const void*) noexcept {
    SinkList outputs;
    for (uint8 property = 0; property < ExposedViewPropertyCount; ++property) {
        ViewPropertyProperties properties;
        properties.property = static_cast<ExposedViewProperty>(property);
        const Guid node = fixture.Add("Material.ViewProperty", fixture.env.Save(properties));
        (void)outputs.Add(fixture, node, "Property", MaterialPinType::Numeric);
        (void)outputs.Add(fixture, node, "InvProperty", MaterialPinType::Numeric);
    }

    sink = outputs.Connect(fixture, code, false);
    hasSink = true;
    return !outputs.Overflowed();
}

// custom 출력 BentNormal(시험 pass가 EvaluateMaterialCustomOutput_BentNormal0을 부른다)이다.
bool BuildBentNormal(GraphFixture& fixture, StringView, Guid&, bool&, const void*) noexcept {
    ProbeSource probes(fixture, true);
    const Guid output = fixture.Add("Material.BentNormalOutput");
    fixture.Connect(probes.Add(3), "Result", output, "Input");
    return true;
}

bool IsHostUnsupported(const shader::ShaderCompileJobResult& result) noexcept {
    return result.status == shader::ShaderCompileStatus::UnsupportedCapability;
}

bool IsOperatorWithoutTestInputs(uint8 kind) noexcept {
    for (OperatorKind item : OperatorKindsWithoutTestInputs) {
        if (static_cast<uint8>(item) == kind) {
            return true;
        }
    }

    return false;
}

// 공급 퍼뮤테이션이다(7.8절).
enum class SupplyPermutation : uint8 {
    // 공급 표지 없음(UE 기본값 경로 — 경고 허용).
    None = 0,
    // 공급 표지 전부(경고는 오류).
    All = 1
};

StringView PermutationName(SupplyPermutation permutation) noexcept {
    return permutation == SupplyPermutation::All ? StringView("supply-all")
                                                 : StringView("supply-none");
}

// 실패 원문 폴더 경로다(작업 디렉터리 기준 — 넘치면 false).
bool FailureFolderPath(StringView working, char (&folder)[manager_test::ManagerPathBytes],
                       usize& bytes) noexcept {
    const int written =
        snprintf(folder, sizeof(folder), "%.*s/%s", static_cast<int>(working.ByteCount()),
                 working.Data(), FailureFolder);
    if (written <= 0 || static_cast<usize>(written) >= sizeof(folder)) {
        return false;
    }

    bytes = static_cast<usize>(written);
    return true;
}

// 전 실행의 실패 원문을 지운다(없으면 그대로).
void ClearFailureFolder(StringView working) noexcept {
    char folder[manager_test::ManagerPathBytes];
    usize bytes = 0;
    if (!FailureFolderPath(working, folder, bytes)) {
        return;
    }

    const platform::FileStatus removed =
        platform::DeleteDirectoryTree(platform::PhysicalFileSystem(), folder, bytes);
    if (!removed && removed.code != platform::FileCode::NotFound) {
        fprintf(stderr, "    cannot clear %s\n", folder);
    }
}

// 실패 job의 원문을 <working>/out/MaterialCorpusFailures/<job>.rsh에 쓰고 경로를 찍는다.
void DumpFailure(StringView working, usize job, StringView text) noexcept {
    char folder[manager_test::ManagerPathBytes];
    char path[manager_test::ManagerPathBytes];
    usize folderBytes = 0;
    const bool named = FailureFolderPath(working, folder, folderBytes);
    const int pathBytes = named ? snprintf(path, sizeof(path), "%s/%zu.rsh", folder, job) : 0;
    if (!named || pathBytes <= 0 || static_cast<usize>(pathBytes) >= sizeof(path)) {
        fprintf(stderr, "    source not written: path too long\n");
        return;
    }

    platform::File file;
    if (!platform::MakeDirectoryTree(platform::PhysicalFileSystem(), folder, folderBytes) ||
        !file.Open(path, platform::FileOpenMode::CreateAlways)) {
        fprintf(stderr, "    source not written: cannot create %s\n", path);
        return;
    }

    const platform::FileTransfer written = file.WriteAll(text.Data(), text.ByteCount());
    const platform::FileStatus closed = file.Close();
    if (written.status && closed) {
        fprintf(stderr, "    source: %s\n", path);
    } else {
        fprintf(stderr, "    source not written: write failed %s\n", path);
    }
}

} // namespace

TEST(MaterialShaders_CompileCorpus) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    TangentCurve r(env.allocator);
    TangentCurve g(env.allocator);
    TangentCurve b(env.allocator);
    TangentCurve a(env.allocator);
    for (usize at = 0; at < CorpusCurveRedKeys; ++at) {
        REQUIRE(
            r.TryAddKey(static_cast<float>(at) * CorpusCurveKeySpacing,
                        static_cast<float>(at % CorpusCurveValuePeriod) * CorpusCurveValueStep));
    }

    REQUIRE(g.TryAddKey(0.0f, CorpusCurveGreenValue));
    const MaterialColorCurveSnapshot curve{&r, &g, &b, &a, ColorCurveAdjustments{}};
    const CorpusSources sources(curve);
    Corpus corpus(env, sources);
    StringBuilder<> stubs(env.allocator);
    REQUIRE(AppendSupplyStubs(stubs));
    corpus.stubs = stubs.View();
    const usize stubLines = CountLines(stubs.View());

    // 노드 모음이다: 전부 이은 판·필수만 이은 판, 픽셀에서 번역되지 않으면 정점 판.
    usize nodeMaterials = 0;
    usize skippedNodes = 0;
    for (const Span<const MaterialNodeDefinition> group : BuiltinMaterialNodeGroups()) {
        for (const MaterialNodeDefinition& definition : group) {
            if (definition.status != MaterialNodeStatus::Implemented ||
                IsCorpusExcluded(definition.type)) {
                continue;
            }

            bool included = false;
            const CorpusMaterial* first = nullptr;
            // 첫 시도(전부 이은 픽셀 판)의 진단이다.
            String reason(env.allocator);
            for (bool allInputs : {true, false}) {
                for (bool vertex : {false, true}) {
                    StringBuilder<> name(env.allocator);
                    name << definition.type << (allInputs ? " all" : " required")
                         << (vertex ? " vertex" : "");
                    const NodeRecipe recipe(definition, allInputs, vertex);
                    const CorpusOutcome outcome =
                        corpus.Add(recipe, name.View(), Traits(), vertex, first);
                    if (outcome == CorpusOutcome::Added) {
                        first = corpus.materials[corpus.materials.Size() - 1].Get();
                    } else if (reason.IsEmpty()) {
                        (void)reason.TryAssign(corpus.firstDiagnostic.View());
                    }

                    if (outcome == CorpusOutcome::Added || outcome == CorpusOutcome::Duplicate) {
                        included = true;
                        break;
                    }
                }
            }

            if (included) {
                ++nodeMaterials;
            } else {
                ++skippedNodes;
                printf("    skipped %.*s: %.*s\n", static_cast<int>(definition.type.ByteCount()),
                       definition.type.Data(), static_cast<int>(reason.View().ByteCount()),
                       reason.View().Data());
            }
        }
    }

    printf("  MaterialShaders_CompileCorpus: %zu nodes in %zu materials, %zu nodes skipped\n",
           nodeMaterials, corpus.materials.Size(), skippedNodes);
    CHECK(!corpus.notLowered);
    CHECK(nodeMaterials >= MaterialCompileCorpusMinimumNodes);

    // Operator 연산마다 번역되는 입력 모양을 고른다(혼자 번역해 본다). 번역되지 않는 연산은 기대
    // 목록과 같아야 하고, NotYetLowered는 LWC 타일만이다.
    OperatorPlan operators;
    for (uint8 kind = 0; kind < OperatorKindCount; ++kind) {
        operators.inputs[kind] = OperatorInputs::None;
        corpus.notLowered = false;
        for (OperatorInputs inputs :
             {OperatorInputs::Float, OperatorInputs::UInt, OperatorInputs::Bool}) {
            OperatorPlan single;
            single.single = true;
            single.only = kind;
            single.inputs[kind] = inputs;
            GraphFixture fixture(env);
            Guid sink{};
            bool hasSink = false;
            MaterialTranslation out(env.allocator);
            if (BuildOperators(fixture, ProvisionalSinkCode, sink, hasSink, &single) &&
                corpus.Translate(fixture, Traits(), "Operator", out)) {
                operators.inputs[kind] = inputs;
                break;
            }
        }

        if (operators.inputs[kind] == OperatorInputs::None) {
            printf("    operator kind %u not compiled: %.*s\n", static_cast<uint32>(kind),
                   static_cast<int>(corpus.firstDiagnostic.View().ByteCount()),
                   corpus.firstDiagnostic.View().Data());
        }

        CHECK((operators.inputs[kind] == OperatorInputs::None) ==
              IsOperatorWithoutTestInputs(kind));
        CHECK(!corpus.notLowered || static_cast<OperatorKind>(kind) == OperatorKind::LWCTile);
    }

    corpus.notLowered = false;

    // 손 모음이다(두 엔트리).
    MaterialTraitsDesc customizedUv = Traits();
    customizedUv.customizedUvCount = 1;
    struct HandItem {
        StringView name;
        HandRecipe recipe;
        MaterialTraitsDesc traits;
    };

    const HandItem hand[] = {
        {"DefaultSurface", HandRecipe(&BuildDefaultSurface), Traits()},
        {"MathGolden", HandRecipe(&BuildMathGolden), Traits(BlendMode::Masked)},
        {"VertexResults", HandRecipe(&BuildVertexResults), customizedUv},
        {"Custom", HandRecipe(&BuildCustom), Traits()},
        {"Noise", HandRecipe(&BuildNoise), Traits()},
        {"RampAndTextures", HandRecipe(&BuildRampAndTextures), Traits()},
        {"Operators", HandRecipe(&BuildOperators, &operators), Traits()},
        {"ViewProperties", HandRecipe(&BuildViewProperties), Traits()},
        {"CurveAtlasRow", HandRecipe(&BuildCurveAtlasRow), Traits()},
        {"BentNormal", HandRecipe(&BuildBentNormal), Traits()}};
    for (const HandItem& item : hand) {
        const CorpusOutcome outcome =
            corpus.Add(item.recipe, item.name, item.traits, true, nullptr);
        if (outcome != CorpusOutcome::Added) {
            printf("    hand material %.*s failed: %.*s\n", static_cast<int>(item.name.ByteCount()),
                   item.name.Data(), static_cast<int>(corpus.firstDiagnostic.View().ByteCount()),
                   corpus.firstDiagnostic.View().Data());
        }

        CHECK(outcome == CorpusOutcome::Added);
    }

    CHECK(!corpus.notLowered);
    CHECK(!corpus.broken);

    // 공급 전부 퍼뮤테이션 정의다(기능 표에서 — 표와 늘 맞는다).
    Array<String> supplyNames(env.allocator);
    Array<shader::ShaderDefine> supplyAll(env.allocator);
    for (uint32 feature = 0; feature < MaterialContextFeatureCount; ++feature) {
        String define(env.allocator);
        REQUIRE(define.TryAssign("MATERIAL_SUPPLY_"));
        REQUIRE(define.TryAppend(
            MaterialContextFeatureDefineName(static_cast<MaterialContextFeature>(feature))));
        REQUIRE(supplyNames.TryPushBack(Move(define)));
    }

    for (const String& define : supplyNames) {
        REQUIRE(supplyAll.TryPushBack(shader::ShaderDefine{define.View(), "1"}));
    }

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
    char working[manager_test::ManagerPathBytes];
    usize workingBytes = 0;
    REQUIRE(platform::GetStandardDirectory(platform::StandardDirectory::Working, working,
                                           sizeof(working), workingBytes));
    char passes[manager_test::ManagerPathBytes];
    {
        const int bytes = snprintf(passes, sizeof(passes), "%s/%s", working, PassFolder);
        REQUIRE(bytes > 0 && static_cast<usize>(bytes) < sizeof(passes));
    }

    Allocator& allocator = shader_source_test::TestAllocator();
    shader::ShaderSourceTree tree(allocator);
    REQUIRE(tree.Mount("/Engine", StringView(engine)));
    REQUIRE(tree.Mount(PassRoot, StringView(passes)));
    manager_test::KeyEnv keys;
    REQUIRE(keys.registered);

    Array<UniquePtr<shader::ShaderFormatSnapshot>> snapshots(allocator);
    for (const StringView id : manager_test::ShaderFormatIds) {
        auto snapshot = UniquePtr<shader::ShaderFormatSnapshot>::TryMake(allocator, allocator);
        REQUIRE(snapshot);
        REQUIRE(
            shader::LoadShaderFormatSnapshot(shader::ShaderFormatDirectory, id, *snapshot.Value()));
        REQUIRE(snapshots.TryPushBack(Move(snapshot.Value())));
    }

    // job: 형식 × 머티리얼 × 엔트리(노드 모음 픽셀 판은 픽셀만) × 공급 퍼뮤테이션이다.
    Array<shader::ShaderCompileJob> jobs(allocator);
    Array<usize> owners(allocator);
    Array<SupplyPermutation> permutations(allocator);
    Array<usize> formatBegins(allocator);
    for (const UniquePtr<shader::ShaderFormatSnapshot>& snapshot : snapshots) {
        REQUIRE(formatBegins.TryPushBack(jobs.Size()));
        for (usize index = 0; index < corpus.materials.Size(); ++index) {
            const CorpusMaterial& material = *corpus.materials[index];
            for (SupplyPermutation permutation :
                 {SupplyPermutation::None, SupplyPermutation::All}) {
                const bool all = permutation == SupplyPermutation::All;
                for (bool vertex : {false, true}) {
                    if (vertex && !material.vertex) {
                        continue;
                    }

                    shader::ShaderCompileJob job;
                    job.format = snapshot.Get();
                    job.entryPath = StringView(PassEntry);
                    job.entryPoint = vertex ? StringView(VertexEntry) : StringView(PixelEntry);
                    job.stage = vertex ? shader::ShaderStageVertex : shader::ShaderStagePixel;
                    job.defines = all ? supplyAll.View() : Span<const shader::ShaderDefine>();
                    job.options = shader::DefaultShaderCompileOptions(snapshot->definition);
                    // 공급 없음은 머티리얼 job 정책대로 경고를 오류로 하지 않는다(MD15 0 기본값의
                    // 상수 접기 — 7.8절 판정). 공급 전부는 기본(경고는 오류 — 생성기 경고를
                    // 잡는다).
                    job.options.warningsAsErrors = all;
                    job.materialSource =
                        all ? material.supplied.View() : material.source.text.View();
                    REQUIRE(jobs.TryPushBack(job));
                    REQUIRE(owners.TryPushBack(index));
                    REQUIRE(permutations.TryPushBack(permutation));
                }
            }
        }
    }

    REQUIRE(formatBegins.TryPushBack(jobs.Size()));
    shader::ShaderCompileManagerConfig config;
    char workRoot[manager_test::ManagerPathBytes];
    {
        const int bytes = snprintf(workRoot, sizeof(workRoot), "%s/Work", work.Path());
        REQUIRE(bytes > 0 && static_cast<usize>(bytes) < sizeof(workRoot));
    }

    config.allocator = &allocator;
    config.tree = &tree;
    config.reflection = keys.Reflection();
    config.workerExecutable = StringView(worker);
    config.workRoot = StringView(workRoot);
    shader::ShaderCompileManager manager(config);
    const StringView workingView(working, workingBytes);
    ClearFailureFolder(workingView);
    REQUIRE(manager.Start());
    Array<shader::ShaderCompileJobResult> results(allocator);
    REQUIRE(manager.Compile(jobs.View(), results));
    REQUIRE(results.Size() == jobs.Size());

    for (usize format = 0; format < snapshots.Size(); ++format) {
        const StringView id = manager_test::ShaderFormatIds[format];
        const usize begin = formatBegins[format];
        const usize end = formatBegins[format + 1];
        if (snapshots[format]->definition.stages == 0) {
            printf("[SKIP] MaterialShaders_CompileCorpus %.*s: placeholder format without stages\n",
                   static_cast<int>(id.ByteCount()), id.Data());
            continue;
        }

        usize compiled = 0;
        usize skipped = 0;
        usize warned = 0;
        const char* reason = "";
        usize reported = 0;
        for (usize index = begin; index < end; ++index) {
            const shader::ShaderCompileJobResult& result = results[index];
            if (result.status == shader::ShaderCompileStatus::Success) {
                ++compiled;
                // 경고는 진단으로 남는다(공급 없음 — 캐시 적중 job은 비어 있다).
                warned += result.diagnostics.IsEmpty() ? 0 : 1;
                continue;
            }

            if (IsHostUnsupported(result)) {
                ++skipped;
                reason = shader::ShaderCompileStatusName(result.status);
                continue;
            }

            // 실패마다 머티리얼·엔트리·퍼뮤테이션과 진단을 찍고 원문을 쓴다(형식마다 앞 몇 개).
            if (reported++ < FailureReportLimit) {
                const CorpusMaterial& material = *corpus.materials[owners[index]];
                const bool all = permutations[index] == SupplyPermutation::All;
                const StringView permutation = PermutationName(permutations[index]);
                fprintf(
                    stderr, "%.*s %.*s %.*s %.*s: %s (stub lines %zu)\n%.*s\n",
                    static_cast<int>(id.ByteCount()), id.Data(),
                    static_cast<int>(material.name.View().ByteCount()), material.name.View().Data(),
                    static_cast<int>(jobs[index].entryPoint.ByteCount()),
                    jobs[index].entryPoint.Data(), static_cast<int>(permutation.ByteCount()),
                    permutation.Data(), shader::ShaderCompileStatusName(result.status),
                    all ? stubLines : usize{0}, static_cast<int>(result.diagnostics.ByteCount()),
                    result.diagnostics.Data());
                DumpFailure(workingView, index,
                            all ? material.supplied.View() : material.source.text.View());
            }

            CHECK(result.status == shader::ShaderCompileStatus::Success);
        }

        if (skipped == end - begin) {
            printf("[SKIP] MaterialShaders_CompileCorpus %.*s: this host cannot build the format "
                   "(%s)\n",
                   static_cast<int>(id.ByteCount()), id.Data(), reason);
            continue;
        }

        // 한 형식 안에서 일부만 지원하지 않으면 원문·도구 문제다(전부가 아니면 SKIP이 아니다).
        for (usize index = begin; skipped != 0 && index < end; ++index) {
            const shader::ShaderCompileJobResult& result = results[index];
            if (IsHostUnsupported(result)) {
                const CorpusMaterial& material = *corpus.materials[owners[index]];
                fprintf(
                    stderr, "%.*s %.*s %.*s: %s\n%.*s\n", static_cast<int>(id.ByteCount()),
                    id.Data(), static_cast<int>(material.name.View().ByteCount()),
                    material.name.View().Data(),
                    static_cast<int>(jobs[index].entryPoint.ByteCount()),
                    jobs[index].entryPoint.Data(), shader::ShaderCompileStatusName(result.status),
                    static_cast<int>(result.diagnostics.ByteCount()), result.diagnostics.Data());
            }
        }

        CHECK(skipped == 0);
        printf("  MaterialShaders_CompileCorpus %.*s: %zu jobs compiled, %zu with warnings\n",
               static_cast<int>(id.ByteCount()), id.Data(), compiled, warned);
    }
}

} // namespace material_shader_compile_tests_file
