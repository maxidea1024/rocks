#pragma once

#include <Core/Memory/BinnedAllocator.h>

// BinnedAllocator의 실패 주입·내부 조회 진입점이다. 시험 통로(BinnedAllocatorTestAccess.h — Tests/
// 소스만 include한다)가 부르고, 구현은 내부 상태를 아는 BinnedAllocator.cpp에 있다. 제품 코드는
// 부르지 않는다.
namespace rocks {

struct BinnedAllocator::TestHooks {
    // ---- 생성 때의 실패 주입(이 스레드에서 다음에 만드는 인스턴스 하나에만 적용) ----

    // FailNextReserveAttempts의 "모든 시도" 값이다(예약 없이 열린다).
    static constexpr uint32 AllReserveAttempts = NumericLimits<uint32>::Max();
    // 주소 범위 예약 시도 가운데 처음 count번을 실패시킨다(절반씩 줄여 다시 시도하는 경로).
    static void FailNextReserveAttempts(uint32 count) noexcept;
    // slab 표의 플랫폼 할당을 실패시킨다(예약을 풀고 예약 없이 열린다).
    static void FailNextSlabTableAllocation() noexcept;

    // ---- 인스턴스의 실패 주입 ----

    // slab commit·decommit의 실패는 VirtualMemoryRegion의 시험 통로로 주입한다
    // (<PlatformBase/Private/Common/VirtualMemoryTestAccess.h>).

    // 큰 할당 원장 저장소의 oneBasedAttempt번째 할당(표 늘리기)을 실패시킨다(0이면 끈다).
    static void FailLedgerStorageOnAttempt(BinnedAllocator& allocator,
                                           uint64 oneBasedAttempt) noexcept;

    // ---- 등급 표 ----

    static uint32 ClassCount() noexcept;
    static usize ClassBlockBytes(uint32 sizeClass) noexcept;
    // slab 하나에 드는 블록 수다.
    static uint32 ClassCapacity(uint32 sizeClass) noexcept;
    // 요청(1..MaxSmallBytes, 정렬 MaxSmallAlignment 이하)이 가는 등급이다.
    static uint32 ClassOf(usize bytes, usize alignment) noexcept;
    // slab 안 offset이 sizeClass 블록의 시작이면 그 번호를 쓰고 참이다(역수 상수 나눗셈).
    static bool BlockIndexOf(uint32 sizeClass, usize offsetInSlab, uint32& blockIndex) noexcept;

    // ---- 상태 조회 ----

    // SlabTableEntry가 예약 범위 밖 주소에 돌려주는 값이다.
    static constexpr uint8 OutsideRange = NumericLimits<uint8>::Max();
    // address가 든 slab의 표 값이다(0은 쓰지 않는 slab, 등급 + 1은 쓰는 slab). address를 읽지
    // 않는다.
    static uint8 SlabTableEntry(BinnedAllocator& allocator, const void* address) noexcept;
};

} // namespace rocks
