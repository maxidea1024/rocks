#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <NodeGraph/GraphDocument.h>

// 위치가 있는 그래프 진단이다(Docs/Design/NodeGraph GD19). 도메인 검증·컴파일러가 내고 화면은
// 그대로 표시한다. 노드에 오류 필드를 저장하지 않는다.
//
// 사용 예:
//   GraphDiagnosticList diagnostics(allocator);
//   schema.Validate(view, diagnostics);
//   AppendViolationDiagnostics(violations, assetGuid, diagnostics);
namespace rocks::nodegraph {

struct GraphViolationList;

enum class GraphDiagnosticSeverity : uint8 {
    Info,
    Warning,
    Error
};

// 진단 하나다. asset은 자산 id(AssetId의 Guid 값)다. 메시지는 영어다.
struct GraphDiagnostic {
    explicit GraphDiagnostic(Allocator& allocator) noexcept : message(allocator) {
    }

    Guid asset;
    Guid graph;
    Guid node;
    Name pin;
    GraphDiagnosticSeverity severity = GraphDiagnosticSeverity::Error;
    // 도메인이 정한 코드(공통 불변식 위반은 GI 번호)다.
    uint32 code = 0;
    String message;
};

struct ROCKS_NODEGRAPH_API GraphDiagnosticList {
    explicit GraphDiagnosticList(Allocator& allocator) noexcept : items(allocator) {
    }

    // 진단 하나를 더한다. 할당 실패는 그 오류다.
    Result<void> Add(GraphDiagnosticSeverity severity, uint32 code, Guid asset, Guid graph,
                     Guid node, Name pin, StringView message) noexcept;

    Array<GraphDiagnostic> items;
};

// 불변식 위반을 오류 진단으로 바꿔 out에 더한다(코드 = GI 번호, 메시지 = 규칙의 영어 설명).
ROCKS_NODEGRAPH_API Result<void> AppendViolationDiagnostics(const GraphViolationList& violations,
                                                            Guid asset,
                                                            GraphDiagnosticList& out) noexcept;

} // namespace rocks::nodegraph
