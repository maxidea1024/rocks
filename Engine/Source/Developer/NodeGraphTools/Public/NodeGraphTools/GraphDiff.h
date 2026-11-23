#pragma once

#include <Core/Text/StringBuilder.h>
#include <NodeGraph/GraphDocument.h>

// 문서 diff다(Docs/Design/NodeGraph 9.4절 "diff"). 두 문서(배치 선택)를 GraphId·NodeId·간선
// 키· 주석 id·북마크 번호로 맞춰 차이 항목을 만든다. 두 문서는 같은 NamePool의 Name 토큰을 쓴다.
//
// 사용 예:
//   Array<GraphDiffEntry> entries(allocator);
//   auto computed = ComputeGraphDiff(before, &beforeLayout, after, &afterLayout, {}, entries);
//   StringBuilder<> text(allocator);
//   auto formatted = FormatGraphDiff(names, entries.View(), text);
namespace rocks::nodegraph {

// 차이 항목 종류다(출력 줄의 머리말 — FormatGraphDiff).
enum class GraphDiffKind : uint8 {
    // 문서 머리(도메인·주 그래프·형식 버전) 중 하나가 다르다(number: 0 도메인, 1 주 그래프, 2
    // 버전).
    DocumentHeader,
    GraphAdded,
    GraphRemoved,
    // 그래프 역할·domainRole이 다르다.
    GraphRole,
    // 그래프 제목이 다르다(배치).
    GraphTitle,
    NodeAdded,
    NodeRemoved,
    NodeType,
    NodeVersion,
    // 속성 Record 바이트가 다르다.
    NodeProperties,
    NodeEnableState,
    // 입력 기본값 하나(pin)가 더해지거나 빠지거나 바뀌었다.
    InputDefault,
    // 고아 핀 하나(pin)가 더해지거나 빠지거나 바뀌었다.
    RetainedPin,
    EdgeAdded,
    EdgeRemoved,
    // 간선 순번이 다르다(number = 뒤 문서의 순번).
    EdgeOrder,
    // 노드 배치가 다르다.
    NodeLayout,
    CommentAdded,
    CommentRemoved,
    CommentChanged,
    // 북마크 하나(number)가 더해지거나 빠지거나 바뀌었다.
    Bookmark
};

// 차이 항목 하나다. 해당하지 않는 칸은 0·None이다. 간선 항목은 node·pin이 출력 끝, other·otherPin이
// 입력 끝이다. NodeAdded·NodeRemoved는 pin에 노드 종류를 둔다.
struct GraphDiffEntry {
    GraphDiffKind kind = GraphDiffKind::DocumentHeader;
    Guid graph;
    Guid node;
    Guid other;
    Name pin;
    Name otherPin;
    uint32 number = 0;
};

// semanticOnly면 배치·그래프 제목·주석·북마크를 보지 않는다.
struct GraphDiffOptions {
    bool semanticOnly = false;
};

// before → after의 차이를 out에 만든다(out을 먼저 비운다). 두 문서는 names의 Name 토큰을 쓴다. 항목
// 순서는 그래프 id 순, 그래프 안에서 그래프 → 노드(id 순, 노드마다 종류·버전·속성·활성·입력 기본값·
// 고아 핀) → 간선(문서의 정규 간선 순서 — 핀은 원문 순이라 pool의 발급 순서와 무관) → 배치(노드 id
// 순) → 주석 → 북마크다(결정적). 배치가 null이면 제목·주석·북마크는 비어 있는 것으로 보고, 노드
// 배치는 두 쪽 모두 그 그래프의 배치가 있을 때만 비교한다. 입력은 GI5(중복 없는 키)를 전제한다.
// 오류: Name 조회 실패, 할당 실패.
ROCKS_NODEGRAPHTOOLS_API Result<void>
ComputeGraphDiff(const GraphDocument& before, const GraphDocumentLayout* beforeLayout,
                 const GraphDocument& after, const GraphDocumentLayout* afterLayout,
                 NamePool& names, const GraphDiffOptions& options,
                 Array<GraphDiffEntry>& out) noexcept;

// 항목마다 한 줄(영어 머리말 + id·핀)을 out에 덧붙인다. 예: "edge-added 0000…01 0000…0a.Value ->
// 0000…0c.A". 오류: Name 조회·할당 실패.
ROCKS_NODEGRAPHTOOLS_API Result<void> FormatGraphDiff(NamePool& names,
                                                      Span<const GraphDiffEntry> entries,
                                                      StringBuilderBase& out) noexcept;

} // namespace rocks::nodegraph
