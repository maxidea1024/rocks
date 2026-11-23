#include "../Common/ProcessEntryInternal.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

namespace rocks::platform {

namespace {

// 종료 요청으로 받는 신호다.
constexpr int RequestSignals[] = {SIGINT, SIGTERM};
constexpr usize RequestSignalCount = sizeof(RequestSignals) / sizeof(RequestSignals[0]);

struct sigaction previousRequestActions[RequestSignalCount];
// 신호마다 처리기를 걸었는가다. 설치 전 동작이 SIG_IGN(nohup·부모가 무시를 물려준 경우)이면 걸지
// 않고 무시를 그대로 둔다(요청을 받지 않던 프로세스를 새로 끝낼 수 있게 만들지 않는다).
bool signalInstalled[RequestSignalCount] = {};
constinit bool signalsInstalled = false;

// 신호 처리기다(async-signal-safe 호출만). 첫 요청은 처리기만 부르고 돌아간다. 두 번째 요청은 설치
// 전 동작을 되돌리고 같은 신호를 다시 낸다. 처리 중에는 같은 신호가 막혀 있으므로 처리기가 끝난 뒤
// 이전 동작(기본: 종료)으로 전달된다.
void TerminationSignalHandler(int signal) noexcept {
    if (detail::DispatchTerminationRequest() == detail::TerminationRequestOutcome::Handled) {
        return;
    }

    for (usize index = 0; index < RequestSignalCount; ++index) {
        if (RequestSignals[index] == signal) {
            (void)sigaction(signal, &previousRequestActions[index], nullptr);
        }
    }

    (void)raise(signal);
}

} // namespace

FileStatus ConvertEntryArgument(const wchar_t* argument, char* buffer, usize capacity,
                                usize& bytes) noexcept {
    bytes = 0;
    if (argument == nullptr || buffer == nullptr) {
        return {FileCode::InvalidArgument, 0};
    }

    // POSIX 진입점은 main(char**)이다(넓은 문자 인자가 없다).
    if (capacity != 0) {
        buffer[0] = '\0';
    }

    return {FileCode::Unsupported, 0};
}

FileStatus ConvertEntryArgument(const char* argument, char* buffer, usize capacity,
                                usize& bytes) noexcept {
    bytes = 0;
    if (argument == nullptr || buffer == nullptr) {
        return {FileCode::InvalidArgument, 0};
    }

    const usize length = strlen(argument);
    if (length >= capacity) {
        if (capacity != 0) {
            buffer[0] = '\0';
        }

        bytes = length;
        return {FileCode::Overflow, 0};
    }

    memcpy(buffer, argument, length);
    buffer[length] = '\0';
    bytes = length;
    return {};
}

bool GetWindowedEntryArguments(int&, wchar_t**&) noexcept {
    return false;
}

bool InstallTerminationRequestHandler(TerminationRequestHandler handler) noexcept {
    if (!detail::AcquireTerminationRequestHandler(handler)) {
        return false;
    }

    struct sigaction action{};
    action.sa_handler = &TerminationSignalHandler;
    action.sa_flags = SA_RESTART;
    (void)sigemptyset(&action.sa_mask);
    for (usize index = 0; index < RequestSignalCount; ++index) {
        signalInstalled[index] = false;
        bool failed =
            sigaction(RequestSignals[index], nullptr, &previousRequestActions[index]) != 0;
        if (!failed && previousRequestActions[index].sa_handler == SIG_IGN) {
            continue;
        }

        failed = failed ||
                 sigaction(RequestSignals[index], &action, &previousRequestActions[index]) != 0;
        if (failed) {
            // 이미 건 신호를 되돌린다.
            for (usize undo = 0; undo < index; ++undo) {
                if (signalInstalled[undo]) {
                    (void)sigaction(RequestSignals[undo], &previousRequestActions[undo], nullptr);
                    signalInstalled[undo] = false;
                }
            }

            detail::ReleaseTerminationRequestHandler();
            return false;
        }

        signalInstalled[index] = true;
    }

    signalsInstalled = true;
    return true;
}

void RemoveTerminationRequestHandler() noexcept {
    if (!signalsInstalled) {
        return;
    }

    for (usize index = 0; index < RequestSignalCount; ++index) {
        if (signalInstalled[index]) {
            (void)sigaction(RequestSignals[index], &previousRequestActions[index], nullptr);
            signalInstalled[index] = false;
        }
    }

    signalsInstalled = false;
    detail::ReleaseTerminationRequestHandler();
}

void ExitProcessWithoutTeardown(int exitCode) noexcept {
    _Exit(exitCode);
}

} // namespace rocks::platform
