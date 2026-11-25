#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>
#include <NodeGraph/GraphChange.h>
#include <NodeGraph/GraphCommand.h>
#include <NodeGraph/GraphDocument.h>
#include <NodeGraph/GraphIdSource.h>
#include <NodeGraph/GraphMigration.h>
#include <NodeGraph/GraphPatch.h>
#include <NodeGraph/GraphReadView.h>
#include <NodeGraph/NodeTypeRegistry.h>

// 문서의 유일한 변경 API다(Docs/Design/NodeGraph GD5·8절·8.6절). 명령 목록을 차례로
// 적용·검증하고, 실패하면 무할당으로 되돌리며, 성공하면 patch와 통지 묶음을 낸다.
// undo·redo·bracket도 여기서 한다.
//
// 사용 예:
//   GraphMutator mutator(document, layout, registry, ids);
//   if (!mutator.Initialize()) { ... }       // 문서가 불변식을 지켜야 한다
//   GraphCommandFailure failure;
//   auto patch = mutator.Execute(source, mutator.Revision(), list, &failure);
//   if (!patch) { ... failure.reason ... }
//   mutator.Undo(source, patch.Value());
//
// 스레드: 문서 소유 lane 하나에서만 쓴다(GD24). 수명: document·layout·registry·ids는 mutator보다
// 오래 산다. mutator 밖에서 문서를 바꾸면 안 된다(색인·revision이 어긋난다 — 문서 교체는 mutator를
// 새로 만든다).
namespace rocks::nodegraph {

namespace detail {

class GraphMutatorCore;

} // namespace detail

class ROCKS_NODEGRAPH_API GraphMutator {
public:
    GraphMutator(GraphDocument& document, GraphDocumentLayout& layout,
                 const NodeTypeRegistry& registry, GraphIdSource& ids) noexcept;
    ~GraphMutator() noexcept;

    GraphMutator(const GraphMutator&) = delete;
    GraphMutator& operator=(const GraphMutator&) = delete;

    // 문서를 검사하고(ValidateGraphDocument, 배치 포함) 파생 색인을 세운다. 다른 함수보다 먼저 한
    // 번 부른다. 오류: 문서 위반 InvalidArgument, 두 번째 호출 AlreadyExists, 할당·names 오류.
    Result<void> Initialize() noexcept;

    // 문서 revision이다(Initialize 뒤 0, 성공한 명령·undo·redo·취소마다 1 오른다).
    uint64 Revision() const noexcept;

    // 명령 목록을 적용한다(8.2·8.6절). 반환 patch는 undo용이다(bracket 안이면 빈 patch — bracket
    // patch에 합친다).
    //
    // 오류: Initialize 전 Unsupported, revision이 expectedRevision과 다름 StaleHandle, 다른
    // source의 bracket이 열림 Busy, 명령 검증 위반 InvalidArgument(failure에 명령 번호·사유),
    // 할당·names 오류. 실패하면 문서·revision은 그대로이고 통지는 없다.
    Result<GraphPatch> Execute(uint64 source, uint64 expectedRevision,
                               const GraphCommandList& commands,
                               GraphCommandFailure* failure = nullptr) noexcept;

    // 적용된 patch를 되돌린다(LIFO로 써야 한다 — 마지막에 적용한 patch부터). patch는 Redo를 받는
    // 상태가 된다. 오류: bracket이 열림 Busy, 다른 mutator의 patch·이미 되돌린 patch
    // InvalidArgument, 문서가 이 patch를 적용한 직후 상태가 아님(LIFO 위반 — 상태 토큰으로 판정)
    // StaleHandle, 통지 기록의 할당 실패. 실패하면 문서는 그대로다.
    Result<void> Undo(uint64 source, GraphPatch& patch) noexcept;

    // 되돌린 patch를 다시 적용한다. 오류는 Undo와 같다(이미 적용된 patch면 InvalidArgument).
    Result<void> Redo(uint64 source, GraphPatch& patch) noexcept;

    // 서술 버전보다 낮은 노드를 모두 현재 버전으로 옮긴다(GD13, Docs/Design/NodeGraph 10.1절).
    // 노드마다 서술의 migrate를 버전이 닿을 때까지 거듭 부르고, 핀 재정리(고아 핀 생성·재결합·키
    // 재지정)를 한다. 실패한 노드는 그대로 두고 report에 사유를 적는다. 옮긴 노드가 있으면
    // revision을 하나 올리고 원인 Migration 통지를 보낸다. undo 이력에 넣지 않으므로 그 전 patch는
    // 모두 낡는다 (StaleHandle). 옮길 노드가 없으면 아무것도 바꾸지 않는다. 오류: Initialize 전
    // Unsupported, bracket이 열려 있으면 Busy, 할당 실패 OutOfMemory(문서 불변).
    Result<void> Migrate(uint64 source, GraphMigrationReport& report) noexcept;

    // 등록표 변경(NodeTypeRegistryListener 통지)을 문서에 반영한다(10.1절). 문서 도메인의 종류만
    // 보고 문서 전체를 다시 검사한다. 통과하면 종류마다 NodeTypesChanged(원인 Registry)를 보낸다.
    // revision·문서 바이트는 그대로다. 목록의 종류가 지금 등록돼 있으면(재등록) undo 이력을 낡게
    // 한다. bracket이 열려 있거나 통지 콜백 안이면 미뤘다가 bracket이 끝날 때·통지가 끝날 때
    // 반영한다(Success). 오류: Initialize 전 Unsupported, 검사 실패(같은 버전인데 핀이 다른 재등록,
    // 버전을 낮춘 재등록) TypeMismatch, 검사·미루기 중 할당 실패 OutOfMemory — 두 경우 모두
    // mutator는 Reload 전까지 명령·undo·redo·bracket·migration을 DependencyFailed로 거부한다.
    Result<void> RefreshNodeTypes(uint64 source, Span<const NodeTypeKey> keys) noexcept;

    // 문서·배치 전체를 바꿔 넣는다(파일 다시 읽기, 10.1절). 새 내용을 먼저 검사하고 색인을 만든 뒤
    // 바꾸므로 실패하면 옛 내용이 그대로다. 성공하면 revision을 하나 올리고 undo 이력을 낡게 하며
    // DependencyFailed 상태를 풀고, DocumentReloaded(원인 Reload)를 보낸다. migration은 따로
    // 부른다. 넘긴 문서의 할당자를 그대로 쓰므로 그 할당자는 mutator보다 오래 살아야 한다.
    // 오류: Unsupported, bracket 중·통지 콜백 안 Busy, 도메인이 다르면 TypeMismatch, 불변식 위반은
    // 검사 오류, 할당 실패 OutOfMemory.
    Result<void> Reload(uint64 source, GraphDocument&& document,
                        GraphDocumentLayout&& layout) noexcept;

    // bracket을 연다(8.2절). 이미 열려 있으면 Busy다.
    Result<void> BeginInteraction(uint64 source) noexcept;

    // bracket을 닫고 합친 patch를 돌려준다. 확정 통지(interactive 거짓)를 보낸다. 열지 않은
    // source면 Busy, 열린 bracket이 없으면 NotReady다.
    Result<GraphPatch> EndInteraction(uint64 source) noexcept;

    // bracket을 취소한다(bracket 동안의 변경을 되돌리고 원인 Cancel 통지). 오류는 EndInteraction과
    // 같고, 통지 기록의 할당이 실패하면 문서를 bracket 상태로 둔 채 bracket을 열어 두고 그 오류를
    // 돌려준다(다시 취소하거나 확정할 수 있다).
    Result<void> CancelInteraction(uint64 source) noexcept;

    bool InteractionOpen() const noexcept;

    // 통지 구독자를 더한다·뺀다. 구독자는 mutator보다 오래 살거나 먼저 뺀다.
    Result<void> AddListener(GraphChangeListener& listener) noexcept;

    void RemoveListener(GraphChangeListener& listener) noexcept;

    // Initialize 뒤에 부른다. 참이면 성공한 명령·undo·redo마다 문서 전체 불변식을
    // 검사한다(디버그·시험용, 6.3절). 위반이면 그 변경을 되돌리고 InvalidArgument(사유
    // InvariantBroken)를 돌려준다.
    void SetFullValidation(bool enabled) noexcept;

    // 읽기 view다.
    GraphReadView View() const noexcept;

private:
    GraphDocument& document_;
    GraphDocumentLayout& layout_;
    const NodeTypeRegistry& registry_;
    GraphIdSource& ids_;
    UniquePtr<detail::GraphMutatorCore> core_;
};

} // namespace rocks::nodegraph
