#include "ObjectRegistryInternal.h"
#include "PropertyPathInternal.h"

#include <Object/ConstructionContext.h>
#include <Object/PreparedStructElements.h>

// StructRefArray 원소 지우기 준비물(리플렉션 B9i3c4 — Object/PreparedStructElements.h)의 index 표,
// 경로 cursor의 묶기, 묶음 게시 구간 안의 압축과 staged 압축, 떼어 낸 블록의 소멸이다.
namespace rocks {

using namespace registry_detail;

namespace {

// 지울 원소의 참조 세기·모으기 상태다(trace 방문 함수가 받는다).
struct ReferenceCollector {
    Array<ObjectHandle>* out = nullptr;
    usize count = 0;
};

void CountReference(void* state, ObjectHandle handle) noexcept {
    if (!handle.IsNull()) {
        ++static_cast<ReferenceCollector*>(state)->count;
    }
}

void AppendReference(void* state, ObjectHandle handle) noexcept {
    if (!handle.IsNull()) {
        // 용량은 센 수만큼 잡았다(할당하지 않는다).
        (void)static_cast<ReferenceCollector*>(state)->out->TryPushBack(handle);
    }
}

} // namespace

bool PreparedStructElements::IsRemoved(uint32 original) const noexcept {
    // kept_는 오름차순이다.
    usize low = 0;
    usize high = kept_.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (kept_[middle] == original) {
            return false;
        }

        if (kept_[middle] < original) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return original < original_;
}

Result<void> PreparedStructElements::RemoveAt(usize index) noexcept {
    if (!IsReady() || index >= kept_.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return kept_.TryRemoveAt(index);
}

Result<void> PreparedStructElements::Clear() noexcept {
    if (!IsReady()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    kept_.Clear();
    return Result<void>::Success();
}

Result<void> PreparedStructElements::PreparePublish() noexcept {
    if (!IsReady()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return removed_.TryReserve(removed_.Size() + (original_ - kept_.Size()));
}

void PreparedStructElements::NoteRemoved(void* registry, ObjectHandle handle) noexcept {
    static_cast<ObjectRegistry*>(registry)->NoteBatchRemoved(handle);
}

Result<void> PreparedStructElements::CollectReferences() noexcept {
    references_.Clear();
    const auto trace = elementType_.IsValid() ? elementType_.StructOps().trace : nullptr;
    if (!trace || !HasRemovals()) {
        return Result<void>::Success();
    }

    // 먼저 세어 용량을 잡고, 다시 돌며 담는다(담는 차례는 할당하지 않는다).
    ReferenceCollector collector{&references_, 0};
    const TraceVisitor::VisitFn passes[] = {&CountReference, &AppendReference};
    for (const TraceVisitor::VisitFn pass : passes) {
        TraceVisitor visitor(&collector, pass);
        usize next = 0;
        for (uint32 index = 0; index < original_; ++index) {
            if (next < kept_.Size() && kept_[next] == index) {
                ++next;
                continue;
            }

            trace(storage_->blocks[index], visitor);
        }

        if (pass == &CountReference) {
            if (auto reserved = references_.TryReserve(collector.count); !reserved) {
                return reserved;
            }
        }
    }

    return Result<void>::Success();
}

void PreparedStructElements::Compact(ObjectRegistry* registry) noexcept {
    const usize removedCount = original_ - kept_.Size();
    if (removed_.Capacity() - removed_.Size() < removedCount) {
        platform::Fatal("Prepared struct elements need PreparePublish before publishing");
    }

    const auto trace = elementType_.IsValid() ? elementType_.StructOps().trace : nullptr;
    void** blocks = storage_->blocks;
    usize next = 0;
    for (uint32 index = 0; index < original_; ++index) {
        if (next < kept_.Size() && kept_[next] == index) {
            ++next;
            continue;
        }

        if (registry != nullptr && trace != nullptr) {
            TraceVisitor visitor(registry, &PreparedStructElements::NoteRemoved);
            trace(blocks[index], visitor);
        }

        (void)removed_.TryPushBack(blocks[index]);
    }

    // kept_는 오름차순이고 kept_[k] ≥ k라 앞에서부터 당겨도 아직 읽을 칸을 덮지 않는다.
    for (usize index = 0; index < kept_.Size(); ++index) {
        blocks[index] = blocks[kept_[index]];
        kept_[index] = static_cast<uint32>(index);
    }

    storage_->size = static_cast<uint32>(kept_.Size());
    ++storage_->revision;
    // 게시한 뒤에는 지금 배열이 묶을 때의 배열이다(다시 게시하면 무동작).
    original_ = kept_.Size();
    revision_ = storage_->revision;
    references_.Clear();
}

void PreparedStructElements::Publish(ObjectBatchPublish& batch) noexcept {
    if (!IsReady() || kind_ != PathTarget::Kind::Object || !batch.IsActive() ||
        !(batch.Owner() == owner_)) {
        platform::Fatal("Prepared struct elements need the open batch of their owner");
    }

    if (!HasRemovals()) {
        return;
    }

    ObjectRegistry& registry = *registry_;
    // 열린 구간·owner 저장소 안 배열인지 확인한다(아니면 Fatal).
    registry.CheckBatchContainer(storage_, sizeof(*storage_));
    if (storage_->revision != revision_ || storage_->size != original_) {
        platform::Fatal("Batch struct element removal found the array changed since binding");
    }

    Compact(&registry);
    registry.batchPublished_ = true;
}

Result<void> PreparedStructElements::Publish() noexcept {
    if (!IsReady() || kind_ != PathTarget::Kind::Staged || !transaction_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!HasRemovals()) {
        return Result<void>::Success();
    }

    ConstructionContext& transaction = *transaction_;
    if (auto writable = transaction.CheckStagedWrite(); !writable) {
        return writable;
    }

    if (auto prepared = PreparePublish(); !prepared) {
        return prepared;
    }

    ObjectRegistry& registry = transaction.registry_;
    if (auto valid = registry.CheckAccess(transaction.access_); !valid) {
        return valid;
    }

    {
        platform::LockGuard lock(registry.gate_);
        if (registry.closed_ || registry.processing_) {
            return Result<void>::Failure(registry.closed_ ? ErrorCode::ShuttingDown
                                                          : ErrorCode::Busy);
        }

        auto checked = registry.CheckContainerOwnerLocked(owner_, storage_, sizeof(*storage_),
                                                          transaction.transaction_, false);
        if (!checked) {
            return checked;
        }

        if (storage_->revision != revision_ || storage_->size != original_) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        // 공개 전 객체는 관찰자 기록이 없다(Commit이 trace로 센다). 떼어 낸 원소의 소멸은 처리
        // 표시 아래 잠금 밖에서 한다(AssignElements와 같은 규칙).
        Compact(nullptr);
        registry.processing_ = true;
    }

    ReleaseRemoved();
    platform::LockGuard lock(registry.gate_);
    registry.processing_ = false;
    return Result<void>::Success();
}

void PreparedStructElements::ReleaseRemoved() noexcept {
    for (usize remaining = removed_.Size(); remaining != 0; --remaining) {
        void* block = removed_[remaining - 1];
        element_->destroy(block);
        ReleaseContainerMemory(*elementAllocator_, block);
    }

    removed_.Clear();
}

void PreparedStructElements::Reset() noexcept {
    ReleaseRemoved();
    kept_.Clear();
    references_.Clear();
    elementType_ = TypeLease{};
    storage_ = nullptr;
    elementAllocator_ = nullptr;
    element_ = nullptr;
    registry_ = nullptr;
    owner_ = {};
    kind_ = PathTarget::Kind::Object;
    transaction_ = nullptr;
    original_ = 0;
    revision_ = 0;
}

namespace detail {

Result<void> PathCursor::BindStructElements(StructRefArrayStorage& storage) noexcept {
    PreparedStructElements& out = *elements_;
    if (out.IsReady()) {
        if (out.storage_ != &storage || !(out.owner_ == owner_) || out.kind_ != root_) {
            return Fail(ErrorCode::InvalidArgument, PropertyPathError::InvalidValue);
        }

        if (storage.revision != out.revision_ || storage.size != out.original_) {
            return Fail(ErrorCode::Busy);
        }

        out.references_.Clear();
        if (!IsLive() || !out.HasRemovals()) {
            return Result<void>::Success();
        }

        // 게시된 객체는 모으기의 할당 콜백이 원소를 바꾸지 못하게 고정한다.
        Result<void> collected = Pin();
        if (collected) {
            collected = out.CollectReferences();
        }

        if (collected) {
            collected = CheckOwner();
        }

        if (!collected) {
            Record(collected.Error(), PropertyPathError::None);
            out.references_.Clear();
        }

        return collected;
    }

    Result<void> bound = Pin();
    if (bound) {
        bound = out.kept_.TryReserve(storage.size);
    }

    if (bound) {
        for (uint32 index = 0; index < storage.size; ++index) {
            (void)out.kept_.TryPushBack(index);
        }

        bound = CheckOwner();
    }

    if (!bound) {
        Record(bound.Error(), PropertyPathError::None);
        out.kept_.Clear();
        return bound;
    }

    out.storage_ = &storage;
    out.elementAllocator_ = storage.allocator;
    out.element_ = storage.element;
    out.registry_ = &registry_;
    out.owner_ = owner_;
    out.kind_ = root_;
    out.transaction_ = construction_;
    out.original_ = storage.size;
    out.revision_ = storage.revision;
    return Result<void>::Success();
}

} // namespace detail

Result<void> PathPrepareStructElements(ObjectRegistry& registry, const ObjectAccessContext& access,
                                       const PathTarget& target, PropertyPath path,
                                       PreparedStructElements& out,
                                       PropertyPathFailure* failure) noexcept {
    const bool first = !out.IsReady();
    detail::PathCursor cursor = detail::PathRunner::Make(
        registry, access, target, detail::PathOperation::Length, nullptr, nullptr, failure);
    detail::PathRunner::BindElements(cursor, out);
    if (auto ran = detail::PathRunner::Run(cursor, path); !ran) {
        return ran;
    }

    // 끝이 값 struct 배열이면 thunk가 원소 수만 읽고 묶지 않는다.
    if (!out.IsReady()) {
        if (failure) {
            failure->reason = PropertyPathError::InvalidValue;
            failure->step = path.steps.IsEmpty() ? 0 : path.steps.Size() - 1;
        }

        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (first) {
        // 지울 원소의 trace는 등록 struct의 실행 정보로 얻는다(StructRefArray.h의 원소 표 규칙).
        auto lease = registry.Types().AcquireStructByNative(out.element_->nativeType);
        if (!lease) {
            out.Reset();
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        out.elementType_ = Move(lease.Value());
    }

    return Result<void>::Success();
}

} // namespace rocks
