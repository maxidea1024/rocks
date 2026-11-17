#include "ObjectRegistryInternal.h"

#include <Base/Compiler.h>
#include <Core/Diagnostics/Log.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>
#include <Task/WorkerScheduler.h>

// ObjectRegistry GC다. 전체 수집(Collect)과 증분 수집(Begin·Step·Cancel), 표식(MarkSerial·삽입
// 장벽 ShadeLocked·cutoff 판정), Registry가 방문하는 신원 간선(VisitIdentityEdges·TraceIndex),
// sweep(RetireLocked), 회수 대상 목록(Pending·Deferred·Ready)과 회수(ReclaimEligible·
// DrainRetired), GC 밀집 자료(GC 페이지·타입 표·루트 목록)를 둔다.
//
// GC 밀집 자료(GC-S3, Docs/Design/GarbageCollectionScale.ko.md): marker는 Slot(192 B)을 읽지 않고
// 슬롯 번호로 GC 페이지의 16 B hot 레코드(object·Outer 번호·타입 번호)와 chunk마다의 비트 묶음
// (live·mark·leaf)만 읽는다. 작업 큐는 슬롯 번호(uint32)다. 간선의 세대 검사는 Debug만 한다(강한
// 간선은 언제나 살아 있다는 불변식을 Debug가 확인한다). Release·Development는 domain·슬롯 범위·
// live 비트만 본다.
namespace rocks {

using namespace registry_detail;

void ObjectRegistry::VisitIdentityEdges(const Slot& slot, TraceVisitor& visitor) const noexcept {
    visitor.Visit(slot.outer);
    // 패키지 정책: 자식 고리가 export 표다. 처리 표시 아래라 고리가 바뀌지 않고 열린 생성
    // 트랜잭션이 있으면 GC가 진행하지 않으므로 Reserved 자식은 Commit 검증(같은 트랜잭션)에서만
    // 만난다.
    if (options_.policy.packageNativeType != nullptr) {
        for (const Slot* child = slot.firstChild; child != nullptr; child = child->nextSibling) {
            visitor.Visit(HandleOf(*child));
        }
    }
}

namespace {

// 직렬 marker가 시계를 읽는 간격(trace한 객체 수)이다. 시간 예산은 soft deadline이라 이 간격만큼
// 넘을 수 있다.
constexpr uint32 TimeCheckInterval = 128;
// sweep이 시계를 읽는 간격(chunk 수)이다.
constexpr uint32 SweepTimeCheckInterval = 64;
// 작업 큐에서 몇 칸 앞의 hot 레코드·객체 본체를 미리 읽는지다(3.4절 모델의 16·8칸). 참조 배열
// 버퍼는 본체 배치를 알아야 하므로(M3) 미리 읽지 않는다.
constexpr usize HotPrefetchDistance = 16;
constexpr usize ObjectPrefetchDistance = 8;
// 참조 위치 표(M3)의 배열 칸 버퍼를 몇 칸 앞에서 미리 읽는지다(본체는 8칸 앞에서 이미 당겼다).
constexpr usize ArrayPrefetchDistance = 4;
// 병렬 slice의 꼬리 시간 보정이 시간 예산에서 가져갈 수 있는 최대 몫(예산 / 이 값)이다. 선점 같은
// 일회성 지연이 다음 slice의 표식 시간을 지나치게 줄이지 않게 한다.
constexpr uint64 ParallelTailBudgetDivisor = 4;
// GC 자료 할당 태그다.
constexpr uint32 GcPageTag = 0x47435047u;      // 'GCPG'
constexpr uint32 GcPageTableTag = 0x47435054u; // 'GCPT'
constexpr uint32 GcTypeTag = 0x47435459u;      // 'GCTY'
constexpr uint32 RootListTag = 0x4743524cu;    // 'GCRL'
constexpr uint32 GcBlockTag = 0x4743424bu;     // 'GCBK'

// GC 사이클 통계 로그 카테고리다(Debug 레벨 — 객체마다 찍지 않는다).
inline constexpr LogCategory LogObjectGC{"Object.GC"};

void LogCycle(const char* kind, const GcReport& report) noexcept {
    Log::Debug(LogObjectGC,
               "{Kind} finished: examined {Examined}, marked {Marked}, retired {Retired}, "
               "reclaimed {Reclaimed}, {Milliseconds:.3f} ms",
               kind, report.examined, report.marked, report.retired, report.reclaimed,
               static_cast<double>(report.durationNanoseconds) * 1e-6);
}

uint64 LoadMark(const uint64& word) noexcept {
    return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&word);
}

void StoreMark(uint64& word, uint64 value) noexcept {
    platform::AtomicStore<platform::MemoryOrder::Relaxed>(&word, value);
}

uint64 BitOf(uint32 index) noexcept {
    return uint64(1) << (index % SlotsPerChunk);
}

} // namespace

ErrorCode ObjectRegistry::MarkIndex(uint32 index, Array<uint32>& work, uint32& marked) noexcept {
    // 잎 객체는 표식만 하고 Outer 사슬을 따라 올라간다(Outer는 생성 때 고정이고 inner가 사는 동안
    // 산다).
    for (;;) {
        GcBits& bits = BitsOf(index);
        const uint64 bit = BitOf(index);
        if ((bits.live & bit) == 0) {
            return ErrorCode::StaleHandle;
        }

        // 클러스터 소속은 그 클러스터를 표식한다(GC-S7 — 루트 → 소속 암묵 간선). 이번 cycle에 이미
        // 클러스터 표식을 본 소속이면 비트 하나로 끝난다(장부 상태·epoch를 읽지 않는다).
        if ((bits.clustered & bit) != 0) {
            const uint64 seen = LoadMark(bits.clusterSeen);
            if ((seen & bit) != 0) {
                return ErrorCode::None;
            }

            StoreMark(bits.clusterSeen, seen | bit);
            return MarkCluster(MemberClusterOf(index), work, marked);
        }

        const uint64 mark = LoadMark(bits.mark);
        if ((mark & bit) != 0) {
            return ErrorCode::None;
        }

        StoreMark(bits.mark, mark | bit);
        ++marked;
        if ((bits.leaf & bit) == 0) {
            // 작업 큐는 모든 슬롯을 담을 용량을 미리 확보했다(Begin·생성·Collect) — 할당하지
            // 않는다.
            auto pushed = work.TryPushBack(index);
            return pushed ? ErrorCode::None : pushed.Error();
        }

        index = HotOf(index).outer;
        if (index == NoSlot) {
            return ErrorCode::None;
        }
    }
}

bool ObjectRegistry::IsMarkableEdge(ObjectHandle handle, uint64 slots) const noexcept {
    if (handle.registry != domain_ || handle.slot >= slots) {
        return false;
    }

    if constexpr (IsDebugBuild) {
        // 강한 간선은 언제나 살아 있는 객체를 가리킨다(세대까지 맞다). Release는 세대를 보지
        // 않으므로 (GC-S3) 회수된 객체의 옛 handle을 Debug가 여기서 잡는다. 어긋나면 Trace나
        // Registry의 계약 위반이다.
        const Slot* slot = FindSlot(handle);
        if (!slot ||
            !(slot->state == SlotState::Alive || slot->state == SlotState::DestroyRequested)) {
            return false;
        }
    }

    return true;
}

// 배치 간선 처리의 공통 본문이다. 대상의 비트 묶음을 먼저 모두 미리 읽은 뒤 방문 순서대로 검사·
// 표식·enqueue한다(순서는 간선마다 처리할 때와 같다).
void ObjectRegistry::MarkHandles(MarkState& state, const ObjectHandle* handles,
                                 uint32 count) noexcept {
    const uint64 slots = uint64(chunkCount_) * SlotsPerChunk;
    for (uint32 index = 0; index < count; ++index) {
        if (handles[index].slot < slots) {
            platform::PrefetchRead(&BitsOf(handles[index].slot));
        }
    }

    for (uint32 index = 0; index < count; ++index) {
        if (!IsMarkableEdge(handles[index], slots)) {
            state.error = ErrorCode::StaleHandle;
            return;
        }

        if (state.counters != nullptr) {
            ++state.counters->edges;
            const uint32 target = handles[index].slot;
            if ((BitsOf(target).clustered & BitOf(target)) != 0) {
                ++state.counters->clusteredEdges;
                state.counters->sameClusterEdges +=
                    ClusterStateOf(target) == state.ownerClusterState ? 1u : 0u;
            }
        }

        const ErrorCode marked = MarkIndex(handles[index].slot, *state.work, state.marked);
        if (marked != ErrorCode::None) {
            state.error = marked;
            return;
        }
    }
}

void ObjectRegistry::MarkBatch(void* pointer, const ObjectHandle* handles, uint32 count) noexcept {
    auto& state = *static_cast<MarkState*>(pointer);
    if (state.error == ErrorCode::None) {
        state.registry->MarkHandles(state, handles, count);
    }
}

void ObjectRegistry::VisitStrongEdges(const GcType& type, const Object& object,
                                      TraceVisitor& visitor) const noexcept {
    if (type.layout != GcType::Layout::Table) {
        type.trace(object, visitor);
        return;
    }

    const auto* base = reinterpret_cast<const unsigned char*>(&object);
    for (uint16 at = 0; at < type.slotCount; ++at) {
        const detail::ReferenceSlot& slot = type.slots[at];
        if (slot.kind == detail::ReferenceSlotKind::Single) {
            visitor.Visit(*reinterpret_cast<const ObjectHandle*>(base + slot.offset));
            continue;
        }

        const auto& storage =
            *reinterpret_cast<const detail::ObjectRefArrayStorage*>(base + slot.offset);
        for (uint32 element = 0; element < storage.size; ++element) {
            visitor.Visit(storage.data[element]);
        }
    }

    if constexpr (IsDebugBuild) {
        // 표가 trace와 같은 간선을 같은 순서로 방문하는지 대조한다(M3 — 표가 간선 하나를 빠뜨리면
        // 회수 후 사용이 된다).
        struct Compare {
            const GcType* type;
            const unsigned char* base;
            uint16 slot = 0;
            uint32 element = 0;
            bool diverged = false;

            ObjectHandle Next() noexcept {
                while (slot < type->slotCount) {
                    const detail::ReferenceSlot& at = type->slots[slot];
                    if (at.kind == detail::ReferenceSlotKind::Single) {
                        ++slot;
                        const ObjectHandle handle =
                            *reinterpret_cast<const ObjectHandle*>(base + at.offset);
                        if (!handle.IsNull()) {
                            return handle;
                        }

                        continue;
                    }

                    const auto& storage =
                        *reinterpret_cast<const detail::ObjectRefArrayStorage*>(base + at.offset);
                    while (element < storage.size) {
                        const ObjectHandle handle = storage.data[element++];
                        if (!handle.IsNull()) {
                            return handle;
                        }
                    }

                    element = 0;
                    ++slot;
                }

                return {};
            }
        };

        Compare compare{&type, base};
        TraceVisitor check(
            &compare, +[](void* pointer, ObjectHandle handle) noexcept {
                auto& state = *static_cast<Compare*>(pointer);
                if (!handle.IsNull() && !(state.Next() == handle)) {
                    state.diverged = true;
                }
            });
        type.trace(object, check);
        if (compare.diverged || !compare.Next().IsNull()) {
            platform::Fatal("Reference layout table diverged from the type Trace");
        }
    }
}

void ObjectRegistry::PrefetchArrays(uint32 index) const noexcept {
    const GcHot& hot = HotOf(index);
    const GcType& type = gcTypes_[hot.type];
    if (!type.hasArray || hot.object == nullptr) {
        return;
    }

    const auto* base = reinterpret_cast<const unsigned char*>(hot.object);
    for (uint16 at = 0; at < type.slotCount; ++at) {
        if (type.slots[at].kind == detail::ReferenceSlotKind::Array) {
            const auto& storage = *reinterpret_cast<const detail::ObjectRefArrayStorage*>(
                base + type.slots[at].offset);
            if (storage.data != nullptr) {
                platform::PrefetchRead(storage.data);
            }
        }
    }
}

void ObjectRegistry::PrepareReferenceLayout(uint16 index, const Object& object) noexcept {
    GcType& type = gcTypes_[index];
    if (type.layout != GcType::Layout::Unknown) {
        return;
    }

    type.layout = GcType::Layout::Trace;
    if (type.referenceLayout == nullptr || !options_.referenceTables) {
        return;
    }

    detail::ReferenceLayoutBuilder builder(&object);
    type.referenceLayout(object, builder);
    if (!builder.Supported()) {
        return;
    }

    // 칸이 없는 타입은 빈 표다(표식이 trace를 부르지 않는다).
    if (builder.Count() != 0) {
        auto memory = allocator_.Allocate(sizeof(detail::ReferenceSlot) * builder.Count(),
                                          alignof(detail::ReferenceSlot), GcTypeTag);
        if (!memory) {
            // 표 할당 실패는 trace로 남는다(정확성은 같다).
            return;
        }

        type.slots = static_cast<detail::ReferenceSlot*>(memory.Value());
        for (uint32 at = 0; at < builder.Count(); ++at) {
            type.slots[at] = builder.Slots()[at];
            type.hasArray =
                type.hasArray || builder.Slots()[at].kind == detail::ReferenceSlotKind::Array;
        }
    }

    type.slotCount = static_cast<uint16>(builder.Count());
    type.layout = GcType::Layout::Table;
}

void ObjectRegistry::TraceIndex(uint32 index, MarkState& state, TraceVisitor& visitor) noexcept {
    const GcHot& hot = HotOf(index);
    if (state.counters != nullptr) {
        ++state.counters->tracedObjects;
        state.ownerClusterState = clustersEnabled_ && (BitsOf(index).clustered & BitOf(index)) != 0
                                      ? ClusterStateOf(index)
                                      : 0;
    }

    VisitStrongEdges(gcTypes_[hot.type], *hot.object, visitor);
    // 객체마다 비워 큐 순서를 간선마다 처리할 때와 같게 둔다(Trace 간선 → Outer → 자식).
    visitor.Flush();
    if (state.error != ErrorCode::None) {
        return;
    }

    // inner → outer는 Registry가 방문하는 강한 간선이다.
    if (hot.outer != NoSlot) {
        const ErrorCode outer = MarkIndex(hot.outer, *state.work, state.marked);
        if (outer != ErrorCode::None) {
            state.error = outer;
            return;
        }
    }

    // 패키지 정책: outer → inner(자식 고리)도 강한 간선이다. 고리는 cold Slot에 둔다(GC-S3 결정 —
    // 패키지 정책 Registry만 읽는다). 처리 표시 아래라 고리가 바뀌지 않는다.
    if (options_.policy.packageNativeType != nullptr) {
        for (const Slot* child = SlotAt(index)->firstChild; child != nullptr;
             child = child->nextSibling) {
            const ErrorCode marked = MarkIndex(child->index, *state.work, state.marked);
            if (marked != ErrorCode::None) {
                state.error = marked;
                return;
            }
        }
    }
}

uint32 ObjectRegistry::MarkSerial(Array<uint32>& work, usize& cursor, MarkState& state,
                                  uint32 maxObjects, uint64 deadline) noexcept {
    TraceVisitor visitor(&state, &ObjectRegistry::MarkBatch);
    uint32 processed = 0;
    while (cursor < work.Size() && processed < maxObjects && state.error == ErrorCode::None) {
        if (deadline != 0 && processed != 0 && processed % TimeCheckInterval == 0 &&
            platform::MonotonicNanoseconds() >= deadline) {
            break;
        }

        // 미스 사슬(hot 레코드 → 본체)을 큐 앞쪽에서 미리 당긴다. 큐의 번호는 표식할 때 검증했다.
        // 클러스터 항목(ClusterItem)은 슬롯이 아니라 미리 읽지 않는다.
        const usize size = work.Size();
        if (cursor + HotPrefetchDistance < size &&
            (work[cursor + HotPrefetchDistance] & ClusterItem) == 0) {
            platform::PrefetchRead(&HotOf(work[cursor + HotPrefetchDistance]));
        }

        if (cursor + ObjectPrefetchDistance < size &&
            (work[cursor + ObjectPrefetchDistance] & ClusterItem) == 0) {
            platform::PrefetchRead(HotOf(work[cursor + ObjectPrefetchDistance]).object);
        }

        if (cursor + ArrayPrefetchDistance < size &&
            (work[cursor + ArrayPrefetchDistance] & ClusterItem) == 0) {
            PrefetchArrays(work[cursor + ArrayPrefetchDistance]);
        }

        const uint32 item = work[cursor++];
        if ((item & ClusterItem) != 0) {
            ProcessCluster(item & ~ClusterItem, state);
        } else {
            TraceIndex(item, state, visitor);
        }

        ++processed;
    }

    return processed;
}

bool ObjectRegistry::IsCutoff(const Slot* slot) const noexcept {
    // 클러스터 소속은 클러스터 표식으로 판정한다(13.5절). cutoff 때 표식되지 않은 클러스터를
    // 해체하므로 Sweeping 중 소속이면 그 클러스터는 표식됐다.
    if (!slot || gcPhase_ != GcPhase::Sweeping ||
        !(slot->state == SlotState::Alive || slot->state == SlotState::DestroyRequested)) {
        return false;
    }

    const GcBits& bits = BitsOf(slot->index);
    return ((LoadMark(bits.mark) | bits.clustered) & BitOf(slot->index)) == 0;
}

ErrorCode ObjectRegistry::ShadeLocked(ObjectHandle handle) noexcept {
    if (gcPhase_ != GcPhase::Marking || handle.IsNull()) {
        return ErrorCode::None;
    }

    Slot* slot = FindSlot(handle);
    if (!slot || !(slot->state == SlotState::Alive || slot->state == SlotState::DestroyRequested)) {
        return ErrorCode::StaleHandle;
    }

    if ((LoadMark(BitsOf(slot->index).mark) & BitOf(slot->index)) != 0) {
        return ErrorCode::None;
    }

    // 장벽은 장벽 큐에만 넣는다(slice 시작에 표식 큐로 옮긴다 — GC-S4). Reserve는 Begin/
    // CreatePending에서 gate 밖에 수행한다. barrier가 allocator 콜백을 실행하지 않도록 capacity를
    // 먼저 검사하며 불변식 위반은 실패로 반환한다.
    if (gcShade_.Size() == gcShade_.Capacity()) {
        return ErrorCode::CapacityExceeded;
    }

    return MarkIndex(slot->index, gcShade_, cycleGc_.marked);
}

void ObjectRegistry::DrainShadeQueue() noexcept {
    for (usize index = 0; index < gcShade_.Size(); ++index) {
        // 표식 큐도 모든 슬롯을 담을 용량이 있다(회색 객체는 cycle마다 슬롯당 한 번이다).
        if (!gcWork_.TryPushBack(gcShade_[index])) {
            platform::Fatal("Preallocated GC work queue exhausted");
        }
    }

    gcShade_.Clear();
}

void ObjectRegistry::AddRootListLocked(Slot& slot) noexcept {
    if (rootCount_ == rootCapacity_) {
        platform::Fatal("Root list capacity is below the slot count");
    }

    slot.rootPosition = rootCount_;
    rootList_[rootCount_++] = slot.index;
}

void ObjectRegistry::RemoveRootListLocked(Slot& slot) noexcept {
    const uint32 position = slot.rootPosition;
    const uint32 last = rootList_[--rootCount_];
    rootList_[position] = last;
    SlotAt(last)->rootPosition = position;
    slot.rootPosition = NoSlot;
}

Result<uint16> ObjectRegistry::AcquireGcType(const RuntimeTypeOps& ops) noexcept {
    const void* key = &ops;
    const bool leaf = !ops.hasStrongReferences && options_.policy.packageNativeType == nullptr;
    for (uint32 index = 1; index < gcTypeCount_; ++index) {
        GcType& type = gcTypes_[index];
        if (type.key == key) {
            // 같은 주소를 새 레코드가 쓰는 경우(옛 레코드 해제 뒤)를 위해 값을 다시 채운다. 참조
            // 위치 표는 함수가 바뀌었으면 다시 정한다(그때는 옛 타입의 객체가 남아 있지 않다).
            type.trace = ops.trace;
            type.parallelTraceSafe = ops.parallelTraceSafe;
            type.leaf = leaf;
            if (type.referenceLayout != ops.referenceLayout) {
                ReleaseRegistryMemory(allocator_, type.slots);
                type.slots = nullptr;
                type.slotCount = 0;
                type.hasArray = false;
                type.layout = GcType::Layout::Unknown;
                type.referenceLayout = ops.referenceLayout;
            }

            return Result<uint16>::Success(static_cast<uint16>(index));
        }
    }

    if (gcTypeCount_ > NumericLimits<uint16>::Max()) {
        return Result<uint16>::Failure(ErrorCode::CapacityExceeded);
    }

    if (gcTypeCount_ == gcTypeCapacity_) {
        const uint32 capacity = gcTypeCapacity_ == 0 ? 16 : gcTypeCapacity_ * 2;
        auto memory = allocator_.Allocate(sizeof(GcType) * capacity, alignof(GcType), GcTypeTag);
        if (!memory) {
            return Result<uint16>::Failure(memory.Error());
        }

        auto* grown = static_cast<GcType*>(memory.Value());
        for (uint32 index = 0; index < capacity; ++index) {
            ::new (&grown[index]) GcType(index < gcTypeCount_ ? gcTypes_[index] : GcType{});
        }

        ReleaseRegistryMemory(allocator_, gcTypes_);
        gcTypes_ = grown;
        gcTypeCapacity_ = capacity;
        // 0번은 비어 있는 항목이다(hot 레코드의 타입 없음).
        gcTypeCount_ = gcTypeCount_ == 0 ? 1 : gcTypeCount_;
    }

    GcType& type = gcTypes_[gcTypeCount_];
    type.key = key;
    type.trace = ops.trace;
    type.referenceLayout = ops.referenceLayout;
    type.parallelTraceSafe = ops.parallelTraceSafe;
    type.leaf = leaf;
    return Result<uint16>::Success(static_cast<uint16>(gcTypeCount_++));
}

Result<void> ObjectRegistry::PrepareGcGrowth(uint64 slots, GcGrowth& growth) noexcept {
    // 페이지: 새 chunk가 새 페이지에 들면 페이지 하나와(표가 가득하면) 두 배 표를 받는다.
    const uint64 pages = (slots + GcPageSlots - 1) / GcPageSlots;
    if (pages > gcPageCount_) {
        auto page = allocator_.Allocate(sizeof(GcPage), alignof(GcPage), GcPageTag);
        if (!page) {
            return Result<void>::Failure(page.Error());
        }

        growth.page = ::new (page.Value()) GcPage();
        if (clustersEnabled_) {
            // 클러스터 장부 칸(GC-S7)도 페이지와 함께 잠금 밖에서 받는다.
            auto cells = allocator_.Allocate(sizeof(ClusterCell) * GcPageSlots,
                                             alignof(ClusterCell), GcPageTag);
            if (!cells) {
                return Result<void>::Failure(cells.Error());
            }

            auto* table = static_cast<ClusterCell*>(cells.Value());
            for (uint32 index = 0; index < GcPageSlots; ++index) {
                ::new (&table[index]) ClusterCell();
            }

            growth.page->cells = table;
        }

        if (gcPageCount_ == gcPageCapacity_) {
            const uint32 capacity = gcPageCapacity_ == 0 ? 4 : gcPageCapacity_ * 2;
            auto table =
                allocator_.Allocate(sizeof(GcPage*) * capacity, alignof(GcPage*), GcPageTableTag);
            if (!table) {
                return Result<void>::Failure(table.Error());
            }

            growth.pageTable = static_cast<GcPage**>(table.Value());
            growth.pageTableCapacity = capacity;
        }
    }

    // 루트 목록: 용량을 슬롯 수 이상으로 둔다(gate_ 아래 추가가 할당하지 않는다).
    if (slots > rootCapacity_) {
        uint64 capacity = rootCapacity_ == 0 ? uint64(GcPageSlots) : uint64(rootCapacity_) * 2;
        capacity = capacity > options_.maxSlots ? uint64(options_.maxSlots) : capacity;
        capacity = capacity < slots ? slots : capacity;
        auto list = allocator_.Allocate(sizeof(uint32) * static_cast<usize>(capacity),
                                        alignof(uint32), RootListTag);
        if (!list) {
            return Result<void>::Failure(list.Error());
        }

        growth.rootList = static_cast<uint32*>(list.Value());
        growth.rootCapacity = static_cast<uint32>(capacity);
    }

    return Result<void>::Success();
}

void ObjectRegistry::GrowGcLocked(GcGrowth& growth) noexcept {
    if (growth.pageTable != nullptr) {
        for (uint32 index = 0; index < gcPageCount_; ++index) {
            growth.pageTable[index] = gcPages_[index];
        }

        GcPage** previous = gcPages_;
        gcPages_ = growth.pageTable;
        gcPageCapacity_ = growth.pageTableCapacity;
        // 옛 표는 잠금 밖에서 반환한다(ReleaseGcGrowth).
        growth.pageTable = previous;
    }

    if (growth.page != nullptr) {
        gcPages_[gcPageCount_++] = growth.page;
        growth.page = nullptr;
    }

    if (growth.rootList != nullptr) {
        for (uint32 index = 0; index < rootCount_; ++index) {
            growth.rootList[index] = rootList_[index];
        }

        uint32* previous = rootList_;
        rootList_ = growth.rootList;
        rootCapacity_ = growth.rootCapacity;
        growth.rootList = previous;
    }
}

void ObjectRegistry::ReleaseGcGrowth(GcGrowth& growth) noexcept {
    if (growth.page != nullptr) {
        ReleaseRegistryMemory(allocator_, growth.page->cells);
        growth.page->~GcPage();
        ReleaseRegistryMemory(allocator_, growth.page);
    }

    ReleaseRegistryMemory(allocator_, growth.pageTable);
    ReleaseRegistryMemory(allocator_, growth.rootList);
    growth = {};
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
        capacity = static_cast<usize>(GcQueueBound(uint64(chunkCount_) * SlotsPerChunk));
    }

    gcWork_.Clear();
    gcShade_.Clear();
    auto reserved = gcWork_.TryReserve(capacity);
    if (reserved) {
        reserved = gcShade_.TryReserve(capacity);
    }

    // Debug 장부 검증(RegistryOptions::verifyClusterLedger)은 처리 표시 아래 잠금 밖에서 한다.
    if (reserved) {
        reserved = VerifyClusterLedger();
    }

    platform::LockGuard lock(gate_);
    processing_ = false;
    if (!reserved) {
        return reserved;
    }

    // 표식 비트와 클러스터 표식을 본 소속 비트를 지운다(chunk마다 word 하나씩 — O(슬롯 수 / 64)).
    for (uint32 chunk = 0; chunk < chunkCount_; ++chunk) {
        GcBits& bits = BitsOf(chunk * SlotsPerChunk);
        StoreMark(bits.mark, 0);
        StoreMark(bits.clusterSeen, 0);
    }

    gcPhase_ = GcPhase::Marking;
    gcCursor_ = 0;
    AdvanceGcEpochLocked();
    cycleGc_ = {};
    cycleGc_.examined = tally_.liveObjects + tally_.destroyRequested;
    sweepChunk_ = 0;
    // 루트 목록만 걷는다(O(루트 수)). 루트가 있는 객체는 Retire되지 않으므로 살아 있다.
    for (uint32 position = 0; position < rootCount_; ++position) {
        if (MarkIndex(rootList_[position], gcWork_, cycleGc_.marked) != ErrorCode::None) {
            platform::Fatal("Root list holds a dead slot or the root queue is exhausted");
        }
    }

    cycleGc_.durationNanoseconds = platform::MonotonicNanoseconds() - started;
    cycleGc_.beginNanoseconds = cycleGc_.durationNanoseconds;
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
    gcShade_.Clear();
    ReleaseGrayBlocks();
    ReleaseDissolvedClustersLocked();
    gcCursor_ = 0;
    return Result<void>::Success();
}

bool ObjectRegistry::IsReclaimable(const Slot& slot) const noexcept {
    // 고리에 남은 자식이 있으면 부모 슬롯을 재사용하지 않는다. white 부모가 자식보다 먼저
    // Retire되면 자식들이 같은 sweep에서 스스로 떨어질 때까지 기다린다(그 사이 DrainRetired 회수
    // 포함 — rollback은 저널의 슬롯만 회수하고, 그 자식은 같은 저널의 뒤쪽이라 먼저 떨어진다).
    return slot.state == SlotState::Retired && slot.roots == 0 && slot.borrows == 0 &&
           slot.backendHolds == 0 && slot.encodePins == 0 && slot.firstChild == nullptr;
}

ObjectRegistry::Slot* ObjectRegistry::NextReclaimableLocked() noexcept {
    // 지연 목록의 슬롯은 회수할 수 없다는 불변식이 있다(회수 가능해지는 순간 WakeDeferredLocked가
    // 풀린 목록으로 옮긴다). 그래서 지연 슬롯은 다시 걷지 않는다.
    if (retiredReady_.head != nullptr) {
        ++reclaimProbes_;
        return retiredReady_.head;
    }

    while (Slot* slot = retiredPending_.head) {
        ++reclaimProbes_;
        if (IsReclaimable(*slot)) {
            return slot;
        }

        UnlinkRetiredLocked(*slot);
        LinkRetiredLocked(*slot, RetiredQueue::Deferred);
    }

    return nullptr;
}

bool ObjectRegistry::HasReclaimableLocked() noexcept {
    return NextReclaimableLocked() != nullptr;
}

void ObjectRegistry::LinkRetiredLocked(Slot& slot, RetiredQueue queue) noexcept {
    RetiredList& list = queue == RetiredQueue::Pending    ? retiredPending_
                        : queue == RetiredQueue::Deferred ? retiredDeferred_
                                                          : retiredReady_;
    slot.queue = queue;
    slot.nextInList = nullptr;
    slot.prevInList = list.tail;
    if (list.tail != nullptr) {
        list.tail->nextInList = &slot;
    } else {
        list.head = &slot;
    }

    list.tail = &slot;
}

void ObjectRegistry::UnlinkRetiredLocked(Slot& slot) noexcept {
    RetiredList& list = slot.queue == RetiredQueue::Pending    ? retiredPending_
                        : slot.queue == RetiredQueue::Deferred ? retiredDeferred_
                                                               : retiredReady_;
    if (slot.prevInList != nullptr) {
        slot.prevInList->nextInList = slot.nextInList;
    } else {
        list.head = slot.nextInList;
    }

    if (slot.nextInList != nullptr) {
        slot.nextInList->prevInList = slot.prevInList;
    } else {
        list.tail = slot.prevInList;
    }

    slot.queue = RetiredQueue::None;
    slot.nextInList = nullptr;
    slot.prevInList = nullptr;
}

void ObjectRegistry::RetireLocked(Slot& slot) noexcept {
    // 패키지 정책 Registry의 객체는 Discard로만 죽는다(1.7절). 패키지 정책에서는 RequestDestroy가
    // 없으므로 Alive는 Discard되지 않았다는 뜻이다. 종료(closed_) 뒤의 회수는 세지 않는다.
    if (slot.state == SlotState::Alive && options_.policy.packageNativeType != nullptr &&
        !closed_) {
        if (tally_.retiredWithoutDiscard != NumericLimits<uint32>::Max()) {
            ++tally_.retiredWithoutDiscard;
        }

        if constexpr (IsDebugBuild) {
            if (options_.assertRetiredWithoutDiscard) {
                platform::Fatal("Package-policy object retired without Discard");
            }
        }
    }

    if constexpr (IsDebugBuild) {
        // 소속·detached는 sweep 후보가 아니다(소속은 clustered 비트, detached는 그 클러스터 처리가
        // 표식하거나 cutoff 해체가 비운다).
        if (clustersEnabled_ && ClusterStateOf(slot.index) != 0) {
            platform::Fatal("Retiring an object that is still in a cluster ledger");
        }
    }

    slot.SetState(tally_, SlotState::Retired);
    BitsOf(slot.index).live &= ~BitOf(slot.index);
    DetachIdentityLocked(slot);
    LinkRetiredLocked(slot, RetiredQueue::Pending);
}

void ObjectRegistry::WakeDeferredLocked(Slot& slot) noexcept {
    if (slot.queue == RetiredQueue::Deferred && IsReclaimable(slot)) {
        UnlinkRetiredLocked(slot);
        LinkRetiredLocked(slot, RetiredQueue::Ready);
    }
}

// 병렬 표식 참여자 하나(worker task 또는 소유 스레드)의 지역 상태다.
struct ObjectRegistry::MarkParticipant {
    ParallelMark* shared = nullptr;
    // 지역 큐: 블록 더미의 맨 위 블록에서 넣고 뺀다. 맨 위 아래의 블록은 언제나 가득 차 있다.
    GcBlock* stack = nullptr;
    // worker가 만난 수동 Trace(parallelTraceSafe가 아닌) 객체를 소유 스레드로 보내려고 모으는
    // 블록이다.
    GcBlock* owner = nullptr;
    uint32 marked = 0;
    uint32 traced = 0;
    // 공유 trace 수에 아직 더하지 않은 수다(예산 검사 간격).
    uint32 pending = 0;
    bool isOwner = false;
};

// 한 slice(또는 Collect)의 병렬 표식이다(GC-S4, Docs/Design/GarbageCollectionScale.ko.md).
// 참여자는 worker task W개와 소유 스레드다. 공유 풀은 512칸 블록 목록이고 잠금 하나(gate)로 지킨다
// (설계의 "블록 풀 하나를 잠금으로 보호하는 단순한 방식"). 참여자는 지역 큐가 비면 소유 스레드 큐
// (소유 스레드만)와 공유 풀에서 블록을 가져오고, 둘 다 비면 idle로 기다린다. 모든 참여자가 idle이고
// 두 풀이 비면 끝이다(새 회색 객체는 idle이 아닌 참여자만 만들고, 수동 Trace 객체는 idle 전에
// 넘긴다). 예산(객체 수·시간)이나 오류는 stop을 세워 모두 멈추고 남은 블록을 leftovers로 모은다.
struct ObjectRegistry::ParallelMark {
    ObjectRegistry* registry = nullptr;
    platform::Mutex gate;
    platform::ConditionVariable changed;
    GcBlock* pool = nullptr;
    GcBlock* ownerPool = nullptr;
    GcBlock* leftovers = nullptr;
    GcBlock* freeBlocks = nullptr;
    // 풀의 블록 수·idle 참여자 수다(잠금 안에서 쓰고 밖에서는 relaxed로 읽어 나눔 판단에만 쓴다).
    uint32 poolBlocks = 0;
    uint32 idle = 0;
    uint32 participants = 0;
    bool done = false;
    // atomic: 멈춤 요청, 첫 오류(ErrorCode), 모든 참여자의 trace 수.
    uint32 stop = 0;
    uint32 error = 0;
    uint32 traced = 0;
    uint32 maxObjects = 0;
    uint64 deadline = 0;
    // 예산 검사 간격(참여자마다 trace한 객체 수)이다. 기본 64, 객체 예산이 그보다 작으면 그 값이다.
    uint32 budgetInterval = 64;

    bool Stopped() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&stop) != 0;
    }

    void RequestStop() noexcept {
        platform::AtomicStore<platform::MemoryOrder::Relaxed>(&stop, 1u);
        platform::LockGuard lock(gate);
        changed.NotifyAll();
    }

    void SetError(ErrorCode code) noexcept {
        uint32 expected = 0;
        (void)platform::AtomicCompareExchangeStrong<platform::MemoryOrder::Relaxed,
                                                    platform::MemoryOrder::Relaxed>(
            &error, expected, static_cast<uint32>(code));
        RequestStop();
    }

    GcBlock* TakeBlock() noexcept {
        {
            platform::LockGuard lock(gate);
            if (GcBlock* block = freeBlocks) {
                freeBlocks = block->next;
                block->next = nullptr;
                block->count = 0;
                return block;
            }
        }

        // 여러 worker가 동시에 부를 수 있다(Registry allocator의 다중 스레드 계약 — StepCollection
        // 주석).
        auto memory = registry->allocator_.Allocate(sizeof(GcBlock), alignof(GcBlock), GcBlockTag);
        if (!memory) {
            SetError(memory.Error());
            return nullptr;
        }

        return ::new (memory.Value()) GcBlock();
    }

    void ReleaseBlock(GcBlock* block) noexcept {
        platform::LockGuard lock(gate);
        block->next = freeBlocks;
        freeBlocks = block;
    }

    void Publish(GcBlock* block) noexcept {
        platform::LockGuard lock(gate);
        block->next = pool;
        pool = block;
        platform::AtomicStore<platform::MemoryOrder::Relaxed>(&poolBlocks, poolBlocks + 1);
        changed.NotifyOne();
    }

    void PublishOwner(GcBlock* block) noexcept {
        platform::LockGuard lock(gate);
        block->next = ownerPool;
        ownerPool = block;
        // 기다리는 참여자 중 소유 스레드를 깨워야 한다(누가 소유 스레드인지 골라 깨울 수 없다).
        changed.NotifyAll();
    }

    // 지역 큐에 넣는다. 맨 위 블록이 가득 차면 풀이 모자랄 때(참여자 수보다 적을 때) 그 블록을
    // 풀에 내놓고 새 블록을 받는다.
    void Push(MarkParticipant& self, uint32 index) noexcept {
        GcBlock* top = self.stack;
        if (top == nullptr || top->count == GcBlock::Capacity) {
            if (top != nullptr &&
                platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&poolBlocks) < participants) {
                self.stack = top->next;
                Publish(top);
            }

            GcBlock* block = TakeBlock();
            if (block == nullptr) {
                return;
            }

            block->next = self.stack;
            self.stack = block;
            top = block;
        }

        top->items[top->count++] = index;
    }

    void PushOwner(MarkParticipant& self, uint32 index) noexcept {
        if (self.owner == nullptr || self.owner->count == GcBlock::Capacity) {
            if (self.owner != nullptr) {
                PublishOwner(self.owner);
            }

            self.owner = TakeBlock();
            if (self.owner == nullptr) {
                return;
            }
        }

        self.owner->items[self.owner->count++] = index;
    }

    bool Pop(MarkParticipant& self, uint32& index) noexcept {
        while (GcBlock* top = self.stack) {
            if (top->count != 0) {
                index = top->items[--top->count];
                // 미스 사슬(hot 레코드 → 본체)을 지역 큐 앞쪽(LIFO라 아래 칸)에서 미리 당긴다.
                // 클러스터 항목(ClusterItem)은 슬롯이 아니라 미리 읽지 않는다.
                if (top->count > HotPrefetchDistance &&
                    (top->items[top->count - HotPrefetchDistance] & ClusterItem) == 0) {
                    platform::PrefetchRead(
                        &registry->HotOf(top->items[top->count - HotPrefetchDistance]));
                }

                if (top->count > ObjectPrefetchDistance &&
                    (top->items[top->count - ObjectPrefetchDistance] & ClusterItem) == 0) {
                    platform::PrefetchRead(
                        registry->HotOf(top->items[top->count - ObjectPrefetchDistance]).object);
                }

                if (top->count > ArrayPrefetchDistance &&
                    (top->items[top->count - ArrayPrefetchDistance] & ClusterItem) == 0) {
                    registry->PrefetchArrays(top->items[top->count - ArrayPrefetchDistance]);
                }

                return true;
            }

            self.stack = top->next;
            ReleaseBlock(top);
        }

        return false;
    }

    // 클러스터를 표식한다(atomic 비교 교환 — 이긴 참여자만 클러스터 항목을 넣는다).
    void MarkCluster(MarkParticipant& self, uint32 id) noexcept {
        uint32& slot = registry->clusterEpochs_[id];
        const uint32 epoch = registry->gcEpoch_;
        uint32 expected = platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&slot);
        if (expected == epoch ||
            !platform::AtomicCompareExchangeStrong<platform::MemoryOrder::Relaxed,
                                                   platform::MemoryOrder::Relaxed>(&slot, expected,
                                                                                   epoch)) {
            return;
        }

        self.marked += registry->clusters_[id].members;
        Push(self, id | ClusterItem);
    }

    // 클러스터 처리(병렬): 경계 소속을 지역 큐에 넣고(mark 비트 fetch_or로 한 번) detached를
    // 표식한다. slice 동안 목록은 바뀌지 않고 한 클러스터는 이긴 참여자 하나만 처리한다.
    void ProcessCluster(MarkParticipant& self, uint32 id) noexcept {
        ClusterRecord& record = registry->clusters_[id];
        if (record.dissolved) {
            return;
        }

        for (uint32 at = record.boundaryHead; at != NoSlot;
             at = registry->CellOf(at).boundaryNext) {
            const uint64 bit = BitOf(at);
            GcBits& bits = registry->BitsOf(at);
            if ((LoadMark(bits.mark) & bit) == 0 &&
                (platform::AtomicFetchOr<platform::MemoryOrder::Relaxed>(&bits.mark, bit) & bit) ==
                    0) {
                Push(self, at);
            }
        }

        for (uint32 at = record.detachedHead; at != NoSlot; at = registry->CellOf(at).listNext) {
            const ErrorCode marked = Mark(self, at);
            if (marked != ErrorCode::None) {
                SetError(marked);
                return;
            }
        }

        platform::AtomicStore<platform::MemoryOrder::Relaxed>(&record.processedEpoch,
                                                              registry->gcEpoch_);
    }

    // 표식한다(atomic fetch_or — 이긴 참여자만 지역 큐에 넣는다). 잎은 Outer 사슬을 바로 따라간다.
    ErrorCode Mark(MarkParticipant& self, uint32 index) noexcept {
        for (;;) {
            GcBits& bits = registry->BitsOf(index);
            const uint64 bit = BitOf(index);
            if ((bits.live & bit) == 0) {
                return ErrorCode::StaleHandle;
            }

            if ((bits.clustered & bit) != 0) {
                // 이번 cycle에 클러스터 표식을 본 소속이면 비트 하나로 끝난다(경합해도 같은 비트를
                // 켤 뿐이다).
                uint64& seen = bits.clusterSeen;
                if ((LoadMark(seen) & bit) == 0) {
                    (void)platform::AtomicFetchOr<platform::MemoryOrder::Relaxed>(&seen, bit);
                    MarkCluster(self, registry->MemberClusterOf(index));
                }

                return ErrorCode::None;
            }

            if ((LoadMark(bits.mark) & bit) != 0 ||
                (platform::AtomicFetchOr<platform::MemoryOrder::Relaxed>(&bits.mark, bit) & bit) !=
                    0) {
                return ErrorCode::None;
            }

            ++self.marked;
            if ((bits.leaf & bit) == 0) {
                Push(self, index);
                return ErrorCode::None;
            }

            index = registry->HotOf(index).outer;
            if (index == NoSlot) {
                return ErrorCode::None;
            }
        }
    }

    static void MarkBatchShared(void* pointer, const ObjectHandle* handles, uint32 count) noexcept {
        auto& self = *static_cast<MarkParticipant*>(pointer);
        ParallelMark& shared = *self.shared;
        ObjectRegistry& registry = *shared.registry;
        const uint64 slots = uint64(registry.chunkCount_) * SlotsPerChunk;
        for (uint32 index = 0; index < count; ++index) {
            if (handles[index].slot < slots) {
                platform::PrefetchRead(&registry.BitsOf(handles[index].slot));
            }
        }

        for (uint32 index = 0; index < count; ++index) {
            if (!registry.IsMarkableEdge(handles[index], slots)) {
                shared.SetError(ErrorCode::StaleHandle);
                return;
            }

            const ErrorCode marked = shared.Mark(self, handles[index].slot);
            if (marked != ErrorCode::None) {
                shared.SetError(marked);
                return;
            }
        }
    }

    void Trace(MarkParticipant& self, uint32 index, TraceVisitor& visitor) noexcept {
        const GcHot& hot = registry->HotOf(index);
        registry->VisitStrongEdges(registry->gcTypes_[hot.type], *hot.object, visitor);
        visitor.Flush();
        if (hot.outer != NoSlot) {
            const ErrorCode outer = Mark(self, hot.outer);
            if (outer != ErrorCode::None) {
                SetError(outer);
                return;
            }
        }

        // 패키지 정책의 자식 고리는 slice 동안 바뀌지 않아 여러 참여자가 읽어도 경합이 없다.
        if (registry->options_.policy.packageNativeType != nullptr) {
            for (const Slot* child = registry->SlotAt(index)->firstChild; child != nullptr;
                 child = child->nextSibling) {
                const ErrorCode marked = Mark(self, child->index);
                if (marked != ErrorCode::None) {
                    SetError(marked);
                    return;
                }
            }
        }
    }

    // trace 수를 공유 계수에 더하고 객체 수·시간 예산을 본다.
    void ChargeBudget(MarkParticipant& self) noexcept {
        const uint32 total =
            platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&traced, self.pending) +
            self.pending;
        self.pending = 0;
        if (total >= maxObjects ||
            (deadline != 0 && platform::MonotonicNanoseconds() >= deadline)) {
            RequestStop();
        }
    }

    // 지역 큐가 비었다: 소유 스레드 큐(소유 스레드만)나 공유 풀에서 블록을 받는다. 받으면 true,
    // 끝이나 멈춤이면 false다.
    bool Refill(MarkParticipant& self) noexcept {
        platform::LockGuard lock(gate);
        for (;;) {
            if (Stopped() || done) {
                return false;
            }

            GcBlock** source = self.isOwner && ownerPool != nullptr ? &ownerPool
                               : pool != nullptr                    ? &pool
                                                                    : nullptr;
            if (source != nullptr) {
                GcBlock* block = *source;
                *source = block->next;
                if (source == &pool) {
                    platform::AtomicStore<platform::MemoryOrder::Relaxed>(&poolBlocks,
                                                                          poolBlocks - 1);
                }

                block->next = self.stack;
                self.stack = block;
                return true;
            }

            platform::AtomicStore<platform::MemoryOrder::Relaxed>(&idle, idle + 1);
            // 모두 idle이고 두 풀이 비었다: 회색 객체가 더 생길 수 없다. worker가 소유 스레드 큐를
            // 남긴 채 idle이 되면(소유 스레드가 아직 깨어나 잠금을 다시 잡기 전) 끝이 아니다.
            if (idle == participants && pool == nullptr && ownerPool == nullptr) {
                done = true;
                changed.NotifyAll();
                return false;
            }

            (void)changed.Wait(gate);
            platform::AtomicStore<platform::MemoryOrder::Relaxed>(&idle, idle - 1);
        }
    }

    // 참여자 하나의 표식 루프다.
    void Run(MarkParticipant& self) noexcept {
        TraceVisitor visitor(&self, &ParallelMark::MarkBatchShared);
        for (;;) {
            uint32 index = 0;
            while (!Stopped() && Pop(self, index)) {
                if ((index & ClusterItem) != 0) {
                    ProcessCluster(self, index & ~ClusterItem);
                } else if (!self.isOwner &&
                           !registry->gcTypes_[registry->HotOf(index).type].parallelTraceSafe) {
                    PushOwner(self, index);
                    continue;
                } else {
                    Trace(self, index, visitor);
                }

                ++self.traced;
                if (++self.pending == budgetInterval) {
                    ChargeBudget(self);
                    // 다른 참여자가 기다리고 지역 큐에 가득 찬 블록이 더 있으면 하나 내놓는다.
                    if (platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&idle) != 0 &&
                        self.stack != nullptr && self.stack->next != nullptr) {
                        GcBlock* spare = self.stack->next;
                        self.stack->next = spare->next;
                        Publish(spare);
                    }
                }
            }

            // idle 전에 소유 스레드로 보낼 객체를 넘긴다(끝 판정이 이 블록을 놓치지 않게).
            if (self.owner != nullptr) {
                if (self.owner->count != 0) {
                    PublishOwner(self.owner);
                } else {
                    ReleaseBlock(self.owner);
                }

                self.owner = nullptr;
            }

            if (Stopped() || !Refill(self)) {
                break;
            }
        }

        if (self.pending != 0) {
            ChargeBudget(self);
        }

        // 멈췄으면 남은 지역 블록을 다음 slice 몫으로 모은다.
        platform::LockGuard lock(gate);
        while (GcBlock* block = self.stack) {
            self.stack = block->next;
            block->next = leftovers;
            leftovers = block;
        }
    }
};

void ObjectRegistry::FlattenGrayBlocks() noexcept {
    while (GcBlock* block = gcGrayBlocks_) {
        gcGrayBlocks_ = block->next;
        for (uint32 item = 0; item < block->count; ++item) {
            // 표식 큐는 모든 슬롯을 담을 용량이 있다(회색 객체는 cycle마다 슬롯당 한 번이다).
            if (!gcWork_.TryPushBack(block->items[item])) {
                platform::Fatal("Preallocated GC work queue exhausted");
            }
        }

        block->next = gcFreeBlocks_;
        gcFreeBlocks_ = block;
    }
}

void ObjectRegistry::ReleaseGrayBlocks() noexcept {
    while (GcBlock* block = gcGrayBlocks_) {
        gcGrayBlocks_ = block->next;
        block->count = 0;
        block->next = gcFreeBlocks_;
        gcFreeBlocks_ = block;
    }
}

uint32 ObjectRegistry::MarkParallel(Array<uint32>& work, usize& cursor, MarkState& state,
                                    uint32 maxObjects, uint64 deadline, WorkerScheduler& scheduler,
                                    bool& completed, GcBlock** carried,
                                    GcSliceTimes* times) noexcept {
    completed = false;
    const uint64 setupStarted = platform::MonotonicNanoseconds();
    constexpr uint32 MaxWorkers = 64;
    const uint32 workers =
        scheduler.Stats().workerCount < MaxWorkers ? scheduler.Stats().workerCount : MaxWorkers;
    ParallelMark shared;
    shared.registry = this;
    shared.participants = workers + 1;
    shared.maxObjects = maxObjects;
    shared.deadline = deadline;
    shared.budgetInterval = maxObjects < shared.budgetInterval ? maxObjects : shared.budgetInterval;
    shared.freeBlocks = gcFreeBlocks_;
    gcFreeBlocks_ = nullptr;
    MarkParticipant participants[MaxWorkers + 1];
    for (uint32 index = 0; index <= workers; ++index) {
        participants[index].shared = &shared;
    }

    participants[0].isOwner = true;
    // 씨앗(work[cursor..))을 블록으로 묶어 공유 풀에 둔다. 큐는 비우고 끝에서 남은 것을 다시
    // 담는다.
    for (usize at = cursor; at < work.Size();) {
        GcBlock* block = shared.TakeBlock();
        if (block == nullptr) {
            break;
        }

        while (at < work.Size() && block->count < GcBlock::Capacity) {
            block->items[block->count++] = work[at++];
        }

        block->next = shared.pool;
        shared.pool = block;
        ++shared.poolBlocks;
    }

    work.Clear();
    cursor = 0;
    // 앞 slice가 남긴 회색 블록은 펴지 않고 그대로 풀에 붙인다.
    if (carried != nullptr) {
        while (GcBlock* block = *carried) {
            *carried = block->next;
            block->next = shared.pool;
            shared.pool = block;
            ++shared.poolBlocks;
        }
    }

    WorkerTaskHandle tasks[MaxWorkers];
    uint32 submitted = 0;
    if (!shared.Stopped()) {
        for (; submitted < workers; ++submitted) {
            MarkParticipant* self = &participants[submitted + 1];
            auto body = Function<void(WorkerTaskContext&)>::TryCreate(
                allocator_, [&shared, self](WorkerTaskContext&) noexcept { shared.Run(*self); });
            if (!body) {
                shared.SetError(body.Error());
                break;
            }

            auto accepted = scheduler.Submit(Move(body.Value()));
            if (!accepted) {
                shared.SetError(accepted.Error());
                break;
            }

            tasks[submitted] = Move(accepted.Value());
        }

        if (submitted != workers) {
            // 제출하지 못한 참여자는 끝 판정 수에서 뺀다(이미 멈춤이 걸려 모두 곧 나온다).
            platform::LockGuard lock(shared.gate);
            shared.participants = submitted + 1;
            shared.changed.NotifyAll();
        }
    }

    const uint64 markStarted = platform::MonotonicNanoseconds();
    shared.Run(participants[0]);
    const uint64 drainStarted = platform::MonotonicNanoseconds();
    for (uint32 index = 0; index < submitted; ++index) {
        auto waited = tasks[index].Wait();
        if (!waited || waited.Value() != platform::WaitResult::Signaled) {
            // stack에 남은 표식 상태를 쓰는 worker를 버리고 복귀할 수 없다.
            platform::Fatal("Cannot drain a GC marker task");
        }

        auto status = tasks[index].Status();
        if (!status) {
            shared.SetError(status.Error());
        } else if (status.Value().state != WorkerTaskState::Succeeded) {
            shared.SetError(status.Value().error == ErrorCode::None ? ErrorCode::DependencyFailed
                                                                    : status.Value().error);
        }
    }

    const uint64 requeueStarted = platform::MonotonicNanoseconds();
    // 모든 참여자가 끝났다. 남은 회색 객체(멈춤)를 큐에 다시 담고 블록을 재사용 목록으로 돌린다.
    uint32 traced = 0;
    for (uint32 index = 0; index <= workers; ++index) {
        state.marked += participants[index].marked;
        traced += participants[index].traced;
        if (participants[index].owner != nullptr) {
            participants[index].owner->next = shared.leftovers;
            shared.leftovers = participants[index].owner;
        }
    }

    uint64 gray = 0;
    GcBlock* lists[] = {shared.leftovers, shared.pool, shared.ownerPool};
    for (GcBlock* block : lists) {
        while (block != nullptr) {
            GcBlock* next = block->next;
            gray += block->count;
            if (carried != nullptr && block->count != 0) {
                // 블록째 다음 slice로 넘긴다.
                block->next = *carried;
                *carried = block;
                block = next;
                continue;
            }

            for (uint32 item = 0; item < block->count; ++item) {
                // 큐는 모든 슬롯을 담을 용량을 미리 확보했다(회색 객체는 슬롯마다 한 번이다).
                if (!work.TryPushBack(block->items[item])) {
                    platform::Fatal("Preallocated GC work queue exhausted");
                }
            }

            block->next = shared.freeBlocks;
            shared.freeBlocks = block;
            block = next;
        }
    }

    gcFreeBlocks_ = shared.freeBlocks;
    if (times != nullptr) {
        times->setup += markStarted - setupStarted;
        times->marking += drainStarted - markStarted;
        times->drain += requeueStarted - drainStarted;
        times->requeue += platform::MonotonicNanoseconds() - requeueStarted;
        times->grayCarried = static_cast<uint32>(gray);
    }

    const auto error = static_cast<ErrorCode>(shared.error);
    if (error != ErrorCode::None) {
        state.error = error;
        return traced;
    }

    completed = work.IsEmpty() && shared.done && (carried == nullptr || *carried == nullptr);
    return traced;
}

uint32 ObjectRegistry::RetireWhiteLocked(uint32 chunk) noexcept {
    // 비트 묶음의 live & ~mark가 이 chunk의 white 후보다. 낮은 번호부터 Retire한다(슬롯 순서 유지).
    const GcBits& bits = BitsOf(chunk * SlotsPerChunk);
    uint64 white = bits.live & ~(LoadMark(bits.mark) | bits.clustered);
    uint32 retired = 0;
    while (white != 0) {
        const uint32 offset =
            static_cast<uint32>(platform::PlatformMath::CountTrailingZeros64(white));
        white &= white - 1;
        RetireLocked(chunks_[chunk]->slots[offset]);
        ++retired;
    }

    return retired;
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
        const uint64 maximum = NumericLimits<uint64>::Max();
        deadline =
            budget.maxNanoseconds > maximum - started ? maximum : started + budget.maxNanoseconds;
    }

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

    // 처리 표시 아래에서 gcPhase_·gcWork_·gcCursor_·sweepChunk_·GC 비트는 이 스레드만 바꾼다(장벽·
    // 생성·Discard는 Busy, 다른 스레드의 RootRef 복제도 Busy). 그래서 아래 루프는 잠금 없이 읽고,
    // 다른 스레드가 잠금 아래에서 읽는 전이(cutoff 게시·Retire·Reclaiming)만 잠가서 쓴다.
    uint32 processed = 0;
    const uint64 markStarted = platform::MonotonicNanoseconds();
    MarkState state{this, &gcWork_};
    GcSliceTimes slice;
    bool cutoffPublished = false;
    if (gcPhase_ == GcPhase::Marking) {
        // slice 사이에 장벽이 모은 회색 객체를 표식 큐로 옮긴다(slice 동안은 장벽이 없다 — 처리
        // 표시).
        DrainShadeQueue();
        slice.setup += platform::MonotonicNanoseconds() - markStarted;
        bool completed = false;
        if (scheduler == nullptr) {
            // 앞 병렬 slice가 블록으로 남긴 회색 객체를 직렬 큐로 편다.
            FlattenGrayBlocks();
            const uint64 serialStarted = platform::MonotonicNanoseconds();
            processed = MarkSerial(gcWork_, gcCursor_, state, budget.maxObjects, deadline);
            slice.marking += platform::MonotonicNanoseconds() - serialStarted;
            // 큐가 빈 것을 이 slice의 예산 안에서 보았을 때만 끝난 것으로 본다(예산을 다 쓴 slice는
            // Marking으로 끝나 다음 slice 전에 mutator가 장벽을 거칠 수 있다).
            completed = gcCursor_ == gcWork_.Size() && processed < budget.maxObjects &&
                        (deadline == 0 || platform::MonotonicNanoseconds() < deadline);
        } else {
            // 병렬 slice는 표식이 멈춘 뒤에도 worker 회수·회색 넘기기가 남는다. 앞 slice에서 잰 그
            // 꼬리 시간만큼(예산의 1/ParallelTailBudgetDivisor까지) 표식을 일찍 멈춘다.
            uint64 markDeadline = deadline;
            if (deadline != 0) {
                const uint64 limit = budget.maxNanoseconds / ParallelTailBudgetDivisor;
                const uint64 reserve = gcParallelTail_ < limit ? gcParallelTail_ : limit;
                markDeadline = deadline - reserve > started ? deadline - reserve : deadline;
            }

            processed = MarkParallel(gcWork_, gcCursor_, state, budget.maxObjects, markDeadline,
                                     *scheduler, completed, &gcGrayBlocks_, &slice);
            gcParallelTail_ = slice.drain + slice.requeue;
        }

        cycleGc_.marked += state.marked;
        if (state.error == ErrorCode::None && completed && gcShade_.IsEmpty()) {
            // 모든 장벽이 같은 gate에서 게시되고 slice 동안은 장벽이 없으므로(처리 표시) 빈 큐
            // 판정과 cutoff는 하나의 선형화다. 아직 slot을 Retired로 바꾸기 전에도 white 객체의
            // Resolve·Pin·연결을 차단한다. cutoff를 게시한 slice는 sweep 없이 끝낸다. sweep 단위가
            // chunk라 작은 그래프는 한 slice에 끝나므로, mutator가 cutoff 뒤 아직 분류하지 않은
            // white를 만나는 구간(Sweeping)을 언제나 한 번은 거친다
            // (Docs/Implementation/IncrementalGarbageCollection.ko.md의 cutoff 계약).
            platform::LockGuard lock(gate_);
            // 표식되지 않은 클러스터를 해체해 소속을 white 후보로 내보낸다(13.5절 — 패키지
            // RootRef를 Discard 없이 놓은 버그 경로). 큐가 비었으므로 해체한 레코드를 바로 재사용할
            // 수 있다.
            DissolveUnmarkedClustersLocked();
            // 경계 비율 안전장치: 남은 클러스터는 모두 표식됐으므로 해체가 소속에 mark를 켠다.
            cycleGc_.flattenedClusters += FlattenBoundaryClustersLocked();
            ReleaseDissolvedClustersLocked();
            gcPhase_ = GcPhase::Sweeping;
            sweepChunk_ = chunkCount_;
            cutoffPublished = true;
        }
    }

    if (state.error != ErrorCode::None) {
        platform::LockGuard lock(gate_);
        gcPhase_ = GcPhase::Idle;
        gcWork_.Clear();
        gcShade_.Clear();
        ReleaseGrayBlocks();
        ReleaseDissolvedClustersLocked();
        gcCursor_ = 0;
        processing_ = false;
        return Result<GcProgress>::Failure(state.error);
    }

    const uint64 sweepStarted = platform::MonotonicNanoseconds();
    cycleGc_.markNanoseconds += sweepStarted - markStarted;
    // sweep의 작업 단위는 chunk(64칸) 하나다(GC-S2). chunk의 white 후보는 비트 묶음의 live &
    // ~mark로 잠금 없이 판정하고(비트는 이 스레드만 바꾼다) white가 있는 chunk만 잠가 Retire한다.
    // cutoff 뒤 Sweeping 중 생성이 붙인 새 chunk(더 큰 번호)는 방문하지 않는다. 그 객체는 표식되어
    // 이번 cycle의 white 후보가 아니다. 순서는 새 chunk부터, chunk 안은 낮은 번호부터다.
    uint32 swept = 0;
    while (!cutoffPublished && gcPhase_ == GcPhase::Sweeping && processed < budget.maxObjects) {
        if (deadline != 0 && swept != 0 && swept % SweepTimeCheckInterval == 0 &&
            platform::MonotonicNanoseconds() >= deadline) {
            break;
        }

        if (sweepChunk_ == 0) {
            platform::LockGuard lock(gate_);
            gcPhase_ = GcPhase::Reclaiming;
            break;
        }

        const uint32 chunk = --sweepChunk_;
        const GcBits& bits = BitsOf(chunk * SlotsPerChunk);
        if ((bits.live & ~(LoadMark(bits.mark) | bits.clustered)) != 0) {
            platform::LockGuard lock(gate_);
            cycleGc_.retired += RetireWhiteLocked(chunk);
        }

        ++processed;
        ++swept;
    }

    const uint64 reclaimStarted = platform::MonotonicNanoseconds();
    cycleGc_.sweepNanoseconds += reclaimStarted - sweepStarted;
    slice.sweep = reclaimStarted - sweepStarted;
    if (gcPhase_ == GcPhase::Reclaiming && processed < budget.maxObjects &&
        (deadline == 0 || platform::MonotonicNanoseconds() < deadline)) {
        const uint32 reclaimed = ReclaimEligible(budget.maxObjects - processed, deadline);
        cycleGc_.reclaimed += reclaimed;
        processed += reclaimed;
    }

    const uint64 finished = platform::MonotonicNanoseconds();
    GcProgress progress;
    {
        platform::LockGuard lock(gate_);
        cycleGc_.reclaimNanoseconds += finished - reclaimStarted;
        slice.reclaim = finished - reclaimStarted;
        cycleGc_.durationNanoseconds += finished - started;
        if (gcPhase_ == GcPhase::Reclaiming && !HasReclaimableLocked()) {
            gcPhase_ = GcPhase::Idle;
            lastGc_ = cycleGc_;
            gcWork_.Clear();
            gcShade_.Clear();
            ReleaseGrayBlocks();
            gcCursor_ = 0;
        }

        processing_ = false;
        progress = {gcPhase_, processed, cycleGc_, slice};
    }

    if (progress.IsComplete()) {
        LogCycle("Incremental GC cycle", progress.report);
    }

    return Result<GcProgress>::Success(progress);
}

Result<GcReport> ObjectRegistry::Collect(const ObjectAccessContext& access,
                                         WorkerScheduler* scheduler) noexcept {
    auto valid = CheckAccess(access);
    if (!valid) {
        return Result<GcReport>::Failure(valid.Error());
    }

    if (scheduler && scheduler->IsCurrentWorker()) {
        return Result<GcReport>::Failure(ErrorCode::WrongThread);
    }

    const uint64 started = platform::MonotonicNanoseconds();
    GcReport report;
    {
        platform::LockGuard lock(gate_);
        if (processing_ || constructions_ || totalBorrows_ || gcPhase_ != GcPhase::Idle) {
            return Result<GcReport>::Failure(ErrorCode::Busy);
        }

        processing_ = true;
        report.examined = tally_.liveObjects + tally_.destroyRequested;
    }

    // 모든 후보를 담을 용량을 먼저 확보한다. OOM이 발생해도 아직 어떤 객체도 retire하지 않았다.
    // 후보당 한 번만 큐에 넣으므로 이후 순회는 추가 할당 없이 깊은 그래프도 처리한다.
    Array<uint32> work(allocator_);
    auto reserved = work.TryReserve(static_cast<usize>(GcQueueBound(report.examined)));
    if (reserved) {
        reserved = VerifyClusterLedger();
    }

    if (!reserved) {
        platform::LockGuard lock(gate_);
        processing_ = false;
        return Result<GcReport>::Failure(reserved.Error());
    }

    MarkState state{this, &work};
    {
        platform::LockGuard lock(gate_);
        for (uint32 chunk = 0; chunk < chunkCount_; ++chunk) {
            GcBits& bits = BitsOf(chunk * SlotsPerChunk);
            StoreMark(bits.mark, 0);
            StoreMark(bits.clusterSeen, 0);
        }

        AdvanceGcEpochLocked();
        for (uint32 position = 0; position < rootCount_; ++position) {
            if (MarkIndex(rootList_[position], work, state.marked) != ErrorCode::None) {
                platform::Fatal("Root list holds a dead slot or the GC work queue is exhausted");
            }
        }
    }

    const uint64 markStarted = platform::MonotonicNanoseconds();
    report.beginNanoseconds = markStarted - started;
    usize cursor = 0;
    if (scheduler == nullptr) {
        state.counters = options_.countGcWork ? &report.work : nullptr;
        (void)MarkSerial(work, cursor, state, NumericLimits<uint32>::Max(), 0);
    } else {
        bool completed = false;
        (void)MarkParallel(work, cursor, state, NumericLimits<uint32>::Max(), 0, *scheduler,
                           completed);
        if (state.error == ErrorCode::None && !completed) {
            platform::Fatal("Unbounded parallel marking stopped before completion");
        }
    }

    const uint64 sweepStarted = platform::MonotonicNanoseconds();
    report.markNanoseconds = sweepStarted - markStarted;
    if (state.error != ErrorCode::None) {
        // 표식 비트는 다음 시도가 다시 지우므로 부분 표식이 남지 않는다.
        platform::LockGuard lock(gate_);
        processing_ = false;
        return Result<GcReport>::Failure(state.error);
    }

    // 이 지점 이전에는 도달성 판단을 확정하지 않는다. Trace 오류 시 sweep 전체를 생략한다.
    report.marked = state.marked;
    {
        platform::LockGuard lock(gate_);
        // 표식되지 않은 클러스터는 sweep 전에 해체한다(소속이 white 후보가 된다).
        DissolveUnmarkedClustersLocked();
        for (uint32 chunk = chunkCount_; chunk-- != 0;) {
            report.retired += RetireWhiteLocked(chunk);
        }

        // 경계 비율 안전장치: 분류가 끝난 뒤라 남은 소속은 모두 살아 있다(해체가 mark를 켜지 않아도
        // 된다).
        report.flattenedClusters = FlattenBoundaryClustersLocked();
    }

    const uint64 reclaimStarted = platform::MonotonicNanoseconds();
    report.sweepNanoseconds = reclaimStarted - sweepStarted;
    report.reclaimed = ReclaimEligible();
    const uint64 finished = platform::MonotonicNanoseconds();
    report.reclaimNanoseconds = finished - reclaimStarted;
    report.durationNanoseconds = finished - started;
    {
        platform::LockGuard lock(gate_);
        lastGc_ = report;
        processing_ = false;
    }

    LogCycle("Stop-the-world GC", report);
    return Result<GcReport>::Success(report);
}

ObjectRegistry::ReclaimWork ObjectRegistry::TakeForReclaimLocked(Slot& slot) noexcept {
    if (slot.indexed || slot.linked) {
        platform::Fatal("Reclaiming an object still in the name index or child ring");
    }

    if (slot.queue != RetiredQueue::None) {
        platform::Fatal("Reclaiming an object still in a retired queue");
    }

    // 저장소를 목록에서 분리한 뒤 잠금을 푼다. 콜백 중 통계 조회는 가능하지만 재회수는 불가하다.
    // 지역 TypeLease가 소멸자 코드까지 살아 있게 하므로 해제 콜백이 끝난 뒤 반환한다. 이름 기록도
    // 잠금 밖에서 반환한다(조회는 잠금 안에서 복사해 두었다).
    slot.SetState(tally_, SlotState::Reclaiming);
    ReclaimWork work;
    work.slot = &slot;
    work.type = Move(slot.type);
    work.object = slot.object;
    work.storage = slot.storage;
    work.record = slot.name;
    slot.name = nullptr;
    return work;
}

void ObjectRegistry::FinishReclaim(ReclaimWork& work) noexcept {
    // User destructors and allocator callbacks never execute under the registry gate.
    work.type.Ops().destroy(work.object);
    auto released = allocator_.Deallocate(work.storage);
    if (!released) {
        platform::Fatal("Object allocation ownership mismatch");
    }

    ReleaseRegistryMemory(allocator_, work.record);
    {
        platform::LockGuard lock(gate_);
        Slot& slot = *work.slot;
        slot.object = nullptr;
        slot.storage = nullptr;
        slot.transaction = 0;
        slot.stableType = {};
        slot.outer = {};
        // GC 밀집 자료를 비운다(live는 Retire 때 이미 꺼졌다).
        HotOf(slot.index) = GcHot{};
        GcBits& bits = BitsOf(slot.index);
        bits.leaf &= ~BitOf(slot.index);
        StoreMark(bits.mark, LoadMark(bits.mark) & ~BitOf(slot.index));
        StoreMark(bits.clusterSeen, LoadMark(bits.clusterSeen) & ~BitOf(slot.index));
        if (clustersEnabled_) {
            ClearClusterSlotLocked(slot.index);
        }

        slot.outermost = nullptr;
        slot.parent = nullptr;
        slot.lastChild = nullptr;
        slot.pathBytes = 0;
        slot.discarded = false;
        // 세대를 0/과거 값으로 순환시키면 오래된 WeakRef가 새 객체를 가리킬 수 있다.
        // 한도에 도달한 슬롯은 재사용하지 않고 Exhausted로 남긴다.
        if (slot.generation >= options_.maxGeneration) {
            slot.SetState(tally_, SlotState::Exhausted);
        } else {
            ++slot.generation;
            slot.SetState(tally_, SlotState::Free);
            slot.nextInList = freeSlots_;
            freeSlots_ = &slot;
        }
    }

    work.type.Reset();
}

uint32 ObjectRegistry::ReclaimEligible(uint32 maximum, uint64 deadline) noexcept {
    // 회수 사이에 잠금을 풀고 사용자 소멸자를 부른다. 그 사이 소멸자(또는 다른 스레드)가 지연
    // 슬롯의 보유 계수를 반환하면 그 슬롯이 풀린 목록으로 옮겨져 다음 차례에 회수된다(목록을
    // 처음부터 다시 걷지 않는다).
    uint32 reclaimed = 0;
    while (reclaimed < maximum && (deadline == 0 || platform::MonotonicNanoseconds() < deadline)) {
        ReclaimWork work;
        {
            platform::LockGuard lock(gate_);
            Slot* selected = NextReclaimableLocked();
            if (selected == nullptr) {
                break;
            }

            UnlinkRetiredLocked(*selected);
            work = TakeForReclaimLocked(*selected);
        }

        FinishReclaim(work);
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

GcReport ObjectRegistry::LastGcReport() const noexcept {
    platform::LockGuard lock(gate_);
    return lastGc_;
}

} // namespace rocks
