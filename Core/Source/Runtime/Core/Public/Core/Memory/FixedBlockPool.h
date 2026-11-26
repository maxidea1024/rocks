#pragma once

#include <Base/Compiler.h>
#include <Base/Types.h>
#include <Core/Containers/IndexedFreeList.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Platform/Atomics.h>
#include <Platform/Sync.h>

namespace rocks {

// 고정 크기 블록 할당기다(UE TLockFreeFixedSizeAllocator 대응, Docs/Design/Synchronization 12.2절).
// 블록은 색인으로 다룬다. 빈 블록은 IndexedFreeList로 돌리므로 할당·해제는 lock-free이고, 빈 블록이
// 없을 때만 Mutex를 쥐고 페이지 하나를 늘린다(페이지는 파괴 때만 돌려준다 — 회수 없음).
// 스레드 지역 캐시는 두지 않는다(측정 뒤).
//
// 성질:
// - 블록 수 상한(maxBlocks)과 페이지 크기(blocksPerPage)는 Initialize에서 정한다. 상한에 닿으면
//   TryAllocate가 CapacityExceeded다.
// - TryAllocate·Free·Data는 어느 스레드에서나 부른다. Free한 블록은 다시 할당될 때까지 쓰지 않는다.
// - 블록 내용은 0으로 채우지 않는다.
//
// 사용 예:
//   FixedBlockPool pool;
//   if (!pool.Initialize(allocator, sizeof(Record), alignof(Record), 64, 4096)) { ... }
//   auto block = pool.TryAllocate();
//   if (block) { auto* record = new (block.Value().data) Record(); ...;
//   pool.Free(block.Value().index); }
class ROCKS_CORE_API FixedBlockPool {
public:
    struct Block {
        void* data = nullptr;
        uint32 index = 0;
    };

    FixedBlockPool() noexcept = default;
    FixedBlockPool(const FixedBlockPool&) = delete;
    FixedBlockPool& operator=(const FixedBlockPool&) = delete;

    ~FixedBlockPool() noexcept {
        Shutdown();
    }

    // 블록 크기·정렬(2의 거듭제곱), 페이지마다 블록 수, 전체 블록 상한을 정하고 페이지 표를
    // 할당한다. 페이지는 아직 없다. 실패: 값이 0·정렬이 2의 거듭제곱 아님·상한이 IndexedFreeList
    // 용량 초과·이미 초기화됨은 InvalidArgument, 크기 넘침은 Overflow, 그 밖은 allocator
    // 오류(상태는 그대로).
    Result<void> Initialize(Allocator& allocator, usize blockBytes, usize blockAlignment,
                            uint32 blocksPerPage, uint32 maxBlocks) noexcept;

    // 페이지와 표를 돌려준다. 다른 스레드가 쓰지 않고 블록을 더 쓰지 않을 때만 부른다.
    void Shutdown() noexcept;

    // 빈 블록 하나를 준다. 없으면 페이지 하나를 늘린다. 실패: 상한에 닿음은 CapacityExceeded,
    // 페이지 할당 실패는 allocator 오류.
    Result<Block> TryAllocate() noexcept;

    // index 블록을 빈 목록에 돌려준다.
    void Free(uint32 index) noexcept;

    // index 블록의 주소다(할당된 블록에만 부른다).
    void* Data(uint32 index) const noexcept;

    uint32 BlockCapacity() const noexcept {
        return maxBlocks_;
    }

    // 지금까지 늘린 페이지 수다.
    uint32 PageCount() const noexcept {
        return pageCount_.Load<platform::MemoryOrder::Acquire>();
    }

private:
    IndexedFreeList<> free_;
    platform::Mutex growMutex_;
    platform::Atomic<uint32> pageCount_;
    unsigned char** pages_ = nullptr;
    Allocator* allocator_ = nullptr;
    usize stride_ = 0;
    usize alignment_ = 0;
    uint32 blocksPerPage_ = 0;
    uint32 maxBlocks_ = 0;
    uint32 maxPages_ = 0;
};

} // namespace rocks
