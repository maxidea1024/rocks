#include "GraphToolsTestSupport.h"

#include <NodeGraphTools/GraphPaste.h>

// 복사·붙여넣기 시험이다(Docs/Design/NodeGraph 9.4절, 13절 GT5): id 재발급·간선 재연결·위치
// 이동·주석, undo 1개, 같은 seed면 같은 바이트, 텍스트를 거친 왕복, 다른 도메인 거부, 노드 판정
// 진단(미해석·버전·속성·복제 불가·문서당 하나·schema 거부), 자리 노드 바이트 보존, 바깥 간선 정책,
// 값 없는 고아 핀 정리, 복사 오류.
using namespace rocks;
namespace graph_paste_tests_file {

using namespace rocks::nodegraph_tools_test;

constexpr uint64 PasteSeed = 0x5041535445ull;
constexpr Guid SourceAsset{0x5ADE, 1};
constexpr Guid OtherAsset{0x5ADE, 2};

const Graph& GraphOf(Sample& sample, Guid id) noexcept {
    for (const Graph& graph : sample.document.graphs) {
        if (graph.id == id) {
            return graph;
        }
    }

    return sample.document.graphs[0];
}

const GraphNode* FindNode(const Graph& graph, Guid id) noexcept {
    for (const GraphNode& node : graph.nodes) {
        if (node.id == id) {
            return &node;
        }
    }

    return nullptr;
}

Guid MappedNode(const GraphPasteResult& result, Guid from) noexcept {
    for (const GraphIdMapping& mapping : result.nodes) {
        if (mapping.from == from) {
            return mapping.to;
        }
    }

    return Guid{};
}

bool HasCode(const GraphPasteResult& result, GraphPasteCode code, Guid node) noexcept {
    for (const GraphDiagnostic& diagnostic : result.diagnostics.items) {
        if (diagnostic.code == static_cast<uint32>(code) && diagnostic.node == node &&
            diagnostic.severity == GraphDiagnosticSeverity::Warning) {
            return true;
        }
    }

    return false;
}

// Sequence·StepA·StepB와 주석을 복사해 하위 그래프에 붙인다.
struct SequencePaste {
    explicit SequencePaste(ToolsEnv& tools) noexcept
        : m(tools.env), fragment(tools.env.allocator), result(tools.env.allocator),
          ids(PasteSeed, SourceAsset) {
        const Guid selected[] = {SampleSequence, SampleStepA, SampleStepB};
        const Guid comments[] = {SampleComment};
        ready =
            m.ready && CopyGraphFragment(m.sample.document, &m.sample.layout, tools.env.registry,
                                         SampleMain, Span<const Guid>(selected, 3),
                                         Span<const Guid>(comments, 1), SourceAsset, fragment);
        GraphPasteOptions options;
        options.targetAsset = SourceAsset;
        options.graph = SampleCollapsed;
        options.position = Vec2(500, 600);
        ready = ready && BuildPasteCommands(fragment, m.sample.document, tools.env.registry,
                                            options, ids, result);
    }

    MutatorEnv m;
    GraphFragment fragment;
    GraphPasteResult result;
    SeededGraphIdSource ids;
    bool ready = false;
};

TEST(GraphPaste_CopyPasteRoundTrip) {
    ToolsEnv tools;
    REQUIRE(tools.ready);
    SequencePaste paste(tools);
    REQUIRE(paste.ready);
    CHECK(paste.fragment.nodes.Size() == 3 && paste.fragment.edges.Size() == 2 &&
          paste.fragment.externalEdges.IsEmpty() && paste.fragment.comments.Size() == 1);
    CHECK(paste.result.diagnostics.items.IsEmpty());
    REQUIRE(paste.result.nodes.Size() == 3 && paste.result.comments.Size() == 1);
    MutatorEnv& m = paste.m;
    const Array<uint8> before = m.Bytes();
    auto patch = m.Run(paste.result.commands);
    REQUIRE(patch);

    // 새 id로 옮겨졌고 순번이 원래대로다(StepA 0, StepB 1). 원래 노드는 그대로다.
    const Graph& inner = GraphOf(m.sample, SampleCollapsed);
    const Guid sequence = MappedNode(paste.result, SampleSequence);
    const Guid stepA = MappedNode(paste.result, SampleStepA);
    const Guid stepB = MappedNode(paste.result, SampleStepB);
    REQUIRE(FindNode(inner, sequence) && FindNode(inner, stepA) && FindNode(inner, stepB));
    CHECK(!(sequence == SampleSequence) && FindNode(m.sample.Main(), SampleSequence));
    uint32 found = 0;
    for (const GraphEdge& edge : inner.edges) {
        if (edge.fromNode == sequence) {
            CHECK((edge.toNode == stepA && edge.order == 0) ||
                  (edge.toNode == stepB && edge.order == 1));
            ++found;
        }
    }

    CHECK(found == 2);

    // 붙인 노드·주석의 왼쪽 위가 붙이는 위치다.
    float minX = NumericLimits<float>::Max();
    float minY = NumericLimits<float>::Max();
    for (const GraphLayout& graph : m.sample.layout.graphs) {
        if (!(graph.graph == SampleCollapsed)) {
            continue;
        }

        for (const NodeLayout& node : graph.nodes) {
            if (node.node == sequence || node.node == stepA || node.node == stepB) {
                minX = node.position.x < minX ? node.position.x : minX;
                minY = node.position.y < minY ? node.position.y : minY;
            }
        }

        REQUIRE(graph.comments.Size() == 1);
        CHECK(graph.comments[0].id == paste.result.comments[0].to);
        minX = graph.comments[0].position.x < minX ? graph.comments[0].position.x : minX;
        minY = graph.comments[0].position.y < minY ? graph.comments[0].position.y : minY;
    }

    CHECK(minX == 500.0f && minY == 600.0f);
    REQUIRE(m.mutator.Undo(TestSource, patch.Value()));
    CHECK(SameBytes(m.Bytes(), before));
    REQUIRE(m.mutator.Redo(TestSource, patch.Value()));
    const Array<uint8> after = m.Bytes();

    // 같은 seed·조각·문서면 같은 바이트다.
    SequencePaste again(tools);
    REQUIRE(again.ready);
    REQUIRE(again.m.Run(again.result.commands));
    CHECK(SameBytes(again.m.Bytes(), after));

    // 텍스트를 거쳐도 같다.
    StringBuilder<> text(tools.env.allocator);
    REQUIRE(WriteGraphFragmentJson(tools.context, paste.fragment, {}, text));
    GraphFragment read(tools.env.allocator);
    GraphTextDiagnostics diagnostics(tools.env.allocator);
    REQUIRE(ReadGraphFragmentJson(tools.context, text.View(), read, diagnostics));
    MutatorEnv viaText(tools.env);
    REQUIRE(viaText.ready);
    SeededGraphIdSource ids(PasteSeed, SourceAsset);
    GraphPasteOptions options;
    options.targetAsset = SourceAsset;
    options.graph = SampleCollapsed;
    options.position = Vec2(500, 600);
    GraphPasteResult fromText(tools.env.allocator);
    REQUIRE(BuildPasteCommands(read, viaText.sample.document, tools.env.registry, options, ids,
                               fromText));
    REQUIRE(viaText.Run(fromText.commands));
    CHECK(SameBytes(viaText.Bytes(), after));
}

// 노드 판정 진단과 자리 노드 보존, 다른 도메인·없는 그래프.
TEST(GraphPaste_NodeDecisions) {
    ToolsEnv tools;
    REQUIRE(tools.ready);
    TestEnv& env = tools.env;
    NodeTypeDescriptor single;
    single.type = env.Intern("Test.Single");
    single.capabilities = DefaultNodeCapabilities | NodeCapabilities::SinglePerDocument;
    NodeTypeDescriptor noCopy;
    noCopy.type = env.Intern("Test.NoCopy");
    noCopy.capabilities = NodeCapabilities::CanDelete;
    REQUIRE(env.registry.RegisterNodeType(env.domain, single, ModuleLease()));
    REQUIRE(env.registry.RegisterNodeType(env.domain, noCopy, ModuleLease()));
    env.schema.deniedPasteType = env.typeToggle;
    MutatorEnv m(env);
    REQUIRE(m.ready);

    GraphFragment fragment(env.allocator);
    fragment.domain = env.domain;
    fragment.sourceGraph = SampleMain;
    const auto add = [&](GraphNode node) noexcept {
        auto entry = fragment.nodes.TryEmplaceBack(env.allocator);
        if (!entry) {
            return static_cast<GraphFragmentNode*>(nullptr);
        }

        entry.Value()->layout.node = node.id;
        entry.Value()->node = Move(node);
        return entry.Value();
    };

    GraphFragmentNode* unresolved = add(MakeNode(env, Id(70), env.typeAdd));
    REQUIRE(unresolved && unresolved->unresolvedType.TryAssign("Gone.Type"));
    REQUIRE(add(MakeNode(env, Id(71), env.typeAdd)));
    REQUIRE(add(MakeNode(env, Id(72), env.typeAdd, 2)));
    GraphNode badParam = MakeNode(env, Id(73), env.typeParam, TestParamVersion);
    REQUIRE(FillRecord(badParam.properties.bytes, TestValueStruct, 1, 1));
    REQUIRE(add(Move(badParam)));
    REQUIRE(add(MakeNode(env, Id(74), noCopy.type)));
    REQUIRE(add(MakeNode(env, Id(75), single.type)));
    REQUIRE(add(MakeNode(env, Id(76), single.type)));
    REQUIRE(add(MakeNode(env, Id(77), env.typeToggle)));
    GraphNode placeholder = MakeNode(env, Id(78), env.typeUnknown, 9);
    REQUIRE(FillRecord(placeholder.properties.bytes, TestValueStruct, 5, 55));
    REQUIRE(add(Move(placeholder)));
    REQUIRE(fragment.edges.TryPushBack(MakeEdge(Id(70), env.pinResult, Id(71), env.pinA)));

    SeededGraphIdSource ids(PasteSeed, SourceAsset);
    GraphPasteOptions options;
    options.graph = SampleMain;
    GraphPasteResult result(env.allocator);
    REQUIRE(BuildPasteCommands(fragment, m.sample.document, env.registry, options, ids, result));
    CHECK(HasCode(result, GraphPasteCode::UnresolvedProperties, Id(70)));
    CHECK(HasCode(result, GraphPasteCode::EdgeDropped, Id(70)));
    CHECK(HasCode(result, GraphPasteCode::TypeVersion, Id(72)));
    CHECK(HasCode(result, GraphPasteCode::PropertiesSchema, Id(73)));
    CHECK(HasCode(result, GraphPasteCode::CannotDuplicate, Id(74)));
    CHECK(HasCode(result, GraphPasteCode::SinglePerDocument, Id(76)));
    CHECK(HasCode(result, GraphPasteCode::PasteDenied, Id(77)));
    CHECK(result.diagnostics.items.Size() == 7);
    REQUIRE(result.nodes.Size() == 3);
    REQUIRE(m.Run(result.commands));
    const GraphNode* pasted = FindNode(m.sample.Main(), MappedNode(result, Id(78)));
    REQUIRE(pasted);
    GraphNode expected = MakeNode(env, Id(78), env.typeUnknown, 9);
    REQUIRE(FillRecord(expected.properties.bytes, TestValueStruct, 5, 55));
    CHECK(pasted->typeSchemaVersion == 9 &&
          SameBytes(pasted->properties.bytes, expected.properties.bytes));

    // 다른 도메인은 전체 거부, 없는 대상 그래프는 NotFound다.
    fragment.domain = env.Intern("Other");
    auto domain =
        BuildPasteCommands(fragment, m.sample.document, env.registry, options, ids, result);
    CHECK(!domain && domain.Error() == ErrorCode::InvalidArgument);
    fragment.domain = env.domain;
    options.graph = Id(99);
    auto graph =
        BuildPasteCommands(fragment, m.sample.document, env.registry, options, ids, result);
    CHECK(!graph && graph.Error() == ErrorCode::NotFound);
}

// 바깥 간선은 같은 에셋이고 schema가 허락하며 판정이 Make일 때만 다시 잇는다.
TEST(GraphPaste_ExternalEdges) {
    ToolsEnv tools;
    REQUIRE(tools.ready);
    TestEnv& env = tools.env;
    MutatorEnv m(env);
    REQUIRE(m.ready);
    GraphFragment fragment(env.allocator);
    const Guid selected[] = {SampleAdd};
    REQUIRE(CopyGraphFragment(m.sample.document, &m.sample.layout, env.registry, SampleMain,
                              Span<const Guid>(selected, 1), Span<const Guid>(), SourceAsset,
                              fragment));
    REQUIRE(fragment.externalEdges.Size() == 3);

    const auto paste = [&](Guid target, bool reconnect, GraphPasteResult& result) noexcept {
        env.schema.reconnectExternal = reconnect;
        SeededGraphIdSource ids(PasteSeed, SourceAsset);
        GraphPasteOptions options;
        options.targetAsset = target;
        options.graph = SampleMain;
        return static_cast<bool>(
            BuildPasteCommands(fragment, m.sample.document, env.registry, options, ids, result));
    };

    // 같은 에셋·허락: ParamA.Value → A, Reroute.Out → B는 잇고, Toggle.In은 입력이 차 있어(바꾸기
    // 판정) 끊는다.
    GraphPasteResult same(env.allocator);
    REQUIRE(paste(SourceAsset, true, same));
    uint32 connects = 0;
    for (const GraphCommand& command : same.commands.View()) {
        connects += command.kind == GraphCommandKind::Connect ? 1u : 0u;
    }

    CHECK(connects == 2);
    CHECK(same.diagnostics.items.Size() == 1 &&
          same.diagnostics.items[0].code ==
              static_cast<uint32>(GraphPasteCode::ExternalEdgeDropped));
    auto patch = m.Run(same.commands);
    REQUIRE(patch);
    const Guid pasted = MappedNode(same, SampleAdd);
    uint32 incoming = 0;
    for (const GraphEdge& edge : m.sample.Main().edges) {
        incoming += edge.toNode == pasted ? 1u : 0u;
    }

    CHECK(incoming == 2);
    REQUIRE(m.mutator.Undo(TestSource, patch.Value()));

    // 다른 에셋이거나 schema가 허락하지 않으면 모두 끊는다.
    GraphPasteResult other(env.allocator);
    REQUIRE(paste(OtherAsset, true, other));
    CHECK(other.diagnostics.items.Size() == 3);
    GraphPasteResult denied(env.allocator);
    REQUIRE(paste(SourceAsset, false, denied));
    CHECK(denied.diagnostics.items.Size() == 3);
    env.schema.reconnectExternal = false;
}

// 값 없는 고아 핀은 붙는 간선이 남지 않으면 뺀다(GI7). 값 있는 고아 핀은 남는다.
TEST(GraphPaste_PrunesValuelessRetainedPins) {
    ToolsEnv tools;
    REQUIRE(tools.ready);
    TestEnv& env = tools.env;
    MutatorEnv m(env);
    REQUIRE(m.ready);
    GraphFragment fragment(env.allocator);
    fragment.domain = env.domain;
    fragment.sourceGraph = SampleMain;
    auto entry = fragment.nodes.TryEmplaceBack(env.allocator);
    REQUIRE(entry);
    entry.Value()->node = MakeNode(env, Id(80), env.typeAdd);
    entry.Value()->layout.node = Id(80);
    RetainedPin spare(env.allocator);
    spare.key = env.Intern("Spare");
    RetainedPin kept(env.allocator);
    kept.key = env.Intern("Kept");
    REQUIRE(FillRecord(kept.value.bytes, TestValueStruct, 1, 4));
    REQUIRE(entry.Value()->node.retainedPins.TryPushBack(Move(kept)));
    REQUIRE(entry.Value()->node.retainedPins.TryPushBack(Move(spare)));
    REQUIRE(fragment.externalEdges.TryPushBack(
        MakeEdge(SampleParamA, env.pinValue, Id(80), env.Intern("Spare"))));
    SeededGraphIdSource ids(PasteSeed, SourceAsset);
    GraphPasteOptions options;
    options.graph = SampleMain;
    GraphPasteResult result(env.allocator);
    REQUIRE(BuildPasteCommands(fragment, m.sample.document, env.registry, options, ids, result));
    REQUIRE(m.Run(result.commands));
    const GraphNode* pasted = FindNode(m.sample.Main(), MappedNode(result, Id(80)));
    REQUIRE(pasted && pasted->retainedPins.Size() == 1);
    CHECK(pasted->retainedPins[0].key == env.Intern("Kept"));
}

TEST(GraphPaste_CopyErrors) {
    ToolsEnv tools;
    REQUIRE(tools.ready);
    TestEnv& env = tools.env;
    Sample sample(env);
    REQUIRE(sample.ok);
    GraphFragment fragment(env.allocator);
    const Guid missing[] = {Id(99)};
    auto node =
        CopyGraphFragment(sample.document, &sample.layout, env.registry, SampleMain,
                          Span<const Guid>(missing, 1), Span<const Guid>(), Guid{}, fragment);
    CHECK(!node && node.Error() == ErrorCode::NotFound);
    auto comment =
        CopyGraphFragment(sample.document, &sample.layout, env.registry, SampleMain,
                          Span<const Guid>(), Span<const Guid>(missing, 1), Guid{}, fragment);
    CHECK(!comment && comment.Error() == ErrorCode::NotFound);
    auto graph = CopyGraphFragment(sample.document, &sample.layout, env.registry, Id(99),
                                   Span<const Guid>(), Span<const Guid>(), Guid{}, fragment);
    CHECK(!graph && graph.Error() == ErrorCode::NotFound);
    const Guid twice[] = {SampleAdd, SampleAdd};
    auto duplicate =
        CopyGraphFragment(sample.document, &sample.layout, env.registry, SampleMain,
                          Span<const Guid>(twice, 2), Span<const Guid>(), Guid{}, fragment);
    CHECK(!duplicate && duplicate.Error() == ErrorCode::InvalidArgument);

    // 배치 없이 복사하면 기본 배치다.
    const Guid one[] = {SampleAdd};
    REQUIRE(CopyGraphFragment(sample.document, nullptr, env.registry, SampleMain,
                              Span<const Guid>(one, 1), Span<const Guid>(), Guid{}, fragment));
    REQUIRE(fragment.nodes.Size() == 1);
    CHECK(fragment.nodes[0].layout.position.x == 0.0f &&
          fragment.nodes[0].layout.node == SampleAdd);
}

// InsertNode·InsertEdge 규칙에 걸릴 노드·간선은 붙여넣기가 미리 빼고 진단한다(목록 전체가 거부되지
// 않는다). 같은 조각을 이어 쓰는 발급기로 두 번 붙일 수 있다.
TEST(GraphPaste_PrechecksAndRepeats) {
    ToolsEnv tools;
    REQUIRE(tools.ready);
    TestEnv& env = tools.env;
    MutatorEnv m(env);
    REQUIRE(m.ready);
    GraphFragment fragment(env.allocator);
    fragment.domain = env.domain;
    fragment.sourceGraph = SampleMain;
    const auto add = [&](GraphNode node) noexcept {
        auto entry = fragment.nodes.TryEmplaceBack(env.allocator);
        if (!entry) {
            return static_cast<GraphFragmentNode*>(nullptr);
        }

        entry.Value()->layout.node = node.id;
        entry.Value()->node = Move(node);
        return entry.Value();
    };

    GraphNode disabled = MakeNode(env, Id(90), env.typeAdd);
    disabled.enableState = NodeEnableState::Disabled;
    REQUIRE(add(Move(disabled)));
    GraphNode wrongDefault = MakeNode(env, Id(91), env.typeAdd);
    PinValueEntry entry(env.allocator);
    entry.pin = env.Intern("Nope");
    REQUIRE(FillRecord(entry.value.bytes, TestValueStruct, 1, 1));
    REQUIRE(wrongDefault.inputDefaults.TryPushBack(Move(entry)));
    REQUIRE(add(Move(wrongDefault)));
    REQUIRE(add(MakeNode(env, Id(92), env.typeAdd)));
    REQUIRE(add(MakeNode(env, Id(93), env.typeAdd)));
    REQUIRE(fragment.edges.TryPushBack(MakeEdge(Id(92), env.Intern("Nope"), Id(93), env.pinA)));
    REQUIRE(fragment.edges.TryPushBack(MakeEdge(Id(92), env.pinResult, Id(93), env.pinB)));

    SeededGraphIdSource ids(PasteSeed, SourceAsset);
    GraphPasteOptions options;
    options.graph = SampleMain;
    GraphPasteResult result(env.allocator);
    REQUIRE(BuildPasteCommands(fragment, m.sample.document, env.registry, options, ids, result));
    CHECK(HasCode(result, GraphPasteCode::NodeData, Id(90)));
    CHECK(HasCode(result, GraphPasteCode::NodeData, Id(91)));
    CHECK(HasCode(result, GraphPasteCode::EdgeDropped, Id(93)));
    CHECK(result.diagnostics.items.Size() == 3);
    REQUIRE(m.Run(result.commands));

    // 같은 발급기로 한 번 더 붙이면 새 id라 겹치지 않는다.
    GraphPasteResult again(env.allocator);
    REQUIRE(BuildPasteCommands(fragment, m.sample.document, env.registry, options, ids, again));
    REQUIRE(m.Run(again.commands));
    CHECK(!(MappedNode(again, Id(92)) == MappedNode(result, Id(92))));
}

} // namespace graph_paste_tests_file
