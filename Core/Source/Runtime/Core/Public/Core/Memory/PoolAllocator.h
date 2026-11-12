#pragma once

#include <Core/Memory/Allocator.h>
#include <Platform/Sync.h>

namespace rocks {

// 크기가 비슷한 객체의 저장소를 재사용하는 고정 블록 allocator다.
// 사용 예: PoolAllocator pool(backing, sizeof(Particle), alignof(Particle));
// auto memory = pool.Allocate(sizeof(Particle), alignof(Particle));
// 사용한 객체를 직접 소멸한 다음 정확히 반환받은 주소로 Deallocate한다.
// backing은 이 PoolAllocator와 모든 사용자보다 오래 살아야 한다.
//
// Allocate/Deallocate/Stats/Capacity/Trim/Shutdown은 서로 동기화된다. 블록 내용과
// allocator 파괴는 호출자가 동기화해야 한다. backing Allocate/Deallocate는 pool 호출
// 스레드에서 실행되므로 backing 자체도 그 스레드의 호출을 허용해야 한다. 소유 스레드 전용
// backing이라면 다른 스레드의 호출이 실패할 수 있다. backing 콜백이 같은 pool에 재진입하면 Result
// API는 Busy, 값 조회와 파괴는 Fatal이다. 서로 다른 pool 사이의 역순 잠금 순환과 상위
// 콜백이 다른 스레드의 같은 pool 작업을 기다리는 것은 교착 위험으로 금지한다.
// 빈 블록은 free list로 재사용하며 반환 시에는 chunk 목록을 검색한다. 많은 chunk에서의
// 반환 비용은 chunk 수에 비례하므로 고성능 보장은 별도 측정이 필요하다.
// 상위 allocator가 호출 스레드를 허용해야 한다. 예를 들어 스레드 전용 상위 allocator는
// 다른 스레드의 확장에서 WrongThread를 반환할 수 있고, Trim/Shutdown/파괴에서 상위 저장소
// 반환이 거부되면 Fatal이다. 이 allocator는 객체 생성과 소멸을 대신하지 않는다. 사용 중인
// 블록을 모두 반환한 후에만
// Trim/Shutdown이 가능하며, 남은 블록이 있을 때 파괴하면 Fatal이다.
class PoolAllocator final : public Allocator {
public:
    // blockBytes와 blocksPerChunk는 0보다 커야 하고 blockAlignment는 2의 거듭제곱이어야
    // 한다. 설정 자체가 잘못되면 Fatal, 표현할 수 없는 저장소 크기는 Allocate에서 Overflow다.
    explicit PoolAllocator(Allocator& backing, usize blockBytes, usize blockAlignment,
                           usize blocksPerChunk = 64) noexcept;
    ~PoolAllocator() noexcept override;

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;
    PoolAllocator(PoolAllocator&&) = delete;
    PoolAllocator& operator=(PoolAllocator&&) = delete;

    // 크기와 정렬이 설정을 넘으면 Unsupported다. 실패 시 기존 블록과 통계는 바뀌지 않는다.
    // 유효한 alignment의 0-byte 요청은 저장소를 확보하지 않고 nullptr를 반환한다.
    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override;

    // 정확히 이 pool에서 받은 살아 있는 블록의 시작 주소만 반환한다. nullptr는 no-op이다.
    // 미소유·중복·중간 주소는 역참조하지 않고 NotFound다. 반환 후 같은 주소가 다시 쓰이면
    // 오래된 포인터와 새 포인터는 구분할 수 없다. 사용자가 객체를 먼저 소멸해야 한다.
    Result<void> Deallocate(void* address) noexcept override;

    // 비어 있는 pool의 예약 저장소를 backing에 반환한다. 사용 중이면 Busy이고 변경하지 않는다.
    // 성공 뒤 다시 Allocate할 수 있다. backing이 소유 블록의 반환을 거부하면 Fatal이다.
    Result<void> Trim() noexcept;

    // 사용 중이면 Busy이고 열린 상태를 유지한다. 전부 반환한 뒤 재호출할 수 있으며,
    // 성공 이후에 유효하고 지원되는 비영 Allocate, Trim과 일반 Deallocate는 ShuttingDown이다.
    // Allocate는 종료 확인보다 정렬과 크기 검사를 먼저 하며 0-byte는 성공한다. 재호출은 성공한다.
    Result<void> Shutdown() noexcept;

    // 한 시점의 통계와 전체 블록 수다. 스냅샷만으로 다른 스레드의 작업 종료를 보장하지 않는다.
    AllocationStats Stats() const noexcept;
    usize Capacity() const noexcept;
    usize ChunkCount() const noexcept;

private:
    struct Slot;
    struct Chunk;
    struct EntryScope;

    static thread_local EntryScope* activeEntry_;

    Result<void> AddChunk() noexcept;
    void ReleaseChunks() noexcept;

    Allocator& backing_;
    usize blockBytes_;
    usize blockAlignment_;
    usize blocksPerChunk_;
    usize stride_ = 0;
    usize slotOffset_ = 0;
    usize dataOffset_ = 0;
    usize chunkBytes_ = 0;
    usize chunkAlignment_ = 0;
    bool layoutOverflow_ = false;

    mutable platform::Mutex mutex_;
    Chunk* chunks_ = nullptr;
    Slot* free_ = nullptr;
    usize capacity_ = 0;
    usize chunkCount_ = 0;
    AllocationStats stats_{};
    bool closed_ = false;
};

} // namespace rocks
