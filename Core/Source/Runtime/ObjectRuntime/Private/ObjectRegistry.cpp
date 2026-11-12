#include <Object/ObjectRegistry.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Task/WorkerScheduler.h>
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
      domain_(detail::NextRegistryDomain()), ownerThread_(platform::CurrentThreadId()),
      gcWork_(allocator) {
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
    if (slot->state != SlotState::Alive || IsCutoff(slot)) {
        return Result<void>::Failure(ErrorCode::Destroyed);
    }
    // 요청 타입이거나 그 파생이면 허용한다(단일 비가상 상속 사슬, TypeLease::IsNativeA).
    if (nativeType && !slot->type.IsNativeA(nativeType)) {
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
    const ErrorCode shaded = ShadeLocked(handle);
    if (shaded != ErrorCode::None) {
        return Result<void>::Failure(shaded);
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
    const ErrorCode shaded = registry->ShadeLocked(handle);
    if (shaded != ErrorCode::None) {
        return Result<void>::Failure(shaded);
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
    // 명시된 문맥이 있으면 다른 World의 Borrow에서 포인터를 얻을 수 없다.
    // 문맥이 없을 때의 일반 조회와 다른 스레드의 Borrow 반환은 그대로 허용한다.
    auto current = ObjectAccessScope::Current();
    if (current && !(current.Value().context == registry->context_)) {
        return Result<void*>::Failure(ErrorCode::WrongContext);
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
    // 항상 Object 하위 객체 주소를 돌려준다. Borrow<T>::Get이 static_cast로 T*를 얻는다(파생 타입의
    // 기반 T 하위 객체 위치는 C++ 변환만 정확히 안다).
    return Result<void*>::Success(static_cast<void*>(slot->object));
}

Result<ObjectLifecycle> InspectBorrow(ObjectRegistry* registry, ObjectHandle handle) noexcept {
    if (!registry) {
        return Result<ObjectLifecycle>::Failure(ErrorCode::InvalidArgument);
    }
    auto access = registry->ScopedAccess();
    if (!access) {
        return Result<ObjectLifecycle>::Failure(access.Error());
    }
    return registry->InspectChecked(access.Value(), handle, nullptr, true);
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
    if (!source || !source->object || IsCutoff(source) ||
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
        if (!destination || IsCutoff(destination) ||
            !(destination->state == SlotState::Alive ||
              (transaction && destination->state == SlotState::Reserved &&
               destination->transaction == transaction))) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }
        if (nativeType && !destination->type.IsNativeA(nativeType)) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }
        // staged 내부 간선은 Commit에서 전체 게시·shade한다. 이미 게시된 대상은 즉시 보존한다.
        if (destination->state == SlotState::Alive) {
            const ErrorCode shaded = ShadeLocked(target);
            if (shaded != ErrorCode::None) {
                return Result<void>::Failure(shaded);
            }
        }
    }
    field = target;
    return Result<void>::Success();
}

Result<void> ObjectRegistry::RequestDestroy(const ObjectAccessContext& access,
                                            ObjectHandle handle) noexcept {
    return RequestDestroyChecked(access, handle, nullptr, false);
}

Result<ObjectAccessContext> ObjectRegistry::ScopedAccess() const noexcept {
    if (platform::CurrentThreadId() != ownerThread_) {
        return Result<ObjectAccessContext>::Failure(ErrorCode::WrongThread);
    }
    auto current = ObjectAccessScope::Current();
    if (current) {
        // 명시된 실행 문맥은 항상 우선한다. 다른 World라면 CheckAccess가 거부한다.
        return current;
    }
    // 실행 문맥이 없으면 객체가 속한 Registry를 사용한다. 이때 호출자의 World는 식별하지
    // 못한다. 등록된 Borrow·슬롯·스레드 검사는 이후에도 그대로 수행한다.
    if (current.Error() == ErrorCode::WrongContext && !options_.requireLifecycleScope) {
        return Result<ObjectAccessContext>::Success(OwnerAccess());
    }
    return current;
}

Result<void> ObjectRegistry::DestroySelf(ObjectHandle handle, const Object* object) noexcept {
    auto access = ScopedAccess();
    if (!access) {
        return Result<void>::Failure(access.Error());
    }
    return RequestDestroyChecked(access.Value(), handle, object, true);
}

Result<void> ObjectRegistry::RequestDestroyChecked(const ObjectAccessContext& access,
                                                   ObjectHandle handle, const Object* object,
                                                   bool requireBorrow) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }
    platform::LockGuard lock(gate_);
    if (closed_ || processing_) {
        return Result<void>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }
    Slot* slot = FindSlot(handle);
    if (requireBorrow) {
        if (!slot || slot->state == SlotState::Reserved || slot->state == SlotState::Free ||
            slot->state == SlotState::Exhausted || (object && slot->object != object)) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }
        if (slot->borrows == 0) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }
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

Result<ObjectLifecycle> ObjectRegistry::InspectLifecycle(const ObjectAccessContext& access,
                                                         ObjectHandle handle) const noexcept {
    return InspectChecked(access, handle, nullptr, false);
}

Result<ObjectLifecycle> ObjectRegistry::InspectSelf(ObjectHandle handle,
                                                    const Object* object) const noexcept {
    auto access = ScopedAccess();
    if (!access) {
        return Result<ObjectLifecycle>::Failure(access.Error());
    }
    return InspectChecked(access.Value(), handle, object, true);
}

Result<ObjectLifecycle> ObjectRegistry::InspectChecked(const ObjectAccessContext& access,
                                                       ObjectHandle handle, const Object* object,
                                                       bool requireBorrow) const noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<ObjectLifecycle>::Failure(valid.Error());
    }
    platform::LockGuard lock(gate_);
    if (closed_ || processing_) {
        return Result<ObjectLifecycle>::Failure(closed_ ? ErrorCode::ShuttingDown
                                                        : ErrorCode::Busy);
    }
    Slot* slot = FindSlot(handle);
    if (!slot || slot->state == SlotState::Free || slot->state == SlotState::Reserved ||
        slot->state == SlotState::Exhausted) {
        return Result<ObjectLifecycle>::Failure(ErrorCode::StaleHandle);
    }
    if (object && slot->object != object) {
        return Result<ObjectLifecycle>::Failure(ErrorCode::StaleHandle);
    }
    if (requireBorrow && slot->borrows == 0) {
        return Result<ObjectLifecycle>::Failure(ErrorCode::InvalidArgument);
    }
    if (IsCutoff(slot)) {
        return Result<ObjectLifecycle>::Success(ObjectLifecycle::Unreachable);
    }
    switch (slot->state) {
    case SlotState::Alive:
        return Result<ObjectLifecycle>::Success(ObjectLifecycle::Alive);
    case SlotState::DestroyRequested:
        return Result<ObjectLifecycle>::Success(ObjectLifecycle::DestroyRequested);
    case SlotState::Retired:
        return Result<ObjectLifecycle>::Success(ObjectLifecycle::Retired);
    case SlotState::Reclaiming:
        return Result<ObjectLifecycle>::Failure(ErrorCode::Busy);
    default:
        return Result<ObjectLifecycle>::Failure(ErrorCode::StaleHandle);
    }
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

Result<TypeLease> ObjectRegistry::AcquireTypeSelf(ObjectHandle handle,
                                                  const Object* object) noexcept {
    auto access = ScopedAccess();
    if (!access) {
        return Result<TypeLease>::Failure(access.Error());
    }

    auto valid = CheckAccess(access.Value());
    if (!valid) {
        return Result<TypeLease>::Failure(valid.Error());
    }

    platform::LockGuard lock(gate_);
    if (closed_ || processing_) {
        return Result<TypeLease>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }

    Slot* slot = FindSlot(handle);
    if (!slot || slot->state == SlotState::Free || slot->state == SlotState::Reserved ||
        slot->state == SlotState::Exhausted || slot->object != object) {
        return Result<TypeLease>::Failure(ErrorCode::StaleHandle);
    }
    if (slot->borrows == 0) {
        return Result<TypeLease>::Failure(ErrorCode::InvalidArgument);
    }

    auto live = CheckLive(slot, nullptr);
    if (!live) {
        return Result<TypeLease>::Failure(live.Error());
    }
    return slot->type.Clone();
}

Result<TypeLease> ObjectRegistry::AcquireType(const ObjectAccessContext& access,
                                              ObjectHandle handle) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<TypeLease>::Failure(valid.Error());
    }
    platform::LockGuard lock(gate_);
    if (closed_ || processing_) {
        return Result<TypeLease>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }
    Slot* slot = FindSlot(handle);
    auto live = CheckLive(slot, nullptr);
    if (!live) {
        return Result<TypeLease>::Failure(live.Error());
    }
    return slot->type.Clone();
}

Result<TypeLease> ObjectRegistry::AcquireRegisteredType(const ObjectAccessContext& access,
                                                        TypeId type) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<TypeLease>::Failure(valid.Error());
    }
    platform::LockGuard lock(gate_);
    if (closed_ || processing_) {
        return Result<TypeLease>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }
    return types_.Acquire(type);
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
    if (nativeType && !typeLease.Value().IsNativeA(nativeType)) {
        finish();
        return Result<ObjectHandle>::Failure(ErrorCode::TypeMismatch);
    }
    const auto& ops = typeLease.Value().Ops();
    // Abstract 선언은 C++가 생성 가능한 타입이어도 직접 생성할 수 없다.
    // 슬롯/메모리를 예약하기 전에 검사하여 실패가 관찰 가능한 부작용을 남기지 않는다.
    if (HasFlag(typeLease.Value().Schema().flags, TypeFlags::Abstract) ||
        ops.construct == nullptr) {
        finish();
        return Result<ObjectHandle>::Failure(ErrorCode::Unsupported);
    }
    if (gcPhase_ == GcPhase::Marking) {
        // mutator가 새 객체를 만드는 지점에서만 작업 큐를 확장한다. gate/Trace 안의 barrier는
        // 할당하지 않는다. Marking 동안 공개된 객체는 회수하지 않아 슬롯 수가 큐 상한이 된다.
        uint64 capacity = (uint64(chunkCount_) + 1) * SlotsPerChunk;
        if (capacity > options_.maxSlots) {
            capacity = options_.maxSlots;
        }
        auto reserve = gcWork_.TryReserve(static_cast<usize>(capacity));
        if (!reserve) {
            finish();
            return Result<ObjectHandle>::Failure(reserve.Error());
        }
    }
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
    Object* object = freeSlot->type.Ops().construct(memory.Value(), allocator_);
    if (!object) {
        platform::Fatal("A noexcept object constructor must return its object");
    }
    object->registry_ = this;
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
    if (nativeType && !slot->type.IsNativeA(nativeType)) {
        return Result<void*>::Failure(ErrorCode::TypeMismatch);
    }
    return Result<void*>::Success(static_cast<void*>(slot->object));
}

void ObjectRegistry::ValidateStagedEdge(void* pointer, ObjectHandle handle) noexcept {
    auto& state = *static_cast<MarkState*>(pointer);
    if (handle.IsNull() || state.error != ErrorCode::None) {
        return;
    }
    platform::LockGuard lock(state.registry->gate_);
    Slot* slot = state.registry->FindSlot(handle);
    if (!slot || state.registry->IsCutoff(slot) ||
        !(slot->state == SlotState::Alive ||
          (slot->state == SlotState::Reserved && slot->transaction == state.transaction))) {
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
        if (gcPhase_ == GcPhase::Marking) {
            if (ShadeLocked(staged[index]) != ErrorCode::None) {
                platform::Fatal("Preallocated newborn mark queue exhausted");
            }
            ++cycleGc_.examined;
        } else if (gcPhase_ == GcPhase::Sweeping) {
            // cutoff 이후 태어난 객체는 이전 cohort의 white 후보가 아니다. Commit 검증이
            // 기존 white 대상 연결을 거부하므로 새 객체를 이번 cycle에서 trace할 필요가 없다.
            slot->mark = markEpoch_;
        }
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
    if (handle.IsNull()) {
        return;
    }
    platform::LockGuard lock(state.registry->gate_);
    if (state.error != ErrorCode::None) {
        return;
    }
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

bool ObjectRegistry::IsCutoff(const Slot* slot) const noexcept {
    return slot && gcPhase_ == GcPhase::Sweeping &&
           (slot->state == SlotState::Alive || slot->state == SlotState::DestroyRequested) &&
           slot->mark != markEpoch_;
}

ErrorCode ObjectRegistry::ShadeLocked(ObjectHandle handle) noexcept {
    if (gcPhase_ != GcPhase::Marking || handle.IsNull()) {
        return ErrorCode::None;
    }
    Slot* slot = FindSlot(handle);
    if (!slot || !(slot->state == SlotState::Alive || slot->state == SlotState::DestroyRequested)) {
        return ErrorCode::StaleHandle;
    }
    if (slot->mark == markEpoch_) {
        return ErrorCode::None;
    }
    // Reserve는 Begin/CreatePending에서 gate 밖에 수행한다. barrier가 allocator 콜백을 실행하지
    // 않도록 capacity를 먼저 검사하며 불변식 위반은 실패로 반환한다.
    if (gcWork_.Size() == gcWork_.Capacity()) {
        return ErrorCode::CapacityExceeded;
    }
    auto pushed = gcWork_.TryPushBack(handle);
    if (!pushed) {
        return pushed.Error();
    }
    slot->mark = markEpoch_;
    return ErrorCode::None;
}

GcPhase ObjectRegistry::CollectionPhase() const noexcept {
    platform::LockGuard lock(gate_);
    return gcPhase_;
}

Result<void> ObjectRegistry::BeginCollection(const ObjectAccessContext& access) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }
    const uint64 started = platform::MonotonicNanoseconds();
    usize capacity;
    {
        platform::LockGuard lock(gate_);
        if (processing_ || constructions_ || totalBorrows_ || gcPhase_ != GcPhase::Idle) {
            return Result<void>::Failure(ErrorCode::Busy);
        }
        processing_ = true;
        capacity = static_cast<usize>(uint64(chunkCount_) * SlotsPerChunk);
    }
    gcWork_.Clear();
    auto reserved = gcWork_.TryReserve(capacity);
    platform::LockGuard lock(gate_);
    processing_ = false;
    if (!reserved) {
        return reserved;
    }
    if (++markEpoch_ == 0) {
        markEpoch_ = 1;
        for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
            for (auto& slot : chunk->slots) {
                slot.mark = 0;
            }
        }
    }
    gcPhase_ = GcPhase::Marking;
    gcCursor_ = 0;
    cycleGc_ = {};
    sweepChunk_ = nullptr;
    sweepOffset_ = 0;
    for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
        for (auto& slot : chunk->slots) {
            if (slot.state == SlotState::Alive || slot.state == SlotState::DestroyRequested) {
                ++cycleGc_.examined;
                if (slot.roots != 0 &&
                    ShadeLocked({domain_, slot.index, slot.generation}) != ErrorCode::None) {
                    platform::Fatal("Preallocated root snapshot queue exhausted");
                }
            }
        }
    }
    cycleGc_.durationNanoseconds = platform::MonotonicNanoseconds() - started;
    return Result<void>::Success();
}

Result<void> ObjectRegistry::CancelCollection(const ObjectAccessContext& access) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }
    platform::LockGuard lock(gate_);
    if (processing_ || gcPhase_ == GcPhase::Sweeping || gcPhase_ == GcPhase::Reclaiming) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    gcPhase_ = GcPhase::Idle;
    gcWork_.Clear();
    gcCursor_ = 0;
    return Result<void>::Success();
}

bool ObjectRegistry::HasReclaimableLocked() const noexcept {
    for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
        for (const auto& slot : chunk->slots) {
            if (slot.state == SlotState::Retired && slot.roots == 0 && slot.borrows == 0 &&
                slot.backendHolds == 0) {
                return true;
            }
        }
    }
    return false;
}

Result<GcProgress> ObjectRegistry::StepCollection(const ObjectAccessContext& access,
                                                  GcBudget budget,
                                                  WorkerScheduler* scheduler) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<GcProgress>::Failure(valid.Error());
    }
    if (budget.maxObjects == 0) {
        return Result<GcProgress>::Failure(ErrorCode::InvalidArgument);
    }
    if (scheduler && scheduler->IsCurrentWorker()) {
        return Result<GcProgress>::Failure(ErrorCode::WrongThread);
    }
    const uint64 started = platform::MonotonicNanoseconds();
    uint64 deadline = 0;
    if (budget.maxNanoseconds != 0) {
        const uint64 maximum = static_cast<uint64>(-1);
        deadline =
            budget.maxNanoseconds > maximum - started ? maximum : started + budget.maxNanoseconds;
    }
    auto inBudget = [deadline]() noexcept {
        return deadline == 0 || platform::MonotonicNanoseconds() < deadline;
    };
    {
        platform::LockGuard lock(gate_);
        if (processing_ || constructions_ || totalBorrows_) {
            return Result<GcProgress>::Failure(ErrorCode::Busy);
        }
        if (gcPhase_ == GcPhase::Idle) {
            return Result<GcProgress>::Failure(ErrorCode::InvalidArgument);
        }
        processing_ = true;
    }

    uint32 processed = 0;
    MarkState state{this, &gcWork_};
    auto setError = [this, &state](ErrorCode error) noexcept {
        platform::LockGuard lock(gate_);
        if (state.error == ErrorCode::None) {
            state.error = error;
        }
    };
    auto hasError = [this, &state]() noexcept {
        platform::LockGuard lock(gate_);
        return state.error != ErrorCode::None;
    };
    while (gcPhase_ == GcPhase::Marking && processed < budget.maxObjects && inBudget() &&
           !hasError()) {
        ObjectHandle next;
        bool parallel = false;
        {
            platform::LockGuard lock(gate_);
            if (gcCursor_ == gcWork_.Size()) {
                // 모든 barrier가 같은 gate에서 게시되므로 빈 큐 판정과 cutoff는 하나의 선형화다.
                // 아직 slot을 Retired로 바꾸기 전에도 white 객체의 Resolve/Pin/연결을 차단한다.
                cycleGc_.marked = static_cast<uint32>(gcWork_.Size());
                gcPhase_ = GcPhase::Sweeping;
                sweepChunk_ = chunks_;
                sweepOffset_ = 0;
                break;
            }
            next = gcWork_[gcCursor_];
            parallel = scheduler && FindSlot(next)->type.Ops().parallelTraceSafe;
        }
        if (!parallel) {
            ++gcCursor_;
            TraceVisitor visitor(&state, &ObjectRegistry::MarkEdge);
            Slot* slot = FindSlot(next);
            slot->type.Ops().trace(*slot->object, visitor);
            ++processed;
            continue;
        }

        // 한 batch는 최대 64개다. 각 task의 완료는 capture 정리 뒤 관측하고 모두 회수한 뒤에만
        // stack state/visitor를 없앤다. 미선언 Trace는 이 batch와 겹쳐 실행하지 않는다.
        WorkerTaskHandle tasks[64];
        uint32 submitted = 0;
        while (submitted < 64 && processed < budget.maxObjects && inBudget() && !hasError()) {
            ObjectHandle handle;
            {
                platform::LockGuard lock(gate_);
                if (gcCursor_ == gcWork_.Size()) {
                    break;
                }
                handle = gcWork_[gcCursor_];
                if (!FindSlot(handle)->type.Ops().parallelTraceSafe) {
                    break;
                }
                ++gcCursor_;
            }
            auto body = Function<void(WorkerTaskContext&)>::TryCreate(
                allocator_, [this, &state, handle](WorkerTaskContext&) noexcept {
                    TraceVisitor visitor(&state, &ObjectRegistry::MarkEdge);
                    Slot* slot = FindSlot(handle);
                    slot->type.Ops().trace(*slot->object, visitor);
                });
            if (!body) {
                setError(body.Error());
                break;
            }
            auto accepted = scheduler->Submit(Move(body.Value()));
            if (!accepted) {
                setError(accepted.Error());
                break;
            }
            tasks[submitted++] = Move(accepted.Value());
            ++processed;
        }
        for (uint32 index = 0; index < submitted; ++index) {
            auto waited = tasks[index].Wait();
            if (!waited || waited.Value() != platform::WaitResult::Signaled) {
                // stack에 남은 trace context를 사용하는 worker를 버리고 복귀할 수 없다.
                platform::Fatal("Cannot drain a GC marker task");
            }
            auto status = tasks[index].Status();
            if (!status) {
                setError(status.Error());
            } else if (status.Value().state != WorkerTaskState::Succeeded) {
                setError(status.Value().error == ErrorCode::None ? ErrorCode::DependencyFailed
                                                                 : status.Value().error);
            }
        }
    }
    if (hasError()) {
        platform::LockGuard lock(gate_);
        gcPhase_ = GcPhase::Idle;
        gcWork_.Clear();
        gcCursor_ = 0;
        processing_ = false;
        return Result<GcProgress>::Failure(state.error);
    }

    while (gcPhase_ == GcPhase::Sweeping && processed < budget.maxObjects && inBudget()) {
        platform::LockGuard lock(gate_);
        if (!sweepChunk_) {
            gcPhase_ = GcPhase::Reclaiming;
            break;
        }
        Slot& slot = sweepChunk_->slots[sweepOffset_++];
        if ((slot.state == SlotState::Alive || slot.state == SlotState::DestroyRequested) &&
            slot.mark != markEpoch_) {
            slot.state = SlotState::Retired;
            ++cycleGc_.retired;
        }
        if (sweepOffset_ == SlotsPerChunk) {
            sweepChunk_ = sweepChunk_->next;
            sweepOffset_ = 0;
        }
        ++processed;
    }
    if (gcPhase_ == GcPhase::Reclaiming && processed < budget.maxObjects && inBudget()) {
        const uint32 reclaimed = ReclaimEligible(budget.maxObjects - processed, deadline);
        cycleGc_.reclaimed += reclaimed;
        processed += reclaimed;
    }
    platform::LockGuard lock(gate_);
    cycleGc_.durationNanoseconds += platform::MonotonicNanoseconds() - started;
    if (gcPhase_ == GcPhase::Reclaiming && !HasReclaimableLocked()) {
        gcPhase_ = GcPhase::Idle;
        lastGc_ = cycleGc_;
        gcWork_.Clear();
        gcCursor_ = 0;
    }
    processing_ = false;
    return Result<GcProgress>::Success({gcPhase_, processed, cycleGc_});
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
        if (processing_ || constructions_ || totalBorrows_ || gcPhase_ != GcPhase::Idle) {
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

uint32 ObjectRegistry::ReclaimEligible(uint32 maximum, uint64 deadline) noexcept {
    uint32 reclaimed = 0;
    while (reclaimed < maximum && (deadline == 0 || platform::MonotonicNanoseconds() < deadline)) {
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
    // cutoff를 게시한 cycle은 끝까지 분류해야 한다. 중간 취소로 white 객체를 다시 노출하지 않는다.
    while (CollectionPhase() != GcPhase::Idle) {
        auto advanced = StepCollection(access, {0xffffffffu, 0});
        if (!advanced) {
            return Result<void>::Failure(advanced.Error());
        }
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
