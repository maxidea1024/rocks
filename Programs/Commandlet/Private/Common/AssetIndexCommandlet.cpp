#include "AssetIndexCommandlet.h"

#include <AssetDatabase/AssetIndex.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetDatabase/ProjectLayout.h>
#include <Core/Build/BuildInfo.h>
#include <Core/FileError.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/StringBuilder.h>
#include <HostConfig/HostSettings.h>
#include <Object/AssetId.h>
#include <Platform/File.h>

// asset-list·asset-index-verify 명령 구현이다(Docs/Design/AssetSystem.ko.md 13절, 7.1절 13 — A2b).
// 색인·진단은 AssetDatabase AssetIndex가 맡고, 여기서는 인수·프로젝트 찾기·출력만 다룬다.
namespace rocks::commandlet_host {

namespace {

constexpr StringView ListName = "asset-list";
constexpr StringView VerifyName = "asset-index-verify";

constexpr StringView ListUsage =
    "usage: asset-list [--under=<path>] [--project=<dir>]\n"
    "  List the editor packages under Content/ in logical path order, one line each:\n"
    "  '<logical path> <AssetId> <representative type id>'. --under=<path> keeps the\n"
    "  packages below a logical directory (/Game or /Game/...). --project=<dir> picks the\n"
    "  project; without it the nearest '*.rocksproject' from the current directory up is used.\n";

constexpr StringView VerifyUsage =
    "usage: asset-index-verify [--project=<dir>]\n"
    "  Scan Content/ and check the packages: invalid names, package checks, duplicate asset\n"
    "  ids and paths, missing dependencies and type mismatches. Each problem is one line\n"
    "  '<diagnostic code>: <path>: <description>' on standard error; the exit code is 1 when\n"
    "  there is any.\n";

// 출력 한 줄의 인라인 byte 수다(넘으면 allocator로 자란다).
constexpr usize LineBytes = 512;

Result<int32> Write(CommandletOutput& output, CommandletStream stream, StringView text,
                    int32 code) noexcept {
    auto written = output.write(output.context, stream, text);
    return written ? Result<int32>::Success(code) : Result<int32>::Failure(written.Error());
}

// "<command>: <서식>\n"을 표준 오류에 쓰고 code로 돌려준다.
template <class... TArgs>
Result<int32> Fail(CommandletOutput& output, Allocator& allocator, StringView command, int32 code,
                   StringView format, const TArgs&... args) noexcept {
    StringBuilder<LineBytes> text(allocator);
    if (auto formatted = text.TryAppendFormat("{}: ", command); !formatted) {
        return Result<int32>::Failure(formatted.Error());
    }

    if (auto formatted = text.TryAppendFormat(format, args...); !formatted) {
        return Result<int32>::Failure(formatted.Error());
    }

    if (auto formatted = text.TryAppend("\n"); !formatted) {
        return Result<int32>::Failure(formatted.Error());
    }

    return Write(output, CommandletStream::Error, text.View(), code);
}

// "--이름=값" 옵션이다. 모르는 이름·같은 이름 두 번·값 없음·위치 인수는 사용법 오류다.
struct Options {
    StringView project;
    StringView under;
    bool help = false;

    // allowUnder는 --under를 받는지다(asset-list).
    bool Parse(Span<const StringView> args, bool allowUnder) noexcept {
        if (args.Size() == 1 && args[0] == "--help") {
            help = true;
            return true;
        }

        for (StringView argument : args) {
            StringView* slot = nullptr;
            StringView value;
            const StringView names[] = {"--project=", "--under="};
            for (StringView name : names) {
                if (argument.StartsWith(name, StringComparison::CaseSensitive)) {
                    slot = name == "--project=" ? &project : (allowUnder ? &under : nullptr);
                    value = StringView(argument.Data() + name.ByteCount(),
                                       argument.ByteCount() - name.ByteCount());
                }
            }

            if (slot == nullptr || !slot->IsEmpty() || value.IsEmpty()) {
                return false;
            }

            *slot = value;
        }

        return true;
    }
};

// 옵션의 프로젝트(없으면 현재 디렉터리에서 위로 찾은 것)로 layout을 연다. 실패하면 오류를 출력하고
// 그 종료 코드를 failure에 둔다.
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

} // namespace

Result<int32> RunAssetListCommand(void* context, Span<const StringView> args,
                                  CommandletOutput& output) noexcept {
    Allocator& allocator = *static_cast<Allocator*>(context);
    Options options;
    if (!options.Parse(args, true)) {
        return Write(output, CommandletStream::Error, ListUsage, 2);
    }

    if (options.help) {
        return Write(output, CommandletStream::Output, ListUsage, 0);
    }

    ProjectLayout layout(allocator);
    Result<int32> failure = Result<int32>::Success(1);
    if (!OpenProject(output, allocator, ListName, options.project, layout, failure)) {
        return failure;
    }

    AssetIndex index(allocator, layout);
    if (auto rebuilt = index.Rebuild(platform::PhysicalFileSystem(), IndexReadOptions());
        !rebuilt) {
        if (rebuilt.Error() == ErrorCode::OutOfMemory) {
            return Result<int32>::Failure(rebuilt.Error());
        }

        return Fail(output, allocator, ListName, 1, "cannot scan '{}' ({})",
                    layout.ContentDirectory(), ErrorCodeName(rebuilt.Error()));
    }

    Array<const AssetIndexEntry*> entries(allocator);
    if (auto listed =
            index.ListUnder(options.under.IsEmpty() ? GameMount : options.under, true, entries);
        !listed) {
        if (listed.Error() == ErrorCode::OutOfMemory) {
            return Result<int32>::Failure(listed.Error());
        }

        return Fail(output, allocator, ListName, 1, "InvalidPath: '{}' is not a logical directory",
                    options.under);
    }

    for (const AssetIndexEntry* entry : entries) {
        StringBuilder<LineBytes> line(allocator);
        if (auto formatted =
                line.TryAppendFormat("{} {} {}\n", entry->path.View(), entry->id, entry->type);
            !formatted) {
            return Result<int32>::Failure(formatted.Error());
        }

        if (auto written = Write(output, CommandletStream::Output, line.View(), 0); !written) {
            return written;
        }
    }

    return Result<int32>::Success(0);
}

Result<int32> RunAssetIndexVerifyCommand(void* context, Span<const StringView> args,
                                         CommandletOutput& output) noexcept {
    Allocator& allocator = *static_cast<Allocator*>(context);
    Options options;
    if (!options.Parse(args, false)) {
        return Write(output, CommandletStream::Error, VerifyUsage, 2);
    }

    if (options.help) {
        return Write(output, CommandletStream::Output, VerifyUsage, 0);
    }

    ProjectLayout layout(allocator);
    Result<int32> failure = Result<int32>::Success(1);
    if (!OpenProject(output, allocator, VerifyName, options.project, layout, failure)) {
        return failure;
    }

    AssetIndex index(allocator, layout);
    if (auto rebuilt = index.Rebuild(platform::PhysicalFileSystem(), IndexReadOptions());
        !rebuilt) {
        if (rebuilt.Error() == ErrorCode::OutOfMemory) {
            return Result<int32>::Failure(rebuilt.Error());
        }

        return Fail(output, allocator, VerifyName, 1, "cannot scan '{}' ({})",
                    layout.ContentDirectory(), ErrorCodeName(rebuilt.Error()));
    }

    // 타입 Registry 없이 대조한다(명령은 타입을 등록하지 않는다 — 파생 판정은 정확 일치).
    Array<AssetIndexDiagnostic> verified(allocator);
    if (auto checked = index.Verify(nullptr, verified); !checked) {
        return Result<int32>::Failure(checked.Error());
    }

    const Span<const AssetIndexDiagnostic> groups[] = {index.Diagnostics(), verified.View()};
    for (Span<const AssetIndexDiagnostic> group : groups) {
        for (const AssetIndexDiagnostic& diagnostic : group) {
            StringBuilder<LineBytes> line(allocator);
            if (auto formatted = AppendAssetIndexDiagnosticLine(line, diagnostic); !formatted) {
                return Result<int32>::Failure(formatted.Error());
            }

            if (auto written = Write(output, CommandletStream::Error, line.View(), 1); !written) {
                return written;
            }
        }
    }

    const usize problems = index.Diagnostics().Size() + verified.Size();
    StringBuilder<LineBytes> summary(allocator);
    if (auto formatted = summary.TryAppendFormat("{}: {} packages, {} diagnostics\n", VerifyName,
                                                 index.Count(), problems);
        !formatted) {
        return Result<int32>::Failure(formatted.Error());
    }

    return Write(output, CommandletStream::Output, summary.View(), problems == 0 ? 0 : 1);
}

} // namespace rocks::commandlet_host
