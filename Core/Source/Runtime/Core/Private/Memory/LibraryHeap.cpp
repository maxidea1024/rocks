#include <Core/Memory/LibraryHeap.h>

#include <Base/NumericLimits.h>
#include <Base/PlacementNew.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Atomics.h>
#include <stddef.h>
#include <string.h>

namespace rocks {

namespace {

// 블록 정렬(malloc과 같은 보장)이다.
constexpr usize LibraryBlockAlignment = alignof(max_align_t);

// 블록 앞에 크기를 적는 머리다. 정렬을 지키려고 정렬 크기와 같다.
constexpr usize LibraryBlockHeader = alignof(max_align_t);

static_assert(LibraryBlockHeader >= sizeof(usize), "Library block header must hold the size");

// 프로세스 수명 할당기다(소멸하지 않는 정적 저장소).
SystemAllocator& HeapAllocator() noexcept {
    alignas(SystemAllocator) static unsigned char storage[sizeof(SystemAllocator)];
    static SystemAllocator* allocator = ::new (static_cast<void*>(storage)) SystemAllocator();
    return *allocator;
}

// 풀리지 않은 블록 수다(원자 함수로만 접근한다).
usize* LiveBlocks() noexcept {
    static constinit usize count = 0;
    return &count;
}

usize StoredSize(void* memory) noexcept {
    usize size = 0;
    memcpy(&size, static_cast<uint8*>(memory) - LibraryBlockHeader, sizeof(size));
    return size;
}

} // namespace

void* LibraryHeapAllocate(usize size) noexcept {
    if (size > NumericLimits<usize>::Max() - LibraryBlockHeader) {
        return nullptr;
    }

    Result<void*> block =
        HeapAllocator().Allocate(LibraryBlockHeader + size, LibraryBlockAlignment);
    if (!block) {
        return nullptr;
    }

    auto* bytes = static_cast<uint8*>(block.Value());
    memcpy(bytes, &size, sizeof(size));
    platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(LiveBlocks(), 1);
    return bytes + LibraryBlockHeader;
}

void* LibraryHeapAllocateZeroed(usize count, usize size) noexcept {
    if (size != 0 && count > NumericLimits<usize>::Max() / size) {
        return nullptr;
    }

    void* block = LibraryHeapAllocate(count * size);
    if (block != nullptr) {
        memset(block, 0, count * size);
    }

    return block;
}

void* LibraryHeapReallocate(void* memory, usize size) noexcept {
    void* resized = LibraryHeapAllocate(size);
    if (resized == nullptr || memory == nullptr) {
        return resized;
    }

    const usize previous = StoredSize(memory);
    memcpy(resized, memory, previous < size ? previous : size);
    LibraryHeapFree(memory);
    return resized;
}

void LibraryHeapFree(void* memory) noexcept {
    if (memory == nullptr) {
        return;
    }

    (void)HeapAllocator().Deallocate(static_cast<uint8*>(memory) - LibraryBlockHeader);
    platform::AtomicFetchSub<platform::MemoryOrder::Relaxed>(LiveBlocks(), 1);
}

usize LibraryHeapLiveBlocks() noexcept {
    return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(LiveBlocks());
}

} // namespace rocks
