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

    // ---- 스레드 캐시 ----

    // 이 스레드의 다음 캐시 만들기를 실패시킨다(그 호출은 등급 잠금 경로로 가고, 다음 호출이 다시
    // 만든다).
    static void FailNextThreadCacheCreation() noexcept;
    // 호출 스레드의 캐시와 이전 칸의 블록을 모두 slab으로 돌려준다(decommit하지 않는다 — Trim과
    // 달리 보관소는 그대로다).
    static void FlushCurrentThreadCache(BinnedAllocator& allocator) noexcept;
    // 호출 스레드가 이 인스턴스의 캐시를 갖고 있는지다.
    static bool HasCurrentThreadCache(BinnedAllocator& allocator) noexcept;
    // 호출 스레드의 종료 정리를 지금 돌린다(스레드가 끝날 때 OS가 부르는 것과 같은 함수 — 캐시를
    // 모두 돌려주고 그 스레드를 "종료한 스레드"로 표시한다). 그 뒤 이 스레드는 캐시를 다시 만들지
    // 않으므로 곧 끝날 worker 스레드에서만 부른다. 종료 정리 뒤에 도는 코드(다른 종료 정리)가
    // allocator를 쓰는 경로를 본다.
    static void RunCurrentThreadExitHook() noexcept;
    // 겹친 중복 반환의 결과를 만든다: 방금 반환한 작은 블록 address를 표지 검사 없이 호출 스레드의
    // 캐시에 한 번 더 넣는다(같은 블록이 자유 목록에 두 번 든다). 그 뒤의 할당이 그 블록을 두
    // 번째로 꺼낼 때 Fatal이어야 한다 — Fatal 대조 프로그램(자식 프로세스)에서만 부른다. 호출
    // 스레드의 캐시가 없거나 작은 블록이 아니면 거짓이다.
    static bool ReturnAgainIgnoringMarker(BinnedAllocator& allocator, void* address) noexcept;
    // 호출 스레드가 "종료한 스레드"로 표시됐는지다.
    static bool IsCurrentThreadExited() noexcept;
    // 호출 스레드의 thread_local 포인터만 지운다(기록과 종료 칸의 값은 그대로다). 스레드가 끝나는
    // 동안 thread_local 저장소가 먼저 해제되는 플랫폼(macOS)의 상태를 만든다: 그 뒤의 호출은 칸의
    // 값에서 기록을 다시 찾아야 한다. 기록이 종료 칸에 걸려 있는 스레드에서 부른다.
    static void ForgetCurrentThreadRecord() noexcept;
    // 호출 스레드의 thread_local 포인터 값이다(칸을 보지 않는다 — 비교에만 쓴다).
    static const void* CurrentThreadRecordPointer() noexcept;
    // 이 스레드의 기록을 스레드 종료 칸에 거는 일을 실패시킨다(아직 기록이 없는 스레드에서 부른다).
    // 그 스레드는 그 뒤로 캐시를 만들지 않으므로 곧 끝날 worker 스레드에서만 부른다.
    static void FailNextThreadExitHook() noexcept;
    // 호출 스레드가 "종료 칸에 걸지 못한 스레드"로 표시됐는지다.
    static bool IsCurrentThreadUnhooked() noexcept;
    // 호출 스레드의 기록에 든 항목(인스턴스 → 캐시) 수다. 파괴된 인스턴스의 항목은 다음 캐시를 만들
    // 때 버려진다.
    static uint32 CurrentThreadRecordEntries() noexcept;
    // 인스턴스에 등록된 스레드 캐시 수다.
    static uint32 ThreadCacheCount(BinnedAllocator& allocator) noexcept;
    // 인스턴스의 모든 스레드 캐시에 든 블록의 바이트 합이다(이전 칸은 세지 않는다). 다른 스레드가
    // 이 인스턴스를 쓰지 않을 때만 부른다(그 스레드의 묶음 개수를 잠금 없이 읽는다).
    static usize ThreadCacheBlockBytes(BinnedAllocator& allocator) noexcept;
    // sizeClass의 이전 칸 가운데 묶음이 든 칸 수와, 그 가운데 첫 묶음의 블록 수다(없으면 0).
    // TransferChainLength는 다른 스레드가 이 인스턴스를 쓰지 않을 때만 부른다.
    static uint32 TransferBundleCount(BinnedAllocator& allocator, uint32 sizeClass) noexcept;
    static uint32 TransferChainLength(BinnedAllocator& allocator, uint32 sizeClass) noexcept;
    // 등급의 묶음 한도(블록 수)다.
    static uint32 BundleBlocks(uint32 sizeClass) noexcept;

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
