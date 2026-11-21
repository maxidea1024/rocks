#include "AssetCommandletSupport.h"

#include <Core/Build/BuildInfo.h>
#include <Core/FileError.h>
#include <HostConfig/HostSettings.h>

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

} // namespace rocks::commandlet_host
