#include "LaunchProbe.h"

#include <signal.h>

namespace rocks::launch::probe {

void ResetIgnoredTerminationSignal() noexcept {
    // 부모가 SIGINT를 무시로 물려줬으면 기본 동작으로 되돌린다(Launch가 처리기를 걸기 전에 부른다).
    // .NET Process는 시작 때 무시였던 신호를 자식에게도 무시로 물려주므로 C#이 대신할 수 없다.
    struct sigaction current{};
    if (sigaction(SIGINT, nullptr, &current) != 0 || (current.sa_flags & SA_SIGINFO) != 0 ||
        current.sa_handler != SIG_IGN) {
        return;
    }

    struct sigaction defaults{};
    defaults.sa_handler = SIG_DFL;
    sigemptyset(&defaults.sa_mask);
    (void)sigaction(SIGINT, &defaults, nullptr);
}

void IgnoreTerminationSignal() noexcept {
    // nohup·백그라운드 셸처럼 SIGINT를 무시로 물려받은 상태를 만든다(Launch 설치 전).
    struct sigaction ignored{};
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    (void)sigaction(SIGINT, &ignored, nullptr);
}

SignalReadiness CheckTerminationSignal() noexcept {
    // 부모가 SIGINT를 무시(SIG_IGN)로 물려주면(nohup, 백그라운드 셸) Launch는 처리기를 걸지
    // 않는다(설치 전 동작 보존). 그 환경에서는 이 시험을 할 수 없다.
    struct sigaction current{};
    if (sigaction(SIGINT, nullptr, &current) != 0) {
        return SignalReadiness::Ignored;
    }

    return (current.sa_flags & SA_SIGINFO) == 0 && current.sa_handler == SIG_IGN
               ? SignalReadiness::Ignored
               : SignalReadiness::Ready;
}

bool RaiseTerminationSignal() noexcept {
    // 자기 자신에게 SIGINT를 보낸다(터미널 Ctrl-C와 같은 신호).
    return raise(SIGINT) == 0;
}

} // namespace rocks::launch::probe
