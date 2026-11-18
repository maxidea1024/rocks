#include <HostConfig/HostSettings.h>

#include <Config/ConfigLoader.h>
#include <Core/Diagnostics/Log.h>
#include <Core/FileError.h>
#include <Core/Text/StringBuilder.h>
#include <LogHost/LogHost.h>
#include <Platform/Fatal.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <TraceControl/TraceController.h>

namespace rocks {

namespace {

// 호스트 설정 부트의 로그 카테고리다.
inline constexpr LogCategory LogHostConfig{"Config"};

// 호스트 설정 문맥의 객체 Registry 문맥 id다("HCFG").
constexpr ContextId HostSettingsContext = {0x48434647, 1};

bool IsPathSeparator(char c) noexcept {
    return c == '/' || c == '\\';
}

} // namespace

Result<usize> FindEngineDirectory(Span<char> out) noexcept {
    char executable[4096];
    usize length = 0;
    if (const platform::FileStatus found =
            platform::GetExecutablePath(executable, sizeof(executable), length);
        !found) {
        return Result<usize>::Failure(FileErrorCode(found));
    }

    constexpr StringView ConfigSuffix = "/Config";
    platform::FileSystem& files = platform::PhysicalFileSystem();
    while (length != 0) {
        // 마지막 경로 구분자 앞까지가 부모 폴더다.
        usize cut = length;
        while (cut != 0 && !IsPathSeparator(executable[cut - 1])) {
            --cut;
        }

        if (cut == 0) {
            break;
        }

        length = cut - 1;
        FixedStringBuilder candidate{out};
        if (!candidate.TryAppendFormat("{}/Engine{}", StringView(executable, length),
                                       ConfigSuffix)) {
            return Result<usize>::Failure(ErrorCode::CapacityExceeded);
        }

        platform::FileInfo info;
        if (files.Stat(out.Data(), candidate.ByteCount(), info) &&
            info.type == platform::FileType::Directory) {
            const usize bytes = candidate.ByteCount() - ConfigSuffix.ByteCount();
            out[bytes] = '\0';
            return Result<usize>::Success(bytes);
        }
    }

    return Result<usize>::Failure(ErrorCode::NotFound);
}

HostSettings::HostSettings(Allocator& allocator) noexcept
    : allocator_(allocator), types_(allocator), modules_(types_, allocator), names_(allocator),
      objects_(allocator, types_, HostSettingsContext, MakeOptions(names_)) {
}

HostSettings::~HostSettings() noexcept {
    reloader_ = nullptr;
    context_ = nullptr;
    (void)objects_.Shutdown(objects_.OwnerAccess());
    (void)modules_.Unregister();
    if (namesReady_) {
        (void)names_.Shutdown();
    }

    (void)types_.Shutdown();
}

RegistryOptions HostSettings::MakeOptions(NamePool& names) noexcept {
    RegistryOptions options;
    options.names = &names;
    return options;
}

config::ConfigContext& HostSettings::Context() noexcept {
    if (context_.Get() == nullptr) {
        platform::Fatal("HostSettings::Context called before a successful Initialize");
    }

    return *context_;
}

Result<void> HostSettings::Initialize(const HostSettingsOptions& options) noexcept {
    Result<void> step = names_.Initialize();
    namesReady_ = step.IsOk();
    const char* stage = "name pool";
    if (step) {
        stage = "module types";
        step = trace_control::RegisterTraceControlTypes(modules_);
    }

    if (step && options.registerModuleTypes != nullptr) {
        step = options.registerModuleTypes(modules_);
    }

    if (step) {
        stage = "config context";
        config::ConfigContextOptions contextOptions;
        contextOptions.mode = options.mode;
        contextOptions.engineDir = options.engineDir;
        contextOptions.projectDir = options.projectDir;
        contextOptions.projectName = options.projectName;
        contextOptions.userDataDir = options.userDataDir;
        contextOptions.savedDir = options.savedDir;
        auto made = UniquePtr<config::ConfigContext>::TryMake(allocator_, allocator_, objects_,
                                                              contextOptions);
        step = made ? Result<void>::Success() : Result<void>::Failure(made.Error());
        if (made) {
            context_ = Move(made.Value());
            step = context_->Initialize();
        }
    }

    if (step) {
        stage = "settings registration";
        step = RegisterLogSettings(*context_);
    }

    if (step) {
        step = trace_control::RegisterTraceSettings(*context_);
    }

    if (step && options.registerSettings != nullptr) {
        step = options.registerSettings(*context_);
    }

    if (!step) {
        Log::Error(LogHostConfig, "Config setup failed at {Stage}: {Error}", StringView(stage),
                   ErrorCodeName(step.Error()));
        context_ = nullptr;
    }

    return step;
}

void HostSettings::ReportRejected(StringView what, ErrorCode error) noexcept {
    config::ConfigDiagnostics& all = context_->Diagnostics();
    Log::Error(LogHostConfig, "Config {What} rejected: {Error} ({Count} diagnostics)", what,
               ErrorCodeName(error), all.Count());
    for (usize index = 0; index < all.Count(); ++index) {
        const config::ConfigDiagnostic item = all.Get(index);
        if (item.severity == config::ConfigSeverity::Error) {
            Log::Error(LogHostConfig, "  {Source}:{Line}:{Column} {Code} at '{Path}': {Message}",
                       item.source, item.position.line, item.position.column,
                       config::ConfigDiagnosticCodeName(item.code), item.path, item.message);
        } else if (item.severity == config::ConfigSeverity::Warning) {
            Log::Warning(LogHostConfig, "  {Source}:{Line}:{Column} {Code} at '{Path}': {Message}",
                         item.source, item.position.line, item.position.column,
                         config::ConfigDiagnosticCodeName(item.code), item.path, item.message);
        }
    }

    all.Clear();
}

void HostSettings::ReportWarnings() noexcept {
    config::ConfigDiagnostics& all = context_->Diagnostics();
    for (usize index = 0; index < all.Count(); ++index) {
        const config::ConfigDiagnostic item = all.Get(index);
        if (item.severity != config::ConfigSeverity::Warning) {
            continue;
        }

        Log::Warning(LogHostConfig, "Config {Source}:{Line}:{Column} {Code} at '{Path}': {Message}",
                     item.source, item.position.line, item.position.column,
                     config::ConfigDiagnosticCodeName(item.code), item.path, item.message);
    }

    all.Clear();
}

Result<void> HostSettings::LoadFiles() noexcept {
    auto loaded = config::LoadConfigFiles(Context(), platform::PhysicalFileSystem());
    if (!loaded) {
        ReportRejected("files", loaded.Error());
    } else {
        // 사용자 파일에서 버린 키(오래된 파일)는 실패가 아니지만 남긴다.
        ReportWarnings();
    }

    return loaded;
}

Result<void> HostSettings::AddFile(config::ConfigLayer layer, StringView branch,
                                   const char* path) noexcept {
    config::ConfigContext& context = Context();
    platform::File file;
    uint64 size = 0;
    Array<char> text(allocator_);
    platform::FileStatus status = file.Open(path, platform::FileOpenMode::ReadExisting);
    if (status) {
        status = file.Size(size);
    }

    if (status && !text.TryResize(static_cast<usize>(size))) {
        Log::Error(LogHostConfig, "Config file '{Path}' does not fit in memory", StringView(path));
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    if (status) {
        status = file.ReadExact(text.Data(), text.Size()).status;
    }

    if (!status) {
        Log::Error(LogHostConfig,
                   "Cannot read config file '{Path}' (file code {FileCode}, OS error {OsError})",
                   StringView(path), static_cast<uint32>(status.code), status.nativeError);
        return Result<void>::Failure(FileErrorCode(status));
    }

    auto added = context.AddLayerText(layer, "HostFile", branch, StringView(path),
                                      StringView(text.Data(), text.Size()));
    if (!added) {
        ReportRejected(StringView(path), added.Error());
    }

    return added;
}

Result<void> HostSettings::ApplyCommandLine(Span<const StringView> arguments) noexcept {
    // 모양이 틀린 인자(대상 뒤 `=` 없음)는 문맥 진단이 없으므로 여기서 인자를 적어 남긴다.
    for (const StringView argument : arguments) {
        if (!argument.StartsWith(config::SetArgumentPrefix, StringComparison::CaseSensitive)) {
            continue;
        }

        const StringView assignment =
            argument.TryRightBytes(argument.ByteCount() - config::SetArgumentPrefix.ByteCount())
                .Value();
        if (assignment.Find("=", StringComparison::CaseSensitive) == StringView::NotFound ||
            assignment.StartsWith("=", StringComparison::CaseSensitive)) {
            Log::Error(LogHostConfig,
                       "Malformed argument '{Argument}'; expected --set=<target>=<value>",
                       argument);
        }
    }

    auto added = config::AddCommandLineAssignments(Context(), arguments);
    if (!added) {
        ReportRejected("--set", added.Error());
        return Result<void>::Failure(added.Error());
    }

    return Result<void>::Success();
}

Result<void> HostSettings::CompleteStartup() noexcept {
    config::ConfigContext& context = Context();
    auto synced = context.Sync();
    if (!synced) {
        ReportRejected("startup sync", synced.Error());
        return synced;
    }

    // Sync가 거부한 입력(이전 층에 따라 달라진 배열 연산 등)은 값에서 빠지고 진단만 남는다. 시작은
    // 계속하되 남은 Error 진단을 기록한다.
    bool rejected = false;
    for (usize index = 0; index < context.Diagnostics().Count(); ++index) {
        rejected =
            rejected || context.Diagnostics().Get(index).severity == config::ConfigSeverity::Error;
    }

    if (rejected) {
        ReportRejected("inputs at startup", ErrorCode::InvalidArgument);
    }

    auto completed = context.CompleteStartup();
    if (!completed) {
        Log::Error(LogHostConfig, "Config startup could not complete: {Error}",
                   ErrorCodeName(completed.Error()));
    }

    return completed;
}

Result<void> HostSettings::StartHotReload() noexcept {
    config::ConfigContext& context = Context();
    auto made = UniquePtr<config::ConfigFileReloader>::TryMake(allocator_, context,
                                                               platform::PhysicalFileSystem());
    if (!made) {
        Log::Error(LogHostConfig, "Config hot reload could not start: {Error}",
                   ErrorCodeName(made.Error()));
        return Result<void>::Failure(made.Error());
    }

    auto started = made.Value()->Start(true);
    if (!started) {
        if (started.Error() != ErrorCode::Unsupported) {
            Log::Error(LogHostConfig, "Config hot reload could not start: {Error}",
                       ErrorCodeName(started.Error()));
        }

        return started;
    }

    reloader_ = Move(made.Value());
    Log::Info(LogHostConfig, "Config hot reload is watching the config folders");
    return Result<void>::Success();
}

} // namespace rocks
