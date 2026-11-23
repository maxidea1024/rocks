#include "GraphMutatorCore.h"

#include <Core/Assert.h>
#include <NodeGraph/GraphValidation.h>

// 등록표 변경 반영과 문서 다시 읽기다(Docs/Design/NodeGraph 10.1절 NG3b).

namespace rocks::nodegraph::detail {

Result<void> GraphMutatorCore::RefreshNodeTypes(uint64 source,
                                                Span<const NodeTypeKey> keys) noexcept {
    static_cast<void>(source);
    if (!initialized_) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    // bracket 중이거나 통지 콜백 안이면 미뤘다가 bracket이 끝날 때·통지가 끝날 때 반영한다(등록표는
    // 이미 바뀌었으므로 버리지 않는다). 미룰 자리를 잡지 못하면 검사 없이 명령을 받을 수 없으므로
    // Reload 전까지 막는다.
    if (interaction_ || notifying_) {
        if (auto appended = pendingRefresh_.TryAppend(keys); !appended) {
            broken_ = true;
            return appended;
        }

        return Result<void>::Success();
    }

    bool relevant = false;
    bool reregistered = false;
    for (const NodeTypeKey& key : keys) {
        if (key.domain == document_.domain) {
            relevant = true;
            reregistered = reregistered || registry_.FindNodeType(key.domain, key.type) != nullptr;
        }
    }

    if (!relevant) {
        return Result<void>::Success();
    }

    // 종류 하나의 해석 변경이 여러 노드·간선에 닿으므로 문서 전체를 다시 본다.
    GraphViolationList violations(allocator_);
    // 검사하지 못하면(할당 실패 포함) 문서가 불변식을 지키는지 모르므로 Reload 전까지 막는다.
    if (auto valid = ValidateGraphDocument(document_, &layout_, registry_, allocator_, violations);
        !valid) {
        broken_ = true;
        return Result<void>::Failure(valid.Error() == ErrorCode::OutOfMemory
                                         ? ErrorCode::OutOfMemory
                                         : ErrorCode::TypeMismatch);
    }

    // 재등록이면 patch 안에만 남은 그 종류 노드(지운 노드)가 바뀐 핀으로 되살아날 수 있으므로
    // 이력을 낡게 한다. 해제만이면 자리 노드 검사가 느슨해 이력을 그대로 둔다. 통지 기록보다 먼저
    // 바꿔 기록이 실패해도 이력이 낡는다.
    if (reregistered) {
        state_ = nextState_;
        ++nextState_;
    }

    pending_.changes.Clear();
    pending_.semanticTouched = false;
    for (const NodeTypeKey& key : keys) {
        if (!(key.domain == document_.domain)) {
            continue;
        }

        if (auto recorded = Record(GraphChangeKind::NodeTypesChanged, Guid{}, Guid{}, Guid{},
                                   key.type, Name(), 0);
            !recorded) {
            pending_.changes.Clear();
            return recorded;
        }
    }

    Notify(GraphChangeCause::Registry, false);
    return Result<void>::Success();
}

Result<void> GraphMutatorCore::Reload(uint64 source, GraphDocument&& document,
                                      GraphDocumentLayout&& layout) noexcept {
    static_cast<void>(source);
    if (!initialized_) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (interaction_ || notifying_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (!(document.domain == document_.domain)) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    // 새 내용을 먼저 검사하고 색인을 만든 뒤 바꾼다(실패하면 옛 내용 그대로).
    GraphViolationList violations(allocator_);
    if (auto valid = ValidateGraphDocument(document, &layout, registry_, allocator_, violations);
        !valid) {
        return valid;
    }

    Array<Guid> ids(allocator_);
    Array<OutgoingEdge> outgoing(allocator_);
    if (auto built = BuildIndexes(document, layout, ids, outgoing); !built) {
        return built;
    }

    // 통지 기록 자리를 먼저 잡아 바꿔 넣은 뒤에는 실패하지 않게 한다.
    if (auto reserved = pending_.changes.TryEnsureCapacity(pending_.changes.Size() + 1);
        !reserved) {
        return reserved;
    }

    document_ = Move(document);
    layout_ = Move(layout);
    idIndex_ = Move(ids);
    outgoing_ = Move(outgoing);
    broken_ = false;
    state_ = nextState_;
    ++nextState_;
    ++revision_;
    pending_.changes.Clear();
    pending_.semanticTouched = true;
    ROCKS_ASSERT(
        Record(GraphChangeKind::DocumentReloaded, Guid{}, Guid{}, Guid{}, Name(), Name(), 0));

    Notify(GraphChangeCause::Reload, false);
    return Result<void>::Success();
}

void GraphMutatorCore::ProcessPendingRefresh() noexcept {
    if (pendingRefresh_.IsEmpty() || interaction_ || notifying_) {
        return;
    }

    // 꺼내서 비운 뒤 반영한다(반영 중 통지가 다시 미루면 다음 차례에 처리된다). 실패는
    // RefreshNodeTypes가 broken_으로 남긴다.
    Array<NodeTypeKey> keys(Move(pendingRefresh_));
    pendingRefresh_ = Array<NodeTypeKey>(allocator_);
    static_cast<void>(RefreshNodeTypes(0, keys.View()));
}

} // namespace rocks::nodegraph::detail
