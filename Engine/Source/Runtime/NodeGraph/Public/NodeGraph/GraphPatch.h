#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <NodeGraph/GraphDocument.h>

// 되돌릴 수 있는 문서 변경 기록이다(Docs/Design/NodeGraph 8.6절 "op와 patch"). GraphMutator가
// 명령· bracket마다 만들고, undo·redo는 같은 mutator가 이 patch를 다시 적용한다. 내용(op)은 mutator
// 전용이다.
//
// 사용 예(Transaction 대신 시험이 이력을 쥔다):
//   auto patch = mutator.Execute(source, mutator.Revision(), list);
//   mutator.Undo(source, patch.Value());
//   mutator.Redo(source, patch.Value());
//
// 수명: op 값은 문서 allocator로 만든다. patch는 문서와 mutator보다 오래 살면 안 된다(적용은 그
// mutator만 한다).
namespace rocks::nodegraph {

class GraphMutator;

namespace detail {

class GraphMutatorCore;

// op가 가리키는 문서 슬롯 종류다(8.6절 표).
enum class GraphSlotKind : uint8 {
    Graph,
    GraphTitle,
    Node,
    NodeLayout,
    Edge,
    Comment,
    Bookmark
};

// op 하나다: 슬롯 키와 보관 값. 적용은 슬롯과 값을 맞바꾸고 hasValue를 뒤집는다(Title·값 바꾸기는
// 그대로).
struct GraphPatchOp {
    explicit GraphPatchOp(Allocator& allocator) noexcept
        : graphValue(allocator), graphLayoutValue(allocator), title(allocator), node(allocator),
          layout(allocator), comment(allocator) {
    }

    GraphSlotKind kind = GraphSlotKind::Node;
    // 슬롯 키: 그래프, 노드·주석 id, 간선 키(edge), 북마크 번호.
    Guid graph;
    Guid id;
    GraphEdge edgeKey;
    uint32 number = 0;
    // op가 값을 담고 있는가(담고 있으면 다음 적용이 슬롯에 넣거나 맞바꾼다).
    bool hasValue = false;
    // 직전 적용이 "있음 → 있음"(값 바꾸기)이었는가 — 합치기 판정(8.6절).
    bool replaced = false;
    Graph graphValue;
    GraphLayout graphLayoutValue;
    String title;
    GraphNode node;
    NodeLayout layout;
    GraphEdge edge;
    CommentBox comment;
    Bookmark bookmark;
};

} // namespace detail

class ROCKS_NODEGRAPH_API GraphPatch {
public:
    explicit GraphPatch(Allocator& allocator) noexcept : ops_(allocator) {
    }

    GraphPatch(GraphPatch&&) noexcept = default;
    GraphPatch& operator=(GraphPatch&&) noexcept = default;
    GraphPatch(const GraphPatch&) = delete;
    GraphPatch& operator=(const GraphPatch&) = delete;

    // op가 없으면 참이다(bracket 안 명령의 patch, 아무것도 바꾸지 않은 명령).
    bool IsEmpty() const noexcept {
        return ops_.IsEmpty();
    }

    usize OpCount() const noexcept {
        return ops_.Size();
    }

    // 지금 문서에 적용된 상태인가(참이면 Undo, 거짓이면 Redo를 받는다).
    bool IsApplied() const noexcept {
        return applied_;
    }

    // undo 메모리 budget용 크기 추정(바이트)이다(GD6). op 구조체와 담은 값의 가변 바이트를 더한다.
    usize ApproximateBytes() const noexcept;

private:
    friend class detail::GraphMutatorCore;

    Array<detail::GraphPatchOp> ops_;
    // 이 patch를 만든 mutator(내부)다(다른 mutator는 적용을 거부한다).
    const detail::GraphMutatorCore* owner_ = nullptr;
    bool applied_ = true;
    bool semantic_ = false;
    // 적용 전·후 문서 상태 토큰이다(8.6절 — Undo는 지금 상태가 stateAfter_일 때만, Redo는
    // stateBefore_일 때만 받는다. LIFO 위반을 StaleHandle로 잡는다).
    uint64 stateBefore_ = 0;
    uint64 stateAfter_ = 0;
};

} // namespace rocks::nodegraph
