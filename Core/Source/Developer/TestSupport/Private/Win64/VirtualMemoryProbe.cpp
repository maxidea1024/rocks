#include <Platform/CrashReporting.h>

namespace rocks::test {

bool PrepareVirtualMemoryAccessProbe() noexcept {
    // 접근 위반 대화상자가 자동 검증을 가로막지 않게 한다.
    platform::SuppressSystemErrorDialogs();
    return true;
}

} // namespace rocks::test
