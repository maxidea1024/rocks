#include "NodeGraphMutatorTestSupport.h"

#include <stdio.h>

// 붙여넣기 명령 InsertNode·InsertEdge 시험이다(Docs/Design/NodeGraph 9.4절 "공통 명령 추가"):
// 완성된 노드 데이터(입력 기본값·고아 핀·자리 노드)를 넣고 정규 순서로 맞추며, 종류 규칙·GI8·GI14를
// 지킨다. 간선은 schema 판정 없이 현재 핀·고아 핀·알 수 없는 종류 끝에 넣고, ordered 출력은 끝에
// 붙이며 순번 규칙이 없는 끝은 주어진 순번을 둔다.
using namespace rocks;
namespace node_graph_mutator_insert_tests_file {

using namespace rocks::nodegraph;
using namespace rocks::nodegraph_test;

constexpr Guid Inserted = Id(60);
constexpr Guid Placeholder = Id(61);

bool Rejects(MutatorEnv& m, const GraphCommandList& list, GraphCommandReason reason) noexcept {
    const Array<uint8> before = m.Bytes();
    const uint64 revision = m.mutator.Revision();
    GraphCommandFailure failure;
    auto patch = m.Run(list, &failure);
    const bool rejected = !patch && patch.Error() == ErrorCode::InvalidArgument &&
                          failure.reason == reason && SameBytes(m.Bytes(), before) &&
                          m.mutator.Revision() == revision;
    if (!rejected) {
        fprintf(stderr, "expected %s, got %s\n", GraphCommandReasonName(reason),
                GraphCommandReasonName(failure.reason));
    }

    return rejected;
}

PinValueEntry Default(TestEnv& env, Name pin, uint32 payload) noexcept {
    PinValueEntry entry(env.allocator);
    entry.pin = pin;
    static_cast<void>(FillRecord(entry.value.bytes, TestValueStruct, 1, payload));
    return entry;
}

RetainedPin Retained(TestEnv& env, Name key, PinDirection direction, bool value) noexcept {
    RetainedPin pin(env.allocator);
    pin.key = key;
    pin.direction = direction;
    if (value) {
        static_cast<void>(FillRecord(pin.value.bytes, TestValueStruct, 1, 9));
    }

    return pin;
}

// 기본값 B·A(정규 순서가 아님)와 값 있는 고아 핀 Old를 가진 Add 노드다.
GraphNode CompleteAdd(TestEnv& env, Guid id) noexcept {
    GraphNode node = MakeNode(env, id, env.typeAdd);
    static_cast<void>(node.inputDefaults.TryPushBack(Default(env, env.pinB, 2)));
    static_cast<void>(node.inputDefaults.TryPushBack(Default(env, env.pinA, 1)));
    static_cast<void>(
        node.retainedPins.TryPushBack(Retained(env, env.Intern("Old"), PinDirection::Input, true)));
    return node;
}

const GraphNode* FindNode(const Graph& graph, Guid id) noexcept {
    for (const GraphNode& node : graph.nodes) {
        if (node.id == id) {
            return &node;
        }
    }

    return nullptr;
}

TEST(NodeGraph_InsertNodeKeepsCompleteData) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    const Array<uint8> before = m.Bytes();
    GraphCommandList list(env.allocator);
    REQUIRE(
        list.InsertNode(SampleMain, CompleteAdd(env, Inserted), MakeLayout(env, Inserted, 3, 4)));
    auto patch = m.Run(list);
    REQUIRE(patch);
    const GraphNode* node = FindNode(m.sample.Main(), Inserted);
    REQUIRE(node);
    REQUIRE(node->inputDefaults.Size() == 2 && node->retainedPins.Size() == 1);
    CHECK(node->inputDefaults[0].pin == env.pinA && node->inputDefaults[1].pin == env.pinB);
    REQUIRE(m.mutator.Undo(TestSource, patch.Value()));
    CHECK(SameBytes(m.Bytes(), before));

    // 자리 노드(등록되지 않은 종류)는 바이트 그대로 들어간다.
    GraphNode placeholder = MakeNode(env, Placeholder, env.typeUnknown, 7);
    REQUIRE(FillRecord(placeholder.properties.bytes, TestValueStruct, 4, 44));
    REQUIRE(placeholder.retainedPins.TryPushBack(
        Retained(env, env.Intern("Mystery"), PinDirection::Output, true)));
    GraphCommandList unknown(env.allocator);
    REQUIRE(unknown.InsertNode(SampleMain, placeholder, MakeLayout(env, Placeholder, 0, 0)));
    REQUIRE(m.Run(unknown));
    const GraphNode* inserted = FindNode(m.sample.Main(), Placeholder);
    REQUIRE(inserted);
    CHECK(inserted->typeSchemaVersion == 7 &&
          SameBytes(inserted->properties.bytes, placeholder.properties.bytes));
}

TEST(NodeGraph_InsertNodeRules) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    const auto rejects = [&](GraphNode node, GraphCommandReason reason) noexcept {
        GraphCommandList list(env.allocator);
        const Guid id = node.id;
        return list.InsertNode(SampleMain, node, MakeLayout(env, id, 0, 0)) &&
               Rejects(m, list, reason);
    };

    CHECK(rejects(CompleteAdd(env, Guid{}), GraphCommandReason::IdZero));
    CHECK(rejects(CompleteAdd(env, SampleAdd), GraphCommandReason::NodeExists));
    CHECK(rejects(MakeNode(env, Inserted, Name()), GraphCommandReason::TypeUnknown));
    CHECK(rejects(MakeNode(env, Inserted, env.typeAdd, 2), GraphCommandReason::TypeVersion));
    GraphNode param = MakeNode(env, Inserted, env.typeParam, TestParamVersion);
    REQUIRE(FillRecord(param.properties.bytes, TestValueStruct, 1, 1));
    CHECK(rejects(Move(param), GraphCommandReason::PropertiesSchema));
    GraphNode missing = MakeNode(env, Inserted, env.typeAdd);
    REQUIRE(missing.inputDefaults.TryPushBack(Default(env, env.Intern("Nope"), 1)));
    CHECK(rejects(Move(missing), GraphCommandReason::PinMissing));
    GraphNode output = MakeNode(env, Inserted, env.typeAdd);
    REQUIRE(output.inputDefaults.TryPushBack(Default(env, env.pinResult, 1)));
    CHECK(rejects(Move(output), GraphCommandReason::PinMissing));
    GraphNode empty = MakeNode(env, Inserted, env.typeAdd);
    PinValueEntry blank(env.allocator);
    blank.pin = env.pinA;
    REQUIRE(empty.inputDefaults.TryPushBack(Move(blank)));
    CHECK(rejects(Move(empty), GraphCommandReason::ValueEmpty));
    GraphNode shadow = MakeNode(env, Inserted, env.typeAdd);
    REQUIRE(shadow.retainedPins.TryPushBack(Retained(env, env.pinA, PinDirection::Input, true)));
    CHECK(rejects(Move(shadow), GraphCommandReason::RetainedShadowed));
    GraphNode twice = MakeNode(env, Inserted, env.typeAdd);
    REQUIRE(twice.inputDefaults.TryPushBack(Default(env, env.pinB, 1)));
    REQUIRE(twice.inputDefaults.TryPushBack(Default(env, env.pinB, 2)));
    CHECK(rejects(Move(twice), GraphCommandReason::NodeDataInvalid));
    GraphNode retainedTwice = MakeNode(env, Inserted, env.typeUnknown);
    REQUIRE(retainedTwice.retainedPins.TryPushBack(
        Retained(env, env.Intern("K"), PinDirection::Input, true)));
    REQUIRE(retainedTwice.retainedPins.TryPushBack(
        Retained(env, env.Intern("K"), PinDirection::Output, true)));
    CHECK(rejects(Move(retainedTwice), GraphCommandReason::NodeDataInvalid));
    GraphNode disabled = MakeNode(env, Inserted, env.typeAdd);
    disabled.enableState = NodeEnableState::Disabled;
    CHECK(rejects(Move(disabled), GraphCommandReason::EnableState));
    GraphNode large = MakeNode(env, Inserted, env.typeUnknown);
    REQUIRE(large.properties.bytes.TryResize(MaxPropertyRecordBytes + 1));
    CHECK(rejects(Move(large), GraphCommandReason::ValueTooLarge));
    GraphNode largeDefault = MakeNode(env, Inserted, env.typeUnknown);
    PinValueEntry largeEntry(env.allocator);
    largeEntry.pin = env.pinA;
    REQUIRE(largeEntry.value.bytes.TryResize(MaxPropertyRecordBytes + 1));
    REQUIRE(largeDefault.inputDefaults.TryPushBack(Move(largeEntry)));
    CHECK(rejects(Move(largeDefault), GraphCommandReason::ValueTooLarge));
    GraphCommandList missingGraph(env.allocator);
    REQUIRE(missingGraph.InsertNode(Id(99), CompleteAdd(env, Inserted),
                                    MakeLayout(env, Inserted, 0, 0)));
    CHECK(Rejects(m, missingGraph, GraphCommandReason::GraphMissing));
}

TEST(NodeGraph_InsertEdgeEnds) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    GraphNode placeholder = MakeNode(env, Placeholder, env.typeUnknown);
    GraphCommandList setup(env.allocator);
    REQUIRE(
        setup.InsertNode(SampleMain, CompleteAdd(env, Inserted), MakeLayout(env, Inserted, 0, 0)));
    REQUIRE(setup.InsertNode(SampleMain, placeholder, MakeLayout(env, Placeholder, 0, 0)));
    REQUIRE(setup.AddNode(SampleMain, MakeNode(env, Id(62), env.typeSequence),
                          MakeLayout(env, Id(62), 0, 0)));
    REQUIRE(m.Run(setup));
    const Array<uint8> before = m.Bytes();

    // 현재 핀, 고아 핀(입력), 알 수 없는 종류의 출력(순번 5를 둔다), ordered 출력(끝에 붙인다).
    GraphCommandList edges(env.allocator);
    REQUIRE(edges.InsertEdge(SampleMain, MakeEdge(SampleParamA, env.pinValue, Inserted, env.pinA)));
    REQUIRE(edges.InsertEdge(SampleMain,
                             MakeEdge(SampleParamB, env.pinValue, Inserted, env.Intern("Old"))));
    REQUIRE(edges.InsertEdge(SampleMain,
                             MakeEdge(Placeholder, env.Intern("Out9"), Inserted, env.pinB, 5)));
    REQUIRE(
        edges.InsertEdge(SampleMain, MakeEdge(SampleSequence, env.pinThen, Id(62), env.pinIn, 7)));
    auto patch = m.Run(edges);
    REQUIRE(patch);
    uint32 unknownOrder = NumericLimits<uint32>::Max();
    uint32 orderedOrder = NumericLimits<uint32>::Max();
    for (const GraphEdge& edge : m.sample.Main().edges) {
        if (edge.fromNode == Placeholder) {
            unknownOrder = edge.order;
        }

        if (edge.fromNode == SampleSequence && edge.toNode == Id(62)) {
            orderedOrder = edge.order;
        }
    }

    CHECK(unknownOrder == 5);
    CHECK(orderedOrder == 2);
    const Array<uint8> after = m.Bytes();
    REQUIRE(m.mutator.Undo(TestSource, patch.Value()));
    CHECK(SameBytes(m.Bytes(), before));
    REQUIRE(m.mutator.Redo(TestSource, patch.Value()));
    CHECK(SameBytes(m.Bytes(), after));
    REQUIRE(m.mutator.Undo(TestSource, patch.Value()));

    const auto rejects = [&](GraphEdge edge, GraphCommandReason reason) noexcept {
        GraphCommandList list(env.allocator);
        return list.InsertEdge(SampleMain, edge) && Rejects(m, list, reason);
    };

    CHECK(rejects(MakeEdge(SampleParamA, env.pinValue, Inserted, env.Intern("Nope")),
                  GraphCommandReason::PinMissing));
    CHECK(rejects(MakeEdge(SampleParamA, env.pinValue, Inserted, env.pinResult),
                  GraphCommandReason::PinDirection));
    CHECK(rejects(MakeEdge(Inserted, env.Intern("Old"), SampleToggle, env.pinIn),
                  GraphCommandReason::PinDirection));
    CHECK(rejects(MakeEdge(SampleParamA, env.pinValue, SampleAdd, env.pinA),
                  GraphCommandReason::EdgeExists));
    CHECK(rejects(MakeEdge(SampleParamB, env.pinValue, SampleAdd, env.pinA),
                  GraphCommandReason::EdgeLimit));
    CHECK(rejects(MakeEdge(SampleReroute, env.pinOut, SampleReroute, env.pinIn),
                  GraphCommandReason::RerouteCycle));
    CHECK(rejects(MakeEdge(Id(98), env.pinValue, Inserted, env.pinA),
                  GraphCommandReason::NodeMissing));
    CHECK(
        rejects(MakeEdge(Placeholder, Name(), Inserted, env.pinB), GraphCommandReason::PinMissing));
}

// 재경유 노드의 고아 핀(GI13)·None 키(GI11)·목록 끝에 값도 간선도 없는 고아 핀(GI7)은 거부한다.
// 같은 목록에서 그 고아 핀에 간선을 넣으면 받는다. 고아 출력 핀 끝은 주어진 순번을 둔다.
TEST(NodeGraph_InsertNodeDataInvariants) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    const auto rejects = [&](GraphNode node, GraphCommandReason reason) noexcept {
        GraphCommandList list(env.allocator);
        const Guid id = node.id;
        return list.InsertNode(SampleMain, node, MakeLayout(env, id, 0, 0)) &&
               Rejects(m, list, reason);
    };

    GraphNode reroute = MakeNode(env, Inserted, env.reroute);
    REQUIRE(reroute.retainedPins.TryPushBack(
        Retained(env, env.Intern("Gone"), PinDirection::Input, true)));
    CHECK(rejects(Move(reroute), GraphCommandReason::NodeDataInvalid));
    GraphNode noneDefault = MakeNode(env, Inserted, env.typeUnknown);
    REQUIRE(noneDefault.inputDefaults.TryPushBack(Default(env, Name(), 1)));
    CHECK(rejects(Move(noneDefault), GraphCommandReason::NodeDataInvalid));
    GraphNode noneRetained = MakeNode(env, Inserted, env.typeUnknown);
    REQUIRE(
        noneRetained.retainedPins.TryPushBack(Retained(env, Name(), PinDirection::Input, true)));
    CHECK(rejects(Move(noneRetained), GraphCommandReason::NodeDataInvalid));
    const auto bare = [&env]() noexcept {
        GraphNode node = MakeNode(env, Inserted, env.typeAdd);
        static_cast<void>(node.retainedPins.TryPushBack(
            Retained(env, env.Intern("Spare"), PinDirection::Input, false)));
        return node;
    };
    CHECK(rejects(bare(), GraphCommandReason::NodeDataInvalid));

    // 같은 목록의 InsertEdge가 그 고아 핀에 닿으면 받는다.
    GraphCommandList joined(env.allocator);
    REQUIRE(joined.InsertNode(SampleMain, bare(), MakeLayout(env, Inserted, 0, 0)));
    REQUIRE(joined.InsertEdge(SampleMain,
                              MakeEdge(SampleParamA, env.pinValue, Inserted, env.Intern("Spare"))));
    REQUIRE(m.Run(joined));

    // 알려진 종류의 고아 출력 핀에서 나가는 간선은 주어진 순번을 둔다.
    GraphNode source = MakeNode(env, Placeholder, env.typeAdd);
    REQUIRE(source.retainedPins.TryPushBack(
        Retained(env, env.Intern("OldOut"), PinDirection::Output, true)));
    GraphCommandList output(env.allocator);
    REQUIRE(output.InsertNode(SampleMain, source, MakeLayout(env, Placeholder, 0, 0)));
    REQUIRE(output.InsertEdge(
        SampleMain, MakeEdge(Placeholder, env.Intern("OldOut"), SampleDefaults, env.pinA, 4)));
    REQUIRE(m.Run(output));
    bool found = false;
    for (const GraphEdge& edge : m.sample.Main().edges) {
        if (edge.fromNode == Placeholder) {
            found = edge.order == 4;
        }
    }

    CHECK(found);
}

// requireMake Connect는 schema 판정이 바꾸기면 ConnectDenied다(붙여넣기 바깥 간선).
TEST(NodeGraph_ConnectRequireMake) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    GraphCommandList replacing(env.allocator);
    REQUIRE(replacing.Connect(SampleMain, MakeEdge(SampleParamB, env.pinValue, SampleAdd, env.pinA),
                              true));
    CHECK(Rejects(m, replacing, GraphCommandReason::ConnectDenied));
    GraphCommandList free(env.allocator);
    REQUIRE(free.Connect(SampleMain, MakeEdge(SampleParamB, env.pinValue, SampleDefaults, env.pinA),
                         true));
    CHECK(m.Run(free));
}

} // namespace node_graph_mutator_insert_tests_file
