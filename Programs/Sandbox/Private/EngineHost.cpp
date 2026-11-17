#include "EngineHost.h"

#include <Core/Containers/Array.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Text/StringBuilder.h>
#include <Engine/Engine.h>
#include <HostConfig/HostSettings.h>
#include <LogHost/LogHost.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <TraceCapture/TraceSession.h>
#include <TraceControl/TraceController.h>
#include <TraceControl/TraceSettings.generated.h>
#include <stdio.h>
#include <stdlib.h>

namespace sandbox {

namespace {

using namespace rocks;

// Sandbox 엔진 호스트의 로그 카테고리다.
inline constexpr LogCategory LogSandboxHost{"Sandbox"};

// 헤드리스 실행의 고정 프레임 시간이다(결정적 시간 진행).
constexpr double HeadlessFrameSeconds = 1.0 / 60.0;

// 자체 시험이 띄우는 자식의 프레임 수와 대기 상한이다(상한은 멈춘 자식을 끊는 장치일 뿐이다).
constexpr uint64 SelfTestFrames = 3;
constexpr uint32 SelfTestTimeoutMilliseconds = 60000;

// 자식 stderr를 모으는 버퍼 크기다.
constexpr usize SelfTestOutputBytes = 64 * 1024;

// 엔진을 World 하나로 frames만큼 돌린다. 실패 원인은 엔진이 한 번 기록한다(호스트는 다시 기록하지
// 않는다).
int RunEngine(Allocator& allocator, uint64 frames, config::ConfigContext& settings) noexcept {
    EngineConfig config;
    config.maxFrames = frames;
    config.settings = &settings;
    config.fixedFrameDeltaSeconds = HeadlessFrameSeconds;
    Engine engine(allocator);
    if (!engine.Initialize(config)) {
        return 1;
    }

    int exitCode = 0;
    WorldCreateDesc world;
    world.name = "Main";
    if (!engine.CreateWorld(world) || !engine.Run()) {
        exitCode = 1;
    }

    if (!engine.Shutdown()) {
        exitCode = 1;
    }

    return exitCode;
}

// 자식 실행 결과다.
struct ChildRun {
    bool exited = false;
    uint32 code = 0;
    StringView stderrText;
};

ChildRun RunChild(Span<const platform::ProcessArgument> arguments, char* errorText,
                  usize errorCapacity) noexcept {
    ChildRun child;
    char executable[4096];
    usize executableBytes = 0;
    if (!platform::GetExecutablePath(executable, sizeof(executable), executableBytes)) {
        return child;
    }

    platform::ProcessStartInfo info;
    info.executable = executable;
    info.executableBytes = executableBytes;
    info.arguments = arguments.Data();
    info.argumentCount = arguments.Size();
    info.output = platform::ProcessStreamMode::Null;
    info.error = platform::ProcessStreamMode::Pipe;
    platform::ProcessRunOptions run;
    run.error = errorText;
    run.errorCapacity = errorCapacity;
    run.timeoutMilliseconds = SelfTestTimeoutMilliseconds;
    const platform::ProcessRunResult result = platform::RunProcess(info, run);
    child.exited =
        result.status && !result.timedOut && result.exit.kind == platform::ProcessExitKind::Exited;
    child.code = result.exit.code;
    child.stderrText = StringView(errorText, result.errorBytes);
    return child;
}

struct FirstFile {
    char name[256]{};
    bool found = false;
};

bool FindFirstFile(const platform::DirectoryEntry& entry, void* raw) noexcept {
    auto& first = *static_cast<FirstFile*>(raw);
    if (entry.info.type != platform::FileType::File || entry.nameBytes >= sizeof(first.name)) {
        return true;
    }

    platform::MemoryCopy(first.name, entry.name, entry.nameBytes);
    first.found = true;
    return false;
}

// 설정 파일로 Sync 모드와 파일 기록을 켠 자식이 로그 파일에 종료 줄을 남기는지 본다.
bool CheckConfiguredFileLogging() noexcept {
    char directory[128];
    FixedStringBuilder directoryText{Span<char>(directory)};
    (void)directoryText.TryAppendFormat("rocks-sandbox-selftest-{}", platform::CurrentProcessId());
    platform::FileSystem& files = platform::PhysicalFileSystem();
    (void)platform::DeleteDirectoryTree(files, directory, directoryText.ByteCount());
    if (!platform::MakeDirectoryTree(files, directory, directoryText.ByteCount())) {
        return false;
    }

    char configPath[192];
    FixedStringBuilder configText{Span<char>(configPath)};
    (void)configText.TryAppendFormat("{}/Engine.json", directoryText.View());
    char json[512];
    FixedStringBuilder jsonText{Span<char>(json)};
    (void)jsonText.TryAppendFormat(
        "{{\"Settings\": {{\"log.mode\": \"sync\", \"log.file.enabled\": true, "
        "\"log.file.directory\": \"{}/Logs\"}}}}\n",
        directoryText.View());
    bool passed = false;
    {
        platform::File config;
        passed = config.Open(configPath, platform::FileOpenMode::CreateAlways) &&
                 config.WriteAll(json, jsonText.ByteCount()).status && config.Close();
    }

    if (passed) {
        const platform::ProcessArgument arguments[] = {{"--engine", 8},
                                                       {"--frames", 8},
                                                       {"2", 1},
                                                       {"--config", 8},
                                                       {configPath, configText.ByteCount()}};
        static char errorText[SelfTestOutputBytes];
        const ChildRun child = RunChild(arguments, errorText, sizeof(errorText));
        char logs[192];
        FixedStringBuilder logsText{Span<char>(logs)};
        (void)logsText.TryAppendFormat("{}/Logs", directoryText.View());
        FirstFile first;
        (void)files.EnumerateDirectory(logs, logsText.ByteCount(), &FindFirstFile, &first);
        char logPath[512];
        FixedStringBuilder logPathText{Span<char>(logPath)};
        (void)logPathText.TryAppendFormat("{}/{}", logsText.View(), StringView(first.name));
        static char content[SelfTestOutputBytes];
        usize contentBytes = 0;
        platform::File log;
        if (first.found && log.Open(logPath, platform::FileOpenMode::ReadExisting)) {
            const platform::FileTransfer read = log.Read(content, sizeof(content));
            contentBytes = read.bytes;
        }

        passed = child.exited && child.code == 0 &&
                 StringView(content, contentBytes)
                     .Contains("[Info][Engine] Engine shut down", StringComparison::CaseSensitive);
        if (!passed) {
            printf("    exit %u, log file '%s' (%llu bytes), stderr:\n%.*s\n", child.code,
                   first.found ? first.name : "<none>",
                   static_cast<unsigned long long>(contentBytes),
                   static_cast<int>(child.stderrText.ByteCount()), child.stderrText.Data());
        }
    }

    (void)platform::DeleteDirectoryTree(files, directory, directoryText.ByteCount());
    return passed;
}

} // namespace

int RunEngineHost(Allocator& allocator, const EngineHostOptions& options) noexcept {
    char engineDir[4096] = {};
    HostSettingsOptions boot;
    if (auto found = FindEngineDirectory(Span<char>(engineDir)); found) {
        boot.engineDir = StringView(engineDir, found.Value());
    } else {
        // 엔진 층 파일 없이 코드 기본값으로 돈다(설치 전이므로 비상 경로로 나간다).
        Log::Warning(LogSandboxHost,
                     "Engine/Config was not found above the executable ({Error}); engine config "
                     "files are not read",
                     ErrorCodeName(found.Error()));
    }

    // 1)–5) 설정 문맥과 시작 설정. 실패는 HostSettings가 진단과 함께 기록했다.
    HostSettings settings(allocator);
    if (!settings.Initialize(boot)) {
        return 1;
    }

    if (!settings.LoadFiles() ||
        (options.configPath != nullptr &&
         !settings.AddFile(config::ConfigLayer::Project, "Engine", options.configPath)) ||
        !settings.ApplyCommandLine(options.arguments) || !settings.CompleteStartup()) {
        return 2;
    }

    // 6) 로그. log.mode·log.file.* 는 5)에서 고정됐다.
    LogHost host(allocator);
    if (!host.Start(LogSystemConfig::ForEngineHost(), &settings.Context(), "Sandbox")) {
        Log::Error(LogSandboxHost, "The log system could not start");
        return 1;
    }

    int exitCode = 0;
    {
        LogInstallScope installed(host.System());
        (void)host.System().SetThreadName("GameThread");
        (void)host.Watch(settings.Context());
        // 7) trace 세션(설정 섹션 Trace)·콘솔 명령·--trace. 세션을 열지 못해도 엔진은 돈다.
        TraceSession trace;
        TraceSessionSettings traceSettings = trace_control::MakeTraceSessionSettings(
            settings.Context().Get<trace_control::TraceCaptureSettings>(), allocator);
        if (auto started = trace.Start(traceSettings); !started) {
            Log::Warning(LogSandboxHost, "Trace capture could not start: {Error}",
                         ErrorCodeName(started.Error()));
        }

        {
            trace_control::TraceController control(allocator, trace, settings.Context());
            if (trace.IsRunning()) {
                (void)control.RegisterConsoleCommands();
                (void)control.ApplyCommandLine(options.arguments);
            }

            // 8) 엔진 루프(프레임마다 sync 지점).
            exitCode = RunEngine(allocator, options.frames, settings.Context());
        }

        trace.Stop();
    }

    host.Stop();
    if (host.WriterAbandoned()) {
        // 멈춘 sink에 걸린 출력 스레드가 남았다. 설정 문맥·전역 소멸자·CRT 종료 처리를 돌리면 그
        // 스레드와 겹칠 수 있으므로 정리 없이 끝낸다(LogHost::Stop 설명).
        _Exit(exitCode);
    }

    return exitCode;
}

uint32 RunEngineHostSelfTest() noexcept {
    char frames[24];
    FixedStringBuilder framesText{Span<char>(frames)};
    (void)framesText.TryAppendFormat("{}", SelfTestFrames);
    const platform::ProcessArgument arguments[] = {
        {"--engine", 8}, {"--frames", 8}, {frames, framesText.ByteCount()}};
    static char errorText[SelfTestOutputBytes];
    const ChildRun child = RunChild(arguments, errorText, sizeof(errorText));
    const char* const expected[] = {
        "[Info][Engine] Engine initialized with ",
        "[Info][Engine] Engine run finished after 3 frames: MaxFrames",
        "[Info][Engine] Engine shutting down with 1 worlds after 3 frames",
        "[Info][Engine] Engine shut down"};
    bool passed = child.exited && child.code == 0;
    for (const char* line : expected) {
        passed =
            passed && child.stderrText.Contains(StringView(line), StringComparison::CaseSensitive);
    }

    printf("  [%s] engine host: --engine --frames 3 logs start, run finished and shutdown\n",
           passed ? "PASS" : "FAIL");
    if (!passed) {
        printf("    exit %u, stderr:\n%.*s\n", child.code,
               static_cast<int>(child.stderrText.ByteCount()), child.stderrText.Data());
    }

    const bool configured = CheckConfiguredFileLogging();
    printf("  [%s] engine host: --config switches to sync and writes the log file\n",
           configured ? "PASS" : "FAIL");
    return (passed ? 0u : 1u) + (configured ? 0u : 1u);
}

} // namespace sandbox
