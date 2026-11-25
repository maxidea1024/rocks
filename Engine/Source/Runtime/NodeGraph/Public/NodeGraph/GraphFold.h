#pragma once

#include <Base/Guid.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <NodeGraph/GraphCommand.h>
#include <NodeGraph/GraphDocument.h>
#include <NodeGraph/GraphIdSource.h>
#include <NodeGraph/GraphReadView.h>

// 접기·펼치기 명령 목록 도우미다(Docs/Design/NodeGraph 10.1절 NG3c, 8.1절 접기 행). 공통 명령만
// 만든다(실행은 GraphMutator::Execute — 실패하면 목록 전체가 되돌아간다).
//
// 사용 예:
//   GraphCommandList list(allocator);
//   auto built = BuildCollapseCommands(mutator.View(), &layout, mainGraph, selected, ids, list);
//   if (built) {
//       auto patch = mutator.Execute(source, mutator.Revision(), list);
//   }
//
// 한계: 순번 있는 출력 핀을 건너는 간선은 다시 이을 때 끝에 붙는다. 하위 그래프 배치 좌표는 부모
// 좌표를 그대로 쓴다(화면 단계 NG7에서 정한다).

namespace rocks::nodegraph {

// 접은 경계 노드를 옮긴 노드 경계 상자에서 띄우는 가로 거리(캔버스 단위)다.
inline constexpr float FoldBoundaryOffset = 200.0f;

// 펼치기 결과다. droppedDefaults는 Graph.Collapsed 입력 기본값 중 안쪽 받는 핀이 Hidden이거나
// 핀을 셀 수 없어 옮기지 못한 수다(진단용).
struct GraphFoldReport {
    uint32 droppedDefaults = 0;
};

// graph의 nodes를 새 Collapsed 하위 그래프로 접는 목록을 out 끝에 더한다. 경계를 건너는 간선을
// 정규 순서로 모아 안쪽 핀마다 경계 핀 하나(키 In<n>·Out<n>)를 만든다. 새 id(하위 그래프·
// Graph.Collapsed·경계 노드 둘)는 ids에서 받는다. layout은 배치 위치 계산용이다(nullptr이면 원점).
// 오류: 그래프·노드 없음 NotFound, nodes가 비었거나 같은 id 두 번·경계 노드 포함 InvalidArgument,
// 핀을 셀 수 없는 노드(자리 노드·migration 전)·고아 핀·구조(Structural) 핀에 건너는 간선이 닿음
// Unsupported, 할당·발급 오류.
ROCKS_NODEGRAPH_API Result<void>
BuildCollapseCommands(const GraphReadView& view, const GraphDocumentLayout* layout, Guid graph,
                      Span<const Guid> nodes, GraphIdSource& ids, GraphCommandList& out) noexcept;

// graph의 Graph.Collapsed collapsedNode를 펼치는 목록을 out 끝에 더한다. 하위 그래프의 경계 노드가
// 아닌 노드를 graph로 옮기고, 경계 핀마다 바깥 끝 × 안쪽 끝을 다시 잇는다(requireMake). 바깥 간선이
// 없던 Collapsed 입력의 기본값은 안쪽 받는 핀에 옮긴다(옮길 곳이 없으면 report에 센다). 오류: 노드
// 없음·Collapsed 아님·하위 그래프 없음 NotFound, Graph.Collapsed·경계 노드에 고아 핀이 있음
// Unsupported(그 간선·값을 이을 곳이 없다), 할당 오류.
ROCKS_NODEGRAPH_API Result<void> BuildExpandCommands(const GraphReadView& view, Guid graph,
                                                     Guid collapsedNode, GraphCommandList& out,
                                                     GraphFoldReport* report = nullptr) noexcept;

} // namespace rocks::nodegraph
