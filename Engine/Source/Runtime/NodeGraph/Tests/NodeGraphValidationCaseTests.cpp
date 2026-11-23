#include "NodeGraphTestSupport.h"

// 불변식 검사의 경계 사례 시험이다(Docs/Design/NodeGraph 6.3절, 13절 GT1): 간선 수를 세지
// 않는 끝(고아 핀·알 수 없는 종류), 배치 그래프 순서·중복, 고아 핀 키 중복, id 겹침의 여러 모양,
// 순환으로 들어가는 재경유 사슬, 0 id·None·잘못된 토큰, 주 그래프 지정 오류, 동적 핀 오류 경로(함수
// 실패·키 중복·타입 범위 밖·핀 수 초과), 재경유의 기본값·고아 핀.
using namespace rocks;
namespace node_graph_validation_case_tests_file {

using namespace rocks::nodegraph;
using namespace rocks::nodegraph_test;

struct CaseResult {
    explicit CaseResult(TestEnv& env) noexcept : violations(env.allocator) {
    }

    GraphViolationList violations;
    ErrorCode error = ErrorCode::None;
};

CaseResult Check(TestEnv& env, Sample& sample, bool withLayout = true) noexcept {
    CaseResult result(env);
    auto validated =
        Validate(env, sample.document, withLayout ? &sample.layout : nullptr, result.violations);
    result.error = validated ? ErrorCode::None : validated.Error();
    return result;
}

bool Only(const CaseResult& result, GraphInvariant rule, GraphSubject subject) noexcept {
    return result.error == ErrorCode::InvalidArgument &&
           OnlyViolation(result.violations, rule, subject);
}

GraphLayout& LayoutFor(Sample& sample, Guid graph) noexcept {
    for (GraphLayout& layout : sample.layout.graphs) {
        if (layout.graph == graph) {
            return layout;
        }
    }

    sample.ok = false;
    return sample.layout.graphs[0];
}

bool Canonical(TestEnv& env, Sample& sample) noexcept {
    return static_cast<bool>(
        CanonicalizeGraphDocument(sample.document, &sample.layout, env.names, env.allocator));
}

bool PushEdge(TestEnv& env, Sample& sample, const GraphEdge& edge) noexcept {
    return sample.Main().edges.TryPushBack(edge) && Canonical(env, sample);
}

bool PushNode(TestEnv& env, Sample& sample, GraphNode&& node) noexcept {
    NodeLayout position(env.allocator);
    position.node = node.id;
    return LayoutFor(sample, SampleMain).nodes.TryPushBack(Move(position)) &&
           sample.Main().nodes.TryPushBack(Move(node)) && Canonical(env, sample);
}

// Test.Dynamic 노드의 속성 Record schema version을 바꾼다(오류 경로 선택).
bool SetDynamicVersion(Sample& sample, uint32 version) noexcept {
    return FillRecord(sample.Node(SampleDynamic).properties.bytes, TestDynamicStruct, version, 0);
}

// GI3은 현재 핀 목록의 입력 핀만 센다: 고아 입력 핀과 알 수 없는 종류로 들어가는 간선은 세지
// 않는다.
TEST(NodeGraph_CaseGI3SkipsRetainedAndUnknownEnds) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample sample(env);
    REQUIRE(PushEdge(env, sample, MakeEdge(SampleParamA, env.pinValue, SampleAdd, env.pinGone)));
    REQUIRE(PushEdge(env, sample, MakeEdge(SampleParamB, env.pinValue, SampleAdd, env.pinGone)));
    REQUIRE(PushEdge(env, sample, MakeEdge(SampleParamA, env.pinValue, SampleUnknown, env.pinA)));
    REQUIRE(PushEdge(env, sample, MakeEdge(SampleParamB, env.pinValue, SampleUnknown, env.pinA)));
    REQUIRE(PushEdge(env, sample, MakeEdge(SampleAdd, env.pinResult, SampleUnknown, env.pinA)));
    CHECK(Check(env, sample).error == ErrorCode::None);
}

// ordered 순번은 현재 핀 목록의 출력 핀만 본다: 고아 출력 핀·알 수 없는 종류에서 나온 간선의 순번은
// 보지 않는다.
TEST(NodeGraph_CaseOrderSkipsRetainedAndUnknownSources) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample sample(env);
    RetainedPin output(env.allocator);
    output.key = env.pinOther;
    output.direction = PinDirection::Output;
    REQUIRE(sample.Node(SampleParamB).retainedPins.TryPushBack(Move(output)));
    REQUIRE(PushEdge(env, sample, MakeEdge(SampleParamB, env.pinOther, SampleStepA, env.pinA, 3)));
    REQUIRE(PushEdge(env, sample, MakeEdge(SampleUnknown, env.pinThen, SampleStepB, env.pinA, 7)));
    // StepA·StepB(Sequence)에는 A 핀이 없으므로 그 끝만 GI2로 보고된다.
    const CaseResult result = Check(env, sample);
    REQUIRE(result.error == ErrorCode::InvalidArgument);
    for (const GraphViolation& violation : result.violations.items) {
        CHECK(violation.rule == GraphInvariant::EdgePinExists && violation.pin == env.pinA);
    }

    CHECK(result.violations.items.Size() == 2);
}

TEST(NodeGraph_CaseGI5LayoutGraphOrderAndRetainedDuplicate) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample graphs(env);
    GraphLayout first = Move(graphs.layout.graphs[0]);
    graphs.layout.graphs[0] = Move(graphs.layout.graphs[1]);
    graphs.layout.graphs[1] = Move(first);
    CHECK(Only(Check(env, graphs), GraphInvariant::CanonicalOrder, GraphSubject::LayoutGraph));

    Sample retained(env);
    RetainedPin again(env.allocator);
    again.key = env.Intern("GONE");
    REQUIRE(FillRecord(again.value.bytes, TestValueStruct, 1, 8));
    REQUIRE(retained.Node(SampleAdd).retainedPins.TryPushBack(Move(again)));
    CHECK(Only(Check(env, retained), GraphInvariant::CanonicalOrder, GraphSubject::RetainedPin));

    // 알 수 없는 종류의 입력 기본값도 순서를 본다(B 다음 A).
    Sample unknown(env);
    const Name reversedPins[] = {env.pinB, env.pinA};
    for (Name pin : reversedPins) {
        PinValueEntry entry(env.allocator);
        entry.pin = pin;
        REQUIRE(FillRecord(entry.value.bytes, TestValueStruct, 1, 1));
        REQUIRE(unknown.Node(SampleUnknown).inputDefaults.TryPushBack(Move(entry)));
    }

    CHECK(Only(Check(env, unknown), GraphInvariant::CanonicalOrder, GraphSubject::InputDefault));
}

TEST(NodeGraph_CaseGI6DuplicateLayoutGraph) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample sample(env);
    GraphLayout copy(env.allocator);
    copy.graph = SampleCollapsed;
    NodeLayout inner(env.allocator);
    inner.node = SampleInner;
    REQUIRE(copy.nodes.TryPushBack(Move(inner)));
    REQUIRE(sample.layout.graphs.TryPushBack(Move(copy)));
    CHECK(
        Only(Check(env, sample), GraphInvariant::LayoutMatchesDocument, GraphSubject::LayoutGraph));
}

TEST(NodeGraph_CaseGI9IdShapes) {
    TestEnv env;
    REQUIRE(env.ready);
    // 그래프 id끼리 겹친다(배치도 같이 바꾼다).
    Sample graphs(env);
    graphs.document.graphs[1].id = SampleMain;
    LayoutFor(graphs, SampleCollapsed).graph = SampleMain;
    const CaseResult first = Check(env, graphs, false);
    REQUIRE(first.error == ErrorCode::InvalidArgument);
    CHECK(first.violations.Has(GraphInvariant::DuplicateId, GraphSubject::Graph));

    // 서로 다른 그래프 배치의 주석 id가 겹친다.
    Sample comments(env);
    CommentBox other(env.allocator);
    other.id = SampleComment;
    REQUIRE(LayoutFor(comments, SampleCollapsed).comments.TryPushBack(Move(other)));
    CHECK(Only(Check(env, comments), GraphInvariant::DuplicateId, GraphSubject::Comment));
}

// 사슬이 순환으로 들어가는 모양(R62 → R60 ↔ R61)도 순환 하나로 한 번만 보고한다.
TEST(NodeGraph_CaseGI10ChainIntoCycle) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample sample(env);
    const uint64 reroutes[] = {60, 61, 62};
    for (uint64 id : reroutes) {
        REQUIRE(PushNode(env, sample, MakeNode(env, Id(id), env.reroute)));
    }

    REQUIRE(PushEdge(env, sample, MakeEdge(Id(60), env.pinOut, Id(61), env.pinIn)));
    REQUIRE(PushEdge(env, sample, MakeEdge(Id(61), env.pinOut, Id(60), env.pinIn)));
    REQUIRE(PushEdge(env, sample, MakeEdge(Id(60), env.pinOut, Id(62), env.pinIn)));
    CHECK(Only(Check(env, sample), GraphInvariant::RerouteCycle, GraphSubject::Node));
}

TEST(NodeGraph_CaseGI11ZeroIdsAndInvalidTokens) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample graph(env);
    graph.document.graphs[1].id = Guid{};
    LayoutFor(graph, SampleCollapsed).graph = Guid{};
    REQUIRE(Canonical(env, graph));
    CHECK(Only(Check(env, graph), GraphInvariant::EmptyIdentity, GraphSubject::Graph));

    Sample node(env);
    node.Node(SampleUnknown).id = Guid{};
    LayoutFor(node, SampleMain).nodes[10].node = Guid{};
    REQUIRE(Canonical(env, node));
    const CaseResult nodeResult = Check(env, node);
    REQUIRE(nodeResult.error == ErrorCode::InvalidArgument);
    CHECK(nodeResult.violations.Has(GraphInvariant::EmptyIdentity, GraphSubject::Node));

    Sample edge(env);
    REQUIRE(PushEdge(env, edge, MakeEdge(Guid{}, env.pinResult, SampleStepA, env.pinIn)));
    CHECK(Only(Check(env, edge), GraphInvariant::EmptyIdentity, GraphSubject::Edge));

    Sample retained(env);
    retained.Node(SampleAdd).retainedPins[0].key = Name();
    CHECK(Only(Check(env, retained), GraphInvariant::EmptyIdentity, GraphSubject::RetainedPin));

    // 구조가 잘못된 토큰은 오류로 검사를 끝내지 않고 GI11로 보고한다.
    Name broken;
    broken.kind = NameKind::Normal;
    Sample brokenEdge(env);
    for (GraphEdge& item : brokenEdge.Main().edges) {
        if (item.toNode == SampleToggle) {
            item.fromPin = broken;
        }
    }

    CHECK(Only(Check(env, brokenEdge), GraphInvariant::EmptyIdentity, GraphSubject::Edge));
    Sample brokenType(env);
    brokenType.Node(SampleUnknown).type = broken;
    CHECK(Only(Check(env, brokenType), GraphInvariant::EmptyIdentity, GraphSubject::Node));
    Sample brokenDomain(env);
    brokenDomain.document.domain = broken;
    const CaseResult domain = Check(env, brokenDomain);
    REQUIRE(domain.error == ErrorCode::InvalidArgument);
    CHECK(domain.violations.Has(GraphInvariant::EmptyIdentity, GraphSubject::Document));
}

TEST(NodeGraph_CaseGI12MainGraphTarget) {
    TestEnv env;
    REQUIRE(env.ready);
    // mainGraph가 Collapsed 역할 그래프를 가리킨다: 그 그래프(역할 불일치)와 Main 역할 그래프(주
    // 그래프 아님).
    Sample collapsed(env);
    collapsed.document.mainGraph = SampleCollapsed;
    const CaseResult first = Check(env, collapsed);
    REQUIRE(first.error == ErrorCode::InvalidArgument);
    CHECK(first.violations.items.Size() == 2);
    for (const GraphViolation& violation : first.violations.items) {
        CHECK(violation.rule == GraphInvariant::DocumentHeader &&
              violation.subject == GraphSubject::Graph);
    }

    Sample zero(env);
    zero.document.mainGraph = Guid{};
    const CaseResult second = Check(env, zero);
    REQUIRE(second.error == ErrorCode::InvalidArgument);
    CHECK(second.violations.Has(GraphInvariant::DocumentHeader, GraphSubject::Document));
}

// 동적 핀 계산의 오류는 검사를 끝내지 않고 GI13으로 보고하고, 그 노드는 핀 목록 모름으로 둔다.
TEST(NodeGraph_CaseDynamicPinErrorsReportGI13) {
    TestEnv env;
    REQUIRE(env.ready);
    const uint32 versions[] = {TestDynamicFailVersion, TestDynamicDuplicateVersion,
                               TestDynamicBadTypeVersion, TestDynamicOverflowVersion};
    for (uint32 version : versions) {
        Sample sample(env);
        REQUIRE(SetDynamicVersion(sample, version));
        CHECK(Only(Check(env, sample), GraphInvariant::NodeTypeData, GraphSubject::Node));
    }

    // CollectPins가 돌려주는 오류 자체도 확인한다.
    Sample sample(env);
    PinList pins(env.allocator);
    const auto collect = [&](uint32 version) noexcept {
        sample.ok = SetDynamicVersion(sample, version);
        auto collected = env.registry.CollectPins(sample.document, sample.Main(),
                                                  sample.Node(SampleDynamic), pins);
        return collected ? ErrorCode::None : collected.Error();
    };

    CHECK(collect(TestDynamicFailVersion) == ErrorCode::Unsupported);
    CHECK(collect(TestDynamicDuplicateVersion) == ErrorCode::InvalidArgument);
    CHECK(collect(TestDynamicBadTypeVersion) == ErrorCode::InvalidArgument);
    CHECK(collect(TestDynamicOverflowVersion) == ErrorCode::CapacityExceeded);
    CHECK(pins.Size() == 0);
    CHECK(collect(1) == ErrorCode::None);
    CHECK(pins.Size() == 2);
}

// 속성이 틀린 노드(GI13)는 동적 핀을 계산하지 않는다: 동적 핀 간선이 GI2로 덧붙지 않는다.
TEST(NodeGraph_CaseInvalidPropertiesSkipPinChecks) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample sample(env);
    REQUIRE(FillRecord(sample.Node(SampleDynamic).properties.bytes, TestValueStruct, 2, 0));
    CHECK(Only(Check(env, sample), GraphInvariant::NodeTypeData, GraphSubject::Node));
}

// 재경유는 기본값(GI14 — Hidden 핀)과 고아 핀(GI13)을 둘 수 없다.
TEST(NodeGraph_CaseRerouteRejectsValues) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample defaults(env);
    PinValueEntry value(env.allocator);
    value.pin = env.pinIn;
    REQUIRE(FillRecord(value.value.bytes, TestValueStruct, 1, 1));
    REQUIRE(defaults.Node(SampleReroute).inputDefaults.TryPushBack(Move(value)));
    CHECK(Only(Check(env, defaults), GraphInvariant::InputDefaultPin, GraphSubject::InputDefault));

    Sample retained(env);
    RetainedPin pin(env.allocator);
    pin.key = env.pinOther;
    REQUIRE(FillRecord(pin.value.bytes, TestValueStruct, 1, 1));
    REQUIRE(retained.Node(SampleReroute).retainedPins.TryPushBack(Move(pin)));
    CHECK(Only(Check(env, retained), GraphInvariant::NodeTypeData, GraphSubject::Node));
}

// 의미 바이트는 입력 배열 순서와 무관하다(그래프·노드·입력 기본값·고아 핀·간선을 정규 순서로 쓴다).
TEST(NodeGraph_CaseSemanticIgnoresArrayOrder) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample canonical(env);
    Sample shuffled(env);
    REQUIRE(canonical.ok);
    REQUIRE(shuffled.ok);
    // 입력 기본값·고아 핀을 둘씩 두어 순서를 바꿀 수 있게 한다.
    Sample* const samples[] = {&canonical, &shuffled};
    for (Sample* sample : samples) {
        PinValueEntry entry(env.allocator);
        entry.pin = env.pinB;
        REQUIRE(FillRecord(entry.value.bytes, TestValueStruct, 1, 5));
        REQUIRE(sample->Node(SampleDefaults).inputDefaults.TryPushBack(Move(entry)));
        RetainedPin pin(env.allocator);
        pin.key = env.pinOther;
        REQUIRE(FillRecord(pin.value.bytes, TestValueStruct, 1, 6));
        REQUIRE(sample->Node(SampleAdd).retainedPins.TryPushBack(Move(pin)));
    }

    auto reverse = [](auto& array) noexcept {
        const usize size = array.Size();
        for (usize index = 0; index < size / 2; ++index) {
            auto temporary = Move(array[index]);
            array[index] = Move(array[size - 1 - index]);
            array[size - 1 - index] = Move(temporary);
        }
    };

    reverse(shuffled.Node(SampleDefaults).inputDefaults);
    reverse(shuffled.Node(SampleAdd).retainedPins);
    for (Graph& graph : shuffled.document.graphs) {
        reverse(graph.nodes);
        reverse(graph.edges);
    }

    reverse(shuffled.document.graphs);
    Array<uint8> expected(env.allocator);
    Array<uint8> actual(env.allocator);
    REQUIRE(WriteSemanticBytes(canonical.document, env.registry, env.allocator, expected));
    REQUIRE(WriteSemanticBytes(shuffled.document, env.registry, env.allocator, actual));
    CHECK(SameBytes(expected, actual));
}

// schema가 WriteNode의 쓰기 오류를 무시하고 성공을 돌려줘도 의미 바이트 쓰기는 그 오류로 실패한다.
TEST(NodeGraph_CaseSemanticKeepsIgnoredWriteFailure) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample sample(env);
    Array<uint8> oversized(env.allocator);
    REQUIRE(oversized.TryResize(MaxPropertyRecordBytes + 1));
    env.schema.oversized = oversized.View();
    env.schema.mode = TestNormalize::IgnoreFailure;
    Array<uint8> out(env.allocator);
    auto written = WriteSemanticBytes(sample.document, env.registry, env.allocator, out);
    REQUIRE(!written);
    CHECK(written.Error() == ErrorCode::CapacityExceeded);
    CHECK(out.IsEmpty());
}

} // namespace node_graph_validation_case_tests_file
