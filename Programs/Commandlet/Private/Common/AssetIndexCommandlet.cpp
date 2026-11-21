#include "AssetIndexCommandlet.h"
#include "AssetCommandletSupport.h"

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
    "usage: asset-list [--type=<Type>] [--under=<path>] [--project=<dir>] [--wait=<seconds>]\n"
    "  List the editor packages under Content/ in logical path order, one line each:\n"
    "  '<logical path> <AssetId> <representative type id>'. --type=<Type> keeps the packages\n"
    "  whose representative asset is that class or derives from it (a type tool name).\n"
    "  --under=<path> keeps the\n"
    "  packages below a logical directory (/Game or /Game/...). --project=<dir> picks the\n"
    "  project; without it the nearest '*.rocksproject' from the current directory up is used.\n"
    "  --wait is accepted like the other asset commands; listing takes no lock.\n";

constexpr StringView VerifyUsage =
    "usage: asset-index-verify [--project=<dir>] [--wait=<seconds>]\n"
    "  Scan Content/ and check the packages: invalid names, package checks, duplicate asset\n"
    "  ids and paths, missing dependencies and type mismatches. Each problem is one line\n"
    "  '<diagnostic code>: <path>: <description>' on standard error; the exit code is 1 when\n"
    "  there is any. --wait is accepted like the other asset commands; checking takes no lock.\n";

// "--이름=값" 옵션이다. 모르는 이름·같은 이름 두 번·값 없음·위치 인수는 사용법 오류다.
struct Options {
    StringView project;
    StringView under;
    StringView type;
    // 13절 공통 옵션 --wait=<초>(쓰기 잠금 대기)다. 읽기 명령은 잠금을 잡지 않으므로 형식(10진
    // 숫자)만 검사하고 쓰지 않는다.
    StringView wait;
    bool help = false;

    // allowUnder는 --under·--type을 받는지다(asset-list).
    bool Parse(Span<const StringView> args, bool allowUnder) noexcept {
        if (args.Size() == 1 && args[0] == "--help") {
            help = true;
            return true;
        }

        for (StringView argument : args) {
            StringView* slot = nullptr;
            StringView value;
            StringView* const slots[] = {&project, allowUnder ? &under : nullptr, &wait,
                                         allowUnder ? &type : nullptr};
            const StringView names[] = {"--project=", "--under=", "--wait=", "--type="};
            for (usize index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
                if (argument.StartsWith(names[index], StringComparison::CaseSensitive)) {
                    slot = slots[index];
                    value = StringView(argument.Data() + names[index].ByteCount(),
                                       argument.ByteCount() - names[index].ByteCount());
                }
            }

            if (slot == nullptr || !slot->IsEmpty() || value.IsEmpty()) {
                return false;
            }

            *slot = value;
        }

        for (char digit : wait) {
            if (digit < '0' || digit > '9') {
                return false;
            }
        }

        return true;
    }
};

// 항목을 한 줄씩 쓴다(`<논리 경로> <AssetId> <대표 타입 id>`).
Result<int32> WriteEntries(CommandletOutput& output, Allocator& allocator,
                           const Array<const AssetIndexEntry*>& entries) noexcept {
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

// --type이 있는 목록이다: 에셋 타입을 등록하고 서비스(AssetService::List — 파생 포함)로 거른다.
Result<int32> ListByType(CommandletOutput& output, const AssetCommandContext& command,
                         const Options& options) noexcept {
    Allocator& allocator = *command.allocator;
    EditSession session(allocator, *command.modules);
    Result<int32> failure = Result<int32>::Success(1);
    if (!OpenSession(output, allocator, ListName, options.project, options.wait, false, session,
                     failure)) {
        return failure;
    }

    Array<const AssetIndexEntry*> entries(allocator);
    AssetDiagnostic diagnostic;
    if (auto listed = session.service->List(options.type, options.under, entries, &diagnostic);
        !listed) {
        if (listed.Error() == ErrorCode::OutOfMemory) {
            return Result<int32>::Failure(listed.Error());
        }

        if (diagnostic.code == AssetDiagnosticCode::InvalidPath) {
            return Fail(output, allocator, ListName, 1,
                        "InvalidPath: '{}' is not a logical directory", options.under);
        }

        return Fail(
            output, allocator, ListName, 1, "{}: {}: {}",
            diagnostic.code == AssetDiagnosticCode::None ? ErrorCodeName(listed.Error())
                                                         : AssetDiagnosticName(diagnostic.code),
            options.type, listed.Error() == ErrorCode::NotFound ? "unknown type" : "not a class");
    }

    return WriteEntries(output, allocator, entries);
}

} // namespace

Result<int32> RunAssetListCommand(void* context, Span<const StringView> args,
                                  CommandletOutput& output) noexcept {
    const AssetCommandContext& command = *static_cast<const AssetCommandContext*>(context);
    Allocator& allocator = *command.allocator;
    Options options;
    if (!options.Parse(args, true)) {
        return Write(output, CommandletStream::Error, ListUsage, 2);
    }

    if (options.help) {
        return Write(output, CommandletStream::Output, ListUsage, 0);
    }

    if (!options.type.IsEmpty()) {
        return ListByType(output, command, options);
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

    return WriteEntries(output, allocator, entries);
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
