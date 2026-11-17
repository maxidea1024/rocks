#include "EngineHost.h"

#include <Core/Containers/Array.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/StringBuilder.h>
#include <Engine/Engine.h>
#include <LogHost/LogHost.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <Platform/File.h>
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

// 자식 stderr를 모으는 버퍼 크기다.
constexpr usize SelfTestOutputBytes = 64 * 1024;

// 로그 설정용 설정 문맥이다(엔진 시작 전 — 로그는 엔진보다 먼저 준비한다). 소멸 순서: 문맥 →
// Registry → pool → 타입.
class HostSettings {
public:
    explicit HostSettings(Allocator& allocator) noexcept
        : allocator_(allocator), types_(allocator), names_(allocator),
          objects_(allocator, types_, {0x53414e44, 1}, MakeOptions(names_)) {
    }

    ~HostSettings() noexcept {
        context_ = nullptr;
        (void)objects_.Shutdown(objects_.OwnerAccess());
        (void)names_.Shutdown();
        (void)types_.Shutdown();
    }

    HostSettings(const HostSettings&) = delete;
    HostSettings& operator=(const HostSettings&) = delete;

    bool Initialize() noexcept {
        if (!names_.Initialize()) {
            return false;
        }

        auto made = UniquePtr<config::ConfigContext>::TryMake(allocator_, allocator_, objects_,
                                                              config::ConfigContextOptions{});
        if (!made) {
            return false;
        }

        context_ = Move(made.Value());
        return context_->Initialize() && RegisterLogSettings(*context_) && context_->Sync();
    }

    // Engine branch 파일 하나를 Project 층으로 읽는다(log.* 설정은 "Settings" 섹션).
    bool Load(const char* path) noexcept {
        platform::File file;
        uint64 size = 0;
        Array<char> text(allocator_);
        if (!file.Open(path, platform::FileOpenMode::ReadExisting) || !file.Size(size) ||
            !text.TryResize(static_cast<usize>(size)) ||
            !file.ReadExact(text.Data(), text.Size()).status) {
            fprintf(stderr, "Sandbox: cannot read config file '%s'\n", path);
            return false;
        }

        const StringView source(path);
        if (!context_->AddLayerText(config::ConfigLayer::Project, "SandboxConfig", "Engine", source,
                                    StringView(text.Data(), text.Size())) ||
            !context_->Sync()) {
            fprintf(stderr, "Sandbox: config file '%s' was rejected (%llu diagnostics)\n", path,
                    static_cast<unsigned long long>(context_->Diagnostics().Count()));
            return false;
        }

        return true;
    }

    config::ConfigContext& Context() noexcept {
        return *context_;
    }

private:
    static RegistryOptions MakeOptions(NamePool& names) noexcept {
        RegistryOptions options;
        options.names = &names;
        return options;
    }

    Allocator& allocator_;
    TypeRegistry types_;
    NamePool names_;
    ObjectRegistry objects_;
    UniquePtr<config::ConfigContext> context_;
};

// 엔진을 World 하나로 frames만큼 돌린다. 실패 원인은 엔진이 한 번 기록한다(호스트는 다시 기록하지
// 않는다).
int RunEngine(Allocator& allocator, uint64 frames) noexcept {
    EngineConfig config;
    config.maxFrames = frames;
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
    HostSettings settings(allocator);
    if (!settings.Initialize()) {
        fprintf(stderr, "Sandbox: the config context could not start\n");
        return 1;
    }

    if (options.configPath != nullptr && !settings.Load(options.configPath)) {
        return 2;
    }

    LogHost host(allocator);
    if (!host.Start(LogSystemConfig::ForEngineHost(), &settings.Context(), "Sandbox")) {
        fprintf(stderr, "Sandbox: the log system could not start\n");
        return 1;
    }

    int exitCode = 0;
    {
        LogInstallScope installed(host.System());
        (void)host.System().SetThreadName("GameThread");
        (void)host.Watch(settings.Context());
        // log.mode·log.file.* 는 여기서 고정된다(실행 중 변경 거부).
        (void)settings.Context().CompleteStartup();
        exitCode = RunEngine(allocator, options.frames);
    }

    host.Stop();
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
