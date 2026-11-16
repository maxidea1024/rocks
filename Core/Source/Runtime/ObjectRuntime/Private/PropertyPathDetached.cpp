#include "PropertyPathInternal.h"

#include <Object/ConstructionContext.h>
#include <Object/ObjectBatchPublish.h>
#include <Platform/Fatal.h>

// 분리된 값과 경로 연산 Detach·Attach(리플렉션 B9i3c0 — Object/DetachedPath.h)의 구현이다: 상자의
// 수명, cursor의 Detach·Attach 끝 지점, 준비한 자리의 게시, 공개 진입점 PathDetach·
// PathPrepareAttach. 연산 실행 순서(시작 위치별 해석 포함)는 PropertyPathRunner.cpp다.
namespace rocks {

Guid DetachedValue::FieldId() const noexcept {
    return field_ ? field_->id : Guid{};
}

void DetachedValue::Reset() noexcept {
    if (!storage_) {
        return;
    }

    void* storage = storage_;
    const RuntimeValueOps* ops = ops_;
    storage_ = nullptr;
    ops_ = nullptr;
    enter_ = nullptr;
    field_ = nullptr;
    effectiveFlags_ = 0;
    fromStaged_ = false;
    enumeration_ = nullptr;
    keyEnumeration_ = nullptr;
    target_ = StructView{};
    // 값(옛 값일 수 있다)을 먼저 비운 상자 밖에서 소멸·반환하고 lease를 마지막에 놓는다(소멸 함수의
    // 코드 수명을 lease가 지킨다).
    ops->destroy(storage);
    (void)allocator_->Deallocate(storage);
    lease_.Reset();
}

void DetachedValue::ResetToDefault() noexcept {
    if (!storage_) {
        platform::Fatal("Resetting an empty detached value");
    }

    ops_->destroy(storage_);
    ops_->construct(storage_, *allocator_);
}

void PreparedAttach::Publish(ObjectBatchPublish& batch) noexcept {
    if (!IsReady() || kind_ != PathTarget::Kind::Object || !batch.IsActive() ||
        !(batch.Owner() == owner_) || box_->storage_ != boxStorage_) {
        platform::Fatal("Prepared attach needs the open batch of its owner and its detached value");
    }

    batch.PublishValue(location_, bytes_, inElement_ ? &element_ : nullptr);
    swap_(location_, boxStorage_);
    location_ = nullptr;
}

Result<void> PreparedAttach::Publish() noexcept {
    if (!IsReady()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (kind_ == PathTarget::Kind::Object || box_->storage_ != boxStorage_) {
        platform::Fatal("Prepared attach to a published object needs a batch publish section");
    }

    if (kind_ == PathTarget::Kind::Staged) {
        if (auto open = transaction_->CheckOpen(); !open) {
            return open;
        }
    }

    swap_(location_, boxStorage_);
    location_ = nullptr;
    return Result<void>::Success();
}

namespace detail {

Result<void> PathCursor::DetachValue(const RuntimeValueOps& ops, const void* value,
                                     DetachedPathFunction enter) noexcept {
    if (!ops.copy || !ops.swap) {
        return Fail(ErrorCode::Unsupported, PropertyPathError::NotCopyable);
    }

    // 상자가 필드 스키마의 수명을 지키도록 시작 위치의 lease를 복제해 쥔다.
    const TypeLease& source = ownerType_.IsValid()                ? ownerType_
                              : root_ == PathTarget::Kind::Struct ? *rootLease_
                                                                  : rootBox_->lease_;
    auto lease = source.Clone();
    if (!lease) {
        return Fail(lease.Error());
    }

    // 게시된 객체는 복사의 할당 콜백이 원본을 바꾸지 못하게 고정한다(Read와 같다 — 이미 쥐었다).
    if (auto pinned = Pin(); !pinned) {
        return pinned;
    }

    DetachedValue& box = *box_;
    Allocator& allocator = box.GetAllocator();
    auto memory = allocator.Allocate(ops.size, ops.alignment);
    if (!memory) {
        return Fail(memory.Error());
    }

    void* storage = memory.Value();
    ops.construct(storage, allocator);
    Result<void> copied = ops.copy(storage, value, allocator);
    if (copied) {
        copied = CheckOwner();
    }

    if (!copied) {
        // 실패 기록이 고정을 먼저 푼다 — 사본은 그 뒤에 반환한다.
        Record(copied.Error(), PropertyPathError::None);
        ops.destroy(storage);
        (void)allocator.Deallocate(storage);
        return copied;
    }

    ReleasePin();
    const PathFrame& frame = Frame();
    box.storage_ = storage;
    box.ops_ = &ops;
    box.enter_ = enter;
    box.lease_ = Move(lease.Value());
    box.field_ = frame.field;
    box.effectiveFlags_ = frame.effectiveFlags;
    box.fromStaged_ = AcceptsReadOnlyWrites();
    box.enumeration_ = frame.enumeration;
    box.keyEnumeration_ = frame.keyEnumeration;
    box.target_ = frame.target;
    return Result<void>::Success();
}

Result<void> PathCursor::AttachValue(const RuntimeValueOps& ops, void* value) noexcept {
    const DetachedValue& box = *box_;
    if (box.ops_->nativeType != ops.nativeType || box.ops_->size != ops.size) {
        return Fail(ErrorCode::TypeMismatch, PropertyPathError::StaticTypeMismatch);
    }

    if (!ops.swap) {
        return Fail(ErrorCode::Unsupported, PropertyPathError::NotCopyable);
    }

    PreparedAttach& out = *attach_;
    out.location_ = value;
    out.swap_ = ops.swap;
    out.bytes_ = ops.size;
    out.box_ = box_;
    out.boxStorage_ = box.storage_;
    out.owner_ = owner_;
    out.kind_ = root_;
    out.transaction_ = construction_;
    out.inElement_ = inElement_;
    out.element_ = inElement_ ? element_ : StructElementLocation{};
    return Result<void>::Success();
}

} // namespace detail

Result<void> PathDetach(ObjectRegistry& registry, const ObjectAccessContext& access,
                        const PathTarget& target, PropertyPath path, DetachedValue& box,
                        PropertyPathFailure* failure) noexcept {
    detail::PathCursor cursor = detail::PathRunner::Make(
        registry, access, target, detail::PathOperation::Detach, nullptr, nullptr, failure);
    detail::PathRunner::BindBox(cursor, box, nullptr);
    return detail::PathRunner::Run(cursor, path);
}

Result<void> PathPrepareAttach(ObjectRegistry& registry, const ObjectAccessContext& access,
                               const PathTarget& target, PropertyPath path, DetachedValue& box,
                               PreparedAttach& out, PropertyPathFailure* failure) noexcept {
    detail::PathCursor cursor = detail::PathRunner::Make(
        registry, access, target, detail::PathOperation::Attach, nullptr, nullptr, failure);
    detail::PathRunner::BindBox(cursor, box, &out);
    return detail::PathRunner::Run(cursor, path);
}

} // namespace rocks
