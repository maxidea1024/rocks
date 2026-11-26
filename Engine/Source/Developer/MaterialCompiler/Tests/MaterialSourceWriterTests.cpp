#include <Core/FileRead.h>
#include <Core/Text/String.h>
#include <MaterialCompiler/MaterialContextSource.h>
#include <MaterialCompiler/MaterialSourceWriter.h>
#include <MaterialSchema/Nodes/Constants/ConstantDoubleProperties.generated.h>
#include <MaterialSchema/Nodes/Constants/ConstantDoubleProperties.h>
#include <MaterialSchema/Nodes/Constants/TimeProperties.generated.h>
#include <MaterialSchema/Nodes/Constants/TimeProperties.h>
#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.generated.h>
#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.h>
#include <MaterialSchema/Nodes/Math/CosineProperties.generated.h>
#include <MaterialSchema/Nodes/Math/CosineProperties.h>
#include <MaterialSchema/Nodes/Math/OperatorProperties.generated.h>
#include <MaterialSchema/Nodes/Math/OperatorProperties.h>
#include <MaterialSchema/Nodes/Parameters/DoubleVectorParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/DoubleVectorParameterProperties.h>
#include <MaterialSchema/Nodes/Parameters/ScalarParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/ScalarParameterProperties.h>
#include <MaterialSchema/Nodes/Parameters/VectorParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/VectorParameterProperties.h>

#include "MaterialGraphFixture.h"

// 생성 원문 시험이다(Docs/Design/MaterialSystem 7.3·7.7절, 단계 MS2a3, MS-T1·MS-T2·MS-T3): fixture
// 그래프를 번역·생성한 원문을 저장된 기대 원문(Tests/Golden)과 byte로 견주고, 같은 그래프·저장
// 순서만 다른 그래프·재경유를 더한 그래프가 같은 byte인지, 원문 위치 지도가 값 줄과 원인 노드를
// 가리키는지, 생성할 수 없는 값(Double)이 진단되는지 본다. 기대 원문이 다르면 실패 메시지에 실제
// 원문을 남긴다 (생성 규칙을 바꾸면 기대 원문을 함께 고친다).
using namespace rocks;
using namespace rocks::material;
namespace material_source_writer_tests_file {

using namespace material_graph_fixture;

namespace {

// 기대 원문 파일의 최대 크기다.
constexpr uint64 MaxGoldenFileBytes = 1024u * 1024u;

// 이 소스 파일 옆 Golden 폴더의 name 경로다(시험은 소스 트리에서 돈다 — __FILE__ 기준).
bool GoldenPath(StringView name, String& out) noexcept {
    const StringView source(__FILE__);
    usize cut = source.ByteCount();
    while (cut > 0 && source.Data()[cut - 1] != '/' && source.Data()[cut - 1] != '\\') {
        --cut;
    }

    return cut > 0 && out.TryAssign(StringView(source.Data(), cut)) && out.TryAppend("Golden/") &&
           out.TryAppend(name);
}

// 저장소 뿌리 기준 relative 경로다(이 파일은 Engine/Source/Developer/MaterialCompiler/Tests에
// 있다 — 파일 이름과 디렉터리 다섯을 걷어 낸다).
bool SourceTreePath(StringView relative, String& out) noexcept {
    constexpr uint32 LevelsBelowRoot = 6;
    const StringView source(__FILE__);
    usize cut = source.ByteCount();
    for (uint32 level = 0; level < LevelsBelowRoot && cut > 0; ++level) {
        --cut;
        while (cut > 0 && source.Data()[cut - 1] != '/' && source.Data()[cut - 1] != '\\') {
            --cut;
        }
    }

    // __FILE__가 상대 경로면(뿌리에서 컴파일) 앞부분이 비고 작업 디렉터리 기준이 된다.
    return out.TryAssign(StringView(source.Data(), cut)) && out.TryAppend(relative);
}

// text가 path 파일과 byte로 같은가. 다르면 실제 원문을 실패로 남긴다.
bool MatchesFile(Allocator& allocator, bool located, StringView path, StringView text) noexcept {
    Array<uint8> bytes(allocator);
    bool existed = false;
    const bool read =
        located &&
        ReadWholeFile(platform::PhysicalFileSystem(), path, MaxGoldenFileBytes, bytes, existed)
            .IsOk() &&
        existed;
    const bool same =
        read && StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size()) == text;
    if (!same) {
        // 실패 문구는 NUL로 끝나는 C 문자열이다.
        constexpr StringView Prefix =
            "generated source differs from the golden file; actual text:\n";
        const StringView parts[] = {Prefix, text};
        Array<char> message(allocator);
        bool built = true;
        for (StringView part : parts) {
            for (usize index = 0; built && index < part.ByteCount(); ++index) {
                built = message.TryPushBack(part.Data()[index]).IsOk();
            }
        }

        if (built && message.TryPushBack('\0')) {
            test::RecordFailure(message.Data(), __FILE__, __LINE__);
        }
    }

    return same;
}

// text가 기대 원문 name(Golden 폴더)과 byte로 같은가.
bool MatchesGolden(Allocator& allocator, StringView name, StringView text) noexcept {
    String path(allocator);
    const bool located = GoldenPath(name, path);
    return MatchesFile(allocator, located, path.View(), text);
}

// text가 저장소 파일 relative와 byte로 같은가.
bool MatchesSourceTreeFile(Allocator& allocator, StringView relative, StringView text) noexcept {
    String path(allocator);
    const bool located = SourceTreePath(relative, path);
    return MatchesFile(allocator, located, path.View(), text);
}

struct Generated {
    explicit Generated(Allocator& allocator) noexcept : translation(allocator), source(allocator) {
    }

    MaterialTranslation translation;
    MaterialGeneratedSource source;
};

// fixture를 번역하고 원문을 만든다. 번역·생성이 모두 성공하면 true다.
bool Generate(GraphFixture& fixture, const MaterialTraitsDesc& traits, Generated& out) noexcept {
    if (!fixture.ready || !Translate(fixture, traits, out.translation) ||
        !out.translation.succeeded) {
        return false;
    }

    return WriteMaterialSource(fixture.env.allocator, out.translation.module, traits, TestAssetId,
                               MainGraphId, out.source)
               .IsOk() &&
           out.source.succeeded;
}

bool Contains(StringView text, StringView part) noexcept {
    if (part.ByteCount() > text.ByteCount()) {
        return false;
    }

    for (usize at = 0; at + part.ByteCount() <= text.ByteCount(); ++at) {
        if (StringView(text.Data() + at, part.ByteCount()) == part) {
            return true;
        }
    }

    return false;
}

// 여러 연산 철자를 지나는 그래프다(7.7절):
// sin·cos·Append·ComponentMask·Power(Select)·If·DynamicBranch
// (Branch)·Clamp·ArcsineFast·FloatToUInt(Convert)·Operator 비교.
void BuildMathGraph(GraphFixture& fixture) noexcept {
    TranslatorEnv& env = fixture.env;
    const Guid wave = AddVarying(fixture, 0.5f);
    CosineProperties flat;
    flat.period = 0.0f;
    const Guid cosine = fixture.Add("Material.Cosine", env.Save(flat));
    fixture.Connect(wave, "Result", cosine, "Input");

    // WPO = float3(x, x, cos(x)).
    const Guid pair = fixture.Add("Material.Append");
    fixture.Connect(wave, "Result", pair, "A");
    fixture.Connect(wave, "Result", pair, "B");
    const Guid triple = fixture.Add("Material.Append");
    fixture.Connect(pair, "Result", triple, "A");
    fixture.Connect(cosine, "Result", triple, "B");
    fixture.Connect(triple, "Result", OutputNodeId, "WorldPositionOffset");

    // OpacityMask = clamp(asinFast(pow(triple.z)), 0, 1).
    ComponentMaskProperties blue;
    blue.b = true;
    const Guid component = fixture.Add("Material.ComponentMask", env.Save(blue));
    fixture.Connect(triple, "Result", component, "Input");
    const Guid power = fixture.Add("Material.Power");
    fixture.Connect(component, "Result", power, "Base");
    const Guid arcsine = fixture.Add("Material.ArcsineFast");
    fixture.Connect(power, "Result", arcsine, "Input");
    const Guid clamp = fixture.Add("Material.Clamp");
    fixture.Connect(arcsine, "Result", clamp, "Input");
    fixture.Connect(clamp, "Result", OutputNodeId, "OpacityMask");

    // AO = if(x, 0 → 0.25, 0.5, 0.75).
    const Guid choice = fixture.Add("Material.If");
    fixture.Connect(wave, "Result", choice, "A");
    fixture.Connect(AddConstant(fixture, 0.25f), "Result", choice, "AGreaterThanB");
    fixture.Connect(AddConstant(fixture, 0.75f), "Result", choice, "ALessThanB");
    fixture.Connect(AddConstant(fixture, 0.5f), "Result", choice, "AEqualsB");
    fixture.Connect(choice, "Result", OutputNodeId, "AmbientOcclusion");

    // PDO = branch(x < 1, float(uint(trunc(x))), 2).
    OperatorProperties less(env.allocator);
    less.operatorKind = OperatorKind::LessThan;
    (void)less.dynamicInputs.TryPushBack(OperatorInput{});
    (void)less.dynamicInputs.TryPushBack(OperatorInput{});
    const Guid test = fixture.Add("Material.Operator", env.Save(less));
    fixture.Connect(wave, "Result", test, "A");
    const Guid toUInt = fixture.Add("Material.FloatToUInt");
    fixture.Connect(wave, "Result", toUInt, "Input");
    const Guid toFloat = fixture.Add("Material.UIntToFloat");
    fixture.Connect(toUInt, "Result", toFloat, "Input");
    const Guid branch = fixture.Add("Material.DynamicBranch");
    fixture.Connect(test, "Result", branch, "Condition");
    fixture.Connect(toFloat, "Result", branch, "True");
    fixture.Connect(AddConstant(fixture, 2.0f), "Result", branch, "False");
    fixture.Connect(branch, "Result", OutputNodeId, "PixelDepthOffset");
}

} // namespace

// MS-T3: 출력 노드만 있는 Surface·Opaque의 생성 원문(정의·기본값 함수).
TEST(MaterialSourceWriter_DefaultSurfaceGolden) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    Generated generated(env.allocator);
    REQUIRE(Generate(fixture, Traits(), generated));
    CHECK(MatchesGolden(env.allocator, "MaterialSource_DefaultSurface.txt",
                        generated.source.text.View()));
    CHECK(generated.source.locations.IsEmpty());
}

// MS-T3: 여러 연산 철자를 지나는 Masked 그래프의 생성 원문.
TEST(MaterialSourceWriter_MathGolden) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    BuildMathGraph(fixture);
    Generated generated(env.allocator);
    REQUIRE(Generate(fixture, Traits(BlendMode::Masked), generated));
    CHECK(MatchesGolden(env.allocator, "MaterialSource_Math.txt", generated.source.text.View()));
    CHECK(Contains(generated.source.text.View(), "#define MATERIAL_USES_WPO 1\n"));
    CHECK(Contains(generated.source.text.View(), "#define MATERIAL_USES_OPACITY_MASK 1\n"));
}

// MS-T1 (a)(b)·MS-T2(생성 원문): 같은 그래프 두 번, 노드 저장 순서만 섞은 그래프, 재경유를 더한
// 그래프가 같은 byte다.
TEST(MaterialSourceWriter_SameBytes) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture first(env);
    BuildMathGraph(first);
    Generated a(env.allocator);
    Generated b(env.allocator);
    REQUIRE(Generate(first, Traits(BlendMode::Masked), a));
    REQUIRE(Generate(first, Traits(BlendMode::Masked), b));
    CHECK(a.source.text.View() == b.source.text.View());

    // 노드·간선 저장 순서를 뒤집는다.
    GraphFixture shuffled(env);
    BuildMathGraph(shuffled);
    nodegraph::Graph& graph = shuffled.Main();
    for (usize low = 0, high = graph.nodes.Size() - 1; low < high; ++low, --high) {
        nodegraph::GraphNode swap(Move(graph.nodes[low]));
        graph.nodes[low] = Move(graph.nodes[high]);
        graph.nodes[high] = Move(swap);
    }

    for (usize low = 0, high = graph.edges.Size() - 1; low < high; ++low, --high) {
        const nodegraph::GraphEdge swap = graph.edges[low];
        graph.edges[low] = graph.edges[high];
        graph.edges[high] = swap;
    }

    Generated reordered(env.allocator);
    REQUIRE(Generate(shuffled, Traits(BlendMode::Masked), reordered));
    CHECK(a.source.text.View() == reordered.source.text.View());

    // 출력 간선 하나 사이에 재경유를 넣는다(화면 편집 — 값을 그대로 넘긴다).
    GraphFixture rerouted(env);
    BuildMathGraph(rerouted);
    nodegraph::Graph& routed = rerouted.Main();
    const Guid reroute = rerouted.Add(nodegraph::RerouteNodeTypeName);
    for (nodegraph::GraphEdge& edge : routed.edges) {
        if (edge.toNode == OutputNodeId && edge.toPin == env.Intern("AmbientOcclusion")) {
            const Guid source = edge.fromNode;
            const Name pin = edge.fromPin;
            edge.fromNode = reroute;
            edge.fromPin = env.Intern(nodegraph::ReroutePinOut);
            rerouted.Connect(source, "Result", reroute, nodegraph::ReroutePinIn);
            CHECK(pin == env.Intern("Result"));
            break;
        }
    }

    Generated withReroute(env.allocator);
    REQUIRE(Generate(rerouted, Traits(BlendMode::Masked), withReroute));
    CHECK(a.source.text.View() == withReroute.source.text.View());
}

// 원문 위치 지도(7.6·7.7절): 값 줄마다 하나이고, 그 줄은 m<번호> 선언이며, 원인 노드가 문서 노드다.
TEST(MaterialSourceWriter_SourceLocations) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    BuildMathGraph(fixture);
    Generated generated(env.allocator);
    REQUIRE(Generate(fixture, Traits(BlendMode::Masked), generated));
    const StringView text = generated.source.text.View();
    REQUIRE(!generated.source.locations.IsEmpty());
    uint32 previous = 0;
    for (const MaterialSourceLocation& location : generated.source.locations) {
        CHECK(location.line > previous);
        previous = location.line;
        CHECK(fixture.Find(location.node) != nullptr);

        // location.line 줄의 시작을 찾는다.
        uint32 line = 1;
        usize at = 0;
        while (at < text.ByteCount() && line < location.line) {
            if (text.Data()[at] == '\n') {
                ++line;
            }

            ++at;
        }

        const StringView rest(text.Data() + at, text.ByteCount() - at);
        const bool declaration =
            Contains(StringView(rest.Data(), rest.ByteCount() < 64 ? rest.ByteCount() : 64), " m");
        CHECK(declaration);
    }
}

// 기본 속성 정의: 영역·블렌드 모드는 해당하는 것만 1, 마스크 기준값은 float 리터럴이다.
TEST(MaterialSourceWriter_Definitions) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    MaterialTraitsDesc traits = Traits(BlendMode::AlphaComposite, MaterialDomain::UserInterface);
    traits.twoSided = true;
    traits.opacityMaskClipValue = 0.5f;
    Generated generated(env.allocator);
    REQUIRE(Generate(fixture, traits, generated));
    const StringView text = generated.source.text.View();
    CHECK(Contains(text, "#define MATERIAL_DOMAIN_USER_INTERFACE 1\n"));
    CHECK(Contains(text, "#define MATERIAL_DOMAIN_SURFACE 0\n"));
    CHECK(Contains(text, "#define MATERIAL_BLEND_ALPHA_COMPOSITE 1\n"));
    CHECK(Contains(text, "#define MATERIAL_BLEND_OPAQUE 0\n"));
    CHECK(Contains(text, "#define MATERIAL_TWO_SIDED 1\n"));
    CHECK(Contains(text, "#define MATERIAL_USES_WPO 0\n"));
    CHECK(Contains(text, "#define MATERIAL_OPACITY_MASK_CLIP 0.5\n"));
    // UI·AlphaComposite는 Opacity를 쓴다(기본 1.0).
    CHECK(
        Contains(text, "float EvaluateMaterialOpacity(in MaterialInputs c) {\n    return 1.0;\n}"));
}

// Double 값은 LWC 표현(tile + offset — 6.9절)으로 생성된다. LWC 표현이 없는 연산(UE IR 전용
// WorldPositionTile)이 결과에 닿으면 원인 노드에 NotYetLowered 하나이고 원문이 없다.
TEST(MaterialSourceWriter_DoubleValues) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    ConstantDoubleProperties precise;
    precise.value = 2.5;
    const Guid wide = fixture.Add("Material.ConstantDouble", env.Save(precise));
    const Guid sum = fixture.Add("Material.Add");
    fixture.Connect(wide, "Result", sum, "A");
    fixture.Connect(AddVarying(fixture, 0.5f), "Result", sum, "B");
    fixture.Connect(sum, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation translation(env.allocator);
    REQUIRE(Translate(fixture, Traits(), translation));
    REQUIRE(translation.succeeded);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(WriteMaterialSource(env.allocator, translation.module, Traits(), TestAssetId,
                                MainGraphId, source));
    REQUIRE(source.succeeded);
    CHECK(Contains(source.text.View(), "MaterialDoubleAdd(MakeMaterialDouble1(0.0, 2.5), "));
    CHECK(Contains(source.text.View(), "MaterialDoublePromote("));
    CHECK(Contains(source.text.View(), "MaterialDoubleDemote("));

    IrBuilder builder(env.allocator);
    const Guid site{NodeIdHigh, 77};
    builder.SetSite(IrSite{site, StringView()});
    const double components[] = {1.0};
    auto constant = builder.DoubleConstant(Span<const double>(components, 1));
    REQUIRE(constant);
    auto tile = builder.Unary(IrOp::WorldPositionTile, constant.Value());
    REQUIRE(tile);
    const IrResult results[] = {{IrEntry::Surface, "AmbientOcclusion", tile.Value()}};
    IrModule module(env.allocator);
    REQUIRE(BuildIrModule(builder, results, module));
    MaterialGeneratedSource rejected(env.allocator);
    REQUIRE(
        WriteMaterialSource(env.allocator, module, Traits(), TestAssetId, MainGraphId, rejected));
    CHECK(!rejected.succeeded);
    CHECK(rejected.text.IsEmpty());
    REQUIRE(rejected.diagnostics.items.Size() == 1);
    CHECK(rejected.diagnostics.items[0].code ==
          ToDiagnosticCode(MaterialDiagnosticCode::NotYetLowered));
    CHECK(rejected.diagnostics.items[0].node == site);
}

// 연산 철자(7.7절): 비트 검사·논리 매크로·부정·역수·sign 감싸기·빠른 역삼각·역쌍곡·uint 변환·-0.0
// 리터럴·MaterialSelect가 원문에 그대로 나온다.
TEST(MaterialSourceWriter_OperatorSpellings) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid wave = AddVarying(fixture, 0.5f);
    const auto unary = [&](OperatorKind kind, Guid input) noexcept {
        OperatorProperties properties(env.allocator);
        properties.operatorKind = kind;
        (void)properties.dynamicInputs.TryPushBack(OperatorInput{});
        const Guid node = fixture.Add("Material.Operator", env.Save(properties));
        fixture.Connect(input, "Result", node, "A");
        return node;
    };
    const auto binary = [&](OperatorKind kind, Guid left, Guid right) noexcept {
        OperatorProperties properties(env.allocator);
        properties.operatorKind = kind;
        (void)properties.dynamicInputs.TryPushBack(OperatorInput{});
        (void)properties.dynamicInputs.TryPushBack(OperatorInput{});
        const Guid node = fixture.Add("Material.Operator", env.Save(properties));
        fixture.Connect(left, "Result", node, "A");
        fixture.Connect(right, "Result", node, "B");
        return node;
    };

    const Guid nan = unary(OperatorKind::IsNan, wave);
    const Guid notNan = unary(OperatorKind::Not, nan);
    const Guid below = binary(OperatorKind::LessThan, wave, AddConstant(fixture, 1.0f));
    const Guid both = binary(OperatorKind::And, notNan, below);
    const Guid either = binary(OperatorKind::Or, both, nan);
    OperatorProperties select(env.allocator);
    select.operatorKind = OperatorKind::Select;
    for (usize index = 0; index < 3; ++index) {
        (void)select.dynamicInputs.TryPushBack(OperatorInput{});
    }

    const Guid chosen = fixture.Add("Material.Operator", env.Save(select));
    fixture.Connect(either, "Result", chosen, "A");
    fixture.Connect(unary(OperatorKind::Reciprocal, wave), "Result", chosen, "B");
    fixture.Connect(unary(OperatorKind::Sign, wave), "Result", chosen, "C");

    OperatorProperties sum(env.allocator);
    sum.operatorKind = OperatorKind::Add;
    for (usize index = 0; index < 6; ++index) {
        (void)sum.dynamicInputs.TryPushBack(OperatorInput{});
    }

    const Guid total = fixture.Add("Material.Operator", env.Save(sum));
    const Guid toUInt = fixture.Add("Material.FloatToUInt");
    fixture.Connect(wave, "Result", toUInt, "Input");
    const Guid toFloat = fixture.Add("Material.UIntToFloat");
    fixture.Connect(toUInt, "Result", toFloat, "Input");
    const Guid sources[] = {chosen,
                            unary(OperatorKind::ACosFast, wave),
                            unary(OperatorKind::ATanFast, wave),
                            unary(OperatorKind::ACosh, wave),
                            toFloat,
                            AddConstant(fixture, -0.0f)};
    const StringView keys[] = {"A", "B", "C", "D", "E", "F"};
    for (usize index = 0; index < 6; ++index) {
        fixture.Connect(sources[index], "Result", total, keys[index]);
    }

    fixture.Connect(total, "Result", OutputNodeId, "AmbientOcclusion");
    Generated generated(env.allocator);
    REQUIRE(Generate(fixture, Traits(), generated));
    const StringView text = generated.source.text.View();
    const StringView expected[] = {"MaterialIsNan(m",
                                   "(!m",
                                   "MaterialAnd(m",
                                   "MaterialOr(m",
                                   "(1.0 / m",
                                   "float(sign(m",
                                   "MaterialSelect(m",
                                   "MaterialAcosFast(m",
                                   "MaterialAtanFast(m",
                                   "MaterialAcosh(m",
                                   "uint(m",
                                   "float(m",
                                   " + -0.0)",
                                   "trunc(m"};
    for (StringView part : expected) {
        CHECK(Contains(text, part));
    }
}

// 원문 위치 지도도 노드·간선 저장 순서와 무관하다(같은 값을 여러 노드가 만들면 가장 작은 NodeId
// — 7.7절).
TEST(MaterialSourceWriter_LocationsAreDeterministic) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture first(env);
    BuildMathGraph(first);
    GraphFixture second(env);
    BuildMathGraph(second);
    nodegraph::Graph& graph = second.Main();
    for (usize low = 0, high = graph.edges.Size() - 1; low < high; ++low, --high) {
        const nodegraph::GraphEdge swap = graph.edges[low];
        graph.edges[low] = graph.edges[high];
        graph.edges[high] = swap;
    }

    Generated a(env.allocator);
    Generated b(env.allocator);
    REQUIRE(Generate(first, Traits(BlendMode::Masked), a));
    REQUIRE(Generate(second, Traits(BlendMode::Masked), b));
    REQUIRE(a.source.locations.Size() == b.source.locations.Size());
    for (usize index = 0; index < a.source.locations.Size(); ++index) {
        CHECK(a.source.locations[index].line == b.source.locations[index].line);
        CHECK(a.source.locations[index].node == b.source.locations[index].node);
    }

    // sin 줄의 원인은 Sine 노드다.
    const StringView text = a.source.text.View();
    for (const MaterialSourceLocation& location : a.source.locations) {
        uint32 line = 1;
        usize at = 0;
        while (at < text.ByteCount() && line < location.line) {
            line += text.Data()[at] == '\n' ? 1 : 0;
            ++at;
        }

        const usize length = text.ByteCount() - at < 40 ? text.ByteCount() - at : 40;
        if (Contains(StringView(text.Data() + at, length), "= sin(")) {
            const nodegraph::GraphNode* node = first.Find(location.node);
            REQUIRE(node != nullptr);
            CHECK(node->type == env.Intern("Material.Sine"));
        }
    }
}

// 접히지 않는 깊은 사슬(4096 단계)도 번역·모듈·원문 생성이 재귀 없이 끝난다.
TEST(MaterialSourceWriter_DeepVaryingChain) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    constexpr usize ChainLength = 4096;
    GraphFixture fixture(env);
    Guid previous = AddVarying(fixture, 0.5f);
    for (usize index = 0; index < ChainLength; ++index) {
        const Guid next = fixture.Add("Material.OneMinus");
        fixture.Connect(previous, "Result", next, "Input");
        previous = next;
    }

    fixture.Connect(previous, "Result", OutputNodeId, "AmbientOcclusion");
    Generated generated(env.allocator);
    REQUIRE(Generate(fixture, Traits(), generated));
    // sin 하나 + 1 − x 4096개가 표면 함수에 나온다.
    CHECK(generated.source.locations.Size() >= ChainLength + 1);
}

// MS-T3(6.9절): 문맥 값 — 정점·이전 정점 진입점의 Time(이전 변형), 표면의 정점 색 성분, PDO의
// 미분, 진입점별 사용 정의.
TEST(MaterialSourceWriter_ContextGolden) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid time = fixture.Add("Material.Time", env.Save(TimeProperties{}));
    fixture.Connect(time, "Result", OutputNodeId, "WorldPositionOffset");
    const Guid color = fixture.Add("Material.VertexColor");
    fixture.Connect(color, "G", OutputNodeId, "AmbientOcclusion");
    const Guid depth = fixture.Add("Material.PixelDepth");
    const Guid derivative = fixture.Add("Material.Ddx");
    fixture.Connect(depth, "Result", derivative, "Value");
    fixture.Connect(derivative, "Result", OutputNodeId, "PixelDepthOffset");
    Generated generated(env.allocator);
    REQUIRE(Generate(fixture, Traits(), generated));
    CHECK(MatchesGolden(env.allocator, "MaterialSource_Context.txt", generated.source.text.View()));
}

// 문맥 원문(6.9절): 저장소의 MaterialContext.rsh가 문맥 값 표로 만든 원문과 같다(표를 고치면
// 파일도 고친다). 같은 표는 두 번 만들어도 같은 byte다.
TEST(MaterialSourceWriter_ContextSourceMatchesTable) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    String first(env.allocator);
    String second(env.allocator);
    REQUIRE(WriteMaterialContextSource(env.allocator, first));
    REQUIRE(WriteMaterialContextSource(env.allocator, second));
    CHECK(first.View() == second.View());
    CHECK(MatchesSourceTreeFile(
        env.allocator, "Engine/Shaders/Private/Material/MaterialContext.rsh", first.View()));
}

// MS2c2: 파라미터 그래프의 생성 원문(상수 블록 선언·출력 읽기·Double 두 칸·프로그램 아래 값 생략·
// 접지 않는 Lerp는 원문에 남음).
TEST(MaterialSourceWriter_ParametersGolden) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    ScalarParameterProperties roughness;
    roughness.parameterName = env.Intern("Roughness");
    roughness.parameterId = Guid{0x50415241ull, 1};
    roughness.defaultValue = 0.5f;
    const Guid scalar = fixture.Add("Material.ScalarParameter", env.Save(roughness));
    VectorParameterProperties tint(env.allocator);
    tint.parameterName = env.Intern("Tint");
    tint.parameterId = Guid{0x50415241ull, 2};
    const Guid vector = fixture.Add("Material.VectorParameter", env.Save(tint));
    DoubleVectorParameterProperties origin;
    origin.parameterName = env.Intern("Origin");
    origin.parameterId = Guid{0x50415241ull, 3};
    const Guid wide = fixture.Add("Material.DoubleVectorParameter", env.Save(origin));
    // (Roughness + 1) × Tint.R은 프로그램이 접는다(출력 하나).
    const Guid plus = fixture.Add("Material.Add");
    fixture.Connect(scalar, "Result", plus, "A");
    fixture.Connect(AddConstant(fixture, 1.0f), "Result", plus, "B");
    const Guid folded = fixture.Add("Material.Multiply");
    fixture.Connect(plus, "Result", folded, "A");
    fixture.Connect(vector, "R", folded, "B");
    // lerp(folded, Tint.G, sin(0.5))는 셰이더가 계산한다.
    const Guid lerp = fixture.Add("Material.LinearInterpolate");
    fixture.Connect(folded, "Result", lerp, "A");
    fixture.Connect(vector, "G", lerp, "B");
    fixture.Connect(AddVarying(fixture, 0.5f), "Result", lerp, "Alpha");
    // length(WorldPosition − Origin.XYZ)는 Double 출력 두 칸을 읽는다.
    const Guid world = fixture.Add("Material.WorldPosition");
    const Guid relative = fixture.Add("Material.Subtract");
    fixture.Connect(world, "XYZ", relative, "A");
    fixture.Connect(wide, "XYZ", relative, "B");
    const Guid distance = fixture.Add("Material.Length");
    fixture.Connect(relative, "Result", distance, "Input");
    const Guid total = fixture.Add("Material.Add");
    fixture.Connect(lerp, "Result", total, "A");
    fixture.Connect(distance, "Result", total, "B");
    fixture.Connect(total, "Result", OutputNodeId, "AmbientOcclusion");
    Generated generated(env.allocator);
    REQUIRE(Generate(fixture, Traits(), generated));
    CHECK(MatchesGolden(env.allocator, "MaterialSource_Parameters.txt",
                        generated.source.text.View()));
}

} // namespace material_source_writer_tests_file
