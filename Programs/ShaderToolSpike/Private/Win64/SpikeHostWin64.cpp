#include "SpikeTools.h"

#include <Windows.h>
#include <crtdbg.h>
#include <stdlib.h>

namespace rocks::spike {

// TestSupport PrepareAutomatedTestProcess와 같은 설정이다(스파이크는 시험 runner가 아니라
// TestSupport를 링크하지 않는다). abort·assert·처리되지 않은 예외가 사람 입력을 기다리는 대화상자를
// 띄우지 않고 stderr와 종료 코드로 끝나게 한다.
void PrepareSpikeProcess() noexcept {
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}

} // namespace rocks::spike
