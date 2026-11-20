#include "../Common/ProcessEntryInternal.h"

#include <Platform/Win64/WideString.h>

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

#include <stdlib.h>
#include <wchar.h>

namespace rocks::platform {

namespace {

constinit bool consoleRoutineInstalled = false;

// 콘솔 제어 스레드(OS가 만든다)에서 불린다. Ctrl-C·Ctrl-Break만 받는다. 첫 요청은 처리했다고 알려
// 프로세스를 살리고, 그 밖(두 번째 요청, 창 닫기·로그오프·시스템 종료)은 다음 처리기(기본: 종료)로
// 넘긴다.
BOOL WINAPI ConsoleControlRoutine(DWORD type) noexcept {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) {
        return FALSE;
    }

    return detail::DispatchTerminationRequest() == detail::TerminationRequestOutcome::Handled
               ? TRUE
               : FALSE;
}

} // namespace

FileStatus ConvertEntryArgument(const wchar_t* argument, char* buffer, usize capacity,
                                usize& bytes) noexcept {
    bytes = 0;
    if (argument == nullptr || buffer == nullptr) {
        return {FileCode::InvalidArgument, 0};
    }

    const windows::TextConversion converted = windows::ConvertWideToUtf8(
        argument, static_cast<usize>(wcslen(argument)), buffer, capacity, windows::TextFit::Exact);
    if (!converted) {
        bytes = converted.required;
        return converted.status;
    }

    bytes = converted.length;
    return {};
}

FileStatus ConvertEntryArgument(const char* argument, char* buffer, usize capacity,
                                usize& bytes) noexcept {
    bytes = 0;
    if (argument == nullptr || buffer == nullptr) {
        return {FileCode::InvalidArgument, 0};
    }

    // Windows 진입점은 넓은 문자만 받는다(설계 D7).
    if (capacity != 0) {
        buffer[0] = '\0';
    }

    return {FileCode::Unsupported, 0};
}

bool GetWindowedEntryArguments(int& argc, wchar_t**& argv) noexcept {
    // UCRT는 wWinMain 실행 파일의 시작 때 명령줄을 나눠 __argc·__wargv에 둔다(좁은 문자 진입점은
    // __wargv를 채우지 않는다).
    if (__wargv == nullptr || __argc < 1) {
        return false;
    }

    argc = __argc;
    argv = __wargv;
    return true;
}

bool InstallTerminationRequestHandler(TerminationRequestHandler handler) noexcept {
    if (!detail::AcquireTerminationRequestHandler(handler)) {
        return false;
    }

    if (!SetConsoleCtrlHandler(&ConsoleControlRoutine, TRUE)) {
        detail::ReleaseTerminationRequestHandler();
        return false;
    }

    consoleRoutineInstalled = true;
    return true;
}

void RemoveTerminationRequestHandler() noexcept {
    if (!consoleRoutineInstalled) {
        return;
    }

    (void)SetConsoleCtrlHandler(&ConsoleControlRoutine, FALSE);
    consoleRoutineInstalled = false;
    detail::ReleaseTerminationRequestHandler();
}

} // namespace rocks::platform
