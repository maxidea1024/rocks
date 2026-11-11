#include <signal.h>
#include <sys/resource.h>

namespace rocks::test {

bool PrepareVirtualMemoryAccessProbe() noexcept {
    // ASan을 포함한 상속된 핸들러 대신 실제 신호 종료를 부모가 확인한다.
    if (signal(SIGSEGV, SIG_DFL) == SIG_ERR || signal(SIGBUS, SIG_DFL) == SIG_ERR) {
        return false;
    }
    const rlimit coreLimit{0, 0};
    return setrlimit(RLIMIT_CORE, &coreLimit) == 0;
}

} // namespace rocks::test
