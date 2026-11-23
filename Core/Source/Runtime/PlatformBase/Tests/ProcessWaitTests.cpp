#include <Platform/Process.h>
#include <TestSupport/ProcessProbe.h>
#include <TestSupport/Test.h>
#include <stdio.h>

// PID로 프로세스 종료를 기다리는 WaitForProcessExit(Docs/Design/ShaderSystem 11.1절 SC7)
// 시험이다: 살아 있는 자식은 TimedOut(0 ms), 강제 종료한 자식은 Signaled(회수 전 POSIX zombie
// 포함), PID 0은 Failed. 시간 상한은 실패 판정에만 쓴다(OS 대기가 끝을 알린다).
using namespace rocks;
namespace process_wait_tests_file {

using namespace rocks::platform;

namespace {

constexpr uint32 ExitWaitMilliseconds = 60'000;

} // namespace

TEST(Process_WaitForProcessExitObservesTheEnd) {
    CHECK(WaitForProcessExit(0, 0) == WaitResult::Failed);
    if (!test::ProcessSpawnExpected()) {
        printf("[SKIP] Process_WaitForProcessExitObservesTheEnd: no child processes on this "
               "platform\n");
        return;
    }

    char executable[4096];
    usize executableBytes = 0;
    REQUIRE(GetExecutablePath(executable, sizeof(executable), executableBytes));
    const ProcessArgument arguments[] = {{"--process-probe", 15}, {"sleep", 5}, {"600000", 6}};
    ProcessStartInfo info;
    info.executable = executable;
    info.executableBytes = executableBytes;
    info.arguments = arguments;
    info.argumentCount = 3;
    info.input = ProcessStreamMode::Null;
    info.output = ProcessStreamMode::Null;
    info.error = ProcessStreamMode::Null;
    Process child;
    REQUIRE(child.Start(info));
    const uint32 id = child.Id();
    CHECK(WaitForProcessExit(id, 0) == WaitResult::TimedOut);
    REQUIRE(child.Terminate());
    CHECK(WaitForProcessExit(id, ExitWaitMilliseconds) == WaitResult::Signaled);
    CHECK(child.Wait(ExitWaitMilliseconds) == WaitResult::Signaled);
    child.Close();
}

} // namespace process_wait_tests_file
