#include "NodeGraphTestSupport.h"

#include <stdio.h>

// 의미 바이트·semantic hash 시험이다(Docs/Design/NodeGraph.ko.md 8.4절, 13절 GT9 바이트 부분):
// 결정성, 재경유 접기 (사슬·원천 없는 재경유·순번 출력 핀을 거친 순번), 배치·표시 대소문자와
// 무관함, 의미 필드마다 바이트가 바뀜, 도메인 정규화의 적용과 계약 위반, 순환·입력 둘인 재경유의
// 거부, 형식 머리와 golden hash.
using namespace rocks;
namespace node_graph_semantic_tests_file {

using namespace rocks::nodegraph;
using namespace rocks::nodegraph_test;

Array<uint8> Bytes(TestEnv& env, const GraphDocument& document) noexcept {
    Array<uint8> bytes(env.allocator);
    if (!WriteSemanticBytes(document, env.registry, env.allocator, bytes)) {
        bytes.Clear();
    }

    return bytes;
}

bool Same(TestEnv& env, const GraphDocument& left, const GraphDocument& right) noexcept {
    const Array<uint8> leftBytes = Bytes(env, left);
    const Array<uint8> rightBytes = Bytes(env, right);
    return !leftBytes.IsEmpty() && SameBytes(leftBytes, rightBytes);
}

bool Canonical(TestEnv& env, Sample& sample) noexcept {
    return static_cast<bool>(
        CanonicalizeGraphDocument(sample.document, &sample.layout, env.names, env.allocator));
}

// 주 그래프에서 (to, toPin)으로 들어오는 간선을 지운다.
bool RemoveEdgeInto(Sample& sample, Guid to, Name toPin) noexcept {
    Array<GraphEdge>& edges = sample.Main().edges;
    for (usize index = 0; index < edges.Size(); ++index) {
        if (edges[index].toNode == to && edges[index].toPin == toPin) {
            return static_cast<bool>(edges.TryRemoveAt(index));
        }
    }

    return false;
}

// 주 그래프에서 id인 노드를 지운다(배치는 건드리지 않는다 — 의미 바이트는 배치를 받지 않는다).
bool RemoveNode(Sample& sample, Guid id) noexcept {
    Array<GraphNode>& nodes = sample.Main().nodes;
    for (usize index = 0; index < nodes.Size(); ++index) {
        if (nodes[index].id == id) {
            return static_cast<bool>(nodes.TryRemoveAt(index));
        }
    }

    return false;
}

TEST(NodeGraph_SemanticDeterministicAndHash) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample first(env);
    Sample second(env);
    REQUIRE(first.ok);
    REQUIRE(second.ok);
    const Array<uint8> bytes = Bytes(env, first.document);
    REQUIRE(!bytes.IsEmpty());
    CHECK(SameBytes(bytes, Bytes(env, first.document)));
    CHECK(SameBytes(bytes, Bytes(env, second.document)));
    auto hash = ComputeSemanticHash(first.document, env.registry, env.allocator);
    REQUIRE(hash);
    CHECK(hash.Value() == Sha256::Hash(bytes.View()));

    // 머리: "RNGS", 형식 번호 1(little-endian u32).
    REQUIRE(bytes.Size() > 8);
    CHECK(bytes[0] == 'R' && bytes[1] == 'N' && bytes[2] == 'G' && bytes[3] == 'S');
    CHECK(bytes[4] == SemanticBytesFormatVersion && bytes[5] == 0 && bytes[6] == 0 &&
          bytes[7] == 0);

    // out은 먼저 비운다.
    Array<uint8> reused(env.allocator);
    REQUIRE(reused.TryPushBack(0xAB));
    REQUIRE(WriteSemanticBytes(first.document, env.registry, env.allocator, reused));
    CHECK(SameBytes(bytes, reused));
}

// 표본 문서(NodeGraphTestSupport.h Sample)의 semantic hash golden이다. 형식(8.4절)이나 표본을
// 바꾸면 SemanticBytesFormatVersion을 올리고(형식일 때) 이 값을 함께 고친다.
constexpr const char* NodeGraphSampleGoldenHash =
    "4e54499baee462a1fb2e6574f96ffd3013c27776594c63d412526ed6d3dcdbf6";

TEST(NodeGraph_SemanticGoldenHash) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample sample(env);
    REQUIRE(sample.ok);
    auto hash = ComputeSemanticHash(sample.document, env.registry, env.allocator);
    REQUIRE(hash);
    char hex[Sha256Digest::HexBytes + 1] = {};
    hash.Value().ToHex(hex);
    const bool matches = StringView(hex) == StringView(NodeGraphSampleGoldenHash);
    if (!matches) {
        fprintf(stderr, "NodeGraph sample semantic hash: %s\n", hex);
    }

    CHECK(matches);
}

// 배치(위치·주석·북마크)와 그래프 표시 이름은 의미 바이트 입력이 아니다. 문서 형식 버전도 쓰지
// 않는다.
TEST(NodeGraph_SemanticIgnoresLayoutAndFormatVersion) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample base(env);
    Sample other(env);
    for (GraphLayout& graph : other.layout.graphs) {
        for (NodeLayout& node : graph.nodes) {
            node.position = Vec2(1.0f, 2.0f);
            node.collapsed = true;
        }

        graph.comments.Clear();
    }

    CHECK(Same(env, base.document, other.document));
    other.document.documentFormatVersion = GraphDocumentFormatVersion + 1;
    CHECK(Same(env, base.document, other.document));
}

// 재경유를 접는다: 재경유를 거친 연결은 직접 연결과 같은 바이트다(재경유 노드 자체도 쓰지 않는다).
TEST(NodeGraph_SemanticFoldsReroutes) {
    TestEnv env;
    REQUIRE(env.ready);
    // 표본의 ParamB → Reroute → Add.B를 ParamB → Add.B로 바꾼 문서.
    Sample direct(env);
    REQUIRE(RemoveEdgeInto(direct, SampleReroute, env.pinIn));
    REQUIRE(RemoveEdgeInto(direct, SampleAdd, env.pinB));
    REQUIRE(RemoveNode(direct, SampleReroute));
    REQUIRE(
        direct.Main().edges.TryPushBack(MakeEdge(SampleParamB, env.pinValue, SampleAdd, env.pinB)));
    REQUIRE(Canonical(env, direct));
    Sample folded(env);
    CHECK(Same(env, direct.document, folded.document));

    // 재경유 사슬 셋: ParamB → R13 → R60 → R61 → Add.B.
    Sample chain(env);
    REQUIRE(RemoveEdgeInto(chain, SampleAdd, env.pinB));
    REQUIRE(chain.Main().nodes.TryPushBack(MakeNode(env, Id(60), env.reroute)));
    REQUIRE(chain.Main().nodes.TryPushBack(MakeNode(env, Id(61), env.reroute)));
    REQUIRE(chain.Main().edges.TryPushBack(MakeEdge(SampleReroute, env.pinOut, Id(60), env.pinIn)));
    REQUIRE(chain.Main().edges.TryPushBack(MakeEdge(Id(60), env.pinOut, Id(61), env.pinIn)));
    REQUIRE(chain.Main().edges.TryPushBack(MakeEdge(Id(61), env.pinOut, SampleAdd, env.pinB)));
    REQUIRE(Canonical(env, chain));
    CHECK(Same(env, direct.document, chain.document));

    // 재경유 하나에서 여러 대상으로 퍼진다: R13.Out → Add.B와 Multi.Items(직접 연결 둘과 같다).
    Sample fan(env);
    Sample fanDirect(env);
    REQUIRE(fan.Main().nodes.TryPushBack(MakeNode(env, Id(50), env.typeMulti)));
    REQUIRE(
        fan.Main().edges.TryPushBack(MakeEdge(SampleReroute, env.pinOut, Id(50), env.pinItems)));
    REQUIRE(Canonical(env, fan));
    REQUIRE(fanDirect.Main().nodes.TryPushBack(MakeNode(env, Id(50), env.typeMulti)));
    REQUIRE(fanDirect.Main().edges.TryPushBack(
        MakeEdge(SampleParamB, env.pinValue, Id(50), env.pinItems)));
    REQUIRE(Canonical(env, fanDirect));
    CHECK(Same(env, fan.document, fanDirect.document));
    CHECK(!Same(env, fan.document, folded.document));
}

// 원천이 없는 재경유에서 나온 간선은 쓰지 않는다: 그 간선을 지운 문서와 같은 바이트다.
TEST(NodeGraph_SemanticDropsDanglingReroute) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample dangling(env);
    REQUIRE(RemoveEdgeInto(dangling, SampleReroute, env.pinIn));
    Sample removed(env);
    REQUIRE(RemoveEdgeInto(removed, SampleReroute, env.pinIn));
    REQUIRE(RemoveEdgeInto(removed, SampleAdd, env.pinB));
    CHECK(Same(env, dangling.document, removed.document));
}

// ordered 출력 핀에서 재경유를 거친 간선은 사슬 첫 간선(원천에서 나온 간선)의 순번을 갖는다.
TEST(NodeGraph_SemanticRerouteKeepsSourceOrder) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample direct(env);
    Sample routed(env);
    // Sequence.Then(순번 1) → StepB.In을 Sequence.Then(순번 1) → R60 → StepB.In으로.
    REQUIRE(RemoveEdgeInto(routed, SampleStepB, env.pinIn));
    REQUIRE(routed.Main().nodes.TryPushBack(MakeNode(env, Id(60), env.reroute)));
    REQUIRE(routed.Main().edges.TryPushBack(
        MakeEdge(SampleSequence, env.pinThen, Id(60), env.pinIn, 1)));
    REQUIRE(routed.Main().edges.TryPushBack(MakeEdge(Id(60), env.pinOut, SampleStepB, env.pinIn)));
    REQUIRE(Canonical(env, routed));
    GraphViolationList violations(env.allocator);
    CHECK(Validate(env, routed.document, nullptr, violations));
    CHECK(Same(env, direct.document, routed.document));

    // 순번이 다르면 다른 바이트다.
    for (GraphEdge& edge : routed.Main().edges) {
        if (edge.toNode == Id(60)) {
            edge.order = 0;
        }
    }

    CHECK(!Same(env, direct.document, routed.document));
}

// 표시 대소문자만 다른 PinKey·종류 이름은 같은 Name이고 같은 바이트다.
TEST(NodeGraph_SemanticIgnoresDisplayCase) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample base(env);
    Sample cased(env);
    const Name lowerB = env.Intern("b");
    const Name upperType = env.Intern("TEST.ADD");
    for (GraphEdge& edge : cased.Main().edges) {
        if (edge.toPin == env.pinB) {
            edge.toPin = lowerB;
        }
    }

    cased.Node(SampleAdd).type = upperType;
    REQUIRE(Canonical(env, cased));
    CHECK(Same(env, base.document, cased.document));
}

// 의미 필드 하나만 바꿔도 바이트가 바뀐다.
TEST(NodeGraph_SemanticChangesWithEveryField) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample base(env);
    REQUIRE(base.ok);
    auto differs = [&env, &base](auto&& edit) noexcept {
        Sample changed(env);
        edit(changed);
        return changed.ok && !Same(env, base.document, changed.document);
    };

    CHECK(differs([](Sample& sample) noexcept {
        sample.ok = FillRecord(sample.Node(SampleParamA).properties.bytes, TestParamStruct,
                               TestParamVersion, 6);
    }));
    CHECK(
        differs([](Sample& sample) noexcept { sample.Node(SampleParamA).typeSchemaVersion = 1; }));
    CHECK(differs([](Sample& sample) noexcept {
        sample.Node(SampleToggle).enableState = NodeEnableState::Enabled;
    }));
    CHECK(differs([](Sample& sample) noexcept {
        sample.ok = FillRecord(sample.Node(SampleDefaults).inputDefaults[0].value.bytes,
                               TestValueStruct, 1, 43);
    }));
    CHECK(differs(
        [](Sample& sample) noexcept { sample.Node(SampleAdd).retainedPins[0].lastType = 0; }));
    CHECK(differs([](Sample& sample) noexcept {
        sample.Node(SampleAdd).retainedPins[0].direction = PinDirection::Output;
    }));
    CHECK(differs([](Sample& sample) noexcept {
        sample.document.graphs[1].role = GraphRole::DomainDefined;
    }));
    CHECK(differs([](Sample& sample) noexcept {
        sample.document.graphs[1].role = GraphRole::DomainDefined;
        sample.document.graphs[1].domainRole = 4;
    }));
    CHECK(differs([](Sample& sample) noexcept { sample.document.mainGraph = SampleCollapsed; }));
    CHECK(
        differs([&env](Sample& sample) noexcept { sample.document.domain = env.Intern("Other"); }));
    CHECK(differs([](Sample& sample) noexcept {
        for (GraphEdge& edge : sample.Main().edges) {
            if (edge.toNode == SampleStepB) {
                edge.order = 0;
            }
        }
    }));
    CHECK(differs([&env](Sample& sample) noexcept {
        sample.ok = sample.Main().edges.TryPushBack(
                        MakeEdge(SampleParamB, env.pinValue, SampleDynamic, env.pinItem0)) &&
                    Canonical(env, sample);
    }));
}

// 도메인 정규화: schema가 거른 속성으로 쓴다. 등록된 schema가 없는 도메인은 기본 정규화다.
TEST(NodeGraph_SemanticDomainNormalization) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample first(env);
    Sample second(env);
    REQUIRE(FillRecord(second.Node(SampleParamA).properties.bytes, TestParamStruct,
                       TestParamVersion, 99));
    CHECK(!Same(env, first.document, second.document));
    env.schema.mode = TestNormalize::StripProperties;
    CHECK(Same(env, first.document, second.document));
    env.schema.mode = TestNormalize::Default;

    // schema 없는 도메인: 기본 정규화(노드는 모두 알 수 없는 종류로 쓴다 — 재경유는 공통이라
    // 접힌다).
    Sample unscoped(env);
    unscoped.document.domain = env.Intern("Unscoped");
    CHECK(!Bytes(env, unscoped.document).IsEmpty());
}

// schema 계약 위반(WriteNode를 부르지 않음·두 번 부름)과 다른 노드를 쓰는 것은 InvalidArgument다.
TEST(NodeGraph_SemanticNormalizeContract) {
    TestEnv env;
    REQUIRE(env.ready);
    Sample sample(env);
    Array<uint8> out(env.allocator);
    REQUIRE(out.TryPushBack(1));
    env.schema.mode = TestNormalize::Skip;
    auto skipped = WriteSemanticBytes(sample.document, env.registry, env.allocator, out);
    REQUIRE(!skipped);
    CHECK(skipped.Error() == ErrorCode::InvalidArgument);
    CHECK(out.IsEmpty());
    env.schema.mode = TestNormalize::Twice;
    auto twice = ComputeSemanticHash(sample.document, env.registry, env.allocator);
    REQUIRE(!twice);
    CHECK(twice.Error() == ErrorCode::InvalidArgument);
}

TEST(NodeGraph_SemanticRejectsBrokenReroutes) {
    TestEnv env;
    REQUIRE(env.ready);
    // 순환: R60 ↔ R61, R61.Out → Add.A(대신).
    Sample cycle(env);
    REQUIRE(RemoveEdgeInto(cycle, SampleAdd, env.pinA));
    REQUIRE(cycle.Main().nodes.TryPushBack(MakeNode(env, Id(60), env.reroute)));
    REQUIRE(cycle.Main().nodes.TryPushBack(MakeNode(env, Id(61), env.reroute)));
    REQUIRE(cycle.Main().edges.TryPushBack(MakeEdge(Id(60), env.pinOut, Id(61), env.pinIn)));
    REQUIRE(cycle.Main().edges.TryPushBack(MakeEdge(Id(61), env.pinOut, Id(60), env.pinIn)));
    REQUIRE(cycle.Main().edges.TryPushBack(MakeEdge(Id(61), env.pinOut, SampleAdd, env.pinA)));
    REQUIRE(Canonical(env, cycle));
    Array<uint8> out(env.allocator);
    auto cyclic = WriteSemanticBytes(cycle.document, env.registry, env.allocator, out);
    REQUIRE(!cyclic);
    CHECK(cyclic.Error() == ErrorCode::CycleDetected);

    // 입력 간선이 둘인 재경유.
    Sample doubled(env);
    REQUIRE(doubled.Main().edges.TryPushBack(
        MakeEdge(SampleParamA, env.pinValue, SampleReroute, env.pinIn)));
    REQUIRE(Canonical(env, doubled));
    auto twoInputs = WriteSemanticBytes(doubled.document, env.registry, env.allocator, out);
    REQUIRE(!twoInputs);
    CHECK(twoInputs.Error() == ErrorCode::InvalidArgument);
}

} // namespace node_graph_semantic_tests_file
