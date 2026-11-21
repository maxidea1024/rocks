#include "AssetCommandletSupport.h"

#include <Core/Build/BuildInfo.h>
#include <Core/FileError.h>
#include <HostConfig/HostSettings.h>
#include <ModuleLoader/ModuleLoader.h>
#include <Platform/File.h>

// 에셋 명령의 공용 도구 구현이다(Docs/Design/AssetSystem.ko.md 13절).
namespace rocks::commandlet_host {

bool OpenProject(CommandletOutput& output, Allocator& allocator, StringView command,
                 StringView project, ProjectLayout& layout, Result<int32>& failure) noexcept {
    String directory(allocator);
    String name(allocator);
    if (project.IsEmpty()) {
        // 현재 디렉터리를 절대 경로로 만든 뒤(layout이 작업 디렉터리 기준으로 푼다) 위로 찾는다.
        ProjectLayout working(allocator);
        if (auto initialized = working.Initialize("."); !initialized) {
            failure =
                Fail(output, allocator, command, 1, "cannot resolve the current directory ({})",
                     ErrorCodeName(initialized.Error()));
            return false;
        }

        if (auto found = FindProjectFrom(working.Root(), directory, name); !found) {
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
        failure = Fail(output, allocator, command, 1, "'{}' is not a usable project directory ({})",
                       project, ErrorCodeName(initialized.Error()));
        return false;
    }

    // --project로 준 디렉터리에도 프로젝트 기술자가 있어야 한다(오타로 빈 색인을 내지 않게).
    String descriptorDirectory(allocator);
    auto found = FindProjectFrom(layout.Root(), descriptorDirectory, name);
    if (!found && found.Error() != ErrorCode::NotFound) {
        failure = Fail(output, allocator, command, 1, "cannot search for the project ({})",
                       ErrorCodeName(found.Error()));
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

AssetCommandTypes::AssetCommandTypes(Allocator& allocator, ModuleLoader& modules) noexcept
    : modules_(&modules), types_(allocator), registrations_(types_, allocator), names_(allocator) {
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
    if (auto loaded = modules_->Load(AssetTypeModule); !loaded.IsOk()) {
        return Result<void>::Failure(loaded.error != ErrorCode::None ? loaded.error
                                                                     : ErrorCode::Unsupported);
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
                       ErrorCodeName(started.Error()), AssetTypeModule);
        return false;
    }

    AssetServiceConfig config;
    config.packages.types = &session.types.Types();
    config.packages.files = &platform::PhysicalFileSystem();
    config.packages.hooks = &session.hooks;
    config.packages.read = IndexReadOptions();
    config.packages.registryContext = CommandletAssetContext;
    config.packages.registryOptions.names = &session.types.Names();
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
