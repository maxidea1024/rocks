#include <Base/Compiler.h>
#include <Platform/Memory.h>
#include <Platform/Path.h>
#include <Platform/Process.h>
#include <Platform/Thread.h>
#include <TestSupport/ProcessProbe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace rocks;
using namespace rocks::platform;

// Process 시험의 자식 쪽이다. 테스트 실행 파일을 "--process-probe <모드> ..."로 다시 띄우면
// Programs/FoundationTests/Private/Main.cpp가 시험을 돌리지 않고 이 함수로 온다.
namespace rocks::test {

namespace {

bool Equal(const char* left, const char* right) noexcept {
    return strcmp(left, right) == 0;
}

bool WriteText(int stream, const char* text) noexcept {
    return WriteStandardStream(stream, text, strlen(text));
}

int EchoArguments(int argc, char** argv) noexcept {
    static char storage[64 * 1024];
    const char* arguments[64];
    const usize count = ReadUtf8Arguments(argc, argv, storage, sizeof(storage), arguments, 64);
    if (count < 3) {
        return 92;
    }

    for (usize i = 3; i < count; ++i) {
        char length[32];
        const usize bytes = strlen(arguments[i]);
        snprintf(length, sizeof(length), "%llu:", static_cast<unsigned long long>(bytes));
        if (!WriteText(1, length) || !WriteStandardStream(1, arguments[i], bytes) ||
            !WriteText(1, ";")) {
            return 93;
        }
    }

    return 0;
}

int Cat() noexcept {
    static uint8 chunk[16 * 1024];
    for (;;) {
        const int64 got = ReadStandardInput(chunk, sizeof(chunk));
        if (got == 0) {
            return 0;
        }

        if (got < 0 || !WriteStandardStream(1, chunk, static_cast<usize>(got))) {
            return 94;
        }
    }
}

int Flood(usize bytes) noexcept {
    static uint8 output[4096];
    static uint8 error[4096];
    MemorySet(output, 'o', sizeof(output));
    MemorySet(error, 'e', sizeof(error));
    while (bytes != 0) {
        const usize chunk = bytes < sizeof(output) ? bytes : sizeof(output);
        if (!WriteStandardStream(1, output, chunk) || !WriteStandardStream(2, error, chunk)) {
            return 95;
        }

        bytes -= chunk;
    }

    return 0;
}

// 부모가 끝낼 때까지 쉬지 않고 출력한다(읽는 쪽이 계속 바쁜 상태에서 시간 초과를 시험한다).
int Spew() noexcept {
    static uint8 output[16 * 1024];
    MemorySet(output, 's', sizeof(output));
    while (WriteStandardStream(1, output, sizeof(output))) {
    }

    return 0;
}

int WorkingDirectory() noexcept {
    char path[4096];
    usize bytes = 0;
    if (!GetStandardDirectory(StandardDirectory::Working, path, sizeof(path), bytes)) {
        return 97;
    }

    return WriteStandardStream(1, path, bytes) ? 0 : 97;
}

int SpawnDescendant(const char* then) noexcept {
    const uint32 descendant = SpawnSleepingDescendant();
    if (descendant == 0) {
        return 98;
    }

    char line[32];
    snprintf(line, sizeof(line), "%u\n", descendant);
    if (!WriteText(1, line)) {
        return 98;
    }

    if (Equal(then, "wait")) {
        SleepFor(60'000);
    }

    return 0;
}

// 이 실행 파일을 "streams" 모드로 표준 출력·오류를 물려주어 띄운다. 바깥 부모가 Inherit가 실제로
// 자기 pipe까지 이어지는지 확인한다.
int InheritChild() noexcept {
    char path[4096];
    usize bytes = 0;
    if (!GetExecutablePath(path, sizeof(path), bytes)) {
        return 80;
    }

    const ProcessArgument arguments[] = {{"--process-probe", 15}, {"streams", 7}};
    ProcessStartInfo info;
    info.executable = path;
    info.executableBytes = bytes;
    info.arguments = arguments;
    info.argumentCount = 2;
    info.output = ProcessStreamMode::Inherit;
    info.error = ProcessStreamMode::Inherit;
    Process child;
    if (!child.Start(info) || child.Wait(30'000) != WaitResult::Signaled) {
        return 81;
    }

    ProcessExit exit;
    return child.GetExit(exit) && exit.kind == ProcessExitKind::Exited ? static_cast<int>(exit.code)
                                                                       : 82;
}

int Priority() noexcept {
    char text[32];
    snprintf(text, sizeof(text), "%d", static_cast<int>(CurrentPriorityRank()));
    return WriteText(1, text) ? 0 : 83;
}

} // namespace

// probe 모드:
//   exit <code>                 종료 코드를 그대로 돌려준다.
//   echo-args <arg>...          받은 인자를 "<길이>:<byte>;"로 표준 출력에 쓴다(UTF-8 원문 대조).
//   cat                         표준 입력을 EOF까지 표준 출력으로 옮긴다.
//   flood <bytes>               표준 출력에 'o', 표준 오류에 'e'를 bytes씩 번갈아 쓴다.
//   spew                        쓰기가 실패할 때까지 표준 출력에 쓴다.
//   streams                     "out1"(출력) "err1"(오류) "out2"(출력)를 이 순서로 쓴다.
//   sleep <ms>                  기다린 뒤 0으로 끝난다.
//   cwd                         현재 디렉터리를 표준 출력에 쓴다.
//   spawn-descendant wait|exit  같은 job·그룹의 손자를 띄우고 PID를 "<pid>\n"으로 쓴다.
//                               wait면 기다리고 exit면 바로 끝난다(손자는 출력을 잡고 있다).
//   inherit-child               "streams" 손자에게 표준 스트림을 물려주고 그 종료 코드로 끝난다.
//   priority                    자기 우선순위 순위(CurrentPriorityRank)를 쓴다.
//   handle-check <h> <id>       handle·fd 값 h가 파일 id를 가리키면 0(물려받음), 아니면 1이다.
//   signal-self                 자신에게 SIGTERM을 보낸다(신호가 없는 플랫폼은 77).
//   crash                       중단점으로 끝난다(처리되지 않은 예외·SIGTRAP).
//   parent-alive <pid>          IsProcessRunning(pid)이면 0, 아니면 1이다.
int RunProcessProbe(int argc, char** argv) noexcept {
    if (argc < 3) {
        return 90;
    }

    const char* mode = argv[2];
    if (Equal(mode, "exit") && argc == 4) {
        return atoi(argv[3]);
    }

    if (Equal(mode, "echo-args")) {
        return EchoArguments(argc, argv);
    }

    if (Equal(mode, "cat")) {
        return Cat();
    }

    if (Equal(mode, "flood") && argc == 4) {
        return Flood(static_cast<usize>(strtoull(argv[3], nullptr, 10)));
    }

    if (Equal(mode, "spew")) {
        return Spew();
    }

    if (Equal(mode, "streams")) {
        return WriteText(1, "out1") && WriteText(2, "err1") && WriteText(1, "out2") ? 0 : 96;
    }

    if (Equal(mode, "sleep") && argc == 4) {
        SleepFor(static_cast<uint32>(strtoul(argv[3], nullptr, 10)));
        return 0;
    }

    if (Equal(mode, "cwd")) {
        return WorkingDirectory();
    }

    if (Equal(mode, "spawn-descendant") && argc == 4) {
        return SpawnDescendant(argv[3]);
    }

    if (Equal(mode, "inherit-child")) {
        return InheritChild();
    }

    if (Equal(mode, "priority")) {
        return Priority();
    }

    if (Equal(mode, "handle-check") && argc == 5) {
        const auto handle = static_cast<uptr>(strtoull(argv[3], nullptr, 10));
        const auto identity = static_cast<uint64>(strtoull(argv[4], nullptr, 10));
        return HandleRefersToFile(handle, identity) ? 0 : 1;
    }

    if (Equal(mode, "signal-self")) {
        if (!RaiseTerminateSignal()) {
            return 77;
        }

        SleepFor(10'000);
        return 78;
    }

    if (Equal(mode, "crash")) {
        // 디버거가 없으면 처리되지 않은 중단점 예외(Windows)·SIGTRAP(POSIX)으로 끝난다.
        ROCKS_DEBUG_BREAK();
    }

    if (Equal(mode, "parent-alive") && argc == 4) {
        return IsProcessRunning(static_cast<uint32>(strtoul(argv[3], nullptr, 10))) ? 0 : 1;
    }

    return 99;
}

} // namespace rocks::test
