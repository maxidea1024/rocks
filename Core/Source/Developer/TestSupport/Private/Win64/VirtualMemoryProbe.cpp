#include <Windows.h>

namespace rocks::test {

bool PrepareVirtualMemoryAccessProbe() noexcept {
    // 접근 위반 대화상자가 자동 검증을 가로막지 않게 한다.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    return true;
}

} // namespace rocks::test
