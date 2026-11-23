#include "NodeGraphMutatorTestSupport.h"

// GraphMutator 명령 규칙의 경계 사례 시험이다(Docs/Design/NodeGraph 6.4·8.6절, 13절 GT1·GT2):
// 중복 id, 고아 핀 자동 정리(GI7), 고아 핀 가림(GI8), 재경유 순환(GI10), 값·그래프 크기 상한, undo
// 순서(LIFO) 위반, 통지 중 구독 해제, bracket 중 undo, 변환 노드의 문서당 하나, 같은 바이트
// 속성(무동작), 출력 핀 단위 순번 통지.
using namespace rocks;
namespace node_graph_mutator_rule_tests_file {

using namespace rocks::nodegraph;
using namespace rocks::nodegraph_test;

using Kind = GraphChangeKind;

// 값 없는 고아 핀 둘(Defaults의 입력 Spare ← ParamA.Value, ParamB의 출력 SpareOut →
// Dynamic.Item_0)을 더한다.
constexpr Guid SpareTarget = SampleDefaults;

bool AddSparePins(TestEnv& env, Sample& sample) noexcept {
    RetainedPin input(env.allocator);
    input.key = env.Intern("Spare");
    input.direction = PinDirection::Input;
    RetainedPin output(env.allocator);
    output.key = env.Intern("SpareOut");
    output.direction = PinDirection::Output;
    return sample.Node(SpareTarget).retainedPins.TryPushBack(Move(input)) &&
           sample.Node(SampleParamB).retainedPins.TryPushBack(Move(output)) &&
           sample.Main().edges.TryPushBack(
               MakeEdge(SampleParamA, env.pinValue, SpareTarget, env.Intern("Spare"))) &&
           sample.Main().edges.TryPushBack(
               MakeEdge(SampleParamB, env.Intern("SpareOut"), SampleDynamic, env.pinItem0));
}

bool Rejects(MutatorEnv& m, const GraphCommandList& list, GraphCommandReason reason) noexcept {
    const Array<uint8> before = m.Bytes();
    const uint64 revision = m.mutator.Revision();
    GraphCommandFailure failure;
    auto patch = m.Run(list, &failure);
    return !patch && patch.Error() == ErrorCode::InvalidArgument && failure.reason == reason &&
           SameBytes(m.Bytes(), before) && m.mutator.Revision() == revision;
}

bool HasRetained(const GraphNode& node, Name key) noexcept {
    for (const RetainedPin& pin : node.retainedPins) {
        if (pin.key == key) {
            return true;
        }
    }

    return false;
}

TEST(NodeGraph_RuleDuplicateNodeIds) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    const Guid twice[] = {SampleAdd, SampleAdd};
    GraphCommandList remove(env.allocator);
    REQUIRE(remove.RemoveNodes(SampleMain, Span<const Guid>(twice, 2)));
    CHECK(Rejects(m, remove, GraphCommandReason::DuplicateNode));
    const Guid group[] = {SampleSequence, SampleStepA, SampleSequence};
    GraphCommandList move(env.allocator);
    REQUIRE(move.MoveNodesToGraph(SampleMain, Span<const Guid>(group, 3), SampleCollapsed));
    CHECK(Rejects(m, move, GraphCommandReason::DuplicateNode));
}

// 간선이 빠져 값도 간선도 없게 된 고아 핀은 같은 명령에서 지운다(6.4절, GI7). undo는 되살린다.
TEST(NodeGraph_RulePrunesRetainedPins) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env, &AddSparePins);
    REQUIRE(m.ready);
    const Name spare = env.Intern("Spare");
    const Name spareOut = env.Intern("SpareOut");
    const Array<uint8> before = m.Bytes();

    // 끊기: 입력 고아 핀 Spare가 사라진다.
    GraphCommandList cut(env.allocator);
    REQUIRE(cut.Disconnect(SampleMain, MakeEdge(SampleParamA, env.pinValue, SpareTarget, spare)));
    auto patch = m.Run(cut);
    REQUIRE(patch);
    const Kind cutKinds[] = {Kind::EdgeRemoved, Kind::RetainedPinChanged};
    CHECK(m.recorder.LastKinds(Span<const Kind>(cutKinds, 2)));
    CHECK(!HasRetained(m.sample.Node(SpareTarget), spare));
    REQUIRE(m.mutator.Undo(TestSource, patch.Value()));
    CHECK(SameBytes(m.Bytes(), before));

    // 노드 지우기: Dynamic을 지우면 ParamB의 출력 고아 핀 SpareOut이 사라진다.
    GraphCommandList remove(env.allocator);
    REQUIRE(remove.RemoveNodes(SampleMain, Span<const Guid>(&SampleDynamic, 1)));
    auto removed = m.Run(remove);
    REQUIRE(removed);
    CHECK(!HasRetained(m.sample.Node(SampleParamB), spareOut));
    REQUIRE(m.mutator.Undo(TestSource, removed.Value()));
    CHECK(SameBytes(m.Bytes(), before));

    // 바꾸기 연결: ParamA 출력을 바꾸면(ReplaceOutput) Spare 간선이 빠지고 Spare도 사라진다.
    GraphCommandList free(env.allocator);
    REQUIRE(free.AddNode(SampleMain, MakeNode(env, Id(50), env.typeMulti),
                         MakeLayout(env, Id(50), 0, 0)));
    REQUIRE(m.Run(free));
    env.schema.connect = TestConnect::ReplaceOutput;
    GraphCommandList toMulti(env.allocator);
    REQUIRE(
        toMulti.Connect(SampleMain, MakeEdge(SampleParamA, env.pinValue, Id(50), env.pinItems)));
    auto replaced = m.Run(toMulti);
    REQUIRE(replaced);
    CHECK(!HasRetained(m.sample.Node(SpareTarget), spare));
    env.schema.connect = TestConnect::Default;

    // 값이 있는 고아 핀(Add.Gone)은 간선이 없어도 남는다.
    CHECK(HasRetained(m.sample.Node(SampleAdd), env.pinGone));
}

// 새 핀 목록에 고아 핀과 같은 키가 생기면 거부한다(GI8 — 재결합은 NG3).
TEST(NodeGraph_RuleRetainedShadowed) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env, [](TestEnv& testEnv, Sample& sample) noexcept {
        RetainedPin item(testEnv.allocator);
        item.key = testEnv.Intern("Item_2");
        return FillRecord(item.value.bytes, TestValueStruct, 1, 1) &&
               sample.Node(SampleDynamic).retainedPins.TryPushBack(Move(item));
    });
    REQUIRE(m.ready);
    Array<uint8> bytes(env.allocator);
    REQUIRE(FillRecord(bytes, TestDynamicStruct, 3, 0));
    GraphCommandList grow(env.allocator);
    REQUIRE(grow.SetNodeProperties(SampleMain, SampleDynamic, bytes.View()));
    CHECK(Rejects(m, grow, GraphCommandReason::RetainedShadowed));
}

// 재경유 사슬 순환(GI10)과 재경유 자기 간선을 거부한다.
TEST(NodeGraph_RuleRerouteCycle) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    GraphCommandList self(env.allocator);
    REQUIRE(
        self.Connect(SampleMain, MakeEdge(SampleReroute, env.pinOut, SampleReroute, env.pinIn)));
    CHECK(Rejects(m, self, GraphCommandReason::RerouteCycle));
    GraphCommandList chain(env.allocator);
    REQUIRE(chain.AddNode(SampleMain, MakeNode(env, Id(60), env.reroute),
                          MakeLayout(env, Id(60), 0, 0)));
    REQUIRE(chain.Connect(SampleMain, MakeEdge(SampleReroute, env.pinOut, Id(60), env.pinIn)));
    REQUIRE(m.Run(chain));
    GraphCommandList back(env.allocator);
    REQUIRE(back.Connect(SampleMain, MakeEdge(Id(60), env.pinOut, SampleReroute, env.pinIn)));
    CHECK(Rejects(m, back, GraphCommandReason::RerouteCycle));
    // 재경유가 아닌 노드를 거치는 순환은 공통 층이 막지 않는다(도메인 판정).
    GraphCommandList loop(env.allocator);
    REQUIRE(loop.Connect(SampleMain, MakeEdge(SampleToggle, env.pinOut, SampleReroute, env.pinIn)));
    CHECK(m.Run(loop));
}

TEST(NodeGraph_RuleValueTooLarge) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    Array<uint8> huge(env.allocator);
    REQUIRE(huge.TryResize(MaxPropertyRecordBytes + 1));
    GraphCommandList value(env.allocator);
    REQUIRE(value.SetInputDefault(SampleMain, SampleDefaults, env.pinB, huge.View()));
    CHECK(Rejects(m, value, GraphCommandReason::ValueTooLarge));
    GraphCommandList properties(env.allocator);
    REQUIRE(properties.SetNodeProperties(SampleMain, SampleParamA, huge.View()));
    CHECK(Rejects(m, properties, GraphCommandReason::ValueTooLarge));
    GraphNode param = MakeNode(env, Id(50), env.typeParam, TestParamVersion);
    REQUIRE(param.properties.bytes.TryAppend(huge.View()));
    GraphCommandList add(env.allocator);
    REQUIRE(add.AddNode(SampleMain, param, MakeLayout(env, Id(50), 0, 0)));
    CHECK(Rejects(m, add, GraphCommandReason::ValueTooLarge));
}

// 그래프 노드 수가 MaxGraphNodes에 이르면 노드를 더하거나 옮겨 넣을 수 없다.
TEST(NodeGraph_RuleGraphNodeLimit) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env, [](TestEnv& testEnv, Sample& sample) noexcept {
        Graph& inner = sample.document.graphs[1];
        GraphLayout& layout = sample.layout.graphs[1];
        bool ok = inner.nodes.TryReserve(MaxGraphNodes) && layout.nodes.TryReserve(MaxGraphNodes);
        for (uint32 index = 1; ok && inner.nodes.Size() < MaxGraphNodes; ++index) {
            const Guid id{1, index};
            NodeLayout position(testEnv.allocator);
            position.node = id;
            ok = inner.nodes.TryPushBack(MakeNode(testEnv, id, testEnv.typeAdd)) &&
                 layout.nodes.TryPushBack(Move(position));
        }

        return ok;
    });
    REQUIRE(m.ready);
    m.mutator.SetFullValidation(false);
    GraphCommandList add(env.allocator);
    REQUIRE(add.AddNode(SampleCollapsed, MakeNode(env, Id(50), env.typeAdd),
                        MakeLayout(env, Id(50), 0, 0)));
    CHECK(Rejects(m, add, GraphCommandReason::GraphFull));
    GraphCommandList move(env.allocator);
    REQUIRE(move.MoveNodesToGraph(SampleMain, Span<const Guid>(&SampleStepA, 1), SampleCollapsed));
    CHECK(Rejects(m, move, GraphCommandReason::GraphFull));
}

// 그래프 간선 수가 MaxGraphEdges에 이르면 더 잇지 못한다(입력 무제한 핀 512개 × 출력 512개).
TEST(NodeGraph_RuleGraphEdgeLimit) {
    TestEnv env;
    REQUIRE(env.ready);
    const PinDescriptor sinkPins[] = {[&env]() noexcept {
        PinDescriptor pin = InputPin(env.pinItems, TestFloatType, "Items");
        pin.maxEdges = UnlimitedEdges;
        return pin;
    }()};
    NodeTypeDescriptor sink;
    sink.type = env.Intern("Test.Sink");
    sink.staticPins = sinkPins;
    REQUIRE(env.registry.RegisterNodeType(env.domain, sink, ModuleLease()));
    constexpr uint32 Side = 512;
    static_assert(Side * Side == MaxGraphEdges, "edge limit fixture must fill the graph exactly");
    struct Fill {
        static bool Run(TestEnv& testEnv, Sample& sample) noexcept {
            Graph& inner = sample.document.graphs[1];
            GraphLayout& layout = sample.layout.graphs[1];
            const Name sinkType = testEnv.Intern("Test.Sink");
            bool ok = static_cast<bool>(inner.edges.TryReserve(MaxGraphEdges));
            // 출력 노드 {2, i}(Add), 입력 노드 {3, j}(Sink). 간선 정규 순서: 입력 노드, 입력 핀,
            // 출력 노드.
            for (uint32 index = 0; ok && index < Side; ++index) {
                for (uint64 group = 2; ok && group <= 3; ++group) {
                    const Guid id{group, index + 1};
                    NodeLayout position(testEnv.allocator);
                    position.node = id;
                    ok = inner.nodes.TryPushBack(
                             MakeNode(testEnv, id, group == 2 ? testEnv.typeAdd : sinkType)) &&
                         layout.nodes.TryPushBack(Move(position));
                }
            }

            for (uint32 to = 0; ok && to < Side; ++to) {
                for (uint32 from = 0; ok && from < Side; ++from) {
                    ok = static_cast<bool>(inner.edges.TryPushBack(MakeEdge(
                        Guid{2, from + 1}, testEnv.pinResult, Guid{3, to + 1}, testEnv.pinItems)));
                }
            }

            return ok;
        }
    };

    MutatorEnv m(env, &Fill::Run);
    REQUIRE(m.ready);
    m.mutator.SetFullValidation(false);
    GraphCommandList connect(env.allocator);
    REQUIRE(connect.Connect(SampleCollapsed,
                            MakeEdge(SampleInner, env.pinResult, Guid{3, 1}, env.pinItems)));
    CHECK(Rejects(m, connect, GraphCommandReason::GraphFull));
}

// undo·redo는 LIFO여야 한다: 마지막 patch가 아닌 것의 undo, 순서가 틀린 redo는 StaleHandle이고
// 문서는 그대로다.
TEST(NodeGraph_RuleUndoOrder) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    GraphCommandList first(env.allocator);
    REQUIRE(first.SetLayout(SampleMain, SampleAdd, MakeLayout(env, SampleAdd, 1, 1)));
    GraphCommandList second(env.allocator);
    REQUIRE(second.SetLayout(SampleMain, SampleAdd, MakeLayout(env, SampleAdd, 2, 2)));
    auto p1 = m.Run(first);
    auto p2 = m.Run(second);
    REQUIRE(p1 && p2);
    const Array<uint8> after = m.Bytes();
    auto early = m.mutator.Undo(TestSource, p1.Value());
    REQUIRE(!early);
    CHECK(early.Error() == ErrorCode::StaleHandle);
    CHECK(SameBytes(m.Bytes(), after));
    REQUIRE(m.mutator.Undo(TestSource, p2.Value()));
    REQUIRE(m.mutator.Undo(TestSource, p1.Value()));
    auto wrongRedo = m.mutator.Redo(TestSource, p2.Value());
    REQUIRE(!wrongRedo);
    CHECK(wrongRedo.Error() == ErrorCode::StaleHandle);
    REQUIRE(m.mutator.Redo(TestSource, p1.Value()));
    REQUIRE(m.mutator.Redo(TestSource, p2.Value()));
    CHECK(SameBytes(m.Bytes(), after));

    // 바꾼 것이 없는 patch(빈 patch)는 상태를 바꾸지 않으므로 바로 그 상태에서 undo·redo할 수 있다.
    GraphCommandList empty(env.allocator);
    auto none = m.Run(empty);
    REQUIRE(none);
    REQUIRE(m.mutator.Undo(TestSource, none.Value()));
    REQUIRE(m.mutator.Undo(TestSource, p2.Value()));
}

// 통지 콜백 안에서 구독을 해제해도 다른 구독자는 그 묶음을 받는다.
TEST(NodeGraph_RuleRemoveListenerDuringNotify) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    struct SelfRemoving final : public GraphChangeListener {
        GraphMutator* mutator = nullptr;
        uint32 calls = 0;

        void OnGraphChanged(const GraphChangeBatch& batch) noexcept override {
            static_cast<void>(batch);
            ++calls;
            mutator->RemoveListener(*this);
        }
    };

    SelfRemoving first;
    first.mutator = &m.mutator;
    ChangeRecorder later(env.allocator);
    REQUIRE(m.mutator.AddListener(first));
    REQUIRE(m.mutator.AddListener(later));
    GraphCommandList list(env.allocator);
    REQUIRE(list.SetLayout(SampleMain, SampleAdd, MakeLayout(env, SampleAdd, 1, 1)));
    REQUIRE(m.Run(list));
    REQUIRE(m.Run(list));
    CHECK(first.calls == 1);
    CHECK(later.batches.Size() == 2);
    m.mutator.RemoveListener(later);
}

TEST(NodeGraph_RuleUndoDuringInteraction) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    GraphCommandList list(env.allocator);
    REQUIRE(list.SetLayout(SampleMain, SampleAdd, MakeLayout(env, SampleAdd, 1, 1)));
    auto patch = m.Run(list);
    REQUIRE(patch);
    REQUIRE(m.mutator.BeginInteraction(TestSource));
    auto undo = m.mutator.Undo(TestSource, patch.Value());
    REQUIRE(!undo);
    CHECK(undo.Error() == ErrorCode::Busy);
    REQUIRE(m.mutator.CancelInteraction(TestSource));
    REQUIRE(m.mutator.Undo(TestSource, patch.Value()));
}

// 문서당 하나인 종류는 변환 노드로도 둘째를 만들 수 없다.
TEST(NodeGraph_RuleConversionSinglePerDocument) {
    TestEnv env;
    REQUIRE(env.ready);
    const PinDescriptor pins[] = {InputPin(env.pinIn, UnresolvedPinType, "In"),
                                  OutputPin(env.pinOut, UnresolvedPinType, "Out")};
    NodeTypeDescriptor single;
    single.type = env.Intern("Test.SingleConvert");
    single.staticPins = pins;
    single.capabilities = DefaultNodeCapabilities | NodeCapabilities::SinglePerDocument;
    REQUIRE(env.registry.RegisterNodeType(env.domain, single, ModuleLease()));
    MutatorEnv m(env);
    REQUIRE(m.ready);
    GraphCommandList first(env.allocator);
    REQUIRE(first.AddNode(SampleCollapsed, MakeNode(env, Id(50), single.type),
                          MakeLayout(env, Id(50), 0, 0)));
    REQUIRE(m.Run(first));
    env.schema.connect = TestConnect::Convert;
    env.schema.conversionType = single.type;
    GraphCommandList convert(env.allocator);
    REQUIRE(convert.Connect(SampleMain,
                            MakeEdge(SampleParamB, env.pinValue, SampleDefaults, env.pinA)));
    CHECK(Rejects(m, convert, GraphCommandReason::SinglePerDocument));
    env.schema.connect = TestConnect::Default;
}

// 같은 바이트의 속성 설정은 무동작이다(op·통지의 semanticTouched 없음). 출력 핀 순번 통지는 핀마다
// 하나다.
TEST(NodeGraph_RuleNoOpPropertiesAndOrderNotice) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    Array<uint8> same(env.allocator);
    REQUIRE(same.TryAppend(m.sample.Node(SampleParamA).properties.bytes.View()));
    GraphCommandList list(env.allocator);
    REQUIRE(list.SetNodeProperties(SampleMain, SampleParamA, same.View()));
    auto patch = m.Run(list);
    REQUIRE(patch);
    CHECK(patch.Value().IsEmpty());
    CHECK(!m.recorder.Last().semanticTouched && m.recorder.Last().changes.IsEmpty());

    const GraphEdge swapped[] = {MakeEdge(SampleSequence, env.pinThen, SampleStepB, env.pinIn),
                                 MakeEdge(SampleSequence, env.pinThen, SampleStepA, env.pinIn)};
    GraphCommandList reorder(env.allocator);
    REQUIRE(reorder.ReorderEdges(SampleMain, SampleSequence, env.pinThen,
                                 Span<const GraphEdge>(swapped, 2)));
    REQUIRE(m.Run(reorder));
    const Kind once[] = {Kind::EdgeOrderChanged};
    CHECK(m.recorder.LastKinds(Span<const Kind>(once, 1)));
    GraphCommandList input(env.allocator);
    REQUIRE(input.ReorderEdges(SampleMain, SampleSequence, env.pinIn, Span<const GraphEdge>()));
    CHECK(Rejects(m, input, GraphCommandReason::PinDirection));
}

} // namespace node_graph_mutator_rule_tests_file
