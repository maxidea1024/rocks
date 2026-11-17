#include "ObjectRegistryInternal.h"

#include <Base/Compiler.h>

#include <Core/Text/NameComponent.h>
#include <Core/Text/StringBuilder.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <new>

// ObjectRegistry 생성 트랜잭션이다(ConstructionContext가 거치는 Registry 쪽 지점).
// BeginConstruction, CreatePending(생성 검사·이름 기록·이름 색인 확장·chunk 표 확장·빈 슬롯
// 할당·게시), GetPending, AcquireTypeChecked(트랜잭션 객체도 받는 타입 lease), Commit(staged 간선·
// Outer 검증, 관찰자 집계, 공개), Rollback(생성 저널
// 역순 회수)을 둔다.
namespace rocks {

using namespace registry_detail;

namespace {

// chunk 포인터 표의 첫 크기다(1024 슬롯). 이후 두 배로 늘리고 maxSlots가 담는 chunk 수에서 멈춘다.
constexpr uint32 MinimumChunkCapacity = 16;
constexpr uint32 ChunkTag = 0x43484e4bu;      // 'CHNK'
constexpr uint32 ChunkTableTag = 0x43484e54u; // 'CHNT'
constexpr uint32 NameTableTag = 0x4e414d54u;  // 'NAMT'
constexpr uint32 NameRecordTag = 0x4e414d52u; // 'NAMR'

} // namespace

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

    if (nextTransaction_ == 0 || constructions_ == NumericLimits<uint32>::Max()) {
        return Result<uint64>::Failure(ErrorCode::Overflow);
    }

    ++constructions_;
    return Result<uint64>::Success(nextTransaction_++);
}

Result<ObjectHandle> ObjectRegistry::CreatePending(const ObjectAccessContext& access, TypeId type,
                                                   uint64 transaction, const void* nativeType,
                                                   ObjectHandle outer, StringView name) noexcept {
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

        // 이후 잠금 밖 할당·사용자 콜백 동안 Registry 변경 재진입은 Busy다. 그래서 아래에서 확인한
        // Outer 상태·이름 색인은 게시할 때까지 바뀌지 않는다.
        processing_ = true;
    }

    // 실패하면 잠금 밖에서 반환할 할당이다. 게시에 성공하면 슬롯·색인·chunk 표가 소유한다.
    NameRecord* record = nullptr;
    NameEntry* grown = nullptr;
    ChunkTable* grownChunks = nullptr;
    GcGrowth gcGrowth;
    void* storage = nullptr;
    auto fail = [&](ErrorCode error) noexcept -> Result<ObjectHandle> {
        ReleaseRegistryMemory(allocator_, storage);
        ReleaseRegistryMemory(allocator_, grownChunks);
        ReleaseGcGrowth(gcGrowth);
        ReleaseRegistryMemory(allocator_, grown);
        ReleaseRegistryMemory(allocator_, record);
        platform::LockGuard lock(gate_);
        processing_ = false;
        return Result<ObjectHandle>::Failure(error);
    };
    // 3. 타입
    auto typeLease = types_.Acquire(type);
    if (!typeLease) {
        return fail(typeLease.Error());
    }

    if (nativeType && !typeLease.Value().IsNativeA(nativeType)) {
        return fail(ErrorCode::TypeMismatch);
    }

    const auto& ops = typeLease.Value().Ops();
    // 4. Abstract 선언은 C++가 생성 가능한 타입이어도 직접 생성할 수 없다.
    // 슬롯/메모리를 예약하기 전에 검사하여 실패가 관찰 가능한 부작용을 남기지 않는다.
    if (HasFlag(typeLease.Value().Schema().flags, TypeFlags::Abstract) ||
        ops.construct == nullptr) {
        return fail(ErrorCode::Unsupported);
    }

    // 4. 패키지 정책 Registry는 완전한 필드 배치(completeFieldLayout — 모든 강한 간선이 metadata에
    // 있고 Trace가 그것을 모두 방문한다는 선언)가 아닌 타입을 만들지 않는다. 관찰자 계수(편집은
    // Registry API로, Commit·Discard는 Trace로 센다)와 패키지 그래프 저장이 이 선언에 기댄다
    // (Docs/Design/ObjectFoundations.ko.md 1.5절).
    if (options_.policy.packageNativeType != nullptr && !ops.completeFieldLayout) {
        return fail(ErrorCode::Unsupported);
    }

    // 5. Outer 상태: 생존(Alive, cutoff 아님) 또는 이 트랜잭션의 공개 전 객체다. 다른 Registry의
    // handle은 FindSlot이 domain 불일치로 찾지 않는다.
    const bool hasOuter = !outer.IsNull();
    const ObjectHandle outerKey = hasOuter ? outer : ObjectHandle{};
    Slot* parent = nullptr;
    OuterSummary summary;
    bool outerValid = true;
    if (hasOuter) {
        platform::LockGuard lock(gate_);
        parent = FindSlot(outer);
        outerValid = parent != nullptr && !IsCutoff(parent) &&
                     (parent->state == SlotState::Alive ||
                      (parent->state == SlotState::Reserved && parent->transaction == transaction &&
                       parent->object != nullptr));
        if (outerValid) {
            // hasNamedInners는 대표 자식(빈 이름) 생성에만 쓰인다. 이름 있는 생성은 고리를 훑지
            // 않는다 (Sweeping 중 cutoff white 형제가 많아도 잠금 시간이 늘지 않게).
            summary = DescribeSlotLocked(*parent, name.IsEmpty());
        }
    }

    if (!outerValid) {
        return fail(ErrorCode::StaleHandle);
    }

    // 6–9. WorldOnly 정책·이름 형식·패키지·대표 자식·경로 길이(NFC 판정 scratch는 잠금 밖).
    const OuterSummary* outerSummary = hasOuter ? &summary : nullptr;
    auto identity = detail::CheckNewObjectIdentity(options_.policy, typeLease.Value(), outerSummary,
                                                   name, allocator_);
    if (!identity) {
        return fail(identity.Error());
    }

    const usize pathBytes = ChildPathBytes(outerSummary, name.ByteCount());
    // 10·11. 이름 있는 객체와 대표 자식은 색인에 넣는다(Outer 없는 이름 없는 객체는 넣지 않는다).
    const bool named = hasOuter || !name.IsEmpty();
    uint64 hash = 0;
    uint32 grownCapacity = 0;
    if (named) {
        StringBuilder<128> key(allocator_);
        if (auto made = TryMakeNameComponentKey(name, key); !made) {
            return fail(made.Error());
        }

        hash = NameKeyHash(outerKey, key.View());
        const usize bytes = sizeof(NameRecord) + name.ByteCount() + key.ByteCount();
        auto allocated = allocator_.Allocate(bytes, alignof(NameRecord), NameRecordTag);
        if (!allocated) {
            return fail(allocated.Error());
        }

        record = ::new (allocated.Value()) NameRecord();
        record->keyHash = hash;
        record->nameBytes = static_cast<uint16>(name.ByteCount());
        record->keyBytes = static_cast<uint16>(key.ByteCount());
        char* data = reinterpret_cast<char*>(record + 1);
        platform::MemoryCopy(data, name.Data(), name.ByteCount());
        platform::MemoryCopy(data + name.ByteCount(), key.View().Data(), key.ByteCount());
        Result<uint32> growth = Result<uint32>::Success(0);
        {
            platform::LockGuard lock(gate_);
            growth = NameIndexGrowthLocked();
        }

        if (!growth) {
            return fail(growth.Error());
        }

        if (growth.Value() != 0) {
            // 새 표(같은 크기 rebuild 또는 두 배)를 잠금 밖에서 받고 게시할 때 잠금 안에서 옮긴다.
            const uint64 tableBytes = uint64(growth.Value()) * sizeof(NameEntry);
            if (tableBytes > MaxSize) {
                return fail(ErrorCode::CapacityExceeded);
            }

            auto table = allocator_.Allocate(static_cast<usize>(tableBytes), alignof(NameEntry),
                                             NameTableTag);
            if (!table) {
                return fail(table.Error());
            }

            grown = static_cast<NameEntry*>(table.Value());
            platform::MemoryZero(grown, static_cast<usize>(tableBytes));
            grownCapacity = growth.Value();
        }

        bool taken = false;
        {
            platform::LockGuard lock(gate_);
            taken = FindNamedLocked(outerKey, hash, record->Key(), false) != nullptr;
        }

        if (taken) {
            return fail(ErrorCode::AlreadyExists);
        }
    }

    // 12. 객체 할당·슬롯. 처리 표시 아래라 그동안 다른 생성·회수가 chunk 표·빈 슬롯 목록을 바꾸지
    // 않는다(다른 스레드의 계수 반환은 목록·표를 바꾸지 않는다).
    if (gcPhase_ == GcPhase::Marking) {
        // mutator가 새 객체를 만드는 지점에서만 작업 큐를 확장한다. gate/Trace 안의 barrier는
        // 할당하지 않는다. Marking 동안 공개된 객체는 회수하지 않아 슬롯 수가 큐 상한이 된다.
        // 모자랄 때만 두 배(상한 maxSlots)로 늘려 새 chunk마다 큐 전체를 다시 할당·복사하지 않는다.
        // 표식 큐와 장벽 큐 둘 다다(장벽은 gcShade_에 넣고 slice 시작에 gcWork_로 옮긴다). 클러스터
        // 항목 몫(레코드 수)도 담는다.
        const uint64 needed = GcQueueBound((uint64(chunkCount_) + 1) * SlotsPerChunk);
        if (auto reserve = ReserveGcQueues(needed, GcQueueBound(options_.maxSlots)); !reserve) {
            return fail(reserve.Error());
        }
    }

    // GC 타입 표 번호(hot 레코드에 둔다 — GC-S3). 표는 처리 표시 아래에서만 자란다.
    auto gcType = AcquireGcType(ops);
    if (!gcType) {
        return fail(gcType.Error());
    }

    auto memory = allocator_.Allocate(ops.size, ops.alignment, 0x4f424a45u);
    if (!memory) {
        return fail(memory.Error());
    }

    storage = memory.Value();
    bool needChunk = false;
    {
        platform::LockGuard lock(gate_);
        needChunk = freeSlots_ == nullptr;
    }

    if (needChunk) {
        if (uint64(chunkCount_) * SlotsPerChunk >= options_.maxSlots) {
            return fail(ErrorCode::CapacityExceeded);
        }

        // chunk 표가 가득 찼으면 두 배 표(상한은 maxSlots를 담는 chunk 수)를 잠금 밖에서 받는다.
        uint32 grownChunkCapacity = 0;
        if (chunkCount_ == chunkCapacity_) {
            const uint64 limit = (uint64(options_.maxSlots) + SlotsPerChunk - 1) / SlotsPerChunk;
            uint64 capacity =
                chunkCapacity_ == 0 ? MinimumChunkCapacity : uint64(chunkCapacity_) * 2;
            if (capacity > limit) {
                capacity = limit;
            }

            auto table = allocator_.Allocate(sizeof(ChunkTable) +
                                                 static_cast<usize>(capacity) * sizeof(Chunk*),
                                             alignof(ChunkTable), ChunkTableTag);
            if (!table) {
                return fail(table.Error());
            }

            grownChunks = ::new (table.Value()) ChunkTable();
            grownChunkCapacity = static_cast<uint32>(capacity);
        }

        // 새 chunk의 슬롯을 담을 GC 페이지·루트 목록 용량(GC-S3)도 잠금 밖에서 준비한다.
        if (auto prepared = PrepareGcGrowth((uint64(chunkCount_) + 1) * SlotsPerChunk, gcGrowth);
            !prepared) {
            return fail(prepared.Error());
        }

        auto block = allocator_.Allocate(sizeof(Chunk), alignof(Chunk), ChunkTag);
        if (!block) {
            return fail(block.Error());
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
            if (grownChunks != nullptr) {
                // 옛 표는 반환하지 않고 사슬로 보관한다(ChunkTable 주석).
                Chunk** entries = grownChunks->Entries();
                for (uint32 index = 0; index < chunkCount_; ++index) {
                    entries[index] = chunks_[index];
                }

                grownChunks->previous = chunkTables_;
                chunkTables_ = grownChunks;
                chunks_ = entries;
                chunkCapacity_ = grownChunkCapacity;
            }

            GrowGcLocked(gcGrowth);
            chunks_[chunkCount_++] = chunk;
            // 낮은 번호부터 쓰도록 거꾸로 넣는다. 상한 밖(Exhausted) 슬롯은 넣지 않는다.
            for (uint32 index = SlotsPerChunk; index-- != 0;) {
                Slot& slot = chunk->slots[index];
                if (slot.state == SlotState::Free) {
                    slot.nextInList = freeSlots_;
                    freeSlots_ = &slot;
                }
            }
        }

        grownChunks = nullptr;
        // GrowGcLocked가 넘겨준 옛 페이지 표·루트 목록을 잠금 밖에서 반환한다.
        ReleaseGcGrowth(gcGrowth);
    }

    Slot* freeSlot = nullptr;
    ObjectHandle handle;
    NameEntry* previousTable = nullptr;
    {
        // 상태·이름 색인·부모 고리를 한 잠금 안에서 게시한다(공개 전 객체도 이름을 점유한다).
        platform::LockGuard lock(gate_);
        freeSlot = freeSlots_;
        freeSlots_ = freeSlot->nextInList;
        freeSlot->nextInList = nullptr;
        ++freeSlotProbes_;
        handle = HandleOf(*freeSlot);
        freeSlot->SetState(tally_, SlotState::Reserved);
        freeSlot->storage = memory.Value();
        freeSlot->transaction = transaction;
        freeSlot->stableType = type;
        freeSlot->type = Move(typeLease.Value());
        freeSlot->outer = outerKey;
        // hot 레코드와 잎 비트(GC-S3·M2). object는 생성자 실행 뒤 채운다.
        GcHot& hot = HotOf(freeSlot->index);
        hot.object = nullptr;
        hot.outer = hasOuter ? outerKey.slot : NoSlot;
        hot.type = gcType.Value();
        if (gcTypes_[hot.type].leaf) {
            BitsOf(freeSlot->index).leaf |= uint64(1) << (freeSlot->index % SlotsPerChunk);
        }
        freeSlot->parent = parent;
        freeSlot->outermost = parent != nullptr ? parent->outermost : freeSlot;
        freeSlot->pathBytes = static_cast<uint16>(pathBytes);
        freeSlot->name = record;
        if (named) {
            if (grown != nullptr) {
                previousTable = RehashNamesLocked(grown, grownCapacity);
            }

            InsertNameLocked(*freeSlot, hash, record->Key());
        }

        if (parent != nullptr) {
            LinkChildLocked(*parent, *freeSlot);
        }
    }

    record = nullptr;
    grown = nullptr;
    storage = nullptr;
    ReleaseRegistryMemory(allocator_, previousTable);
    Object* object = freeSlot->type.Ops().construct(memory.Value(), allocator_);
    if (!object) {
        platform::Fatal("A noexcept object constructor must return its object");
    }

    object->registry_ = this;
    object->handle_ = handle;
    // 그 타입의 첫 객체면 참조 위치 표를 정한다(M3 — 처리 표시 아래라 marker와 겹치지 않는다).
    PrepareReferenceLayout(gcType.Value(), *object);
    {
        platform::LockGuard lock(gate_);
        freeSlot->object = object;
        HotOf(freeSlot->index).object = object;
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

Result<TypeLease> ObjectRegistry::AcquireTypeChecked(const ObjectAccessContext& access,
                                                     ObjectHandle handle,
                                                     uint64 transaction) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<TypeLease>::Failure(valid.Error());
    }

    platform::LockGuard lock(gate_);
    if (closed_ || processing_) {
        return Result<TypeLease>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Busy);
    }

    // staged 참조 대상 규칙(CheckTargetLocked)과 같은 기준에 생성자 실행 뒤(object 있음)를 더한다.
    const Slot* slot = FindSlot(handle);
    if (!slot || IsCutoff(slot) ||
        !(slot->state == SlotState::Alive ||
          (transaction != 0 && slot->state == SlotState::Reserved &&
           slot->transaction == transaction && slot->object != nullptr))) {
        return Result<TypeLease>::Failure(ErrorCode::StaleHandle);
    }

    return slot->type.Clone();
}

void ObjectRegistry::ValidateStagedEdge(void* pointer, ObjectHandle handle) noexcept {
    auto& state = *static_cast<MarkState*>(pointer);
    if (handle.IsNull() || state.error != ErrorCode::None) {
        return;
    }

    if constexpr (IsDebugBuild) {
        // 잎 타입(RuntimeTypeOps::hasStrongReferences == false)은 GC가 trace하지 않으므로 간선을
        // 방문하면 그 간선이 GC에 보이지 않는다.
        if (state.leafOwner) {
            platform::Fatal("A type declared without strong references visited a reference");
        }
    }

    ObjectHandle package;
    {
        platform::LockGuard lock(state.registry->gate_);
        Slot* slot = state.registry->FindSlot(handle);
        if (!slot || state.registry->IsCutoff(slot) ||
            !(slot->state == SlotState::Alive ||
              (slot->state == SlotState::Reserved && slot->transaction == state.transaction))) {
            state.error = ErrorCode::StaleHandle;
            return;
        }

        // 영구 창의 staged 간선(outer 포함)은 같은 트랜잭션 객체나 영구 객체만 가리킨다(13.7절).
        if (state.permanent && slot->state != SlotState::Reserved &&
            !state.registry->IsPermanentLocked(slot->index)) {
            state.error = ErrorCode::Unsupported;
            return;
        }

        if (state.observe) {
            package = state.registry->HandleOf(*slot->outermost);
        }
    }

    // 클러스터 장부: 가입할 소유자의 간선을 (소유자, 대상) 쌍으로 모은다(잠금 밖 할당 — 소속을
    // 모두 정한 뒤 공개 잠금 안에서 센다).
    if (state.ledger != nullptr && state.ledgerOwner != NoSlot) {
        if (!state.ledger->TryPushBack(state.ledgerOwner) ||
            !state.ledger->TryPushBack(handle.slot)) {
            state.error = ErrorCode::OutOfMemory;
            return;
        }
    }

    if (!state.observe) {
        return;
    }

    // 관찰자 쌍 집계(staged 저널 순 × trace 순). 집계 표 삽입은 잠금 밖이다.
    if (!(package == state.ownerPackage)) {
        if (auto added = state.tally->Add(state.ownerPackage, package, state.delta); !added) {
            state.error = added.Error();
        }
    }
}

Result<void> ObjectRegistry::Commit(const ObjectAccessContext& access, uint64 transaction,
                                    Span<const ObjectHandle> staged, Span<const ObjectHandle> roots,
                                    Span<RootRef<Object>> out) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return valid;
    }

    uint32 newClusters = 0;
    const bool packageClusters = clustersEnabled_ && options_.policy.packageNativeType != nullptr;
    bool permanent = false;
    {
        platform::LockGuard lock(gate_);
        if (auto open = OpenState(closed_, processing_); !open) {
            return open;
        }

        permanent = permanentOpen_;

        for (usize index = 0; index < staged.Size(); ++index) {
            Slot* slot = FindSlot(staged[index]);
            if (!slot || slot->state != SlotState::Reserved || slot->transaction != transaction ||
                !slot->object) {
                return Result<void>::Failure(ErrorCode::StaleHandle);
            }
        }

        // root는 이 트랜잭션의 객체여야 한다. 공개 전 객체의 root 계수는 0이므로(Reserved는 Pin할
        // 수 없다) 중복 root도 roots 크기가 uint32 안이면 넘치지 않는다.
        for (usize index = 0; index < roots.Size(); ++index) {
            Slot* slot = FindSlot(roots[index]);
            if (!slot || slot->state != SlotState::Reserved || slot->transaction != transaction ||
                !slot->object) {
                return Result<void>::Failure(ErrorCode::StaleHandle);
            }
        }

        if (roots.Size() > NumericLimits<uint32>::Max()) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        // 클러스터 가입 지정(DesignateCluster)을 다시 본다: 기존 루트가 그 사이 해체·파괴됐으면
        // StaleHandle이다. 새 루트 수만큼 레코드를 잠금 밖에서 확보한다.
        // 영구 창: 모든 객체가 완전한 필드 배치여야 하고 영구 클러스터에 들어간다(13.7절).
        if (permanent) {
            for (usize index = 0; index < staged.Size(); ++index) {
                if (!FindSlot(staged[index])->type.Ops().completeFieldLayout) {
                    return Result<void>::Failure(ErrorCode::Unsupported);
                }
            }

            newClusters = permanentCluster_ == NoSlot ? 1u : 0u;
        }

        if (clustersEnabled_ && !permanent) {
            for (usize index = 0; index < staged.Size(); ++index) {
                uint32& state = ClusterStateOf(staged[index].slot);
                uint32& pendingRoot = ClusterInCountOf(staged[index].slot);
                // 패키지 정책: 패키지 객체는 새 클러스터 루트이고 나머지는 outermost 패키지의
                // 클러스터에 가입한다(13.3절 — 모든 객체가 완전한 필드 배치다).
                if (packageClusters && state == 0) {
                    const Slot* slot = FindSlot(staged[index]);
                    // 경계 비율 안전장치가 클러스터를 해체한 패키지의 새 객체는 일반 객체다.
                    if (!slot->outer.IsNull() && IsFlattenedPackageLocked(*slot->outermost)) {
                        continue;
                    }

                    state = ClusterPending;
                    pendingRoot = slot->outer.IsNull() ? NoSlot : slot->outermost->index;
                }

                if (state != ClusterPending) {
                    continue;
                }

                if (pendingRoot == NoSlot) {
                    ++newClusters;
                } else if (!IsJoinableRootLocked(pendingRoot, transaction)) {
                    return Result<void>::Failure(ErrorCode::StaleHandle);
                }
            }
        }

        processing_ = true;
    }

    // staged 간선과 Outer(패키지 정책이면 자식도 — 같은 트랜잭션 객체다)를 잠금 밖에서 검증한다
    // (처리 표시 아래라 간선·Outer·고리가 바뀌지 않는다). Outer는 대상처럼 생존(Alive) 또는 이
    // 트랜잭션 객체이고 cutoff white가 아니어야 한다. 관찰자가 있으면 같은 trace(staged 저널 순 ×
    // trace 순)로 Outer 있는 소유자의 패키지 사이 쌍을 집계한다(집계 표 삽입은 gate 밖).
    EdgeTally tally(allocator_);
    MarkState state{this, nullptr, transaction};
    state.tally = &tally;
    state.delta = 1;
    state.permanent = permanent;
    // 가입할 소유자의 Trace 간선(소유자, 대상) 쌍이다(outer는 공개 잠금 안에서 hot 레코드로 센다).
    Array<uint32> ledger(allocator_);
    if (newClusters != 0) {
        if (auto reserved = ReserveClusters(newClusters); !reserved) {
            state.error = reserved.Error();
        }
    }

    TraceVisitor visitor(&state, &ObjectRegistry::ValidateStagedEdge);
    for (usize index = 0; index < staged.Size() && state.error == ErrorCode::None; ++index) {
        Slot* slot = FindSlot(staged[index]);
        state.observe = options_.referenceObserver != nullptr && !slot->outer.IsNull();
        state.leafOwner = !slot->type.Ops().hasStrongReferences;
        if (state.observe) {
            state.ownerPackage = HandleOf(*slot->outermost);
        }

        // 처리 표시 아래라 가입 지정 칸이 바뀌지 않는다.
        if (permanent || (clustersEnabled_ && ClusterStateOf(slot->index) == ClusterPending)) {
            state.ledger = &ledger;
            state.ledgerOwner = slot->index;
        }

        slot->type.Ops().trace(*slot->object, visitor);
        state.observe = false;
        state.leafOwner = false;
        state.ledger = nullptr;
        state.ledgerOwner = NoSlot;
        VisitIdentityEdges(*slot, visitor);
    }

    // 관찰자 공간 예약(처리 표시 아래 잠금 밖). 실패하면 공개 없이 비공개 상태로 돌아간다.
    if (state.error == ErrorCode::None && options_.referenceObserver != nullptr) {
        if (auto reserved = ReserveTally(tally, ReferenceChangeCause::Commit, transaction);
            !reserved) {
            state.error = reserved.Error();
        }
    }

    {
        platform::LockGuard lock(gate_);
        processing_ = false;
        if (state.error == ErrorCode::None && options_.referenceObserver != nullptr &&
            MissingKeysLocked(tally, ReferenceChangeCause::Commit, transaction) >
                ObserverFreeLocked()) {
            state.error = ErrorCode::Busy;
        }

        if (state.error != ErrorCode::None) {
            return Result<void>::Failure(state.error);
        }

        // 이름 색인 항목은 Create 때 이미 들어 있으므로 상태만 바꾼다(조회가 Alive부터 보게 된다).
        for (usize index = 0; index < staged.Size(); ++index) {
            Slot* slot = FindSlot(staged[index]);
            slot->SetState(tally_, SlotState::Alive);
            slot->transaction = 0;
            BitsOf(slot->index).live |= uint64(1) << (slot->index % SlotsPerChunk);
        }

        // 클러스터 가입(13.4절): 새 루트의 클러스터를 먼저 만들고 가입 객체를 넣은 뒤, 소속을 모두
        // 정한 상태에서 staged 간선과 outer를 센다. 공개 전 객체는 같은 트랜잭션의 staged 간선만
        // 받으므로 가입 때문에 분류가 바뀌는 기존 간선이 없다.
        if (permanent) {
            if (permanentCluster_ == NoSlot) {
                permanentCluster_ = CreateClusterLocked(NoSlot, false);
                clusters_[permanentCluster_].permanent = true;
                // 영구 영역은 언제나 표식된 클러스터다(AdvanceGcEpochLocked가 cycle마다 맞춘다).
                clusterEpochs_[permanentCluster_] = gcEpoch_;
            }

            for (usize index = 0; index < staged.Size(); ++index) {
                JoinClusterLocked(permanentCluster_, staged[index].slot);
            }
        }

        if (clustersEnabled_) {
            for (usize index = 0; index < staged.Size(); ++index) {
                const uint32 slot = staged[index].slot;
                if (ClusterStateOf(slot) == ClusterPending && ClusterInCountOf(slot) == NoSlot) {
                    // 패키지 클러스터는 고리가 소유라 소속 불변식 검증 대상이 아니다(13.8절).
                    (void)CreateClusterLocked(slot, !packageClusters);
                }
            }

            for (usize index = 0; index < staged.Size(); ++index) {
                const uint32 slot = staged[index].slot;
                if (ClusterStateOf(slot) == ClusterPending) {
                    const uint32 root = ClusterInCountOf(slot);
                    JoinClusterLocked(ClusterStateOf(root) & ClusterIdMask, slot);
                }
            }

            for (usize at = 0; at + 1 < ledger.Size(); at += 2) {
                LedgerEdgeLocked(ledger[at], ledger[at + 1], true);
            }

            for (usize index = 0; index < staged.Size(); ++index) {
                const uint32 slot = staged[index].slot;
                const uint32 outer = HotOf(slot).outer;
                if ((ClusterStateOf(slot) & ClusterMember) != 0 && outer != NoSlot) {
                    LedgerEdgeLocked(slot, outer, true);
                }
            }
        }

        for (usize index = 0; index < staged.Size(); ++index) {
            Slot* slot = FindSlot(staged[index]);
            const uint64 bit = uint64(1) << (slot->index % SlotsPerChunk);
            GcBits& bits = BitsOf(slot->index);
            const bool member = (bits.clustered & bit) != 0;
            if (gcPhase_ == GcPhase::Marking) {
                if (ShadeLocked(staged[index]) != ErrorCode::None) {
                    platform::Fatal("Preallocated newborn mark queue exhausted");
                }

                // 13.6절: 이미 처리된 클러스터에 가입한 소속은 shade가 클러스터에 닿아 끝나므로
                // trace되지 않는다. staged 간선의 생존 대상은 쓸 때 shade됐고, 생존 outer는 여기서
                // shade한다(같은 트랜잭션의 outer도 이 루프가 shade한다).
                if (member && !slot->outer.IsNull() &&
                    ShadeLocked(slot->outer) != ErrorCode::None) {
                    platform::Fatal("Preallocated newborn mark queue exhausted");
                }

                ++cycleGc_.examined;
            } else if (gcPhase_ == GcPhase::Sweeping && !member) {
                // cutoff 이후 태어난 객체는 이전 cohort의 white 후보가 아니다. Commit 검증이
                // 기존 white 대상·Outer 연결을 거부하므로 새 객체를 이번 cycle에서 trace할 필요가
                // 없다. 소속은 clustered 비트로 sweep에서 빠진다(그 클러스터는 표식됐다).
                bits.mark |= bit;
            }
        }

        // root 증가와 관찰자 기록(transaction = 이 트랜잭션)은 공개와 같은 잠금 안이다.
        for (usize index = 0; index < roots.Size(); ++index) {
            Slot* slot = FindSlot(roots[index]);
            if (slot->roots++ == 0) {
                AddRootListLocked(*slot);
            }

            ++tally_.roots;
        }

        if (options_.referenceObserver != nullptr) {
            ApplyTallyLocked(tally, ReferenceChangeCause::Commit, transaction);
        }

        --constructions_;
    }

    for (usize index = 0; index < roots.Size(); ++index) {
        out[index] = RootRef<Object>(this, roots[index]);
    }

    return Result<void>::Success();
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

    // 생성 저널을 역순으로 처리해 나중에 만든 객체를 먼저 정리한다. 저널의 슬롯만 회수한다(다른
    // Retired 슬롯은 DrainRetired·GC가 회수한다).
    // 소멸자는 Registry 잠금 밖에서 호출하되 processing_으로 변경 재진입을 막는다.
    for (usize remaining = staged.Size(); remaining != 0; --remaining) {
        ReclaimWork work;
        {
            platform::LockGuard lock(gate_);
            Slot* slot = FindSlot(staged[remaining - 1]);
            if (!slot || slot->state != SlotState::Reserved || slot->transaction != transaction) {
                platform::Fatal("Corrupt construction journal");
            }

            // 클러스터 가입 지정을 지운다(공개 전 객체만 가진다).
            if (clustersEnabled_) {
                ClearClusterSlotLocked(slot->index);
            }

            // 역순이라 같은 트랜잭션 자식이 먼저 떨어지므로 부모는 자식이 없을 때 회수된다.
            slot->SetState(tally_, SlotState::Retired);
            DetachIdentityLocked(*slot);
            if (!IsReclaimable(*slot)) {
                // 공개 전 객체는 보유 계수가 없고 자식(같은 트랜잭션)은 먼저 회수되어 여기에 오지
                // 않는다. 그래도 남으면 회수 대상 목록에 넣어 DrainRetired·GC가 회수하게 한다.
                LinkRetiredLocked(*slot, RetiredQueue::Pending);
                continue;
            }

            work = TakeForReclaimLocked(*slot);
        }

        FinishReclaim(work);
    }

    {
        platform::LockGuard lock(gate_);
        --constructions_;
        processing_ = false;
    }
}

} // namespace rocks
