#pragma once

#include "VirtualMemoryBackend.h"

// VirtualMemoryRegion의 시험 통로다. 제품 코드는 쓰지 않는다 — Tests/ 소스만 include한다(정책 검사
// test-access-include). 시험은 <PlatformBase/Private/Common/VirtualMemoryTestAccess.h>로 쓴다.
namespace rocks::platform::detail {

struct VirtualMemoryTestAccess {
    // 이 스레드에서 지금부터 oneBasedCall번째 Commit(인자 검사를 지난 호출)을 OS를 부르지 않고
    // 실패시킨다(0이면 끈다). 그 호출은 {OsFailure, InjectedVirtualMemoryError}를 돌려주고 범위는
    // 그대로다. commit 한도 초과 같은 일시적인 실패 뒤의 경로를 본다.
    static void FailCommitOnCall(uint64 oneBasedCall) noexcept {
        ThisThreadVirtualMemoryFaults().commitCalls = 0;
        ThisThreadVirtualMemoryFaults().failCommitCall = oneBasedCall;
    }

    // 같은 방식으로 Decommit을 실패시킨다.
    static void FailDecommitOnCall(uint64 oneBasedCall) noexcept {
        ThisThreadVirtualMemoryFaults().decommitCalls = 0;
        ThisThreadVirtualMemoryFaults().failDecommitCall = oneBasedCall;
    }

    // 이 스레드의 주입을 모두 끈다(시험 끝에 부른다 — 주입은 스레드에 남는다).
    static void ClearFaults() noexcept {
        ThisThreadVirtualMemoryFaults() = VirtualMemoryFaults{};
    }
};

} // namespace rocks::platform::detail
