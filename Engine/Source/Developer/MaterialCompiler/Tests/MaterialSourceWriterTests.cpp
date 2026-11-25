#include <Core/FileRead.h>
#include <Core/Text/String.h>
#include <MaterialCompiler/MaterialSourceWriter.h>
#include <MaterialSchema/Nodes/Constants/ConstantDoubleProperties.generated.h>
#include <MaterialSchema/Nodes/Constants/ConstantDoubleProperties.h>
#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.generated.h>
#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.h>
#include <MaterialSchema/Nodes/Math/CosineProperties.generated.h>
#include <MaterialSchema/Nodes/Math/CosineProperties.h>
#include <MaterialSchema/Nodes/Math/OperatorProperties.generated.h>
#include <MaterialSchema/Nodes/Math/OperatorProperties.h>

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

// text가 기대 원문 name과 byte로 같은가. 다르면 실제 원문을 실패로 남긴다.
bool MatchesGolden(Allocator& allocator, StringView name, StringView text) noexcept {
    String path(allocator);
    Array<uint8> bytes(allocator);
    bool existed = false;
    const bool read = GoldenPath(name, path) &&
                      ReadWholeFile(platform::PhysicalFileSystem(), path.View(), MaxGoldenFileBytes,
                                    bytes, existed)
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

// 생성할 수 없는 값(Double — LWC 표현은 MS2b)은 원인 노드에 NotYetLowered다.
TEST(MaterialSourceWriter_RejectsDoubleValues) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    ConstantDoubleProperties precise;
    precise.value = 2.5;
    const Guid wide = fixture.Add("Material.ConstantDouble", env.Save(precise));
    const Guid sum = fixture.Add("Material.Add");
    fixture.Connect(wide, "Result", sum, "A");
    fixture.Connect(sum, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation translation(env.allocator);
    REQUIRE(Translate(fixture, Traits(), translation));
    REQUIRE(translation.succeeded);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(WriteMaterialSource(env.allocator, translation.module, Traits(), TestAssetId,
                                MainGraphId, source));
    CHECK(!source.succeeded);
    CHECK(source.text.IsEmpty());
    bool found = false;
    for (const nodegraph::GraphDiagnostic& diagnostic : source.diagnostics.items) {
        found =
            found || (diagnostic.code == ToDiagnosticCode(MaterialDiagnosticCode::NotYetLowered) &&
                      (diagnostic.node == wide || diagnostic.node == sum));
    }

    CHECK(found);
}

} // namespace material_source_writer_tests_file
