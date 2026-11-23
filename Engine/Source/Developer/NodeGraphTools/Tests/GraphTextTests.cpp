#include "GraphToolsTestSupport.h"

#include <Core/Encoding/Base64.h>

#include <stdio.h>

// 그래프 텍스트 codec 시험이다(Docs/Design/NodeGraph 9.4절): 문서 왕복(바이트·텍스트 결정성),
// 레코드 값 텍스트·바이트 형식, semanticOnly, Json5 입력, 조각 왕복과 미해석 노드, 읽기 오류의 진단
// 위치.
using namespace rocks;
namespace graph_text_tests_file {

using namespace rocks::nodegraph_tools_test;

// 표본 문서에 리플렉션 속성(Defaults의 입력 기본값 B·Add의 고아 핀 값을 InstancedPoint로)을 더한다.
bool AddReflectedRecords(ToolsEnv& tools, Sample& sample) noexcept {
    PinValueEntry entry(tools.env.allocator);
    entry.pin = tools.env.pinB;
    auto point = tools.PointRecord(7, "seven");
    auto retained = tools.PointRecord(-1, "gone");
    Array<uint8>& gone = sample.Node(SampleAdd).retainedPins[0].value.bytes;
    gone.Clear();
    return entry.value.bytes.TryAppend(point.View()) &&
           sample.Node(SampleDefaults).inputDefaults.TryPushBack(Move(entry)) &&
           gone.TryAppend(retained.View()) &&
           CanonicalizeGraphDocument(sample.document, &sample.layout, tools.env.names,
                                     tools.env.allocator);
}

// 문서를 쓰고 읽어 다시 쓰면 같은 텍스트이고, 읽은 문서는 원래와 같은 바이트다.
TEST(GraphText_DocumentRoundTrip) {
    ToolsEnv tools;
    REQUIRE(tools.ready);
    Sample sample(tools.env);
    REQUIRE(sample.ok);
    REQUIRE(AddReflectedRecords(tools, sample));
    auto text = tools.WriteDocument(sample.document, &sample.layout);
    REQUIRE(!text.IsEmpty());
    CHECK(Contains(TextOf(text), "\"format\": \"rocks.graph-document\""));
    CHECK(Contains(TextOf(text), "\"label\": \"seven\""));
    CHECK(Contains(TextOf(text), "\"record\": \""));
    CHECK(Contains(TextOf(text), "\"bookmarks\""));

    GraphDocument document(tools.env.allocator);
    GraphDocumentLayout layout(tools.env.allocator);
    GraphTextDiagnostics diagnostics(tools.env.allocator);
    REQUIRE(ReadGraphDocumentJson(tools.context, TextOf(text), document, layout, diagnostics));
    CHECK(diagnostics.items.IsEmpty());
    CHECK(SameBytes(DocumentBytes(tools.env, document, layout),
                    DocumentBytes(tools.env, sample.document, sample.layout)));
    auto again = tools.WriteDocument(document, &layout);
    CHECK(SameBytes(again, text));

    // records 옵션은 모든 값을 바이트로 쓰고, 읽으면 같은 문서다.
    GraphTextWriteOptions records;
    records.records = true;
    auto bytesText = tools.WriteDocument(sample.document, &sample.layout, records);
    CHECK(!Contains(TextOf(bytesText), "\"label\": \"seven\""));
    GraphDocument fromBytes(tools.env.allocator);
    GraphDocumentLayout bytesLayout(tools.env.allocator);
    REQUIRE(ReadGraphDocumentJson(tools.context, TextOf(bytesText), fromBytes, bytesLayout,
                                  diagnostics));
    CHECK(SameBytes(DocumentBytes(tools.env, fromBytes, bytesLayout),
                    DocumentBytes(tools.env, sample.document, sample.layout)));
}

// semanticOnly는 배치·제목·주석·북마크를 빼고, 읽으면 의미 바이트가 같다(배치는 기본값).
TEST(GraphText_SemanticOnly) {
    ToolsEnv tools;
    REQUIRE(tools.ready);
    Sample sample(tools.env);
    REQUIRE(sample.ok);
    GraphTextWriteOptions semantic;
    semantic.semanticOnly = true;
    auto text = tools.WriteDocument(sample.document, &sample.layout, semantic);
    REQUIRE(!text.IsEmpty());
    CHECK(!Contains(TextOf(text), "\"layout\""));
    CHECK(!Contains(TextOf(text), "\"comments\""));
    CHECK(!Contains(TextOf(text), "\"bookmarks\""));
    GraphDocument document(tools.env.allocator);
    GraphDocumentLayout layout(tools.env.allocator);
    GraphTextDiagnostics diagnostics(tools.env.allocator);
    REQUIRE(ReadGraphDocumentJson(tools.context, TextOf(text), document, layout, diagnostics));
    Array<uint8> expected(tools.env.allocator);
    Array<uint8> actual(tools.env.allocator);
    REQUIRE(WriteSemanticBytes(sample.document, tools.env.registry, tools.env.allocator, expected));
    REQUIRE(WriteSemanticBytes(document, tools.env.registry, tools.env.allocator, actual));
    CHECK(SameBytes(expected, actual));
    for (const GraphLayout& graph : layout.graphs) {
        CHECK(graph.comments.IsEmpty() && graph.bookmarks.IsEmpty());
        for (const NodeLayout& node : graph.nodes) {
            CHECK(node.position.x == 0.0f && node.position.y == 0.0f);
        }
    }

    // 배치 없이 쓴 문서와 같은 텍스트다.
    CHECK(SameBytes(text, tools.WriteDocument(sample.document, nullptr, semantic)));
}

// Json5(주석·끝 쉼표·따옴표 없는 키·작은따옴표)를 받는다.
TEST(GraphText_ReadsJson5) {
    ToolsEnv tools;
    REQUIRE(tools.ready);
    const StringView text = R"(// hand edited
{
  format: 'rocks.graph-document',
  formatVersion: 1,
  domain: 'Test',
  documentFormatVersion: 1,
  mainGraph: '00000000-0000-0000-0000-000000000001',
  graphs: [
    {
      id: '00000000-0000-0000-0000-000000000001',
      role: 'Main',
      nodes: [
        { id: '00000000-0000-0000-0000-00000000000c', type: 'Test.Add', typeVersion: 1,
          layout: { x: 1.5, y: -2, }, },
      ],
    },
  ],
}
)";
    GraphDocument document(tools.env.allocator);
    GraphDocumentLayout layout(tools.env.allocator);
    GraphTextDiagnostics diagnostics(tools.env.allocator);
    REQUIRE(ReadGraphDocumentJson(tools.context, text, document, layout, diagnostics));
    REQUIRE(document.graphs.Size() == 1 && document.graphs[0].nodes.Size() == 1);
    CHECK(document.graphs[0].nodes[0].id == SampleAdd);
    CHECK(document.graphs[0].nodes[0].type == tools.env.typeAdd);
    REQUIRE(layout.graphs.Size() == 1 && layout.graphs[0].nodes.Size() == 1);
    CHECK(layout.graphs[0].nodes[0].position.x == 1.5f &&
          layout.graphs[0].nodes[0].position.y == -2.0f);
}

// 조각 왕복: 미해석 type의 노드는 unresolvedType을 남기고(진단 없음), 그런 조각은 쓸 수 없다. 문서
// 읽기에서 미해석은 오류다.
TEST(GraphText_FragmentRoundTripAndUnresolved) {
    ToolsEnv tools;
    REQUIRE(tools.ready);
    GraphFragment fragment(tools.env.allocator);
    fragment.domain = tools.env.domain;
    fragment.sourceAsset = Guid{9, 9};
    fragment.sourceGraph = SampleMain;
    auto node = fragment.nodes.TryEmplaceBack(tools.env.allocator);
    REQUIRE(node);
    node.Value()->node = MakeNode(tools.env, Id(70), tools.env.typeAdd);
    auto point = tools.PointRecord(3, "p");
    REQUIRE(node.Value()->node.properties.bytes.TryAppend(point.View()));
    node.Value()->layout = MakeLayout(tools.env, Id(70), 10, 20);
    REQUIRE(
        fragment.edges.TryPushBack(MakeEdge(Id(70), tools.env.pinResult, Id(70), tools.env.pinA)));
    REQUIRE(fragment.externalEdges.TryPushBack(
        MakeEdge(SampleParamA, tools.env.pinValue, Id(70), tools.env.pinB)));
    CommentBox comment(tools.env.allocator);
    comment.id = Id(80);
    REQUIRE(comment.text.TryAssign("note"));
    REQUIRE(fragment.comments.TryPushBack(Move(comment)));

    StringBuilder<> text(tools.env.allocator);
    REQUIRE(WriteGraphFragmentJson(tools.context, fragment, {}, text));
    GraphFragment back(tools.env.allocator);
    GraphTextDiagnostics diagnostics(tools.env.allocator);
    REQUIRE(ReadGraphFragmentJson(tools.context, text.View(), back, diagnostics));
    CHECK(back.domain == fragment.domain && back.sourceAsset == fragment.sourceAsset &&
          back.sourceGraph == SampleMain);
    REQUIRE(back.nodes.Size() == 1 && back.edges.Size() == 1 && back.externalEdges.Size() == 1 &&
            back.comments.Size() == 1);
    CHECK(back.nodes[0].unresolvedType.IsEmpty());
    CHECK(SameBytes(back.nodes[0].node.properties.bytes, point));
    CHECK(back.nodes[0].layout.position.x == 10.0f);
    CHECK(back.comments[0].text.View() == "note");
    StringBuilder<> again(tools.env.allocator);
    REQUIRE(WriteGraphFragmentJson(tools.context, back, {}, again));
    CHECK(again.View() == text.View());

    // 등록되지 않은 type: 조각은 미해석 노드, 문서는 오류.
    const StringView unresolved = R"({"format": "rocks.graph-fragment", "formatVersion": 1,
  "domain": "Test", "sourceGraph": "00000000-0000-0000-0000-000000000001",
  "nodes": [{"id": "00000000-0000-0000-0000-000000000046", "type": "Test.Add", "typeVersion": 1,
             "properties": {"type": "Nope.Missing", "value": {"x": 1}}}]})";
    REQUIRE(ReadGraphFragmentJson(tools.context, unresolved, back, diagnostics));
    REQUIRE(back.nodes.Size() == 1);
    CHECK(back.nodes[0].unresolvedType.View() == "Nope.Missing");
    CHECK(back.nodes[0].node.properties.bytes.IsEmpty());
    CHECK(diagnostics.items.IsEmpty());
    StringBuilder<> rejected(tools.env.allocator);
    auto written = WriteGraphFragmentJson(tools.context, back, {}, rejected);
    CHECK(!written && written.Error() == ErrorCode::InvalidArgument);
}

struct ReadCase {
    const char* text;
    GraphTextCode code;
    uint32 line;
};

// 읽기 오류마다 진단 하나와 그 줄이다(문서 형식).
TEST(GraphText_ReadErrorsReportPositions) {
    ToolsEnv tools;
    REQUIRE(tools.ready);
    // 둘째 줄부터 그래프 하나의 노드 하나다. 오류 줄 번호는 1부터.
    const ReadCase cases[] = {
        {"{\"format\": \"rocks.graph-document\",\n\"formatVersion\": 1 1}",
         GraphTextCode::JsonSyntax, 2},
        {"{\"format\": \"rocks.graph-fragment\", \"formatVersion\": 1, \"domain\": \"Test\",\n"
         "\"documentFormatVersion\": 1, \"mainGraph\": \"00000000-0000-0000-0000-000000000001\"}",
         GraphTextCode::UnsupportedFormat, 1},
        {"{\"format\": \"rocks.graph-document\",\n\"formatVersion\": 2, \"domain\": \"Test\",\n"
         "\"documentFormatVersion\": 1, \"mainGraph\": \"00000000-0000-0000-0000-000000000001\"}",
         GraphTextCode::UnsupportedFormat, 2},
        {"{\"format\": \"rocks.graph-document\", \"formatVersion\": 1, \"domain\": \"Test\",\n"
         "\"documentFormatVersion\": 1, \"mainGraph\": \"00000000-0000-0000-0000-000000000001\",\n"
         "\"extra\": 1}",
         GraphTextCode::UnknownKey, 3},
        {"{\"format\": \"rocks.graph-document\", \"formatVersion\": 1, \"domain\": \"Test\",\n"
         "\"documentFormatVersion\": 1, \"mainGraph\": \"not-a-guid\"}",
         GraphTextCode::InvalidValue, 2},
        {"{\"format\": \"rocks.graph-document\", \"formatVersion\": 1, \"domain\": \"Test\",\n"
         "\"documentFormatVersion\": 1}",
         GraphTextCode::MissingKey, 1},
        {"{\"format\": \"rocks.graph-document\", \"formatVersion\": 1, \"domain\": \"Test\",\n"
         "\"documentFormatVersion\": 1, \"mainGraph\": \"00000000-0000-0000-0000-000000000001\",\n"
         "\"graphs\": [{\"id\": \"00000000-0000-0000-0000-000000000001\", \"role\": \"Bad\"}]}",
         GraphTextCode::InvalidValue, 3},
        {"{\"format\": \"rocks.graph-document\", \"formatVersion\": 1, \"domain\": \"Test\",\n"
         "\"documentFormatVersion\": 1, \"mainGraph\": \"00000000-0000-0000-0000-000000000001\",\n"
         "\"graphs\": [{\"id\": \"00000000-0000-0000-0000-000000000001\", \"role\": \"Main\",\n"
         "\"nodes\": [{\"id\": \"00000000-0000-0000-0000-00000000000c\", \"type\": \"Test.Add\",\n"
         "\"typeVersion\": 1, \"properties\": {\"record\": \"!!!!\"}}]}]}",
         GraphTextCode::InvalidValue, 5},
        {"{\"format\": \"rocks.graph-document\", \"formatVersion\": 1, \"domain\": \"Test\",\n"
         "\"documentFormatVersion\": 1, \"mainGraph\": \"00000000-0000-0000-0000-000000000001\",\n"
         "\"graphs\": [{\"id\": \"00000000-0000-0000-0000-000000000001\", \"role\": \"Main\",\n"
         "\"nodes\": [{\"id\": \"00000000-0000-0000-0000-00000000000c\", \"type\": \"Test.Add\",\n"
         "\"typeVersion\": 1, \"properties\": {\"type\": \"Nope\", \"value\": {}}}]}]}",
         GraphTextCode::UnknownRecordType, 5},
        {"{\"format\": \"rocks.graph-document\", \"formatVersion\": 1, \"domain\": \"Test\",\n"
         "\"documentFormatVersion\": 1, \"mainGraph\": \"00000000-0000-0000-0000-000000000001\",\n"
         "\"graphs\": [{\"id\": \"00000000-0000-0000-0000-000000000001\", \"role\": \"Main\",\n"
         "\"nodes\": [{\"id\": \"00000000-0000-0000-0000-00000000000c\", \"type\": \"Test.Add\",\n"
         "\"typeVersion\": 4294967296}]}]}",
         GraphTextCode::InvalidValue, 5},
        {"{\"format\": \"rocks.graph-document\", \"formatVersion\": 1, \"domain\": \"Test\",\n"
         "\"documentFormatVersion\": 1, \"mainGraph\": \"00000000-0000-0000-0000-000000000001\",\n"
         "\"graphs\": [{\"id\": \"00000000-0000-0000-0000-000000000001\", \"role\": \"Main\",\n"
         "\"nodes\": [{\"id\": \"00000000-0000-0000-0000-00000000000c\", \"type\": \"Test.Add\",\n"
         "\"typeVersion\": 1, \"layout\": {\"x\": 1e40, \"y\": 0}}]}]}",
         GraphTextCode::InvalidValue, 5},
        {"{\"format\": \"rocks.graph-document\", \"formatVersion\": 1, \"domain\": \"Test\",\n"
         "\"documentFormatVersion\": 1, \"mainGraph\": \"00000000-0000-0000-0000-000000000001\",\n"
         "\"graphs\": [{\"id\": \"00000000-0000-0000-0000-000000000001\", \"role\": \"Main\",\n"
         "\"comments\": [{\"id\": \"00000000-0000-0000-0000-000000000028\", \"x\": 0, \"y\": 0,\n"
         "\"width\": 1, \"height\": 1, \"text\": \"\", \"color\": [1, 2], \"fontSize\": 1}]}]}",
         GraphTextCode::InvalidValue, 5},
        {"{\"format\": \"rocks.graph-document\", \"formatVersion\": 1, \"domain\": \"Test\",\n"
         "\"documentFormatVersion\": 1, \"mainGraph\": \"00000000-0000-0000-0000-000000000001\",\n"
         "\"graphs\": [{\"id\": \"00000000-0000-0000-0000-000000000001\", \"role\": \"Main\",\n"
         "\"edges\": [{\"from\": [\"00000000-0000-0000-0000-00000000000c\"], \"to\": []}]}]}",
         GraphTextCode::InvalidValue, 4},
        {"{\"format\": \"rocks.graph-document\", \"formatVersion\": 1, \"domain\": \"Test\",\n"
         "\"documentFormatVersion\": 1, \"mainGraph\": \"00000000-0000-0000-0000-000000000001\",\n"
         "\"graphs\": [{\"id\": \"00000000-0000-0000-0000-000000000001\", \"role\": \"Main\",\n"
         "\"nodes\": [{\"id\": \"00000000-0000-0000-0000-00000000000c\", \"type\": \"Test.Add\",\n"
         "\"typeVersion\": 1, \"retainedPins\": [{\"key\": \"G\", \"direction\": \"Up\"}]}]}]}",
         GraphTextCode::InvalidValue, 5},
        {"{\"format\": \"rocks.graph-document\", \"formatVersion\": 1, \"domain\": \"Test\",\n"
         "\"documentFormatVersion\": 1, \"mainGraph\": \"00000000-0000-0000-0000-000000000001\",\n"
         "\"graphs\": [{\"id\": \"00000000-0000-0000-0000-000000000001\", \"role\": \"Main\",\n"
         "\"nodes\": [{\"id\": \"00000000-0000-0000-0000-00000000000c\", \"type\": \"Test.Add\",\n"
         "\"typeVersion\": 1, \"properties\": {\"type\": \"InstancedPoint\",\n"
         "\"value\": {\"nope\": 1}}}]}]}",
         GraphTextCode::RecordText, 6},
    };
    for (const ReadCase& item : cases) {
        GraphDocument document(tools.env.allocator);
        GraphDocumentLayout layout(tools.env.allocator);
        GraphTextDiagnostics diagnostics(tools.env.allocator);
        auto read = ReadGraphDocumentJson(tools.context, item.text, document, layout, diagnostics);
        const bool matched =
            !read && read.Error() == ErrorCode::InvalidArgument && !diagnostics.items.IsEmpty() &&
            diagnostics.items[0].code == item.code && diagnostics.items[0].line == item.line;
        if (!matched) {
            fprintf(stderr, "GraphText read case failed: %s\n", item.text);
            if (!diagnostics.items.IsEmpty()) {
                fprintf(stderr, "  got %.*s at line %u\n",
                        static_cast<int>(GraphTextCodeName(diagnostics.items[0].code).ByteCount()),
                        GraphTextCodeName(diagnostics.items[0].code).Data(),
                        diagnostics.items[0].line);
            }
        }

        CHECK(matched);
    }
}

// 표본에 없는 필드(그래프 제목·domainRole·배치 크기·접힘·고급 핀·말풍선, -0.0·FLT_MAX)도 왕복한다.
// ReadOnly 필드가 기본값이 아닌 속성은 텍스트 왕복이 다르므로 바이트 형식으로 쓰인다.
TEST(GraphText_RoundTripsEveryLayoutField) {
    ToolsEnv tools;
    REQUIRE(tools.ready);
    Sample sample(tools.env);
    REQUIRE(sample.ok);
    sample.document.graphs[1].role = GraphRole::DomainDefined;
    sample.document.graphs[1].domainRole = 3;
    REQUIRE(sample.layout.graphs[0].title.TryAssign("Main graph"));
    NodeLayout& node = sample.layout.graphs[0].nodes[0];
    node.size = Vec2(-0.0f, NumericLimits<float>::Max());
    node.collapsed = true;
    node.showAdvanced = true;
    REQUIRE(node.bubble.TryAssign("hint"));
    node.bubbleVisible = true;
    node.position = Vec2(NumericLimits<float>::Lowest(), 0.1f);
    InstancedLocked locked;
    locked.fixed = 5;
    locked.free = 2;
    auto lockedBytes = tools.Record(locked);
    PinValueEntry entry(tools.env.allocator);
    entry.pin = tools.env.pinB;
    REQUIRE(entry.value.bytes.TryAppend(lockedBytes.View()));
    REQUIRE(sample.Node(SampleDefaults).inputDefaults.TryPushBack(Move(entry)));
    REQUIRE(CanonicalizeGraphDocument(sample.document, &sample.layout, tools.env.names,
                                      tools.env.allocator));

    auto text = tools.WriteDocument(sample.document, &sample.layout);
    REQUIRE(!text.IsEmpty());
    CHECK(Contains(TextOf(text), "\"title\": \"Main graph\""));
    CHECK(Contains(TextOf(text), "\"domainRole\": 3"));
    CHECK(Contains(TextOf(text), "\"width\": -0.0"));
    CHECK(!Contains(TextOf(text), "\"fixed\""));
    GraphDocument document(tools.env.allocator);
    GraphDocumentLayout layout(tools.env.allocator);
    GraphTextDiagnostics diagnostics(tools.env.allocator);
    REQUIRE(ReadGraphDocumentJson(tools.context, TextOf(text), document, layout, diagnostics));
    CHECK(SameBytes(DocumentBytes(tools.env, document, layout),
                    DocumentBytes(tools.env, sample.document, sample.layout)));
    CHECK(SameBytes(tools.WriteDocument(document, &layout), text));
}

// 크기 경계·대소문자만 다른 핀·빈 type·조각 키 누락·JSON 중복 키.
TEST(GraphText_ReadRejectsEdgeInputs) {
    ToolsEnv tools;
    REQUIRE(tools.ready);
    const auto readDocument = [&tools](StringView text, GraphTextCode code,
                                       StringView message) noexcept {
        GraphDocument document(tools.env.allocator);
        GraphDocumentLayout layout(tools.env.allocator);
        GraphTextDiagnostics diagnostics(tools.env.allocator);
        auto read = ReadGraphDocumentJson(tools.context, text, document, layout, diagnostics);
        return !read && !diagnostics.items.IsEmpty() && diagnostics.items[0].code == code &&
               (message.IsEmpty() || diagnostics.items[0].message.View() == message);
    };

    // 한도보다 1바이트 큰 Record(base64)는 풀어 본 크기로 거부한다.
    Array<uint8> large(tools.env.allocator);
    REQUIRE(large.TryResize(MaxPropertyRecordBytes + 1));
    auto encodedSize = Base64EncodedSize(large.Size());
    REQUIRE(encodedSize);
    Array<char> encoded(tools.env.allocator);
    REQUIRE(encoded.TryResize(encodedSize.Value()));
    REQUIRE(TryBase64Encode(large.View(), encoded.View()));
    StringBuilder<> big(tools.env.allocator);
    big << "{\"format\": \"rocks.graph-document\", \"formatVersion\": 1, \"domain\": \"Test\", "
           "\"documentFormatVersion\": 1, \"mainGraph\": \"00000000-0000-0000-0000-000000000001\", "
           "\"graphs\": [{\"id\": \"00000000-0000-0000-0000-000000000001\", \"role\": \"Main\", "
           "\"nodes\": [{\"id\": \"00000000-0000-0000-0000-00000000000c\", \"type\": \"Test.Add\", "
           "\"typeVersion\": 1, \"properties\": {\"record\": \""
        << StringView(encoded.Data(), encoded.Size()) << "\"}}]}]}";
    REQUIRE(big.Status());
    CHECK(readDocument(big.View(), GraphTextCode::InvalidValue, "record is too large"));

    const StringView prefix =
        "{\"format\": \"rocks.graph-document\", \"formatVersion\": 1, \"domain\": \"Test\", "
        "\"documentFormatVersion\": 1, \"mainGraph\": \"00000000-0000-0000-0000-000000000001\", "
        "\"graphs\": [{\"id\": \"00000000-0000-0000-0000-000000000001\", \"role\": \"Main\", "
        "\"nodes\": [{\"id\": \"00000000-0000-0000-0000-00000000000c\", \"type\": \"Test.Add\", "
        "\"typeVersion\": 1, ";
    StringBuilder<> repeated(tools.env.allocator);
    repeated << prefix
             << "\"inputDefaults\": {\"B\": {\"type\": \"InstancedPoint\", \"value\": {}}, "
                "\"b\": {\"type\": \"InstancedPoint\", \"value\": {}}}}]}]}";
    REQUIRE(repeated.Status());
    CHECK(readDocument(repeated.View(), GraphTextCode::InvalidValue,
                       "input default pin is repeated"));
    StringBuilder<> retained(tools.env.allocator);
    retained << prefix
             << "\"retainedPins\": [{\"key\": \"G\", \"direction\": \"Input\"}, "
                "{\"key\": \"g\", \"direction\": \"Input\"}]}]}]}";
    REQUIRE(retained.Status());
    CHECK(
        readDocument(retained.View(), GraphTextCode::InvalidValue, "retained pin key is repeated"));
    StringBuilder<> emptyType(tools.env.allocator);
    emptyType << prefix << "\"properties\": {\"type\": \"\", \"value\": {}}}]}]}";
    REQUIRE(emptyType.Status());
    CHECK(readDocument(emptyType.View(), GraphTextCode::InvalidValue, ""));
    CHECK(readDocument("{\"format\": \"rocks.graph-document\", \"format\": \"x\"}",
                       GraphTextCode::JsonSyntax, ""));

    GraphFragment fragment(tools.env.allocator);
    GraphTextDiagnostics diagnostics(tools.env.allocator);
    auto missing = ReadGraphFragmentJson(
        tools.context,
        "{\"format\": \"rocks.graph-fragment\", \"formatVersion\": 1, \"domain\": \"Test\"}",
        fragment, diagnostics);
    CHECK(!missing && !diagnostics.items.IsEmpty() &&
          diagnostics.items[0].code == GraphTextCode::MissingKey);
}

} // namespace graph_text_tests_file
