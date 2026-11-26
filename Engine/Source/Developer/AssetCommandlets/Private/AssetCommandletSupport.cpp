#include <AssetCommandlets/AssetCommandletSupport.h>

#include <Core/Build/BuildInfo.h>
#include <Core/FileError.h>
#include <Core/Text/Path.h>
#include <HostConfig/HostSettings.h>
#include <ModuleLoader/ModuleLoader.h>
#include <Platform/File.h>
#include <Platform/Thread.h>

// 에셋 명령의 공용 도구 구현이다(Docs/Design/AssetSystem 13절).
namespace rocks::commandlet_host {
namespace {

// 할당 실패는 진단 줄로 바꾸지 않고 함수 오류로 돌려준다(호스트가 쓴다 — 다른 명령과 같다,
// 14.1.119 할당 실패 전수가 찾음). 참이면 failure를 채웠다.
bool FailOnOutOfMemory(ErrorCode error, Result<int32>& failure) noexcept {
    if (error != ErrorCode::OutOfMemory) {
        return false;
    }

    failure = Result<int32>::Failure(error);
    return true;
}

} // namespace

bool OpenProject(CommandletOutput& output, Allocator& allocator, StringView command,
                 StringView project, ProjectLayout& layout, Result<int32>& failure) noexcept {
    String directory(allocator);
    String name(allocator);
    if (project.IsEmpty()) {
        // 현재 디렉터리를 절대 경로로 만든 뒤(layout이 작업 디렉터리 기준으로 푼다) 위로 찾는다.
        ProjectLayout working(allocator);
        if (auto initialized = working.Initialize("."); !initialized) {
            if (!FailOnOutOfMemory(initialized.Error(), failure)) {
                failure =
                    Fail(output, allocator, command, 1, "cannot resolve the current directory ({})",
                         ErrorCodeName(initialized.Error()));
            }

            return false;
        }

        if (auto found = FindProjectFrom(working.Root(), directory, name); !found) {
            if (FailOnOutOfMemory(found.Error(), failure)) {
                return false;
            }

            failure =
                found.Error() == ErrorCode::NotFound
                    ? Fail(output, allocator, command, 1,
                           "no project found: no '*.rocksproject' in '{}' or above; use "
                           "--project=<dir>",
                           working.Root())
                    : Fail(output, allocator, command, 1, "cannot search for the project ({})",
                           ErrorCodeName(found.Error()));
            return false;
        }

        project = directory.View();
    }

    if (auto initialized = layout.Initialize(project); !initialized) {
        if (!FailOnOutOfMemory(initialized.Error(), failure)) {
            failure =
                Fail(output, allocator, command, 1, "'{}' is not a usable project directory ({})",
                     project, ErrorCodeName(initialized.Error()));
        }

        return false;
    }

    // --project로 준 디렉터리에도 프로젝트 기술자가 있어야 한다(오타로 빈 색인을 내지 않게).
    String descriptorDirectory(allocator);
    auto found = FindProjectFrom(layout.Root(), descriptorDirectory, name);
    if (!found && found.Error() != ErrorCode::NotFound) {
        if (!FailOnOutOfMemory(found.Error(), failure)) {
            failure = Fail(output, allocator, command, 1, "cannot search for the project ({})",
                           ErrorCodeName(found.Error()));
        }

        return false;
    }

    if (!found || descriptorDirectory.View() != layout.Root()) {
        failure = Fail(output, allocator, command, 1, "no '*.rocksproject' in '{}'", layout.Root());
        return false;
    }

    return true;
}

PackageReadOptions IndexReadOptions() noexcept {
    PackageReadOptions options;
    options.expectedKind = PackageKind::Editor;
    // 이 엔진보다 새 엔진이 저장한 패키지는 NewerVersion이다(UE AssetRegistry도 읽지 않는다).
    options.engineVersion = build::CurrentEngineVersion();
    return options;
}

ScanScheduler::ScanScheduler(Allocator& allocator) noexcept : scheduler_(allocator) {
    const uint32 processors = platform::LogicalProcessorCount();
    const uint32 helpers = processors > 1 ? processors - 1 : 0;
    WorkerSchedulerConfig config;
    config.workerCount = helpers < CommandletScanWorkers ? helpers : CommandletScanWorkers;
    started_ = config.workerCount != 0 && static_cast<bool>(scheduler_.Start(config));
}

ScanScheduler::~ScanScheduler() noexcept {
    if (started_) {
        (void)scheduler_.Shutdown();
    }
}

AssetCommandTypes::AssetCommandTypes(Allocator& allocator, ModuleLoader& modules,
                                     StringView module) noexcept
    : modules_(&modules), module_(module.IsEmpty() ? AssetTypeModules[0] : module),
      defaultModules_(module.IsEmpty()), types_(allocator), registrations_(types_, allocator),
      names_(allocator) {
}

AssetCommandTypes::~AssetCommandTypes() noexcept {
    if (attached_) {
        modules_->DetachTypeRegistrar(registrations_);
    }

    if (namesReady_) {
        (void)names_.Shutdown();
    }

    (void)types_.Shutdown();
}

Result<void> AssetCommandTypes::Start() noexcept {
    // 기본은 타입 모듈 목록 전부다(실패하면 그 모듈 이름을 남긴다).
    const Span<const StringView> list = defaultModules_ ? Span<const StringView>(AssetTypeModules)
                                                        : Span<const StringView>(&module_, 1);
    for (StringView name : list) {
        if (auto loaded = modules_->Load(name); !loaded.IsOk()) {
            module_ = name;
            return Result<void>::Failure(loaded.error != ErrorCode::None ? loaded.error
                                                                         : ErrorCode::Unsupported);
        }
    }

    if (auto attached = modules_->AttachTypeRegistrar(registrations_); !attached.IsOk()) {
        return Result<void>::Failure(attached.error != ErrorCode::None ? attached.error
                                                                       : ErrorCode::Unsupported);
    }

    attached_ = true;
    if (auto ready = names_.Initialize(); !ready) {
        return ready;
    }

    namesReady_ = true;
    return Result<void>::Success();
}

StringView AssetTextDiagnosticName(reflection_text::TextDiagnosticCode code) noexcept {
    return code == reflection_text::TextDiagnosticCode::HiddenEnumerator
               ? StringView(AssetDiagnosticName(AssetDiagnosticCode::InvalidSetting))
               : reflection_text::TextDiagnosticCodeName(code);
}

Result<String> AbsoluteCommandPath(Allocator& allocator, StringView path,
                                   bool* workingFailed) noexcept {
    if (workingFailed != nullptr) {
        *workingFailed = false;
    }

    auto kind = Path::Root(path);
    if (!kind || kind.Value() != PathRoot::Relative) {
        String copy(allocator);
        if (auto assigned = copy.TryAssign(path); !assigned) {
            return Result<String>::Failure(assigned.Error());
        }

        return Result<String>::Success(rocks::Move(copy));
    }

    Array<char> working(allocator);
    if (auto sized = working.TryResize(MaxNativePathBytes() + 1); !sized) {
        return Result<String>::Failure(sized.Error());
    }

    usize bytes = 0;
    if (auto got = platform::GetStandardDirectory(platform::StandardDirectory::Working,
                                                  working.Data(), working.Size(), bytes);
        !got) {
        if (workingFailed != nullptr) {
            *workingFailed = true;
        }

        return Result<String>::Failure(FileErrorCode(got));
    }

    return Path::MakeAbsolute(allocator, StringView(working.Data(), bytes), path);
}

bool ParseWaitMilliseconds(StringView text, uint32& out) noexcept {
    uint32 seconds = 0;
    for (char digit : text) {
        if (digit < '0' || digit > '9') {
            return false;
        }

        seconds = seconds * 10 + static_cast<uint32>(digit - '0');
        if (seconds > MaxWaitSeconds) {
            return false;
        }
    }

    out = seconds * MillisecondsPerSecond;
    return true;
}

uint32 RemainingWaitMilliseconds(uint32 waitMilliseconds, uint64 startNanoseconds,
                                 uint64 nowNanoseconds) noexcept {
    constexpr uint64 NanosecondsPerMillisecond = 1000000;
    const uint64 elapsed = nowNanoseconds > startNanoseconds
                               ? (nowNanoseconds - startNanoseconds) / NanosecondsPerMillisecond
                               : 0;
    return elapsed >= waitMilliseconds ? 0u : static_cast<uint32>(waitMilliseconds - elapsed);
}

bool OpenSession(CommandletOutput& output, Allocator& allocator, StringView command,
                 StringView project, StringView wait, bool write, EditSession& session,
                 Result<int32>& failure) noexcept {
    if (!OpenProject(output, allocator, command, project, session.layout, failure)) {
        return false;
    }

    // 쓰기 명령은 색인을 만들기 전에 잠근다(7·8절 — 쓰기 판정은 잠금 아래에서 본 디스크로 한다).
    if (write) {
        uint32 milliseconds = 0;
        (void)ParseWaitMilliseconds(wait, milliseconds);
        if (auto held = session.lock.Acquire(session.layout, milliseconds); !held) {
            failure = held.Error() == ErrorCode::Busy
                          ? Fail(output, allocator, command, 1,
                                 "Busy: {}: the project is locked by another process; use "
                                 "--wait=<seconds>",
                                 session.layout.Root())
                          : Fail(output, allocator, command, 1, "{}: {}: cannot lock the project",
                                 ErrorCodeName(held.Error()), session.layout.Root());
            return false;
        }
    }

    if (auto started = session.types.Start(); !started) {
        failure =
            started.Error() == ErrorCode::OutOfMemory
                ? Result<int32>::Failure(started.Error())
                : Fail(output, allocator, command, 1, "{}: {}: cannot register the asset types",
                       ErrorCodeName(started.Error()), session.types.Module());
        return false;
    }

    const AssetTypeHookSetup& hookSetup = session.context->typeHooks;
    if (hookSetup.registerHooks != nullptr) {
        if (auto registered =
                hookSetup.registerHooks(hookSetup.state, session.hooks, session.hookTokens);
            !registered) {
            failure = registered.Error() == ErrorCode::OutOfMemory
                          ? Result<int32>::Failure(registered.Error())
                          : Fail(output, allocator, command, 1,
                                 "{}: cannot register the asset type hooks",
                                 ErrorCodeName(registered.Error()));
            return false;
        }
    }

    const AssetImporterSetup& setup = session.context->importers;
    if (setup.registerImporters != nullptr) {
        if (auto registered =
                setup.registerImporters(setup.state, session.importers, session.importerTokens);
            !registered) {
            failure = registered.Error() == ErrorCode::OutOfMemory
                          ? Result<int32>::Failure(registered.Error())
                          : Fail(output, allocator, command, 1, "{}: cannot register the importers",
                                 ErrorCodeName(registered.Error()));
            return false;
        }
    }

    AssetServiceConfig config;
    config.importers = &session.importers;
    config.packages.types = &session.types.Types();
    config.packages.files = &platform::PhysicalFileSystem();
    config.packages.hooks = &session.hooks;
    config.packages.read = IndexReadOptions();
    config.packages.registryContext = CommandletAssetContext;
    config.packages.registryOptions.names = &session.types.Names();
    config.cacheFiles = &platform::PhysicalFileSystem();
    config.scheduler = session.scan.Get();
    auto made = UniquePtr<AssetService>::TryMake(allocator, allocator, session.layout, config);
    if (!made) {
        failure = Result<int32>::Failure(made.Error());
        return false;
    }

    session.service = Move(made.Value());
    if (auto initialized = session.service->Initialize(); !initialized) {
        failure =
            initialized.Error() == ErrorCode::OutOfMemory
                ? Result<int32>::Failure(initialized.Error())
                : Fail(output, allocator, command, 1, "{}: {}: cannot scan the content directory",
                       ErrorCodeName(initialized.Error()), session.layout.ContentDirectory());
        return false;
    }

    return true;
}

} // namespace rocks::commandlet_host
