#pragma once

#include <Core/Memory/Allocator.h>

namespace rocks {

// 저장된 위치는 생성한 StackAllocator에서만 사용한다. 기본 생성된 표시는 유효하지 않다.
// 표시는 객체를 소유하지 않으며, 할당자 소멸 후에는 어떤 할당자에도 사용할 수 없다.
class StackMark {
public:
    StackMark() noexcept = default;

private:
    friend class StackAllocator;
    StackMark(uint64 owner, uint64 epoch, uint64 top) noexcept
        : owner_(owner), epoch_(epoch), top_(top) {
    }

    uint64 owner_ = 0;
    uint64 epoch_ = 0;
    uint64 top_ = 0;
};

struct StackStats {
    AllocationStats allocations;
    usize reservedBytes = 0;
    usize chunks = 0;
};

// 단일 스레드 LIFO allocator다. backing은 이 객체와 모든 반환 storage보다 오래 살아야 한다.
// 생성한 스레드에서만 호출·파괴하며, 생성 스레드는 이 객체보다 오래 살아야 한다. 종료된
// 스레드 ID의 OS 재사용은 감지하지 못한다. backing 콜백의 재진입은 Result API에서 Busy,
// Mark/Stats/소멸에서는 Fatal이다.
//
// 사용 예: auto mark = stack.Mark(); 할당한 객체를 직접 소멸하고 역순 Deallocate한 뒤
// stack.Rewind(mark)를 검사한다. 각각의 Deallocate가 공간을 즉시 되돌리며 Rewind는 해당
// 경계 이후의 할당이 모두 반환되었는지 확인한다. 자동 객체 소멸이나 일괄 무효화는 없다.
// 표시는 복사 가능하며 동일 상태에서 Rewind를 반복해도 성공한다. 표시의 기준 할당이
// 반환되거나 Reset/Trim을 수행하면 표시는 무효가 된다. Array와 같은 컨테이너가 성장할 때
// 새 버퍼를 할당한 뒤 이전 버퍼를 반환하면 LIFO 규약을 어긴다. stack에서는 성장하지 않는
// 단일 버퍼와 다른 할당의 역순 반환이 보장되는 사용만 허용한다.
class StackAllocator final : public Allocator {
public:
    explicit StackAllocator(Allocator& backing, usize chunkBytes = 65536) noexcept;
    ~StackAllocator() noexcept override;

    StackAllocator(const StackAllocator&) = delete;
    StackAllocator& operator=(const StackAllocator&) = delete;
    StackAllocator(StackAllocator&&) = delete;
    StackAllocator& operator=(StackAllocator&&) = delete;

    // alignment는 0이 아닌 2의 거듭제곱이며, 0 byte는 성공한 nullptr다. tag는 새 chunk
    // 확보 시 backing에 전달된다. 실패 시 기존 할당·주소·통계·소유권은 변하지 않는다.
    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override;

    // 살아 있는 가장 최근 할당만 반환할 수 있다. 다른 순서면 Busy, 외부·중복 주소는
    // NotFound이며 nullptr는 성공한 무효 연산이다. 객체 소멸과 모든 사용 종료는 caller 책임이다.
    // raw 주소가 새 할당에 재사용되면 과거 주소와 현재 주소는 구분할 수 없다.
    Result<void> Deallocate(void* address) noexcept override;

    // Mark는 생성 스레드에서만 호출한다. 오류를 복구해야 하면 TryMark를 사용한다.
    StackMark Mark() const noexcept;
    Result<StackMark> TryMark() const noexcept;

    // 다른 할당자의 표시는 InvalidArgument, 기본 생성된 표시는 InvalidArgument다.
    // 무효가 된 표시는 StaleHandle, 이후 할당이 살아 있으면 Busy다. 실패 시 상태는 유지된다.
    Result<void> Rewind(StackMark mark) noexcept;

    // 살아 있는 할당이 없을 때만 성공한다. Reset은 chunk를 보유하고 Trim은 반환한다.
    // 둘 다 모든 기존 표시를 무효화한다. backing의 소유권 오류는 Fatal이다.
    Result<void> Reset() noexcept;
    Result<void> Trim() noexcept;

    // 생성 스레드에서만 조회한다. 예약 바이트는 backing에서 확보한 chunk의 총 크기다.
    StackStats Stats() const noexcept;

private:
    struct Chunk;
    struct Record;
    struct CallGuard;

    bool IsOwnerThread() const noexcept;
    bool TryLayout(const Chunk& chunk, usize bytes, usize alignment, usize& recordOffset,
                   usize& dataOffset, usize& endOffset) const noexcept;
    void ReleaseChunks() noexcept;

    Allocator* backing_;
    usize chunkBytes_;
    uint64 ownerThread_;
    uint64 ownerId_;
    uint64 epoch_ = 1;
    mutable bool inCall_ = false;
    Chunk* chunks_ = nullptr;
    Record* top_ = nullptr;
    StackStats stats_{};
};

} // namespace rocks
