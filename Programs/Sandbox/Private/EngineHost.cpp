#include "EngineHost.h"

#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Text/StringBuilder.h>
#include <Engine/Engine.h>
#include <Platform/Process.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;

// 헤드리스 실행의 고정 프레임 시간이다(결정적 시간 진행).
constexpr double HeadlessFrameSeconds = 1.0 / 60.0;

// 자체 시험이 띄우는 자식의 프레임 수와 대기 상한이다(상한은 멈춘 자식을 끊는 장치일 뿐이다).
constexpr uint64 SelfTestFrames = 3;
constexpr uint32 SelfTestTimeoutMilliseconds = 60000;

} // namespace

int RunEngineHost(Allocator& allocator, const EngineHostOptions& options) noexcept {
    LogSystem logs(allocator, LogSystemConfig::ForEngineHost());
    StderrLogSink stderrSink;
    if (!logs.AddSink(stderrSink) || !logs.Start()) {
        fprintf(stderr, "Sandbox: the log system could not start\n");
        return 1;
    }

    int exitCode = 0;
    {
        LogInstallScope installed(logs);
        (void)logs.SetThreadName("GameThread");
        EngineConfig config;
        config.maxFrames = options.frames;
        config.fixedFrameDeltaSeconds = HeadlessFrameSeconds;
        Engine engine(allocator);
        // 실패 원인은 엔진이 한 번 기록한다(호스트는 다시 기록하지 않는다).
        if (!engine.Initialize(config)) {
            exitCode = 1;
        } else {
            WorldCreateDesc world;
            world.name = "Main";
            if (!engine.CreateWorld(world) || !engine.Run()) {
                exitCode = 1;
            }

            if (!engine.Shutdown()) {
                exitCode = 1;
            }
        }
    }

    logs.Stop();
    return exitCode;
}

uint32 RunEngineHostSelfTest() noexcept {
    char executable[4096];
    usize executableBytes = 0;
    if (!platform::GetExecutablePath(executable, sizeof(executable), executableBytes)) {
        printf("  [FAIL] engine host: executable path unavailable\n");
        return 1;
    }

    char frames[24];
    FixedStringBuilder framesText{Span<char>(frames)};
    (void)framesText.TryAppendFormat("{}", SelfTestFrames);
    const platform::ProcessArgument arguments[] = {
        {"--engine", 8}, {"--frames", 8}, {frames, framesText.ByteCount()}};
    platform::ProcessStartInfo info;
    info.executable = executable;
    info.executableBytes = executableBytes;
    info.arguments = arguments;
    info.argumentCount = 3;
    info.output = platform::ProcessStreamMode::Null;
    info.error = platform::ProcessStreamMode::Pipe;
    static char errorText[64 * 1024];
    platform::ProcessRunOptions run;
    run.error = errorText;
    run.errorCapacity = sizeof(errorText);
    run.timeoutMilliseconds = SelfTestTimeoutMilliseconds;
    const platform::ProcessRunResult result = platform::RunProcess(info, run);
    const StringView stderrText(errorText, result.errorBytes);
    const char* const expected[] = {
        "[Info][Engine] Engine initialized with ",
        "[Info][Engine] Engine run finished after 3 frames: MaxFrames",
        "[Info][Engine] Engine shutting down with 1 worlds after 3 frames",
        "[Info][Engine] Engine shut down"};
    bool passed = result.status && !result.timedOut &&
                  result.exit.kind == platform::ProcessExitKind::Exited && result.exit.code == 0;
    for (const char* line : expected) {
        passed = passed && stderrText.Contains(StringView(line), StringComparison::CaseSensitive);
    }

    printf("  [%s] engine host: --engine --frames 3 logs start, run finished and shutdown\n",
           passed ? "PASS" : "FAIL");
    if (!passed) {
        printf("    exit kind %u code %u, stderr:\n%.*s\n", static_cast<unsigned>(result.exit.kind),
               result.exit.code, static_cast<int>(stderrText.ByteCount()), stderrText.Data());
    }

    return passed ? 0u : 1u;
}

} // namespace sandbox
