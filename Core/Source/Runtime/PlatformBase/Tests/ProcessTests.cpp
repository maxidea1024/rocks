#include <Platform/Clock.h>
#include <Platform/File.h>
#include <Platform/Memory.h>
#include <Platform/Path.h>
#include <Platform/Process.h>
#include <Platform/Thread.h>
#include <TestSupport/ProcessProbe.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace rocks::test {
platform::PathStyle ExpectedHostPathStyle() noexcept;
} // namespace rocks::test

using namespace rocks;
using namespace rocks::platform;
using namespace rocks::test;

// 자식 프로세스 계약(Platform/Process.h)을 실제 OS 프로세스로 검사한다. 자식은 이 테스트 실행
// 파일을 "--process-probe <모드>"로 다시 띄운 probe다(ProcessProbe.cpp). 지원해야 하는
// 플랫폼(Windows·macOS)에서 Unsupported가 나오면 실패이고, Android는 Unsupported만 확인한다.
namespace {

constexpr uint32 GenerousTimeout = 30'000;

struct Probe {
    char path[4096]{};
    usize pathBytes = 0;
    ProcessArgument arguments[24]{};
    usize argumentCount = 0;

    Probe() noexcept {
        if (!GetExecutablePath(path, sizeof(path), pathBytes)) {
            pathBytes = 0;
        }

        Add("--process-probe");
    }

    Probe& Add(const char* text) noexcept {
        return Add(text, strlen(text));
    }

    Probe& Add(const char* text, usize bytes) noexcept {
        if (argumentCount < sizeof(arguments) / sizeof(arguments[0])) {
            arguments[argumentCount++] = {text, bytes};
        }

        return *this;
    }

    ProcessStartInfo Info() const noexcept {
        ProcessStartInfo info;
        info.executable = path;
        info.executableBytes = pathBytes;
        info.arguments = arguments;
        info.argumentCount = argumentCount;
        info.output = ProcessStreamMode::Null;
        info.error = ProcessStreamMode::Null;
        return info;
    }
};

uint64 ElapsedMilliseconds(uint64 start) noexcept {
    return (MonotonicNanoseconds() - start) / 1'000'000u;
}

ProcessRunResult RunProbe(const ProcessStartInfo& info, ProcessRunOptions options = {}) noexcept {
    if (options.timeoutMilliseconds == InfiniteTimeout) {
        options.timeoutMilliseconds = GenerousTimeout;
    }

    return RunProcess(info, options);
}

// 지원하지 않는 플랫폼이면 Unsupported인지 확인하고 시험을 건너뛴다. 지원해야 하는 플랫폼에서는
// 건너뛰지 않으므로 Unsupported가 나오면 뒤따르는 검사가 실패한다.
bool SkipUnsupported(const FileStatus& status) noexcept {
    if (ProcessSpawnExpected()) {
        return false;
    }

    CHECK(status.code == FileCode::Unsupported);
    return true;
}

// 지원하지 않는 플랫폼에서는 준비물(hard link·파일·디렉터리)을 만들기 전에 Unsupported만 확인하고
// 건너뛴다(Android 셸은 SELinux가 hard link를 막는다).
bool SkipBeforeFixtures() noexcept {
    if (ProcessSpawnExpected()) {
        return false;
    }

    const Probe probe;
    Process process;
    CHECK(process.Start(probe.Info()).code == FileCode::Unsupported);
    return true;
}

// 출력이 "<pid>\n"인 probe에서 PID를 읽는다.
uint32 ReadProcessIdLine(Process& process) noexcept {
    char line[64]{};
    usize used = 0;
    const uint64 start = MonotonicNanoseconds();
    while (used < sizeof(line) - 1u && memchr(line, '\n', used) == nullptr &&
           ElapsedMilliseconds(start) < GenerousTimeout) {
        const auto read =
            process.Read(ProcessStream::Output, line + used, sizeof(line) - 1u - used, 1000);
        if (!read.status && read.status.code != FileCode::NoProgress) {
            return 0;
        }

        used += read.bytes;
    }

    return static_cast<uint32>(strtoul(line, nullptr, 10));
}

// 작업 디렉터리 기준 상대 이름을 만든다(다른 PlatformBase 시험과 같다). Android의 Temporary는 앱
// 디렉터리 등록(RegisterApplicationDirectories)에 따라 바뀌므로 쓰지 않는다.
bool ScratchPath(const char* tag, char* path, usize capacity) noexcept {
    const int written = snprintf(path, capacity, "rocks-%s-%llu", tag,
                                 static_cast<unsigned long long>(MonotonicNanoseconds()));
    return written > 0 && static_cast<usize>(written) < capacity;
}

bool IsWindows() noexcept {
    return ExpectedHostPathStyle() == PathStyle::Windows;
}

} // namespace

TEST(Process_ExitCodesAreReported) {
    const char* codes[] = {"0", "7", "255"};
    const uint32 expected[] = {0, 7, 255};
    for (int i = 0; i < 3; ++i) {
        Probe probe;
        probe.Add("exit").Add(codes[i]);
        const auto run = RunProbe(probe.Info());
        if (SkipUnsupported(run.status)) {
            return;
        }

        REQUIRE(run.status);
        CHECK(!run.timedOut);
        CHECK(run.exit.kind == ProcessExitKind::Exited);
        CHECK(run.exit.code == expected[i]);
    }
}

TEST(Process_StartRejectsInvalidInput) {
    Probe probe;
    probe.Add("exit").Add("0");
    Process process;

    ProcessStartInfo info = probe.Info();
    info.executable = nullptr;
    CHECK(process.Start(info).code == FileCode::InvalidArgument);
    info = probe.Info();
    info.executableBytes = 0;
    CHECK(process.Start(info).code == FileCode::InvalidArgument);
    info = probe.Info();
    info.arguments = nullptr;
    CHECK(process.Start(info).code == FileCode::InvalidArgument);
    info = probe.Info();
    info.output = static_cast<ProcessStreamMode>(9);
    CHECK(process.Start(info).code == FileCode::InvalidArgument);
    info = probe.Info();
    info.priority = static_cast<ProcessPriority>(9);
    CHECK(process.Start(info).code == FileCode::InvalidArgument);

    // 인자·작업 디렉터리 안의 NUL과 잘못된 UTF-8은 자식에게 그대로 전달할 수 없다.
    const ProcessArgument withNul[] = {{"a\0b", 3}};
    info = probe.Info();
    info.arguments = withNul;
    info.argumentCount = 1;
    CHECK(process.Start(info).code == FileCode::InvalidArgument);
    const ProcessArgument badUtf8[] = {{"\xff", 1}};
    info.arguments = badUtf8;
    CHECK(process.Start(info).code == FileCode::InvalidArgument);
    info = probe.Info();
    info.workingDirectory = "\xc3\x28";
    info.workingDirectoryBytes = 2;
    CHECK(process.Start(info).code == FileCode::InvalidArgument);
    CHECK(!process.IsStarted());

    // 모으지 않는 스트림에 주는 입력·출력 버퍼는 호출 오류다.
    char buffer[16];
    ProcessRunOptions options;
    options.input = "x";
    options.inputBytes = 1;
    CHECK(RunProcess(probe.Info(), options).status.code == FileCode::InvalidArgument);
    options = {};
    options.output = buffer;
    options.outputCapacity = sizeof(buffer);
    CHECK(RunProcess(probe.Info(), options).status.code == FileCode::InvalidArgument);
    info = probe.Info();
    info.output = ProcessStreamMode::Pipe;
    info.error = ProcessStreamMode::Pipe;
    info.mergeErrorIntoOutput = true;
    options = {};
    options.error = buffer;
    options.errorCapacity = sizeof(buffer);
    CHECK(RunProcess(info, options).status.code == FileCode::InvalidArgument);

    info = probe.Info();
    char missing[128];
    snprintf(missing, sizeof(missing), "rocks-missing-%llu.exe",
             static_cast<unsigned long long>(MonotonicNanoseconds()));
    info.executable = missing;
    info.executableBytes = strlen(missing);
    const auto missingStatus = process.Start(info);
    if (SkipUnsupported(missingStatus)) {
        return;
    }

    CHECK(missingStatus.code == FileCode::NotFound);

    // Windows는 경로의 '"'를 거부하고 batch 파일은 인자 재해석 때문에 실행하지 않는다. POSIX에서는
    // 평범한 이름이라 없는 파일이다.
    info.executable = "missing\"name.exe";
    info.executableBytes = strlen(info.executable);
    CHECK(process.Start(info).code ==
          (IsWindows() ? FileCode::InvalidArgument : FileCode::NotFound));
    info.executable = "rocks-missing.BAT";
    info.executableBytes = strlen(info.executable);
    CHECK(process.Start(info).code == (IsWindows() ? FileCode::Unsupported : FileCode::NotFound));

    info = probe.Info();
    info.workingDirectory = missing;
    info.workingDirectoryBytes = strlen(missing);
    CHECK(process.Start(info).code == FileCode::NotFound);

    // 작업 디렉터리에 파일을 주면 WrongType이다.
    char file[512];
    REQUIRE(ScratchPath("process-file", file, sizeof(file)));
    {
        File created;
        REQUIRE(created.Open(file, FileOpenMode::CreateNew));
        CHECK(created.Close());
    }

    info.workingDirectory = file;
    info.workingDirectoryBytes = strlen(file);
    CHECK(process.Start(info).code == FileCode::WrongType);
    CHECK(PhysicalFileSystem().Delete(file, strlen(file)));

    // OS 명령줄 한도를 넘는 인자는 Overflow다(Windows 32767 단위, macOS ARG_MAX 1 MiB).
    constexpr usize HugeBytes = 2u * 1024u * 1024u;
    auto* huge = static_cast<char*>(AlignedAllocate(HugeBytes, 16));
    REQUIRE(huge != nullptr);
    MemorySet(huge, 'x', HugeBytes);
    const ProcessArgument hugeArgument[] = {{huge, HugeBytes}};
    info = probe.Info();
    info.arguments = hugeArgument;
    info.argumentCount = 1;
    CHECK(process.Start(info).code == FileCode::Overflow);
    AlignedFree(huge);

    // 이미 시작한 객체는 다시 Start할 수 없다.
    REQUIRE(process.Start(probe.Info()));
    CHECK(process.Start(probe.Info()).code == FileCode::AlreadyOpen);
    CHECK(process.Wait(GenerousTimeout) == WaitResult::Signaled);
    process.Close();
}

TEST(Process_ArgumentsReachChildByteForByte) {
    // Windows 명령줄 인용(따옴표·역슬래시·따옴표로 감싼 인자의 끝 역슬래시)과 UTF-8(BMP 밖 문자
    // 포함)을 왕복한다.
    const char* values[] = {"",
                            "plain",
                            "with space",
                            "tab\there",
                            "quote\"inside",
                            "trailing\\",
                            "trailing\\\\",
                            "dir x\\",
                            "dir x\\\\",
                            "back\\\\\"quote",
                            "\\\\server\\share\\",
                            "multi\nline",
                            "a\"\"b",
                            "\"",
                            "\\",
                            " leading",
                            "\xED\x95\x9C\xEA\xB8\x80 \xC3\xBC \xE2\x9C\x93 \xF0\x9F\x98\x80"};
    constexpr usize count = sizeof(values) / sizeof(values[0]);
    Probe probe;
    probe.Add("echo-args");
    for (const char* value : values) {
        probe.Add(value);
    }

    ProcessStartInfo info = probe.Info();
    info.output = ProcessStreamMode::Pipe;
    char output[4096];
    ProcessRunOptions options;
    options.output = output;
    options.outputCapacity = sizeof(output);
    const auto run = RunProbe(info, options);
    if (SkipUnsupported(run.status)) {
        return;
    }

    REQUIRE(run.status);
    REQUIRE(run.exit.kind == ProcessExitKind::Exited && run.exit.code == 0);
    REQUIRE(!run.outputTruncated);
    usize position = 0;
    for (usize i = 0; i < count; ++i) {
        char* end = nullptr;
        const unsigned long long length = strtoull(output + position, &end, 10);
        REQUIRE(end != nullptr && *end == ':');
        position = static_cast<usize>(end - output) + 1u;
        REQUIRE(length == strlen(values[i]));
        REQUIRE(position + length < run.outputBytes);
        CHECK(memcmp(output + position, values[i], length) == 0);
        position += length;
        REQUIRE(output[position] == ';');
        ++position;
    }

    CHECK(position == run.outputBytes);
}

TEST(Process_UnicodeExecutableAndWorkingDirectory) {
    if (SkipBeforeFixtures()) {
        return;
    }

    // 같은 디렉터리의 UTF-8 이름 hard link로 실행한다(Windows ASan 런타임 DLL을 같은 자리에서
    // 찾는다).
    char executable[4096];
    usize bytes = 0;
    REQUIRE(GetExecutablePath(executable, sizeof(executable), bytes));
    usize directory = bytes;
    while (directory > 0 && executable[directory - 1] != '/' && executable[directory - 1] != '\\') {
        --directory;
    }

    char link[4096];
    snprintf(link, sizeof(link), "%.*s\xED\x94\x84\xEB\xA1\x9C\xEC\x84\xB8\xEC\x8A\xA4-%llu.exe",
             static_cast<int>(directory), executable,
             static_cast<unsigned long long>(MonotonicNanoseconds()));
    REQUIRE(MakeHardLink(executable, link));
    char work[512];
    REQUIRE(ScratchPath("\xEC\x9E\x91\xEC\x97\x85", work, sizeof(work)));
    REQUIRE(PhysicalFileSystem().MakeDirectory(work, strlen(work)));

    Probe probe;
    probe.Add("cwd");
    ProcessStartInfo info = probe.Info();
    info.executable = link;
    info.executableBytes = strlen(link);
    info.workingDirectory = work;
    info.workingDirectoryBytes = strlen(work);
    info.output = ProcessStreamMode::Pipe;
    char output[4096];
    ProcessRunOptions options;
    options.output = output;
    options.outputCapacity = sizeof(output);
    const auto run = RunProbe(info, options);
    if (!SkipUnsupported(run.status)) {
        CHECK(run.status);
        CHECK(run.exit.kind == ProcessExitKind::Exited && run.exit.code == 0);
        // 자식이 본 현재 디렉터리는 지정한 이름으로 끝난다(OS가 link를 풀어 앞부분은 다를 수 있다).
        const char* name = work + strlen(work);
        while (name > work && name[-1] != '/' && name[-1] != '\\') {
            --name;
        }

        const usize nameBytes = strlen(name);
        CHECK(run.outputBytes >= nameBytes &&
              memcmp(output + run.outputBytes - nameBytes, name, nameBytes) == 0);
    }

    CHECK(PhysicalFileSystem().Delete(link, strlen(link)));
    CHECK(PhysicalFileSystem().DeleteDirectory(work, strlen(work)));
}

TEST(Process_RelativeExecutableUsesParentDirectory) {
    if (SkipBeforeFixtures()) {
        return;
    }

    // 상대 실행 파일 경로는 작업 디렉터리가 아니라 부모의 현재 디렉터리 기준이다.
    char current[4096];
    usize currentBytes = 0;
    REQUIRE(
        GetStandardDirectory(StandardDirectory::Working, current, sizeof(current), currentBytes));
    Probe probe;
    if (probe.pathBytes <= currentBytes + 1u || memcmp(probe.path, current, currentBytes) != 0 ||
        (probe.path[currentBytes] != '/' && probe.path[currentBytes] != '\\')) {
        return;
    }

    char work[512];
    REQUIRE(ScratchPath("process-relative", work, sizeof(work)));
    REQUIRE(PhysicalFileSystem().MakeDirectory(work, strlen(work)));
    probe.Add("exit").Add("5");
    ProcessStartInfo info = probe.Info();
    info.executable = probe.path + currentBytes + 1u;
    info.executableBytes = probe.pathBytes - currentBytes - 1u;
    info.workingDirectory = work;
    info.workingDirectoryBytes = strlen(work);
    const auto run = RunProbe(info);
    if (!SkipUnsupported(run.status)) {
        CHECK(run.status);
        CHECK(run.exit.kind == ProcessExitKind::Exited && run.exit.code == 5);
    }

    CHECK(PhysicalFileSystem().DeleteDirectory(work, strlen(work)));
}

TEST(Process_LargeInputAndOutputDoNotDeadlock) {
    // 입력·출력 모두 pipe 버퍼보다 훨씬 크다. 한 스레드가 쓰기와 읽기를 함께 진행하지 않으면
    // 멈춘다.
    constexpr usize Bytes = 1024u * 1024u;
    auto* input = static_cast<uint8*>(AlignedAllocate(Bytes, 16));
    auto* output = static_cast<uint8*>(AlignedAllocate(Bytes + 1u, 16));
    REQUIRE(input != nullptr && output != nullptr);
    for (usize i = 0; i < Bytes; ++i) {
        // \r\n·0x1A(Ctrl+Z) 같은 텍스트 모드 변환 대상 byte를 포함한다.
        input[i] = static_cast<uint8>((i * 131u + (i >> 9)) & 0xFFu);
    }

    Probe probe;
    probe.Add("cat");
    ProcessStartInfo info = probe.Info();
    info.input = ProcessStreamMode::Pipe;
    info.output = ProcessStreamMode::Pipe;
    ProcessRunOptions options;
    options.input = input;
    options.inputBytes = Bytes;
    options.output = output;
    options.outputCapacity = Bytes + 1u;
    const auto run = RunProbe(info, options);
    if (!SkipUnsupported(run.status)) {
        CHECK(run.status);
        CHECK(!run.timedOut);
        CHECK(run.exit.kind == ProcessExitKind::Exited && run.exit.code == 0);
        CHECK(!run.outputTruncated);
        CHECK(run.outputBytes == Bytes && memcmp(input, output, Bytes) == 0);
    }

    AlignedFree(input);
    AlignedFree(output);
}

TEST(Process_OutputBeyondCapacityIsDrainedAndTruncated) {
    // 출력·오류를 동시에 많이 쓰는 자식이 용량을 넘어도 끝까지 읽혀 정상 종료한다.
    Probe probe;
    probe.Add("flood").Add("524288");
    ProcessStartInfo info = probe.Info();
    info.output = ProcessStreamMode::Pipe;
    info.error = ProcessStreamMode::Pipe;
    char output[1000];
    char error[1500];
    ProcessRunOptions options;
    options.output = output;
    options.outputCapacity = sizeof(output);
    options.error = error;
    options.errorCapacity = sizeof(error);
    const auto run = RunProbe(info, options);
    if (SkipUnsupported(run.status)) {
        return;
    }

    REQUIRE(run.status);
    CHECK(!run.timedOut);
    CHECK(run.exit.kind == ProcessExitKind::Exited && run.exit.code == 0);
    CHECK(run.outputBytes == sizeof(output) && run.outputTruncated);
    CHECK(run.errorBytes == sizeof(error) && run.errorTruncated);
    bool outputOnly = true;
    for (char value : output) {
        outputOnly = outputOnly && value == 'o';
    }

    bool errorOnly = true;
    for (char value : error) {
        errorOnly = errorOnly && value == 'e';
    }

    CHECK(outputOnly);
    CHECK(errorOnly);

    // 용량 0은 모두 버린다.
    options = {};
    options.output = nullptr;
    options.outputCapacity = 0;
    info.error = ProcessStreamMode::Null;
    const auto discarded = RunProbe(info, options);
    CHECK(discarded.status && discarded.exit.code == 0);
    CHECK(discarded.outputBytes == 0 && discarded.outputTruncated);
}

TEST(Process_MergedErrorKeepsWriteOrder) {
    Probe probe;
    probe.Add("streams");
    ProcessStartInfo info = probe.Info();
    info.output = ProcessStreamMode::Pipe;
    info.error = ProcessStreamMode::Pipe;
    info.mergeErrorIntoOutput = true;
    char output[64];
    ProcessRunOptions options;
    options.output = output;
    options.outputCapacity = sizeof(output);
    const auto run = RunProbe(info, options);
    if (SkipUnsupported(run.status)) {
        return;
    }

    REQUIRE(run.status);
    CHECK(run.exit.code == 0);
    CHECK(run.outputBytes == 12 && memcmp(output, "out1err1out2", 12) == 0);

    // 합친 오류는 따로 읽을 수 없다.
    Process process;
    REQUIRE(process.Start(info));
    char buffer[8];
    CHECK(process.Read(ProcessStream::Error, buffer, sizeof(buffer), 0).status.code ==
          FileCode::NotOpen);
    CHECK(process.Wait(GenerousTimeout) == WaitResult::Signaled);
}

TEST(Process_InheritAndNullStreams) {
    // 손자가 물려받은 표준 출력·오류는 바깥 부모가 준 pipe까지 이어진다.
    Probe probe;
    probe.Add("inherit-child");
    ProcessStartInfo info = probe.Info();
    info.output = ProcessStreamMode::Pipe;
    info.error = ProcessStreamMode::Pipe;
    char output[64];
    char error[64];
    ProcessRunOptions options;
    options.output = output;
    options.outputCapacity = sizeof(output);
    options.error = error;
    options.errorCapacity = sizeof(error);
    const auto run = RunProbe(info, options);
    if (SkipUnsupported(run.status)) {
        return;
    }

    REQUIRE(run.status);
    CHECK(run.exit.kind == ProcessExitKind::Exited && run.exit.code == 0);
    CHECK(run.outputBytes == 8 && memcmp(output, "out1out2", 8) == 0);
    CHECK(run.errorBytes == 4 && memcmp(error, "err1", 4) == 0);

    // Null 입력은 곧바로 EOF다(cat이 기다리지 않고 끝난다).
    Probe cat;
    cat.Add("cat");
    info = cat.Info();
    info.input = ProcessStreamMode::Null;
    info.output = ProcessStreamMode::Pipe;
    options = {};
    options.output = output;
    options.outputCapacity = sizeof(output);
    options.timeoutMilliseconds = 10'000;
    const auto empty = RunProbe(info, options);
    CHECK(empty.status && !empty.timedOut && empty.exit.code == 0 && empty.outputBytes == 0);
}

TEST(Process_RunTimeoutTerminatesChild) {
    Probe probe;
    probe.Add("sleep").Add("60000");
    ProcessRunOptions options;
    options.timeoutMilliseconds = 300;
    uint64 start = MonotonicNanoseconds();
    const auto run = RunProbe(probe.Info(), options);
    if (SkipUnsupported(run.status)) {
        return;
    }

    REQUIRE(run.status);
    CHECK(run.timedOut);
    CHECK(run.exit.kind == ProcessExitKind::Terminated);
    CHECK(ElapsedMilliseconds(start) < 20'000);

    // 출력이 쉬지 않고 들어와도 시간 초과를 놓치지 않는다.
    Probe spew;
    spew.Add("spew");
    ProcessStartInfo info = spew.Info();
    info.output = ProcessStreamMode::Pipe;
    char output[1000];
    options.output = output;
    options.outputCapacity = sizeof(output);
    start = MonotonicNanoseconds();
    const auto busy = RunProbe(info, options);
    REQUIRE(busy.status);
    CHECK(busy.timedOut);
    CHECK(busy.outputTruncated);
    CHECK(ElapsedMilliseconds(start) < 20'000);
}

TEST(Process_WaitTerminateCloseAndRestart) {
    Probe probe;
    probe.Add("sleep").Add("60000");
    Process process;
    const auto started = process.Start(probe.Info());
    if (SkipUnsupported(started)) {
        return;
    }

    REQUIRE(started);
    CHECK(process.IsStarted());
    const uint32 id = process.Id();
    CHECK(id != 0);
    CHECK(IsProcessRunning(id));
    CHECK(process.Wait(0) == WaitResult::TimedOut);
    CHECK(process.Wait(50) == WaitResult::TimedOut);
    ProcessExit exit;
    CHECK(!process.GetExit(exit));
    CHECK(process.Terminate());
    CHECK(process.Terminate());
    REQUIRE(process.Wait(GenerousTimeout) == WaitResult::Signaled);
    CHECK(process.Wait(0) == WaitResult::Signaled);
    REQUIRE(process.GetExit(exit));
    CHECK(exit.kind == ProcessExitKind::Terminated);
    CHECK(process.Id() == id);
    // 끝났지만 아직 닫지 않은 자식은 POSIX에서 zombie라 있다고 보고 Windows는 없다고 본다.
    CHECK(IsProcessRunning(id) == !IsWindows());
    process.Close();
    process.Close();
    CHECK(!process.IsStarted());
    CHECK(process.Id() == 0);
    CHECK(process.Wait(0) == WaitResult::Failed);
    CHECK(process.Terminate().code == FileCode::NotOpen);

    // 닫은 객체는 다시 시작할 수 있다. 이동하면 소유권이 옮겨 간다.
    Probe quick;
    quick.Add("exit").Add("3");
    REQUIRE(process.Start(quick.Info()));
    Process moved(static_cast<Process&&>(process));
    CHECK(!process.IsStarted());
    REQUIRE(moved.Wait(GenerousTimeout) == WaitResult::Signaled);
    REQUIRE(moved.GetExit(exit));
    CHECK(exit.kind == ProcessExitKind::Exited && exit.code == 3);
    // 스스로 끝난 뒤의 Terminate는 종료 종류를 바꾸지 않는다.
    CHECK(moved.Terminate());
    REQUIRE(moved.GetExit(exit));
    CHECK(exit.kind == ProcessExitKind::Exited && exit.code == 3);
}

TEST(Process_DestroyingRunningProcessEndsIt) {
    Probe probe;
    probe.Add("sleep").Add("60000");
    uint32 id = 0;
    {
        Process process;
        const auto started = process.Start(probe.Info());
        if (SkipUnsupported(started)) {
            return;
        }

        REQUIRE(started);
        id = process.Id();
        ProcessWatch watch(id);
        process.Close();
        CHECK(watch.WaitGone(10'000));
    }

    CHECK(id != 0);
}

TEST(Process_TerminateEndsDescendants) {
    Probe probe;
    probe.Add("spawn-descendant").Add("wait");
    ProcessStartInfo info = probe.Info();
    info.output = ProcessStreamMode::Pipe;
    Process process;
    const auto started = process.Start(info);
    if (SkipUnsupported(started)) {
        return;
    }

    REQUIRE(started);
    const uint32 descendant = ReadProcessIdLine(process);
    REQUIRE(descendant != 0);
    ProcessWatch watch(descendant);
    CHECK(IsProcessRunning(descendant));
    CHECK(process.Terminate());
    CHECK(process.Wait(GenerousTimeout) == WaitResult::Signaled);
    CHECK(watch.WaitGone(10'000));
    process.Close();
}

TEST(Process_CloseAfterNaturalExitEndsDescendants) {
    // 직접 자식이 스스로 끝난 뒤에도 남은 자손은 Terminate·Close가 끝낸다.
    const bool viaTerminate[] = {true, false};
    for (const bool terminate : viaTerminate) {
        Probe probe;
        probe.Add("spawn-descendant").Add("exit");
        ProcessStartInfo info = probe.Info();
        info.output = ProcessStreamMode::Pipe;
        Process process;
        const auto started = process.Start(info);
        if (SkipUnsupported(started)) {
            return;
        }

        REQUIRE(started);
        const uint32 descendant = ReadProcessIdLine(process);
        REQUIRE(descendant != 0);
        ProcessWatch watch(descendant);
        REQUIRE(process.Wait(GenerousTimeout) == WaitResult::Signaled);
        ProcessExit exit;
        REQUIRE(process.GetExit(exit));
        CHECK(exit.kind == ProcessExitKind::Exited && exit.code == 0);
        CHECK(IsProcessRunning(descendant));
        if (terminate) {
            CHECK(process.Terminate());
            REQUIRE(process.GetExit(exit));
            CHECK(exit.kind == ProcessExitKind::Exited);
        } else {
            process.Close();
        }

        CHECK(watch.WaitGone(10'000));
        process.Close();
    }
}

TEST(Process_RunEndsWhenDescendantHoldsOutput) {
    // 손자가 표준 출력 pipe를 물려받아 잡고 있어도 직접 자식이 끝나면 RunProcess가 끝나야 한다.
    Probe probe;
    probe.Add("spawn-descendant").Add("exit");
    ProcessStartInfo info = probe.Info();
    info.output = ProcessStreamMode::Pipe;
    char output[64]{};
    ProcessRunOptions options;
    options.output = output;
    options.outputCapacity = sizeof(output) - 1u;
    const uint64 start = MonotonicNanoseconds();
    const auto run = RunProbe(info, options);
    if (SkipUnsupported(run.status)) {
        return;
    }

    REQUIRE(run.status);
    CHECK(!run.timedOut);
    CHECK(run.exit.kind == ProcessExitKind::Exited && run.exit.code == 0);
    CHECK(ElapsedMilliseconds(start) < 20'000);
    const auto descendant = static_cast<uint32>(strtoul(output, nullptr, 10));
    REQUIRE(descendant != 0);
    ProcessWatch watch(descendant);
    CHECK(watch.WaitGone(10'000));
}

TEST(Process_StreamingPipesAndEndOfStream) {
    Probe probe;
    probe.Add("cat");
    ProcessStartInfo info = probe.Info();
    info.input = ProcessStreamMode::Pipe;
    info.output = ProcessStreamMode::Pipe;
    Process process;
    const auto started = process.Start(info);
    if (SkipUnsupported(started)) {
        return;
    }

    REQUIRE(started);
    char buffer[64];
    // 아직 쓰지 않았으므로 읽을 것이 없다.
    CHECK(process.Read(ProcessStream::Output, buffer, sizeof(buffer), 50).status.code ==
          FileCode::NoProgress);
    const auto written = process.WriteInput("hello\n", 6, GenerousTimeout);
    CHECK(written.status && written.bytes == 6);
    usize used = 0;
    const uint64 start = MonotonicNanoseconds();
    while (used < 6) {
        REQUIRE(ElapsedMilliseconds(start) < GenerousTimeout);
        const auto read =
            process.Read(ProcessStream::Output, buffer + used, sizeof(buffer) - used, 1000);
        REQUIRE(read.status || read.status.code == FileCode::NoProgress);
        used += read.bytes;
    }

    CHECK(memcmp(buffer, "hello\n", 6) == 0);
    CHECK(process.CloseInput());
    CHECK(process.CloseInput().code == FileCode::NotOpen);
    CHECK(process.WriteInput("x", 1).status.code == FileCode::NotOpen);
    for (;;) {
        const auto read =
            process.Read(ProcessStream::Output, buffer, sizeof(buffer), GenerousTimeout);
        if (read.status.code == FileCode::EndOfFile) {
            break;
        }

        REQUIRE(read.status);
    }

    CHECK(process.Read(ProcessStream::Output, buffer, sizeof(buffer), 0).status.code ==
          FileCode::EndOfFile);
    CHECK(process.Read(ProcessStream::Error, buffer, sizeof(buffer), 0).status.code ==
          FileCode::NotOpen);
    CHECK(process.Read(ProcessStream::Output, buffer, 0, 0).status.code ==
          FileCode::InvalidArgument);
    REQUIRE(process.Wait(GenerousTimeout) == WaitResult::Signaled);
    ProcessExit exit;
    REQUIRE(process.GetExit(exit));
    CHECK(exit.kind == ProcessExitKind::Exited && exit.code == 0);
}

TEST(Process_WriteInputTimeoutAndClosedReader) {
    // 출력을 비우지 않는 cat에 큰 입력을 쓰면 pipe가 차서 시간 초과다. 그때까지 쓴 양은 정확해야
    // 한다.
    constexpr usize Bytes = 4u * 1024u * 1024u;
    auto* input = static_cast<uint8*>(AlignedAllocate(Bytes, 16));
    auto* output = static_cast<uint8*>(AlignedAllocate(Bytes, 16));
    REQUIRE(input != nullptr && output != nullptr);
    for (usize i = 0; i < Bytes; ++i) {
        input[i] = static_cast<uint8>((i * 7u + (i >> 11)) & 0xFFu);
    }

    Probe probe;
    probe.Add("cat");
    ProcessStartInfo info = probe.Info();
    info.input = ProcessStreamMode::Pipe;
    info.output = ProcessStreamMode::Pipe;
    Process process;
    const auto started = process.Start(info);
    if (!SkipUnsupported(started)) {
        REQUIRE(started);
        const auto written = process.WriteInput(input, Bytes, 300);
        CHECK(written.status.code == FileCode::NoProgress);
        CHECK(written.bytes > 0 && written.bytes < Bytes);
        CHECK(process.CloseInput());
        usize used = 0;
        for (;;) {
            const auto read =
                process.Read(ProcessStream::Output, output + used, Bytes - used, GenerousTimeout);
            if (read.status.code == FileCode::EndOfFile || !read.status) {
                CHECK(read.status.code == FileCode::EndOfFile);
                break;
            }

            used += read.bytes;
        }

        CHECK(used == written.bytes && memcmp(input, output, used) == 0);
        CHECK(process.Wait(GenerousTimeout) == WaitResult::Signaled);
        process.Close();

        // 입력을 읽지 않고 끝난 자식에게 쓰면 EndOfFile이다.
        Probe quit;
        quit.Add("exit").Add("0");
        info = quit.Info();
        info.input = ProcessStreamMode::Pipe;
        REQUIRE(process.Start(info));
        REQUIRE(process.Wait(GenerousTimeout) == WaitResult::Signaled);
        CHECK(process.WriteInput("x", 1, GenerousTimeout).status.code == FileCode::EndOfFile);
        process.Close();
    }

    AlignedFree(input);
    AlignedFree(output);
}

TEST(Process_PriorityIsApplied) {
    const int32 parent = CurrentPriorityRank();
    int32 ranks[2] = {};
    const ProcessPriority priorities[2] = {ProcessPriority::Normal, ProcessPriority::Low};
    for (int i = 0; i < 2; ++i) {
        Probe probe;
        probe.Add("priority");
        ProcessStartInfo info = probe.Info();
        info.output = ProcessStreamMode::Pipe;
        info.priority = priorities[i];
        char output[32]{};
        ProcessRunOptions options;
        options.output = output;
        options.outputCapacity = sizeof(output) - 1u;
        const auto run = RunProbe(info, options);
        if (SkipUnsupported(run.status)) {
            return;
        }

        REQUIRE(run.status && run.exit.code == 0);
        ranks[i] = static_cast<int32>(strtol(output, nullptr, 10));
    }

    // 부모가 이미 낮은 우선순위면 두 자식이 같을 수 있다(Windows는 BELOW_NORMAL 부모의 기본값을
    // 물려준다).
    if (parent < 5) {
        CHECK(ranks[0] == parent);
        CHECK(ranks[1] > ranks[0]);
    }
}

TEST(Process_ParentHandlesAreNotInherited) {
    if (SkipBeforeFixtures()) {
        return;
    }

    // 부모가 상속 가능하게 연 파일을 자식이 물려받지 않는다(Windows handle 목록, macOS
    // POSIX_SPAWN_CLOEXEC_DEFAULT).
    char file[512];
    REQUIRE(ScratchPath("process-handle", file, sizeof(file)));
    uptr handle = 0;
    uint64 identity = 0;
    REQUIRE(OpenInheritableFile(file, handle, identity));
    char handleText[32];
    char identityText[32];
    snprintf(handleText, sizeof(handleText), "%llu", static_cast<unsigned long long>(handle));
    snprintf(identityText, sizeof(identityText), "%llu", static_cast<unsigned long long>(identity));
    // probe 판정의 양성 대조: 부모 자신은 그 handle이 그 파일임을 알아본다.
    CHECK(HandleRefersToFile(handle, identity));
    Probe probe;
    probe.Add("handle-check").Add(handleText).Add(identityText);
    const auto run = RunProbe(probe.Info());
    if (!SkipUnsupported(run.status)) {
        CHECK(run.status);
        CHECK(run.exit.kind == ProcessExitKind::Exited && run.exit.code == 1);
    }

    CloseInheritableFile(handle);
}

TEST(Process_AbnormalEndsAreReported) {
    // 신호로 끝난 자식은 Signaled와 신호 번호다(신호가 없는 Windows probe는 77로 끝난다).
    Probe probe;
    probe.Add("signal-self");
    const auto signaled = RunProbe(probe.Info());
    if (SkipUnsupported(signaled.status)) {
        return;
    }

    REQUIRE(signaled.status);
    if (!(signaled.exit.kind == ProcessExitKind::Exited && signaled.exit.code == 77)) {
        CHECK(signaled.exit.kind == ProcessExitKind::Signaled);
        CHECK(signaled.exit.code == 15);
    }

    // 처리되지 않은 예외·잘못된 명령으로 끝난 자식은 대화상자 없이 끝나고 0이 아닌 종료로 보고된다.
    Probe crash;
    crash.Add("crash");
    const auto crashed = RunProbe(crash.Info());
    REQUIRE(crashed.status);
    CHECK(!crashed.timedOut);
    CHECK(crashed.exit.kind == ProcessExitKind::Signaled || crashed.exit.code != 0);
}

TEST(Process_ProcessIdentityHelpers) {
    CHECK(!IsProcessRunning(0));
    CHECK(IsProcessRunning(CurrentProcessId()));

    char path[4096];
    usize bytes = 0;
    REQUIRE(GetExecutablePath(path, sizeof(path), bytes));
    CHECK(bytes > 0 && path[bytes] == '\0');
    char small[4];
    usize required = 0;
    CHECK(GetExecutablePath(small, sizeof(small), required).code == FileCode::Overflow);
    CHECK(required == bytes);
    CHECK(GetExecutablePath(nullptr, 4, required).code == FileCode::InvalidArgument);

    // 자식이 부모 PID로 부모 생존을 확인한다(워커의 보조 감시 방식).
    char parent[32];
    snprintf(parent, sizeof(parent), "%u", CurrentProcessId());
    Probe probe;
    probe.Add("parent-alive").Add(parent);
    const auto run = RunProbe(probe.Info());
    if (SkipUnsupported(run.status)) {
        return;
    }

    REQUIRE(run.status);
    CHECK(run.exit.kind == ProcessExitKind::Exited && run.exit.code == 0);
}
