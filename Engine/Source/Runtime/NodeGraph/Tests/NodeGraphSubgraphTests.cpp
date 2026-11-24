#include "NodeGraphMutatorTestSupport.h"

#include <NodeGraph/GraphFold.h>
#include <NodeGraph/GraphSemantic.h>
#include <NodeGraph/GraphValidation.h>

// 하위 그래프 경계 노드 시험이다(Docs/Design/NodeGraph 10.1절 NG3c): GI15·GI16 양성·음성,
// SetGraphBoundary의 세 노드 핀 재정리와 undo·redo 통지, 경계 종류 명령 규칙, 하위 그래프 연쇄
// 삭제와 undo, 접기·펼치기 왕복(펼친 뒤 의미 hash가 접기 전과 같음).

using namespace rocks;

namespace node_graph_subgraph_tests_file {

using namespace rocks::nodegraph;
using namespace rocks::nodegraph_test;
using Kind = GraphChangeKind;

GraphBoundaryPin Boundary(TestEnv& env, Name key, PinKind kind = PinKind::Data,
                          uint16 maxEdges = 1) noexcept {
    GraphBoundaryPin pin;
    pin.key = key;
    pin.kind = kind;
    auto type = env.names.TryIntern("Float");
    pin.type = type ? type.Value() : Name();
    pin.maxEdges = maxEdges;
    return pin;
}

GraphViolationList Validate(TestEnv& env, const Sample& sample) noexcept {
    GraphViolationList violations(env.allocator);
    static_cast<void>(ValidateGraphDocument(sample.document, &sample.layout, env.registry,
                                            env.allocator, violations));
    return violations;
}

bool HasRule(const GraphViolationList& list, GraphInvariant rule) noexcept {
    for (const GraphViolation& violation : list.items) {
        if (violation.rule == rule) {
            return true;
        }
    }

    return false;
}

Graph& Inner(Sample& sample) noexcept {
    return sample.document.graphs[1];
}

TEST(NodeGraph_SubgraphInvariants) {
    TestEnv env;
    REQUIRE(env.ready);
    {
        Sample sample(env);
        REQUIRE(sample.ok);
        CHECK(Validate(env, sample).items.IsEmpty());
    }

    // GI15: 주 그래프의 경계 노드, 경계 노드 둘, 경계 노드 없음, 경계 목록 규칙.
    {
        Sample sample(env);
        REQUIRE(
            sample.Main().nodes.TryPushBack(MakeNode(env, Id(80), env.registry.GraphInputType())));
        NodeLayout position(env.allocator);
        position.node = Id(80);
        REQUIRE(sample.layout.graphs[0].nodes.TryPushBack(Move(position)));
        CHECK(HasRule(Validate(env, sample), GraphInvariant::BoundaryInterface));
    }

    {
        Sample sample(env);
        REQUIRE(Inner(sample).nodes.TryRemoveAt(2));
        REQUIRE(sample.layout.graphs[1].nodes.TryRemoveAt(2));
        CHECK(HasRule(Validate(env, sample), GraphInvariant::BoundaryInterface));
    }

    const auto boundaryRule = [&](GraphBoundaryPin first, GraphBoundaryPin second) noexcept {
        Sample sample(env);
        return Inner(sample).inputs.TryPushBack(first) &&
               Inner(sample).outputs.TryPushBack(second) &&
               HasRule(Validate(env, sample), GraphInvariant::BoundaryInterface);
    };
    CHECK(boundaryRule(Boundary(env, env.pinA), Boundary(env, env.pinA)));
    CHECK(boundaryRule(Boundary(env, env.pinA), Boundary(env, env.pinB, PinKind::Structural)));
    CHECK(boundaryRule(Boundary(env, env.pinA), Boundary(env, env.pinB, PinKind::Data, 0)));
    CHECK(boundaryRule(Boundary(env, Name()), Boundary(env, env.pinB)));
    {
        Sample sample(env);
        REQUIRE(Inner(sample).inputs.TryPushBack(Boundary(env, env.pinA)));
        REQUIRE(Inner(sample).outputs.TryPushBack(Boundary(env, env.pinB, PinKind::Flow, 3)));
        CHECK(Validate(env, sample).items.IsEmpty());
    }

    // Collapsed가 아닌 그래프의 인터페이스.
    {
        Sample sample(env);
        Inner(sample).role = GraphRole::DomainDefined;
        CHECK(HasRule(Validate(env, sample), GraphInvariant::BoundaryInterface));
    }

    // GI16: 부모가 Graph.Collapsed가 아님, 부모 없음, 하위 그래프 없는 Graph.Collapsed, 순환.
    {
        Sample sample(env);
        Inner(sample).ownerNode = SampleAdd;
        const GraphViolationList violations = Validate(env, sample);
        CHECK(HasRule(violations, GraphInvariant::CollapsedOwner));
    }

    {
        Sample sample(env);
        Inner(sample).ownerNode = Id(99);
        CHECK(HasRule(Validate(env, sample), GraphInvariant::CollapsedOwner));
    }

    {
        Sample sample(env);
        // 하위 그래프 안에 자기를 가리키는 Graph.Collapsed를 둔다(순환).
        GraphNode self = MakeNode(env, Id(81), env.registry.CollapsedType());
        REQUIRE(Inner(sample).nodes.TryPushBack(Move(self)));
        NodeLayout position(env.allocator);
        position.node = Id(81);
        REQUIRE(sample.layout.graphs[1].nodes.TryPushBack(Move(position)));
        Inner(sample).ownerNode = Id(81);
        CHECK(HasRule(Validate(env, sample), GraphInvariant::CollapsedOwner));
    }
}

TEST(NodeGraph_SubgraphSetBoundary) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    const GraphBoundaryPin inputs[] = {Boundary(env, env.pinA)};
    const GraphBoundaryPin outputs[] = {Boundary(env, env.pinResult)};
    GraphCommandList set(env.allocator);
    REQUIRE(set.SetGraphBoundary(SampleCollapsed, Span<const GraphBoundaryPin>(inputs, 1),
                                 Span<const GraphBoundaryPin>(outputs, 1)));
    auto first = m.Run(set);
    REQUIRE(first);
    const RecordedBatch& batch = m.recorder.Last();
    REQUIRE(!batch.changes.IsEmpty());
    CHECK(batch.changes[0].kind == Kind::GraphBoundaryChanged && batch.semanticTouched);
    usize pinsChanged = 0;
    for (const GraphChange& change : batch.changes) {
        pinsChanged += change.kind == Kind::NodePinsChanged ? 1 : 0;
    }

    CHECK(pinsChanged == 3);

    // 경계 핀으로 바깥·안쪽을 잇는다.
    GraphCommandList link(env.allocator);
    REQUIRE(link.Connect(SampleMain,
                         MakeEdge(SampleParamA, env.pinValue, SampleCollapsedNode, env.pinA)));
    REQUIRE(
        link.Connect(SampleCollapsed, MakeEdge(SampleInnerInput, env.pinA, SampleInner, env.pinA)));
    REQUIRE(m.Run(link));

    // 경계 입력 A를 빼면 양쪽 간선이 고아 핀으로 남고, undo하면 되돌아온다.
    const Array<uint8> before = m.Bytes();
    GraphCommandList shrink(env.allocator);
    REQUIRE(shrink.SetGraphBoundary(SampleCollapsed, Span<const GraphBoundaryPin>(),
                                    Span<const GraphBoundaryPin>(outputs, 1)));
    auto shrunk = m.Run(shrink);
    REQUIRE(shrunk);
    const Array<uint8> shrunkBytes = m.Bytes();
    const GraphNode& parent = m.sample.Node(SampleCollapsedNode);
    REQUIRE(parent.retainedPins.Size() == 1);
    CHECK(parent.retainedPins[0].key == env.pinA &&
          parent.retainedPins[0].direction == PinDirection::Input);
    bool innerRetained = false;
    for (const GraphNode& node : Inner(m.sample).nodes) {
        if (node.id == SampleInnerInput) {
            innerRetained = node.retainedPins.Size() == 1 &&
                            node.retainedPins[0].direction == PinDirection::Output;
        }
    }

    CHECK(innerRetained);
    REQUIRE(m.mutator.Undo(TestSource, shrunk.Value()));
    CHECK(SameBytes(m.Bytes(), before));
    bool boundaryNoticed = false;
    for (const GraphChange& change : m.recorder.Last().changes) {
        boundaryNoticed = boundaryNoticed || change.kind == Kind::GraphBoundaryChanged;
    }

    CHECK(boundaryNoticed);
    // redo는 shrink 직후 바이트를 다시 만든다.
    REQUIRE(m.mutator.Redo(TestSource, shrunk.Value()));
    CHECK(SameBytes(m.Bytes(), shrunkBytes));

    // 고아 핀이 남은 Graph.Collapsed는 펼치지 않는다(간선·값을 이을 곳이 없다).
    GraphCommandList expandRetained(env.allocator);
    auto refusedExpand =
        BuildExpandCommands(m.mutator.View(), SampleMain, SampleCollapsedNode, expandRetained);
    CHECK(!refusedExpand && refusedExpand.Error() == ErrorCode::Unsupported);

    // 틀린 목록·Collapsed 아닌 그래프.
    GraphCommandFailure failure;
    const GraphBoundaryPin duplicate[] = {Boundary(env, env.pinB), Boundary(env, env.pinB)};
    GraphCommandList bad(env.allocator);
    REQUIRE(bad.SetGraphBoundary(SampleCollapsed, Span<const GraphBoundaryPin>(duplicate, 2),
                                 Span<const GraphBoundaryPin>()));
    CHECK(!m.Run(bad, &failure) && failure.reason == GraphCommandReason::BoundaryInvalid);
    GraphCommandList mainGraph(env.allocator);
    REQUIRE(mainGraph.SetGraphBoundary(SampleMain, Span<const GraphBoundaryPin>(inputs, 1),
                                       Span<const GraphBoundaryPin>()));
    CHECK(!m.Run(mainGraph, &failure) && failure.reason == GraphCommandReason::GraphRole);
    CHECK(!m.recorder.failed);
}

TEST(NodeGraph_SubgraphCommandRules) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    const auto rejects = [&](const GraphCommandList& list, GraphCommandReason reason) noexcept {
        const Array<uint8> before = m.Bytes();
        GraphCommandFailure failure;
        auto patch = m.Run(list, &failure);
        return !patch && failure.reason == reason && SameBytes(m.Bytes(), before);
    };

    GraphCommandList inputInMain(env.allocator);
    REQUIRE(inputInMain.AddNode(SampleMain, MakeNode(env, Id(80), env.registry.GraphInputType()),
                                MakeLayout(env, Id(80), 0, 0)));
    CHECK(rejects(inputInMain, GraphCommandReason::BoundaryNode));
    GraphCommandList orphanCollapsed(env.allocator);
    REQUIRE(orphanCollapsed.AddNode(SampleMain, MakeNode(env, Id(81), env.registry.CollapsedType()),
                                    MakeLayout(env, Id(81), 0, 0)));
    CHECK(rejects(orphanCollapsed, GraphCommandReason::BoundaryNode));
    GraphCommandList removeBoundary(env.allocator);
    REQUIRE(removeBoundary.RemoveNodes(SampleCollapsed, Span<const Guid>(&SampleInnerInput, 1)));
    CHECK(rejects(removeBoundary, GraphCommandReason::CannotDelete));
    GraphCommandList moveBoundary(env.allocator);
    REQUIRE(moveBoundary.MoveNodesToGraph(SampleCollapsed, Span<const Guid>(&SampleInnerInput, 1),
                                          SampleMain));
    CHECK(rejects(moveBoundary, GraphCommandReason::BoundaryNode));
    GraphCommandList cycle(env.allocator);
    REQUIRE(cycle.MoveNodesToGraph(SampleMain, Span<const Guid>(&SampleCollapsedNode, 1),
                                   SampleCollapsed));
    CHECK(rejects(cycle, GraphCommandReason::CollapsedCycle));
    GraphCommandList noOwner(env.allocator);
    REQUIRE(noOwner.AddGraph(Id(82), GraphRole::Collapsed, 0, "x"));
    CHECK(rejects(noOwner, GraphCommandReason::BoundaryNode));
    GraphCommandList sharedOwner(env.allocator);
    REQUIRE(sharedOwner.AddGraph(Id(82), GraphRole::Collapsed, 0, "x", SampleCollapsedNode));
    CHECK(rejects(sharedOwner, GraphCommandReason::BoundaryNode));
    // 하위 그래프만 더하고 부모·경계 노드가 없으면 목록 끝 검사가 거부한다.
    GraphCommandList unfinished(env.allocator);
    REQUIRE(unfinished.AddGraph(Id(82), GraphRole::Collapsed, 0, "x", Id(83)));
    CHECK(rejects(unfinished, GraphCommandReason::BoundaryNode));

    // 중첩 하위 그래프를 완성해 넣은 뒤 바깥 부모를 지우면 둘 다 연쇄로 지워지고 undo로 돌아온다.
    GraphCommandList nested(env.allocator);
    REQUIRE(nested.AddGraph(Id(82), GraphRole::Collapsed, 0, "Nested", Id(83)));
    REQUIRE(nested.AddNode(Id(82), MakeNode(env, Id(84), env.registry.GraphInputType()),
                           MakeLayout(env, Id(84), 0, 0)));
    REQUIRE(nested.AddNode(Id(82), MakeNode(env, Id(85), env.registry.GraphOutputType()),
                           MakeLayout(env, Id(85), 0, 0)));
    REQUIRE(nested.AddNode(SampleCollapsed, MakeNode(env, Id(83), env.registry.CollapsedType()),
                           MakeLayout(env, Id(83), 0, 0)));
    REQUIRE(m.Run(nested));
    CHECK(m.sample.document.graphs.Size() == 3);
    GraphCommandList referenced(env.allocator);
    REQUIRE(referenced.RemoveGraph(Id(82)));
    CHECK(rejects(referenced, GraphCommandReason::GraphReferenced));
    const Array<uint8> before = m.Bytes();
    GraphCommandList remove(env.allocator);
    REQUIRE(remove.RemoveNodes(SampleMain, Span<const Guid>(&SampleCollapsedNode, 1)));
    auto removed = m.Run(remove);
    REQUIRE(removed);
    CHECK(m.sample.document.graphs.Size() == 1);
    REQUIRE(m.mutator.Undo(TestSource, removed.Value()));
    CHECK(SameBytes(m.Bytes(), before));
    CHECK(!m.recorder.failed);
}

TEST(NodeGraph_SubgraphFoldRoundTrip) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    auto before = ComputeSemanticHash(m.sample.document, env.registry, env.allocator);
    REQUIRE(before);

    // Add·Reroute를 접는다(건너는 간선 셋, 안쪽 간선 하나).
    const Guid group[] = {SampleAdd, SampleReroute};
    GraphCommandList collapse(env.allocator);
    REQUIRE(BuildCollapseCommands(m.mutator.View(), &m.sample.layout, SampleMain,
                                  Span<const Guid>(group, 2), m.ids, collapse));
    auto collapsed = m.Run(collapse);
    REQUIRE(collapsed);
    REQUIRE(m.sample.document.graphs.Size() == 3);
    Guid collapsedNode;
    for (const Graph& graph : m.sample.document.graphs) {
        if (graph.role == GraphRole::Collapsed && !(graph.ownerNode == SampleCollapsedNode)) {
            collapsedNode = graph.ownerNode;
            CHECK(graph.inputs.Size() == 2 && graph.outputs.Size() == 1);
            CHECK(graph.nodes.Size() == 4);
        }
    }

    auto folded = ComputeSemanticHash(m.sample.document, env.registry, env.allocator);
    REQUIRE(folded);
    CHECK(!(folded.Value() == before.Value()));

    // 펼치면 의미 hash가 접기 전과 같다.
    GraphCommandList expand(env.allocator);
    GraphFoldReport report;
    REQUIRE(BuildExpandCommands(m.mutator.View(), SampleMain, collapsedNode, expand, &report));
    auto expanded = m.Run(expand);
    REQUIRE(expanded);
    CHECK(m.sample.document.graphs.Size() == 2 && report.droppedDefaults == 0);
    auto after = ComputeSemanticHash(m.sample.document, env.registry, env.allocator);
    REQUIRE(after);
    CHECK(after.Value() == before.Value());

    // undo 두 번으로 원래 바이트다.
    const Array<uint8> original = DocumentBytes(env, Sample(env).document, Sample(env).layout);
    REQUIRE(m.mutator.Undo(TestSource, expanded.Value()));
    REQUIRE(m.mutator.Undo(TestSource, collapsed.Value()));
    CHECK(SameBytes(m.Bytes(), original));

    // 경계 노드를 접으려 하면 거부한다.
    GraphCommandList bad(env.allocator);
    auto refused = BuildCollapseCommands(m.mutator.View(), &m.sample.layout, SampleCollapsed,
                                         Span<const Guid>(&SampleInnerInput, 1), m.ids, bad);
    CHECK(!refused && refused.Error() == ErrorCode::InvalidArgument);
    CHECK(!m.recorder.failed);
}

// 펼치기: 그대로 지나는 경계 간선(Input → Output)은 바깥끼리, Collapsed 입력 기본값은 안쪽 받는
// 핀으로 옮기고 옮길 곳이 없으면 센다.
TEST(NodeGraph_SubgraphExpandPassThroughAndDefaults) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    const GraphBoundaryPin inputs[] = {Boundary(env, env.pinA), Boundary(env, env.pinB),
                                       Boundary(env, env.pinOther)};
    const GraphBoundaryPin outputs[] = {Boundary(env, env.pinResult)};
    Array<uint8> value(env.allocator);
    REQUIRE(FillRecord(value, TestValueStruct, 1, 6));
    GraphCommandList setup(env.allocator);
    REQUIRE(setup.SetGraphBoundary(SampleCollapsed, Span<const GraphBoundaryPin>(inputs, 3),
                                   Span<const GraphBoundaryPin>(outputs, 1)));
    // A: 바깥 ParamA → A, 안쪽 Input.A → Output.Result(그대로 지남), 바깥 Result → Dynamic.Item_0.
    REQUIRE(setup.Connect(SampleMain,
                          MakeEdge(SampleParamA, env.pinValue, SampleCollapsedNode, env.pinA)));
    REQUIRE(setup.Connect(SampleCollapsed,
                          MakeEdge(SampleInnerInput, env.pinA, SampleInnerOutput, env.pinResult)));
    REQUIRE(setup.Connect(
        SampleMain, MakeEdge(SampleCollapsedNode, env.pinResult, SampleDynamic, env.pinItem0)));
    // B: 바깥 간선 없이 기본값, 안쪽 Input.B → Inner.B로 옮겨진다.
    REQUIRE(setup.Connect(SampleCollapsed,
                          MakeEdge(SampleInnerInput, env.pinB, SampleInner, env.pinB)));
    REQUIRE(setup.SetInputDefault(SampleMain, SampleCollapsedNode, env.pinB, value.View()));
    // Other: 기본값만 있고 받는 핀이 없다(버리고 센다).
    REQUIRE(setup.SetInputDefault(SampleMain, SampleCollapsedNode, env.pinOther, value.View()));
    REQUIRE(m.Run(setup));

    GraphCommandList expand(env.allocator);
    GraphFoldReport report;
    REQUIRE(
        BuildExpandCommands(m.mutator.View(), SampleMain, SampleCollapsedNode, expand, &report));
    REQUIRE(m.Run(expand));
    CHECK(report.droppedDefaults == 1);
    CHECK(m.sample.document.graphs.Size() == 1);
    bool through = false;
    for (const GraphEdge& edge : m.sample.Main().edges) {
        through = through || (edge.fromNode == SampleParamA && edge.toNode == SampleDynamic &&
                              edge.toPin == env.pinItem0);
    }

    CHECK(through);
    const GraphNode& inner = m.sample.Node(SampleInner);
    REQUIRE(inner.inputDefaults.Size() == 1);
    CHECK(inner.inputDefaults[0].pin == env.pinB &&
          SameBytes(inner.inputDefaults[0].value.bytes, value));
    CHECK(!m.recorder.failed);
}

} // namespace node_graph_subgraph_tests_file
