#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <NodeGraph/GraphDocument.h>
#include <NodeGraph/NodeTypeRegistry.h>

// 문서 정규화와 전체 불변식 검사다(Docs/Design/NodeGraph 6.3절, GI1–GI14).
//
// 사용 예:
//   // import·시험 입력을 정규 순서로 맞춘다.
//   CanonicalizeGraphDocument(document, &layout, names, scratch);
//   GraphViolationList violations(allocator);
//   auto valid = ValidateGraphDocument(document, &layout, registry, scratch, violations);
//   if (!valid && valid.Error() == ErrorCode::InvalidArgument) {
//       // violations.items에 위반이 모두 있다(상한 MaxReportedGraphViolations)
//   }
namespace rocks::nodegraph {

// 보고하는 위반의 최대 수다. 넘는 위반은 세기만 한다(GraphViolationList::dropped).
inline constexpr uint32 MaxReportedGraphViolations = 256;

// 불변식이다(6.3절 표의 GI 번호와 같다).
enum class GraphInvariant : uint8 {
    // GI1: 간선 양 끝 노드가 같은 그래프에 있다.
    EdgeNodesInGraph = 1,
    // GI2: 간선 끝 핀이 현재 핀 목록 또는 고아 핀에 있고 방향이 출력 → 입력이다.
    EdgePinExists = 2,
    // GI3: 입력 핀의 간선 수 ≤ maxEdges.
    InputEdgeLimit = 3,
    // GI4: 같은 (출력, 입력) 쌍의 간선은 하나다.
    DuplicateEdge = 4,
    // GI5: 정규 순서와 출력 핀 순번.
    CanonicalOrder = 5,
    // GI6: 배치 그래프·노드 항목이 문서 그래프·노드와 1:1이다(빠짐·남음·같은 항목 둘). 배치 순서는
    // GI5, 주석 id 중복은 GI9다.
    LayoutMatchesDocument = 6,
    // GI7: 고아 핀에 연결이나 사용자 값이 있다.
    RetainedPinUnused = 7,
    // GI8: 같은 PinKey가 현재 핀 목록과 고아 핀에 동시에 있지 않다.
    RetainedPinShadowed = 8,
    // GI9: NodeId·GraphId·주석 id가 문서 안에서 겹치지 않는다.
    DuplicateId = 9,
    // GI10: 재경유 사슬에 순환이 없다.
    RerouteCycle = 10,
    // GI11: id는 0이 아니고 도메인·노드 종류·PinKey는 None이 아니다.
    EmptyIdentity = 11,
    // GI12: 문서 머리(형식 버전, 주 그래프, 역할 번호).
    DocumentHeader = 12,
    // GI13: 알려진 종류 노드의 버전·속성 schema·비활성 허용.
    NodeTypeData = 13,
    // GI14: 입력 기본값의 키가 현재 입력 핀이고 값이 비어 있지 않다.
    InputDefaultPin = 14
};

// 위반이 가리키는 요소의 종류다.
enum class GraphSubject : uint8 {
    Document = 0,
    Graph = 1,
    Node = 2,
    Edge = 3,
    InputDefault = 4,
    RetainedPin = 5,
    LayoutGraph = 6,
    LayoutNode = 7,
    Comment = 8,
    Bookmark = 9
};

// 위반 하나다. 해당하지 않는 칸은 0·None이다.
struct GraphViolation {
    GraphInvariant rule = GraphInvariant::CanonicalOrder;
    GraphSubject subject = GraphSubject::Document;
    // 위반이 있는 그래프다(문서 머리면 0).
    Guid graph;
    // 위반이 있는 노드다(간선이면 입력 노드, 주석이면 주석 id).
    Guid node;
    // 관련 id다(간선이면 출력 노드, id 중복이면 처음 쓴 그래프).
    Guid related;
    // 관련 핀이다(간선이면 문제의 끝 핀).
    Name pin;
    // 배열 안 위치다(그래프·노드·간선·입력 기본값·고아 핀·배치 항목의 순번).
    uint32 index = 0;
};

// 위반 목록이다.
struct ROCKS_NODEGRAPH_API GraphViolationList {
    explicit GraphViolationList(Allocator& allocator) noexcept : items(allocator) {
    }

    Array<GraphViolation> items;
    // 상한을 넘어 버린 위반 수다.
    uint32 dropped = 0;

    // rule 위반이 하나라도 있는가.
    bool Has(GraphInvariant rule) const noexcept;

    // rule·subject 위반이 하나라도 있는가.
    bool Has(GraphInvariant rule, GraphSubject subject) const noexcept;

    void Clear() noexcept {
        items.Clear();
        dropped = 0;
    }
};

// document(와 layout)의 배열을 정규 순서로 정렬한다(6.3절): 그래프·노드는 id 순, 간선은 (입력
// NodeId, 입력 PinKey, 출력 NodeId, 출력 PinKey, 순번) 순, 입력 기본값·고아 핀은 PinKey 순, 배치
// 그래프·노드·주석은 id 순, 북마크는 번호 순. 같은 키의 원소는 상대 순서를 유지하지 않는다(중복은
// 검사가 GI4·GI5·GI9로 보고한다). 중복·빠진 항목을 고치지 않는다.
//
// 오류: names의 원문 조회 오류(StaleHandle 등), 토큰 구조 오류 InvalidArgument, 할당 실패. 실패하면
// 문서는 바뀌지 않았거나 일부만 정렬됐다(내용은 그대로, 순서만 다르다). 스레드: 문서 소유 lane.
ROCKS_NODEGRAPH_API Result<void> CanonicalizeGraphDocument(GraphDocument& document,
                                                           GraphDocumentLayout* layout,
                                                           NamePool& names,
                                                           Allocator& scratch) noexcept;

// document(와 layout — nullptr이면 배치 검사를 건너뛴다)가 GI1–GI14를 지키는지 모두 검사해 out에
// 위반을 모은다(out은 먼저 비운다). 알 수 없는 종류의 노드는 6.3절 "알 수 없는 종류"대로 일부
// 검사를 건너뛴다. 문서를 바꾸지 않는다.
//
// 반환: 위반이 없으면 성공, 있으면 InvalidArgument(out에 위반). 그 밖의 오류: names 조회 오류, 동적
// 핀 함수 오류, 할당 실패(out은 정해지지 않는다). 스레드: 문서 소유 lane(registry도 같은 lane).
ROCKS_NODEGRAPH_API Result<void> ValidateGraphDocument(const GraphDocument& document,
                                                       const GraphDocumentLayout* layout,
                                                       const NodeTypeRegistry& registry,
                                                       Allocator& scratch,
                                                       GraphViolationList& out) noexcept;

} // namespace rocks::nodegraph
