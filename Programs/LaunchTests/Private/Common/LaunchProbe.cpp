#include "LaunchProbe.h"

#include <Base/Compiler.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/CrashReporting.h>
#include <Platform/Fatal.h>
#include <Platform/Clock.h>
#include <Platform/Memory.h>
#include <Platform/Thread.h>

namespace rocks::launch::probe {

namespace {

inline constexpr LogCategory LogLaunchProbe{"LaunchProbe"};

constexpr int TeardownExitCode = 7;
constexpr usize LineBytes = 4096;
constexpr char HexDigits[] = "0123456789abcdef";

bool Is(StringView argument, const char* text) noexcept {
    return argument == StringView(text);
}

// 정적 소멸 표시다. teardown·no-teardown mode가 켠다. 정상 종료에서는 CRT 종료 때 한 번 쓴다.
struct TeardownMarker {
    bool armed = false;

    ~TeardownMarker() noexcept {
        if (armed) {
            WriteProbeLine("LAUNCH STATIC TEARDOWN");
        }
    }
};

TeardownMarker teardownMarker;

// 인자 하나를 "<prefix><hex>" 줄로 쓴다(콘솔 인코딩과 무관하게 byte를 그대로 비교한다).
void WriteHexLine(const char* prefix, StringView value) noexcept {
    char line[LineBytes];
    FixedStringBuilder text{Span<char>(line)};
    (void)text.TryAppendFormat("{}", StringView(prefix));
    for (usize index = 0; index < value.ByteCount(); ++index) {
        const auto byte = static_cast<uint8>(value.Data()[index]);
        const char digits[2] = {HexDigits[byte >> 4], HexDigits[byte & 0x0F]};
        (void)text.TryAppendFormat("{}", StringView(digits, 2));
    }

    WriteProbeLine(line);
}

// 본문 안에서 접근 위반을 낸다(보호 프레임 안의 호출된 함수다, D34).
ROCKS_NOINLINE void FaultAccess() noexcept {
    volatile int* volatile target = nullptr;
    *target = 1;
}

void EmptyThread(void*) noexcept {
}

// platform::Thread 시작·join 왕복 count번의 합계 시간이다(측정 실패면 0).
uint64 MeasureThreadRoundTrips(uint32 count) noexcept {
    const uint64 start = platform::MonotonicNanoseconds();
    for (uint32 index = 0; index < count; ++index) {
        platform::Thread thread;
        if (thread.Start(&EmptyThread, nullptr) != platform::ThreadStatus::Success ||
            thread.Join() != platform::ThreadStatus::Success) {
            return 0;
        }
    }

    return platform::MonotonicNanoseconds() - start;
}

bool ParseExitCode(StringView text, int& value) noexcept {
    constexpr int MaxProbeExitCode = 255;
    constexpr int DecimalBase = 10;
    value = 0;
    if (text.ByteCount() == 0) {
        return false;
    }

    for (usize index = 0; index < text.ByteCount(); ++index) {
        const char digit = text.Data()[index];
        if (digit < '0' || digit > '9') {
            return false;
        }

        value = value * DecimalBase + (digit - '0');
        if (value > MaxProbeExitCode) {
            return false;
        }
    }

    return true;
}

// 옵션과 그 값을 건너뛴 첫 인자(mode)의 위치다. 없으면 arguments.Size()다.
usize ModeIndex(Span<const StringView> arguments) noexcept {
    for (usize index = 0; index < arguments.Size(); ++index) {
        if (Is(arguments[index], "--crash-dir") || Is(arguments[index], "--crash-helper")) {
            ++index;
            continue;
        }

        if (arguments[index].ByteCount() < 2 || arguments[index].Data()[0] != '-' ||
            arguments[index].Data()[1] != '-') {
            return index;
        }
    }

    return arguments.Size();
}

// 시험이 도우미 종료를 기다릴 수 있게 도우미 pid를 알린다(0이면 도우미가 없다).
void WriteHelperPid() noexcept {
    char line[LineBytes];
    FixedStringBuilder text{Span<char>(line)};
    (void)text.TryAppendFormat("LAUNCH HELPER PID {}", platform::CrashHelperProcessId());
    WriteProbeLine(line);
}

int WriteState(ApplicationContext& context) noexcept {
    char line[LineBytes];
    FixedStringBuilder text{Span<char>(line)};
    (void)text.TryAppendFormat("LAUNCH STATE crash={} helper={} toolLog={} terminationHandler={}",
                               platform::IsCrashReportingInstalled() ? 1 : 0,
                               platform::CrashHelperProcessId() != 0 ? 1 : 0,
                               context.ToolLog() != nullptr ? 1 : 0,
                               context.Settings().handleTerminationRequests ? 1 : 0);
    WriteProbeLine(line);
    WriteHelperPid();
    char folder[LineBytes];
    const usize folderBytes = platform::CopyCrashReportFolder(folder, sizeof(folder));
    WriteHexLine("LAUNCH FOLDER ", StringView(folder, folderBytes));
    return 0;
}

// 범용 할당자를 통해 받은 할당이 Launch의 시스템 할당자에 닿는지 본다. BinnedAllocator면 작은
// 블록은 닿지 않고(예약 범위의 slab에서 나온다) 큰 블록만 닿는다. SystemAllocator면 모두 닿는다.
int WriteAllocatorState(ApplicationContext& context) noexcept {
    constexpr usize SmallBlocks = 64;
    constexpr usize SmallBytes = 48;
    constexpr usize LargeBytes = 64 * 1024;
    constexpr usize BlockAlignment = 16;
    Allocator& memory = context.DefaultAllocator();
    // Launch의 시스템 할당자는 rocks::SystemAllocator다(Launch/Application.h — 범용 할당자의 상위).
    const auto& system = static_cast<const SystemAllocator&>(context.SystemAllocator());
    const uint64 before = system.Stats().liveAllocations;
    void* small[SmallBlocks] = {};
    bool clean = true;
    for (void*& block : small) {
        auto result = memory.Allocate(SmallBytes, BlockAlignment);
        clean = clean && result;
        block = result ? result.Value() : nullptr;
    }

    const uint64 afterSmall = system.Stats().liveAllocations;
    auto large = memory.Allocate(LargeBytes, BlockAlignment);
    const uint64 afterLarge = system.Stats().liveAllocations;
    clean = clean && large && memory.Deallocate(large.Value());
    for (void* block : small) {
        clean = clean && block != nullptr && memory.Deallocate(block);
    }

    clean = clean && system.Stats().liveAllocations == before;
    char line[LineBytes];
    FixedStringBuilder text{Span<char>(line)};
    (void)text.TryAppendFormat(
        "LAUNCH ALLOCATOR kind={} distinct={} smallInSystem={} largeInSystem={} clean={}",
        StringView(context.Settings().defaultAllocator == DefaultAllocatorKind::Binned ? "binned"
                                                                                       : "system"),
        &memory != &context.SystemAllocator() ? 1 : 0, afterSmall - before, afterLarge - afterSmall,
        clean ? 1 : 0);
    WriteProbeLine(line);
    return clean ? 0 : 1;
}

// 메인 스레드 호스트 시험(--thread-host, Docs/Design/MacPlatform.ko.md 3절)이다. 호스트는 본문을 새
// 스레드에서 돌리고 join한 뒤 본문의 반환값을 돌려준다. Cocoa run loop 대신 join만 하는 호스트다.
uint64 hostThreadId = 0;

struct HostedBody {
    int (*body)(void*) noexcept = nullptr;
    void* context = nullptr;
    int exitCode = 0;
};

void RunHostedBody(void* raw) noexcept {
    auto& hosted = *static_cast<HostedBody*>(raw);
    hosted.exitCode = hosted.body(hosted.context);
}

int ProbeMainThreadHost(int (*body)(void*) noexcept, void* context) noexcept {
    hostThreadId = platform::CurrentThreadId();
    WriteProbeLine("LAUNCH HOST STARTED");
    HostedBody hosted{body, context, 0};
    platform::Thread thread;
    platform::ThreadOptions options;
    options.name = "ProbeGameThread";
    if (thread.Start(&RunHostedBody, &hosted, options) != platform::ThreadStatus::Success ||
        thread.Join() != platform::ThreadStatus::Success) {
        WriteProbeLine("LAUNCH HOST THREAD FAILED");
        return 1;
    }

    WriteProbeLine("LAUNCH HOST JOINED");
    return hosted.exitCode;
}

} // namespace

void WriteProbeLine(const char* text) noexcept {
    usize length = 0;
    while (text[length] != '\0') {
        ++length;
    }

    platform::WriteDiagnostic(text, length);
    platform::WriteDiagnostic("\n", 1);
}

void Configure(Span<const StringView> arguments, ApplicationSettings& settings) noexcept {
    for (usize index = 0; index < arguments.Size(); ++index) {
        const StringView argument = arguments[index];
        if (Is(argument, "--crash-dir") && index + 1 < arguments.Size()) {
            // ProcessCommandLine 인자의 Data()는 NUL로 끝나고 프로세스 수명 동안 유효하다.
            settings.crashDirectory = arguments[++index].Data();
        } else if (Is(argument, "--crash-off")) {
            settings.crashReporting = CrashReportingMode::Off;
        } else if (Is(argument, "--in-process")) {
            settings.crashReporting = CrashReportingMode::InProcess;
        } else if (Is(argument, "--crash-helper") && index + 1 < arguments.Size()) {
            settings.crashHelperPath = arguments[++index].Data();
        } else if (Is(argument, "--bad-budget")) {
            // PlatformBase 상한(10분)을 넘는 예산이다.
            constexpr uint32 TooLargeBudgetMilliseconds = 24 * 60 * 60 * 1000;
            settings.crashCallbackBudgetMilliseconds = TooLargeBudgetMilliseconds;
        } else if (Is(argument, "--tool-log")) {
            settings.toolLogging = ToolLogging::Stderr;
        } else if (Is(argument, "--terminate")) {
            settings.handleTerminationRequests = true;
        } else if (Is(argument, "--system-allocator")) {
            settings.defaultAllocator = DefaultAllocatorKind::System;
        } else if (Is(argument, "--reset-sigint")) {
            ResetIgnoredTerminationSignal();
        } else if (Is(argument, "--ignore-sigint")) {
            IgnoreTerminationSignal();
        } else if (Is(argument, "--thread-host")) {
            settings.mainThreadHost = &ProbeMainThreadHost;
        }
    }
}

int Run(ApplicationContext& context) noexcept {
    const Span<const StringView> arguments = context.Arguments();
    const usize modeIndex = ModeIndex(arguments);
    if (modeIndex == arguments.Size()) {
        return ProbeUsageExitCode;
    }

    const StringView mode = arguments[modeIndex];
    if (Is(mode, "args")) {
        char line[LineBytes];
        for (usize index = 0; index < arguments.Size(); ++index) {
            FixedStringBuilder prefix{Span<char>(line)};
            (void)prefix.TryAppendFormat("LAUNCH ARG {} ", index);
            WriteHexLine(line, arguments[index]);
        }

        WriteHexLine("LAUNCH EXE ", context.ExecutablePath());
        return 0;
    }

    // 본문이 메인 스레드 호스트가 만든 스레드에서 도는지 본다(--thread-host와 함께). 호스트가
    // 없으면 hostThreadId가 0이라 실패한다.
    if (Is(mode, "host-thread")) {
        const bool hosted = hostThreadId != 0 && platform::CurrentThreadId() != hostThreadId;
        WriteProbeLine(hosted ? "LAUNCH BODY ON HOSTED THREAD" : "LAUNCH BODY ON MAIN THREAD");
        return hosted ? HostedBodyExitCode : 1;
    }

    if (Is(mode, "exit")) {
        int code = 0;
        return modeIndex + 1 < arguments.Size() && ParseExitCode(arguments[modeIndex + 1], code)
                   ? code
                   : ProbeUsageExitCode;
    }

    if (Is(mode, "state")) {
        return WriteState(context);
    }

    if (Is(mode, "log")) {
        Log::Info(LogLaunchProbe, "launch probe tool log line");
        return context.ToolLog() != nullptr ? 0 : 1;
    }

    if (Is(mode, "crash")) {
        context.SetStage(platform::RunPhase::Running, "LaunchProbe.Crash");
        WriteHelperPid();
        FaultAccess();
        return 0;
    }

    if (Is(mode, "fatal")) {
        platform::Fatal("launch probe fatal");
    }

    if (Is(mode, "signal-self") || Is(mode, "signal-twice")) {
        // 처리기는 OS가 다른 스레드(콘솔 처리기 스레드)나 신호 문맥에서 부른다. 표시가 켜질 때까지
        // 양보하며 기다린다(순서는 표시로 정한다. 마감은 멈춘 경우를 끊는 장치일 뿐이다).
        constexpr uint64 SignalWaitNanoseconds = 30ull * 1000 * 1000 * 1000;
        const SignalReadiness readiness = CheckTerminationSignal();
        if (readiness == SignalReadiness::Ignored) {
            WriteProbeLine("LAUNCH SIGINT IGNORED");
            return SignalIgnoredExitCode;
        }

        if (readiness == SignalReadiness::SharedConsole) {
            WriteProbeLine("LAUNCH CONSOLE SHARED");
            return SharedConsoleExitCode;
        }

        if (IsTerminationRequested() || !RaiseTerminationSignal()) {
            return 1;
        }

        const uint64 deadline = platform::MonotonicNanoseconds() + SignalWaitNanoseconds;
        while (!IsTerminationRequested() && platform::MonotonicNanoseconds() < deadline) {
            platform::YieldThread();
        }

        if (!IsTerminationRequested()) {
            return 1;
        }

        WriteProbeLine("LAUNCH TERMINATION REQUESTED");
        if (Is(mode, "signal-twice")) {
            // 두 번째 요청은 처리기가 넘겨 OS 기본 동작(즉시 종료)이다. 돌아오면 실패다.
            (void)RaiseTerminationSignal();
            const uint64 second = platform::MonotonicNanoseconds() + SignalWaitNanoseconds;
            while (platform::MonotonicNanoseconds() < second) {
                platform::YieldThread();
            }

            return 1;
        }

        return 0;
    }

    if (Is(mode, "install-invalid")) {
        // 수집기가 없는 상태(--crash-off)에서 설치가 잘못된 옵션을 거절하고 아무것도 걸지 않는지
        // 본다.
        platform::CrashReportingOptions invalid;
        invalid.programName = "bad/name";
        const bool rejected = platform::InstallCrashReporting(invalid) ==
                              platform::CrashReportingStatus::InvalidArgument;
        const bool clean = !platform::IsCrashReportingInstalled();
        WriteProbeLine(rejected && clean ? "LAUNCH INSTALL REJECTED" : "LAUNCH INSTALL ACCEPTED");
        return rejected && clean ? 0 : 1;
    }

    if (Is(mode, "threads")) {
        constexpr uint32 MaxThreadRoundTrips = 100000;
        constexpr uint32 DecimalBase = 10;
        uint32 roundTrips = 0;
        const StringView count =
            modeIndex + 1 < arguments.Size() ? arguments[modeIndex + 1] : StringView();
        for (usize index = 0; index < count.ByteCount(); ++index) {
            const char digit = count.Data()[index];
            if (digit < '0' || digit > '9' || roundTrips > MaxThreadRoundTrips) {
                return ProbeUsageExitCode;
            }

            roundTrips = roundTrips * DecimalBase + static_cast<uint32>(digit - '0');
        }

        if (roundTrips == 0 || roundTrips > MaxThreadRoundTrips) {
            return ProbeUsageExitCode;
        }

        const uint64 elapsed = MeasureThreadRoundTrips(roundTrips);
        char line[LineBytes];
        FixedStringBuilder text{Span<char>(line)};
        (void)text.TryAppendFormat("LAUNCH THREADS {} {}", roundTrips, elapsed);
        WriteProbeLine(line);
        return elapsed != 0 ? 0 : 1;
    }

    if (Is(mode, "allocator")) {
        return WriteAllocatorState(context);
    }

    if (Is(mode, "allocator-leak") || Is(mode, "allocator-leak-no-teardown")) {
        constexpr usize LeakedBytes = 48;
        constexpr usize LeakedAlignment = 16;
        if (!context.DefaultAllocator().Allocate(LeakedBytes, LeakedAlignment)) {
            return 1;
        }

        if (Is(mode, "allocator-leak-no-teardown")) {
            context.RequestExitWithoutStaticTeardown();
        }

        WriteProbeLine("LAUNCH ALLOCATION LEFT");
        return AllocatorLeakExitCode;
    }

    if (Is(mode, "teardown") || Is(mode, "no-teardown")) {
        teardownMarker.armed = true;
        if (Is(mode, "no-teardown")) {
            context.RequestExitWithoutStaticTeardown();
        }

        return TeardownExitCode;
    }

    return ProbeUsageExitCode;
}

} // namespace rocks::launch::probe
