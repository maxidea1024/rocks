#include "ObjectRegistryGcInternal.h"

#include <Base/Compiler.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

// ObjectRegistry GC 표식의 공통·직렬 경로다. 슬롯 표식(MarkIndex)·배치 간선 처리·참조 위치 표(M3)
// ·Registry가 방문하는 신원 간선·직렬 marker·삽입 장벽(ShadeLocked)·cutoff 판정을 둔다.
//
// GC 밀집 자료(GC-S3, Docs/Design/GarbageCollectionScale.ko.md): marker는 Slot(192 B)을 읽지 않고
// 슬롯 번호로 GC 페이지의 16 B hot 레코드(object·Outer 번호·타입 번호)와 chunk마다의 비트 묶음
// (live·mark·leaf)만 읽는다. 작업 큐는 슬롯 번호(uint32)다. 간선의 세대 검사는 Debug만 한다(강한
// 간선은 언제나 살아 있다는 불변식을 Debug가 확인한다). Release·Development는 domain·슬롯 범위·
// live 비트만 본다.
namespace rocks {

using namespace registry_detail;

void ObjectRegistry::VisitIdentityEdges(const Slot& slot, TraceVisitor& visitor) const noexcept {
    visitor.Visit(ColdOf(slot).outer);
    // 패키지 정책: 자식 고리가 export 표다. 처리 표시 아래라 고리가 바뀌지 않고 열린 생성
    // 트랜잭션이 있으면 GC가 진행하지 않으므로 Reserved 자식은 Commit 검증(같은 트랜잭션)에서만
    // 만난다.
    if (options_.policy.packageNativeType != nullptr) {
        for (const Slot* child = ColdOf(slot).firstChild; child != nullptr;
             child = ColdOf(*child).nextSibling) {
            visitor.Visit(HandleOf(*child));
        }
    }
}

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
                                          alignof(detail::ReferenceSlot), GcReferenceTableTag);
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
        for (const Slot* child = ColdOf(*SlotAt(index)).firstChild; child != nullptr;
             child = ColdOf(*child).nextSibling) {
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

} // namespace rocks
