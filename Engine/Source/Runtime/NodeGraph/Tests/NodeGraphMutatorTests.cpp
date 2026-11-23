#include "NodeGraphMutatorTestSupport.h"

// GraphMutator 명령별 시험이다(Docs/Design/NodeGraph 8.6절, 13절 GT1·GT2): 명령마다 성공
// 사례는 통지 종류· semanticTouched·revision이 맞고 undo가 원래 바이트, redo가 적용 바이트를
// 만든다. 위반 사례는 정확한 사유로 거부되고 문서 바이트·revision이 그대로이며 통지가 없다. 목록의
// 뒤 명령이 실패하면 앞 명령도 되돌린다.
using namespace rocks;
namespace node_graph_mutator_tests_file {

using namespace rocks::nodegraph;
using namespace rocks::nodegraph_test;

using Kind = GraphChangeKind;

// list가 성공하고 통지 종류가 kinds, semanticTouched가 semantic이며, undo·redo가 바이트를 되돌리고
// 다시 만든다.
bool RoundTrip(MutatorEnv& m, const GraphCommandList& list, Span<const Kind> kinds,
               bool semantic) noexcept {
    const Array<uint8> before = m.Bytes();
    const uint64 revision = m.mutator.Revision();
    auto patch = m.Run(list);
    if (!patch) {
        return false;
    }

    const Array<uint8> after = m.Bytes();
    const RecordedBatch& applied = m.recorder.Last();
    bool ok = m.recorder.LastKinds(kinds) && applied.semanticTouched == semantic &&
              applied.cause == GraphChangeCause::Edit && !applied.interactive &&
              applied.revision == revision + 1 && m.mutator.Revision() == revision + 1 &&
              patch.Value().IsApplied();
    ok = ok && m.mutator.Undo(TestSource, patch.Value()) && SameBytes(m.Bytes(), before) &&
         m.recorder.Last().cause == GraphChangeCause::Undo &&
         m.recorder.Last().changes.Size() == kinds.Size() &&
         m.recorder.Last().semanticTouched == semantic && !patch.Value().IsApplied();
    ok = ok && m.mutator.Redo(TestSource, patch.Value()) && SameBytes(m.Bytes(), after) &&
         m.recorder.Last().cause == GraphChangeCause::Redo && m.mutator.Revision() == revision + 3;
    return ok && !m.recorder.failed;
}

// list가 reason으로 거부되고 문서·revision·통지가 그대로다.
bool Rejects(MutatorEnv& m, const GraphCommandList& list, GraphCommandReason reason,
             uint32 command = 0) noexcept {
    const Array<uint8> before = m.Bytes();
    const uint64 revision = m.mutator.Revision();
    const usize batches = m.recorder.batches.Size();
    GraphCommandFailure failure;
    auto patch = m.Run(list, &failure);
    return !patch && patch.Error() == ErrorCode::InvalidArgument && failure.reason == reason &&
           failure.command == command && SameBytes(m.Bytes(), before) &&
           m.mutator.Revision() == revision && m.recorder.batches.Size() == batches;
}

template <usize Count>
Span<const Kind> Kinds(const Kind (&kinds)[Count]) noexcept {
    return Span<const Kind>(kinds, Count);
}

GraphNode NewNode(TestEnv& env, Guid id, Name type, uint32 version = 1) noexcept {
    return MakeNode(env, id, type, version);
}

TEST(NodeGraph_MutatorInitializeAndRevision) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    CHECK(m.mutator.Revision() == 0);
    auto again = m.mutator.Initialize();
    REQUIRE(!again);
    CHECK(again.Error() == ErrorCode::AlreadyExists);

    // revision이 다르면 StaleHandle, 문서는 그대로다.
    GraphCommandList list(env.allocator);
    REQUIRE(list.SetLayout(SampleMain, SampleAdd, MakeLayout(env, SampleAdd, 1, 1)));
    auto stale = m.mutator.Execute(TestSource, 5, list);
    REQUIRE(!stale);
    CHECK(stale.Error() == ErrorCode::StaleHandle);

    // Initialize 전 mutator는 Unsupported, 불변식을 어긴 문서는 Initialize가 거부한다.
    Sample broken(env);
    broken.document.documentFormatVersion = 0;
    SeededGraphIdSource ids(1, Id(2));
    GraphMutator invalid(broken.document, broken.layout, env.registry, ids);
    auto early = invalid.Execute(TestSource, 0, list);
    REQUIRE(!early);
    CHECK(early.Error() == ErrorCode::Unsupported);
    auto rejected = invalid.Initialize();
    REQUIRE(!rejected);
    CHECK(rejected.Error() == ErrorCode::InvalidArgument);

    // 빈 목록도 revision을 올리고 빈 통지 묶음을 보낸다.
    GraphCommandList empty(env.allocator);
    auto patch = m.Run(empty);
    REQUIRE(patch);
    CHECK(patch.Value().IsEmpty());
    CHECK(m.mutator.Revision() == 1);
    CHECK(m.recorder.Last().changes.IsEmpty() && !m.recorder.Last().semanticTouched);
}

TEST(NodeGraph_MutatorAddNode) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    GraphCommandList list(env.allocator);
    REQUIRE(
        list.AddNode(SampleMain, NewNode(env, Id(50), env.typeAdd), MakeLayout(env, Id(50), 3, 4)));
    const Kind added[] = {Kind::NodeAdded};
    CHECK(RoundTrip(m, list, Kinds(added), true));

    // 비활성 허용 종류는 Disabled로 더할 수 있다.
    GraphCommandList toggle(env.allocator);
    GraphNode disabled = NewNode(env, Id(51), env.typeToggle);
    disabled.enableState = NodeEnableState::Disabled;
    REQUIRE(toggle.AddNode(SampleMain, disabled, MakeLayout(env, Id(51), 0, 0)));
    CHECK(RoundTrip(m, toggle, Kinds(added), true));

    auto rejects = [&](Guid graph, GraphNode&& node, GraphCommandReason reason) noexcept {
        GraphCommandList bad(env.allocator);
        return bad.AddNode(graph, node, MakeLayout(env, node.id, 0, 0)) && Rejects(m, bad, reason);
    };

    CHECK(rejects(Id(99), NewNode(env, Id(60), env.typeAdd), GraphCommandReason::GraphMissing));
    CHECK(rejects(SampleMain, NewNode(env, Guid{}, env.typeAdd), GraphCommandReason::IdZero));
    CHECK(rejects(SampleMain, NewNode(env, Id(50), env.typeAdd), GraphCommandReason::NodeExists));
    CHECK(rejects(SampleMain, NewNode(env, SampleComment, env.typeAdd),
                  GraphCommandReason::NodeExists));
    CHECK(rejects(SampleMain, NewNode(env, SampleCollapsed, env.typeAdd),
                  GraphCommandReason::NodeExists));
    CHECK(rejects(SampleMain, NewNode(env, Id(60), env.typeUnknown),
                  GraphCommandReason::TypeUnknown));
    CHECK(rejects(SampleMain, NewNode(env, Id(60), Name()), GraphCommandReason::TypeUnknown));
    CHECK(rejects(SampleMain, NewNode(env, Id(60), env.typeParam, 1),
                  GraphCommandReason::TypeVersion));
    CHECK(rejects(SampleMain, NewNode(env, Id(60), env.typeParam, TestParamVersion),
                  GraphCommandReason::PropertiesSchema));
    GraphNode withDefault = NewNode(env, Id(60), env.typeAdd);
    PinValueEntry value(env.allocator);
    value.pin = env.pinA;
    REQUIRE(FillRecord(value.value.bytes, TestValueStruct, 1, 1));
    REQUIRE(withDefault.inputDefaults.TryPushBack(Move(value)));
    CHECK(rejects(SampleMain, Move(withDefault), GraphCommandReason::NodeValues));
    GraphNode disabledAdd = NewNode(env, Id(60), env.typeAdd);
    disabledAdd.enableState = NodeEnableState::Disabled;
    CHECK(rejects(SampleMain, Move(disabledAdd), GraphCommandReason::EnableState));
    GraphNode dynamicBad = NewNode(env, Id(60), env.typeDynamic);
    REQUIRE(FillRecord(dynamicBad.properties.bytes, TestDynamicStruct, TestDynamicFailVersion, 0));
    CHECK(rejects(SampleMain, Move(dynamicBad), GraphCommandReason::PropertiesSchema));

    // 문서당 하나인 종류.
    const PinDescriptor outputPins[] = {InputPin(env.pinA, TestFloatType, "A")};
    NodeTypeDescriptor single;
    single.type = env.Intern("Test.Single");
    single.staticPins = outputPins;
    single.capabilities = DefaultNodeCapabilities | NodeCapabilities::SinglePerDocument;
    REQUIRE(env.registry.RegisterNodeType(env.domain, single, ModuleLease()));
    GraphCommandList first(env.allocator);
    REQUIRE(first.AddNode(SampleCollapsed, NewNode(env, Id(70), single.type),
                          MakeLayout(env, Id(70), 0, 0)));
    REQUIRE(m.Run(first));
    CHECK(rejects(SampleMain, NewNode(env, Id(71), single.type),
                  GraphCommandReason::SinglePerDocument));
}

// 목록의 뒤 명령이 실패하면 앞 명령의 변경을 되돌린다(문서 바이트 그대로, 실패 번호는 뒤 명령).
TEST(NodeGraph_MutatorListIsAtomic) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    GraphCommandList list(env.allocator);
    REQUIRE(
        list.AddNode(SampleMain, NewNode(env, Id(50), env.typeAdd), MakeLayout(env, Id(50), 0, 0)));
    REQUIRE(list.Connect(SampleMain, MakeEdge(SampleParamB, env.pinValue, Id(50), env.pinA)));
    REQUIRE(list.SetLayout(SampleMain, Id(50), MakeLayout(env, Id(50), 9, 9)));
    REQUIRE(list.RemoveNodes(SampleMain, Span<const Guid>(&SampleParamB, 1)));
    REQUIRE(
        list.AddNode(SampleMain, NewNode(env, Id(50), env.typeAdd), MakeLayout(env, Id(50), 0, 0)));
    CHECK(Rejects(m, list, GraphCommandReason::NodeExists, 4));
}

TEST(NodeGraph_MutatorRemoveNodes) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    // Add: 들어오는 간선 둘(A·B), 나가는 간선 하나(Result → Toggle).
    GraphCommandList add(env.allocator);
    REQUIRE(add.RemoveNodes(SampleMain, Span<const Guid>(&SampleAdd, 1)));
    const Kind addKinds[] = {Kind::EdgeRemoved, Kind::EdgeRemoved, Kind::EdgeRemoved,
                             Kind::NodeRemoved};
    CHECK(RoundTrip(m, add, Kinds(addKinds), true));

    // StepA를 빼면 Sequence.Then의 남은 간선(StepB) 순번이 1 → 0이 된다.
    GraphCommandList step(env.allocator);
    REQUIRE(step.RemoveNodes(SampleMain, Span<const Guid>(&SampleStepA, 1)));
    const Kind stepKinds[] = {Kind::EdgeRemoved, Kind::EdgeOrderChanged, Kind::NodeRemoved};
    CHECK(RoundTrip(m, step, Kinds(stepKinds), true));

    // 알 수 없는 종류·재경유도 뺄 수 있다. 여러 노드를 한 명령으로(표본 그대로의 새 환경).
    MutatorEnv fresh(env);
    REQUIRE(fresh.ready);
    const Guid many[] = {SampleUnknown, SampleReroute};
    GraphCommandList several(env.allocator);
    REQUIRE(several.RemoveNodes(SampleMain, Span<const Guid>(many, 2)));
    const Kind severalKinds[] = {Kind::EdgeRemoved, Kind::NodeRemoved, Kind::EdgeRemoved,
                                 Kind::EdgeRemoved, Kind::NodeRemoved};
    CHECK(RoundTrip(fresh, several, Kinds(severalKinds), true));

    GraphCommandList missing(env.allocator);
    const Guid nobody = Id(99);
    REQUIRE(missing.RemoveNodes(SampleMain, Span<const Guid>(&nobody, 1)));
    CHECK(Rejects(m, missing, GraphCommandReason::NodeMissing));
    GraphCommandList noGraph(env.allocator);
    REQUIRE(noGraph.RemoveNodes(Id(99), Span<const Guid>(&SampleAdd, 1)));
    CHECK(Rejects(m, noGraph, GraphCommandReason::GraphMissing));

    // 삭제 불가 종류.
    NodeTypeDescriptor fixed;
    fixed.type = env.Intern("Test.Fixed");
    fixed.capabilities = NodeCapabilities::CanDuplicate;
    REQUIRE(env.registry.RegisterNodeType(env.domain, fixed, ModuleLease()));
    GraphCommandList addFixed(env.allocator);
    REQUIRE(addFixed.AddNode(SampleMain, NewNode(env, Id(80), fixed.type),
                             MakeLayout(env, Id(80), 0, 0)));
    REQUIRE(m.Run(addFixed));
    GraphCommandList removeFixed(env.allocator);
    const Guid fixedId = Id(80);
    REQUIRE(removeFixed.RemoveNodes(SampleMain, Span<const Guid>(&fixedId, 1)));
    CHECK(Rejects(m, removeFixed, GraphCommandReason::CannotDelete));
}

// 자기 자신으로 가는 간선(self-loop — 순환 금지는 도메인 판정이다)이 있는 노드를 지우면 그 간선을
// 한 번만 뺀다 (무작위 명령열이 찾은 결함의 회귀 시험).
TEST(NodeGraph_MutatorRemoveNodeWithSelfLoop) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    GraphCommandList loop(env.allocator);
    REQUIRE(loop.Connect(SampleMain, MakeEdge(SampleAdd, env.pinResult, SampleAdd, env.pinB)));
    REQUIRE(m.Run(loop));
    GraphCommandList remove(env.allocator);
    REQUIRE(remove.RemoveNodes(SampleMain, Span<const Guid>(&SampleAdd, 1)));
    // 들어오는 A·B(self-loop), 나가는 Result → Toggle(self-loop는 다시 세지 않는다).
    const Kind kinds[] = {Kind::EdgeRemoved, Kind::EdgeRemoved, Kind::EdgeRemoved,
                          Kind::NodeRemoved};
    CHECK(RoundTrip(m, remove, Kinds(kinds), true));
}

TEST(NodeGraph_MutatorConnect) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    const Kind added[] = {Kind::EdgeAdded};
    GraphCommandList plain(env.allocator);
    REQUIRE(
        plain.Connect(SampleMain, MakeEdge(SampleParamB, env.pinValue, SampleDefaults, env.pinA)));
    CHECK(RoundTrip(m, plain, Kinds(added), true));

    // 차 있는 단일 입력은 기본 판정이 바꾸기다(UE 기본과 같다).
    GraphCommandList replace(env.allocator);
    REQUIRE(replace.Connect(SampleMain, MakeEdge(SampleParamB, env.pinValue, SampleAdd, env.pinA)));
    const Kind replaced[] = {Kind::EdgeRemoved, Kind::EdgeAdded};
    CHECK(RoundTrip(m, replace, Kinds(replaced), true));

    // ordered 출력 핀은 끝에 붙인다(순번 2).
    GraphCommandList ordered(env.allocator);
    REQUIRE(ordered.AddNode(SampleMain, NewNode(env, Id(50), env.typeSequence),
                            MakeLayout(env, Id(50), 0, 0)));
    REQUIRE(
        ordered.Connect(SampleMain, MakeEdge(SampleSequence, env.pinThen, Id(50), env.pinIn, 7)));
    REQUIRE(m.Run(ordered));
    bool sawOrder = false;
    for (const GraphEdge& edge : m.sample.Main().edges) {
        if (edge.toNode == Id(50)) {
            sawOrder = edge.order == 2;
        }
    }

    CHECK(sawOrder);

    auto rejects = [&](const GraphEdge& edge, GraphCommandReason reason) noexcept {
        GraphCommandList bad(env.allocator);
        return bad.Connect(SampleMain, edge) && Rejects(m, bad, reason);
    };

    CHECK(rejects(MakeEdge(Id(99), env.pinValue, SampleAdd, env.pinA),
                  GraphCommandReason::NodeMissing));
    CHECK(rejects(MakeEdge(SampleParamA, env.pinValue, Id(99), env.pinA),
                  GraphCommandReason::NodeMissing));
    CHECK(rejects(MakeEdge(SampleParamA, env.pinValue, SampleUnknown, env.pinA),
                  GraphCommandReason::TypeUnknown));
    CHECK(rejects(MakeEdge(SampleUnknown, env.pinOther, SampleDefaults, env.pinA),
                  GraphCommandReason::TypeUnknown));
    CHECK(rejects(MakeEdge(SampleParamA, env.pinOther, SampleDefaults, env.pinA),
                  GraphCommandReason::PinMissing));
    CHECK(rejects(MakeEdge(SampleParamA, env.pinValue, SampleAdd, env.pinGone),
                  GraphCommandReason::RetainedPin));
    CHECK(rejects(MakeEdge(SampleAdd, env.pinA, SampleDefaults, env.pinA),
                  GraphCommandReason::PinDirection));
    CHECK(rejects(MakeEdge(SampleParamB, env.pinValue, SampleDefaults, env.pinResult),
                  GraphCommandReason::PinDirection));
    CHECK(rejects(MakeEdge(SampleParamB, env.pinValue, SampleAdd, env.pinA),
                  GraphCommandReason::EdgeExists));
    // Flow 출력 → Data 입력은 기본 판정이 거부한다.
    CHECK(rejects(MakeEdge(SampleSequence, env.pinThen, SampleDefaults, env.pinA),
                  GraphCommandReason::ConnectDenied));
    env.schema.connect = TestConnect::Deny;
    CHECK(rejects(MakeEdge(SampleParamA, env.pinValue, SampleDefaults, env.pinB),
                  GraphCommandReason::ConnectDenied));
    // 판정이 Make인데 입력이 차 있으면 상한 위반이다.
    env.schema.connect = TestConnect::AlwaysMake;
    CHECK(rejects(MakeEdge(SampleParamA, env.pinValue, SampleDefaults, env.pinB),
                  GraphCommandReason::EdgeLimit));
    env.schema.connect = TestConnect::Default;
}

TEST(NodeGraph_MutatorConnectReplaceAndConvert) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    // 출력 바꾸기: Sequence.Then의 기존 간선 둘을 빼고 새 간선만 남긴다(순번 0).
    env.schema.connect = TestConnect::ReplaceOutput;
    GraphCommandList output(env.allocator);
    REQUIRE(output.AddNode(SampleMain, NewNode(env, Id(50), env.typeSequence),
                           MakeLayout(env, Id(50), 0, 0)));
    REQUIRE(output.Connect(SampleMain, MakeEdge(SampleSequence, env.pinThen, Id(50), env.pinIn)));
    const Kind outputKinds[] = {Kind::NodeAdded, Kind::EdgeRemoved, Kind::EdgeRemoved,
                                Kind::EdgeAdded};
    CHECK(RoundTrip(m, output, Kinds(outputKinds), true));

    // 양쪽 바꾸기: Add.A(ParamA)와 ParamB의 Value 출력 간선들을 빼고 잇는다.
    env.schema.connect = TestConnect::ReplaceBoth;
    GraphCommandList both(env.allocator);
    REQUIRE(both.Connect(SampleMain, MakeEdge(SampleParamB, env.pinValue, SampleAdd, env.pinA)));
    const Kind bothKinds[] = {Kind::EdgeRemoved, Kind::EdgeRemoved, Kind::EdgeAdded};
    CHECK(RoundTrip(m, both, Kinds(bothKinds), true));

    // 변환 노드: Test.Convert를 끼워 잇는다(id는 seed 발급기 — 같은 seed면 같은 id).
    env.schema.connect = TestConnect::Convert;
    env.schema.conversionType = env.typeConvert;
    GraphCommandList convert(env.allocator);
    REQUIRE(convert.Connect(SampleMain,
                            MakeEdge(SampleParamB, env.pinValue, SampleDefaults, env.pinA)));
    const Kind convertKinds[] = {Kind::NodeAdded, Kind::EdgeAdded, Kind::EdgeAdded};
    SeededGraphIdSource expectedIds(TestIdSeed, Id(0xA55E7));
    auto expectedId = expectedIds.Next();
    REQUIRE(expectedId);
    const Array<uint8> before = m.Bytes();
    auto patch = m.Run(convert);
    REQUIRE(patch);
    CHECK(m.recorder.LastKinds(Kinds(convertKinds)));
    CHECK(m.recorder.Last().changes[0].node == expectedId.Value());
    REQUIRE(m.mutator.Undo(TestSource, patch.Value()));
    CHECK(SameBytes(m.Bytes(), before));

    // 변환 종류가 없거나 속성이 있는 종류면 거부한다.
    env.schema.conversionType = env.typeUnknown;
    CHECK(Rejects(m, convert, GraphCommandReason::ConversionInvalid));
    env.schema.conversionType = env.typeParam;
    CHECK(Rejects(m, convert, GraphCommandReason::ConversionInvalid));
    env.schema.connect = TestConnect::Default;
}

TEST(NodeGraph_MutatorDisconnectAndReorder) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    // StepA 간선을 끊으면 StepB 순번이 0으로 당겨진다.
    GraphCommandList cut(env.allocator);
    REQUIRE(
        cut.Disconnect(SampleMain, MakeEdge(SampleSequence, env.pinThen, SampleStepA, env.pinIn)));
    const Kind cutKinds[] = {Kind::EdgeRemoved, Kind::EdgeOrderChanged};
    CHECK(RoundTrip(m, cut, Kinds(cutKinds), true));
    GraphCommandList missing(env.allocator);
    REQUIRE(missing.Disconnect(SampleMain,
                               MakeEdge(SampleParamA, env.pinValue, SampleStepA, env.pinIn)));
    CHECK(Rejects(m, missing, GraphCommandReason::EdgeMissing));

    // 순서 바꾸기: [StepB, StepA](끊기 전 표본에서).
    MutatorEnv fresh(env);
    REQUIRE(fresh.ready);
    const GraphEdge swapped[] = {MakeEdge(SampleSequence, env.pinThen, SampleStepB, env.pinIn),
                                 MakeEdge(SampleSequence, env.pinThen, SampleStepA, env.pinIn)};
    GraphCommandList reorder(env.allocator);
    REQUIRE(reorder.ReorderEdges(SampleMain, SampleSequence, env.pinThen,
                                 Span<const GraphEdge>(swapped, 2)));
    const Kind reorderKinds[] = {Kind::EdgeOrderChanged};
    CHECK(RoundTrip(fresh, reorder, Kinds(reorderKinds), true));

    auto rejects = [&](Guid node, Name pin, Span<const GraphEdge> order,
                       GraphCommandReason reason) noexcept {
        GraphCommandList bad(env.allocator);
        return bad.ReorderEdges(SampleMain, node, pin, order) && Rejects(fresh, bad, reason);
    };

    CHECK(
        rejects(SampleAdd, env.pinResult, Span<const GraphEdge>(), GraphCommandReason::NotOrdered));
    CHECK(rejects(SampleSequence, env.pinOther, Span<const GraphEdge>(),
                  GraphCommandReason::PinMissing));
    CHECK(rejects(Id(99), env.pinThen, Span<const GraphEdge>(), GraphCommandReason::NodeMissing));
    CHECK(rejects(SampleUnknown, env.pinThen, Span<const GraphEdge>(),
                  GraphCommandReason::TypeUnknown));
    CHECK(rejects(SampleSequence, env.pinThen, Span<const GraphEdge>(swapped, 1),
                  GraphCommandReason::OrderMismatch));
    const GraphEdge duplicate[] = {swapped[0], swapped[0]};
    CHECK(rejects(SampleSequence, env.pinThen, Span<const GraphEdge>(duplicate, 2),
                  GraphCommandReason::OrderMismatch));
}

TEST(NodeGraph_MutatorSetNodeProperties) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    Array<uint8> bytes(env.allocator);
    REQUIRE(FillRecord(bytes, TestParamStruct, TestParamVersion, 77));
    GraphCommandList param(env.allocator);
    REQUIRE(param.SetNodeProperties(SampleMain, SampleParamA, bytes.View()));
    const Kind propertyKinds[] = {Kind::NodePropertiesChanged};
    CHECK(RoundTrip(m, param, Kinds(propertyKinds), true));

    // 동적 핀이 늘면 NodePinsChanged도 낸다.
    REQUIRE(FillRecord(bytes, TestDynamicStruct, 3, 0));
    GraphCommandList grow(env.allocator);
    REQUIRE(grow.SetNodeProperties(SampleMain, SampleDynamic, bytes.View()));
    const Kind growKinds[] = {Kind::NodePropertiesChanged, Kind::NodePinsChanged};
    CHECK(RoundTrip(m, grow, Kinds(growKinds), true));

    auto rejects = [&](Guid node, Guid schema, uint32 version, GraphCommandReason reason) noexcept {
        Array<uint8> record(env.allocator);
        GraphCommandList bad(env.allocator);
        return FillRecord(record, schema, version, 0) &&
               bad.SetNodeProperties(SampleMain, node, record.View()) && Rejects(m, bad, reason);
    };

    // 간선이 있는 동적 핀(Item_1)이 사라진다.
    CHECK(rejects(SampleDynamic, TestDynamicStruct, 1, GraphCommandReason::PinsDropped));
    CHECK(rejects(SampleParamA, TestValueStruct, 1, GraphCommandReason::PropertiesSchema));
    CHECK(rejects(SampleDynamic, TestDynamicStruct, TestDynamicFailVersion,
                  GraphCommandReason::PropertiesSchema));
    CHECK(rejects(SampleUnknown, TestValueStruct, 1, GraphCommandReason::TypeUnknown));
    CHECK(rejects(Id(99), TestValueStruct, 1, GraphCommandReason::NodeMissing));

    // 입력 기본값이 있는 동적 핀이 사라진다.
    GraphCommandList value(env.allocator);
    REQUIRE(FillRecord(bytes, TestValueStruct, 1, 5));
    REQUIRE(value.Disconnect(SampleMain,
                             MakeEdge(SampleParamA, env.pinValue, SampleDynamic, env.pinItem1)));
    REQUIRE(value.SetInputDefault(SampleMain, SampleDynamic, env.pinItem1, bytes.View()));
    REQUIRE(m.Run(value));
    CHECK(rejects(SampleDynamic, TestDynamicStruct, 1, GraphCommandReason::PinsDropped));
}

TEST(NodeGraph_MutatorInputDefaults) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    Array<uint8> bytes(env.allocator);
    REQUIRE(FillRecord(bytes, TestValueStruct, 1, 9));
    const Kind changed[] = {Kind::InputDefaultChanged};
    GraphCommandList add(env.allocator);
    REQUIRE(add.SetInputDefault(SampleMain, SampleDefaults, env.pinB, bytes.View()));
    CHECK(RoundTrip(m, add, Kinds(changed), true));
    GraphCommandList replace(env.allocator);
    REQUIRE(replace.SetInputDefault(SampleMain, SampleDefaults, env.pinA, bytes.View()));
    CHECK(RoundTrip(m, replace, Kinds(changed), true));
    GraphCommandList reset(env.allocator);
    REQUIRE(reset.ResetInputDefault(SampleMain, SampleDefaults, env.pinA));
    CHECK(RoundTrip(m, reset, Kinds(changed), true));

    auto rejects = [&](Guid node, Name pin, Span<const uint8> value,
                       GraphCommandReason reason) noexcept {
        GraphCommandList bad(env.allocator);
        return bad.SetInputDefault(SampleMain, node, pin, value) && Rejects(m, bad, reason);
    };

    CHECK(rejects(SampleDefaults, env.pinOther, bytes.View(), GraphCommandReason::PinMissing));
    CHECK(rejects(SampleDefaults, env.pinResult, bytes.View(), GraphCommandReason::PinDirection));
    CHECK(rejects(SampleReroute, env.pinIn, bytes.View(), GraphCommandReason::ValueHidden));
    CHECK(rejects(SampleDefaults, env.pinB, Span<const uint8>(), GraphCommandReason::ValueEmpty));
    CHECK(rejects(SampleUnknown, env.pinA, bytes.View(), GraphCommandReason::TypeUnknown));
    CHECK(rejects(Id(99), env.pinA, bytes.View(), GraphCommandReason::NodeMissing));
    GraphCommandList missing(env.allocator);
    REQUIRE(missing.ResetInputDefault(SampleMain, SampleDefaults, env.pinOther));
    CHECK(Rejects(m, missing, GraphCommandReason::DefaultMissing));
}

TEST(NodeGraph_MutatorLayoutCommentBookmark) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    GraphCommandList layout(env.allocator);
    REQUIRE(layout.SetLayout(SampleMain, SampleAdd, MakeLayout(env, SampleAdd, 123, 456)));
    const Kind layoutKinds[] = {Kind::NodeLayoutChanged};
    CHECK(RoundTrip(m, layout, Kinds(layoutKinds), false));
    GraphCommandList noNode(env.allocator);
    REQUIRE(noNode.SetLayout(SampleMain, Id(99), MakeLayout(env, Id(99), 0, 0)));
    CHECK(Rejects(m, noNode, GraphCommandReason::NodeMissing));

    const Kind commentKinds[] = {Kind::CommentChanged};
    CommentBox comment(env.allocator);
    comment.id = Id(41);
    REQUIRE(comment.text.TryAssign("Note"));
    GraphCommandList addComment(env.allocator);
    REQUIRE(addComment.AddComment(SampleCollapsed, comment));
    CHECK(RoundTrip(m, addComment, Kinds(commentKinds), false));
    REQUIRE(comment.text.TryAssign("Changed"));
    comment.id = SampleComment;
    GraphCommandList setComment(env.allocator);
    REQUIRE(setComment.SetComment(SampleMain, comment));
    CHECK(RoundTrip(m, setComment, Kinds(commentKinds), false));
    GraphCommandList removeComment(env.allocator);
    REQUIRE(removeComment.RemoveComment(SampleMain, SampleComment));
    CHECK(RoundTrip(m, removeComment, Kinds(commentKinds), false));
    comment.id = SampleAdd;
    GraphCommandList clash(env.allocator);
    REQUIRE(clash.AddComment(SampleMain, comment));
    CHECK(Rejects(m, clash, GraphCommandReason::CommentExists));
    comment.id = Guid{};
    GraphCommandList zero(env.allocator);
    REQUIRE(zero.AddComment(SampleMain, comment));
    CHECK(Rejects(m, zero, GraphCommandReason::IdZero));
    GraphCommandList gone(env.allocator);
    REQUIRE(gone.RemoveComment(SampleCollapsed, SampleComment));
    CHECK(Rejects(m, gone, GraphCommandReason::CommentMissing));
    comment.id = Id(98);
    GraphCommandList setGone(env.allocator);
    REQUIRE(setGone.SetComment(SampleMain, comment));
    CHECK(Rejects(m, setGone, GraphCommandReason::CommentMissing));

    const Kind bookmarkKinds[] = {Kind::BookmarkChanged};
    Bookmark mark;
    mark.number = 2;
    mark.zoom = 0.25f;
    GraphCommandList addMark(env.allocator);
    REQUIRE(addMark.SetBookmark(SampleMain, mark));
    CHECK(RoundTrip(m, addMark, Kinds(bookmarkKinds), false));
    mark.number = 1;
    GraphCommandList setMark(env.allocator);
    REQUIRE(setMark.SetBookmark(SampleMain, mark));
    CHECK(RoundTrip(m, setMark, Kinds(bookmarkKinds), false));
    GraphCommandList clearMark(env.allocator);
    REQUIRE(clearMark.ClearBookmark(SampleMain, 3));
    CHECK(RoundTrip(m, clearMark, Kinds(bookmarkKinds), false));
    // 없는 북마크 지우기는 무동작이다(op 없음).
    GraphCommandList clearNone(env.allocator);
    REQUIRE(clearNone.ClearBookmark(SampleMain, 9));
    auto none = m.Run(clearNone);
    REQUIRE(none);
    CHECK(none.Value().IsEmpty());
    GraphCommandList noGraph(env.allocator);
    REQUIRE(noGraph.SetBookmark(Id(99), mark));
    CHECK(Rejects(m, noGraph, GraphCommandReason::GraphMissing));
}

TEST(NodeGraph_MutatorEnableStateAndRetainedPin) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    GraphCommandList enable(env.allocator);
    REQUIRE(enable.SetEnableState(SampleMain, SampleToggle, NodeEnableState::Enabled));
    const Kind enableKinds[] = {Kind::NodeEnableStateChanged};
    CHECK(RoundTrip(m, enable, Kinds(enableKinds), true));
    // 같은 상태는 무동작이다(redo 뒤 Toggle은 Enabled다).
    GraphCommandList same(env.allocator);
    REQUIRE(same.SetEnableState(SampleMain, SampleToggle, NodeEnableState::Enabled));
    auto none = m.Run(same);
    REQUIRE(none);
    CHECK(none.Value().IsEmpty());

    auto rejects = [&](Guid node, NodeEnableState state, GraphCommandReason reason) noexcept {
        GraphCommandList bad(env.allocator);
        return bad.SetEnableState(SampleMain, node, state) && Rejects(m, bad, reason);
    };

    CHECK(rejects(SampleAdd, NodeEnableState::Disabled, GraphCommandReason::EnableState));
    CHECK(rejects(SampleAdd, static_cast<NodeEnableState>(7), GraphCommandReason::EnableState));
    CHECK(rejects(SampleUnknown, NodeEnableState::Disabled, GraphCommandReason::TypeUnknown));
    CHECK(rejects(Id(99), NodeEnableState::Enabled, GraphCommandReason::NodeMissing));

    // 고아 핀 지우기(간선이 있으면 함께 뺀다).
    GraphCommandList link(env.allocator);
    REQUIRE(
        link.Connect(SampleMain, MakeEdge(SampleParamB, env.pinValue, SampleDefaults, env.pinA)));
    REQUIRE(m.Run(link));
    GraphCommandList removeRetained(env.allocator);
    REQUIRE(removeRetained.RemoveRetainedPin(SampleMain, SampleAdd, env.pinGone));
    const Kind retainedKinds[] = {Kind::RetainedPinChanged};
    CHECK(RoundTrip(m, removeRetained, Kinds(retainedKinds), true));
    GraphCommandList missing(env.allocator);
    REQUIRE(missing.RemoveRetainedPin(SampleMain, SampleAdd, env.pinOther));
    CHECK(Rejects(m, missing, GraphCommandReason::RetainedMissing));
}

TEST(NodeGraph_MutatorGraphs) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    GraphCommandList add(env.allocator);
    REQUIRE(add.AddGraph(Id(90), GraphRole::DomainDefined, 4, "State"));
    const Kind addKinds[] = {Kind::GraphAdded};
    CHECK(RoundTrip(m, add, Kinds(addKinds), true));
    GraphCommandList rename(env.allocator);
    REQUIRE(rename.RenameGraph(SampleCollapsed, "Renamed"));
    const Kind renameKinds[] = {Kind::GraphRenamed};
    CHECK(RoundTrip(m, rename, Kinds(renameKinds), false));

    // 하위 그래프 지우기: 노드·주석·북마크를 먼저 뺀다.
    GraphCommandList fill(env.allocator);
    CommentBox comment(env.allocator);
    comment.id = Id(42);
    Bookmark mark;
    mark.number = 5;
    REQUIRE(fill.AddComment(SampleCollapsed, comment));
    REQUIRE(fill.SetBookmark(SampleCollapsed, mark));
    REQUIRE(fill.AddNode(SampleCollapsed, NewNode(env, Id(91), env.typeAdd),
                         MakeLayout(env, Id(91), 0, 0)));
    REQUIRE(fill.Connect(SampleCollapsed, MakeEdge(Id(91), env.pinResult, SampleInner, env.pinA)));
    REQUIRE(m.Run(fill));
    GraphCommandList remove(env.allocator);
    REQUIRE(remove.RemoveGraph(SampleCollapsed));
    const Kind removeKinds[] = {Kind::EdgeRemoved,    Kind::NodeRemoved,     Kind::NodeRemoved,
                                Kind::CommentChanged, Kind::BookmarkChanged, Kind::GraphRemoved};
    CHECK(RoundTrip(m, remove, Kinds(removeKinds), true));

    auto rejects = [&](GraphCommandList& bad, GraphCommandReason reason) noexcept {
        return Rejects(m, bad, reason);
    };
    GraphCommandList exists(env.allocator);
    REQUIRE(exists.AddGraph(Id(90), GraphRole::Collapsed, 0, ""));
    CHECK(rejects(exists, GraphCommandReason::GraphExists));
    GraphCommandList nodeId(env.allocator);
    REQUIRE(nodeId.AddGraph(SampleAdd, GraphRole::Collapsed, 0, ""));
    CHECK(rejects(nodeId, GraphCommandReason::GraphExists));
    GraphCommandList zero(env.allocator);
    REQUIRE(zero.AddGraph(Guid{}, GraphRole::Collapsed, 0, ""));
    CHECK(rejects(zero, GraphCommandReason::IdZero));
    GraphCommandList main(env.allocator);
    REQUIRE(main.AddGraph(Id(92), GraphRole::Main, 0, ""));
    CHECK(rejects(main, GraphCommandReason::GraphRole));
    GraphCommandList domainRole(env.allocator);
    REQUIRE(domainRole.AddGraph(Id(92), GraphRole::Collapsed, 3, ""));
    CHECK(rejects(domainRole, GraphCommandReason::GraphRole));
    GraphCommandList removeMain(env.allocator);
    REQUIRE(removeMain.RemoveGraph(SampleMain));
    CHECK(rejects(removeMain, GraphCommandReason::MainGraph));
    GraphCommandList removeMissing(env.allocator);
    REQUIRE(removeMissing.RemoveGraph(Id(93)));
    CHECK(rejects(removeMissing, GraphCommandReason::GraphMissing));
    GraphCommandList renameMissing(env.allocator);
    REQUIRE(renameMissing.RenameGraph(Id(93), "x"));
    CHECK(rejects(renameMissing, GraphCommandReason::GraphMissing));
}

TEST(NodeGraph_MutatorMoveNodesToGraph) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    // Sequence·StepA·StepB(그 사이 간선 둘)를 하위 그래프로 옮긴다.
    const Guid group[] = {SampleSequence, SampleStepA, SampleStepB};
    GraphCommandList move(env.allocator);
    REQUIRE(move.MoveNodesToGraph(SampleMain, Span<const Guid>(group, 3), SampleCollapsed));
    const Kind moveKinds[] = {
        Kind::EdgeRemoved, Kind::EdgeRemoved, Kind::NodeRemoved, Kind::NodeAdded, Kind::NodeRemoved,
        Kind::NodeAdded,   Kind::NodeRemoved, Kind::NodeAdded,   Kind::EdgeAdded, Kind::EdgeAdded};
    CHECK(RoundTrip(m, move, Kinds(moveKinds), true));
    // redo 뒤 하위 그래프에 노드 넷(원래 하나 + 셋)과 간선 둘이 있다.
    CHECK(m.sample.document.graphs[1].nodes.Size() == 4);
    CHECK(m.sample.document.graphs[1].edges.Size() == 2);

    auto rejects = [&](Guid source, Span<const Guid> nodes, Guid target,
                       GraphCommandReason reason) noexcept {
        GraphCommandList bad(env.allocator);
        return bad.MoveNodesToGraph(source, nodes, target) && Rejects(m, bad, reason);
    };

    CHECK(rejects(SampleMain, Span<const Guid>(&SampleAdd, 1), SampleCollapsed,
                  GraphCommandReason::CrossingEdge));
    CHECK(rejects(SampleMain, Span<const Guid>(&SampleAdd, 1), SampleMain,
                  GraphCommandReason::SameGraph));
    CHECK(rejects(SampleMain, Span<const Guid>(&SampleSequence, 1), SampleCollapsed,
                  GraphCommandReason::NodeMissing));
    CHECK(rejects(SampleMain, Span<const Guid>(&SampleAdd, 1), Id(99),
                  GraphCommandReason::GraphMissing));
    CHECK(rejects(Id(99), Span<const Guid>(&SampleAdd, 1), SampleCollapsed,
                  GraphCommandReason::GraphMissing));
}

// undo·redo의 오용: 다른 mutator의 patch, 이미 되돌린 patch의 undo, 적용된 patch의 redo.
TEST(NodeGraph_MutatorPatchMisuse) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv first(env);
    MutatorEnv second(env);
    REQUIRE(first.ready);
    REQUIRE(second.ready);
    GraphCommandList list(env.allocator);
    REQUIRE(list.SetLayout(SampleMain, SampleAdd, MakeLayout(env, SampleAdd, 5, 5)));
    auto patch = first.Run(list);
    REQUIRE(patch);
    CHECK(patch.Value().OpCount() == 1);
    CHECK(patch.Value().ApproximateBytes() > 0);
    auto foreign = second.mutator.Undo(TestSource, patch.Value());
    REQUIRE(!foreign);
    CHECK(foreign.Error() == ErrorCode::InvalidArgument);
    auto redoApplied = first.mutator.Redo(TestSource, patch.Value());
    REQUIRE(!redoApplied);
    CHECK(redoApplied.Error() == ErrorCode::InvalidArgument);
    REQUIRE(first.mutator.Undo(TestSource, patch.Value()));
    auto undoTwice = first.mutator.Undo(TestSource, patch.Value());
    REQUIRE(!undoTwice);
    CHECK(undoTwice.Error() == ErrorCode::InvalidArgument);

    // 구독 해제 뒤에는 통지가 오지 않는다.
    first.mutator.RemoveListener(first.recorder);
    const usize batches = first.recorder.batches.Size();
    REQUIRE(first.mutator.Redo(TestSource, patch.Value()));
    CHECK(first.recorder.batches.Size() == batches);
}

} // namespace node_graph_mutator_tests_file
