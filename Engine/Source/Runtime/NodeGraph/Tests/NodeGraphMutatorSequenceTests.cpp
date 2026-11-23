#include "NodeGraphMutatorTestSupport.h"

#include <Core/Math/RandomStream.h>
#include <stdio.h>

// GraphMutator의 이력·bracket·실패 주입 시험이다(Docs/Design/NodeGraph 8.2·8.6절, 13절
// GT3·GT4·GT6).
//   GT3: seed 무작위 명령열(단계 수 RandomCommandSteps). 매 단계 전체 불변식
//   검사(SetFullValidation), 실패한 명령은
//        문서 바이트·revision 불변, 임의 지점까지 undo한 바이트가 그 시점과 같고 redo가 돌아온다.
//   GT4: bracket 확정·취소·중첩·다른 출처 Busy·같은 필드 반복 병합·구조 op 뒤 값 바꾸기.
//   GT6: k번째부터 모든 할당이 실패하는 allocator로 명령·undo·redo를 k = 1부터 돌린다. 실패하면
//   문서 바이트·
//        revision·통지가 그대로다(되돌리기가 할당하면 ROCKS_ASSERT로 멈춘다 — 무할당 확인).
using namespace rocks;
namespace node_graph_mutator_sequence_tests_file {

using namespace rocks::nodegraph;
using namespace rocks::nodegraph_test;

// GT3 무작위 명령열의 단계 수와 seed다.
constexpr uint32 RandomCommandSteps = 10000;
constexpr uint64 RandomCommandSeed = 0x4e4731534551ull;
// 무작위 시험이 쥐는 undo 이력의 최대 길이다.
constexpr usize RandomHistoryLimit = 48;
// GT6 실패 주입의 시도 상한(이 안에 성공해야 한다).
constexpr uint64 FailureAttemptLimit = 4096;

// attempt번째부터 모든 할당이 실패하는 allocator다(0이면 실패하지 않는다).
class FailFromAllocator final : public Allocator {
public:
    explicit FailFromAllocator(Allocator& inner) noexcept : inner_(inner) {
    }

    void FailFrom(uint64 attempt) noexcept {
        attempts_ = 0;
        failFrom_ = attempt;
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        ++attempts_;
        if (failFrom_ != 0 && attempts_ >= failFrom_) {
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }

        return inner_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return inner_.Deallocate(address);
    }

private:
    Allocator& inner_;
    uint64 attempts_ = 0;
    uint64 failFrom_ = 0;
};

// ---- GT3 ----

struct HistoryEntry {
    explicit HistoryEntry(Allocator& allocator) noexcept : patch(allocator), before(allocator) {
    }

    GraphPatch patch;
    Array<uint8> before;
};

class RandomCommands {
public:
    RandomCommands(TestEnv& env, MutatorEnv& m) noexcept
        : env_(env), m_(m), random_(RandomCommandSeed), ids_(RandomCommandSeed, Id(0x5E9)) {
    }

    // 무작위 명령 1–3개의 목록을 만든다(일부는 일부러 위반이다).
    bool Build(GraphCommandList& list) noexcept {
        list.Clear();
        const uint32 count = 1 + random_.NextUInt32Below(3);
        for (uint32 index = 0; index < count; ++index) {
            if (!AddOne(list)) {
                return false;
            }
        }

        return true;
    }

    RandomStream& Random() noexcept {
        return random_;
    }

private:
    const Graph& PickGraph() noexcept {
        const Array<Graph>& graphs = m_.sample.document.graphs;
        return graphs[random_.NextUInt32Below(static_cast<uint32>(graphs.Size()))];
    }

    Guid PickNode(const Graph& graph) noexcept {
        if (graph.nodes.IsEmpty()) {
            return Id(1000 + random_.NextUInt32Below(5));
        }

        return graph.nodes[random_.NextUInt32Below(static_cast<uint32>(graph.nodes.Size()))].id;
    }

    Name PickPin() noexcept {
        const Name pins[] = {env_.pinA,     env_.pinB,   env_.pinResult, env_.pinValue,
                             env_.pinIn,    env_.pinOut, env_.pinThen,   env_.pinItems,
                             env_.pinItem0, env_.pinGone};
        return pins[random_.NextUInt32Below(static_cast<uint32>(sizeof(pins) / sizeof(pins[0])))];
    }

    Guid NewId() noexcept {
        auto id = ids_.Next();
        return id ? id.Value() : Id(1);
    }

    bool AddOne(GraphCommandList& list) noexcept {
        const Graph& graph = PickGraph();
        const Guid node = PickNode(graph);
        switch (random_.NextUInt32Below(18)) {
        case 0:
        case 1: {
            const Name types[] = {env_.typeAdd,   env_.typeParam,  env_.typeSequence,
                                  env_.typeMulti, env_.typeToggle, env_.typeConvert,
                                  env_.reroute};
            const Name type = types[random_.NextUInt32Below(
                static_cast<uint32>(sizeof(types) / sizeof(types[0])))];
            GraphNode created =
                MakeNode(env_, NewId(), type, type == env_.typeParam ? TestParamVersion : 1);
            if (type == env_.typeParam && !FillRecord(created.properties.bytes, TestParamStruct,
                                                      TestParamVersion, random_.NextUInt32())) {
                return false;
            }

            return static_cast<bool>(list.AddNode(
                graph.id, created,
                MakeLayout(env_, created.id, static_cast<float>(random_.NextUInt32Below(500)), 0)));
        }
        case 2: {
            // 노드 1–2개(같은 id가 두 번일 수도 있다 — DuplicateNode 위반).
            const Guid nodes[] = {node, PickNode(graph)};
            return static_cast<bool>(list.RemoveNodes(
                graph.id, Span<const Guid>(nodes, 1 + random_.NextUInt32Below(2))));
        }
        case 3:
        case 4:
            return static_cast<bool>(
                list.Connect(graph.id, MakeEdge(node, PickPin(), PickNode(graph), PickPin())));
        case 5:
            if (graph.edges.IsEmpty()) {
                return static_cast<bool>(
                    list.Disconnect(graph.id, MakeEdge(node, PickPin(), node, PickPin())));
            }

            return static_cast<bool>(list.Disconnect(
                graph.id,
                graph.edges[random_.NextUInt32Below(static_cast<uint32>(graph.edges.Size()))]));
        case 6:
            return static_cast<bool>(list.SetLayout(
                graph.id, node,
                MakeLayout(env_, node, static_cast<float>(random_.NextUInt32Below(900)), 7)));
        case 7: {
            Array<uint8> value(env_.allocator);
            return FillRecord(value, TestValueStruct, 1, random_.NextUInt32()) &&
                   list.SetInputDefault(graph.id, node, PickPin(), value.View());
        }
        case 8:
            if (random_.NextBool()) {
                return static_cast<bool>(list.ResetInputDefault(graph.id, node, PickPin()));
            }

            return static_cast<bool>(list.SetEnableState(
                graph.id, node,
                random_.NextBool() ? NodeEnableState::Disabled : NodeEnableState::Enabled));
        case 9: {
            CommentBox comment(env_.allocator);
            comment.id = random_.NextBool() ? NewId() : SampleComment;
            if (random_.NextBool()) {
                return static_cast<bool>(list.AddComment(graph.id, comment));
            }

            return static_cast<bool>(list.RemoveComment(graph.id, comment.id));
        }
        case 10: {
            Bookmark mark;
            mark.number = 1 + random_.NextUInt32Below(4);
            mark.zoom = 1.0f + static_cast<float>(random_.NextUInt32Below(4));
            if (random_.NextBool()) {
                return static_cast<bool>(list.SetBookmark(graph.id, mark));
            }

            return static_cast<bool>(list.ClearBookmark(graph.id, mark.number));
        }
        case 11:
            switch (random_.NextUInt32Below(3)) {
            case 0:
                return static_cast<bool>(list.AddGraph(NewId(), GraphRole::Collapsed, 0, "Sub"));
            case 1:
                return static_cast<bool>(list.RemoveGraph(graph.id));
            default:
                return static_cast<bool>(
                    list.RenameGraph(graph.id, random_.NextBool() ? "A" : "B"));
            }
        case 12: {
            const Graph& target = PickGraph();
            const Guid nodes[] = {node, PickNode(graph)};
            return static_cast<bool>(list.MoveNodesToGraph(
                graph.id, Span<const Guid>(nodes, 1 + random_.NextUInt32Below(2)), target.id));
        }
        case 13: {
            // 동적 핀 수(1–4개) 바꾸기. Dynamic이 아닌 노드에는 schema가 맞지 않는 바이트다.
            Array<uint8> bytes(env_.allocator);
            return FillRecord(bytes, TestDynamicStruct, 1 + random_.NextUInt32Below(4), 0) &&
                   list.SetNodeProperties(graph.id, node, bytes.View());
        }
        case 14:
            return static_cast<bool>(list.RemoveRetainedPin(
                graph.id, node, random_.NextBool() ? env_.pinGone : PickPin()));
        case 15: {
            // 완성된 노드 데이터(입력 기본값·고아 핀, 알 수 없는 종류 포함)를 넣는다.
            const Name types[] = {env_.typeAdd, env_.typeUnknown, env_.typeToggle};
            const Name type = types[random_.NextUInt32Below(
                static_cast<uint32>(sizeof(types) / sizeof(types[0])))];
            GraphNode created = MakeNode(env_, NewId(), type);
            PinValueEntry entry(env_.allocator);
            entry.pin = PickPin();
            RetainedPin retained(env_.allocator);
            retained.key = env_.pinGone;
            retained.direction = random_.NextBool() ? PinDirection::Input : PinDirection::Output;
            return FillRecord(entry.value.bytes, TestValueStruct, 1, random_.NextUInt32()) &&
                   FillRecord(retained.value.bytes, TestValueStruct, 1, 1) &&
                   created.inputDefaults.TryPushBack(Move(entry)) &&
                   created.retainedPins.TryPushBack(Move(retained)) &&
                   list.InsertNode(graph.id, created, MakeLayout(env_, created.id, 1, 2));
        }
        case 16:
            return static_cast<bool>(
                list.InsertEdge(graph.id, MakeEdge(node, PickPin(), PickNode(graph), PickPin(),
                                                   random_.NextUInt32Below(3))));
        default: {
            // ordered 출력 핀의 간선 순서를 뒤집는다(Sequence가 아니면 위반이다).
            Array<GraphEdge> order(env_.allocator);
            for (const GraphEdge& edge : graph.edges) {
                if (edge.fromNode == node && edge.fromPin == env_.pinThen &&
                    !order.TryInsert(edge, 0)) {
                    return false;
                }
            }

            return static_cast<bool>(list.ReorderEdges(graph.id, node, env_.pinThen, order.View()));
        }
        }
    }

    TestEnv& env_;
    MutatorEnv& m_;
    RandomStream random_;
    SeededGraphIdSource ids_;
};

TEST(NodeGraph_MutatorRandomCommandSequence) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    RandomCommands generator(env, m);
    Array<HistoryEntry> history(env.allocator);
    GraphCommandList list(env.allocator);
    uint32 succeeded = 0;
    uint32 failed = 0;
    uint32 undoRuns = 0;
    uint32 bracketRuns = 0;
    for (uint32 step = 0; step < RandomCommandSteps; ++step) {
        REQUIRE(generator.Build(list));
        Array<uint8> before = m.Bytes();
        const uint64 revision = m.mutator.Revision();
        GraphCommandFailure failure;
        // 가끔 bracket 안에서 실행한다. 거부되면 bracket을 취소하고(문서는 그대로다), 성공하면
        // 확정한 bracket patch를 쓴다.
        const bool bracket = generator.Random().NextUInt32Below(8) == 0;
        if (bracket) {
            REQUIRE(m.mutator.BeginInteraction(TestSource));
            ++bracketRuns;
        }

        auto patch = m.Run(list, &failure);
        if (bracket) {
            if (patch) {
                patch = m.mutator.EndInteraction(TestSource);
                REQUIRE(patch);
            } else {
                REQUIRE(m.mutator.CancelInteraction(TestSource));
            }
        }

        if (!patch) {
            // 거부는 공통 규칙 위반이어야 한다. InvariantBroken은 명령 검사가 놓친 불변식 위반이다.
            const bool unchanged = patch.Error() == ErrorCode::InvalidArgument &&
                                   failure.reason != GraphCommandReason::InvariantBroken &&
                                   SameBytes(m.Bytes(), before) &&
                                   (bracket || m.mutator.Revision() == revision);
            if (!unchanged) {
                fprintf(stderr,
                        "NodeGraph random sequence: failed command changed the document (seed "
                        "%llu, step %u, error %s, command %u, reason %s, kinds",
                        static_cast<unsigned long long>(RandomCommandSeed), step,
                        ErrorCodeName(patch.Error()), failure.command,
                        GraphCommandReasonName(failure.reason));
                for (const GraphCommand& command : list.View()) {
                    fprintf(stderr, " %u", static_cast<unsigned>(command.kind));
                }

                fprintf(stderr, ")\n");
            }

            REQUIRE(unchanged);
            ++failed;
            continue;
        }

        ++succeeded;
        if (history.Size() == RandomHistoryLimit) {
            REQUIRE(history.TryRemoveAt(0));
        }

        auto entry = history.TryEmplaceBack(env.allocator);
        REQUIRE(entry);
        entry.Value()->patch = Move(patch.Value());
        entry.Value()->before = Move(before);

        // 가끔 임의 지점까지 undo하고(바이트가 그 시점과 같다) 다시 redo하거나 버린다.
        if (generator.Random().NextUInt32Below(16) == 0 && !history.IsEmpty()) {
            ++undoRuns;
            const Array<uint8> current = m.Bytes();
            const usize count =
                1 + generator.Random().NextUInt32Below(static_cast<uint32>(history.Size()));
            for (usize index = 0; index < count; ++index) {
                HistoryEntry& undone = history[history.Size() - 1 - index];
                REQUIRE(m.mutator.Undo(TestSource, undone.patch));
                const bool same = SameBytes(m.Bytes(), undone.before);
                if (!same) {
                    fprintf(stderr,
                            "NodeGraph random sequence: undo mismatch (seed %llu, step %u, depth "
                            "%zu)\n",
                            static_cast<unsigned long long>(RandomCommandSeed), step, index);
                }

                REQUIRE(same);
            }

            if (generator.Random().NextBool()) {
                for (usize index = count; index > 0; --index) {
                    REQUIRE(m.mutator.Redo(TestSource, history[history.Size() - index].patch));
                }

                REQUIRE(SameBytes(m.Bytes(), current));
            } else {
                REQUIRE(history.TryRemoveAt(history.Size() - count, count));
            }
        }
    }

    // 무작위 목록이 성공·실패·undo를 모두 거쳤어야 의미가 있다.
    CHECK(succeeded > RandomCommandSteps / 10);
    CHECK(failed > RandomCommandSteps / 10);
    CHECK(undoRuns > 0);
    CHECK(bracketRuns > 0);
    CHECK(!m.recorder.failed);
}

// ---- GT4 ----

TEST(NodeGraph_MutatorInteractionCommit) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    const Array<uint8> before = m.Bytes();
    REQUIRE(m.mutator.BeginInteraction(TestSource));
    CHECK(m.mutator.InteractionOpen());
    auto nested = m.mutator.BeginInteraction(TestSource);
    REQUIRE(!nested);
    CHECK(nested.Error() == ErrorCode::Busy);

    // 같은 노드 배치를 여러 번 바꾼다(drag). 명령마다 interactive 통지, bracket patch는 op 하나로
    // 합친다.
    for (uint32 frame = 1; frame <= 5; ++frame) {
        GraphCommandList drag(env.allocator);
        REQUIRE(drag.SetLayout(SampleMain, SampleAdd,
                               MakeLayout(env, SampleAdd, static_cast<float>(frame), 0)));
        auto patch = m.Run(drag);
        REQUIRE(patch);
        CHECK(patch.Value().IsEmpty());
        CHECK(m.recorder.Last().interactive);
    }

    // 다른 출처의 명령·undo·redo, 다른 출처의 확정·취소는 Busy다.
    GraphCommandList other(env.allocator);
    REQUIRE(other.SetLayout(SampleMain, SampleParamA, MakeLayout(env, SampleParamA, 1, 1)));
    auto busy = m.mutator.Execute(OtherSource, m.mutator.Revision(), other);
    REQUIRE(!busy);
    CHECK(busy.Error() == ErrorCode::Busy);
    auto wrongEnd = m.mutator.EndInteraction(OtherSource);
    REQUIRE(!wrongEnd);
    CHECK(wrongEnd.Error() == ErrorCode::Busy);
    auto wrongCancel = m.mutator.CancelInteraction(OtherSource);
    REQUIRE(!wrongCancel);
    CHECK(wrongCancel.Error() == ErrorCode::Busy);

    const Array<uint8> after = m.Bytes();
    const usize batches = m.recorder.batches.Size();
    auto committed = m.mutator.EndInteraction(TestSource);
    REQUIRE(committed);
    CHECK(!m.mutator.InteractionOpen());
    CHECK(committed.Value().OpCount() == 1);
    // 확정 통지: 바뀐 것 없음, interactive 거짓, 배치만 바뀌었으므로 semanticTouched 거짓.
    REQUIRE(m.recorder.batches.Size() == batches + 1);
    CHECK(!m.recorder.Last().interactive && m.recorder.Last().changes.IsEmpty() &&
          !m.recorder.Last().semanticTouched);

    // bracket patch 하나로 undo·redo한다. 열린 bracket이 없으면 확정·취소는 NotReady다.
    auto endAgain = m.mutator.EndInteraction(TestSource);
    REQUIRE(!endAgain);
    CHECK(endAgain.Error() == ErrorCode::NotReady);
    REQUIRE(m.mutator.Undo(TestSource, committed.Value()));
    CHECK(SameBytes(m.Bytes(), before));
    REQUIRE(m.mutator.Redo(TestSource, committed.Value()));
    CHECK(SameBytes(m.Bytes(), after));
}

TEST(NodeGraph_MutatorInteractionCancelAndStructure) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    const Array<uint8> before = m.Bytes();
    const uint64 revision = m.mutator.Revision();
    REQUIRE(m.mutator.BeginInteraction(TestSource));
    // 노드를 더하고, 그 배치를 두 번 바꾸고, 지우고, 다시 같은 id로 더한 뒤 값을 바꾼다(구조 op
    // 사이의 값 바꾸기는 합치지 않는다).
    GraphCommandList add(env.allocator);
    REQUIRE(
        add.AddNode(SampleMain, MakeNode(env, Id(50), env.typeAdd), MakeLayout(env, Id(50), 0, 0)));
    GraphCommandList move(env.allocator);
    REQUIRE(move.SetLayout(SampleMain, Id(50), MakeLayout(env, Id(50), 9, 9)));
    GraphCommandList remove(env.allocator);
    const Guid id = Id(50);
    REQUIRE(remove.RemoveNodes(SampleMain, Span<const Guid>(&id, 1)));
    REQUIRE(m.Run(add));
    REQUIRE(m.Run(move));
    REQUIRE(m.Run(move));
    REQUIRE(m.Run(remove));
    REQUIRE(m.Run(add));
    REQUIRE(m.Run(move));
    REQUIRE(m.mutator.CancelInteraction(TestSource));
    CHECK(SameBytes(m.Bytes(), before));
    CHECK(m.recorder.Last().cause == GraphChangeCause::Cancel);
    CHECK(m.mutator.Revision() == revision + 7);

    // 같은 순서를 확정한 patch도 undo하면 원래 바이트다.
    REQUIRE(m.mutator.BeginInteraction(TestSource));
    REQUIRE(m.Run(add));
    REQUIRE(m.Run(move));
    REQUIRE(m.Run(move));
    REQUIRE(m.Run(remove));
    REQUIRE(m.Run(add));
    REQUIRE(m.Run(move));
    const Array<uint8> after = m.Bytes();
    auto patch = m.mutator.EndInteraction(TestSource);
    REQUIRE(patch);
    CHECK(m.recorder.Last().semanticTouched);
    REQUIRE(m.mutator.Undo(TestSource, patch.Value()));
    CHECK(SameBytes(m.Bytes(), before));
    REQUIRE(m.mutator.Redo(TestSource, patch.Value()));
    CHECK(SameBytes(m.Bytes(), after));

    // 빈 bracket의 취소는 통지·revision 변화가 없다.
    const uint64 now = m.mutator.Revision();
    const usize batches = m.recorder.batches.Size();
    REQUIRE(m.mutator.BeginInteraction(TestSource));
    REQUIRE(m.mutator.CancelInteraction(TestSource));
    CHECK(m.mutator.Revision() == now && m.recorder.batches.Size() == batches);

    // bracket 안 실패한 명령은 그 명령만 되돌리고 bracket은 열려 있다.
    REQUIRE(m.mutator.BeginInteraction(TestSource));
    REQUIRE(m.Run(move));
    GraphCommandList bad(env.allocator);
    REQUIRE(bad.SetLayout(SampleMain, Id(50), MakeLayout(env, Id(50), 1, 2)));
    REQUIRE(bad.RemoveNodes(SampleMain, Span<const Guid>(&SampleComment, 1)));
    const Array<uint8> mid = m.Bytes();
    CHECK(!m.Run(bad));
    CHECK(SameBytes(m.Bytes(), mid));
    CHECK(m.mutator.InteractionOpen());
    auto finished = m.mutator.EndInteraction(TestSource);
    REQUIRE(finished);
    CHECK(finished.Value().OpCount() == 1);
}

// ---- GT6 ----

// 실패 주입 환경이다: 표본을 FailFromAllocator 문서로 복제하고 그 위에 mutator를 둔다.
struct FailingEnv {
    explicit FailingEnv(TestEnv& testEnv) noexcept
        : env(testEnv), failing(testEnv.allocator), document(failing), layout(failing),
          ids(TestIdSeed, Id(0xFA11)), mutator(document, layout, testEnv.registry, ids),
          recorder(testEnv.allocator) {
        Sample sample(testEnv);
        ready = sample.ok && CloneGraphDocument(sample.document, document) &&
                CloneGraphDocumentLayout(sample.layout, layout) && mutator.Initialize() &&
                mutator.AddListener(recorder);
    }

    Array<uint8> Bytes() noexcept {
        return DocumentBytes(env, document, layout);
    }

    TestEnv& env;
    FailFromAllocator failing;
    GraphDocument document;
    GraphDocumentLayout layout;
    SeededGraphIdSource ids;
    GraphMutator mutator;
    ChangeRecorder recorder;
    bool ready = false;
};

// op 종류를 고루 쓰는 명령 목록이다.
bool BuildFailureCommands(TestEnv& env, GraphCommandList& list) noexcept {
    Array<uint8> value(env.allocator);
    CommentBox comment(env.allocator);
    comment.id = Id(41);
    Bookmark mark;
    mark.number = 7;
    const Guid removed = SampleReroute;
    return FillRecord(value, TestValueStruct, 1, 3) &&
           list.AddNode(SampleMain, MakeNode(env, Id(50), env.typeMulti),
                        MakeLayout(env, Id(50), 1, 2)) &&
           list.Connect(SampleMain, MakeEdge(SampleParamB, env.pinValue, Id(50), env.pinItems)) &&
           list.SetInputDefault(SampleMain, SampleDefaults, env.pinB, value.View()) &&
           list.SetLayout(SampleMain, SampleAdd, MakeLayout(env, SampleAdd, 5, 6)) &&
           list.AddComment(SampleMain, comment) && list.SetBookmark(SampleMain, mark) &&
           list.AddGraph(Id(90), GraphRole::Collapsed, 0, "Failing") &&
           list.RenameGraph(SampleCollapsed, "R") &&
           list.RemoveNodes(SampleMain, Span<const Guid>(&removed, 1)) &&
           list.Disconnect(SampleMain,
                           MakeEdge(SampleSequence, env.pinThen, SampleStepA, env.pinIn));
}

// 나머지 명령(구조 이동·순서·동적 핀·변환 연결·그래프 지우기·활성 상태·고아 핀)을 쓰는 목록이다.
// 변환 연결은 env.schema.connect가 Convert일 때다.
bool BuildStructureFailureCommands(TestEnv& env, GraphCommandList& list) noexcept {
    Array<uint8> dynamic(env.allocator);
    const GraphEdge swapped[] = {MakeEdge(SampleSequence, env.pinThen, SampleStepB, env.pinIn),
                                 MakeEdge(SampleSequence, env.pinThen, SampleStepA, env.pinIn)};
    const Guid moved[] = {SampleSequence, SampleStepA, SampleStepB};
    return FillRecord(dynamic, TestDynamicStruct, 3, 0) &&
           list.ReorderEdges(SampleMain, SampleSequence, env.pinThen,
                             Span<const GraphEdge>(swapped, 2)) &&
           list.AddGraph(Id(90), GraphRole::Collapsed, 0, "Moved") &&
           list.MoveNodesToGraph(SampleMain, Span<const Guid>(moved, 3), Id(90)) &&
           list.SetNodeProperties(SampleMain, SampleDynamic, dynamic.View()) &&
           list.Connect(SampleMain,
                        MakeEdge(SampleParamB, env.pinValue, SampleDynamic, env.pinItem0)) &&
           list.SetEnableState(SampleMain, SampleToggle, NodeEnableState::Enabled) &&
           list.RemoveRetainedPin(SampleMain, SampleAdd, env.pinGone) &&
           list.RemoveGraph(SampleCollapsed);
}

// list를 k번째 할당부터 실패시키며 성공할 때까지 실행한다. 실패마다 문서·revision·통지가
// 그대로이고, undo·redo도 같은 방식으로 확인한다.
void CheckFailureInjection(TestEnv& env, const GraphCommandList& list) noexcept {
    FailingEnv f(env);
    REQUIRE(f.ready);
    const Array<uint8> before = f.Bytes();

    // 명령: k번째 할당부터 실패시키며 성공할 때까지.
    bool executed = false;
    GraphPatch committed(env.allocator);
    uint64 failures = 0;
    for (uint64 attempt = 1; attempt <= FailureAttemptLimit && !executed; ++attempt) {
        const usize batches = f.recorder.batches.Size();
        f.failing.FailFrom(attempt);
        auto patch = f.mutator.Execute(TestSource, f.mutator.Revision(), list);
        f.failing.FailFrom(0);
        if (patch) {
            committed = Move(patch.Value());
            executed = true;
            break;
        }

        ++failures;
        CHECK(patch.Error() == ErrorCode::OutOfMemory);
        CHECK(SameBytes(f.Bytes(), before));
        CHECK(f.mutator.Revision() == 0);
        CHECK(f.recorder.batches.Size() == batches);
    }

    REQUIRE(executed);
    CHECK(failures > 0);
    const Array<uint8> after = f.Bytes();

    // undo·redo도 같은 방식으로: 실패하면 문서가 그대로다.
    for (uint32 round = 0; round < 2; ++round) {
        const bool undo = round == 0;
        const Array<uint8>& expectedOnFailure = undo ? after : before;
        bool done = false;
        for (uint64 attempt = 1; attempt <= FailureAttemptLimit && !done; ++attempt) {
            const uint64 revision = f.mutator.Revision();
            f.failing.FailFrom(attempt);
            auto applied = undo ? f.mutator.Undo(TestSource, committed)
                                : f.mutator.Redo(TestSource, committed);
            f.failing.FailFrom(0);
            if (applied) {
                done = true;
                break;
            }

            CHECK(applied.Error() == ErrorCode::OutOfMemory);
            CHECK(SameBytes(f.Bytes(), expectedOnFailure));
            CHECK(f.mutator.Revision() == revision);
        }

        REQUIRE(done);
        CHECK(SameBytes(f.Bytes(), undo ? before : after));
    }
}

TEST(NodeGraph_MutatorFailureInjection) {
    TestEnv env;
    REQUIRE(env.ready);
    GraphCommandList list(env.allocator);
    REQUIRE(BuildFailureCommands(env, list));
    CheckFailureInjection(env, list);
}

TEST(NodeGraph_MutatorFailureInjectionStructure) {
    TestEnv env;
    REQUIRE(env.ready);
    env.schema.connect = TestConnect::Convert;
    env.schema.conversionType = env.typeConvert;
    GraphCommandList list(env.allocator);
    REQUIRE(BuildStructureFailureCommands(env, list));
    CheckFailureInjection(env, list);
    env.schema.connect = TestConnect::Default;
}

// bracket 취소는 할당이 실패하면 bracket이 열린 채 문서가 그대로다(다시 부를 수 있다). 확정은
// 할당하지 않는다.
TEST(NodeGraph_MutatorFailureInjectionInteraction) {
    TestEnv env;
    REQUIRE(env.ready);
    GraphCommandList list(env.allocator);
    REQUIRE(BuildFailureCommands(env, list));
    GraphCommandList drag(env.allocator);
    REQUIRE(drag.SetLayout(SampleMain, SampleAdd, MakeLayout(env, SampleAdd, 9, 9)));
    for (uint32 round = 0; round < 2; ++round) {
        const bool cancel = round == 0;
        FailingEnv f(env);
        REQUIRE(f.ready);
        const Array<uint8> before = f.Bytes();
        REQUIRE(f.mutator.BeginInteraction(TestSource));
        REQUIRE(f.mutator.Execute(TestSource, f.mutator.Revision(), list));
        REQUIRE(f.mutator.Execute(TestSource, f.mutator.Revision(), drag));
        const Array<uint8> inside = f.Bytes();
        bool done = false;
        uint64 failures = 0;
        for (uint64 attempt = 1; attempt <= FailureAttemptLimit && !done; ++attempt) {
            const usize batches = f.recorder.batches.Size();
            f.failing.FailFrom(attempt);
            bool succeeded = false;
            if (cancel) {
                auto result = f.mutator.CancelInteraction(TestSource);
                succeeded = static_cast<bool>(result);
                if (!succeeded) {
                    CHECK(result.Error() == ErrorCode::OutOfMemory);
                }
            } else {
                auto result = f.mutator.EndInteraction(TestSource);
                succeeded = static_cast<bool>(result);
                if (!succeeded) {
                    CHECK(result.Error() == ErrorCode::OutOfMemory);
                }
            }

            f.failing.FailFrom(0);
            if (succeeded) {
                done = true;
                break;
            }

            ++failures;
            CHECK(f.mutator.InteractionOpen());
            CHECK(SameBytes(f.Bytes(), inside));
            CHECK(f.recorder.batches.Size() == batches);
        }

        REQUIRE(done);
        // 확정은 할당하지 않는다(첫 시도에 성공한다).
        if (!cancel) {
            CHECK(failures == 0);
        }

        CHECK(!f.mutator.InteractionOpen());
        CHECK(SameBytes(f.Bytes(), cancel ? before : inside));
    }
}

// 일부러 실패하는 명령(마지막 명령이 위반)도 모든 할당 실패 지점에서 문서를 바꾸지 않는다.
TEST(NodeGraph_MutatorFailureInjectionRollback) {
    TestEnv env;
    REQUIRE(env.ready);
    FailingEnv f(env);
    REQUIRE(f.ready);
    GraphCommandList list(env.allocator);
    REQUIRE(BuildFailureCommands(env, list));
    REQUIRE(list.RemoveGraph(SampleMain));
    const Array<uint8> before = f.Bytes();
    bool sawValidation = false;
    for (uint64 attempt = 1; attempt <= FailureAttemptLimit && !sawValidation; ++attempt) {
        GraphCommandFailure failure;
        f.failing.FailFrom(attempt);
        auto patch = f.mutator.Execute(TestSource, f.mutator.Revision(), list, &failure);
        f.failing.FailFrom(0);
        REQUIRE(!patch);
        CHECK(SameBytes(f.Bytes(), before));
        CHECK(f.mutator.Revision() == 0);
        if (patch.Error() == ErrorCode::InvalidArgument) {
            sawValidation = true;
            CHECK(failure.reason == GraphCommandReason::MainGraph);
            CHECK(failure.command == static_cast<uint32>(list.Size() - 1));
        }
    }

    CHECK(sawValidation);
}

} // namespace node_graph_mutator_sequence_tests_file
