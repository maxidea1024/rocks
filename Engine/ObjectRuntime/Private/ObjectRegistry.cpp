#include <Object/ObjectRegistry.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include "RegistryIdentity.h"
#include <new>

namespace rocks {

namespace {
constexpr uint32 SlotsPerChunk = 64;
enum class SlotState : uint8 {
    Free,
    Reserved,
    Alive,
    DestroyRequested,
    Retired,
    Reclaiming,
    Exhausted
};
} // namespace

// 슬롯과 객체 저장소를 이동하지 않는다. 슬롯 재사용 시 세대를 올리고 wrap 대신 영구 격리한다.
// TypeLease를 소멸 호출용 지역 변수로 옮긴 뒤에도 진단에는 stableType을 사용한다.
struct ObjectRegistry::Slot {
    uint32 generation = 1;
    uint32 index = 0;
    SlotState state = SlotState::Free;
    Object* object = nullptr;
    void* storage = nullptr;
    TypeLease type;
    TypeId stableType;
    uint32 roots = 0;
    uint32 borrows = 0;
    uint32 backendHolds = 0;
    uint64 transaction = 0;
    uint64 mark = 0;
};

struct ObjectRegistry::Chunk {
    Chunk* next = nullptr;
    Slot slots[SlotsPerChunk];
};

struct ObjectRegistry::MarkState {
    ObjectRegistry* registry;
    Array<ObjectHandle>* work;
    uint64 transaction = 0;
    ErrorCode error = ErrorCode::None;
};

ObjectRegistry::ObjectRegistry(Allocator& allocator, TypeRegistry& types, ContextId context,
                               RegistryOptions options) noexcept
    : allocator_(allocator), types_(types), context_(context), options_(options),
      domain_(detail::NextRegistryDomain()), ownerThread_(platform::CurrentThreadId()) {
}

ObjectRegistry::~ObjectRegistry() {
    auto stopped = Shutdown(OwnerAccess());
    if (!stopped) {
        platform::Fatal("ObjectRegistry destroyed before roots/readers/backend leases drained");
    }
    while (chunks_) {
        Chunk* next = chunks_->next;
        chunks_->~Chunk();
        auto released = allocator_.Deallocate(chunks_);
        if (!released) {
            platform::Fatal("Registry chunk allocator mismatch");
        }
        chunks_ = next;
    }
}

Result<void> ObjectRegistry::CheckAccess(const ObjectAccessContext& access) const noexcept {
    if (platform::CurrentThreadId() != ownerThread_) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }
    if (!(access.context == context_)) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }
    return Result<void>::Success();
}

ObjectRegistry::Slot* ObjectRegistry::SlotAt(uint32 index) const noexcept {
    for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
        const uint32 first = chunk->slots[0].index;
        if (index >= first && index - first < SlotsPerChunk) {
            return &chunk->slots[index - first];
        }
    }
    return nullptr;
}

ObjectRegistry::Slot* ObjectRegistry::FindSlot(ObjectHandle handle) const noexcept {
    if (handle.registry != domain_ || handle.generation == 0 || handle.slot >= options_.maxSlots) {
        return nullptr;
    }
    Slot* slot = SlotAt(handle.slot);
    return slot && slot->generation == handle.generation ? slot : nullptr;
}

Result<void> ObjectRegistry::CheckLive(Slot* slot, const void* nativeType) const noexcept {
    if (!slot || slot->state == SlotState::Free || slot->state == SlotState::Exhausted ||
        slot->state == SlotState::Reserved) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }
    if (slot->state != SlotState::Alive) {
        return Result<void>::Failure(ErrorCode::Destroyed);
    }
    if (nativeType && slot->type.Ops().nativeType != nativeType) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }
    return Result<void>::Success();
}

Result<void> ObjectRegistry::AddRoot(const ObjectAccessContext& access, ObjectHandle handle,
                                     const void* nativeType) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }
    platform::LockGuard lock(gate_);
    if (closed_ || processing_) {
        return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }
    Slot* slot = FindSlot(handle);
    auto live = CheckLive(slot, nativeType);
    if (!live) {
        return live;
    }
    if (slot->roots == 0xffffffffu) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }
    ++slot->roots;
    return Result<void>::Success();
}

Result<void> ObjectRegistry::AddBorrow(const ObjectAccessContext& access, ObjectHandle handle,
                                       const void* nativeType) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }
    platform::LockGuard lock(gate_);
    if (closed_ || processing_) {
        return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }
    Slot* slot = FindSlot(handle);
    auto live = CheckLive(slot, nativeType);
    if (!live) {
        return live;
    }
    if (totalBorrows_ == 0xffffffffu || slot->borrows == 0xffffffffu) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }
    ++slot->borrows;
    ++totalBorrows_;
    return Result<void>::Success();
}

namespace detail {

Result<void> RetainRoot(ObjectRegistry* registry, ObjectHandle handle) noexcept {
    if (!registry) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    platform::LockGuard lock(registry->gate_);
    if (registry->processing_ || registry->closed_) {
        return Result<void>::Failure(registry->closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }
    auto* slot = registry->FindSlot(handle);
    if (!slot || slot->roots == 0 ||
        !(slot->state == SlotState::Alive || slot->state == SlotState::DestroyRequested)) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }
    if (slot->roots == 0xffffffffu) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }
    ++slot->roots;
    return Result<void>::Success();
}

void ReleaseRoot(ObjectRegistry* registry, ObjectHandle handle) noexcept {
    platform::LockGuard lock(registry->gate_);
    auto* slot = registry->FindSlot(handle);
    if (!slot || slot->roots == 0) {
        platform::Fatal("RootRef release after lifetime closure");
    }
    --slot->roots;
}

Result<void*> AccessBorrow(ObjectRegistry* registry, ObjectHandle handle,
                           const void* nativeType) noexcept {
    if (!registry) {
        return Result<void*>::Failure(ErrorCode::InvalidArgument);
    }
    if (platform::CurrentThreadId() != registry->ownerThread_) {
        return Result<void*>::Failure(ErrorCode::WrongThread);
    }
    platform::LockGuard lock(registry->gate_);
    auto* slot = registry->FindSlot(handle);
    auto valid = registry->CheckLive(slot, nativeType);
    if (!valid) {
        return Result<void*>::Failure(valid.Error());
    }
    if (slot->borrows == 0) {
        return Result<void*>::Failure(ErrorCode::StaleHandle);
    }
    return Result<void*>::Success(nativeType ? slot->storage : static_cast<void*>(slot->object));
}

void ReleaseBorrow(ObjectRegistry* registry, ObjectHandle handle) noexcept {
    platform::LockGuard lock(registry->gate_);
    auto* slot = registry->FindSlot(handle);
    if (!slot || slot->borrows == 0 || registry->totalBorrows_ == 0) {
        platform::Fatal("Borrow release after lifetime closure");
    }
    --slot->borrows;
    --registry->totalBorrows_;
}

void ReleaseBackend(ObjectRegistry* registry, ObjectHandle handle) noexcept {
    platform::LockGuard lock(registry->gate_);
    auto* slot = registry->FindSlot(handle);
    if (!slot || slot->backendHolds == 0) {
        platform::Fatal("Backend lease release after lifetime closure");
    }
    --slot->backendHolds;
}

} // namespace detail

Result<void> ObjectRegistry::SetEdge(const ObjectAccessContext& access, ObjectHandle owner,
                                     ObjectHandle& field, ObjectHandle target, uint64 transaction,
                                     const void* nativeType) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }
    platform::LockGuard lock(gate_);
    if (processing_ || closed_) {
        return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }
    Slot* source = FindSlot(owner);
    if (!source || !source->object ||
        (transaction ? source->state != SlotState::Reserved || source->transaction != transaction
                     : source->state != SlotState::Alive)) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }
    const uptr start = reinterpret_cast<uptr>(source->storage);
    const uptr location = reinterpret_cast<uptr>(&field);
    const usize size = source->type.Ops().size;
    if (location < start || location - start > size - sizeof(ObjectHandle)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (!target.IsNull()) {
        Slot* destination = FindSlot(target);
        if (!destination || !(destination->state == SlotState::Alive ||
                              (transaction && destination->state == SlotState::Reserved &&
                               destination->transaction == transaction))) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }
        if (nativeType && destination->type.Ops().nativeType != nativeType) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }
    }
    field = target;
    return Result<void>::Success();
}

Result<void> ObjectRegistry::RequestDestroy(const ObjectAccessContext& access,
                                            ObjectHandle handle) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }
    platform::LockGuard lock(gate_);
    if (processing_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    Slot* slot = FindSlot(handle);
    if (slot && (slot->state == SlotState::DestroyRequested || slot->state == SlotState::Retired)) {
        return Result<void>::Success();
    }
    auto live = CheckLive(slot, nullptr);
    if (!live) {
        return live;
    }
    slot->state = SlotState::DestroyRequested;
    return Result<void>::Success();
}

Result<BackendLease> ObjectRegistry::AcquireBackendLease(const ObjectAccessContext& access,
                                                         ObjectHandle handle) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<BackendLease>::Failure(valid.Error());
    }
    platform::LockGuard lock(gate_);
    if (closed_ || processing_) {
        return Result<BackendLease>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }
    Slot* slot = FindSlot(handle);
    auto live = CheckLive(slot, nullptr);
    if (!live) {
        return Result<BackendLease>::Failure(live.Error());
    }
    if (slot->backendHolds == 0xffffffffu) {
        return Result<BackendLease>::Failure(ErrorCode::Overflow);
    }
    ++slot->backendHolds;
    return Result<BackendLease>::Success(BackendLease(this, handle));
}

Result<uint64> ObjectRegistry::BeginConstruction(const ObjectAccessContext& access) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<uint64>::Failure(valid.Error());
    }
    platform::LockGuard lock(gate_);
    if (closed_ || processing_) {
        return Result<uint64>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }
    if (options_.maxSlots == 0 || options_.maxGeneration == 0) {
        return Result<uint64>::Failure(ErrorCode::InvalidArgument);
    }
    if (nextTransaction_ == 0 || constructions_ == 0xffffffffu) {
        return Result<uint64>::Failure(ErrorCode::Overflow);
    }
    ++constructions_;
    return Result<uint64>::Success(nextTransaction_++);
}

Result<ObjectHandle> ObjectRegistry::CreatePending(const ObjectAccessContext& access, TypeId type,
                                                   uint64 transaction,
                                                   const void* nativeType) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<ObjectHandle>::Failure(valid.Error());
    }
    {
        platform::LockGuard lock(gate_);
        if (closed_ || processing_ || transaction == 0) {
            return Result<ObjectHandle>::Failure(closed_ ? ErrorCode::ShuttingDown
                                                         : ErrorCode::Busy);
        }
        processing_ = true;
    }
    auto finish = [this]() noexcept {
        platform::LockGuard lock(gate_);
        processing_ = false;
    };
    auto typeLease = types_.Acquire(type);
    if (!typeLease) {
        finish();
        return Result<ObjectHandle>::Failure(typeLease.Error());
    }
    if (nativeType && typeLease.Value().Ops().nativeType != nativeType) {
        finish();
        return Result<ObjectHandle>::Failure(ErrorCode::TypeMismatch);
    }
    const auto& ops = typeLease.Value().Ops();
    auto memory = allocator_.Allocate(ops.size, ops.alignment, 0x4f424a45u);
    if (!memory) {
        finish();
        return Result<ObjectHandle>::Failure(memory.Error());
    }

    Slot* freeSlot = nullptr;
    {
        platform::LockGuard lock(gate_);
        for (Chunk* chunk = chunks_; chunk && !freeSlot; chunk = chunk->next) {
            for (auto& slot : chunk->slots) {
                if (slot.index < options_.maxSlots && slot.state == SlotState::Free) {
                    freeSlot = &slot;
                    break;
                }
            }
        }
    }
    if (!freeSlot) {
        if (uint64(chunkCount_) * SlotsPerChunk >= options_.maxSlots) {
            auto released = allocator_.Deallocate(memory.Value());
            if (!released) {
                platform::Fatal("Object allocation ownership mismatch");
            }
            finish();
            return Result<ObjectHandle>::Failure(ErrorCode::CapacityExceeded);
        }
        auto block = allocator_.Allocate(sizeof(Chunk), alignof(Chunk), 0x43484e4bu);
        if (!block) {
            auto released = allocator_.Deallocate(memory.Value());
            if (!released) {
                platform::Fatal("Object allocation ownership mismatch");
            }
            finish();
            return Result<ObjectHandle>::Failure(block.Error());
        }
        Chunk* chunk = ::new (block.Value()) Chunk();
        for (uint32 index = 0; index < SlotsPerChunk; ++index) {
            const uint64 absolute = uint64(chunkCount_) * SlotsPerChunk + index;
            chunk->slots[index].index = static_cast<uint32>(absolute);
            if (absolute >= options_.maxSlots) {
                chunk->slots[index].state = SlotState::Exhausted;
            }
        }
        {
            platform::LockGuard lock(gate_);
            chunk->next = chunks_;
            chunks_ = chunk;
            ++chunkCount_;
        }
        freeSlot = &chunk->slots[0];
    }

    ObjectHandle handle{domain_, freeSlot->index, freeSlot->generation};
    {
        platform::LockGuard lock(gate_);
        freeSlot->state = SlotState::Reserved;
        freeSlot->storage = memory.Value();
        freeSlot->transaction = transaction;
        freeSlot->stableType = type;
        freeSlot->type = Move(typeLease.Value());
    }
    Object* object = freeSlot->type.Ops().construct(memory.Value());
    if (!object) {
        platform::Fatal("A noexcept object constructor must return its object");
    }
    object->handle_ = handle;
    {
        platform::LockGuard lock(gate_);
        freeSlot->object = object;
        processing_ = false;
    }
    return Result<ObjectHandle>::Success(handle);
}

Result<void*> ObjectRegistry::GetPending(const ObjectAccessContext& access, ObjectHandle handle,
                                         uint64 transaction, const void* nativeType) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<void*>::Failure(valid.Error());
    }
    platform::LockGuard lock(gate_);
    Slot* slot = FindSlot(handle);
    if (processing_ || !slot || slot->state != SlotState::Reserved ||
        slot->transaction != transaction || !slot->object) {
        return Result<void*>::Failure(ErrorCode::StaleHandle);
    }
    if (nativeType && slot->type.Ops().nativeType != nativeType) {
        return Result<void*>::Failure(ErrorCode::TypeMismatch);
    }
    return Result<void*>::Success(nativeType ? slot->storage : static_cast<void*>(slot->object));
}

void ObjectRegistry::ValidateStagedEdge(void* pointer, ObjectHandle handle) noexcept {
    auto& state = *static_cast<MarkState*>(pointer);
    if (handle.IsNull() || state.error != ErrorCode::None) {
        return;
    }
    platform::LockGuard lock(state.registry->gate_);
    Slot* slot = state.registry->FindSlot(handle);
    if (!slot || !(slot->state == SlotState::Alive || (slot->state == SlotState::Reserved &&
                                                       slot->transaction == state.transaction))) {
        state.error = ErrorCode::StaleHandle;
    }
}

Result<RootRef<Object>> ObjectRegistry::Commit(const ObjectAccessContext& access,
                                               uint64 transaction,
                                               Span<const ObjectHandle> staged) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<RootRef<Object>>::Failure(valid.Error());
    }
    {
        platform::LockGuard lock(gate_);
        if (closed_ || processing_ || staged.Size() == 0) {
            return Result<RootRef<Object>>::Failure(closed_       ? ErrorCode::ShuttingDown
                                                    : processing_ ? ErrorCode::Busy
                                                                  : ErrorCode::InvalidArgument);
        }
        for (usize index = 0; index < staged.Size(); ++index) {
            Slot* slot = FindSlot(staged[index]);
            if (!slot || slot->state != SlotState::Reserved || slot->transaction != transaction ||
                !slot->object) {
                return Result<RootRef<Object>>::Failure(ErrorCode::StaleHandle);
            }
        }
        processing_ = true;
    }
    MarkState state{this, nullptr, transaction};
    TraceVisitor visitor(&state, &ObjectRegistry::ValidateStagedEdge);
    for (usize index = 0; index < staged.Size() && state.error == ErrorCode::None; ++index) {
        Slot* slot = FindSlot(staged[index]);
        slot->type.Ops().trace(*slot->object, visitor);
    }
    platform::LockGuard lock(gate_);
    processing_ = false;
    if (state.error != ErrorCode::None) {
        return Result<RootRef<Object>>::Failure(state.error);
    }
    for (usize index = 0; index < staged.Size(); ++index) {
        Slot* slot = FindSlot(staged[index]);
        slot->state = SlotState::Alive;
        slot->transaction = 0;
    }
    ++FindSlot(staged[0])->roots;
    --constructions_;
    return Result<RootRef<Object>>::Success(RootRef<Object>(this, staged[0]));
}

void ObjectRegistry::Rollback(uint64 transaction, Span<const ObjectHandle> staged) noexcept {
    if (platform::CurrentThreadId() != ownerThread_) {
        platform::Fatal("Construction rollback on another thread");
    }
    {
        platform::LockGuard lock(gate_);
        if (processing_ || constructions_ == 0) {
            platform::Fatal("Construction rollback while executing a registry callback");
        }
        processing_ = true;
    }
    // 생성 저널을 역순으로 처리해 나중에 만든 객체를 먼저 정리한다.
    // 소멸자는 Registry 잠금 밖에서 호출하되 processing_으로 변경 재진입을 막는다.
    for (usize remaining = staged.Size(); remaining != 0; --remaining) {
        {
            platform::LockGuard lock(gate_);
            Slot* slot = FindSlot(staged[remaining - 1]);
            if (!slot || slot->state != SlotState::Reserved || slot->transaction != transaction) {
                platform::Fatal("Corrupt construction journal");
            }
            slot->state = SlotState::Retired;
        }
        ReclaimEligible();
    }
    {
        platform::LockGuard lock(gate_);
        --constructions_;
        processing_ = false;
    }
}

void ObjectRegistry::MarkEdge(void* pointer, ObjectHandle handle) noexcept {
    auto& state = *static_cast<MarkState*>(pointer);
    if (handle.IsNull() || state.error != ErrorCode::None) {
        return;
    }
    platform::LockGuard lock(state.registry->gate_);
    Slot* slot = state.registry->FindSlot(handle);
    if (!slot || !(slot->state == SlotState::Alive || slot->state == SlotState::DestroyRequested)) {
        state.error = ErrorCode::StaleHandle;
        return;
    }
    if (slot->mark == state.registry->markEpoch_) {
        return;
    }
    auto pushed = state.work->TryPushBack(handle);
    if (!pushed) {
        state.error = pushed.Error();
        return;
    }
    slot->mark = state.registry->markEpoch_;
}

Result<GcReport> ObjectRegistry::Collect(const ObjectAccessContext& access) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<GcReport>::Failure(valid.Error());
    }
    const uint64 started = platform::MonotonicNanoseconds();
    GcReport report;
    {
        platform::LockGuard lock(gate_);
        if (processing_ || constructions_ || totalBorrows_) {
            return Result<GcReport>::Failure(ErrorCode::Busy);
        }
        processing_ = true;
        for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
            for (auto& slot : chunk->slots) {
                if (slot.state == SlotState::Alive || slot.state == SlotState::DestroyRequested) {
                    ++report.examined;
                }
            }
        }
    }
    // 모든 후보를 담을 용량을 먼저 확보한다. OOM이 발생해도 아직 어떤 객체도 retire하지 않았다.
    // 후보당 한 번만 큐에 넣으므로 이후 순회는 추가 할당 없이 깊은 그래프도 처리한다.
    Array<ObjectHandle> work(allocator_);
    auto reserved = work.TryReserve(report.examined);
    if (!reserved) {
        platform::LockGuard lock(gate_);
        processing_ = false;
        return Result<GcReport>::Failure(reserved.Error());
    }
    {
        platform::LockGuard lock(gate_);
        if (++markEpoch_ == 0) {
            markEpoch_ = 1;
            for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
                for (auto& slot : chunk->slots) {
                    slot.mark = 0;
                }
            }
        }
        for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
            for (auto& slot : chunk->slots) {
                if ((slot.state == SlotState::Alive || slot.state == SlotState::DestroyRequested) &&
                    slot.roots != 0) {
                    auto pushed = work.TryPushBack({domain_, slot.index, slot.generation});
                    if (!pushed) {
                        platform::Fatal("Preallocated GC work queue exhausted");
                    }
                    slot.mark = markEpoch_;
                }
            }
        }
    }
    MarkState state{this, &work};
    TraceVisitor visitor(&state, &ObjectRegistry::MarkEdge);
    for (usize cursor = 0; cursor < work.Size() && state.error == ErrorCode::None; ++cursor) {
        Slot* slot = FindSlot(work[cursor]);
        slot->type.Ops().trace(*slot->object, visitor);
    }
    if (state.error != ErrorCode::None) {
        platform::LockGuard lock(gate_);
        processing_ = false;
        return Result<GcReport>::Failure(state.error);
    }
    // 이 지점 이전에는 도달성 판단을 확정하지 않는다. Trace 오류 시 sweep 전체를 생략하고,
    // 다음 시도는 새 epoch를 사용해 부분 표식을 무시한다.
    report.marked = static_cast<uint32>(work.Size());
    {
        platform::LockGuard lock(gate_);
        for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
            for (auto& slot : chunk->slots) {
                if ((slot.state == SlotState::Alive || slot.state == SlotState::DestroyRequested) &&
                    slot.mark != markEpoch_) {
                    slot.state = SlotState::Retired;
                    ++report.retired;
                }
            }
        }
    }
    report.reclaimed = ReclaimEligible();
    report.durationNanoseconds = platform::MonotonicNanoseconds() - started;
    {
        platform::LockGuard lock(gate_);
        lastGc_ = report;
        processing_ = false;
    }
    return Result<GcReport>::Success(report);
}

uint32 ObjectRegistry::ReclaimEligible() noexcept {
    uint32 reclaimed = 0;
    for (;;) {
        Slot* selected = nullptr;
        TypeLease type;
        Object* object = nullptr;
        void* storage = nullptr;
        {
            platform::LockGuard lock(gate_);
            for (Chunk* chunk = chunks_; chunk && !selected; chunk = chunk->next) {
                for (auto& slot : chunk->slots) {
                    if (slot.state == SlotState::Retired && slot.roots == 0 && slot.borrows == 0 &&
                        slot.backendHolds == 0) {
                        selected = &slot;
                        break;
                    }
                }
            }
            if (!selected) {
                break;
            }
            // 저장소를 큐에서 분리한 뒤 잠금을 푼다. 콜백 중 통계 조회는 가능하지만 재회수는
            // 불가하다. 지역 TypeLease가 소멸자 코드까지 살아 있게 하므로 해제 콜백이 끝난 뒤
            // 반환한다.
            selected->state = SlotState::Reclaiming;
            type = Move(selected->type);
            object = selected->object;
            storage = selected->storage;
        }
        // User destructors and allocator callbacks never execute under the registry gate.
        type.Ops().destroy(object);
        auto released = allocator_.Deallocate(storage);
        if (!released) {
            platform::Fatal("Object allocation ownership mismatch");
        }
        {
            platform::LockGuard lock(gate_);
            selected->object = nullptr;
            selected->storage = nullptr;
            selected->transaction = 0;
            selected->stableType = {};
            selected->mark = 0;
            // 세대를 0/과거 값으로 순환시키면 오래된 WeakRef가 새 객체를 가리킬 수 있다.
            // 한도에 도달한 슬롯은 재사용하지 않고 Exhausted로 남긴다.
            if (selected->generation >= options_.maxGeneration) {
                selected->state = SlotState::Exhausted;
            } else {
                ++selected->generation;
                selected->state = SlotState::Free;
            }
        }
        type.Reset();
        ++reclaimed;
    }
    return reclaimed;
}

Result<uint32> ObjectRegistry::DrainRetired(const ObjectAccessContext& access) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<uint32>::Failure(valid.Error());
    }
    {
        platform::LockGuard lock(gate_);
        if (processing_) {
            return Result<uint32>::Failure(ErrorCode::Busy);
        }
        processing_ = true;
    }
    uint32 count = ReclaimEligible();
    {
        platform::LockGuard lock(gate_);
        processing_ = false;
    }
    return Result<uint32>::Success(count);
}

Result<void> ObjectRegistry::Shutdown(const ObjectAccessContext& access) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }
    {
        platform::LockGuard lock(gate_);
        if (processing_) {
            return Result<void>::Failure(ErrorCode::Busy);
        }
        closed_ = true;
    }
    auto collected = Collect(access);
    if (!collected) {
        return Result<void>::Failure(collected.Error());
    }
    auto stats = Stats();
    if (stats.liveObjects || stats.reservedObjects || stats.destroyRequested ||
        stats.retiredObjects || stats.roots || stats.borrows || stats.backendHolds) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    return Result<void>::Success();
}

ObjectStats ObjectRegistry::Stats() const noexcept {
    platform::LockGuard lock(gate_);
    ObjectStats stats;
    stats.chunks = chunkCount_;
    for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
        for (auto& slot : chunk->slots) {
            switch (slot.state) {
            case SlotState::Alive:
                ++stats.liveObjects;
                break;
            case SlotState::Reserved:
                ++stats.reservedObjects;
                break;
            case SlotState::DestroyRequested:
                ++stats.destroyRequested;
                break;
            case SlotState::Retired:
            case SlotState::Reclaiming:
                ++stats.retiredObjects;
                break;
            default:
                break;
            }
            stats.roots += slot.roots;
            stats.borrows += slot.borrows;
            stats.backendHolds += slot.backendHolds;
        }
    }
    return stats;
}

Result<uint32>
ObjectRegistry::CaptureDiagnostics(Span<ObjectDiagnosticRecord> output) const noexcept {
    platform::LockGuard lock(gate_);
    uint32 required = 0;
    for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
        for (auto& slot : chunk->slots) {
            if (slot.state != SlotState::Free && slot.state != SlotState::Exhausted) {
                ++required;
            }
        }
    }
    if (output.Size() < required) {
        return Result<uint32>::Failure(ErrorCode::CapacityExceeded);
    }
    uint32 written = 0;
    for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
        for (auto& slot : chunk->slots) {
            ObjectPhase phase;
            switch (slot.state) {
            case SlotState::Reserved:
                phase = ObjectPhase::Reserved;
                break;
            case SlotState::Alive:
                phase = ObjectPhase::Alive;
                break;
            case SlotState::DestroyRequested:
                phase = ObjectPhase::DestroyRequested;
                break;
            case SlotState::Retired:
                phase = ObjectPhase::Retired;
                break;
            case SlotState::Reclaiming:
                phase = ObjectPhase::Reclaiming;
                break;
            default:
                continue;
            }
            output[written++] = {{domain_, slot.index, slot.generation},
                                 context_,
                                 slot.stableType,
                                 phase,
                                 slot.roots,
                                 slot.borrows,
                                 slot.backendHolds};
        }
    }
    return Result<uint32>::Success(written);
}

GcReport ObjectRegistry::LastGcReport() const noexcept {
    platform::LockGuard lock(gate_);
    return lastGc_;
}

} // namespace rocks
