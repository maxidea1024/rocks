#include "GraphToolsTestSupport.h"

#include <NodeGraph/GraphClone.h>
#include <NodeGraphTools/GraphDiff.h>

// 문서 diff 시험이다(Docs/Design/NodeGraph 9.4절 "diff"): 같은 문서는 빈 결과, 항목 종류마다
// 바뀐 것을 찾고 정해진 순서·줄 형식으로 쓴다. semanticOnly는 배치·제목·주석·북마크를 보지 않는다.
using namespace rocks;
namespace graph_diff_tests_file {

using namespace rocks::nodegraph_tools_test;

GraphNode* FindNode(Graph& graph, Guid id) noexcept {
    for (GraphNode& node : graph.nodes) {
        if (node.id == id) {
            return &node;
        }
    }

    return nullptr;
}

bool HasEntry(const Array<GraphDiffEntry>& entries, GraphDiffKind kind,
              Guid node = Guid{}) noexcept {
    for (const GraphDiffEntry& entry : entries) {
        if (entry.kind == kind && (node == Guid{} || entry.node == node)) {
            return true;
        }
    }

    return false;
}

TEST(GraphDiff_IdenticalDocumentsHaveNoEntries) {
    ToolsEnv tools;
    REQUIRE(tools.ready);
    Sample before(tools.env);
    Sample after(tools.env);
    REQUIRE(before.ok && after.ok);
    Array<GraphDiffEntry> entries(tools.env.allocator);
    REQUIRE(ComputeGraphDiff(before.document, &before.layout, after.document, &after.layout,
                             tools.env.names, {}, entries));
    CHECK(entries.IsEmpty());
}

// 항목 종류마다 하나씩 바꾸고 모두 찾는다.
TEST(GraphDiff_ReportsEveryKind) {
    ToolsEnv tools;
    REQUIRE(tools.ready);
    TestEnv& env = tools.env;
    Sample before(env);
    Sample after(env);
    REQUIRE(before.ok && after.ok);
    Graph& main = after.Main();

    // 노드: StepB 지우기(간선도), 새 노드, 버전·속성·활성 상태·입력 기본값·고아 핀.
    for (usize index = 0; index < main.nodes.Size(); ++index) {
        if (main.nodes[index].id == SampleStepB) {
            REQUIRE(main.nodes.TryRemoveAt(index));
            break;
        }
    }

    for (usize index = main.edges.Size(); index > 0; --index) {
        if (main.edges[index - 1].toNode == SampleStepB) {
            REQUIRE(main.edges.TryRemoveAt(index - 1));
        }
    }

    REQUIRE(main.nodes.TryPushBack(MakeNode(env, Id(70), env.typeAdd)));
    FindNode(main, SampleAdd)->typeSchemaVersion = 5;
    REQUIRE(FillRecord(FindNode(main, SampleParamA)->properties.bytes, TestParamStruct,
                       TestParamVersion, 99));
    FindNode(main, SampleToggle)->enableState = NodeEnableState::Enabled;
    PinValueEntry entry(env.allocator);
    entry.pin = env.pinB;
    REQUIRE(FillRecord(entry.value.bytes, TestValueStruct, 1, 1));
    REQUIRE(FindNode(main, SampleDefaults)->inputDefaults.TryPushBack(Move(entry)));
    REQUIRE(
        FillRecord(FindNode(main, SampleAdd)->retainedPins[0].value.bytes, TestValueStruct, 1, 77));

    // 간선: 새 간선과 순번 바꾸기.
    REQUIRE(main.edges.TryPushBack(MakeEdge(SampleParamB, env.pinValue, Id(70), env.pinA)));

    // 그래프: 역할·제목, 새 그래프. 배치: 노드 이동, 주석 글, 북마크 하나 빼기.
    after.document.graphs[1].domainRole = 4;
    after.document.graphs[1].role = GraphRole::DomainDefined;
    Graph extra(env.allocator);
    extra.id = Id(5);
    extra.role = GraphRole::Collapsed;
    REQUIRE(after.document.graphs.TryPushBack(Move(extra)));
    REQUIRE(after.layout.graphs[0].title.TryAssign("Renamed"));
    for (NodeLayout& node : after.layout.graphs[0].nodes) {
        if (node.node == SampleParamB) {
            node.position.x += 1.0f;
        }
    }

    REQUIRE(after.layout.graphs[0].comments[0].text.TryAssign("changed"));
    REQUIRE(after.layout.graphs[0].bookmarks.TryRemoveAt(1));
    after.document.mainGraph = SampleCollapsed;

    Array<GraphDiffEntry> entries(env.allocator);
    REQUIRE(ComputeGraphDiff(before.document, &before.layout, after.document, &after.layout,
                             tools.env.names, {}, entries));
    CHECK(HasEntry(entries, GraphDiffKind::DocumentHeader));
    CHECK(HasEntry(entries, GraphDiffKind::GraphAdded));
    CHECK(HasEntry(entries, GraphDiffKind::GraphRole));
    CHECK(HasEntry(entries, GraphDiffKind::GraphTitle));
    CHECK(HasEntry(entries, GraphDiffKind::NodeRemoved, SampleStepB));
    CHECK(HasEntry(entries, GraphDiffKind::NodeAdded, Id(70)));
    CHECK(HasEntry(entries, GraphDiffKind::NodeVersion, SampleAdd));
    CHECK(HasEntry(entries, GraphDiffKind::NodeProperties, SampleParamA));
    CHECK(HasEntry(entries, GraphDiffKind::NodeEnableState, SampleToggle));
    CHECK(HasEntry(entries, GraphDiffKind::InputDefault, SampleDefaults));
    CHECK(HasEntry(entries, GraphDiffKind::RetainedPin, SampleAdd));
    CHECK(HasEntry(entries, GraphDiffKind::EdgeAdded, SampleParamB));
    CHECK(HasEntry(entries, GraphDiffKind::EdgeRemoved, SampleSequence));
    CHECK(HasEntry(entries, GraphDiffKind::NodeLayout, SampleParamB));
    CHECK(HasEntry(entries, GraphDiffKind::CommentChanged, SampleComment));
    CHECK(HasEntry(entries, GraphDiffKind::Bookmark));

    StringBuilder<> text(env.allocator);
    REQUIRE(FormatGraphDiff(env.names, entries.View(), text));
    CHECK(Contains(text.View(), "document main-graph\n"));
    CHECK(Contains(text.View(), "edge-added 00000000-0000-0000-0000-000000000001 "
                                "00000000-0000-0000-0000-00000000000b.Value -> "
                                "00000000-0000-0000-0000-000000000046.A\n"));
    CHECK(Contains(text.View(), "node-removed 00000000-0000-0000-0000-000000000001 "
                                "00000000-0000-0000-0000-000000000010 Test.Sequence\n"));
    CHECK(Contains(text.View(), "bookmark 00000000-0000-0000-0000-000000000001 3\n"));

    // semanticOnly: 배치·제목·주석·북마크가 빠진다.
    GraphDiffOptions semantic;
    semantic.semanticOnly = true;
    Array<GraphDiffEntry> semanticEntries(env.allocator);
    REQUIRE(ComputeGraphDiff(before.document, &before.layout, after.document, &after.layout,
                             tools.env.names, semantic, semanticEntries));
    CHECK(!HasEntry(semanticEntries, GraphDiffKind::NodeLayout));
    CHECK(!HasEntry(semanticEntries, GraphDiffKind::GraphTitle));
    CHECK(!HasEntry(semanticEntries, GraphDiffKind::CommentChanged));
    CHECK(!HasEntry(semanticEntries, GraphDiffKind::Bookmark));
    CHECK(HasEntry(semanticEntries, GraphDiffKind::NodeAdded, Id(70)));

    // 결과는 입력 순서와 무관하다(앞 문서의 배열을 뒤집고 뒤 문서를 정규화해도 같은 결과).
    Array<GraphDiffEntry> again(env.allocator);
    REQUIRE(CanonicalizeGraphDocument(after.document, &after.layout, env.names, env.allocator));
    Graph& beforeMain = before.Main();
    for (usize low = 0, high = beforeMain.edges.Size(); low + 1 < high; ++low, --high) {
        const GraphEdge swapped = beforeMain.edges[low];
        beforeMain.edges[low] = beforeMain.edges[high - 1];
        beforeMain.edges[high - 1] = swapped;
    }

    for (usize low = 0, high = beforeMain.nodes.Size(); low + 1 < high; ++low, --high) {
        GraphNode swapped(Move(beforeMain.nodes[low]));
        beforeMain.nodes[low] = Move(beforeMain.nodes[high - 1]);
        beforeMain.nodes[high - 1] = Move(swapped);
    }

    REQUIRE(ComputeGraphDiff(before.document, &before.layout, after.document, &after.layout,
                             tools.env.names, {}, again));
    StringBuilder<> againText(env.allocator);
    REQUIRE(FormatGraphDiff(env.names, again.View(), againText));
    CHECK(againText.View() == text.View());
}

// 순번만 바뀐 간선은 edge-order다.
TEST(GraphDiff_EdgeOrder) {
    ToolsEnv tools;
    REQUIRE(tools.ready);
    Sample before(tools.env);
    Sample after(tools.env);
    REQUIRE(before.ok && after.ok);
    for (GraphEdge& edge : after.Main().edges) {
        if (edge.fromNode == SampleSequence) {
            edge.order = edge.order == 0 ? 1 : 0;
        }
    }

    Array<GraphDiffEntry> entries(tools.env.allocator);
    REQUIRE(ComputeGraphDiff(before.document, &before.layout, after.document, &after.layout,
                             tools.env.names, {}, entries));
    REQUIRE(entries.Size() == 2);
    CHECK(entries[0].kind == GraphDiffKind::EdgeOrder &&
          entries[1].kind == GraphDiffKind::EdgeOrder);
    StringBuilder<> text(tools.env.allocator);
    REQUIRE(FormatGraphDiff(tools.env.names, entries.View(), text));
    CHECK(Contains(text.View(), " order 1\n"));
}

// 앞 시험이 다루지 않은 종류: 그래프 삭제, 노드 종류, 주석 추가·삭제, 북마크 값, 머리의
// 도메인·버전.
TEST(GraphDiff_RemainingKinds) {
    ToolsEnv tools;
    REQUIRE(tools.ready);
    TestEnv& env = tools.env;
    Sample before(env);
    Sample after(env);
    REQUIRE(before.ok && after.ok);
    REQUIRE(after.document.graphs.TryRemoveAt(1));
    FindNode(after.Main(), SampleToggle)->type = env.typeAdd;
    GraphLayout& layout = after.layout.graphs[0];
    REQUIRE(layout.comments.TryRemoveAt(0));
    CommentBox added(env.allocator);
    added.id = Id(81);
    REQUIRE(layout.comments.TryPushBack(Move(added)));
    layout.bookmarks[0].zoom = 2.0f;
    after.document.domain = env.Intern("Other");
    after.document.documentFormatVersion = 9;
    Array<GraphDiffEntry> entries(env.allocator);
    REQUIRE(ComputeGraphDiff(before.document, &before.layout, after.document, &after.layout,
                             env.names, {}, entries));
    CHECK(HasEntry(entries, GraphDiffKind::GraphRemoved));
    CHECK(HasEntry(entries, GraphDiffKind::NodeType, SampleToggle));
    CHECK(HasEntry(entries, GraphDiffKind::CommentRemoved, SampleComment));
    CHECK(HasEntry(entries, GraphDiffKind::CommentAdded, Id(81)));
    CHECK(HasEntry(entries, GraphDiffKind::Bookmark));
    StringBuilder<> text(env.allocator);
    REQUIRE(FormatGraphDiff(env.names, entries.View(), text));
    CHECK(Contains(text.View(), "document domain\n"));
    CHECK(Contains(text.View(), "document format-version\n"));
    CHECK(Contains(text.View(), "graph-removed 00000000-0000-0000-0000-000000000002\n"));
}

// 간선 줄 순서는 정규 순서(핀은 원문 순)라 pool의 Name 발급 순서와 무관하다.
TEST(GraphDiff_EdgeLinesIgnoreTokenOrder) {
    // pool 둘에 핀 이름을 반대 순서로 발급하고 같은 문서 쌍을 만든다.
    const auto run = [](bool reversed, String& out) noexcept {
        ToolsEnv tools;
        TestEnv& env = tools.env;
        const Name zeta = reversed ? env.Intern("Zeta") : Name();
        const Name alpha = env.Intern("Alpha");
        const Name zetaFinal = reversed ? zeta : env.Intern("Zeta");
        Sample before(env);
        Sample after(env);
        if (!tools.ready || !before.ok || !after.ok) {
            return false;
        }

        // 알 수 없는 종류 노드에서 나가는 두 간선(같은 출력 노드, 핀만 다르다)을 더한다.
        const bool added =
            after.Main().edges.TryPushBack(
                MakeEdge(SampleUnknown, zetaFinal, SampleAdd, env.pinGone)) &&
            after.Main().edges.TryPushBack(MakeEdge(SampleUnknown, alpha, SampleAdd, env.pinGone));
        Array<GraphDiffEntry> entries(env.allocator);
        StringBuilder<> text(env.allocator);
        return added &&
               ComputeGraphDiff(before.document, &before.layout, after.document, &after.layout,
                                env.names, {}, entries) &&
               FormatGraphDiff(env.names, entries.View(), text) && out.TryAssign(text.View());
    };

    SystemAllocator allocator;
    String first(allocator);
    String second(allocator);
    REQUIRE(run(false, first));
    REQUIRE(run(true, second));
    CHECK(first.View() == second.View());
    CHECK(Contains(first.View(), ".Alpha -> "));
}

} // namespace graph_diff_tests_file
