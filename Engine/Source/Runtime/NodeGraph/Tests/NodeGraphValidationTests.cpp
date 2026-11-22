#include "NodeGraphTestSupport.h"

// 문서 정규화와 불변식 검사 시험이다(Docs/Design/NodeGraph.ko.md 6.3절, 13절 GT1 구조 사례): 표본
// 문서는 통과하고, 규칙(GI1–GI14)마다 하나씩 어긴 문서는 정확히 그 규칙·요소로 거부된다. 검사는
// 문서를 바꾸지 않는다. 알 수 없는 종류의 검사 범위, 정규화의 결과·멱등성, 위반 상한.
using namespace rocks;
namespace node_graph_validation_tests_file {

using namespace rocks::nodegraph;
using namespace rocks::nodegraph_test;

// sample을 고친 뒤 검사해 위반 목록을 돌려준다(문서의 의미 바이트가 검사 전후 같은지도 본다).
struct Checked {
    explicit Checked(TestEnv& env) noexcept : violations(env.allocator) {
    }

    GraphViolationList violations;
    ErrorCode error = ErrorCode::None;
};

Checked Run(TestEnv& env, Sample& sample, bool withLayout = true) noexcept {
    Checked checked(env);
    auto result =
        Validate(env, sample.document, withLayout ? &sample.layout : nullptr, checked.violations);
    checked.error = result ? ErrorCode::None : result.Error();
    return checked;
}

// 위반이 정확히 하나이고 rule·subject이며 결과가 InvalidArgument인가.
bool Only(const Checked& checked, GraphInvariant rule, GraphSubject subject) noexcept {
    return checked.error == ErrorCode::InvalidArgument &&
           OnlyViolation(checked.violations, rule, subject);
}

GraphLayout& LayoutOf(Sample& sample, Guid graph) noexcept {
    for (GraphLayout& layout : sample.layout.graphs) {
        if (layout.graph == graph) {
            return layout;
        }
    }

    sample.ok = false;
    return sample.layout.graphs[0];
}

// 주 그래프에 간선을 더하고 정규 순서로 맞춘다.
bool AddEdge(TestEnv& env, Sample& sample, const GraphEdge& edge) noexcept {
    return sample.Main().edges.TryPushBack(edge) &&
           CanonicalizeGraphDocument(sample.document, &sample.layout, env.names, env.allocator);
}

// 주 그래프에 노드(와 배치)를 더하고 정규 순서로 맞춘다.
bool AddNode(TestEnv& env, Sample& sample, GraphNode&& node) noexcept {
    NodeLayout position(env.allocator);
    position.node = node.id;
    return LayoutOf(sample, SampleMain).nodes.TryPushBack(Move(position)) &&
           sample.Main().nodes.TryPushBack(Move(node)) &&
           CanonicalizeGraphDocument(sample.document, &sample.layout, env.names, env.allocator);
}

TEST(NodeGraph_ValidateSamplePasses) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample sample(env);
    REQUIRE(sample.ok);
    Array<uint8> before(env.allocator);
    Array<uint8> after(env.allocator);
    REQUIRE(WriteSemanticBytes(sample.document, env.registry, env.allocator, before));
    const Checked checked = Run(env, sample);
    CHECK(checked.error == ErrorCode::None);
    CHECK(checked.violations.items.IsEmpty() && checked.violations.dropped == 0);
    const Checked withoutLayout = Run(env, sample, false);
    CHECK(withoutLayout.error == ErrorCode::None);
    REQUIRE(WriteSemanticBytes(sample.document, env.registry, env.allocator, after));
    CHECK(SameBytes(before, after));
}

TEST(NodeGraph_ValidateGI1EdgeNodesInGraph) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample sample(env);
    // 다른 그래프(하위 그래프)의 노드에서 오는 간선과 없는 노드로 가는 간선.
    REQUIRE(AddEdge(env, sample, MakeEdge(SampleInner, env.pinResult, SampleStepA, env.pinA)));
    const Checked checked = Run(env, sample);
    CHECK(Only(checked, GraphInvariant::EdgeNodesInGraph, GraphSubject::Edge));
    CHECK(checked.violations.items[0].related == SampleInner);
    CHECK(checked.violations.items[0].pin == env.pinResult);

    Sample missing(env);
    REQUIRE(AddEdge(env, missing, MakeEdge(SampleAdd, env.pinResult, Id(77), env.pinA)));
    const Checked second = Run(env, missing);
    CHECK(Only(second, GraphInvariant::EdgeNodesInGraph, GraphSubject::Edge));
    CHECK(second.violations.items[0].node == Id(77));
}

TEST(NodeGraph_ValidateGI2EdgePins) {
    TestEnv env;
    REQUIRE(env.ready);
    // 출력 핀을 입력 끝으로 쓴다.
    Sample reversed(env);
    REQUIRE(AddEdge(env, reversed, MakeEdge(SampleParamB, env.pinValue, SampleStepB, env.pinThen)));
    const Checked first = Run(env, reversed);
    CHECK(Only(first, GraphInvariant::EdgePinExists, GraphSubject::Edge));
    CHECK(first.violations.items[0].pin == env.pinThen);

    // 없는 핀 키, 출력 끝의 입력 핀.
    Sample unknownPin(env);
    REQUIRE(AddEdge(env, unknownPin, MakeEdge(SampleStepA, env.pinIn, SampleToggle, env.pinOther)));
    const Checked second = Run(env, unknownPin);
    REQUIRE(second.error == ErrorCode::InvalidArgument);
    CHECK(second.violations.items.Size() == 2);
    CHECK(second.violations.items[0].rule == GraphInvariant::EdgePinExists &&
          second.violations.items[1].rule == GraphInvariant::EdgePinExists);

    // 입력 고아 핀(Gone)을 출력 끝으로 쓴다(고아 핀은 방향만 본다).
    Sample retained(env);
    REQUIRE(AddEdge(env, retained, MakeEdge(SampleAdd, env.pinGone, SampleDefaults, env.pinA)));
    const Checked third = Run(env, retained);
    CHECK(Only(third, GraphInvariant::EdgePinExists, GraphSubject::Edge));
    CHECK(third.violations.items[0].pin == env.pinGone);

    // 고아 핀을 맞는 방향으로 쓰면 통과한다.
    Sample retainedOk(env);
    REQUIRE(AddEdge(env, retainedOk, MakeEdge(SampleParamB, env.pinValue, SampleAdd, env.pinGone)));
    CHECK(Run(env, retainedOk).error == ErrorCode::None);
}

TEST(NodeGraph_ValidateGI3InputEdgeLimit) {
    TestEnv env;
    REQUIRE(env.ready);
    // Add.A(간선 1개)에 둘째 간선.
    Sample single(env);
    REQUIRE(AddEdge(env, single, MakeEdge(SampleParamB, env.pinValue, SampleAdd, env.pinA)));
    const Checked first = Run(env, single);
    CHECK(Only(first, GraphInvariant::InputEdgeLimit, GraphSubject::Edge));
    CHECK(first.violations.items[0].pin == env.pinA);

    // Multi.Items는 둘까지 받고 셋째에서 위반이다.
    Sample multi(env);
    REQUIRE(AddNode(env, multi, MakeNode(env, Id(50), env.typeMulti)));
    REQUIRE(AddEdge(env, multi, MakeEdge(SampleParamA, env.pinValue, Id(50), env.pinItems)));
    REQUIRE(AddEdge(env, multi, MakeEdge(SampleParamB, env.pinValue, Id(50), env.pinItems)));
    CHECK(Run(env, multi).error == ErrorCode::None);
    REQUIRE(AddEdge(env, multi, MakeEdge(SampleAdd, env.pinResult, Id(50), env.pinItems)));
    CHECK(Only(Run(env, multi), GraphInvariant::InputEdgeLimit, GraphSubject::Edge));
}

TEST(NodeGraph_ValidateGI4DuplicateEdge) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample sample(env);
    // 같은 간선을 두 번 넣는다(입력 상한 위반이 함께 나오지 않게 간선 2개까지 받는 Multi.Items로
    // 본다).
    REQUIRE(AddNode(env, sample, MakeNode(env, Id(50), env.typeMulti)));
    REQUIRE(AddEdge(env, sample, MakeEdge(SampleParamA, env.pinValue, Id(50), env.pinItems)));
    REQUIRE(AddEdge(env, sample, MakeEdge(SampleParamA, env.pinValue, Id(50), env.pinItems)));
    const Checked checked = Run(env, sample);
    CHECK(Only(checked, GraphInvariant::DuplicateEdge, GraphSubject::Edge));
}

TEST(NodeGraph_ValidateGI5CanonicalOrder) {
    TestEnv env;
    REQUIRE(env.ready);
    // 노드 순서.
    Sample nodes(env);
    Array<GraphNode>& list = nodes.Main().nodes;
    GraphNode first = Move(list[0]);
    list[0] = Move(list[1]);
    list[1] = Move(first);
    CHECK(Only(Run(env, nodes), GraphInvariant::CanonicalOrder, GraphSubject::Node));

    // 간선 순서.
    Sample edges(env);
    GraphEdge edge = edges.Main().edges[0];
    edges.Main().edges[0] = edges.Main().edges[1];
    edges.Main().edges[1] = edge;
    CHECK(Only(Run(env, edges), GraphInvariant::CanonicalOrder, GraphSubject::Edge));

    // 그래프 순서(배치 그래프 순서도 같이 바꾸면 배치 쪽 위반도 나온다 — 문서만 바꾼다).
    Sample graphs(env);
    Graph main = Move(graphs.document.graphs[0]);
    graphs.document.graphs[0] = Move(graphs.document.graphs[1]);
    graphs.document.graphs[1] = Move(main);
    CHECK(Only(Run(env, graphs, false), GraphInvariant::CanonicalOrder, GraphSubject::Graph));

    // 입력 기본값 순서·중복.
    Sample defaults(env);
    PinValueEntry extra(env.allocator);
    extra.pin = env.pinA;
    REQUIRE(FillRecord(extra.value.bytes, TestValueStruct, 1, 1));
    REQUIRE(defaults.Node(SampleDefaults).inputDefaults.TryPushBack(Move(extra)));
    CHECK(Only(Run(env, defaults), GraphInvariant::CanonicalOrder, GraphSubject::InputDefault));

    // 고아 핀 순서: Gone 앞에 Other(순서 뒤)를 둔다.
    Sample retained(env);
    RetainedPin other(env.allocator);
    other.key = env.pinOther;
    REQUIRE(FillRecord(other.value.bytes, TestValueStruct, 1, 9));
    REQUIRE(retained.Node(SampleAdd).retainedPins.TryInsert(Move(other), 0));
    CHECK(Only(Run(env, retained), GraphInvariant::CanonicalOrder, GraphSubject::RetainedPin));

    // ordered 출력 핀 순번: 0, 2(빈틈).
    Sample gap(env);
    for (GraphEdge& item : gap.Main().edges) {
        if (item.toNode == SampleStepB) {
            item.order = 2;
        }
    }

    const Checked gapChecked = Run(env, gap);
    CHECK(Only(gapChecked, GraphInvariant::CanonicalOrder, GraphSubject::Edge));
    CHECK(gapChecked.violations.items[0].pin == env.pinThen);

    // ordered 출력 핀 순번 중복: 0, 0.
    Sample same(env);
    for (GraphEdge& item : same.Main().edges) {
        if (item.toNode == SampleStepB) {
            item.order = 0;
        }
    }

    CHECK(Only(Run(env, same), GraphInvariant::CanonicalOrder, GraphSubject::Edge));

    // ordered가 아닌 출력 핀의 순번은 0이어야 한다.
    Sample unordered(env);
    for (GraphEdge& item : unordered.Main().edges) {
        if (item.toNode == SampleToggle) {
            item.order = 1;
        }
    }

    CHECK(Only(Run(env, unordered), GraphInvariant::CanonicalOrder, GraphSubject::Edge));

    // 배치 노드·주석·북마크 순서.
    Sample layoutNodes(env);
    Array<NodeLayout>& positions = LayoutOf(layoutNodes, SampleMain).nodes;
    NodeLayout moved = Move(positions[0]);
    positions[0] = Move(positions[1]);
    positions[1] = Move(moved);
    CHECK(Only(Run(env, layoutNodes), GraphInvariant::CanonicalOrder, GraphSubject::LayoutNode));

    Sample bookmarks(env);
    Array<Bookmark>& marks = LayoutOf(bookmarks, SampleMain).bookmarks;
    marks[1].number = 1;
    CHECK(Only(Run(env, bookmarks), GraphInvariant::CanonicalOrder, GraphSubject::Bookmark));

    Sample comments(env);
    CommentBox earlier(env.allocator);
    earlier.id = Id(39);
    REQUIRE(LayoutOf(comments, SampleMain).comments.TryPushBack(Move(earlier)));
    CHECK(Only(Run(env, comments), GraphInvariant::CanonicalOrder, GraphSubject::Comment));
}

TEST(NodeGraph_ValidateGI6LayoutMatchesDocument) {
    TestEnv env;
    REQUIRE(env.ready);
    // 배치에 노드 항목이 없다.
    Sample missing(env);
    REQUIRE(LayoutOf(missing, SampleMain).nodes.TryRemoveAt(0));
    const Checked first = Run(env, missing);
    CHECK(Only(first, GraphInvariant::LayoutMatchesDocument, GraphSubject::LayoutNode));
    CHECK(first.violations.items[0].node == SampleParamA);

    // 배치에 없는 노드 항목이 있다.
    Sample extra(env);
    NodeLayout stray(env.allocator);
    stray.node = Id(99);
    REQUIRE(LayoutOf(extra, SampleMain).nodes.TryPushBack(Move(stray)));
    CHECK(Only(Run(env, extra), GraphInvariant::LayoutMatchesDocument, GraphSubject::LayoutNode));

    // 같은 노드 항목이 둘이다.
    Sample twice(env);
    NodeLayout copy(env.allocator);
    copy.node = SampleUnknown;
    REQUIRE(LayoutOf(twice, SampleMain).nodes.TryPushBack(Move(copy)));
    CHECK(Only(Run(env, twice), GraphInvariant::LayoutMatchesDocument, GraphSubject::LayoutNode));

    // 그래프 배치가 없다 / 없는 그래프의 배치가 있다.
    Sample noGraph(env);
    REQUIRE(noGraph.layout.graphs.TryRemoveAt(1));
    const Checked noGraphChecked = Run(env, noGraph);
    CHECK(Only(noGraphChecked, GraphInvariant::LayoutMatchesDocument, GraphSubject::LayoutGraph));
    CHECK(noGraphChecked.violations.items[0].graph == SampleCollapsed);

    Sample strayGraph(env);
    GraphLayout unknownGraph(env.allocator);
    unknownGraph.graph = Id(90);
    REQUIRE(strayGraph.layout.graphs.TryPushBack(Move(unknownGraph)));
    CHECK(Only(Run(env, strayGraph), GraphInvariant::LayoutMatchesDocument,
               GraphSubject::LayoutGraph));
}

TEST(NodeGraph_ValidateGI7RetainedPinUnused) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample sample(env);
    sample.Node(SampleAdd).retainedPins[0].value.bytes.Clear();
    const Checked checked = Run(env, sample);
    CHECK(Only(checked, GraphInvariant::RetainedPinUnused, GraphSubject::RetainedPin));
    CHECK(checked.violations.items[0].pin == env.pinGone);

    // 값이 없어도 간선이 있으면 통과한다.
    REQUIRE(AddEdge(env, sample, MakeEdge(SampleParamB, env.pinValue, SampleAdd, env.pinGone)));
    CHECK(Run(env, sample).error == ErrorCode::None);
}

TEST(NodeGraph_ValidateGI8RetainedPinShadowed) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample sample(env);
    RetainedPin shadow(env.allocator);
    shadow.key = env.Intern("b");
    REQUIRE(FillRecord(shadow.value.bytes, TestValueStruct, 1, 1));
    REQUIRE(sample.Node(SampleAdd).retainedPins.TryInsert(Move(shadow), 0));
    const Checked checked = Run(env, sample);
    CHECK(Only(checked, GraphInvariant::RetainedPinShadowed, GraphSubject::RetainedPin));
}

TEST(NodeGraph_ValidateGI9DuplicateId) {
    TestEnv env;
    REQUIRE(env.ready);
    // 하위 그래프 노드가 주 그래프 노드와 같은 id다.
    Sample nodes(env);
    nodes.document.graphs[1].nodes[0].id = SampleAdd;
    LayoutOf(nodes, SampleCollapsed).nodes[0].node = SampleAdd;
    const Checked first = Run(env, nodes);
    CHECK(Only(first, GraphInvariant::DuplicateId, GraphSubject::Node));

    // 주석 id가 노드 id와 같다.
    Sample comment(env);
    LayoutOf(comment, SampleMain).comments[0].id = SampleUnknown;
    const Checked second = Run(env, comment);
    CHECK(Only(second, GraphInvariant::DuplicateId, GraphSubject::Comment));

    // 그래프 id가 노드 id와 같다.
    Sample graph(env);
    graph.document.graphs[1].nodes[0].id = SampleCollapsed;
    LayoutOf(graph, SampleCollapsed).nodes[0].node = SampleCollapsed;
    REQUIRE(CanonicalizeGraphDocument(graph.document, &graph.layout, env.names, env.allocator));
    CHECK(Only(Run(env, graph), GraphInvariant::DuplicateId, GraphSubject::Node));
}

TEST(NodeGraph_ValidateGI10RerouteCycle) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample sample(env);
    REQUIRE(AddNode(env, sample, MakeNode(env, Id(60), env.reroute)));
    REQUIRE(AddNode(env, sample, MakeNode(env, Id(61), env.reroute)));
    REQUIRE(AddEdge(env, sample, MakeEdge(Id(60), env.pinOut, Id(61), env.pinIn)));
    REQUIRE(AddEdge(env, sample, MakeEdge(Id(61), env.pinOut, Id(60), env.pinIn)));
    const Checked checked = Run(env, sample);
    CHECK(Only(checked, GraphInvariant::RerouteCycle, GraphSubject::Node));

    // 자기 자신으로 도는 재경유.
    Sample self(env);
    REQUIRE(AddNode(env, self, MakeNode(env, Id(60), env.reroute)));
    REQUIRE(AddEdge(env, self, MakeEdge(Id(60), env.pinOut, Id(60), env.pinIn)));
    CHECK(Only(Run(env, self), GraphInvariant::RerouteCycle, GraphSubject::Node));

    // 사슬(순환 없음)은 통과한다.
    Sample chain(env);
    REQUIRE(AddNode(env, chain, MakeNode(env, Id(60), env.reroute)));
    REQUIRE(AddEdge(env, chain, MakeEdge(SampleReroute, env.pinOut, Id(60), env.pinIn)));
    CHECK(Run(env, chain).error == ErrorCode::None);
}

TEST(NodeGraph_ValidateGI11EmptyIdentity) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample domain(env);
    domain.document.domain = Name();
    // 도메인이 None이면 시험 schema의 종류를 찾지 못해 노드가 알 수 없는 종류가 된다 — 도메인
    // 위반만 본다.
    const Checked first = Run(env, domain);
    CHECK(first.error == ErrorCode::InvalidArgument);
    CHECK(first.violations.Has(GraphInvariant::EmptyIdentity, GraphSubject::Document));

    Sample nodeType(env);
    nodeType.Node(SampleUnknown).type = Name();
    CHECK(Only(Run(env, nodeType), GraphInvariant::EmptyIdentity, GraphSubject::Node));

    Sample edgePin(env);
    REQUIRE(AddEdge(env, edgePin, MakeEdge(SampleParamA, Name(), SampleStepA, env.pinA)));
    const Checked edgeChecked = Run(env, edgePin);
    CHECK(edgeChecked.error == ErrorCode::InvalidArgument);
    CHECK(edgeChecked.violations.Has(GraphInvariant::EmptyIdentity, GraphSubject::Edge));

    Sample comment(env);
    LayoutOf(comment, SampleMain).comments[0].id = Guid{};
    REQUIRE(CanonicalizeGraphDocument(comment.document, &comment.layout, env.names, env.allocator));
    CHECK(Only(Run(env, comment), GraphInvariant::EmptyIdentity, GraphSubject::Comment));

    Sample inputDefault(env);
    inputDefault.Node(SampleDefaults).inputDefaults[0].pin = Name();
    CHECK(Only(Run(env, inputDefault), GraphInvariant::EmptyIdentity, GraphSubject::InputDefault));
}

TEST(NodeGraph_ValidateGI12DocumentHeader) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample version(env);
    version.document.documentFormatVersion = 0;
    CHECK(Only(Run(env, version), GraphInvariant::DocumentHeader, GraphSubject::Document));
    version.document.documentFormatVersion = GraphDocumentFormatVersion + 1;
    CHECK(Only(Run(env, version), GraphInvariant::DocumentHeader, GraphSubject::Document));

    Sample mainMissing(env);
    mainMissing.document.mainGraph = Id(99);
    const Checked missing = Run(env, mainMissing);
    REQUIRE(missing.error == ErrorCode::InvalidArgument);
    // 주 그래프가 없고, Main 역할 그래프가 mainGraph가 아니다.
    CHECK(missing.violations.Has(GraphInvariant::DocumentHeader, GraphSubject::Document));
    CHECK(missing.violations.Has(GraphInvariant::DocumentHeader, GraphSubject::Graph));

    Sample secondMain(env);
    secondMain.document.graphs[1].role = GraphRole::Main;
    CHECK(Only(Run(env, secondMain), GraphInvariant::DocumentHeader, GraphSubject::Graph));

    Sample domainRole(env);
    domainRole.document.graphs[1].domainRole = 3;
    CHECK(Only(Run(env, domainRole), GraphInvariant::DocumentHeader, GraphSubject::Graph));
    domainRole.document.graphs[1].role = GraphRole::DomainDefined;
    CHECK(Run(env, domainRole).error == ErrorCode::None);

    Sample badRole(env);
    badRole.document.graphs[1].role = static_cast<GraphRole>(9);
    CHECK(Only(Run(env, badRole), GraphInvariant::DocumentHeader, GraphSubject::Graph));
}

TEST(NodeGraph_ValidateGI13NodeTypeData) {
    TestEnv env;
    REQUIRE(env.ready);
    auto rejectsNode = [&env](auto&& edit) noexcept {
        Sample sample(env);
        edit(sample);
        return Only(Run(env, sample), GraphInvariant::NodeTypeData, GraphSubject::Node);
    };

    // 버전 0, 서술보다 새 버전. 낮은 버전(migration 대상)은 통과한다.
    CHECK(rejectsNode(
        [](Sample& sample) noexcept { sample.Node(SampleParamA).typeSchemaVersion = 0; }));
    CHECK(rejectsNode([](Sample& sample) noexcept {
        sample.Node(SampleParamA).typeSchemaVersion = TestParamVersion + 1;
    }));
    Sample older(env);
    older.Node(SampleParamA).typeSchemaVersion = 1;
    CHECK(Run(env, older).error == ErrorCode::None);

    // 속성: 다른 schema의 Record, 빈 속성(속성 struct가 있는 종류), 속성 없는 종류에 바이트, Record
    // 아닌 바이트.
    CHECK(rejectsNode([](Sample& sample) noexcept {
        sample.ok = FillRecord(sample.Node(SampleParamA).properties.bytes, TestValueStruct, 2, 5);
    }));
    CHECK(rejectsNode(
        [](Sample& sample) noexcept { sample.Node(SampleParamA).properties.bytes.Clear(); }));
    CHECK(rejectsNode([](Sample& sample) noexcept {
        sample.ok = FillRecord(sample.Node(SampleAdd).properties.bytes, TestValueStruct, 1, 5);
    }));
    CHECK(rejectsNode([](Sample& sample) noexcept {
        Array<uint8>& bytes = sample.Node(SampleParamA).properties.bytes;
        bytes.Clear();
        sample.ok = bytes.TryPushBack(1) && bytes.TryPushBack(2);
    }));

    // 비활성 허용이 없는 종류의 Disabled, 범위 밖 활성 상태.
    CHECK(rejectsNode([](Sample& sample) noexcept {
        sample.Node(SampleAdd).enableState = NodeEnableState::Disabled;
    }));
    CHECK(rejectsNode([](Sample& sample) noexcept {
        sample.Node(SampleAdd).enableState = static_cast<NodeEnableState>(5);
    }));

    // 동적 핀을 유도할 수 없는 속성(Record 버전 0은 header 오류, 버전 10은 함수 거부).
    CHECK(rejectsNode([](Sample& sample) noexcept {
        sample.ok = FillRecord(sample.Node(SampleDynamic).properties.bytes, TestDynamicStruct,
                               TestDynamicMaxItems + 1, 0);
    }));

    // 범위 밖 고아 핀 방향.
    Sample direction(env);
    direction.Node(SampleAdd).retainedPins[0].direction = static_cast<PinDirection>(4);
    CHECK(Only(Run(env, direction), GraphInvariant::NodeTypeData, GraphSubject::RetainedPin));
}

TEST(NodeGraph_ValidateGI14InputDefaultPin) {
    TestEnv env;
    REQUIRE(env.ready);
    // 출력 핀의 기본값.
    Sample output(env);
    output.Node(SampleDefaults).inputDefaults[0].pin = env.pinResult;
    CHECK(Only(Run(env, output), GraphInvariant::InputDefaultPin, GraphSubject::InputDefault));

    // 없는 핀의 기본값.
    Sample missing(env);
    missing.Node(SampleDefaults).inputDefaults[0].pin = env.pinOther;
    CHECK(Only(Run(env, missing), GraphInvariant::InputDefaultPin, GraphSubject::InputDefault));

    // 빈 값.
    Sample empty(env);
    empty.Node(SampleDefaults).inputDefaults[0].value.bytes.Clear();
    CHECK(Only(Run(env, empty), GraphInvariant::InputDefaultPin, GraphSubject::InputDefault));

    // 동적 입력 핀의 기본값은 통과한다.
    Sample dynamic(env);
    PinValueEntry item(env.allocator);
    item.pin = env.pinItem0;
    REQUIRE(FillRecord(item.value.bytes, TestValueStruct, 1, 2));
    REQUIRE(dynamic.Node(SampleDynamic).inputDefaults.TryPushBack(Move(item)));
    CHECK(Run(env, dynamic).error == ErrorCode::None);
}

// 알 수 없는 종류의 노드는 핀 검사(GI2·GI3·순번·GI8·GI13·GI14)를 건너뛰지만 나머지는 검사한다.
TEST(NodeGraph_ValidateUnknownNodeScope) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample sample(env);
    GraphNode& unknown = sample.Node(SampleUnknown);
    unknown.typeSchemaVersion = 99;
    REQUIRE(FillRecord(unknown.properties.bytes, TestValueStruct, 1, 1));
    PinValueEntry anyDefault(env.allocator);
    anyDefault.pin = env.pinOther;
    REQUIRE(unknown.inputDefaults.TryPushBack(Move(anyDefault)));
    unknown.enableState = NodeEnableState::Disabled;
    REQUIRE(
        AddEdge(env, sample, MakeEdge(SampleUnknown, env.pinOther, SampleUnknown, env.pinA, 5)));
    REQUIRE(AddEdge(env, sample, MakeEdge(SampleUnknown, env.pinB, SampleUnknown, env.pinA)));
    CHECK(Run(env, sample).error == ErrorCode::None);

    // GI7(고아 핀 사용)은 알 수 없는 종류에도 본다(입력 기본값 순서 GI5는
    // NodeGraphValidationCaseTests).
    RetainedPin idle(env.allocator);
    idle.key = env.pinGone;
    REQUIRE(sample.Node(SampleUnknown).retainedPins.TryPushBack(Move(idle)));
    CHECK(Only(Run(env, sample), GraphInvariant::RetainedPinUnused, GraphSubject::RetainedPin));
}

TEST(NodeGraph_ValidateViolationCap) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample sample(env);
    constexpr uint32 Extra = MaxReportedGraphViolations + 44;
    for (uint32 index = 0; index < Extra; ++index) {
        REQUIRE(sample.Main().edges.TryPushBack(
            MakeEdge(Id(1000 + index), env.pinResult, SampleStepA, env.pinA)));
    }

    REQUIRE(CanonicalizeGraphDocument(sample.document, &sample.layout, env.names, env.allocator));
    const Checked checked = Run(env, sample);
    CHECK(checked.error == ErrorCode::InvalidArgument);
    CHECK(checked.violations.items.Size() == MaxReportedGraphViolations);
    CHECK(checked.violations.dropped == Extra - MaxReportedGraphViolations);
}

// 정규화는 섞인 배열을 정규 순서로 맞추고(검사 통과), 두 번 해도 같다. 섞인 입력의 의미 바이트는
// 정규화한 표본과 같다.
TEST(NodeGraph_CanonicalizeSortsAndIsIdempotent) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample canonical(env);
    REQUIRE(canonical.ok);
    Sample shuffled(env);
    REQUIRE(shuffled.ok);
    // 모든 배열을 뒤집는다.
    auto reverse = [](auto& array) noexcept {
        const usize size = array.Size();
        for (usize index = 0; index < size / 2; ++index) {
            auto temporary = Move(array[index]);
            array[index] = Move(array[size - 1 - index]);
            array[size - 1 - index] = Move(temporary);
        }
    };

    reverse(shuffled.document.graphs);
    for (Graph& graph : shuffled.document.graphs) {
        reverse(graph.nodes);
        reverse(graph.edges);
    }

    reverse(shuffled.layout.graphs);
    for (GraphLayout& graph : shuffled.layout.graphs) {
        reverse(graph.nodes);
        reverse(graph.bookmarks);
    }

    GraphViolationList violations(env.allocator);
    CHECK(!Validate(env, shuffled.document, &shuffled.layout, violations));
    REQUIRE(
        CanonicalizeGraphDocument(shuffled.document, &shuffled.layout, env.names, env.allocator));
    CHECK(Validate(env, shuffled.document, &shuffled.layout, violations));

    Array<uint8> expected(env.allocator);
    Array<uint8> actual(env.allocator);
    REQUIRE(WriteSemanticBytes(canonical.document, env.registry, env.allocator, expected));
    REQUIRE(WriteSemanticBytes(shuffled.document, env.registry, env.allocator, actual));
    CHECK(SameBytes(expected, actual));

    // 멱등: 다시 정규화해도 의미 바이트·검사 결과가 같다.
    REQUIRE(
        CanonicalizeGraphDocument(shuffled.document, &shuffled.layout, env.names, env.allocator));
    REQUIRE(WriteSemanticBytes(shuffled.document, env.registry, env.allocator, actual));
    CHECK(SameBytes(expected, actual));
    CHECK(Validate(env, shuffled.document, &shuffled.layout, violations));

    // 간선 순서는 PinKey 원문 순서다(표시 대소문자와 무관): Defaults.A < Defaults.B.
    const Graph& main = shuffled.Main();
    for (usize index = 1; index < main.edges.Size(); ++index) {
        if (main.edges[index].toNode == main.edges[index - 1].toNode) {
            auto order =
                CompareNames(env.names, main.edges[index - 1].toPin, main.edges[index].toPin);
            REQUIRE(order);
            CHECK(order.Value() <= 0);
        }
    }
}

} // namespace node_graph_validation_tests_file
