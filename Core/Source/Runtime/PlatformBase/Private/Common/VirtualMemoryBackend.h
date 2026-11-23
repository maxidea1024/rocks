#pragma once

#include <Platform/VirtualMemory.h>

namespace rocks::platform::detail {

// 검증과 수명 관리는 호출자의 책임이다. 운영체제 타입을 공개 헤더로 전달하지 않는다.
// access는 예약 때 정한 접근 방식이다(Commit·Decommit에는 그 예약의 값을 그대로 넘긴다).
VirtualMemoryResult ReserveVirtualMemory(usize bytes, VirtualMemoryAccess access,
                                         void*& address) noexcept;
VirtualMemoryResult CommitVirtualMemory(void* address, usize bytes,
                                        VirtualMemoryAccess access) noexcept;
VirtualMemoryResult DecommitVirtualMemory(void* address, usize bytes,
                                          VirtualMemoryAccess access) noexcept;
VirtualMemoryResult ReleaseVirtualMemory(void* address, usize bytes) noexcept;

// 시험 주입이다(VirtualMemoryTestAccess.h가 정한다). 이 스레드에서 부르는 Commit·Decommit 가운데
// 정한 차례의 호출을 OS를 부르지 않고 실패시킨다(0이면 끈다). 차례는 인자 검사를 지난 호출만 센다.
struct VirtualMemoryFaults {
    uint64 commitCalls = 0;
    uint64 failCommitCall = 0;
    uint64 decommitCalls = 0;
    uint64 failDecommitCall = 0;
};

extern thread_local VirtualMemoryFaults virtualMemoryFaults;

// 이 스레드의 주입 상태다(virtualMemoryFaults). thread_local은 공유 라이브러리 경계로 export할 수
// 없으므로 다른 모듈(Modular의 시험 호스트)의 VirtualMemoryTestAccess는 이 함수로
// 닿는다(Docs/Design/BuildTargets 3.8절 BS8d5).
ROCKS_PLATFORMBASE_API VirtualMemoryFaults& ThisThreadVirtualMemoryFaults() noexcept;

// 주입한 실패가 돌려주는 nativeError 값이다(OS 오류 코드와 겹치지 않는 표지).
inline constexpr uint32 InjectedVirtualMemoryError = 0x52464c54;

} // namespace rocks::platform::detail
