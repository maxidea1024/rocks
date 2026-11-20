#include "LaunchProbe.h"

#include <signal.h>
#include <unistd.h>

// Rocks를 내장한 호스트 프로세스의 POSIX 판이다(설계 8.8절). 호스트가 자기 진입점(main)과 치명 신호
// 처리기를 갖고, Rocks는 내장 진입(RunEmbeddedApplication)으로 부른다. 내장 진입에서 Rocks는 신호
// 처리기를 걸지 않으므로 치명 신호는 호스트 처리기가 받는다(이 환경에서는 컴파일만 확인한다).
namespace {

constexpr int HostHandledExitCode = 77;

void HostSignalHandler(int) noexcept {
    constexpr char Line[] = "LAUNCH HOST SIGNAL\n";
    (void)write(STDERR_FILENO, Line, sizeof(Line) - 1);
    _exit(HostHandledExitCode);
}

const rocks::launch::ApplicationDescription& Describe() noexcept {
    static constexpr rocks::launch::ApplicationDescription Description =
        rocks::launch::probe::MakeDescription("LaunchEmbeddedHost",
                                              rocks::launch::CrashReportingMode::Full);
    return Description;
}

} // namespace

int main(int argc, char** argv) {
    struct sigaction action{};
    action.sa_handler = &HostSignalHandler;
    (void)sigemptyset(&action.sa_mask);
    (void)sigaction(SIGSEGV, &action, nullptr);
    (void)sigaction(SIGBUS, &action, nullptr);
    (void)sigaction(SIGABRT, &action, nullptr);
    return rocks::launch::RunEmbeddedApplication(Describe(), argc, argv);
}
