#include <AssetCommandlets/AssetEditCommandlet.h>
#include <AssetCommandlets/AssetCommandletSupport.h>

#include <Core/Containers/Array.h>
#include <ReflectionText/TextAssignments.h>
#include <Core/FileError.h>
#include <Core/Text/Json.h>
#include <Core/Text/Path.h>
#include <Platform/File.h>

// asset-new·asset-set·asset-info 명령 구현이다(Docs/Design/AssetSystem 13절, 7.1절 15
// A2c3b). 대입 해석·저장·출력은 AssetService가 맡고 여기서는 인수·타입 등록·잠금·출력만 다룬다.
namespace rocks::commandlet_host {

namespace {

using reflection_text::AssignmentKind;
using reflection_text::TextAssignment;
using reflection_text::TextDiagnostics;

constexpr StringView NewName = "asset-new";
constexpr StringView SetName = "asset-set";
constexpr StringView InfoName = "asset-info";
constexpr StringView MoveName = "asset-move";
constexpr StringView DeleteName = "asset-delete";
constexpr StringView OrphansDeleteName = "asset-orphans-delete";
constexpr StringView DuplicateName = "asset-duplicate";
constexpr StringView FixIdName = "asset-fix-duplicate-id";
constexpr StringView ImportName = "asset-import";
constexpr StringView ReimportName = "asset-reimport";

constexpr StringView NewUsage =
    "usage: asset-new <Type> <path> [<property>=<value>...] [--unset=<paths>] [--erase=<paths>]\n"
    "                 [--project=<dir>] [--wait=<seconds>]\n"
    "  Create the editor package <path> (a logical path such as /Game/Characters/Hero) with a\n"
    "  representative asset of <Type> (a type tool name), apply the assignments and save it.\n"
    "  Prints '<logical path> <AssetId>'. Values use the ReflectionText notation; references\n"
    "  are logical paths or AssetIds ('none' is null). Takes the project write lock; --wait\n"
    "  waits up to that many seconds (0-86400) for it.\n";

constexpr StringView SetUsage =
    "usage: asset-set <path|AssetId> <property>=<value>... [--unset=<paths>] [--erase=<paths>]\n"
    "                 [--reset=<paths>] [--force] [--project=<dir>] [--wait=<seconds>]\n"
    "  Check every assignment, apply them together and save the package. Errors are all\n"
    "  reported and nothing changes. --unset resets Optional values, --erase removes map\n"
    "  entries, set elements or array elements, --reset restores class default values and\n"
    "  drops those overrides (comma separated path lists). --force saves unresolved imports\n"
    "  as null. Takes the project write lock.\n";

constexpr StringView InfoUsage =
    "usage: asset-info <path|AssetId> [--json] [--all | --import] [--project=<dir>]\n"
    "                  [--wait=<seconds>]\n"
    "  Print the representative asset as '<path>=<value>' lines that asset-set accepts as\n"
    "  they are, or as JSON with --json. --all also prints the fields that editing skips.\n"
    "  --import prints the import state instead: 'import.importer=<name> <version> <state>'\n"
    "  and 'import.source.<role>=<root>:<relative> <state>' for an imported asset, or\n"
    "  'import.owner=<path> <id>' for another output of an import. The state is 'current' or\n"
    "  SourceChanged, MissingSource, InvalidSource, ImporterOutdated, NewerVersion,\n"
    "  UnsupportedSourceFormat; a source line ends with ' (<error>)' when the state has a cause.\n"
    "  The package is not opened. Takes no lock.\n";

constexpr StringView MoveUsage =
    "usage: asset-move <path|directory> <new path> [--project=<dir>] [--wait=<seconds>]\n"
    "  Move or rename an editor package, a package not saved yet, or a logical directory with\n"
    "  everything under it (one file system move). Asset ids and references stay the same; each\n"
    "  moved package leaves an alias from its old path in Config/AssetAliases.json (not for a\n"
    "  rename that only changes case). The new path must be free: no package, file, directory\n"
    "  or alias of another asset. Prints '<count> packages moved, <count> aliases added'. Takes\n"
    "  the project write lock.\n";

constexpr StringView DeleteUsage =
    "usage: asset-delete <path|AssetId> [--force] [--project=<dir>] [--wait=<seconds>]\n"
    "  Delete an editor package, or close a package not saved yet. The target is its current\n"
    "  path or id (aliases are not followed). Packages that reference it on disk are listed and\n"
    "  block the delete unless --force (they keep a missing dependency). A package open in this\n"
    "  process that still references it blocks the delete even with --force. Aliases to the\n"
    "  deleted asset are removed. Prints 'deleted <path> <AssetId>'. Takes the project write\n"
    "  lock.\n";

constexpr StringView OrphansDeleteUsage =
    "usage: asset-orphans-delete <path> [--project=<dir>] [--wait=<seconds>]\n"
    "  Delete an orphaned import output (see asset-orphans-list). Anything else is refused.\n"
    "  Packages that reference it block the delete (use asset-delete --force). Prints\n"
    "  'deleted <path> <AssetId>'. Takes the project write lock.\n";

constexpr StringView ImportUsage =
    "usage: asset-import <path> <source> [--importer=<name>] [--<setting>=<value>...]\n"
    "       asset-import <path> --source.<role>=<file>... [--importer=<name>] "
    "[--<setting>=<value>...]\n"
    "       asset-import <path> <source> [--cube-layout=<layout> | --volume-tile=<W>x<H>] ...\n"
    "       asset-import <path> (--cube=<faces> | --array=<files> | --cube-array=<cubes> |\n"
    "                            --volume=<files>) ...\n"
    "       [--unset=<paths>] [--erase=<paths>] [--project=<dir>] [--wait=<seconds>]\n"
    "  Import a source file into a new asset at <path>. The importer is the default one for the\n"
    "  source file extension unless --importer names one. An importer with several sources takes\n"
    "  one --source.<role>= per role. A setting whose first name is an import option field goes "
    "to\n"
    "  the import record (a reimport uses it again); any other setting is a property of the new\n"
    "  asset. Additional outputs are saved at <path>_<key>. Prints '<path> <AssetId>' and one\n"
    "  'output <key> <path> <AssetId>' line per additional output. Takes the project write lock.\n"
    "  Textures: --cube=px:<file>,nx:<file>,... (six faces), --array and --volume (one file per\n"
    "  slice, comma separated), --cube-array (cubes separated by ';', quote the argument),\n"
    "  --cube-layout (HorizontalCross, VerticalCross, Strip6) and --volume-tile split one image.\n"
    "  A DDS cube, array or volume file picks the matching importer. '\\,' and '\\;' escape.\n";

constexpr StringView ReimportUsage =
    "usage: asset-reimport <path> [--source=<file> | [--replace-sources] "
    "--source.<role>=<file>...]\n"
    "       [--<setting>=<value>...] [--unset=<paths>] [--erase=<paths>] [--project=<dir>] "
    "[--wait=<seconds>]\n"
    "  Import the recorded source files of the asset at <path> again with its recorded importer "
    "and\n"
    "  options, updating the outputs in place (references to them stay valid). --source replaces\n"
    "  the only source, --source.<role> one source of several; with --replace-sources the given\n"
    "  sources replace the whole recorded list. Settings change the recorded "
    "import\n"
    "  options or the asset properties as in asset-import. A recorded output the importer no "
    "longer\n"
    "  makes is reported as OrphanedOutput. Prints '<path> <AssetId>' and one\n"
    "  'output <key> <path> <AssetId>' line per additional output. Takes the project write lock.\n";

constexpr StringView FixIdUsage =
    "usage: asset-fix-duplicate-id <path> [--project=<dir>] [--wait=<seconds>]\n"
    "  Give the package at <path> a new AssetId when another package file has the same id (for\n"
    "  example after a copy made outside the tools). The file is rewritten in place; references\n"
    "  to the old id from other packages keep pointing at the remaining file. An import output\n"
    "  that loses its owner is reported as OrphanedOutput. Prints '<path> <old id> -> <new id>'.\n"
    "  Takes the project write lock.\n";

constexpr StringView DuplicateUsage =
    "usage: asset-duplicate <path|AssetId> <new path> [--project=<dir>] [--wait=<seconds>]\n"
    "  Save a copy of an editor package (with its unsaved edits) at a new path under a new\n"
    "  AssetId. Fields marked DuplicateTransient keep their class defaults, and soft references\n"
    "  to the package itself point at the copy. An import record of another import's output is\n"
    "  dropped. The new path must be free: no package, file or alias. Prints '<path> <AssetId>'.\n"
    "  Takes the project write lock.\n";

// 목록 옵션 값 분리자다(경로 key `[...]`·따옴표 안은 나누지 않는다).
constexpr char ListSeparator = ',';

// 명령의 인수다. 위치 인수 수(positionalCount)를 넘는 위치 인수는 대입이다.
struct EditArgs {
    explicit EditArgs(Allocator& allocator) noexcept
        : positional(allocator), assignments(allocator) {
    }

    StringView project;
    StringView wait;
    bool force = false;
    bool json = false;
    bool all = false;
    bool import = false;
    bool help = false;
    Array<StringView> positional;
    Array<TextAssignment> assignments;
};

// 무엇을 받는지다.
struct EditGrammar {
    usize positionalCount = 1;
    bool assignments = false;
    // `--reset=`(기본값 되돌리기 — asset-set만)을 받는지다.
    bool resets = false;
    bool force = false;
    bool infoFlags = false;
};

// list[start, end)를 kind 대입 하나로 붙인다(빈 항목은 false).
bool AppendPath(StringView list, usize start, usize end, AssignmentKind kind,
                Array<TextAssignment>& out) noexcept {
    if (end == start) {
        return false;
    }

    TextAssignment assignment;
    assignment.kind = kind;
    assignment.path = StringView(list.Data() + start, end - start);
    assignment.sourceIndex = static_cast<uint32>(out.Size());
    return static_cast<bool>(out.TryPushBack(assignment));
}

// 경로 목록을 쉼표로 나눠 kind 대입으로 붙인다. 경로 key `[...]` 안과 그 안의 따옴표(`\"`·`\\`
// escape) 안의 쉼표는 나누지 않는다. 빈 항목은 false다(짝이 맞지 않는 괄호는 경로 해석의 진단).
bool AppendPathList(StringView list, AssignmentKind kind, Array<TextAssignment>& out) noexcept {
    usize start = 0;
    usize depth = 0;
    bool quoted = false;
    for (usize index = 0; index < list.ByteCount(); ++index) {
        const char c = list.Data()[index];
        if (quoted) {
            if (c == '\\') {
                ++index;
            } else if (c == '"') {
                quoted = false;
            }

            continue;
        }

        if (c == '"' && depth != 0) {
            quoted = true;
        } else if (c == '[') {
            ++depth;
        } else if (c == ']' && depth != 0) {
            --depth;
        } else if (c == ListSeparator && depth == 0) {
            if (!AppendPath(list, start, index, kind, out)) {
                return false;
            }

            start = index + 1;
        }
    }

    return AppendPath(list, start, list.ByteCount(), kind, out);
}

bool ParseEdit(Span<const StringView> args, const EditGrammar& grammar, EditArgs& out) noexcept {
    if (args.Size() == 1 && args[0] == "--help") {
        out.help = true;
        return true;
    }

    for (StringView argument : args) {
        if (argument.StartsWith("--", StringComparison::CaseSensitive)) {
            StringView* slot = nullptr;
            StringView value;
            StringView* const slots[] = {&out.project, &out.wait};
            const StringView names[] = {"--project=", "--wait="};
            for (usize index = 0; index < 2; ++index) {
                if (argument.StartsWith(names[index], StringComparison::CaseSensitive)) {
                    slot = slots[index];
                    value = StringView(argument.Data() + names[index].ByteCount(),
                                       argument.ByteCount() - names[index].ByteCount());
                }
            }

            if (slot != nullptr) {
                if (!slot->IsEmpty() || value.IsEmpty()) {
                    return false;
                }

                *slot = value;
                continue;
            }

            bool* flag = nullptr;
            if (argument == "--force" && grammar.force) {
                flag = &out.force;
            } else if (argument == "--json" && grammar.infoFlags) {
                flag = &out.json;
            } else if (argument == "--all" && grammar.infoFlags) {
                flag = &out.all;
            } else if (argument == "--import" && grammar.infoFlags) {
                flag = &out.import;
            }

            if (flag != nullptr) {
                if (*flag) {
                    return false;
                }

                *flag = true;
                continue;
            }

            constexpr StringView Unset = "--unset=";
            constexpr StringView Erase = "--erase=";
            constexpr StringView Reset = "--reset=";
            const bool unset = argument.StartsWith(Unset, StringComparison::CaseSensitive);
            const bool erase = argument.StartsWith(Erase, StringComparison::CaseSensitive);
            const bool reset =
                grammar.resets && argument.StartsWith(Reset, StringComparison::CaseSensitive);
            if (!grammar.assignments || (!unset && !erase && !reset)) {
                return false;
            }

            AssignmentKind kind = AssignmentKind::Reset;
            usize prefix = Reset.ByteCount();
            if (unset) {
                kind = AssignmentKind::Unset;
                prefix = Unset.ByteCount();
            } else if (erase) {
                kind = AssignmentKind::Erase;
                prefix = Erase.ByteCount();
            }

            if (!AppendPathList(StringView(argument.Data() + prefix, argument.ByteCount() - prefix),
                                kind, out.assignments)) {
                return false;
            }

            continue;
        }

        if (out.positional.Size() < grammar.positionalCount) {
            if (argument.IsEmpty() || !out.positional.TryPushBack(argument)) {
                return false;
            }

            continue;
        }

        TextAssignment assignment;
        if (!grammar.assignments ||
            !reflection_text::MakeTextAssignment(
                argument, static_cast<uint32>(out.assignments.Size()), assignment) ||
            !out.assignments.TryPushBack(assignment)) {
            return false;
        }
    }

    // --import는 속성 출력(--all)과 함께 쓰지 않는다.
    uint32 milliseconds = 0;
    return out.positional.Size() == grammar.positionalCount && !(out.all && out.import) &&
           ParseWaitMilliseconds(out.wait, milliseconds);
}

// 편집 경고 이름이다(EditorWarningKind 순서).
const char* WarningName(EditorWarningKind kind) noexcept {
    constexpr const char* Names[] = {
        "ExcludedDependency",   "UnresolvedImport",        "DroppedUnresolvedImport",
        "NotDurable",           "OrphanNotDeleted",        "AliasResolved",
        "AliasTableUnreadable", "AliasNotRestored",        "AliasRemoved",
        "ImportRecordDropped",  "ImportWarning",           "OrphanedOutput",
        "ClassDefaultsChanged", "ClassDefaultsUnavailable"};
    static_assert(sizeof(Names) / sizeof(Names[0]) ==
                      static_cast<usize>(EditorWarningKind::ClassDefaultsUnavailable) + 1,
                  "every editor warning needs a name");
    return Names[static_cast<usize>(kind)];
}

// 진단 코드 이름이다(None이면 원본 오류 이름).
const char* DiagnosticName(const AssetDiagnostic& diagnostic, ErrorCode error) noexcept {
    return diagnostic.code != AssetDiagnosticCode::None ? AssetDiagnosticName(diagnostic.code)
                                                        : ErrorCodeName(error);
}

Result<int32> WriteWarnings(CommandletOutput& output, Allocator& allocator, StringView command,
                            const Array<EditorWarning>& warnings) noexcept {
    for (const EditorWarning& warning : warnings) {
        const AssetDiagnostic& diagnostic = warning.diagnostic;
        // 별칭으로 푼 경로는 옛 경로(경고에 담긴 텍스트)와 대상을 쓴다.
        if (warning.kind == EditorWarningKind::AliasResolved) {
            auto written = Fail(output, allocator, command, 0,
                                "AliasResolved: {}: warning resolved through an alias to {}",
                                warning.Text(), diagnostic.asset);
            if (!written) {
                return written;
            }

            continue;
        }

        // 삭제가 지운 별칭은 그 별칭 경로와 대상을 쓴다.
        if (warning.kind == EditorWarningKind::AliasRemoved) {
            auto written = Fail(output, allocator, command, 0,
                                "AliasRemoved: {}: warning removed the alias to {}", warning.Text(),
                                diagnostic.asset);
            if (!written) {
                return written;
            }

            continue;
        }

        auto written = Fail(output, allocator, command, 0, "{}: {}: warning {} (related {})",
                            DiagnosticName(diagnostic, diagnostic.error), diagnostic.asset,
                            WarningName(warning.kind), diagnostic.related);
        if (!written) {
            return written;
        }
    }

    return Result<int32>::Success(0);
}

// 실패를 쓴다: 대입 진단이 있으면 그 줄들, 없으면 에셋 진단 한 줄이다.
Result<int32> WriteFailure(CommandletOutput& output, Allocator& allocator, StringView command,
                           StringView target, const TextDiagnostics& diagnostics,
                           const AssetDiagnostic& failure, ErrorCode error) noexcept {
    if (error == ErrorCode::OutOfMemory) {
        return Result<int32>::Failure(error);
    }

    for (usize index = 0; index < diagnostics.Count(); ++index) {
        const auto entry = diagnostics.Get(index);
        auto written = Fail(output, allocator, command, 1, "{}: {}: {}",
                            AssetTextDiagnosticName(entry.code), entry.path, entry.message);
        if (!written) {
            return written;
        }
    }

    if (diagnostics.Count() != 0) {
        return Result<int32>::Success(1);
    }

    if (error == ErrorCode::Unsupported && failure.code == AssetDiagnosticCode::None) {
        return Fail(output, allocator, command, 1,
                    "Unsupported: {}: a value cannot be written as lines; use --json", target);
    }

    // 설명: 진단 코드가 있으면 원본 오류 이름, 없으면(코드 자리에 이미 원본 오류) "failed". 관련
    // 대상이 있으면 붙인다(MissingDependency의 대상 등).
    const char* description =
        failure.code != AssetDiagnosticCode::None ? ErrorCodeName(error) : "failed";
    if (!failure.related.IsNull()) {
        return Fail(output, allocator, command, 1, "{}: {}: {} (related {})",
                    DiagnosticName(failure, error), target, description, failure.related);
    }

    return Fail(output, allocator, command, 1, "{}: {}: {}", DiagnosticName(failure, error), target,
                description);
}

// 쓰기 명령이 끝날 때 색인 캐시를 쓴다(7.1절 16). 실패는 경고 CacheNotWritten이다(캐시는 정확성에
// 필요 없다 — 할당 실패만 실행 오류).
Result<int32> FlushCache(CommandletOutput& output, Allocator& allocator, StringView command,
                         EditSession& session) noexcept {
    auto flushed = session.service->FlushCache(session.lock);
    if (flushed) {
        return Result<int32>::Success(0);
    }

    if (flushed.Error() == ErrorCode::OutOfMemory) {
        return Result<int32>::Failure(flushed.Error());
    }

    return Fail(output, allocator, command, 0, "{}: {}: warning CacheNotWritten",
                ErrorCodeName(flushed.Error()), session.layout.IntermediateDirectory());
}

// 사용법을 검사한다: 도움말이면 0, 형식 오류면 2로 끝낸다(done).
Result<int32> CheckUsage(CommandletOutput& output, bool parsed, const EditArgs& args,
                         StringView usage, bool& done) noexcept {
    done = true;
    if (!parsed) {
        return Write(output, CommandletStream::Error, usage, 2);
    }

    if (args.help) {
        return Write(output, CommandletStream::Output, usage, 0);
    }

    done = false;
    return Result<int32>::Success(0);
}

} // namespace

Result<int32> RunAssetNewCommand(void* context, Span<const StringView> args,
                                 CommandletOutput& output) noexcept {
    const AssetCommandContext& command = *static_cast<const AssetCommandContext*>(context);
    Allocator& allocator = *command.allocator;
    EditArgs parsed(allocator);
    EditGrammar grammar;
    grammar.positionalCount = 2;
    grammar.assignments = true;
    bool done = false;
    auto usage = CheckUsage(output, ParseEdit(args, grammar, parsed), parsed, NewUsage, done);
    if (done) {
        return usage;
    }

    EditSession session(command);
    Result<int32> failure = Result<int32>::Success(1);
    if (!OpenSession(output, allocator, NewName, parsed.project, parsed.wait, true, session,
                     failure)) {
        return failure;
    }

    // 타입 이름을 먼저 본다(모르는 이름의 NotFound를 저장 단계의 NotFound와 섞지 않게).
    const StringView typeName = parsed.positional[0];
    if (auto known = session.types.Types().AcquireByToolName(
            Span<const char>(typeName.Data(), typeName.ByteCount()));
        !known) {
        // 할당 실패는 다른 경로처럼 실행 오류로 전파한다.
        if (known.Error() == ErrorCode::OutOfMemory) {
            return Result<int32>::Failure(known.Error());
        }

        return Fail(output, allocator, NewName, 1, "{}: {}: unknown type",
                    ErrorCodeName(known.Error()), typeName);
    }

    AssetDiagnostic diagnostic;
    auto made = session.service->New(session.lock, parsed.positional[0], parsed.positional[1],
                                     parsed.assignments.View(), session.diagnostics,
                                     &session.warnings, &diagnostic);
    if (auto warned = WriteWarnings(output, allocator, NewName, session.warnings); !warned) {
        return warned;
    }

    if (!made) {
        // 에셋 클래스 판정(TypeMismatch)은 타입 이름의 오류다(모르는 이름은 위에서 먼저 본다).
        if (diagnostic.code == AssetDiagnosticCode::TypeMismatch) {
            return Fail(output, allocator, NewName, 1,
                        "TypeMismatch: {}: not a concrete asset class", parsed.positional[0]);
        }

        return WriteFailure(output, allocator, NewName, parsed.positional[1], session.diagnostics,
                            diagnostic, made.Error());
    }

    if (auto flushed = FlushCache(output, allocator, NewName, session); !flushed) {
        return flushed;
    }

    // 저장이 색인에 넣은 정규화 경로다.
    auto entry = session.service->Index().FindById(made.Value());
    StringBuilder<LineBytes> line(allocator);
    if (auto formatted = line.TryAppendFormat(
            "{} {}\n", entry ? entry.Value()->path.View() : parsed.positional[1], made.Value());
        !formatted) {
        return Result<int32>::Failure(formatted.Error());
    }

    return Write(output, CommandletStream::Output, line.View(), 0);
}

Result<int32> RunAssetSetCommand(void* context, Span<const StringView> args,
                                 CommandletOutput& output) noexcept {
    const AssetCommandContext& command = *static_cast<const AssetCommandContext*>(context);
    Allocator& allocator = *command.allocator;
    EditArgs parsed(allocator);
    EditGrammar grammar;
    grammar.assignments = true;
    grammar.resets = true;
    grammar.force = true;
    bool done = false;
    const bool valid = ParseEdit(args, grammar, parsed) && !parsed.assignments.IsEmpty();
    auto usage = CheckUsage(output, valid || parsed.help, parsed, SetUsage, done);
    if (done) {
        return usage;
    }

    EditSession session(command);
    Result<int32> failure = Result<int32>::Success(1);
    if (!OpenSession(output, allocator, SetName, parsed.project, parsed.wait, true, session,
                     failure)) {
        return failure;
    }

    AssetDiagnostic diagnostic;
    auto set =
        session.service->Set(session.lock, parsed.positional[0], parsed.assignments.View(),
                             parsed.force, session.diagnostics, &session.warnings, &diagnostic);
    if (auto warned = WriteWarnings(output, allocator, SetName, session.warnings); !warned) {
        return warned;
    }

    if (!set) {
        return WriteFailure(output, allocator, SetName, parsed.positional[0], session.diagnostics,
                            diagnostic, set.Error());
    }

    return FlushCache(output, allocator, SetName, session);
}

Result<int32> RunAssetMoveCommand(void* context, Span<const StringView> args,
                                  CommandletOutput& output) noexcept {
    const AssetCommandContext& command = *static_cast<const AssetCommandContext*>(context);
    Allocator& allocator = *command.allocator;
    EditArgs parsed(allocator);
    EditGrammar grammar;
    grammar.positionalCount = 2;
    bool done = false;
    auto usage = CheckUsage(output, ParseEdit(args, grammar, parsed), parsed, MoveUsage, done);
    if (done) {
        return usage;
    }

    EditSession session(command);
    Result<int32> failure = Result<int32>::Success(1);
    if (!OpenSession(output, allocator, MoveName, parsed.project, parsed.wait, true, session,
                     failure)) {
        return failure;
    }

    AssetDiagnostic diagnostic;
    AssetMoveResult result;
    auto moved = session.service->Move(session.lock, parsed.positional[0], parsed.positional[1],
                                       result, &session.warnings, &diagnostic);
    if (auto warned = WriteWarnings(output, allocator, MoveName, session.warnings); !warned) {
        return warned;
    }

    if (!moved) {
        return WriteFailure(output, allocator, MoveName, parsed.positional[0], session.diagnostics,
                            diagnostic, moved.Error());
    }

    StringBuilder<LineBytes> line(allocator);
    if (auto formatted = line.TryAppendFormat("{}: {} packages moved, {} aliases added\n", MoveName,
                                              result.packages, result.aliases);
        !formatted) {
        return Result<int32>::Failure(formatted.Error());
    }

    if (auto written = Write(output, CommandletStream::Output, line.View(), 0); !written) {
        return written;
    }

    return FlushCache(output, allocator, MoveName, session);
}

namespace {

// asset-delete·asset-orphans-delete 공용이다(orphan이면 고아만 지운다 — --force 없음).
Result<int32> RunDelete(void* context, Span<const StringView> args, CommandletOutput& output,
                        bool orphan) noexcept {
    const AssetCommandContext& command = *static_cast<const AssetCommandContext*>(context);
    Allocator& allocator = *command.allocator;
    const StringView name = orphan ? OrphansDeleteName : DeleteName;
    EditArgs parsed(allocator);
    EditGrammar grammar;
    grammar.force = !orphan;
    bool done = false;
    auto usage = CheckUsage(output, ParseEdit(args, grammar, parsed), parsed,
                            orphan ? OrphansDeleteUsage : DeleteUsage, done);
    if (done) {
        return usage;
    }

    EditSession session(command);
    Result<int32> failure = Result<int32>::Success(1);
    if (!OpenSession(output, allocator, name, parsed.project, parsed.wait, true, session,
                     failure)) {
        return failure;
    }

    AssetDiagnostic diagnostic;
    AssetDeleteResult result(allocator);
    auto deleted = orphan
                       ? session.service->DeleteOrphan(session.lock, parsed.positional[0], result,
                                                       &session.warnings, &diagnostic)
                       : session.service->Delete(session.lock, parsed.positional[0], parsed.force,
                                                 result, &session.warnings, &diagnostic);
    // 디스크 참조자 줄(지웠으면 경고 — force, 아니면 실패의 근거).
    for (usize index = 0; index < result.referencerIds.Size(); ++index) {
        auto written = Fail(output, allocator, name, 0, "ReferencedBy: {}: {}{} references {}",
                            result.referencerPaths[index].View(), deleted ? "warning " : "",
                            result.referencerIds[index], result.path.View());
        if (!written) {
            return written;
        }
    }

    if (auto warned = WriteWarnings(output, allocator, name, session.warnings); !warned) {
        return warned;
    }

    if (!deleted) {
        return WriteFailure(output, allocator, name, parsed.positional[0], session.diagnostics,
                            diagnostic, deleted.Error());
    }

    StringBuilder<LineBytes> line(allocator);
    if (auto formatted =
            line.TryAppendFormat("{}: deleted {} {}\n", name, result.path.View(), result.id);
        !formatted) {
        return Result<int32>::Failure(formatted.Error());
    }

    if (auto written = Write(output, CommandletStream::Output, line.View(), 0); !written) {
        return written;
    }

    return FlushCache(output, allocator, name, session);
}

} // namespace

Result<int32> RunAssetDeleteCommand(void* context, Span<const StringView> args,
                                    CommandletOutput& output) noexcept {
    return RunDelete(context, args, output, false);
}

Result<int32> RunAssetOrphansDeleteCommand(void* context, Span<const StringView> args,
                                           CommandletOutput& output) noexcept {
    return RunDelete(context, args, output, true);
}

// asset-import의 인자다.
struct ImportArgs {
    explicit ImportArgs(Allocator& allocator) noexcept
        : positional(allocator), sources(allocator), assignments(allocator), reserved(allocator) {
    }

    StringView project;
    StringView wait;
    StringView importer;
    bool help = false;
    // 재임포트의 원본 전체 교체(`--replace-sources` — A2f3b)다.
    bool replaceSources = false;
    Array<StringView> positional;
    Array<AssetImportSourceInput> sources;
    Array<TextAssignment> assignments;
    // 번역기에 넘길 예약 인자(`--cube=` 등 — 12.12절, 새 임포트만)다.
    Array<AssetImportReservedArgument> reserved;
};

// `--<name>=<value>`의 name이 번역기가 받는 예약 이름(ImportTranslatedNames)인지다.
bool IsTranslatedImportName(StringView name) noexcept {
    for (StringView translated : ImportTranslatedNames) {
        if (name == translated) {
            return true;
        }
    }

    return false;
}

// `--<name>=<value>`의 name이 명령 예약 이름인지다(번역기가 받지 않는 예약 이름은 사용법 오류).
bool IsReservedImportName(StringView name) noexcept {
    for (StringView reserved : ImportReservedNames) {
        if (EqualsIgnoringAsciiCase(name, reserved)) {
            return true;
        }
    }

    return false;
}

// asset-import·asset-reimport 인자를 읽는다(reimport면 위치 인자는 path 하나, `--source=`를 받고
// `--importer=`를 받지 않는다). 사용법 위반은 false다.
bool ParseImport(Span<const StringView> args, bool reimport, ImportArgs& out) noexcept {
    if (args.Size() == 1 && args[0] == "--help") {
        out.help = true;
        return true;
    }

    constexpr StringView SourcePrefix = "--source.";
    for (StringView argument : args) {
        if (!argument.StartsWith("--", StringComparison::CaseSensitive)) {
            if (argument.IsEmpty() || out.positional.Size() == (reimport ? 1u : 2u) ||
                !out.positional.TryPushBack(argument)) {
                return false;
            }

            continue;
        }

        // 원본 전체 교체(재임포트만 — 7.1절 19 A2f3b). `--source.<역할>=`과만 쓴다(끝에서 본다).
        if (reimport && argument == "--replace-sources") {
            if (out.replaceSources) {
                return false;
            }

            out.replaceSources = true;
            continue;
        }

        // 원본 하나 바꾸기(재임포트 — 역할이 빈 원본으로 둔다).
        constexpr StringView SingleSource = "--source=";
        if (reimport && argument.StartsWith(SingleSource, StringComparison::CaseSensitive)) {
            const StringView value = argument.SliceBytes(
                SingleSource.ByteCount(), argument.ByteCount() - SingleSource.ByteCount());
            if (value.IsEmpty() || !out.sources.IsEmpty() ||
                !out.sources.TryPushBack(AssetImportSourceInput{{}, value})) {
                return false;
            }

            continue;
        }

        StringView* const slots[] = {&out.project, &out.wait, &out.importer};
        const StringView names[] = {"--project=", "--wait=", "--importer="};
        bool matched = false;
        for (usize index = 0; index < (reimport ? 2u : 3u) && !matched; ++index) {
            if (argument.StartsWith(names[index], StringComparison::CaseSensitive)) {
                const StringView value = argument.SliceBytes(
                    names[index].ByteCount(), argument.ByteCount() - names[index].ByteCount());
                if (!slots[index]->IsEmpty() || value.IsEmpty()) {
                    return false;
                }

                *slots[index] = value;
                matched = true;
            }
        }

        if (matched) {
            continue;
        }

        if (argument.StartsWith(SourcePrefix, StringComparison::CaseSensitive)) {
            const StringView rest = argument.SliceBytes(
                SourcePrefix.ByteCount(), argument.ByteCount() - SourcePrefix.ByteCount());
            usize equals = IndexNone;
            for (usize index = 0; index < rest.ByteCount() && equals == IndexNone; ++index) {
                equals = rest.Data()[index] == '=' ? index : IndexNone;
            }

            if (equals == IndexNone || equals == 0 || equals + 1 == rest.ByteCount()) {
                return false;
            }

            const AssetImportSourceInput source{
                rest.SliceBytes(0, equals),
                rest.SliceBytes(equals + 1, rest.ByteCount() - equals - 1)};
            for (const AssetImportSourceInput& other : out.sources) {
                if (other.role == source.role || other.role.IsEmpty()) {
                    return false;
                }
            }

            if (!out.sources.TryPushBack(source)) {
                return false;
            }

            continue;
        }

        constexpr StringView Unset = "--unset=";
        constexpr StringView Erase = "--erase=";
        const bool unset = argument.StartsWith(Unset, StringComparison::CaseSensitive);
        const bool erase = argument.StartsWith(Erase, StringComparison::CaseSensitive);
        if (unset || erase) {
            const usize prefix = unset ? Unset.ByteCount() : Erase.ByteCount();
            if (!AppendPathList(argument.SliceBytes(prefix, argument.ByteCount() - prefix),
                                unset ? AssignmentKind::Unset : AssignmentKind::Erase,
                                out.assignments)) {
                return false;
            }

            continue;
        }

        // `--<경로>=<값>` 설정(예약 이름은 받지 않는다 — 새 임포트의 번역 예약 이름은 따로 모은다,
        // 같은 이름 둘은 사용법 오류).
        TextAssignment assignment;
        const StringView text = argument.SliceBytes(2, argument.ByteCount() - 2);
        if (!MakeTextAssignment(text, static_cast<uint32>(out.assignments.Size()), assignment) ||
            assignment.path.IsEmpty()) {
            return false;
        }

        if (!reimport && IsTranslatedImportName(assignment.path)) {
            for (const AssetImportReservedArgument& other : out.reserved) {
                if (other.name == assignment.path) {
                    return false;
                }
            }

            if (assignment.value.IsEmpty() || !out.reserved.TryPushBack(AssetImportReservedArgument{
                                                  assignment.path, assignment.value})) {
                return false;
            }

            continue;
        }

        if (IsReservedImportName(assignment.path) || !out.assignments.TryPushBack(assignment)) {
            return false;
        }
    }

    // 재임포트: path 하나(원본 바꾸기는 선택). 전체 교체는 역할 있는 원본이 하나 이상이어야 한다.
    if (reimport) {
        const bool roleSources = !out.sources.IsEmpty() && !out.sources[0].role.IsEmpty();
        return out.positional.Size() == 1 && (!out.replaceSources || roleSources);
    }

    // path는 하나, 원본은 위치 인자 하나 또는 --source.<역할>= 하나 이상(섞지 않는다). 예약
    // 인자가 있으면 원본 없이도 된다(번역기가 원본 규칙을 정한다 — `--cube=` 등).
    if (out.positional.IsEmpty() || (out.positional.Size() == 2 && !out.sources.IsEmpty()) ||
        (out.positional.Size() == 1 && out.sources.IsEmpty() && out.reserved.IsEmpty())) {
        return false;
    }

    if (out.positional.Size() == 2) {
        return static_cast<bool>(out.sources.TryPushBack(
            AssetImportSourceInput{DefaultImportSourceRole, out.positional[1]}));
    }

    return true;
}

// asset-import·asset-reimport의 실행이다.
Result<int32> RunImportCommand(void* context, Span<const StringView> args, CommandletOutput& output,
                               bool reimport) noexcept {
    const AssetCommandContext& command = *static_cast<const AssetCommandContext*>(context);
    Allocator& allocator = *command.allocator;
    const StringView name = reimport ? ReimportName : ImportName;
    const StringView usage = reimport ? ReimportUsage : ImportUsage;
    ImportArgs parsed(allocator);
    const bool valid = ParseImport(args, reimport, parsed);
    if (parsed.help) {
        return Write(output, CommandletStream::Output, usage, 0);
    }

    if (!valid) {
        return Write(output, CommandletStream::Error, usage, 2);
    }

    // 상대 원본 경로는 현재 작업 디렉터리 기준이다(ProjectLayout과 같다). 번역기가 만든 원본도
    // 뒤에서 같은 규칙으로 푼다(String은 힙 byte라 배열이 자라도 view가 유효하다).
    Array<String> absolute(allocator);
    const auto makeAbsolute = [&](Array<AssetImportSourceInput>& sources,
                                  Result<int32>& failed) noexcept {
        for (AssetImportSourceInput& source : sources) {
            auto kind = Path::Root(source.path);
            if (!kind || kind.Value() != PathRoot::Relative) {
                continue;
            }

            Array<char> working(allocator);
            if (auto sized = working.TryResize(MaxNativePathBytes() + 1); !sized) {
                failed = Result<int32>::Failure(sized.Error());
                return false;
            }

            usize bytes = 0;
            if (auto got = platform::GetStandardDirectory(platform::StandardDirectory::Working,
                                                          working.Data(), working.Size(), bytes);
                !got) {
                failed =
                    Fail(output, allocator, name, 1, "{}: {}: cannot read the working directory",
                         ErrorCodeName(FileErrorCode(got)), source.path);
                return false;
            }

            auto made =
                Path::MakeAbsolute(allocator, StringView(working.Data(), bytes), source.path);
            if (!made) {
                failed = made.Error() == ErrorCode::OutOfMemory
                             ? Result<int32>::Failure(made.Error())
                             : Fail(output, allocator, name, 1, "InvalidSource: {}: {}",
                                    source.path, ErrorCodeName(made.Error()));
                return false;
            }

            if (auto pushed = absolute.TryPushBack(rocks::Move(made.Value())); !pushed) {
                failed = Result<int32>::Failure(pushed.Error());
                return false;
            }

            source.path = absolute[absolute.Size() - 1].View();
        }

        return true;
    };
    Result<int32> pathFailure = Result<int32>::Success(1);
    if (!makeAbsolute(parsed.sources, pathFailure)) {
        return pathFailure;
    }

    // 예약 인자 번역(12.12절 — 새 임포트): 예약 인자가 있거나 원본 하나에 `--importer=`가 없을 때.
    // 번역기가 없는데 예약 인자가 있으면 사용법 오류다.
    AssetImportTranslation translation(allocator);
    StringView importer = parsed.importer;
    if (!reimport) {
        const bool single = parsed.sources.Size() == 1 &&
                            parsed.sources[0].role == DefaultImportSourceRole &&
                            parsed.importer.IsEmpty();
        const AssetImportTranslator& translator = command.importTranslator;
        if (!parsed.reserved.IsEmpty() && translator.translate == nullptr) {
            return Write(output, CommandletStream::Error, usage, 2);
        }

        if (translator.translate != nullptr && (!parsed.reserved.IsEmpty() || single)) {
            auto translated =
                translator.translate(translator.state, allocator, parsed.reserved.View(),
                                     parsed.sources.View(), parsed.importer, translation);
            if (!translated) {
                return Result<int32>::Failure(translated.Error());
            }

            if (!translated.Value()) {
                return Write(output, CommandletStream::Error, usage, 2);
            }

            if (!translation.importer.IsEmpty()) {
                importer = translation.importer;
            }

            if (!translation.sources.IsEmpty()) {
                if (!makeAbsolute(translation.sources, pathFailure)) {
                    return pathFailure;
                }

                parsed.sources.Clear();
                for (const AssetImportSourceInput& source : translation.sources) {
                    if (auto pushed = parsed.sources.TryPushBack(source); !pushed) {
                        return Result<int32>::Failure(pushed.Error());
                    }
                }
            }

            // 번역 옵션 대입은 명령 대입 뒤 번호로 붙인다(진단의 대입 번호가 명령 줄 대입과 겹치지
            // 않게).
            for (reflection_text::TextAssignment option : translation.options) {
                option.sourceIndex = static_cast<uint32>(parsed.assignments.Size());
                if (auto pushed = parsed.assignments.TryPushBack(option); !pushed) {
                    return Result<int32>::Failure(pushed.Error());
                }
            }
        }
    }

    EditSession session(command);
    Result<int32> failure = Result<int32>::Success(1);
    if (!OpenSession(output, allocator, name, parsed.project, parsed.wait, true, session,
                     failure)) {
        return failure;
    }

    AssetDiagnostic diagnostic;
    AssetImportResult result(allocator);
    auto imported =
        reimport
            ? session.service->Reimport(session.lock, parsed.positional[0], parsed.sources.View(),
                                        parsed.assignments.View(), session.diagnostics, result,
                                        &session.warnings, &diagnostic, parsed.replaceSources)
            : session.service->Import(session.lock, parsed.positional[0], parsed.sources.View(),
                                      importer, parsed.assignments.View(), session.diagnostics,
                                      result, &session.warnings, &diagnostic);
    if (auto warned = WriteWarnings(output, allocator, name, session.warnings); !warned) {
        return warned;
    }

    if (!imported) {
        return WriteFailure(output, allocator, name, parsed.positional[0], session.diagnostics,
                            diagnostic, imported.Error());
    }

    StringBuilder<LineBytes> line(allocator);
    if (auto formatted = line.TryAppendFormat("{}: {} {}\n", name, result.path.View(), result.id);
        !formatted) {
        return Result<int32>::Failure(formatted.Error());
    }

    for (const AssetImportOutput& one : result.outputs) {
        if (auto formatted = line.TryAppendFormat("{}: output {} {} {}\n", name, one.key.View(),
                                                  one.path.View(), one.id);
            !formatted) {
            return Result<int32>::Failure(formatted.Error());
        }
    }

    if (auto written = Write(output, CommandletStream::Output, line.View(), 0); !written) {
        return written;
    }

    return FlushCache(output, allocator, name, session);
}

Result<int32> RunAssetImportCommand(void* context, Span<const StringView> args,
                                    CommandletOutput& output) noexcept {
    return RunImportCommand(context, args, output, false);
}

Result<int32> RunAssetReimportCommand(void* context, Span<const StringView> args,
                                      CommandletOutput& output) noexcept {
    return RunImportCommand(context, args, output, true);
}

Result<int32> RunAssetFixDuplicateIdCommand(void* context, Span<const StringView> args,
                                            CommandletOutput& output) noexcept {
    const AssetCommandContext& command = *static_cast<const AssetCommandContext*>(context);
    Allocator& allocator = *command.allocator;
    EditArgs parsed(allocator);
    EditGrammar grammar;
    bool done = false;
    auto usage = CheckUsage(output, ParseEdit(args, grammar, parsed), parsed, FixIdUsage, done);
    if (done) {
        return usage;
    }

    EditSession session(command);
    Result<int32> failure = Result<int32>::Success(1);
    if (!OpenSession(output, allocator, FixIdName, parsed.project, parsed.wait, true, session,
                     failure)) {
        return failure;
    }

    AssetDiagnostic diagnostic;
    AssetFixIdResult result(allocator);
    auto fixed = session.service->FixDuplicateId(session.lock, parsed.positional[0], result,
                                                 &session.warnings, &diagnostic);
    if (auto warned = WriteWarnings(output, allocator, FixIdName, session.warnings); !warned) {
        return warned;
    }

    if (!fixed) {
        return WriteFailure(output, allocator, FixIdName, parsed.positional[0], session.diagnostics,
                            diagnostic, fixed.Error());
    }

    StringBuilder<LineBytes> line(allocator);
    if (auto formatted = line.TryAppendFormat("{}: {} {} -> {}\n", FixIdName, result.path.View(),
                                              result.oldId, result.newId);
        !formatted) {
        return Result<int32>::Failure(formatted.Error());
    }

    // 남은 것이 하나면 옛 id 참조는 그쪽으로 풀리고, 둘 이상이면 아직 같은 id다(다시 고친다).
    const StringView remainingFormat = result.remaining.Size() == 1
                                           ? StringView("{}: references to {} resolve to {}\n")
                                           : StringView("{}: {} is still shared by {}\n");
    for (const String& remaining : result.remaining) {
        if (auto formatted =
                line.TryAppendFormat(remainingFormat, FixIdName, result.oldId, remaining.View());
            !formatted) {
            return Result<int32>::Failure(formatted.Error());
        }
    }

    if (auto written = Write(output, CommandletStream::Output, line.View(), 0); !written) {
        return written;
    }

    return FlushCache(output, allocator, FixIdName, session);
}

Result<int32> RunAssetDuplicateCommand(void* context, Span<const StringView> args,
                                       CommandletOutput& output) noexcept {
    const AssetCommandContext& command = *static_cast<const AssetCommandContext*>(context);
    Allocator& allocator = *command.allocator;
    EditArgs parsed(allocator);
    EditGrammar grammar;
    grammar.positionalCount = 2;
    bool done = false;
    auto usage = CheckUsage(output, ParseEdit(args, grammar, parsed), parsed, DuplicateUsage, done);
    if (done) {
        return usage;
    }

    EditSession session(command);
    Result<int32> failure = Result<int32>::Success(1);
    if (!OpenSession(output, allocator, DuplicateName, parsed.project, parsed.wait, true, session,
                     failure)) {
        return failure;
    }

    AssetDiagnostic diagnostic;
    AssetDuplicateResult result(allocator);
    auto duplicated =
        session.service->Duplicate(session.lock, parsed.positional[0], parsed.positional[1], result,
                                   &session.warnings, &diagnostic);
    if (auto warned = WriteWarnings(output, allocator, DuplicateName, session.warnings); !warned) {
        return warned;
    }

    if (!duplicated) {
        return WriteFailure(output, allocator, DuplicateName, parsed.positional[0],
                            session.diagnostics, diagnostic, duplicated.Error());
    }

    StringBuilder<LineBytes> line(allocator);
    if (auto formatted = line.TryAppendFormat("{} {}\n", result.path.View(), result.id);
        !formatted) {
        return Result<int32>::Failure(formatted.Error());
    }

    if (auto written = Write(output, CommandletStream::Output, line.View(), 0); !written) {
        return written;
    }

    return FlushCache(output, allocator, DuplicateName, session);
}

// 임포트 상태 이름이다(None이면 current).
StringView ImportStateName(AssetDiagnosticCode state) noexcept {
    return state == AssetDiagnosticCode::None ? StringView("current")
                                              : StringView(AssetDiagnosticName(state));
}

// asset-info --import 줄이다(7.1절 18 A2f2b).
Result<void> AppendImportLines(const AssetImportStatus& status, StringBuilderBase& out) noexcept {
    if (!status.Owned()) {
        if (status.ownerPath.IsEmpty()) {
            return out.TryAppendFormat("import.owner={} {}\n", status.owner, status.owner);
        }

        return out.TryAppendFormat("import.owner={} {}\n", status.ownerPath.View(), status.owner);
    }

    if (auto formatted =
            out.TryAppendFormat("import.importer={} {} {}\n", status.importerName.View(),
                                status.importerVersion, ImportStateName(status.importer));
        !formatted) {
        return formatted;
    }

    // 원인 오류가 있으면(InvalidSource·모르는 루트) 줄 끝에 ` (<오류>)`를 붙인다.
    for (const AssetImportSourceStatus& source : status.sources) {
        Result<void> formatted =
            out.TryAppendFormat("import.source.{}={}:{} {}", source.role.View(), source.root.View(),
                                source.relative.View(), ImportStateName(source.state));
        if (formatted && source.error != ErrorCode::None) {
            formatted = out.TryAppendFormat(" ({})", ErrorCodeName(source.error));
        }

        if (formatted) {
            formatted = out.TryAppend("\n");
        }

        if (!formatted) {
            return formatted;
        }
    }

    return Result<void>::Success();
}

// JSON 키 하나와 문자열 값이다.
Result<void> WriteJsonField(JsonWriter& writer, StringView key, StringView value) noexcept {
    if (auto keyed = writer.Key(key); !keyed) {
        return keyed;
    }

    return writer.String(value);
}

// asset-info --import --json 객체다(7.1절 18 A2f2b).
Result<void> AppendImportJson(const AssetImportStatus& status, StringBuilderBase& out) noexcept {
    JsonWriter writer(out);
    Result<void> step = writer.BeginObject();
    if (!status.Owned()) {
        char owner[AssetId::TextBytes + 1] = {};
        FixedStringBuilder ownerText{Span<char>(owner)};
        if (step) {
            step = ownerText.TryAppendFormat("{}", status.owner);
        }

        if (step) {
            step = WriteJsonField(writer, "owner", ownerText.View());
        }

        if (step) {
            step = writer.Key("ownerPath");
        }

        if (step) {
            step =
                status.ownerPath.IsEmpty() ? writer.Null() : writer.String(status.ownerPath.View());
        }
    } else {
        if (step) {
            step = writer.Key("importer");
        }

        if (step) {
            step = writer.BeginObject();
        }

        if (step) {
            step = WriteJsonField(writer, "name", status.importerName.View());
        }

        if (step) {
            step = writer.Key("version");
        }

        if (step) {
            step = writer.UInt64(status.importerVersion);
        }

        if (step) {
            step = WriteJsonField(writer, "state", ImportStateName(status.importer));
        }

        if (step) {
            step = writer.EndObject();
        }

        if (step) {
            step = writer.Key("sources");
        }

        if (step) {
            step = writer.BeginArray();
        }

        for (usize index = 0; step && index < status.sources.Size(); ++index) {
            const AssetImportSourceStatus& source = status.sources[index];
            step = writer.BeginObject();
            if (step) {
                step = WriteJsonField(writer, "role", source.role.View());
            }

            if (step) {
                step = WriteJsonField(writer, "root", source.root.View());
            }

            if (step) {
                step = WriteJsonField(writer, "relative", source.relative.View());
            }

            if (step) {
                step = WriteJsonField(writer, "state", ImportStateName(source.state));
            }

            // 원인 오류(없으면 null)다.
            if (step) {
                step = writer.Key("error");
            }

            if (step) {
                step = source.error == ErrorCode::None ? writer.Null()
                                                       : writer.String(ErrorCodeName(source.error));
            }

            if (step) {
                step = writer.EndObject();
            }
        }

        if (step) {
            step = writer.EndArray();
        }
    }

    if (step) {
        step = writer.EndObject();
    }

    if (step) {
        step = writer.Finish();
    }

    return step;
}

// asset-info --import다.
Result<int32> RunImportStatus(Allocator& allocator, const EditArgs& parsed, EditSession& session,
                              CommandletOutput& output) noexcept {
    AssetImportStatus status(allocator);
    AssetDiagnostic diagnostic;
    auto shown =
        session.service->ImportStatus(parsed.positional[0], status, &session.warnings, &diagnostic);
    if (auto warned = WriteWarnings(output, allocator, InfoName, session.warnings); !warned) {
        return warned;
    }

    if (!shown) {
        return WriteFailure(output, allocator, InfoName, parsed.positional[0], session.diagnostics,
                            diagnostic, shown.Error());
    }

    // JSON은 Finish가 줄 끝을 붙인다.
    StringBuilder<LineBytes> text(allocator);
    Result<void> formatted =
        parsed.json ? AppendImportJson(status, text) : AppendImportLines(status, text);
    if (!formatted) {
        return Result<int32>::Failure(formatted.Error());
    }

    return Write(output, CommandletStream::Output, text.View(), 0);
}

Result<int32> RunAssetInfoCommand(void* context, Span<const StringView> args,
                                  CommandletOutput& output) noexcept {
    const AssetCommandContext& command = *static_cast<const AssetCommandContext*>(context);
    Allocator& allocator = *command.allocator;
    EditArgs parsed(allocator);
    EditGrammar grammar;
    grammar.infoFlags = true;
    bool done = false;
    auto usage = CheckUsage(output, ParseEdit(args, grammar, parsed), parsed, InfoUsage, done);
    if (done) {
        return usage;
    }

    EditSession session(command);
    Result<int32> failure = Result<int32>::Success(1);
    if (!OpenSession(output, allocator, InfoName, parsed.project, parsed.wait, false, session,
                     failure)) {
        return failure;
    }

    if (parsed.import) {
        return RunImportStatus(allocator, parsed, session, output);
    }

    AssetInfoOptions options;
    options.json = parsed.json;
    options.all = parsed.all;
    String text(allocator);
    AssetDiagnostic diagnostic;
    auto shown =
        session.service->Info(parsed.positional[0], options, text, &session.warnings, &diagnostic);
    if (auto warned = WriteWarnings(output, allocator, InfoName, session.warnings); !warned) {
        return warned;
    }

    if (!shown) {
        return WriteFailure(output, allocator, InfoName, parsed.positional[0], session.diagnostics,
                            diagnostic, shown.Error());
    }

    if (parsed.json) {
        if (auto ended = text.TryAppend("\n"); !ended) {
            return Result<int32>::Failure(ended.Error());
        }
    }

    return Write(output, CommandletStream::Output, text.View(), 0);
}

} // namespace rocks::commandlet_host
