#include "EngineHost.h"

#include "GraphicsDeviceTraits.h"
#include "SandboxGame.h"

#include <Base/PlacementNew.h>
#include <Core/Containers/Array.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Engine/Engine.h>
#include <HostConfig/HostSettings.h>
#include <Launch/Application.h>
#include <Platform/CrashReporting.h>
#include <LogHost/LogHost.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#if SANDBOX_TRACE_ANALYSIS
#include <TraceAnalysis/TraceModel.h>
#endif
#include <TraceCapture/TraceLogSink.h>
#include <TraceCapture/TraceSession.h>
#include <TraceControl/TraceController.h>
#include <TraceControl/TraceSettings.generated.h>
#include <stdio.h>
#include <stdlib.h>

namespace sandbox {

namespace {

using namespace rocks;

// 엔진을 멈춘 뒤 trace sink를 빼기 전에 로그 출력 스레드를 기다리는 상한이다.
constexpr uint32 TraceLogFlushTimeoutMilliseconds = 2000;

// Sandbox 엔진 호스트의 로그 카테고리다.
inline constexpr LogCategory LogSandboxHost{"Sandbox"};

// 헤드리스 실행의 고정 프레임 시간이다(결정적 시간 진행).
constexpr double HeadlessFrameSeconds = 1.0 / 60.0;

// 자체 시험이 띄우는 자식의 프레임 수와 대기 상한이다(상한은 멈춘 자식을 끊는 장치일 뿐이다).
constexpr uint64 SelfTestFrames = 3;
constexpr uint32 SelfTestTimeoutMilliseconds = 60000;

// 자식 stderr를 모으는 버퍼 크기다.
constexpr usize SelfTestOutputBytes = 64 * 1024;

// 종료 요청 다리다(설계 20절). Ctrl-C·Ctrl-Break·SIGINT·SIGTERM 처리기는 launch::RequestTermination
// 원자 표시만 남긴다. 게임 스레드가 프레임 첫 단계(PumpEvents)에서 그 표시를 보고
// Engine::RequestExit를 불러 정상 종료로 잇는다. 프레임마다 원자 읽기 하나다.
class TerminationRequestBridge final : public EngineLoopHooks {
public:
    explicit TerminationRequestBridge(uint64 requestAtFrame) noexcept
        : requestAtFrame_(requestAtFrame) {
    }

    void PumpEvents(Engine& engine) noexcept override {
        ++pumped_;
        if (requestAtFrame_ != 0 && pumped_ == requestAtFrame_) {
            launch::RequestTermination();
        }

        if (launch::IsTerminationRequested()) {
            engine.RequestExit();
        }
    }

private:
    uint64 requestAtFrame_;
    uint64 pumped_ = 0;
};

// 호스트 서비스를 등록한다(Docs/Design/ModuleLoader.ko.md 3.12절). 서비스는 선택이라 실패하면
// Warning을 남기고 모듈은 서비스 없이 시작한다.
template <class Service>
void RegisterHostService(HostServices& services, Service& service) noexcept {
    if (auto registered = services.Register(service); !registered) {
        Log::Warning(LogSandboxHost, "Host service {Service} could not be registered: {Error}",
                     StringView(HostServiceTraits<Service>::Key().name),
                     ErrorCodeName(registered.Error()));
    }
}

// 엔진을 GameInstance(기본 맵 "Main" — PlayerStart, 첫 LocalPlayer의 SandboxPawn)로 frames만큼
// 돌린다(Docs/Design/EngineRuntime 21.5절). 실패 원인은 엔진이 한 번 기록한다(호스트는 다시
// 기록하지 않는다).
int RunEngine(Allocator& allocator, uint64 frames, uint64 requestTerminationAtFrame,
              config::ConfigContext& settings, ModuleLoader& modules) noexcept {
    TerminationRequestBridge bridge(requestTerminationAtFrame);
    SandboxGame game;
    GameInstanceClient client(allocator, game.config);
    EngineConfig config;
    config.hooks = &bridge;
    config.client = &client;
    config.modules = &modules;
    config.maxFrames = frames;
    config.settings = &settings;
    config.fixedFrameDeltaSeconds = HeadlessFrameSeconds;
    config.registerTypes = &RegisterSandboxTypes;
    Engine engine(allocator);
    if (!engine.Initialize(config)) {
        return 1;
    }

    int exitCode = 0;
    if (!engine.Run()) {
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

#if SANDBOX_TRACE_ANALYSIS
// trace 파일에 로그 기록(Trace.LogMessage)이 남는지 확인한다(`--engine --trace=default
// --trace-file <path>`, 로깅 LG15).
bool CheckTraceLogRecords() noexcept {
    char directory[128];
    FixedStringBuilder directoryText{Span<char>(directory)};
    (void)directoryText.TryAppendFormat("rocks-sandbox-trace-{}", platform::CurrentProcessId());
    platform::FileSystem& files = platform::PhysicalFileSystem();
    (void)platform::DeleteDirectoryTree(files, directory, directoryText.ByteCount());
    if (!platform::MakeDirectoryTree(files, directory, directoryText.ByteCount())) {
        return false;
    }

    char tracePath[192];
    FixedStringBuilder traceText{Span<char>(tracePath)};
    (void)traceText.TryAppendFormat("{}/run.rtrace", directoryText.View());
    const platform::ProcessArgument arguments[] = {
        {"--engine", 8},         {"--frames", 8},      {"2", 1},
        {"--trace=default", 15}, {"--trace-file", 12}, {tracePath, traceText.ByteCount()}};
    static char errorText[SelfTestOutputBytes];
    const ChildRun child = RunChild(arguments, errorText, sizeof(errorText));
    SystemAllocator allocator;
    Array<uint8> bytes(allocator);
    bool passed = child.exited && child.code == 0;
    {
        platform::File trace;
        uint64 size = 0;
        passed = passed && trace.Open(tracePath, platform::FileOpenMode::ReadExisting) &&
                 trace.Size(size) && bytes.TryResize(static_cast<usize>(size)) &&
                 trace.ReadExact(bytes.Data(), bytes.Size()).status;
    }

    usize logs = 0;
    bool finished = false;
    if (passed) {
        TraceModel model(allocator);
        passed = static_cast<bool>(model.Build(bytes.View()));
        if (passed) {
            logs = model.Logs().Size();
            for (const TraceModelLog& log : model.Logs()) {
                finished = finished || log.message.Contains("Engine run finished",
                                                            StringComparison::CaseSensitive);
            }
        }
    }

    passed = passed && logs != 0 && finished;
    if (!passed) {
        printf("    exit %u, trace bytes %llu, log records %llu, stderr:\n%.*s\n", child.code,
               static_cast<unsigned long long>(bytes.Size()), static_cast<unsigned long long>(logs),
               static_cast<int>(child.stderrText.ByteCount()), child.stderrText.Data());
    }

    (void)platform::DeleteDirectoryTree(files, directory, directoryText.ByteCount());
    return passed;
}

#endif

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

// 종료 요청 다리: 자식을 프레임 상한 없이(아주 큰 값) 띄우고 2번째 프레임에서 종료 요청 표시를
// 남기게 한다. 다리가 RequestExit로 이어 "RequestExit" 사유로 정상 종료(0)해야 한다. 순서는 프레임
// 번호로 정해진다(시간·경쟁과 무관).
bool CheckTerminationRequestBridge() noexcept {
    const platform::ProcessArgument arguments[] = {{"--engine", 8},
                                                   {"--frames", 8},
                                                   {"1000000", 7},
                                                   {"--request-termination-at-frame", 30},
                                                   {"2", 1}};
    static char errorText[SelfTestOutputBytes];
    const ChildRun child = RunChild(arguments, errorText, sizeof(errorText));
    const bool passed =
        child.exited && child.code == 0 &&
        child.stderrText.Contains("frames: RequestExit", StringComparison::CaseSensitive) &&
        child.stderrText.Contains("[Info][Engine] Engine shut down",
                                  StringComparison::CaseSensitive);
    if (!passed) {
        printf("    exit %u, stderr:\n%.*s\n", child.code,
               static_cast<int>(child.stderrText.ByteCount()), child.stderrText.Data());
    }

    return passed;
}

// 장치 능력을 기기 특성으로 옮기면 품질 등급 요구(기기 조건)가 그 값으로 판정된다(설정 CF4 —
// Config는 Graphics를 모르고 호스트가 특성을 채운다).
bool CheckDeviceTraitsFromCaps(Allocator& allocator) noexcept {
    graphics::DeviceCaps caps;
    caps.limits.textureSize2D = 16384;
    caps.limits.colorAttachments = 8;
    caps.features.computeShaders = true;
    config::DeviceTraits traits(allocator);
    if (!AppendGraphicsDeviceTraits(caps, traits)) {
        return false;
    }

    config::DeviceCondition large(allocator);
    config::DeviceCondition huge(allocator);
    String reason(allocator);
    if (!large.trait.TryAssign(config::DeviceTraitNames::GpuTextureSize2D) ||
        !huge.trait.TryAssign(config::DeviceTraitNames::GpuTextureSize2D)) {
        return false;
    }

    large.atLeast = 8192.0;
    huge.atLeast = 32768.0;
    auto fits = config::MatchDeviceCondition(traits, large, reason);
    auto exceeds = config::MatchDeviceCondition(traits, huge, reason);
    config::DeviceTrait compute;
    return fits && fits.Value() && exceeds && !exceeds.Value() &&
           traits.Find(config::DeviceTraitNames::GpuComputeShaders, compute) &&
           compute.number == 1.0;
}

} // namespace

// 엔진 호스트 본문이다(Docs/Design/ModuleLoader.ko.md 3.11절). Launch가 소유한 프로세스 모듈
// 로더(부트 모듈이 시작해 있다)로 설정(HostSettings — 모든 모듈의 RegisterSettings) → LogHost·trace
// → PostConfigInit 단계 → 엔진(엔진 단계) → 호스트 묶음 종료를 돈다. 프로세스 묶음은 Launch가
// 내린다. 멈춘 로그 출력 스레드를 포기했으면 LogHost·설정 문맥을 파괴하지 않고 정적 소멸 없이
// 끝내도록 요청한다(설정 문맥이 모듈 lease를 잡은 채 남으므로 Launch도 로더를 내리지 않는다).
int RunEngineHost(launch::ApplicationContext& context, const EngineHostOptions& options) noexcept {
    // 프로세스의 범용 할당자다(기본 BinnedAllocator — Launch가 소유한다).
    Allocator& allocator = context.DefaultAllocator();
    ModuleLoader& modules = context.Modules();
    char engineDir[4096] = {};
    HostSettingsOptions boot;
    boot.modules = &modules;
    if (auto found = FindEngineDirectory(Span<char>(engineDir)); found) {
        boot.engineDir = StringView(engineDir, found.Value());
    } else {
        // 엔진 층 파일 없이 코드 기본값으로 돈다(설치 전이므로 비상 경로로 나간다).
        Log::Warning(LogSandboxHost,
                     "Engine/Config was not found above the executable ({Error}); engine config "
                     "files are not read",
                     ErrorCodeName(found.Error()));
    }

    // 1)–5) 설정 문맥과 시작 설정. 실패는 HostSettings가 진단과 함께 기록했다. 설정 문맥과
    // LogHost는 정적 저장소에 만들고 끝에서 직접 파괴한다(멈춘 출력 스레드를 포기했으면 파괴하지
    // 않고 Launch가 정적 소멸 없이 끝낸다).
    alignas(HostSettings) static unsigned char settingsStorage[sizeof(HostSettings)];
    alignas(LogHost) static unsigned char hostStorage[sizeof(LogHost)];
    HostSettings& settings = *::new (settingsStorage) HostSettings(allocator);
    struct SettingsScope {
        HostSettings* settings;
        ~SettingsScope() noexcept {
            if (settings != nullptr) {
                settings->~HostSettings();
            }
        }
    } settingsScope{&settings};
    if (!settings.Initialize(boot)) {
        return 1;
    }

    if (!settings.LoadFiles() ||
        (options.configPath != nullptr &&
         !settings.AddFile(config::ConfigLayer::Project, "Engine", options.configPath)) ||
        !settings.ApplyCommandLine(options.arguments) || !settings.CompleteStartup()) {
        return 2;
    }

    // 6) 로그. log.mode·log.file.* 는 5)에서 고정됐다. 접근 위반·치명 신호 때 마지막 로그는
    // Launch가 설치한 크래시 수집기의 종료 콜백이 flush한다.
    LogHost& host = *::new (hostStorage) LogHost(allocator);
    struct HostScope {
        LogHost* host;
        ~HostScope() noexcept {
            if (host != nullptr) {
                host->~LogHost();
            }
        }
    } hostScope{&host};
    if (!host.Start(LogSystemConfig::ForEngineHost(), &settings.Context(), "Sandbox")) {
        Log::Error(LogSandboxHost, "The log system could not start");
        return 1;
    }

    int exitCode = 0;
    {
        LogInstallScope installed(host.System());
        (void)host.System().SetThreadName("GameThread");
        (void)host.Watch(settings.Context());
        // 개발 빌드는 설정 파일을 감시해 프레임 sync 지점에서 다시 읽는다(출시 규칙이면 Unsupported
        // — 기록하지 않는다).
        (void)settings.StartHotReload();
        // 7) trace 세션(trace.* 설정 — 5)에서 고정됐다)·콘솔 명령·--trace. 세션을 열지 못해도
        // 엔진은 돈다.
        TraceSession trace;
        auto captureSettings = trace_control::ReadTraceCaptureSettings(settings.Context());
        TraceSessionSettings traceSettings = trace_control::MakeTraceSessionSettings(
            captureSettings ? captureSettings.Value() : trace_control::TraceCaptureSettings{},
            allocator);
        if (auto started = trace.Start(traceSettings); !started) {
            Log::Warning(LogSandboxHost, "Trace capture could not start: {Error}",
                         ErrorCodeName(started.Error()));
        }

        // 로그를 trace 채널 "log"로도 보낸다(채널이 꺼져 있으면 sink 비용은 분기 하나). 순서: trace
        // 시작 → AddSink … 엔진 정리 → LogSystem::Flush → RemoveSink → trace 정지.
        TraceLogSink traceLog;
        const bool traceLogAdded = trace.IsRunning() && host.System().AddSink(traceLog);
        {
            trace_control::TraceController control(allocator, trace, settings.Context());
            if (trace.IsRunning()) {
                (void)control.RegisterConsoleCommands();
                (void)control.ApplyCommandLine(options.arguments);
                if (options.traceFile != nullptr) {
                    // 크래시 메타데이터 associatedFiles.traceFile에 남긴다(설계 LC6).
                    const StringView traceFile(options.traceFile);
                    if (control.OpenFile(traceFile)) {
                        (void)platform::SetCrashAnnotation(platform::CrashAnnotation::TraceFile,
                                                           traceFile.Data(), traceFile.ByteCount());
                    }
                }
            }

            // 8) 호스트 서비스 등록(3.12절 — 로그·설정 문맥·실행 중인 trace 세션) → 호스트 단계
            // 모듈(UE PostConfigInit) → 엔진 루프(프레임마다 sync 지점) → 호스트 묶음 종료(trace·
            // LogHost가 아직 살아 있을 때 — 3.7절) → 서비스 떼기.
            HostServices& services = modules.Services();
            RegisterHostService(services, host.System());
            RegisterHostService(services, settings.Context());
            if (trace.IsRunning()) {
                RegisterHostService(services, trace);
            }

            const ModuleLoadReport hostModules = modules.StartPhase(LoadingPhase::PostConfigInit);
            if (hostModules.IsOk()) {
                exitCode = RunEngine(allocator, options.frames, options.requestTerminationAtFrame,
                                     settings.Context(), modules);
            } else {
                Log::Error(LogSandboxHost,
                           "Host module {Module} failed to start ({Code}) at {Stage}: {Message}",
                           StringView(hostModules.failed), ModuleLoadCodeName(hostModules.code),
                           StringView(hostModules.stage), StringView(hostModules.message));
                exitCode = 1;
            }

            modules.ShutdownGroup(ModuleGroup::Host);
            (void)services.Unregister(trace);
            (void)services.Unregister(settings.Context());
            (void)services.Unregister(host.System());
            // 출력 스레드에 남은 기록이 trace에 닿게 한 뒤 sink를 뺀다(trace 출력이 닫히기 전에).
            if (traceLogAdded) {
                (void)host.System().Flush(TraceLogFlushTimeoutMilliseconds);
                (void)host.System().RemoveSink(traceLog);
            }
        }

        trace.Stop();
    }

    host.Stop();
    if (host.WriterAbandoned()) {
        // 멈춘 sink에 걸린 출력 스레드가 남았다. LogHost·설정 문맥·전역 소멸자·CRT 종료 처리를
        // 돌리면 그 스레드와 겹칠 수 있으므로 파괴하지 않고, Launch가 크래시 수집을 정상 종료로 뗀
        // 뒤 정적 소멸 없이 끝낸다(LogHost::Stop 설명).
        hostScope.host = nullptr;
        settingsScope.settings = nullptr;
        context.RequestExitWithoutStaticTeardown();
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
        "[Info][Gameplay] Opened world Main",
        "[Info][Sandbox.Game] Pawn possessed by the local player",
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
#if SANDBOX_TRACE_ANALYSIS
    const bool traced = CheckTraceLogRecords();
    printf("  [%s] engine host: --trace=default writes log records into the trace file\n",
           traced ? "PASS" : "FAIL");
#else
    // Release는 Developer 모듈(TraceAnalysis)을 링크하지 않으므로 trace 파일을 읽지 않는다.
    const bool traced = true;
    printf("  [NOT RUN] engine host: trace file log records (TraceAnalysis is not linked in "
           "Release)\n");
#endif
    SystemAllocator allocator;
    const bool traits = CheckDeviceTraitsFromCaps(allocator);
    printf("  [%s] engine host: device caps become config device traits\n",
           traits ? "PASS" : "FAIL");
    const bool terminated = CheckTerminationRequestBridge();
    printf("  [%s] engine host: a termination request ends the engine run normally\n",
           terminated ? "PASS" : "FAIL");
    return (passed ? 0u : 1u) + (configured ? 0u : 1u) + (traced ? 0u : 1u) + (traits ? 0u : 1u) +
           (terminated ? 0u : 1u);
}

} // namespace sandbox
