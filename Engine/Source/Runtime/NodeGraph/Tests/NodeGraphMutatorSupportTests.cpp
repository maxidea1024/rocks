#include "NodeGraphMutatorTestSupport.h"

#include <NodeGraph/GraphDiagnostic.h>

// NG1 보조 기능 시험이다(Docs/Design/NodeGraph.ko.md 7·8.5·8.6절, GD19·GD24): id 발급기의
// 결정성·구별·0 회피, 읽기 view 조회, 순환 검사 도우미(재경유 포함), 기본 연결 판정, schema 기본
// 함수, 진단 변환, 문서 복제.
using namespace rocks;
namespace node_graph_mutator_support_tests_file {

using namespace rocks::nodegraph;
using namespace rocks::nodegraph_test;

// id 발급기 시험의 발급 수다.
constexpr uint32 IdSourceSamples = 256;

TEST(NodeGraph_SeededIdSourceIsDeterministic) {
    SeededGraphIdSource first(7, Id(100));
    SeededGraphIdSource second(7, Id(100));
    SeededGraphIdSource otherSeed(8, Id(100));
    SeededGraphIdSource otherAsset(7, Id(101));
    Guid previous{};
    for (uint32 index = 0; index < IdSourceSamples; ++index) {
        auto a = first.Next();
        auto b = second.Next();
        auto c = otherSeed.Next();
        auto d = otherAsset.Next();
        REQUIRE(a && b && c && d);
        CHECK(a.Value() == b.Value());
        CHECK(!(a.Value() == c.Value()) && !(a.Value() == d.Value()));
        CHECK(a.Value().high != 0 || a.Value().low != 0);
        CHECK(!(a.Value() == previous));
        previous = a.Value();
    }

    CHECK(first.Counter() == IdSourceSamples);
}

TEST(NodeGraph_RandomIdSourceIsDistinct) {
    RandomGraphIdSource source;
    auto first = source.Next();
    auto second = source.Next();
    REQUIRE(first && second);
    CHECK(!(first.Value() == second.Value()));
    CHECK(first.Value().high != 0 || first.Value().low != 0);
}

TEST(NodeGraph_ReadViewQueries) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample sample(env);
    REQUIRE(sample.ok);
    const GraphReadView view(sample.document, env.registry);
    const Graph* main = view.FindGraph(SampleMain);
    REQUIRE(main != nullptr);
    CHECK(view.FindGraph(Id(99)) == nullptr);
    CHECK(view.FindNode(*main, SampleAdd) != nullptr);
    CHECK(view.FindNode(*main, SampleInner) == nullptr);

    // Add로 들어오는 간선 둘(A·B), Sequence.Then에서 나가는 간선 둘(순번 순).
    const Span<const GraphEdge> into = view.EdgesInto(*main, SampleAdd);
    REQUIRE(into.Size() == 2);
    CHECK(into[0].toPin == env.pinA && into[1].toPin == env.pinB);
    CHECK(view.EdgesInto(*main, SampleParamA).Size() == 0);
    Array<GraphEdge> from(env.allocator);
    REQUIRE(view.CollectEdgesFrom(*main, SampleSequence, env.pinThen, from));
    REQUIRE(from.Size() == 2);
    CHECK(from[0].toNode == SampleStepA && from[1].toNode == SampleStepB);
    REQUIRE(view.CollectEdgesFrom(*main, SampleParamA, Name(), from));
    CHECK(from.Size() == 2);
    PinList pins(env.allocator);
    REQUIRE(view.CollectPins(*main, *view.FindNode(*main, SampleDynamic), pins));
    CHECK(pins.Size() == 3);
}

TEST(NodeGraph_WouldCreateCycle) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample sample(env);
    REQUIRE(sample.ok);
    const Graph& main = sample.Main();
    // Toggle은 Add 뒤(Add.Result → Toggle.In)이고 Add는 Reroute·ParamA·ParamB 뒤다. Toggle →
    // StepA(하류 없음)는 순환이 아니고, Toggle.Out → Reroute.In은 순환이다(Reroute → Add → Toggle).
    auto none = WouldCreateCycle(main, PinRef{SampleToggle, env.pinOut},
                                 PinRef{SampleStepA, env.pinIn}, env.allocator);
    REQUIRE(none);
    CHECK(!none.Value());
    auto cycle = WouldCreateCycle(main, PinRef{SampleToggle, env.pinOut},
                                  PinRef{SampleReroute, env.pinIn}, env.allocator);
    REQUIRE(cycle);
    CHECK(cycle.Value());
    auto self = WouldCreateCycle(main, PinRef{SampleAdd, env.pinResult},
                                 PinRef{SampleAdd, env.pinA}, env.allocator);
    REQUIRE(self);
    CHECK(self.Value());
    auto direct = WouldCreateCycle(main, PinRef{SampleAdd, env.pinResult},
                                   PinRef{SampleDefaults, env.pinA}, env.allocator);
    REQUIRE(direct);
    CHECK(!direct.Value());
    // ParamA는 Add의 상류라 Add.Result → ParamA는 순환이다.
    auto upstream = WouldCreateCycle(main, PinRef{SampleAdd, env.pinResult},
                                     PinRef{SampleParamA, env.pinA}, env.allocator);
    REQUIRE(upstream);
    CHECK(upstream.Value());
}

TEST(NodeGraph_DefaultConnectVerdict) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample sample(env);
    const GraphReadView view(sample.document, env.registry);
    const PinDescriptor floatOut = OutputPin(env.pinResult, TestFloatType, "Result");
    const PinDescriptor floatIn = InputPin(env.pinA, TestFloatType, "A");
    const PinDescriptor vectorIn = InputPin(env.pinB, TestVectorType, "B");
    const PinDescriptor wildIn = InputPin(env.pinIn, UnresolvedPinType, "In");
    const PinDescriptor flowIn = InputPin(env.pinIn, TestFlowType, "In", PinKind::Flow);
    ConnectQuery query;
    query.fromPin = &floatOut;
    query.toPin = &floatIn;
    CHECK(env.schema.GraphSchema::CanConnect(view, query).action == ConnectAction::Make);
    query.toEdgeCount = 1;
    CHECK(env.schema.GraphSchema::CanConnect(view, query).action ==
          ConnectAction::MakeReplacingInput);
    query.toEdgeCount = 0;
    query.toPin = &vectorIn;
    CHECK(env.schema.GraphSchema::CanConnect(view, query).action == ConnectAction::Deny);
    query.toPin = &wildIn;
    CHECK(env.schema.GraphSchema::CanConnect(view, query).action == ConnectAction::Make);
    query.toPin = &flowIn;
    CHECK(env.schema.GraphSchema::CanConnect(view, query).action == ConnectAction::Deny);
    query.toPin = nullptr;
    CHECK(env.schema.GraphSchema::CanConnect(view, query).action == ConnectAction::Deny);

    // 기본 CreateDefaultNodes·Validate는 아무것도 더하지 않는다.
    GraphCommandList list(env.allocator);
    SeededGraphIdSource ids(1, Id(1));
    REQUIRE(env.schema.CreateDefaultNodes(GraphRole::Main, SampleMain, ids, list));
    CHECK(list.Size() == 0);
    GraphDiagnosticList diagnostics(env.allocator);
    REQUIRE(env.schema.Validate(view, diagnostics));
    CHECK(diagnostics.items.IsEmpty());
}

TEST(NodeGraph_ViolationDiagnostics) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample sample(env);
    sample.Node(SampleAdd).retainedPins[0].value.bytes.Clear();
    GraphViolationList violations(env.allocator);
    CHECK(!Validate(env, sample.document, &sample.layout, violations));
    GraphDiagnosticList diagnostics(env.allocator);
    REQUIRE(AppendViolationDiagnostics(violations, Id(0xA55E7), diagnostics));
    REQUIRE(diagnostics.items.Size() == 1);
    const GraphDiagnostic& diagnostic = diagnostics.items[0];
    CHECK(diagnostic.code == static_cast<uint32>(GraphInvariant::RetainedPinUnused));
    CHECK(diagnostic.severity == GraphDiagnosticSeverity::Error);
    CHECK(diagnostic.asset == Id(0xA55E7) && diagnostic.graph == SampleMain &&
          diagnostic.node == SampleAdd);
    CHECK(diagnostic.pin == env.pinGone);
    CHECK(diagnostic.message.View().StartsWith("GI7:"));
    REQUIRE(diagnostics.Add(GraphDiagnosticSeverity::Warning, 900, Guid{}, SampleMain, Guid{},
                            Name(), "Domain note"));
    CHECK(diagnostics.items.Size() == 2);
    CHECK(GraphCommandReasonName(GraphCommandReason::CrossingEdge) == StringView("CrossingEdge"));
}

// 깊은 복제는 같은 바이트이고, 원본을 바꿔도 사본은 그대로다(GD24 snapshot).
TEST(NodeGraph_CloneDocumentAndLayout) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample sample(env);
    REQUIRE(sample.ok);
    GraphDocument copy(env.allocator);
    GraphDocumentLayout layoutCopy(env.allocator);
    REQUIRE(CloneGraphDocument(sample.document, copy));
    REQUIRE(CloneGraphDocumentLayout(sample.layout, layoutCopy));
    const Array<uint8> original = DocumentBytes(env, sample.document, sample.layout);
    CHECK(SameBytes(original, DocumentBytes(env, copy, layoutCopy)));
    sample.Node(SampleParamA).properties.bytes[0] ^= 1;
    sample.layout.graphs[0].nodes[0].position.x += 1.0f;
    CHECK(!SameBytes(DocumentBytes(env, sample.document, sample.layout),
                     DocumentBytes(env, copy, layoutCopy)));
    CHECK(SameBytes(original, DocumentBytes(env, copy, layoutCopy)));
}

} // namespace node_graph_mutator_support_tests_file
