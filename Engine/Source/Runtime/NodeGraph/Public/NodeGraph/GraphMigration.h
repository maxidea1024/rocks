#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <NodeGraph/GraphCommand.h>
#include <NodeGraph/GraphDocument.h>
#include <NodeGraph/NodeTypeRegistry.h>

// 노드 migration 보고서와 headless 적용 함수다(Docs/Design/NodeGraph 10.1절, GD13).
//
// 사용 예(쿠커·commandlet):
//   GraphMigrationReport report(allocator);
//   auto migrated = MigrateGraphDocument(document, layout, registry, report);
//   // 실패면 문서 오류·할당 실패, 성공이면 entries의 failure가 노드별 결과다.
//
// 에디터는 같은 일을 GraphMutator::Migrate로 한다(통지 원인 Migration). 두 경로는 구현이 하나라서
// 결과 바이트가 같다.

namespace rocks::nodegraph {

// migration 대상 노드 하나의 결과다. 실패하면 노드는 옛 버전 그대로 남고 failure에 사유가 있다.
struct GraphMigrationEntry {
    Guid graph;
    Guid node;
    NodeTypeId fromType;
    uint32 fromVersion = 0;
    // 성공했을 때만 의미가 있다.
    NodeTypeId toType;
    uint32 toVersion = 0;
    // 이 migration이 새로 만든 고아 핀 수다(사라진 핀에 간선·값이 있었다).
    uint32 retainedCreated = 0;
    GraphCommandReason failure = GraphCommandReason::None;
    // failure가 있을 때 관련 핀 키다(없으면 None).
    Name pin;
};

// migration 보고서다. entries는 그래프 순·노드 순이다.
struct GraphMigrationReport {
    explicit GraphMigrationReport(Allocator& allocator) noexcept : entries(allocator) {
    }

    Array<GraphMigrationEntry> entries;
};

// 문서의 옛 버전 노드를 모두 현재 서술 버전으로 옮긴다(10.1절). 내부에서 임시 GraphMutator를
// 만들어 Migrate를 부른다. 노드별 실패는 보고서에 적고 Success를 돌려준다. 문서가 불변식을 어기면
// GraphMutator::Initialize의 오류, 할당 실패는 OutOfMemory(문서는 그대로)다.
ROCKS_NODEGRAPH_API Result<void> MigrateGraphDocument(GraphDocument& document,
                                                      GraphDocumentLayout& layout,
                                                      const NodeTypeRegistry& registry,
                                                      GraphMigrationReport& report) noexcept;

} // namespace rocks::nodegraph
