#include "ObjectRegistryInternal.h"
#include "PropertyPathInternal.h"

#include <Object/ConstructionContext.h>
#include <Object/PreparedStructElements.h>

// StructRefArray 원소 지우기·추가 준비물(리플렉션 B9i3c4·B9i5 — Object/PreparedStructElements.h)의
// index 표와 추가 블록, 경로 cursor의 묶기와 추가 원소 참조 쓰기, 묶음 게시 구간 안의 색인 다시
// 짜기와 staged 다시 짜기, 떼어 낸 블록의 소멸이다.
namespace rocks {

using namespace registry_detail;

namespace {

// 추가 원소 블록·새 색인의 할당 tag다(Registry의 StructRefArray 편집과 같은 값).
constexpr uint32 AddedElementTag = 0x53524145u; // 'SRAE'
constexpr uint32 AddedIndexTag = 0x53524149u;   // 'SRAI'

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

    return original < original_ + added_.Size();
}

Result<void> PreparedStructElements::RemoveAt(usize index) noexcept {
    if (!IsReady() || index >= kept_.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const bool added = kept_[index] >= original_;
    if (auto removed = kept_.TryRemoveAt(index); !removed) {
        return removed;
    }

    addedKept_ -= added ? 1 : 0;
    return Result<void>::Success();
}

Result<void> PreparedStructElements::Clear() noexcept {
    if (!IsReady()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    kept_.Clear();
    addedKept_ = 0;
    return Result<void>::Success();
}

Result<uint32> PreparedStructElements::Append() noexcept {
    if (!IsReady()) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    // id와 게시 뒤 원소 수가 모두 uint32 안이어야 한다.
    const usize next = original_ + added_.Size();
    if (next >= NumericLimits<uint32>::Max()) {
        return Result<uint32>::Failure(ErrorCode::Overflow);
    }

    if (auto reserved = added_.TryReserve(added_.Size() + 1); !reserved) {
        return Result<uint32>::Failure(reserved.Error());
    }

    if (auto reserved = kept_.TryReserve(kept_.Size() + 1); !reserved) {
        return Result<uint32>::Failure(reserved.Error());
    }

    auto memory = elementAllocator_->Allocate(element_->size, element_->alignment, AddedElementTag);
    if (!memory) {
        return Result<uint32>::Failure(memory.Error());
    }

    // 기본 원소의 참조는 null이다(간선·장벽이 없다).
    element_->construct(memory.Value(), *elementAllocator_);
    (void)added_.TryPushBack(memory.Value());
    (void)kept_.TryPushBack(static_cast<uint32>(next));
    ++addedKept_;
    return Result<uint32>::Success(static_cast<uint32>(next));
}

Result<PathTarget> PreparedStructElements::AddedTarget(uint32 id) noexcept {
    if (!IsAdded(id) || IsRemoved(id) || !elementType_.IsValid()) {
        return Result<PathTarget>::Failure(ErrorCode::InvalidArgument);
    }

    const PathTarget start = PathTarget::Value(elementType_, elementType_.Struct(),
                                               added_[id - original_], *elementAllocator_);
    return Result<PathTarget>::Success(
        start.WithAddedElement(*this, kind_ == PathTarget::Kind::Staged ? transaction_ : nullptr));
}

Result<void> PreparedStructElements::PreparePublish() noexcept {
    if (!IsReady()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const usize removedCount = original_ + added_.Size() - kept_.Size();
    if (auto reserved = removed_.TryReserve(removed_.Size() + removedCount); !reserved) {
        return reserved;
    }

    // 게시 뒤 원소 수가 용량을 넘으면 새 색인을 잡는다(배열 allocator — 게시가 옛 색인과
    // 맞바꾼다). 용량은 Registry의 성장 규칙을 따르되 모자라면 원소 수다.
    ReleaseFresh();
    preparedCapacity_ = storage_->capacity;
    if (kept_.Size() <= preparedCapacity_) {
        return Result<void>::Success();
    }

    const uint32 grown = GrownCapacity(preparedCapacity_);
    const uint32 capacity = grown < kept_.Size() ? static_cast<uint32>(kept_.Size()) : grown;
    auto table =
        elementAllocator_->Allocate(sizeof(void*) * capacity, alignof(void*), AddedIndexTag);
    if (!table) {
        return Result<void>::Failure(table.Error());
    }

    fresh_ = static_cast<void**>(table.Value());
    freshCapacity_ = capacity;
    return Result<void>::Success();
}

void PreparedStructElements::ReleaseFresh() noexcept {
    if (fresh_ != nullptr) {
        ReleaseContainerMemory(*elementAllocator_, fresh_);
        fresh_ = nullptr;
        freshCapacity_ = 0;
    }
}

void PreparedStructElements::NoteRemoved(void* registry, ObjectHandle handle) noexcept {
    static_cast<ObjectRegistry*>(registry)->NoteBatchRemoved(handle);
}

void PreparedStructElements::NoteAdded(void* registry, ObjectHandle handle) noexcept {
    // 블록의 handle은 바꾸지 않는다(null은 쓸 때 정규화했다).
    ObjectHandle target = handle;
    static_cast<ObjectRegistry*>(registry)->PublishBatchTarget(target, nullptr);
}

Result<void> PreparedStructElements::CollectReferences() noexcept {
    references_.Clear();
    addedReferences_.Clear();
    const auto trace = elementType_.IsValid() ? elementType_.StructOps().trace : nullptr;
    if (!trace || !HasChanges()) {
        return Result<void>::Success();
    }

    // 지운 원소(묶을 때 있던 것)와 남는 추가 원소를 따로 모은다. 먼저 세어 용량을 잡고, 다시 돌며
    // 담는다(담는 차례는 할당하지 않는다).
    ReferenceCollector removed{&references_, 0};
    ReferenceCollector added{&addedReferences_, 0};
    const TraceVisitor::VisitFn passes[] = {&CountReference, &AppendReference};
    for (const TraceVisitor::VisitFn pass : passes) {
        TraceVisitor removedVisitor(&removed, pass);
        usize next = 0;
        for (uint32 index = 0; index < original_; ++index) {
            if (next < kept_.Size() && kept_[next] == index) {
                ++next;
                continue;
            }

            trace(storage_->blocks[index], removedVisitor);
        }

        TraceVisitor addedVisitor(&added, pass);
        for (; next < kept_.Size(); ++next) {
            trace(added_[kept_[next] - original_], addedVisitor);
        }

        if (pass == &CountReference) {
            if (auto reserved = references_.TryReserve(removed.count); !reserved) {
                return reserved;
            }

            if (auto reserved = addedReferences_.TryReserve(added.count); !reserved) {
                references_.Clear();
                return reserved;
            }
        }
    }

    return Result<void>::Success();
}

void PreparedStructElements::Compact(ObjectRegistry* registry) noexcept {
    const usize count = kept_.Size();
    const usize removedCount = original_ + added_.Size() - count;
    if (removed_.Capacity() - removed_.Size() < removedCount ||
        (count > storage_->capacity && (fresh_ == nullptr || freshCapacity_ < count))) {
        platform::Fatal("Prepared struct elements need PreparePublish before publishing");
    }

    const auto trace = elementType_.IsValid() ? elementType_.StructOps().trace : nullptr;
    void** blocks = storage_->blocks;
    usize next = 0;
    for (uint32 index = 0; index < original_; ++index) {
        if (next < count && kept_[next] == index) {
            ++next;
            continue;
        }

        if (registry != nullptr && trace != nullptr) {
            TraceVisitor visitor(registry, &PreparedStructElements::NoteRemoved);
            trace(blocks[index], visitor);
        }

        (void)removed_.TryPushBack(blocks[index]);
    }

    // 지운 추가 원소는 배열에 들어간 적이 없으므로 소멸만 한다(간선이 없다).
    for (usize slot = 0; slot < added_.Size(); ++slot) {
        if (IsRemoved(static_cast<uint32>(original_ + slot))) {
            (void)removed_.TryPushBack(added_[slot]);
        }
    }

    // kept_는 오름차순이고 묶을 때 원소는 kept_[k] ≥ k라 앞에서부터 당겨도 아직 읽을 칸을 덮지
    // 않는다. 추가 원소는 그 뒤에 오고 블록을 added_에서 읽는다.
    void** target = count > storage_->capacity ? fresh_ : blocks;
    for (usize index = 0; index < count; ++index) {
        const uint32 id = kept_[index];
        void* block = id < original_ ? blocks[id] : added_[id - original_];
        if (id >= original_ && registry != nullptr && trace != nullptr) {
            TraceVisitor visitor(registry, &PreparedStructElements::NoteAdded);
            trace(block, visitor);
        }

        target[index] = block;
        kept_[index] = static_cast<uint32>(index);
    }

    if (target != blocks) {
        retired_ = blocks;
        storage_->blocks = target;
        storage_->capacity = freshCapacity_;
        fresh_ = nullptr;
        freshCapacity_ = 0;
    }

    storage_->size = static_cast<uint32>(count);
    ++storage_->revision;
    // 게시한 뒤에는 지금 배열이 묶을 때의 배열이고 추가 블록은 배열이 소유한다(다시 게시하면
    // 무동작).
    original_ = count;
    revision_ = storage_->revision;
    added_.Clear();
    addedKept_ = 0;
    references_.Clear();
    addedReferences_.Clear();
}

void PreparedStructElements::Publish(ObjectBatchPublish& batch) noexcept {
    if (!IsReady() || kind_ != PathTarget::Kind::Object || !batch.IsActive() ||
        !(batch.Owner() == owner_)) {
        platform::Fatal("Prepared struct elements need the open batch of their owner");
    }

    if (!HasChanges()) {
        return;
    }

    ObjectRegistry& registry = *registry_;
    // 열린 구간·owner 저장소 안 배열인지 확인한다(아니면 Fatal).
    registry.CheckBatchContainer(storage_, sizeof(*storage_));
    if (storage_->revision != revision_ || storage_->size != original_ ||
        storage_->capacity != preparedCapacity_) {
        platform::Fatal("Batch struct element publish found the array changed since binding");
    }

    Compact(&registry);
    registry.batchPublished_ = true;
}

Result<void> PreparedStructElements::Publish() noexcept {
    if (!IsReady() || kind_ != PathTarget::Kind::Staged || !transaction_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!HasChanges()) {
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

        if (storage_->revision != revision_ || storage_->size != original_ ||
            storage_->capacity != preparedCapacity_) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        // 공개 전 객체는 관찰자 기록이 없다(Commit이 trace로 추가 원소의 참조까지 검증하고 센다).
        // 떼어 낸 원소의 소멸은 처리 표시 아래 잠금 밖에서 한다(AssignElements와 같은 규칙).
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
    if (retired_ != nullptr) {
        ReleaseContainerMemory(*elementAllocator_, retired_);
        retired_ = nullptr;
    }
}

void PreparedStructElements::Reset() noexcept {
    ReleaseRemoved();
    // 게시하지 않은 추가 원소는 이 준비물의 것이다.
    for (usize remaining = added_.Size(); remaining != 0; --remaining) {
        void* block = added_[remaining - 1];
        element_->destroy(block);
        ReleaseContainerMemory(*elementAllocator_, block);
    }

    added_.Clear();
    addedKept_ = 0;
    ReleaseFresh();
    preparedCapacity_ = 0;
    kept_.Clear();
    references_.Clear();
    addedReferences_.Clear();
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
        out.addedReferences_.Clear();
        if (!IsLive() || !out.HasChanges()) {
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
            out.addedReferences_.Clear();
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

Result<void> PathCursor::WriteAddedReferenceAt(ObjectHandle& field,
                                               const void* nativeType) noexcept {
    // 시작 위치 블록이 이 준비물의 추가 블록이고 field가 그 안이어야 한다.
    bool inside = added_ != nullptr && root_ == PathTarget::Kind::Struct && input_ != nullptr &&
                  added_->element_ != nullptr;
    if (inside) {
        bool owned = false;
        for (usize slot = 0; slot < added_->added_.Size() && !owned; ++slot) {
            owned = added_->added_[slot] == rootValue_;
        }

        const uptr block = reinterpret_cast<uptr>(rootValue_);
        const uptr location = reinterpret_cast<uptr>(&field);
        const usize size = added_->element_->size;
        inside = owned && size >= sizeof(ObjectHandle) && location >= block &&
                 location - block <= size - sizeof(ObjectHandle);
    }

    if (!inside) {
        return Fail(ErrorCode::InvalidArgument);
    }

    if (auto checked = CheckWeakTarget(nativeType); !checked) {
        return checked;
    }

    // 비정규 null은 정규 null로 쓴다(비교·trace가 정규 null만 보게).
    field = input_->reference.IsNull() ? ObjectHandle{} : input_->reference;
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
