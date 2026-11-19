#pragma once

#include <Core/Memory/SystemAllocator.h>

// SystemAllocator의 시험 통로다(friend). 제품 코드는 쓰지 않는다 — Tests/ 소스만 include한다(정책
// 검사 test-access-include). 시험은 <Core/Private/Memory/SystemAllocatorTestAccess.h>로 쓴다.
namespace rocks::detail {

struct SystemAllocatorTestAccess {
    // 원장 저장소의 oneBasedAttempt번째 할당(표 늘리기)을 실패시킨다(0이면 끈다). 원장 넣기가
    // 실패한 Allocate가 사용자 블록을 게시하지 않고 OutOfMemory를 돌려주는 경로를 본다.
    static void FailLedgerStorageOnAttempt(SystemAllocator& allocator,
                                           uint64 oneBasedAttempt) noexcept {
        allocator.FailLedgerStorageOnAttempt(oneBasedAttempt);
    }

    // 원장에 적힌 주소 수다(통계의 liveAllocations와 같아야 한다).
    static usize LedgerCount(SystemAllocator& allocator) noexcept {
        return allocator.LedgerCount();
    }
};

} // namespace rocks::detail
