#pragma once

#include <Core/Text/Json.h>
#include <Core/Text/StringBuilder.h>
#include <NodeGraph/GraphDocument.h>
#include <Object/ObjectRegistry.h>

// 노드 그래프 텍스트 codec이다(Docs/Design/NodeGraph.ko.md 9.2·9.4절). 문서
// 전체(`rocks.graph-document` — graph-dump)와 조각(`rocks.graph-fragment` — 복사·붙여넣기)을 정규
// JSON으로 쓰고, Json5 문법도 받아 읽는다. 노드 속성·입력 기본값·고아 핀 값(PropertyRecord)은
// 리플렉션 텍스트(`{"type", "value"}`)나 base64 Record 바이트(`{"record"}`)다.
//
// 사용 예(dump):
//   GraphTextContext context(allocator, names, objects, objects.OwnerAccess());
//   StringBuilder<> text(allocator);
//   auto written = WriteGraphDocumentJson(context, document, &layout, {}, text);
namespace rocks::nodegraph {

// 레코드 값 텍스트화의 문맥이다: names는 문서 Name 토큰의 pool, objects는 속성 struct가 등록된
// 리플렉션 Registry(그 TypeRegistry로 struct를 열고 그 ObjectAllocator로 임시 값을 만든다),
// access는 그 Registry의 접근 문맥(값으로 보관한다)이다. allocator·names·objects는 이 문맥보다 오래
// 살아야 하고 objects의 소유 스레드에서 쓴다.
struct GraphTextContext {
    GraphTextContext(Allocator& allocatorValue, NamePool& namesValue, ObjectRegistry& objectsValue,
                     ObjectAccessContext accessValue) noexcept
        : allocator(allocatorValue), names(namesValue), objects(objectsValue), access(accessValue) {
    }

    Allocator& allocator;
    NamePool& names;
    ObjectRegistry& objects;
    ObjectAccessContext access;
};

// 쓰기 옵션이다. semanticOnly는 배치·그래프 제목·주석·북마크를 뺀다(문서만). records는 모든 레코드
// 값을 base64 Record 바이트로 쓴다(바이트 일치 출력).
struct GraphTextWriteOptions {
    bool semanticOnly = false;
    bool records = false;
};

// 텍스트 읽기 진단의 종류다.
enum class GraphTextCode : uint8 {
    // JSON 문법 오류(JsonError).
    JsonSyntax,
    // format·formatVersion이 다르다.
    UnsupportedFormat,
    // 모르는 키(오타 방지 — 모든 객체에서 거부한다).
    UnknownKey,
    // 필요한 키가 없다.
    MissingKey,
    // 값의 JSON 종류·형식·범위가 틀렸다(Guid·enum 이름·base64·Record header 등).
    InvalidValue,
    // 레코드 값의 type이 리플렉션에 등록되어 있지 않다(조각은 노드를 미해석으로 둔다).
    UnknownRecordType,
    // 리플렉션 텍스트 대입이 실패했다(메시지에 ReflectionText 진단 이름과 경로).
    RecordText
};

// 진단 이름(열거자 철자)이다.
ROCKS_NODEGRAPHTOOLS_API StringView GraphTextCodeName(GraphTextCode code) noexcept;

struct GraphTextDiagnostic {
    explicit GraphTextDiagnostic(Allocator& allocator) noexcept : message(allocator) {
    }

    GraphTextCode code = GraphTextCode::InvalidValue;
    // 1부터 센 줄·열(열은 code point)이다. 0이면 위치가 없다.
    uint32 line = 0;
    uint32 column = 0;
    String message;
};

struct ROCKS_NODEGRAPHTOOLS_API GraphTextDiagnostics {
    explicit GraphTextDiagnostics(Allocator& allocator) noexcept : items(allocator) {
    }

    // 진단 하나를 더한다. 할당 실패는 그 오류다.
    Result<void> Add(GraphTextCode code, JsonPosition position, StringView message) noexcept;

    Array<GraphTextDiagnostic> items;
};

// 조각의 노드 하나다. unresolvedType이 비어 있지 않으면 텍스트 레코드 값의 type이 지금 등록되어
// 있지 않아 바이트로 바꾸지 못한 노드다(그 레코드는 비어 있다 — 붙여넣기가 진단하고 뺀다).
struct GraphFragmentNode {
    explicit GraphFragmentNode(Allocator& allocator) noexcept
        : node(allocator), layout(allocator), unresolvedType(allocator) {
    }

    GraphNode node;
    NodeLayout layout;
    String unresolvedType;
};

// 그래프 조각이다(9.2절). edges는 조각 노드끼리, externalEdges는 조각 노드와 밖 노드 사이 간선(원래
// 끝 그대로)이다. sourceAsset이 0이면 출처 에셋이 없다.
struct GraphFragment {
    explicit GraphFragment(Allocator& allocator) noexcept
        : nodes(allocator), edges(allocator), externalEdges(allocator), comments(allocator) {
    }

    Name domain;
    Guid sourceAsset;
    Guid sourceGraph;
    Array<GraphFragmentNode> nodes;
    Array<GraphEdge> edges;
    Array<GraphEdge> externalEdges;
    Array<CommentBox> comments;
};

// 문서(와 배치 — null이면 배치를 쓰지 않는다)를 `rocks.graph-document` JSON으로 out에 덧붙인다.
// 같은 문서·옵션은 같은 바이트다(레코드 값은 다시 읽어 같은 바이트가 되는 리플렉션 텍스트면 텍스트,
// 아니면 바이트). 오류: Name 토큰이 pool에 없으면 그 오류, JSON 쓰기 오류(InvalidArgument·
// CapacityExceeded·OutOfMemory). 실패하면 out에 쓰다 만 내용이 남는다(호출자가 버린다).
ROCKS_NODEGRAPHTOOLS_API Result<void> WriteGraphDocumentJson(GraphTextContext& context,
                                                             const GraphDocument& document,
                                                             const GraphDocumentLayout* layout,
                                                             const GraphTextWriteOptions& options,
                                                             StringBuilderBase& out) noexcept;

// text를 문서와 배치로 읽고 정규 순서로 맞춘다(CanonicalizeGraphDocument — 불변식 검사는 호출자).
// 배치가 없는 노드는 기본 배치다. 실패하면 InvalidArgument이고 diagnostics에 줄·열과 사유가
// 있다(미등록 텍스트 레코드도 실패 — 문서를 바꿔 쓰지 않는다). 실패하면 document·layout 내용은
// 정해지지 않는다.
ROCKS_NODEGRAPHTOOLS_API Result<void>
ReadGraphDocumentJson(GraphTextContext& context, StringView text, GraphDocument& document,
                      GraphDocumentLayout& layout, GraphTextDiagnostics& diagnostics) noexcept;

// 조각을 `rocks.graph-fragment` JSON으로 out에 덧붙인다(레코드 텍스트는 Copy 용도). 미해석 노드는
// 쓸 수 없다(InvalidArgument).
ROCKS_NODEGRAPHTOOLS_API Result<void> WriteGraphFragmentJson(GraphTextContext& context,
                                                             const GraphFragment& fragment,
                                                             const GraphTextWriteOptions& options,
                                                             StringBuilderBase& out) noexcept;

// text를 조각으로 읽는다. 텍스트 레코드의 type이 등록되어 있지 않은 노드는 unresolvedType을 채워
// 둔다(진단 없음 — 붙여넣기가 보고한다). 그 밖의 오류는 InvalidArgument와 진단이다. 실패하면
// fragment 내용은 정해지지 않는다. 조각은 정규화하지 않는다(노드 안 목록 순서는 InsertNode가
// 맞춘다).
ROCKS_NODEGRAPHTOOLS_API Result<void>
ReadGraphFragmentJson(GraphTextContext& context, StringView text, GraphFragment& fragment,
                      GraphTextDiagnostics& diagnostics) noexcept;

} // namespace rocks::nodegraph
