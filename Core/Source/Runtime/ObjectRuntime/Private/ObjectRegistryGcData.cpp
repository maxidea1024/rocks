#include "ObjectRegistryGcInternal.h"

#include <Base/Compiler.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Fatal.h>

// ObjectRegistry GC 밀집 자료다(GC-S3 — GC 페이지·비트 묶음·타입 표·루트 목록과 그 성장).
namespace rocks {

using namespace registry_detail;

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

void ObjectRegistry::ClearMarksLocked() noexcept {
    for (uint32 chunk = 0; chunk < chunkCount_; ++chunk) {
        GcBits& bits = BitsOf(chunk * SlotsPerChunk);
        StoreMark(bits.mark, 0);
        StoreMark(bits.clusterSeen, 0);
    }
}

Result<uint16> ObjectRegistry::AcquireGcType(const RuntimeTypeOps& ops) noexcept {
    const void* key = &ops;
    const bool leaf = !ops.hasStrongReferences && options_.policy.packageNativeType == nullptr;
    uint32 bucket = gcTypeIndexCapacity_ == 0 ? 0 : GcTypeBucket(key, gcTypeIndexCapacity_);
    for (uint32 probe = 0; probe < gcTypeIndexCapacity_; ++probe) {
        const uint16 index = gcTypeIndex_[bucket];
        if (index == 0) {
            break;
        }

        GcType& type = gcTypes_[index];
        if (type.key != key) {
            bucket = (bucket + 1) & (gcTypeIndexCapacity_ - 1);
            continue;
        }

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

    if (gcTypeCount_ > NumericLimits<uint16>::Max()) {
        return Result<uint16>::Failure(ErrorCode::CapacityExceeded);
    }

    if (gcTypeCount_ == gcTypeCapacity_) {
        const uint32 capacity = gcTypeCapacity_ == 0 ? 16 : gcTypeCapacity_ * 2;
        auto memory = allocator_.Allocate(sizeof(GcType) * capacity, alignof(GcType), GcTypeTag);
        if (!memory) {
            return Result<uint16>::Failure(memory.Error());
        }

        // 색인은 표 용량의 두 배다(적재율 1/2 이하 — 탐색이 짧다).
        const uint32 indexCapacity = capacity * 2;
        auto index =
            allocator_.Allocate(sizeof(uint16) * indexCapacity, alignof(uint16), GcTypeTag);
        if (!index) {
            ReleaseRegistryMemory(allocator_, memory.Value());
            return Result<uint16>::Failure(index.Error());
        }

        auto* grown = static_cast<GcType*>(memory.Value());
        for (uint32 at = 0; at < capacity; ++at) {
            ::new (&grown[at]) GcType(at < gcTypeCount_ ? gcTypes_[at] : GcType{});
        }

        auto* table = static_cast<uint16*>(index.Value());
        for (uint32 at = 0; at < indexCapacity; ++at) {
            table[at] = 0;
        }

        for (uint32 at = 1; at < gcTypeCount_; ++at) {
            uint32 slot = GcTypeBucket(grown[at].key, indexCapacity);
            while (table[slot] != 0) {
                slot = (slot + 1) & (indexCapacity - 1);
            }

            table[slot] = static_cast<uint16>(at);
        }

        ReleaseRegistryMemory(allocator_, gcTypes_);
        ReleaseRegistryMemory(allocator_, gcTypeIndex_);
        gcTypes_ = grown;
        gcTypeCapacity_ = capacity;
        gcTypeIndex_ = table;
        gcTypeIndexCapacity_ = indexCapacity;
        // 0번은 비어 있는 항목이다(hot 레코드의 타입 없음).
        gcTypeCount_ = gcTypeCount_ == 0 ? 1 : gcTypeCount_;
    }

    GcType& type = gcTypes_[gcTypeCount_];
    type.key = key;
    type.trace = ops.trace;
    type.referenceLayout = ops.referenceLayout;
    type.parallelTraceSafe = ops.parallelTraceSafe;
    type.leaf = leaf;
    uint32 slot = GcTypeBucket(key, gcTypeIndexCapacity_);
    while (gcTypeIndex_[slot] != 0) {
        slot = (slot + 1) & (gcTypeIndexCapacity_ - 1);
    }

    gcTypeIndex_[slot] = static_cast<uint16>(gcTypeCount_);
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
            auto chunks = allocator_.Allocate(sizeof(ClusterChunk) * ChunksPerGcPage,
                                              alignof(ClusterChunk), GcPageTag);
            if (!chunks) {
                return Result<void>::Failure(chunks.Error());
            }

            auto* chunkTable = static_cast<ClusterChunk*>(chunks.Value());
            for (uint32 index = 0; index < ChunksPerGcPage; ++index) {
                ::new (&chunkTable[index]) ClusterChunk();
            }

            growth.page->clusterChunks = chunkTable;
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
        ReleaseRegistryMemory(allocator_, growth.page->clusterChunks);
        growth.page->~GcPage();
        ReleaseRegistryMemory(allocator_, growth.page);
    }

    ReleaseRegistryMemory(allocator_, growth.pageTable);
    ReleaseRegistryMemory(allocator_, growth.rootList);
    growth = {};
}

} // namespace rocks
