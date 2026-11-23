#include <AssetCommandlets/AssetAliasCommandlet.h>
#include <AssetCommandlets/AssetCommandletSupport.h>

#include <AssetDatabase/AssetAliasTable.h>
#include <AssetDatabase/AssetClock.h>
#include <AssetDatabase/AssetIndex.h>
#include <AssetDatabase/AssetIndexCache.h>
#include <AssetDatabase/LogicalPath.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Time/DateTime.h>
#include <Platform/File.h>

// 별칭 명령 구현이다(Docs/Design/AssetSystem 4.3·13절, 7.1절 17 A2e1). 별칭 표·검색은
// AssetDatabase AssetAliasTable이 맡고 여기서는 인수·잠금·출력만 다룬다. 타입을 등록하지
// 않는다(별칭 표와 색인만 쓴다).
namespace rocks::commandlet_host {

namespace {

constexpr StringView AliasListName = "asset-alias-list";
constexpr StringView AliasRemoveName = "asset-alias-remove";
constexpr StringView AliasPruneName = "asset-alias-prune";

constexpr StringView AliasListUsage =
    "usage: asset-alias-list [--project=<dir>] [--wait=<seconds>]\n"
    "  Print the path aliases of Config/AssetAliases.json in path key order, one line each:\n"
    "  '<old path> <AssetId> <created> <expires>[ expired][ shadowed] -> <current path>'.\n"
    "  The current path is 'missing' when the asset is not in the index and 'ambiguous' when\n"
    "  two packages share its id. 'shadowed' marks an alias whose old path names a package\n"
    "  again (the package wins). Listing takes no lock.\n";

constexpr StringView AliasRemoveUsage =
    "usage: asset-alias-remove <old path> [--project=<dir>] [--wait=<seconds>]\n"
    "  Take the project write lock and remove the alias of <old path> from\n"
    "  Config/AssetAliases.json. Text that still names the old path stops resolving.\n";

constexpr StringView AliasPruneUsage =
    "usage: asset-alias-prune [--project=<dir>] [--wait=<seconds>]\n"
    "  Take the project write lock and remove the expired aliases (created + days has passed)\n"
    "  that no file under Config/ still names. Expired aliases that are still named stay and\n"
    "  are reported as 'AliasInUse' warnings with the file and line; remove them with\n"
    "  asset-alias-remove after fixing the text.\n";

// "--project=" "--wait=" 옵션과 위치 인수(positionals 개)다.
struct AliasOptions {
    StringView project;
    StringView wait;
    StringView positional[1];
    usize positionals = 0;
    bool help = false;

    bool Parse(Span<const StringView> args, usize expected) noexcept {
        if (args.Size() == 1 && args[0] == "--help") {
            help = true;
            return true;
        }

        for (StringView argument : args) {
            if (argument.StartsWith("--", StringComparison::CaseSensitive)) {
                StringView* slot = nullptr;
                StringView value;
                StringView* const slots[] = {&project, &wait};
                const StringView names[] = {"--project=", "--wait="};
                for (usize index = 0; index < 2; ++index) {
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
                continue;
            }

            if (positionals >= expected) {
                return false;
            }

            positional[positionals++] = argument;
        }

        uint32 milliseconds = 0;
        return positionals == expected && ParseWaitMilliseconds(wait, milliseconds);
    }

    uint32 WaitMilliseconds() const noexcept {
        uint32 milliseconds = 0;
        (void)ParseWaitMilliseconds(wait, milliseconds);
        return milliseconds;
    }
};

// 별칭 표 칸 이름이다(CorruptAliasTable 진단 줄 — AssetAliasField 순서, 모르는 값은 "unknown").
const char* FieldName(uint32 field) noexcept {
    constexpr const char* Names[] = {"file size", "JSON syntax", "top-level keys", "version",
                                     "aliases",   "entry keys",  "path",           "asset",
                                     "created",   "days",        "duplicate path"};
    static_assert(sizeof(Names) / sizeof(Names[0]) ==
                      static_cast<usize>(AssetAliasField::DuplicatePath) + 1,
                  "every alias field needs a name");
    return field < sizeof(Names) / sizeof(Names[0]) ? Names[field] : "unknown";
}

// 별칭 표를 읽는다. 실패하면 진단 줄을 쓰고 failure에 종료 코드를 둔다.
bool LoadAliases(CommandletOutput& output, Allocator& allocator, StringView command,
                 AssetAliasTable& aliases, Result<int32>& failure) noexcept {
    AssetDiagnostic diagnostic;
    auto loaded = aliases.Load(platform::PhysicalFileSystem(), &diagnostic);
    if (loaded) {
        return true;
    }

    if (loaded.Error() == ErrorCode::OutOfMemory) {
        failure = Result<int32>::Failure(loaded.Error());
        return false;
    }

    if (diagnostic.code != AssetDiagnosticCode::CorruptAliasTable) {
        failure = Fail(output, allocator, command, 1, "{}: Config/{}: cannot read the alias table",
                       ErrorCodeName(loaded.Error()), AssetAliasFileName);
        return false;
    }

    if (diagnostic.section == static_cast<uint32>(AssetAliasField::Json)) {
        failure =
            Fail(output, allocator, command, 1,
                 "CorruptAliasTable: Config/{}: JSON syntax at line {} column {}",
                 AssetAliasFileName, diagnostic.detail >> 32, diagnostic.detail & 0xffffffffull);
    } else if (diagnostic.detail == AssetAliasNoEntry) {
        failure = Fail(output, allocator, command, 1, "CorruptAliasTable: Config/{}: invalid {}",
                       AssetAliasFileName, FieldName(diagnostic.section));
    } else {
        failure = Fail(output, allocator, command, 1,
                       "CorruptAliasTable: Config/{}: invalid {} (entry {})", AssetAliasFileName,
                       FieldName(diagnostic.section), diagnostic.detail);
    }

    return false;
}

bool LockProject(CommandletOutput& output, Allocator& allocator, StringView command,
                 const ProjectLayout& layout, uint32 milliseconds, AssetDatabaseLock& lock,
                 Result<int32>& failure) noexcept {
    auto held = lock.Acquire(layout, milliseconds);
    if (held) {
        return true;
    }

    failure = held.Error() == ErrorCode::Busy
                  ? Fail(output, allocator, command, 1,
                         "Busy: {}: the project is locked by another process; use "
                         "--wait=<seconds>",
                         layout.Root())
                  : Fail(output, allocator, command, 1, "{}: {}: cannot lock the project",
                         ErrorCodeName(held.Error()), layout.Root());
    return false;
}

// 쓰기 실패 줄이다.
Result<int32> WriteFailed(CommandletOutput& output, Allocator& allocator, StringView command,
                          ErrorCode error) noexcept {
    if (error == ErrorCode::OutOfMemory) {
        return Result<int32>::Failure(error);
    }

    return Fail(output, allocator, command, 1, "{}: Config/{}: cannot write the alias table",
                ErrorCodeName(error), AssetAliasFileName);
}

} // namespace

Result<int32> RunAssetAliasListCommand(void* context, Span<const StringView> args,
                                       CommandletOutput& output) noexcept {
    Allocator& allocator = *static_cast<Allocator*>(context);
    AliasOptions options;
    if (!options.Parse(args, 0)) {
        return Write(output, CommandletStream::Error, AliasListUsage, 2);
    }

    if (options.help) {
        return Write(output, CommandletStream::Output, AliasListUsage, 0);
    }

    ProjectLayout layout(allocator);
    Result<int32> failure = Result<int32>::Success(1);
    if (!OpenProject(output, allocator, AliasListName, options.project, layout, failure)) {
        return failure;
    }

    AssetAliasTable aliases(allocator, layout);
    if (!LoadAliases(output, allocator, AliasListName, aliases, failure)) {
        return failure;
    }

    // 지금 경로는 색인(캐시를 읽는다 — 목록과 같은 길)에서 찾는다.
    AssetIndexCache cache(allocator);
    auto loaded = cache.Load(platform::PhysicalFileSystem(), layout, IndexReadOptions());
    if (!loaded) {
        return Result<int32>::Failure(loaded.Error());
    }

    ScanScheduler scan(allocator);
    AssetIndex index(allocator, layout, scan.Get());
    if (auto rebuilt = index.Rebuild(platform::PhysicalFileSystem(), IndexReadOptions(),
                                     loaded.Value() ? &cache : nullptr);
        !rebuilt) {
        if (rebuilt.Error() == ErrorCode::OutOfMemory) {
            return Result<int32>::Failure(rebuilt.Error());
        }

        return Fail(output, allocator, AliasListName, 1,
                    "{}: {}: cannot scan the content directory", ErrorCodeName(rebuilt.Error()),
                    layout.ContentDirectory());
    }

    SystemAssetClock clock;
    const int64 now = clock.NowTicks();
    for (usize at = 0; at < aliases.Count(); ++at) {
        const AssetAlias& alias = aliases.At(at);
        // 만료 시각은 DateTime 범위 안으로 포화한다(AssetAliasExpiryTicks).
        auto expires = DateTime::TryFromTicks(AssetAliasExpiryTicks(alias));
        auto path = ParseLogicalPath(allocator, alias.path.View());
        if (!path) {
            return Result<int32>::Failure(path.Error());
        }

        auto current = index.FindById(alias.asset);
        auto shadow = index.FindByPath(path.Value());
        const bool shadowed = shadow || shadow.Error() == ErrorCode::AlreadyExists;
        StringBuilder<LineBytes> line(allocator);
        auto formatted = line.TryAppendFormat(
            "{} {} {} {}{}{} -> {}\n", alias.path.View(), alias.asset, alias.created,
            expires.Value(), AssetAliasExpiryTicks(alias) <= now ? " expired" : "",
            shadowed ? " shadowed" : "",
            current ? current.Value()->path.View()
                    : StringView(current.Error() == ErrorCode::AlreadyExists ? "ambiguous"
                                                                             : "missing"));
        if (!formatted) {
            return Result<int32>::Failure(formatted.Error());
        }

        if (auto written = Write(output, CommandletStream::Output, line.View(), 0); !written) {
            return written;
        }
    }

    return Result<int32>::Success(0);
}

Result<int32> RunAssetAliasRemoveCommand(void* context, Span<const StringView> args,
                                         CommandletOutput& output) noexcept {
    Allocator& allocator = *static_cast<Allocator*>(context);
    AliasOptions options;
    if (!options.Parse(args, 1)) {
        return Write(output, CommandletStream::Error, AliasRemoveUsage, 2);
    }

    if (options.help) {
        return Write(output, CommandletStream::Output, AliasRemoveUsage, 0);
    }

    ProjectLayout layout(allocator);
    Result<int32> failure = Result<int32>::Success(1);
    if (!OpenProject(output, allocator, AliasRemoveName, options.project, layout, failure)) {
        return failure;
    }

    AssetDatabaseLock lock;
    if (!LockProject(output, allocator, AliasRemoveName, layout, options.WaitMilliseconds(), lock,
                     failure)) {
        return failure;
    }

    AssetAliasTable aliases(allocator, layout);
    if (!LoadAliases(output, allocator, AliasRemoveName, aliases, failure)) {
        return failure;
    }

    const StringView target = options.positional[0];
    auto path = ParseLogicalPath(allocator, target);
    if (!path) {
        return path.Error() == ErrorCode::OutOfMemory
                   ? Result<int32>::Failure(path.Error())
                   : Fail(output, allocator, AliasRemoveName, 1,
                          "InvalidPath: {}: not a logical path", target);
    }

    if (aliases.Find(path.Value().Key()) == AssetAliasNone) {
        return Fail(output, allocator, AliasRemoveName, 1, "NotFound: {}: no alias", target);
    }

    const StringView keys[] = {path.Value().Key()};
    bool notDurable = false;
    if (auto written =
            aliases.Write(platform::PhysicalFileSystem(), lock, keys, {}, notDurable, nullptr);
        !written) {
        return WriteFailed(output, allocator, AliasRemoveName, written.Error());
    }

    if (notDurable) {
        if (auto warned = Fail(output, allocator, AliasRemoveName, 0,
                               "NotDurable: Config/{}: warning NotDurable", AssetAliasFileName);
            !warned) {
            return warned;
        }
    }

    StringBuilder<LineBytes> line(allocator);
    if (auto formatted = line.TryAppendFormat("{}: removed {}\n", AliasRemoveName, target);
        !formatted) {
        return Result<int32>::Failure(formatted.Error());
    }

    return Write(output, CommandletStream::Output, line.View(), 0);
}

Result<int32> RunAssetAliasPruneCommand(void* context, Span<const StringView> args,
                                        CommandletOutput& output) noexcept {
    Allocator& allocator = *static_cast<Allocator*>(context);
    AliasOptions options;
    if (!options.Parse(args, 0)) {
        return Write(output, CommandletStream::Error, AliasPruneUsage, 2);
    }

    if (options.help) {
        return Write(output, CommandletStream::Output, AliasPruneUsage, 0);
    }

    ProjectLayout layout(allocator);
    Result<int32> failure = Result<int32>::Success(1);
    if (!OpenProject(output, allocator, AliasPruneName, options.project, layout, failure)) {
        return failure;
    }

    AssetDatabaseLock lock;
    if (!LockProject(output, allocator, AliasPruneName, layout, options.WaitMilliseconds(), lock,
                     failure)) {
        return failure;
    }

    AssetAliasTable aliases(allocator, layout);
    if (!LoadAliases(output, allocator, AliasPruneName, aliases, failure)) {
        return failure;
    }

    SystemAssetClock clock;
    AssetAliasPruneReport report(allocator);
    AssetDiagnostic diagnostic;
    if (auto pruned = aliases.Prune(platform::PhysicalFileSystem(), lock, clock.NowTicks(),
                                    AssetAliasSearchLimits{}, report, &diagnostic);
        !pruned) {
        if (pruned.Error() == ErrorCode::OutOfMemory) {
            return Result<int32>::Failure(pruned.Error());
        }

        return Fail(output, allocator, AliasPruneName, 1, "{}: {}: cannot search or write ({})",
                    ErrorCodeName(pruned.Error()), layout.ConfigDirectory(), AssetAliasFileName);
    }

    for (const AssetAliasUse& use : report.uses) {
        if (auto warned = Fail(output, allocator, AliasPruneName, 0,
                               "AliasInUse: {}:{}: warning {} is still named", use.file.View(),
                               use.line, use.alias.View());
            !warned) {
            return warned;
        }
    }

    if (report.truncated) {
        if (auto warned = Fail(output, allocator, AliasPruneName, 0,
                               "AliasSearchTruncated: {}: warning the search hit its limit; "
                               "nothing was removed",
                               layout.ConfigDirectory());
            !warned) {
            return warned;
        }
    }

    if (report.notDurable) {
        if (auto warned = Fail(output, allocator, AliasPruneName, 0,
                               "NotDurable: Config/{}: warning NotDurable", AssetAliasFileName);
            !warned) {
            return warned;
        }
    }

    StringBuilder<LineBytes> summary(allocator);
    if (auto formatted =
            summary.TryAppendFormat("{}: {} removed, {} in use, {} kept\n", AliasPruneName,
                                    report.removed, report.inUse, report.kept);
        !formatted) {
        return Result<int32>::Failure(formatted.Error());
    }

    return Write(output, CommandletStream::Output, summary.View(), 0);
}

} // namespace rocks::commandlet_host
