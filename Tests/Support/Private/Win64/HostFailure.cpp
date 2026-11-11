#include <Tests/Support/HostFailure.h>
#include <Windows.h>
#include <crtdbg.h>
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

void PrepareAutomatedTestProcess() noexcept {
    // abort의 "Debug Error" 대화상자와 Windows 오류 보고를 끈다.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // Debug CRT의 assert·error·warning 보고를 대화상자 대신 stderr로 보낸다(Release CRT에서는
    // 아무 일도 하지 않는 매크로다).
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    // 접근 위반 등 처리되지 않은 예외의 WER 대화상자도 띄우지 않는다.
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
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
