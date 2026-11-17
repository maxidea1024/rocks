#include <Object/PathCursor.h>
#include <Object/PreparedReferences.h>

// Registry 컨테이너 준비 사본(리플렉션 B9i3c3 — Object/PreparedReferences.h)의 수명·게시와 경로
// cursor의 묶기다. 사본 위 연산은 생성 thunk(Object/GeneratedStagedReferenceAccess.h)가 한다.
namespace rocks {

Result<void> PreparedReferences::PreparePublish() noexcept {
    if (!IsReady()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    current_.Clear();
    if (clone_) {
        return ops_->collect(clone_, current_);
    }

    if (auto copied = current_.TryAppend(elements_.View()); !copied) {
        return copied;
    }

    // staged 객체는 트랜잭션의 AssignReferences가 handle 목록을 받는다(버퍼가 필요 없다).
    if (kind_ != PathTarget::Kind::Object) {
        return Result<void>::Success();
    }

    if (!buffer_) {
        auto made = UniquePtr<PreparedReferenceArray>::TryMake(*allocator_, *containerAllocator_);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        buffer_ = Move(made.Value());
    }

    return buffer_->TryAssign(elements_.View());
}

void PreparedReferences::Publish(ObjectBatchPublish& batch) noexcept {
    if (!IsReady() || kind_ != PathTarget::Kind::Object || !batch.IsActive() ||
        !(batch.Owner() == owner_) || (!clone_ && !buffer_)) {
        platform::Fatal(
            "Prepared references need PreparePublish and the open batch of their owner");
    }

    ops_->publish(batch, container_, *this);
}

Result<void> PreparedReferences::Publish() noexcept {
    if (!IsReady() || kind_ != PathTarget::Kind::Staged || !transaction_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return ops_->publishStaged(*transaction_, owner_, container_, *this);
}

void PreparedReferences::Reset() noexcept {
    if (clone_) {
        ops_->destroyClone(clone_);
        clone_ = nullptr;
    }

    buffer_.Reset();
    elements_.Clear();
    previous_.Clear();
    current_.Clear();
    ops_ = nullptr;
    container_ = nullptr;
    containerAllocator_ = nullptr;
    owner_ = {};
    kind_ = PathTarget::Kind::Object;
    transaction_ = nullptr;
}

namespace detail {

Result<void> PathCursor::BindReferences(void* container, const ReferenceStageOps& ops,
                                        Allocator& containerAllocator,
                                        Span<const ObjectHandle> elements, void* clone) noexcept {
    PreparedReferences& stage = *stage_;
    if (stage.IsReady()) {
        if (clone) {
            ops.destroyClone(clone);
        }

        if (stage.container_ != container || stage.ops_ != &ops || !(stage.owner_ == owner_) ||
            stage.kind_ != root_) {
            return Fail(ErrorCode::InvalidArgument, PropertyPathError::InvalidValue);
        }

        return Result<void>::Success();
    }

    // 게시된 객체는 복사의 할당 콜백이 원본을 바꾸지 못하게 고정한다(Detach와 같다).
    Result<void> bound = Pin();
    if (bound) {
        bound = stage.elements_.TryAppend(elements);
    }

    if (bound) {
        bound = stage.previous_.TryAppend(elements);
    }

    if (bound && clone) {
        bound = ops.collect(clone, stage.previous_);
    }

    if (bound) {
        bound = CheckOwner();
    }

    if (!bound) {
        // 실패 기록이 고정을 먼저 푼다 — 사본은 그 뒤에 반환한다.
        Record(bound.Error(), PropertyPathError::None);
        stage.elements_.Clear();
        stage.previous_.Clear();
        if (clone) {
            ops.destroyClone(clone);
        }

        return bound;
    }

    stage.ops_ = &ops;
    stage.container_ = container;
    stage.clone_ = clone;
    stage.containerAllocator_ = &containerAllocator;
    stage.owner_ = owner_;
    stage.kind_ = root_;
    stage.transaction_ = construction_;
    return Result<void>::Success();
}

} // namespace detail

} // namespace rocks
