#include <signal.h>
#include <unistd.h>

namespace rocks::test {
bool ConfigureLoggerFailureProbe() noexcept {
    // 이 child에 한정된 선택이다. 엔진은 프로세스 전역 signal 설정을 바꾸지 않는다.
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        return false;
    }
    int descriptors[2]{};
    if (pipe(descriptors) != 0) {
        return false;
    }
    const bool closed = close(descriptors[0]) == 0;
    const bool redirected = dup2(descriptors[1], STDERR_FILENO) == STDERR_FILENO;
    const bool released = close(descriptors[1]) == 0;
    return closed && redirected && released;
}
} // namespace rocks::test
