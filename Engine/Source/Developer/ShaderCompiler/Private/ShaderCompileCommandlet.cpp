#include <ShaderCompiler/ShaderCompileCommandlet.h>

#include "ShaderCompileReflection.h"

#include <AssetNugget/NuggetFileSystemBackend.h>
#include <Base/InitializerList.h>
#include <Core/Text/NumberText.h>
#include <HostConfig/HostSettings.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <ShaderCompiler/ShaderCompileManager.h>
#include <Task/WorkerScheduler.h>
#include <stdio.h>
#include <string.h>

// shader-compile 명령이다(SC13). 규칙은 공개 헤더 주석을 따른다.
namespace rocks::shader {

namespace compile_command_detail {

constexpr char CommandName[] = "shader-compile";
constexpr char Usage[] =
    "usage: shader-compile [--project=<dir>] --format=<id> [--format=<id>]... "
    "[--define=<NAME>=<VALUE>]... [--option=<name>=<value>]... [--out=<dir>] [--workers=<n>] "
    "[--no-cache] [--keep-input] <virtual path>:<entry>:<vertex|pixel|compute>...\n";
// 경로 버퍼다.
constexpr usize MaxCommandPathBytes = 4096;
// 한 줄 출력 버퍼다(진단은 따로 나눠 쓴다).
constexpr usize MaxCommandLineBytes = 1024;
// 받는 형식 수 상한이다(형식 정의 파일 수보다 넉넉하다).
constexpr usize MaxCommandFormats = 16;
// 종료 코드다.
constexpr int32 ExitSucceeded = 0;
constexpr int32 ExitFailed = 1;
constexpr int32 ExitUsage = 2;
// 나눠 쓰지 않을 만큼 짧은 줄의 상한이다(--out 파일 이름).
constexpr usize MaxOutputNameBytes = 512;

Result<int32> Write(CommandletOutput& output, CommandletStream stream, StringView text,
                    int32 code) noexcept {
    auto written = output.write(output.context, stream, text);
    return written ? Result<int32>::Success(code) : Result<int32>::Failure(written.Error());
}

// printf 형식 한 줄을 쓴다(잘리면 잘린 대로).
template <typename... Args>
Result<void> Line(CommandletOutput& output, CommandletStream stream, const char* format,
                  Args... args) noexcept {
    char text[MaxCommandLineBytes];
    const int length = snprintf(text, sizeof(text), format, args...);
    if (length <= 0) {
        return Result<void>::Success();
    }

    const usize bytes =
        static_cast<usize>(length) < sizeof(text) ? static_cast<usize>(length) : sizeof(text) - 1;
    return output.write(output.context, stream, StringView(text, bytes));
}

template <typename... Args>
Result<int32> Fail(CommandletOutput& output, int32 code, const char* format,
                   Args... args) noexcept {
    if (auto written = Line(output, CommandletStream::Error, format, args...); !written) {
        return Result<int32>::Failure(written.Error());
    }

    return Result<int32>::Success(code);
}

// job 하나의 명령줄 표기다.
struct JobSpec {
    StringView path;
    StringView entry;
    StringView stageName;
    uint32 stage = 0;
};

struct Arguments {
    explicit Arguments(Allocator& allocator) noexcept
        : formats(allocator), defines(allocator), options(allocator), jobs(allocator) {
    }

    StringView project;
    StringView out;
    StringView workers;
    bool noCache = false;
    bool keepInput = false;
    bool help = false;
    Array<StringView> formats;
    Array<ShaderDefine> defines;
    Array<StringView> options;
    Array<JobSpec> jobs;
};

bool ParseStage(StringView name, uint32& out) noexcept {
    const struct {
        const char* name;
        uint32 stage;
    } stages[] = {{"vertex", ShaderStageVertex},
                  {"pixel", ShaderStagePixel},
                  {"compute", ShaderStageCompute}};
    for (const auto& entry : stages) {
        if (name == StringView(entry.name)) {
            out = entry.stage;
            return true;
        }
    }

    return false;
}

// 사용법 오류면 false와 그 이유다.
bool Parse(Span<const StringView> args, Arguments& out, const char*& reason) noexcept {
    for (const StringView argument : args) {
        if (argument == "--help") {
            out.help = true;
            continue;
        }

        if (argument == "--no-cache" || argument == "--keep-input") {
            (argument == "--no-cache" ? out.noCache : out.keepInput) = true;
            continue;
        }

        if (argument.StartsWith("--", StringComparison::CaseSensitive)) {
            StringView name;
            StringView value;
            if (!argument.Split("=", name, value, StringComparison::CaseSensitive) ||
                value.IsEmpty()) {
                reason = "an option needs a value (--name=value)";
                return false;
            }

            StringView* const single[] = {&out.project, &out.out, &out.workers};
            const char* const singleNames[] = {"--project", "--out", "--workers"};
            bool matched = false;
            for (usize index = 0; index < sizeof(single) / sizeof(single[0]); ++index) {
                if (name == StringView(singleNames[index])) {
                    if (!single[index]->IsEmpty()) {
                        reason = "an option is given twice";
                        return false;
                    }

                    *single[index] = value;
                    matched = true;
                }
            }

            if (matched) {
                continue;
            }

            if (name == "--format") {
                if (out.formats.Size() >= MaxCommandFormats || !out.formats.TryPushBack(value)) {
                    reason = "too many --format options";
                    return false;
                }

                continue;
            }

            if (name == "--define") {
                ShaderDefine define;
                if (!value.Split("=", define.name, define.value, StringComparison::CaseSensitive) ||
                    define.name.IsEmpty() || !out.defines.TryPushBack(define)) {
                    reason = "--define needs <NAME>=<VALUE>";
                    return false;
                }

                continue;
            }

            if (name == "--option") {
                if (!out.options.TryPushBack(value)) {
                    reason = "out of memory";
                    return false;
                }

                continue;
            }

            reason = "unknown option";
            return false;
        }

        JobSpec job;
        StringView rest;
        if (!argument.Split(":", job.path, rest, StringComparison::CaseSensitive) ||
            !rest.Split(":", job.entry, job.stageName, StringComparison::CaseSensitive) ||
            job.path.IsEmpty() || job.entry.IsEmpty() || !ParseStage(job.stageName, job.stage)) {
            reason = "a job is <virtual path>:<entry>:<vertex|pixel|compute>";
            return false;
        }

        if (!out.jobs.TryPushBack(job)) {
            reason = "out of memory";
            return false;
        }
    }

    if (!out.help && out.jobs.IsEmpty()) {
        reason = "no shader job";
        return false;
    }

    if (!out.help && out.formats.IsEmpty()) {
        reason = "no shader format (give --format=<id>)";
        return false;
    }

    return true;
}

// 출력 파일 확장자다(backend·컴파일 경로별).
const char* CodeExtension(const ShaderFormatDefinition& format) noexcept {
    switch (format.compiler.path) {
    case ShaderCompilerPath::DxcDxil:
        return "dxil";
    case ShaderCompilerPath::Fxc:
    case ShaderCompilerPath::DxcSpirvHlsl50Fxc:
        return "dxbc";
    case ShaderCompilerPath::DxcSpirv:
        return "spv";
    case ShaderCompilerPath::DxcSpirvMsl:
        return "metallib";
    case ShaderCompilerPath::DxcSpirvEssl:
        return "essl";
    case ShaderCompilerPath::None:
        break;
    }

    return "bin";
}

bool WriteFile(const char* path, Span<const uint8> bytes) noexcept {
    platform::File file;
    if (!file.Open(path, platform::FileOpenMode::CreateAlways)) {
        return false;
    }

    const platform::FileTransfer written = file.WriteAll(bytes.Data(), bytes.Size());
    return static_cast<bool>(file.Close()) && static_cast<bool>(written.status);
}

// 명령 전용 레지스트리 문맥 id다("SHCM").
constexpr uint32 ShaderCompileCommandContextId = 0x5348434d;

class Command final {
public:
    Command(const ShaderCompileCommandContext& context, CommandletOutput& output) noexcept
        : context_(context), allocator_(*context.allocator), output_(output),
          arguments_(allocator_), snapshots_(allocator_), root_(allocator_), workRoot_(allocator_) {
    }

    ~Command() noexcept {
        for (ShaderFormatSnapshot* snapshot : snapshots_) {
            snapshot->~ShaderFormatSnapshot();
            (void)allocator_.Deallocate(snapshot);
        }
    }

    Command(const Command&) = delete;
    Command& operator=(const Command&) = delete;

    Result<int32> Run(Span<const StringView> args) noexcept {
        const char* reason = "";
        if (!Parse(args, arguments_, reason)) {
            if (auto written =
                    Line(output_, CommandletStream::Error, "%s: %s\n", CommandName, reason);
                !written) {
                return Result<int32>::Failure(written.Error());
            }

            return Write(output_, CommandletStream::Error, Usage, ExitUsage);
        }

        if (arguments_.help) {
            return Write(output_, CommandletStream::Output, Usage, ExitSucceeded);
        }

        uint32 workers = 0;
        if (!arguments_.workers.IsEmpty()) {
            auto parsed = TryParseUInt32(arguments_.workers);
            if (!parsed || parsed.Value() == 0) {
                return Fail(output_, ExitUsage, "%s: --workers needs a positive number\n",
                            CommandName);
            }

            workers = parsed.Value();
        }

        if (auto located = Locate(); !located || located.Value() != ExitSucceeded) {
            return located;
        }

        return Compile(workers);
    }

private:
    // 엔진 폴더·프로필·형식·워커·너겟 root를 정한다.
    Result<int32> Locate() noexcept {
        if (!context_.engineDirectory.IsEmpty()) {
            const int written = snprintf(engine_, sizeof(engine_), "%.*s",
                                         static_cast<int>(context_.engineDirectory.ByteCount()),
                                         context_.engineDirectory.Data());
            if (written <= 0 || static_cast<usize>(written) >= sizeof(engine_)) {
                return Fail(output_, ExitFailed, "%s: the engine path is too long\n", CommandName);
            }
        } else if (!FindEngineDirectory(Span<char>(engine_, sizeof(engine_)))) {
            return Fail(output_, ExitFailed, "%s: cannot find the engine folder\n", CommandName);
        }

        if (!context_.workerExecutable.IsEmpty()) {
            snprintf(worker_, sizeof(worker_), "%.*s",
                     static_cast<int>(context_.workerExecutable.ByteCount()),
                     context_.workerExecutable.Data());
        } else if (!compile_support::FindWorkerNextToExecutable(worker_)) {
            return Fail(output_, ExitFailed,
                        "%s: ShaderCompileWorker is not next to this executable\n", CommandName);
        }

        snprintf(formats_, sizeof(formats_), "%s/Config/ShaderFormats", engine_);
        // 형식 snapshot이다. TargetProfile shaderFormats(12.3.1절 SSD10)가 생기기 전까지 --format이
        // 형식 목록의 유일한 출처다(SSD24).
        const Span<const StringView> ids = arguments_.formats.View();
        for (const StringView id : ids) {
            auto memory =
                allocator_.Allocate(sizeof(ShaderFormatSnapshot), alignof(ShaderFormatSnapshot));
            if (!memory) {
                return Result<int32>::Failure(ErrorCode::OutOfMemory);
            }

            auto* snapshot = ::new (memory.Value()) ShaderFormatSnapshot(allocator_);
            if (!snapshots_.TryPushBack(snapshot)) {
                snapshot->~ShaderFormatSnapshot();
                (void)allocator_.Deallocate(snapshot);
                return Result<int32>::Failure(ErrorCode::OutOfMemory);
            }

            const ShaderFormatStatus status = LoadShaderFormatSnapshot(formats_, id, *snapshot);
            if (!status) {
                char detail[MaxCommandLineBytes];
                snprintf(detail, sizeof(detail), "%.*s: %s %s", static_cast<int>(id.ByteCount()),
                         id.Data(), ShaderFormatErrorName(status.code), status.path);
                return Fail(output_, ExitFailed, "shader-compile: cannot load the format %s\n",
                            detail);
            }
        }

        if (auto resolved = ResolveShaderNuggetRoot(arguments_.project, root_); !resolved) {
            return Fail(output_, ExitFailed, "%s: cannot resolve the nugget root\n", CommandName);
        }

        if (!workRoot_.TryAssign(root_.View()) || !workRoot_.TryAppend("/") ||
            !workRoot_.TryAppend(ShaderWorkerDirectoryName)) {
            return Result<int32>::Failure(ErrorCode::OutOfMemory);
        }

        return Result<int32>::Success(ExitSucceeded);
    }

    Result<int32> Mount(ShaderSourceTree& tree) noexcept {
        char shaders[MaxCommandPathBytes];
        snprintf(shaders, sizeof(shaders), "%s/Shaders", engine_);
        if (const ShaderSourceStatus mounted = tree.Mount("/Engine", StringView(shaders));
            !mounted) {
            return Fail(output_, ExitFailed, "shader-compile: cannot mount /Engine: %s\n",
                        mounted.message);
        }

        if (arguments_.project.IsEmpty()) {
            return Result<int32>::Success(ExitSucceeded);
        }

        const int bytes =
            snprintf(shaders, sizeof(shaders), "%.*s/Shaders",
                     static_cast<int>(arguments_.project.ByteCount()), arguments_.project.Data());
        bool exists = false;
        if (bytes > 0 &&
            platform::PhysicalFileSystem().DirectoryExists(shaders, static_cast<usize>(bytes),
                                                           exists) &&
            exists) {
            if (const ShaderSourceStatus mounted = tree.Mount("/Project", StringView(shaders));
                !mounted) {
                return Fail(output_, ExitFailed, "shader-compile: cannot mount /Project: %s\n",
                            mounted.message);
            }
        }

        return Result<int32>::Success(ExitSucceeded);
    }

    Result<int32> Compile(uint32 workers) noexcept {
        ShaderSourceTree tree(allocator_);
        if (auto mounted = Mount(tree); !mounted || mounted.Value() != ExitSucceeded) {
            return mounted;
        }

        compile_support::KeyReflection reflection(allocator_, ShaderCompileCommandContextId);
        if (!reflection.registered) {
            return Fail(output_, ExitFailed, "%s: cannot register the reflection types\n",
                        CommandName);
        }

        NuggetSystemClock clock;
        NuggetCache cache(allocator_, platform::PhysicalFileSystem(), clock);
        WorkerScheduler scheduler(allocator_);
        ShaderCompileManagerConfig config;
        if (!arguments_.noCache) {
            NuggetSettingsFailure failure;
            if (!OpenShaderNuggetCache(cache, allocator_, platform::PhysicalFileSystem(),
                                       root_.View(), &failure)) {
                return Fail(output_, ExitFailed, "%s: cannot open the nugget cache\n", CommandName);
            }

            if (!scheduler.Start()) {
                return Fail(output_, ExitFailed, "%s: cannot start the worker scheduler\n",
                            CommandName);
            }

            config.cache = &cache;
            config.scheduler = &scheduler;
        }

        config.allocator = &allocator_;
        config.tree = &tree;
        config.reflection = reflection.Reflection();
        config.workerExecutable = StringView(worker_);
        config.workRoot = workRoot_.View();
        config.workerCount = workers;
        config.keepInput = arguments_.keepInput;
        Result<int32> result = Result<int32>::Success(ExitSucceeded);
        {
            ShaderCompileManager manager(config);
            if (auto started = manager.Start(); !started) {
                result =
                    Fail(output_, ExitFailed, "%s: cannot start the shader workers\n", CommandName);
            } else {
                result = CompileJobs(manager);
            }

            manager.Shutdown();
        }

        cache.Shutdown(0);
        (void)scheduler.Shutdown();
        return result;
    }

    Result<int32> CompileJobs(ShaderCompileManager& manager) noexcept {
        // job은 형식 × 명령줄 job 순서다.
        Array<ShaderCompileJob> jobs(allocator_);
        for (const ShaderFormatSnapshot* snapshot : snapshots_) {
            ShaderCompileOptions options = DefaultShaderCompileOptions(snapshot->definition);
            if (const ShaderOptionStatus parsed =
                    ParseShaderCompileOptions(arguments_.options.View(), options);
                !parsed) {
                char detail[MaxCommandLineBytes];
                snprintf(detail, sizeof(detail), "%s %s", ShaderOptionErrorName(parsed.code),
                         parsed.name);
                return Fail(output_, ExitUsage, "shader-compile: invalid --option: %s\n", detail);
            }

            for (const JobSpec& spec : arguments_.jobs) {
                ShaderCompileJob job;
                job.format = snapshot;
                job.entryPath = spec.path;
                job.entryPoint = spec.entry;
                job.stage = spec.stage;
                job.defines = arguments_.defines.View();
                job.options = options;
                if (!jobs.TryPushBack(job)) {
                    return Result<int32>::Failure(ErrorCode::OutOfMemory);
                }
            }
        }

        Array<ShaderCompileJobResult> results(allocator_);
        if (auto compiled = manager.Compile(jobs.View(), results); !compiled) {
            return Result<int32>::Failure(compiled.Error());
        }

        bool failed = false;
        for (usize index = 0; index < jobs.Size(); ++index) {
            auto reported = Report(index / arguments_.jobs.Size(),
                                   arguments_.jobs[index % arguments_.jobs.Size()], results[index]);
            if (!reported) {
                return Result<int32>::Failure(reported.Error());
            }

            failed = failed || results[index].status != ShaderCompileStatus::Success ||
                     !reported.Value();
        }

        // 요약은 관리자 계수를 읽는다(진행 보고 12절 — 진행 줄은 Progress 모듈 몫).
        const ShaderCompileProgress progress = manager.Progress();
        if (auto written =
                Line(output_, CommandletStream::Output,
                     "%s: jobs %llu completed %llu failed %llu cache-hits %llu workers %llu "
                     "batches %llu\n",
                     CommandName, static_cast<unsigned long long>(progress.total),
                     static_cast<unsigned long long>(progress.completed),
                     static_cast<unsigned long long>(progress.failed),
                     static_cast<unsigned long long>(progress.cacheHits),
                     static_cast<unsigned long long>(progress.workersStarted),
                     static_cast<unsigned long long>(progress.batches));
            !written) {
            return Result<int32>::Failure(written.Error());
        }

        return Result<int32>::Success(failed ? ExitFailed : ExitSucceeded);
    }

    // job 하나를 알리고 --out이면 코드를 쓴다. 반환: 파일 쓰기까지 성공했는가.
    Result<bool> Report(usize formatIndex, const JobSpec& spec,
                        const ShaderCompileJobResult& result) noexcept {
        const ShaderFormatDefinition& format = snapshots_[formatIndex]->definition;
        char key[Sha256Digest::HexBytes + 1] = "-";
        if (result.hasKey) {
            result.key.ToHex(key);
        }

        char file[MaxCommandPathBytes] = {};
        bool stored = true;
        if (result.status == ShaderCompileStatus::Success && !arguments_.out.IsEmpty()) {
            stored = Store(format, spec, result, file);
        }

        const int pathBytes = static_cast<int>(spec.path.ByteCount());
        const int entryBytes = static_cast<int>(spec.entry.ByteCount());
        const int stageBytes = static_cast<int>(spec.stageName.ByteCount());
        if (auto written = Line(
                output_, CommandletStream::Output, "%s: %s %.*s:%.*s:%.*s %s %s %s%s%s\n",
                CommandName, format.id, pathBytes, spec.path.Data(), entryBytes, spec.entry.Data(),
                stageBytes, spec.stageName.Data(), key, result.cacheHit ? "hit" : "miss",
                ShaderCompileStatusName(result.status), file[0] != 0 ? " " : "", file);
            !written) {
            return Result<bool>::Failure(written.Error());
        }

        if (!result.diagnostics.IsEmpty()) {
            if (auto written = Line(output_, CommandletStream::Error, "%s: %s %.*s:%.*s:%.*s\n",
                                    CommandName, format.id, pathBytes, spec.path.Data(), entryBytes,
                                    spec.entry.Data(), stageBytes, spec.stageName.Data());
                !written) {
                return Result<bool>::Failure(written.Error());
            }

            if (auto written = output_.write(output_.context, CommandletStream::Error,
                                             result.diagnostics.View());
                !written) {
                return Result<bool>::Failure(written.Error());
            }

            if (!result.diagnostics.View().EndsWith("\n", StringComparison::CaseSensitive)) {
                (void)output_.write(output_.context, CommandletStream::Error, "\n");
            }
        }

        if (!stored) {
            if (auto written = Line(output_, CommandletStream::Error,
                                    "%s: cannot write the output file '%s'\n", CommandName, file);
                !written) {
                return Result<bool>::Failure(written.Error());
            }
        }

        return Result<bool>::Success(stored);
    }

    // <out>/<format>/<name>.<entry>.<stage>.<ext>에 코드를 쓴다.
    bool Store(const ShaderFormatDefinition& format, const JobSpec& spec,
               const ShaderCompileJobResult& result, char (&file)[MaxCommandPathBytes]) noexcept {
        char name[MaxOutputNameBytes];
        usize nameBytes = 0;
        for (usize index = 0; index < spec.path.ByteCount(); ++index) {
            if (nameBytes + 1 >= sizeof(name)) {
                return false;
            }

            const char character = spec.path.Data()[index];
            name[nameBytes++] = character == '/' ? '+' : character;
        }

        name[nameBytes] = 0;
        const int directoryBytes =
            snprintf(file, sizeof(file), "%.*s/%s", static_cast<int>(arguments_.out.ByteCount()),
                     arguments_.out.Data(), format.id);
        if (directoryBytes <= 0 || static_cast<usize>(directoryBytes) >= sizeof(file) ||
            !platform::MakeDirectoryTree(platform::PhysicalFileSystem(), file,
                                         static_cast<usize>(directoryBytes))) {
            return false;
        }

        const int bytes =
            snprintf(file + directoryBytes, sizeof(file) - directoryBytes, "/%s.%.*s.%.*s.%s", name,
                     static_cast<int>(spec.entry.ByteCount()), spec.entry.Data(),
                     static_cast<int>(spec.stageName.ByteCount()), spec.stageName.Data(),
                     CodeExtension(format));
        if (bytes <= 0 || static_cast<usize>(directoryBytes + bytes) >= sizeof(file) ||
            !result.values.Has(ShaderNuggetValue::Code)) {
            return false;
        }

        return WriteFile(file, result.values.Part(ShaderNuggetValue::Code).View());
    }

    const ShaderCompileCommandContext& context_;
    Allocator& allocator_;
    CommandletOutput& output_;
    Arguments arguments_;
    char engine_[MaxCommandPathBytes] = {};
    char worker_[MaxCommandPathBytes] = {};
    char formats_[MaxCommandPathBytes] = {};
    Array<ShaderFormatSnapshot*> snapshots_;
    String root_;
    String workRoot_;
};

} // namespace compile_command_detail

Result<int32> RunShaderCompileCommand(void* context, Span<const StringView> args,
                                      CommandletOutput& output) noexcept {
    const auto* typed = static_cast<const ShaderCompileCommandContext*>(context);
    if (typed == nullptr || typed->allocator == nullptr) {
        return Result<int32>::Failure(ErrorCode::InvalidArgument);
    }

    compile_command_detail::Command command(*typed, output);
    return command.Run(args);
}

} // namespace rocks::shader
