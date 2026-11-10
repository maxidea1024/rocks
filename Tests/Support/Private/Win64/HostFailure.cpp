#include <Tests/Support/HostFailure.h>
#include <stdio.h>
#include <stdlib.h>

namespace rocks::test {

FILE* OpenReportFile(const char* path) noexcept {
    if (path == nullptr) {
        return nullptr;
    }
    FILE* file = nullptr;
    return fopen_s(&file, path, "wb") == 0 ? file : nullptr;
}

void PrepareFatalTestProcess() noexcept {
    // 자동 실패 시험에서 Windows 오류 대화상자를 띄우지 않는다.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}

void LockStandardErrorForTest() noexcept {
    _lock_file(stderr);
}

void UnlockStandardErrorForTest() noexcept {
    _unlock_file(stderr);
}

} // namespace rocks::test
