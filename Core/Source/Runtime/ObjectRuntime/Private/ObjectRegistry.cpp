#include "ObjectRegistryInternal.h"

#include <Base/Compiler.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include "RegistryIdentity.h"

// ObjectRegistry 기반부다. 생성·소멸·종료(Shutdown), 슬롯 조회(chunk 표 색인 SlotAt·FindSlot)와
// 접근·생존 검사, root·Borrow·backend lease·encode 고정 보유와 반환, 파괴 요청·수명·타입 조회,
// 통계·진단을 둔다. 생성 트랜잭션은 ObjectRegistryCreate.cpp, Outer·이름은
// ObjectRegistryNames.cpp, 참조 간선 쓰기는 ObjectRegistryEdges.cpp(단일 참조·검증)·
// ObjectRegistryArrayEdges.cpp(ObjectRefArray)·ObjectRegistryStructElements.cpp(StructRefArray),
// GC는 ObjectRegistryGc.cpp, 관찰자 기록과 Discard는 ObjectRegistryObserver.cpp, 공용 내부 정의는
// ObjectRegistryInternal.h다.
namespace rocks {

using namespace registry_detail;

ObjectRegistry::ObjectRegistry(Allocator& allocator, TypeRegistry& types, ContextId context,
                               RegistryOptions options) noexcept
    : allocator_(allocator), types_(types), context_(context), options_(options),
      domain_(detail::NextRegistryDomain()), ownerThread_(platform::CurrentThreadId()),
      gcWork_(allocator), gcShade_(allocator),
      clustersEnabled_(options.clusters ||
                       (options.policy.packageNativeType != nullptr && options.packageClusters)) {
    // 클러스터 장부 칸·작업 큐 항목이 클러스터 번호와 표지 비트를 같은 32비트에 담는다.
    if (clustersEnabled_ && options_.maxSlots > RegistryOptions::ClusterSlotLimit) {
        platform::Fatal("A cluster-enabled ObjectRegistry allows at most ClusterSlotLimit slots");
    }

    // 관찰자는 패키지 정책 Registry 하나에만 붙는다(생성자는 실패를 돌려줄 수 없다).
    if (ReferenceObserver* observer = options_.referenceObserver) {
        if (options_.policy.packageNativeType == nullptr) {
            platform::Fatal("ReferenceObserver requires a package-policy ObjectRegistry");
        }

        if (observer->registry_ != nullptr) {
            platform::Fatal("ReferenceObserver is already attached to an ObjectRegistry");
        }

        observer->registry_ = this;
        observer->ownerThread_ = ownerThread_;
    }
}

ObjectRegistry::~ObjectRegistry() {
    auto stopped = Shutdown(OwnerAccess());
    if (!stopped) {
        platform::Fatal(
            "ObjectRegistry destroyed before roots/readers/backend leases/encode pins drained");
    }

    // 관찰자를 떼어 낸다(남은 변화는 관찰자가 계속 갖는다).
    if (ReferenceObserver* observer = options_.referenceObserver) {
        observer->registry_ = nullptr;
        observer->ownerThread_ = 0;
    }

    // 종료 회수가 모든 NameRecord를 반환했다. 남은 것은 빈 색인 표다.
    ReleaseRegistryMemory(allocator_, names_);
    names_ = nullptr;
    for (uint32 index = chunkCount_; index-- != 0;) {
        Chunk* chunk = chunks_[index];
        chunk->~Chunk();
        auto released = allocator_.Deallocate(chunk);
        if (!released) {
            platform::Fatal("Registry chunk allocator mismatch");
        }
    }

    for (ChunkTable* table = chunkTables_; table != nullptr;) {
        ChunkTable* previous = table->previous;
        table->~ChunkTable();
        ReleaseRegistryMemory(allocator_, table);
        table = previous;
    }

    chunkTables_ = nullptr;
    chunks_ = nullptr;
    for (uint32 index = 0; index < gcPageCount_; ++index) {
        ReleaseRegistryMemory(allocator_, gcPages_[index]->cells);
        gcPages_[index]->~GcPage();
        ReleaseRegistryMemory(allocator_, gcPages_[index]);
    }

    ReleaseGrayBlocks();
    while (GcBlock* block = gcFreeBlocks_) {
        gcFreeBlocks_ = block->next;
        block->~GcBlock();
        ReleaseRegistryMemory(allocator_, block);
    }

    ReleaseRegistryMemory(allocator_, gcPages_);
    ReleaseRegistryMemory(allocator_, gcTypes_);
    ReleaseRegistryMemory(allocator_, rootList_);
    ReleaseRegistryMemory(allocator_, clusters_);
    clusters_ = nullptr;
    gcPages_ = nullptr;
    gcTypes_ = nullptr;
    rootList_ = nullptr;
    chunkCount_ = 0;
    chunkCapacity_ = 0;
    freeSlots_ = nullptr;
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

ObjectHandle ObjectRegistry::HandleOf(const Slot& slot) const noexcept {
    return {domain_, slot.index, slot.generation};
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

    if (slot->roots == NumericLimits<uint32>::Max()) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    const ErrorCode shaded = ShadeLocked(handle);
    if (shaded != ErrorCode::None) {
        return Result<void>::Failure(shaded);
    }

    if (slot->roots++ == 0) {
        AddRootListLocked(*slot);
    }

    ++tally_.roots;
    return Result<void>::Success();
}

Result<void> ObjectRegistry::AddBorrow(const ObjectAccessContext& access, ObjectHandle handle,
                                       const void* nativeType, bool rejectEncodePinned) noexcept {
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

    // 쓰기용 Borrow는 encode 중인 owner의 값을 바꾸지 못하게 한다(ResolveForWrite).
    if (rejectEncodePinned && slot->encodePins != 0) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (totalBorrows_ == NumericLimits<uint32>::Max() ||
        slot->borrows == NumericLimits<uint32>::Max()) {
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

    if (slot->roots == NumericLimits<uint32>::Max()) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    const ErrorCode shaded = registry->ShadeLocked(handle);
    if (shaded != ErrorCode::None) {
        return Result<void>::Failure(shaded);
    }

    ++slot->roots;
    ++registry->tally_.roots;
    return Result<void>::Success();
}

void ReleaseRoot(ObjectRegistry* registry, ObjectHandle handle) noexcept {
    platform::LockGuard lock(registry->gate_);
    auto* slot = registry->FindSlot(handle);
    if (!slot || slot->roots == 0) {
        platform::Fatal("RootRef release after lifetime closure");
    }

    if (--slot->roots == 0) {
        // 루트 목록에서 즉시 뺀다(GC-S3 결정 — 목록은 Begin·Collect의 잠금 구간만 읽으므로 slice
        // 중 반환도 안전하다. 이미 표식된 객체는 이번 cycle에 남는다).
        registry->RemoveRootListLocked(*slot);
    }

    --registry->tally_.roots;
    registry->WakeDeferredLocked(*slot);
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
    registry->WakeDeferredLocked(*slot);
}

void ReleaseBackend(ObjectRegistry* registry, ObjectHandle handle) noexcept {
    platform::LockGuard lock(registry->gate_);
    auto* slot = registry->FindSlot(handle);
    if (!slot || slot->backendHolds == 0) {
        platform::Fatal("Backend lease release after lifetime closure");
    }

    --slot->backendHolds;
    --registry->tally_.backendHolds;
    registry->WakeDeferredLocked(*slot);
}

void ReleaseEncodePin(ObjectRegistry* registry, ObjectHandle handle) noexcept {
    platform::LockGuard lock(registry->gate_);
    auto* slot = registry->FindSlot(handle);
    if (!slot || slot->encodePins == 0) {
        platform::Fatal("Encode pin release after lifetime closure");
    }

    --slot->encodePins;
    --registry->tally_.encodePins;
    registry->WakeDeferredLocked(*slot);
}

} // namespace detail

Result<EncodePin> ObjectRegistry::PinForEncode(const ObjectAccessContext& access,
                                               ObjectHandle owner) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<EncodePin>::Failure(valid.Error());
    }

    platform::LockGuard lock(gate_);
    if (closed_ || processing_) {
        return Result<EncodePin>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }

    Slot* slot = FindSlot(owner);
    auto live = CheckLive(slot, nullptr);
    if (!live) {
        return Result<EncodePin>::Failure(live.Error());
    }

    if (slot->encodePins == NumericLimits<uint32>::Max()) {
        return Result<EncodePin>::Failure(ErrorCode::Overflow);
    }

    ++slot->encodePins;
    ++tally_.encodePins;
    return Result<EncodePin>::Success(EncodePin(this, owner));
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

    // 패키지 정책 Registry의 소속 객체는 Discard로만 죽는다(관찰자 유무와 무관 — 결정 1).
    if (options_.policy.packageNativeType != nullptr) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    Slot* slot = nullptr;
    {
        platform::LockGuard lock(gate_);
        auto requested = RequestDestroyLocked(handle, object, requireBorrow, slot);
        if (!requested || slot == nullptr) {
            return requested;
        }
    }

    // 클러스터 소속의 이탈(13.4절): 처리 표시 아래 잠금 밖에서 Trace로 장부 몫을 뺀 뒤 잠가 뺀다.
    LeaveTrace(*slot);
    platform::LockGuard lock(gate_);
    FinishLeaveLocked(*slot);
    processing_ = false;
    return Result<void>::Success();
}

Result<void> ObjectRegistry::RequestDestroyLocked(ObjectHandle handle, const Object* object,
                                                  bool requireBorrow, Slot*& leaving) noexcept {
    leaving = nullptr;
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

    slot->SetState(tally_, SlotState::DestroyRequested);
    if (IsClusterMemberLocked(*slot)) {
        const uint32 id = CellOf(slot->index).state & ClusterIdMask;
        if (clusters_[id].root == slot->index) {
            // 루트의 파괴 요청은 클러스터를 해체한다(13.2절).
            DissolveClusterLocked(id);
        } else {
            // 소속은 이탈한다. Trace는 잠금 밖에서 하므로 처리 표시를 세운다(그동안 재진입은
            // Busy).
            processing_ = true;
            leaving = slot;
        }
    }

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

    if (slot->backendHolds == NumericLimits<uint32>::Max()) {
        return Result<BackendLease>::Failure(ErrorCode::Overflow);
    }

    ++slot->backendHolds;
    ++tally_.backendHolds;
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
        auto advanced = StepCollection(access, {NumericLimits<uint32>::Max(), 0});
        if (!advanced) {
            return Result<void>::Failure(advanced.Error());
        }
    }

    // 종료는 모든 클러스터를 해체한다(영구 영역 포함 — 13.2·13.7절). 소속은 실제 간선으로 닿지
    // 않으면 아래 수집이 회수한다.
    if (clustersEnabled_) {
        platform::LockGuard lock(gate_);
        DissolveAllClustersLocked();
    }

    auto collected = Collect(access);
    if (!collected) {
        return Result<void>::Failure(collected.Error());
    }

    auto stats = Stats();
    if (stats.liveObjects || stats.reservedObjects || stats.destroyRequested ||
        stats.retiredObjects || stats.roots || stats.borrows || stats.backendHolds ||
        stats.encodePins) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    return Result<void>::Success();
}

ObjectStats ObjectRegistry::Stats() const noexcept {
    platform::LockGuard lock(gate_);
    // 상태 전이·계수 변경마다 맞춘 합이라 슬롯을 훑지 않는다(O(1)).
    ObjectStats stats = tally_;
    stats.borrows = totalBorrows_;
    stats.chunks = chunkCount_;
    stats.namedObjects = nameLive_;
    if constexpr (IsDebugBuild) {
        // Debug 회계: 슬롯 전체를 다시 세어 갱신 누락을 바로 잡아내고, 빈 슬롯·회수 대상 목록이 그
        // 상태의 슬롯을 빠짐없이 한 번씩 담는지(지연 목록에는 회수 가능한 슬롯이 없는지) 본다.
        ObjectStats counted;
        uint64 freeCount = 0;
        uint64 retiredCount = 0;
        for (uint32 chunk = chunkCount_; chunk-- != 0;) {
            for (const auto& slot : chunks_[chunk]->slots) {
                if (uint32* counter = StateTally(counted, slot.state)) {
                    ++*counter;
                }

                freeCount += slot.state == SlotState::Free ? 1u : 0u;
                // GC 밀집 자료(GC-S3): live 비트와 루트 목록 위치가 슬롯과 맞는다.
                const bool live =
                    slot.state == SlotState::Alive || slot.state == SlotState::DestroyRequested;
                const uint64 bit = uint64(1) << (slot.index % SlotsPerChunk);
                if (((BitsOf(slot.index).live & bit) != 0) != live ||
                    (slot.roots != 0) != (slot.rootPosition != NoSlot) ||
                    (slot.rootPosition != NoSlot && (slot.rootPosition >= rootCount_ ||
                                                     rootList_[slot.rootPosition] != slot.index))) {
                    platform::Fatal("Object registry GC bits or root list diverged from its slots");
                }

                retiredCount += slot.state == SlotState::Retired ? 1u : 0u;
                counted.roots += slot.roots;
                counted.borrows += slot.borrows;
                counted.backendHolds += slot.backendHolds;
                counted.encodePins += slot.encodePins;
            }
        }

        if (counted.liveObjects != stats.liveObjects ||
            counted.reservedObjects != stats.reservedObjects ||
            counted.destroyRequested != stats.destroyRequested ||
            counted.retiredObjects != stats.retiredObjects || counted.roots != stats.roots ||
            counted.borrows != stats.borrows || counted.backendHolds != stats.backendHolds ||
            counted.encodePins != stats.encodePins) {
            platform::Fatal("Object registry statistics diverged from its slots");
        }

        const uint64 limit = uint64(chunkCount_) * SlotsPerChunk;
        uint64 listed = 0;
        for (const Slot* slot = freeSlots_; slot != nullptr; slot = slot->nextInList) {
            if (slot->state != SlotState::Free || slot->queue != RetiredQueue::None ||
                ++listed > limit) {
                platform::Fatal("Object registry free list is corrupt");
            }
        }

        if (listed != freeCount) {
            platform::Fatal("Object registry free list lost a slot");
        }

        listed = 0;
        const RetiredList* lists[] = {&retiredPending_, &retiredDeferred_, &retiredReady_};
        const RetiredQueue queues[] = {RetiredQueue::Pending, RetiredQueue::Deferred,
                                       RetiredQueue::Ready};
        for (uint32 at = 0; at < 3; ++at) {
            const Slot* previous = nullptr;
            for (const Slot* slot = lists[at]->head; slot != nullptr; slot = slot->nextInList) {
                if (slot->state != SlotState::Retired || slot->queue != queues[at] ||
                    slot->prevInList != previous || ++listed > limit ||
                    (queues[at] == RetiredQueue::Deferred && IsReclaimable(*slot))) {
                    platform::Fatal("Object registry retired queue is corrupt");
                }

                previous = slot;
            }

            if (lists[at]->tail != previous) {
                platform::Fatal("Object registry retired queue tail is corrupt");
            }
        }

        if (listed != retiredCount) {
            platform::Fatal("Object registry retired queue lost a slot");
        }
    }

    return stats;
}

Result<uint32>
ObjectRegistry::CaptureDiagnostics(Span<ObjectDiagnosticRecord> output) const noexcept {
    platform::LockGuard lock(gate_);
    // Free·Exhausted가 아닌 슬롯 수다(상태별 통계의 합).
    const uint32 required = tally_.liveObjects + tally_.reservedObjects + tally_.destroyRequested +
                            tally_.retiredObjects;
    if (output.Size() < required) {
        return Result<uint32>::Failure(ErrorCode::CapacityExceeded);
    }

    uint32 written = 0;
    for (uint32 chunk = chunkCount_; chunk-- != 0;) {
        for (auto& slot : chunks_[chunk]->slots) {
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
                                 slot.backendHolds,
                                 slot.encodePins};
        }
    }

    return Result<uint32>::Success(written);
}

} // namespace rocks
