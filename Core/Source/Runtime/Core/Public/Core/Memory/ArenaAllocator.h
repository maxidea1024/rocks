#pragma once

#include <Core/Memory/Allocator.h>

namespace rocks {

// 예약 byte는 상위 allocator에 요청한 chunk 총량으로 사용자 payload와 다르다.
struct ArenaStats {
    AllocationStats allocations;
    usize reservedBytes = 0;
    usize chunks = 0;
};

// 상위 allocator를 빌리는 단일 스레드 선형 allocator다. 상위 allocator는 이 객체보다 오래
// 살아야 하며, 생성한 스레드가 모든 호출과 파괴를 수행해야 한다. 생성 스레드는 이 객체보다
// 오래 살아야 한다. OS가 종료된 스레드 ID를 재사용하면 동일 스레드로 오인할 수 있다.
// 원소 생성·소멸과 전달된
// 포인터의 모든 사용 종료는 caller 책임이다. 상위 allocator 콜백의 같은 allocator 재진입은
// Result API에서 Busy이며 Stats 또는 파괴 시 Fatal이다.
//
// 사용 예: ArenaAllocator arena(backing); Array<T> items(arena); items를 먼저 파괴한 후
// arena.Reset()으로 빈 chunk를 재사용하거나 arena.Trim()으로 상위 allocator에 반환한다.
// 해제된 allocation의 공간은 Reset 전까지 다시 사용하지 않는다. 한 프레임을 일괄 무효화하는
// 연산은 제공하지 않는다. 미반환 allocation이 있으면 Reset/Trim은 Busy, 소멸은 Fatal이다.
class ArenaAllocator final : public Allocator {
public:
    explicit ArenaAllocator(Allocator& backing, usize chunkBytes = 65536) noexcept;
    ~ArenaAllocator() noexcept override;

    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;
    ArenaAllocator(ArenaAllocator&&) = delete;
    ArenaAllocator& operator=(ArenaAllocator&&) = delete;

    // alignment는 0이 아닌 2의 거듭제곱이다. 0 byte는 성공한 nullptr다.
    // 실패해도 기존 할당·주소·통계·chunk 목록은 바뀌지 않는다. tag는 새 chunk를 확보할 때
    // 상위 allocator에 전달하며, 개별 할당 tag별 통계는 제공하지 않는다.
    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override;

    // 살아 있는 주소만 반환한다. 낯선 주소를 역참조하지 않으며 중복·외부 주소는 NotFound다.
    // 주소를 반환하기 전에 객체를 소멸하고 모든 사용을 끝내야 한다. raw 주소는 Reset 후
    // 같은 주소가 새 allocation에 재사용된 경우 과거 세대를 구별하지 못한다.
    Result<void> Deallocate(void* address) noexcept override;

    // 살아 있는 allocation이 없을 때 기존 chunk를 비워 재사용한다. 그 외에는 Busy다.
    // 해제된 포인터나 이전 view를 다시 사용해서는 안 된다. 상위 allocator에는 반환하지 않는다.
    Result<void> Reset() noexcept;

    // 살아 있는 allocation이 없으면 모든 chunk를 상위 allocator에 반환한다. 그 외에는 Busy다.
    // 같은 ArenaAllocator를 다시 사용할 수 있다. 상위 allocator의 소유권 오류는 Fatal이다.
    Result<void> Trim() noexcept;

    // 생성 스레드에서만 호출한다. 다른 스레드에서의 조회나 파괴는 Fatal이다.
    ArenaStats Stats() const noexcept;

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
    mutable bool inCall_ = false;
    Chunk* chunks_ = nullptr;
    Record* records_ = nullptr;
    ArenaStats stats_{};
};

} // namespace rocks
