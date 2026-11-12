#include <TestSupport/HostFailure.h>
#include <stdio.h>

namespace rocks::test {

FILE* OpenReportFile(const char* path) noexcept {
    if (path == nullptr) {
        return nullptr;
    }
    return fopen(path, "wb");
}

void PrepareAutomatedTestProcess() noexcept {
    // macOS는 abort·assert가 대화상자를 띄우지 않는다. crash reporter는 시스템 설정을 따른다.
}

void PrepareFatalTestProcess() noexcept {
}

void LockStandardErrorForTest() noexcept {
    flockfile(stderr);
}

void UnlockStandardErrorForTest() noexcept {
    funlockfile(stderr);
}

} // namespace rocks::test
