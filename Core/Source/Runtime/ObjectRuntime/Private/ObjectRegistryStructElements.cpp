#include "ObjectRegistryInternal.h"

#include <Object/ObjectRegistry.h>

// ObjectRegistry의 StructRefArray 원소 편집이다. live 변경(InsertStructElement·
// RemoveStructElement·ClearStructElements)과 생성 트랜잭션의 staged 지정(AssignStructElements)을
// 둔다. owner 검증은 ObjectRegistryEdges.cpp, 색인 용량·반환 도우미는 ObjectRegistryInternal.h다.
namespace rocks {

using namespace registry_detail;

namespace {

constexpr uint32 StructRefElementTag = 0x53524145u; // 'SRAE'
constexpr uint32 StructRefIndexTag = 0x53524149u;   // 'SRAI'

// StructRefArray 원소 하나를 소멸하고 블록을 반환한다.
void DestroyStructElement(Allocator& allocator, const detail::StructElementOps& ops,
                          void* block) noexcept {
    ops.destroy(block);
    ReleaseContainerMemory(allocator, block);
}

} // namespace

Result<usize> ObjectRegistry::InsertStructElement(const ObjectAccessContext& access,
                                                  ObjectHandle owner,
                                                  detail::StructRefArrayStorage& storage,
                                                  usize index, bool append) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<usize>::Failure(valid.Error());
    }

    Allocator& allocator = *storage.allocator;
    const detail::StructElementOps& ops = *storage.element;
    auto validateLocked = [&]() noexcept -> Result<void> {
        if (processing_ || closed_) {
            return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
        }

        return CheckContainerOwnerLocked(owner, &storage, sizeof(storage), 0, false);
    };

    uint32 grown = 0;
    {
        platform::LockGuard lock(gate_);
        auto checked = validateLocked();
        if (!checked) {
            return Result<usize>::Failure(checked.Error());
        }

        const usize position = append ? storage.size : index;
        if (position > storage.size) {
            return Result<usize>::Failure(ErrorCode::InvalidArgument);
        }

        if (storage.size == NumericLimits<uint32>::Max()) {
            return Result<usize>::Failure(ErrorCode::Overflow);
        }

        if (storage.size == storage.capacity) {
            grown = GrownCapacity(storage.capacity);
        }
    }

    // 잠금 밖: 원소 블록 할당·기본 생성(참조는 null이라 장벽이 필요 없다)·필요하면 새 색인 할당.
    // 생성자·allocator 콜백이 같은 배열을 바꿀 수 있으므로 게시는 다시 잠근 뒤 그때 상태로 한다.
    auto memory = allocator.Allocate(ops.size, ops.alignment, StructRefElementTag);
    if (!memory) {
        return Result<usize>::Failure(memory.Error());
    }

    void* block = memory.Value();
    ops.construct(block, allocator);
    void** fresh = nullptr;
    if (grown != 0) {
        auto table = allocator.Allocate(sizeof(void*) * grown, alignof(void*), StructRefIndexTag);
        if (!table) {
            DestroyStructElement(allocator, ops, block);
            return Result<usize>::Failure(table.Error());
        }

        fresh = static_cast<void**>(table.Value());
    }

    void** release = fresh;
    bool published = false;
    usize position = 0;
    Result<void> result = Result<void>::Success();
    {
        platform::LockGuard lock(gate_);
        result = validateLocked();
        if (result) {
            position = append ? storage.size : index;
            void** target = nullptr;
            if (position > storage.size) {
                result = Result<void>::Failure(ErrorCode::Busy);
            } else if (storage.size == NumericLimits<uint32>::Max()) {
                result = Result<void>::Failure(ErrorCode::Overflow);
            } else if (storage.size < storage.capacity) {
                target = storage.blocks;
            } else if (fresh != nullptr && grown > storage.size) {
                target = fresh;
            } else {
                // 새 색인이 없거나(그 사이 용량이 찼다) 모자라다.
                result = Result<void>::Failure(ErrorCode::Busy);
            }

            if (target != nullptr) {
                if (target != storage.blocks) {
                    for (usize at = 0; at < position; ++at) {
                        target[at] = storage.blocks[at];
                    }

                    for (usize at = storage.size; at > position; --at) {
                        target[at] = storage.blocks[at - 1];
                    }

                    release = storage.blocks;
                    storage.blocks = target;
                    storage.capacity = grown;
                } else {
                    for (usize at = storage.size; at > position; --at) {
                        target[at] = target[at - 1];
                    }
                }

                target[position] = block;
                ++storage.size;
                ++storage.revision;
                published = true;
            }
        }
    }

    ReleaseContainerMemory(allocator, release);
    if (!published) {
        DestroyStructElement(allocator, ops, block);
        return Result<usize>::Failure(result.Error());
    }

    return Result<usize>::Success(position);
}

Result<void> ObjectRegistry::RemoveStructElement(const ObjectAccessContext& access,
                                                 ObjectHandle owner,
                                                 detail::StructRefArrayStorage& storage,
                                                 usize index) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    Allocator& allocator = *storage.allocator;
    const detail::StructElementOps& ops = *storage.element;
    void* block = nullptr;
    // 삭제 장벽은 필요 없다(삽입 장벽과 Sweeping cutoff가 지킨다). 색인에서 먼저 떼므로 소멸 중
    // 재진입이 이 원소를 다시 건드릴 수 없다.
    auto detachLocked = [&]() noexcept {
        block = storage.blocks[index];
        for (usize position = index + 1; position < storage.size; ++position) {
            storage.blocks[position - 1] = storage.blocks[position];
        }

        --storage.size;
        ++storage.revision;
    };
    ObjectHandle package;
    bool observe = false;
    bool member = false;
    {
        platform::LockGuard lock(gate_);
        if (processing_ || closed_) {
            return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
        }

        auto checked = CheckContainerOwnerLocked(owner, &storage, sizeof(storage), 0, true);
        if (!checked) {
            return checked;
        }

        if (index >= storage.size) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const Slot& source = *FindSlot(owner);
        observe = ObservesOwnerLocked(source);
        member = IsClusterMemberLocked(source);
        if (!observe && !member) {
            detachLocked();
        } else {
            // 다중 간선 지점: 떼어 낼 원소만 처리 표시 아래 잠금 밖에서 trace해 합친다(관찰자 쌍과
            // 클러스터 장부 — 13.4절).
            package = observe ? HandleOf(*source.outermost) : ObjectHandle{};
            processing_ = true;
        }
    }

    if (block == nullptr) {
        EdgeTally tally(allocator_);
        Array<ObjectHandle> removed(allocator_);
        void* const target = storage.blocks[index];
        if (auto collected = CollectStructEdges(ops, &target, 1, removed); !collected) {
            return EndProcessing(Move(collected));
        }

        if (observe) {
            if (auto tallied = TallyHandles(removed.Data(), removed.Size(), package, tally);
                !tallied) {
                return EndProcessing(Move(tallied));
            }

            if (auto reserved = ReserveTally(tally, ReferenceChangeCause::Edit, 0); !reserved) {
                return EndProcessing(Move(reserved));
            }
        }

        platform::LockGuard lock(gate_);
        processing_ = false;
        if (observe &&
            MissingKeysLocked(tally, ReferenceChangeCause::Edit, 0) > ObserverFreeLocked()) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        LedgerClearLocked(*FindSlot(owner), removed.Data(), removed.Size());
        detachLocked();
        if (observe) {
            ApplyTallyLocked(tally, ReferenceChangeCause::Edit, 0);
        }
    }

    DestroyStructElement(allocator, ops, block);
    return Result<void>::Success();
}

Result<void> ObjectRegistry::ClearStructElements(const ObjectAccessContext& access,
                                                 ObjectHandle owner,
                                                 detail::StructRefArrayStorage& storage) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    Allocator& allocator = *storage.allocator;
    const detail::StructElementOps& ops = *storage.element;
    void** blocks = nullptr;
    uint32 size = 0;
    // 색인째 떼어 낸다(잠금 밖에서 소멸할 원소를 담으려고 할당하지 않는다).
    auto detachLocked = [&]() noexcept {
        blocks = storage.blocks;
        size = storage.size;
        storage.blocks = nullptr;
        storage.size = 0;
        ++storage.revision;
        storage.capacity = 0;
    };
    bool observe = false;
    bool member = false;
    ObjectHandle package;
    {
        platform::LockGuard lock(gate_);
        if (processing_ || closed_) {
            return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
        }

        auto checked = CheckContainerOwnerLocked(owner, &storage, sizeof(storage), 0, true);
        if (!checked) {
            return checked;
        }

        const Slot& source = *FindSlot(owner);
        observe = ObservesOwnerLocked(source) && storage.size != 0;
        member = IsClusterMemberLocked(source) && storage.size != 0;
        if (!observe && !member) {
            detachLocked();
        } else {
            // 다중 간선 지점: 떼어 낼 원소를 처리 표시 아래 잠금 밖에서 trace해 합친다(관찰자 쌍과
            // 클러스터 장부 — 13.4절).
            package = observe ? HandleOf(*source.outermost) : ObjectHandle{};
            processing_ = true;
        }
    }

    if (observe || member) {
        EdgeTally tally(allocator_);
        Array<ObjectHandle> removed(allocator_);
        if (auto collected = CollectStructEdges(ops, storage.blocks, storage.size, removed);
            !collected) {
            return EndProcessing(Move(collected));
        }

        if (observe) {
            if (auto tallied = TallyHandles(removed.Data(), removed.Size(), package, tally);
                !tallied) {
                return EndProcessing(Move(tallied));
            }

            if (auto reserved = ReserveTally(tally, ReferenceChangeCause::Edit, 0); !reserved) {
                return EndProcessing(Move(reserved));
            }
        }

        platform::LockGuard lock(gate_);
        processing_ = false;
        if (observe &&
            MissingKeysLocked(tally, ReferenceChangeCause::Edit, 0) > ObserverFreeLocked()) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        LedgerClearLocked(*FindSlot(owner), removed.Data(), removed.Size());
        detachLocked();
        if (observe) {
            ApplyTallyLocked(tally, ReferenceChangeCause::Edit, 0);
        }
    }

    for (uint32 remaining = size; remaining != 0; --remaining) {
        DestroyStructElement(allocator, ops, blocks[remaining - 1]);
    }

    ReleaseContainerMemory(allocator, blocks);
    return Result<void>::Success();
}

Result<void> ObjectRegistry::AssignStructElements(const ObjectAccessContext& access,
                                                  ObjectHandle owner,
                                                  detail::StructRefArrayStorage& storage,
                                                  usize count, uint64 transaction) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    if (count > NumericLimits<uint32>::Max()) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    Allocator& allocator = *storage.allocator;
    const detail::StructElementOps& ops = *storage.element;
    {
        platform::LockGuard lock(gate_);
        if (closed_ || processing_ || transaction == 0) {
            return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
        }

        auto checked =
            CheckContainerOwnerLocked(owner, &storage, sizeof(storage), transaction, false);
        if (!checked) {
            return checked;
        }

        // 블록 할당·원소 생성·옛 원소 소멸은 처리 표시 아래 잠금 밖에서 한다(CreatePending과 같은
        // 규칙 — 그동안의 Registry 재진입은 Busy, Rollback 재진입은 Fatal).
        processing_ = true;
    }

    auto finish = [this]() noexcept {
        platform::LockGuard lock(gate_);
        processing_ = false;
    };
    void** fresh = nullptr;
    usize built = 0;
    auto discard = [&]() noexcept {
        for (usize remaining = built; remaining != 0; --remaining) {
            DestroyStructElement(allocator, ops, fresh[remaining - 1]);
        }

        ReleaseContainerMemory(allocator, fresh);
    };
    if (count != 0) {
        auto table = allocator.Allocate(sizeof(void*) * count, alignof(void*), StructRefIndexTag);
        if (!table) {
            finish();
            return Result<void>::Failure(table.Error());
        }

        fresh = static_cast<void**>(table.Value());
    }

    for (; built < count; ++built) {
        auto memory = allocator.Allocate(ops.size, ops.alignment, StructRefElementTag);
        if (!memory) {
            discard();
            finish();
            return Result<void>::Failure(memory.Error());
        }

        ops.construct(memory.Value(), allocator);
        fresh[built] = memory.Value();
    }

    void** previous = nullptr;
    uint32 previousSize = 0;
    Result<void> result = Result<void>::Success();
    {
        platform::LockGuard lock(gate_);
        result = CheckContainerOwnerLocked(owner, &storage, sizeof(storage), transaction, false);
        if (result) {
            previous = storage.blocks;
            previousSize = storage.size;
            storage.blocks = fresh;
            storage.size = static_cast<uint32>(count);
            ++storage.revision;
            storage.capacity = static_cast<uint32>(count);
        }
    }

    if (!result) {
        discard();
        finish();
        return result;
    }

    // 옛 원소는 게시 뒤 역순으로 소멸한다.
    for (uint32 remaining = previousSize; remaining != 0; --remaining) {
        DestroyStructElement(allocator, ops, previous[remaining - 1]);
    }

    ReleaseContainerMemory(allocator, previous);
    finish();
    return Result<void>::Success();
}

} // namespace rocks
