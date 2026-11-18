#include <HostConfig/HostSettings.h>

#include <Config/Config.module.generated.h>
#include <Config/ConfigLoader.h>
#include <Core/Diagnostics/Log.h>
#include <Core/FileError.h>
#include <Core/Text/StringBuilder.h>
#include <LogHost/LogHost.h>
#include <Base/Compiler.h>
#include <Platform/Fatal.h>
#include <Platform/File.h>
#include <Platform/Path.h>
#include <Platform/Process.h>
#include <Platform/VirtualMemory.h>
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

// 경로의 구분자를 '/'로 바꿔 out에 쓴다.
Result<void> AssignForwardSlashes(StringView path, String& out) noexcept {
    if (auto assigned = out.TryAssign(path); !assigned) {
        return assigned;
    }

    auto replaced = out.TryReplace("\\", "/", StringComparison::CaseSensitive);
    return replaced ? Result<void>::Success() : Result<void>::Failure(replaced.Error());
}

// 디렉터리에서 이름 byte 순서로 첫 `*.rocksproject` 파일 이름(확장자 제외)을 찾는다.
struct ProjectScan {
    explicit ProjectScan(Allocator& allocator) noexcept : name(allocator) {
    }

    String name;
    bool found = false;
    bool failed = false;
};

bool VisitProjectCandidate(const platform::DirectoryEntry& entry, void* raw) noexcept {
    constexpr StringView Extension = ".rocksproject";
    auto& scan = *static_cast<ProjectScan*>(raw);
    const StringView name(entry.name, entry.nameBytes);
    if (entry.info.type != platform::FileType::File || name.ByteCount() <= Extension.ByteCount() ||
        !name.EndsWith(Extension, StringComparison::CaseSensitive)) {
        return true;
    }

    const StringView stem = name.TryLeftBytes(name.ByteCount() - Extension.ByteCount()).Value();
    bool earlier = !scan.found;
    if (!earlier) {
        const StringView current = scan.name.View();
        const usize count =
            stem.ByteCount() < current.ByteCount() ? stem.ByteCount() : current.ByteCount();
        usize index = 0;
        while (index < count && stem.Data()[index] == current.Data()[index]) {
            ++index;
        }

        earlier = index < count ? static_cast<uint8>(stem.Data()[index]) <
                                      static_cast<uint8>(current.Data()[index])
                                : stem.ByteCount() < current.ByteCount();
    }

    if (earlier) {
        scan.failed = scan.failed || !scan.name.TryAssign(stem);
        scan.found = true;
    }

    return true;
}

} // namespace

StringView HostPlatformName() noexcept {
    if constexpr (IsWindows) {
        return "Windows";
    } else if constexpr (IsMac) {
        return "Mac";
    } else {
        return "Android";
    }
}

Result<void> CollectPlatformDeviceTraits(StringView platform,
                                         config::DeviceTraits& traits) noexcept {
    if (auto named = traits.SetText(config::DeviceTraitNames::Platform, platform); !named) {
        return named;
    }

    uint64 bytes = 0;
    if (!platform::QueryPhysicalMemory(bytes)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    return traits.SetNumber(config::DeviceTraitNames::SystemMemoryMiB,
                            static_cast<double>(bytes / (1024 * 1024)));
}

Result<void> FindProjectFrom(StringView start, String& directory, String& name) noexcept {
    Allocator& allocator = directory.GetAllocator();
    String current(allocator);
    if (auto assigned = AssignForwardSlashes(start, current); !assigned) {
        return assigned;
    }

    platform::FileSystem& files = platform::PhysicalFileSystem();
    while (!current.IsEmpty()) {
        ProjectScan scan(allocator);
        const platform::FileStatus listed = files.EnumerateDirectory(
            current.Data(), current.ByteCount(), &VisitProjectCandidate, &scan);
        if (!listed && listed.code != platform::FileCode::NotFound) {
            return Result<void>::Failure(FileErrorCode(listed));
        }

        if (scan.failed) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        if (scan.found) {
            if (auto assigned = directory.TryAssign(current.View()); !assigned) {
                return assigned;
            }

            return name.TryAssign(scan.name.View());
        }

        // 부모 폴더로 올라간다(마지막 구분자 앞까지 — 루트에서 멈춘다).
        StringView parent;
        StringView leaf;
        if (!current.View().Split("/", parent, leaf, StringComparison::CaseSensitive,
                                  StringSearchDirection::FromEnd) ||
            parent.IsEmpty() || parent.EndsWith(":", StringComparison::CaseSensitive)) {
            break;
        }

        String next(allocator);
        if (auto assigned = next.TryAssign(parent); !assigned) {
            return assigned;
        }

        current = Move(next);
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> FindProjectDirectory(String& directory, String& name) noexcept {
    char executable[4096];
    usize length = 0;
    if (const platform::FileStatus found =
            platform::GetExecutablePath(executable, sizeof(executable), length);
        !found) {
        return Result<void>::Failure(FileErrorCode(found));
    }

    // 실행 파일이 있는 폴더부터 찾는다.
    StringView folder;
    StringView file;
    String path(directory.GetAllocator());
    if (auto assigned = AssignForwardSlashes(StringView(executable, length), path); !assigned) {
        return assigned;
    }

    if (!path.View().Split("/", folder, file, StringComparison::CaseSensitive,
                           StringSearchDirection::FromEnd)) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    return FindProjectFrom(folder, directory, name);
}

Result<void> UserDataDirectory(String& out) noexcept {
    char buffer[4096];
    usize bytes = 0;
    if (const platform::FileStatus found = platform::GetStandardDirectory(
            platform::StandardDirectory::UserData, buffer, sizeof(buffer), bytes);
        !found) {
        return Result<void>::Failure(FileErrorCode(found));
    }

    return AssignForwardSlashes(StringView(buffer, bytes), out);
}

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
      objects_(allocator, types_, HostSettingsContext, MakeOptions(names_)), traits_(allocator),
      profile_(allocator), profileArgument_(allocator) {
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
        step = generated::Config::RegisterModuleTypes(modules_);
    }

    if (step) {
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
        contextOptions.platform =
            options.platform.IsEmpty() ? HostPlatformName() : options.platform;
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

    if (step) {
        step = config::RegisterDeviceProfileSettings(*context_);
    }

    if (step && options.registerSettings != nullptr) {
        step = options.registerSettings(*context_);
    }

    // 품질 조합 검증은 기기 특성(아래에서 채운다)으로 묶음의 펼칠 등급을 낮춘다(CVar 11절).
    if (step) {
        qualityRules_ = options.qualityRules;
        qualityBudgets_ = options.qualityBudgets;
        step = context_->SetQualityValidation(qualityRules_, qualityBudgets_, &traits_);
    }

    if (step) {
        stage = "device traits";
        // 메모리를 조회할 수 없는 플랫폼은 메모리 등급 없이 돈다(기록만 한다).
        const StringView platform =
            options.platform.IsEmpty() ? HostPlatformName() : options.platform;
        if (auto collected = CollectPlatformDeviceTraits(platform, traits_); !collected) {
            if (collected.Error() != ErrorCode::Unsupported) {
                step = Result<void>::Failure(collected.Error());
            } else {
                Log::Warning(LogHostConfig,
                             "System memory could not be queried; device profiles see no memory "
                             "bucket");
            }
        }
    }

    if (!step) {
        Log::Error(LogHostConfig, "Config setup failed at {Stage}: {Error}", StringView(stage),
                   ErrorCodeName(step.Error()));
        context_ = nullptr;
    }

    return step;
}

config::DeviceTraits& HostSettings::DeviceTraits() noexcept {
    if (context_.Get() == nullptr) {
        platform::Fatal("HostSettings::DeviceTraits called before a successful Initialize");
    }

    return traits_;
}

Result<void> HostSettings::ApplyDeviceTraits() noexcept {
    config::ConfigContext& context = Context();
    auto applied = config::ApplyDeviceProfile(context, profileArgument_.View(), traits_, profile_);
    if (!applied) {
        Log::Error(LogHostConfig, "Device profile could not be applied: {Error}",
                   ErrorCodeName(applied.Error()));
        return applied;
    }

    // 특성이 바뀌었으므로 다음 Sync가 품질 묶음의 펼칠 등급을 다시 정한다.
    if (auto validated = context.SetQualityValidation(qualityRules_, qualityBudgets_, &traits_);
        !validated) {
        Log::Error(LogHostConfig, "Quality validation could not be updated: {Error}",
                   ErrorCodeName(validated.Error()));
        return validated;
    }

    return applied;
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

    if (auto copied = profileArgument_.TryAssign(config::FindDeviceProfileArgument(arguments));
        !copied) {
        return copied;
    }

    auto added = config::AddCommandLineAssignments(Context(), arguments);
    if (!added) {
        ReportRejected("--set", added.Error());
        return Result<void>::Failure(added.Error());
    }

    return Result<void>::Success();
}

Result<void> HostSettings::SyncAndReport(StringView what) noexcept {
    config::ConfigContext& context = Context();
    auto synced = context.Sync();
    if (!synced) {
        ReportRejected(what, synced.Error());
        return synced;
    }

    // Sync가 거부한 입력(이전 층에 따라 달라진 배열 연산 등)·프로필과 품질 정의 오류는 값에서
    // 빠지고 진단만 남는다. 시작은 계속하되 남은 진단을 기록한다.
    bool rejected = false;
    for (usize index = 0; index < context.Diagnostics().Count(); ++index) {
        rejected =
            rejected || context.Diagnostics().Get(index).severity == config::ConfigSeverity::Error;
    }

    if (rejected) {
        ReportRejected(what, ErrorCode::InvalidArgument);
    } else {
        ReportWarnings();
    }

    return Result<void>::Success();
}

Result<void> HostSettings::CompleteStartup() noexcept {
    config::ConfigContext& context = Context();
    if (auto synced = SyncAndReport("inputs at startup"); !synced) {
        return synced;
    }

    // 기기 프로필은 파일·--set 값(프로젝트 지정·정의)을 본다.
    if (auto applied =
            config::ApplyDeviceProfile(context, profileArgument_.View(), traits_, profile_);
        !applied) {
        Log::Error(LogHostConfig, "Device profile could not be applied: {Error}",
                   ErrorCodeName(applied.Error()));
        return applied;
    }

    if (auto synced = SyncAndReport("device profile"); !synced) {
        return synced;
    }

    auto completed = context.CompleteStartup();
    if (!completed) {
        Log::Error(LogHostConfig, "Config startup could not complete: {Error}",
                   ErrorCodeName(completed.Error()));
        return completed;
    }

    // 시작이 끝났는데 아무도 등록하지 않은 이름의 값은 오타일 수 있다(CVar 12절 — 경고만, 시작은
    // 계속한다). 보고 자체가 할당 실패로 끝나도 기록만 하고 시작을 이어 간다(봉인은 끝났다).
    if (auto unresolved = context.ReportUnresolved(); !unresolved) {
        Log::Error(LogHostConfig, "Unresolved config names could not be reported: {Error}",
                   ErrorCodeName(unresolved.Error()));
    }

    ReportWarnings();
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
