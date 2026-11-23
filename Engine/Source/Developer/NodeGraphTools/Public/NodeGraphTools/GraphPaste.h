#pragma once

#include <NodeGraph/GraphCommand.h>
#include <NodeGraph/GraphDiagnostic.h>
#include <NodeGraph/GraphIdSource.h>
#include <NodeGraph/NodeTypeRegistry.h>
#include <NodeGraphTools/GraphText.h>

// 복사·붙여넣기다(Docs/Design/NodeGraph 9.2·9.4절). 복사는 문서의 노드·주석 선택을 조각으로
// 담고, 붙여넣기는 조각을 대상 문서의 명령 목록 하나(undo 1개)로 바꾼다. 명령 실행은 호출자가
// GraphMutator로 한다.
//
// 사용 예:
//   GraphFragment fragment(allocator);
//   auto copied = CopyGraphFragment(document, &layout, registry, graph, selected, comments, asset,
//                                   fragment);
//   GraphPasteOptions options;
//   options.graph = target;
//   options.position = Vec2(100, 40);
//   GraphPasteResult result(allocator);
//   auto built = BuildPasteCommands(fragment, document, registry, options, ids, result);
//   auto patch = mutator.Execute(source, mutator.Revision(), result.commands);
namespace rocks::nodegraph {

// 붙여넣기 진단 코드다(GraphDiagnostic::code — 뺀 노드·간선마다 하나, 심각도 Warning). 진단의
// node는 조각 쪽 옛 id다(조각 간선은 빠진 끝, 바깥 간선은 조각 쪽 끝).
enum class GraphPasteCode : uint32 {
    // 텍스트 속성의 type이 등록되어 있지 않아 바이트로 바꾸지 못했다.
    UnresolvedProperties = 1,
    // 노드 종류 버전이 지금 서술과 다르다(옛 버전 migration은 NG3).
    TypeVersion,
    // 속성 Record schema가 서술과 다르다.
    PropertiesSchema,
    // 노드 종류가 복제를 허락하지 않는다(CanDuplicate 거짓).
    CannotDuplicate,
    // 문서당 하나인 종류가 이미 있다.
    SinglePerDocument,
    // 도메인 schema가 붙여넣기를 거부했다(CanPasteNode).
    PasteDenied,
    // 바깥 간선을 다시 잇지 않았다(다른 문서·schema가 허락하지 않음·밖 노드·핀 없음·판정이 Make가
    // 아님).
    ExternalEdgeDropped,
    // 조각 간선의 끝 노드가 빠졌거나 끝 핀이 현재 핀·고아 핀이 아니어서 그 간선을 뺐다.
    EdgeDropped,
    // 노드 데이터가 InsertNode 규칙을 어긴다(키 None·중복, 고아 핀 방향·가림, 재경유의 고아 핀,
    // 활성 상태, 입력 기본값이 현재 입력 핀이 아님, 대상 그래프에서 핀 계산 실패).
    NodeData
};

// 옛 id → 새 id다.
struct GraphIdMapping {
    Guid from;
    Guid to;
};

// 붙여넣기 옵션이다. targetAsset은 대상 문서의 에셋 id(조각의 sourceAsset과 같고 0이 아니면 같은
// 문서로 본다 — 바깥 간선 판정), graph는 대상 그래프, position은 조각 왼쪽 위가 놓일 캔버스 위치다.
struct GraphPasteOptions {
    Guid targetAsset;
    Guid graph;
    Vec2 position;
};

struct ROCKS_NODEGRAPHTOOLS_API GraphPasteResult {
    explicit GraphPasteResult(Allocator& allocator) noexcept
        : commands(allocator), diagnostics(allocator), nodes(allocator), comments(allocator) {
    }

    GraphCommandList commands;
    GraphDiagnosticList diagnostics;
    // 붙인 노드·주석의 옛 id → 새 id(조각 순서)다.
    Array<GraphIdMapping> nodes;
    Array<GraphIdMapping> comments;
};

// document의 graph에서 nodes·comments를 조각으로 담는다(out을 먼저 비운다). 고른 노드끼리의 간선은
// edges, 고른 노드와 나머지 사이 간선은 externalEdges다. 알려진 종류인데 CanDuplicate가 거짓인
// 노드는 빼고 그 노드와의 간선은 바깥 간선이 된다. 조각 노드·주석은 id 순이다(고른 순서와 무관).
// layout이 null이거나 배치가 없는 노드는 기본 배치다. 오류: 그래프·노드·주석이 없으면 NotFound,
// 같은 id가 두 번이면 InvalidArgument, 할당 실패.
ROCKS_NODEGRAPHTOOLS_API Result<void>
CopyGraphFragment(const GraphDocument& document, const GraphDocumentLayout* layout,
                  const NodeTypeRegistry& registry, Guid graph, Span<const Guid> nodes,
                  Span<const Guid> comments, Guid sourceAsset, GraphFragment& out) noexcept;

// fragment를 document(대상 — 읽기만 한다)의 options.graph에 붙이는 명령 목록을 out에 만든다(out을
// 먼저 비운다 — 9.4절 붙여넣기 절차). 노드마다 새 id를 ids에서 받는다(같은 조각·문서·seed면 같은
// 명령 목록). 뺀 노드·간선은 진단(Warning)이다. 오류: 도메인이 다르면 InvalidArgument, 대상
// 그래프가 없으면 NotFound, id 발급·할당 실패.
ROCKS_NODEGRAPHTOOLS_API Result<void>
BuildPasteCommands(const GraphFragment& fragment, const GraphDocument& document,
                   const NodeTypeRegistry& registry, const GraphPasteOptions& options,
                   GraphIdSource& ids, GraphPasteResult& out) noexcept;

} // namespace rocks::nodegraph
