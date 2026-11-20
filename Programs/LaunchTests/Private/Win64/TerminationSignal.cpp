#include "LaunchProbe.h"

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

namespace rocks::launch::probe {

void ResetIgnoredTerminationSignal() noexcept {
    // Ctrl-Break는 상속되는 무시 표시가 없다(SetConsoleCtrlHandler(nullptr, TRUE)는 Ctrl-C만
    // 막는다).
}

void IgnoreTerminationSignal() noexcept {
    // Ctrl-Break에는 상속되는 무시가 없다(시험은 POSIX에서만 이 옵션을 쓴다).
}

SignalReadiness CheckTerminationSignal() noexcept {
    // Ctrl-Break는 같은 콘솔에 붙은 모든 프로세스에 간다. 이 probe 혼자일 때만 보낸다.
    // 반환값은 목록 크기와 관계없이 전체 개수다. 혼자인지만 보면 되므로 두 칸이면 된다.
    constexpr DWORD ProcessListCapacity = 2;
    DWORD processes[ProcessListCapacity] = {};
    return GetConsoleProcessList(processes, ProcessListCapacity) == 1
               ? SignalReadiness::Ready
               : SignalReadiness::SharedConsole;
}

bool RaiseTerminationSignal() noexcept {
    // 이 프로세스가 붙은 콘솔의 모든 프로세스(그룹 0)에 Ctrl-Break를 보낸다. 시험은 probe를 자기
    // 콘솔(창 없음)로 띄우므로 받는 쪽은 probe뿐이다(덤프 도우미는 콘솔이 없다). OS가 콘솔 처리기
    // 스레드에서 처리기를 부른다(실제 콘솔 이벤트 경로).
    return GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0) != FALSE;
}

} // namespace rocks::launch::probe
