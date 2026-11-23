#pragma once

#include <Core/Result.h>
#include <NodeGraph/GraphDocument.h>

// 문서·배치 값의 깊은 복사다(Docs/Design/NodeGraph GD24 — 컴파일러·검색 색인에 넘기는 불변
// snapshot, 8.6절 명령 인자의 사본). 대상은 이미 allocator로 만든 값이고, 복사는 대상의 allocator로
// 한다. 실패하면 대상 내용은 정해지지 않는다(버린다).
//
// 사용 예(컴파일 snapshot):
//   GraphDocument snapshot(compilerAllocator);
//   if (!CloneGraphDocument(document, snapshot)) { ... }
namespace rocks::nodegraph {

ROCKS_NODEGRAPH_API Result<void> CloneBytes(const Array<uint8>& source, Array<uint8>& out) noexcept;

ROCKS_NODEGRAPH_API Result<void> CloneGraphNode(const GraphNode& source, GraphNode& out) noexcept;

ROCKS_NODEGRAPH_API Result<void> CloneGraph(const Graph& source, Graph& out) noexcept;

ROCKS_NODEGRAPH_API Result<void> CloneGraphDocument(const GraphDocument& source,
                                                    GraphDocument& out) noexcept;

ROCKS_NODEGRAPH_API Result<void> CloneNodeLayout(const NodeLayout& source,
                                                 NodeLayout& out) noexcept;

ROCKS_NODEGRAPH_API Result<void> CloneCommentBox(const CommentBox& source,
                                                 CommentBox& out) noexcept;

ROCKS_NODEGRAPH_API Result<void> CloneGraphLayout(const GraphLayout& source,
                                                  GraphLayout& out) noexcept;

ROCKS_NODEGRAPH_API Result<void> CloneGraphDocumentLayout(const GraphDocumentLayout& source,
                                                          GraphDocumentLayout& out) noexcept;

} // namespace rocks::nodegraph
