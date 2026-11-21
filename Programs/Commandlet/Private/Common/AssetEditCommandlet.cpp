#include "AssetEditCommandlet.h"
#include "AssetCommandletSupport.h"

#include <Core/Containers/Array.h>
#include <ReflectionText/TextAssignments.h>

// asset-new·asset-set·asset-info 명령 구현이다(Docs/Design/AssetSystem.ko.md 13절, 7.1절 15
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
    "                 [--force] [--project=<dir>] [--wait=<seconds>]\n"
    "  Check every assignment, apply them together and save the package. Errors are all\n"
    "  reported and nothing changes. --unset resets Optional values, --erase removes map\n"
    "  entries, set elements or array elements (comma separated path lists). --force saves\n"
    "  unresolved imports as null. Takes the project write lock.\n";

constexpr StringView InfoUsage =
    "usage: asset-info <path|AssetId> [--json] [--all] [--project=<dir>] [--wait=<seconds>]\n"
    "  Print the representative asset as '<path>=<value>' lines that asset-set accepts as\n"
    "  they are, or as JSON with --json. --all also prints the fields that editing skips.\n"
    "  Takes no lock.\n";

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
    bool help = false;
    Array<StringView> positional;
    Array<TextAssignment> assignments;
};

// 무엇을 받는지다.
struct EditGrammar {
    usize positionalCount = 1;
    bool assignments = false;
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
            const bool unset = argument.StartsWith(Unset, StringComparison::CaseSensitive);
            const bool erase = argument.StartsWith(Erase, StringComparison::CaseSensitive);
            if (!grammar.assignments || (!unset && !erase)) {
                return false;
            }

            const usize prefix = unset ? Unset.ByteCount() : Erase.ByteCount();
            if (!AppendPathList(StringView(argument.Data() + prefix, argument.ByteCount() - prefix),
                                unset ? AssignmentKind::Unset : AssignmentKind::Erase,
                                out.assignments)) {
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

    uint32 milliseconds = 0;
    return out.positional.Size() == grammar.positionalCount &&
           ParseWaitMilliseconds(out.wait, milliseconds);
}

// 편집 경고 이름이다(EditorWarningKind 순서).
const char* WarningName(EditorWarningKind kind) noexcept {
    constexpr const char* Names[] = {
        "ExcludedDependency",   "UnresolvedImport", "DroppedUnresolvedImport",
        "NotDurable",           "OrphanNotDeleted", "AliasResolved",
        "AliasTableUnreadable", "AliasNotRestored", "AliasRemoved"};
    static_assert(sizeof(Names) / sizeof(Names[0]) ==
                      static_cast<usize>(EditorWarningKind::AliasRemoved) + 1,
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
        auto written =
            Fail(output, allocator, command, 1, "{}: {}: {}",
                 reflection_text::TextDiagnosticCodeName(entry.code), entry.path, entry.message);
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

    EditSession session(allocator, *command.modules);
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
    grammar.force = true;
    bool done = false;
    const bool valid = ParseEdit(args, grammar, parsed) && !parsed.assignments.IsEmpty();
    auto usage = CheckUsage(output, valid || parsed.help, parsed, SetUsage, done);
    if (done) {
        return usage;
    }

    EditSession session(allocator, *command.modules);
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

    EditSession session(allocator, *command.modules);
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

Result<int32> RunAssetDeleteCommand(void* context, Span<const StringView> args,
                                    CommandletOutput& output) noexcept {
    const AssetCommandContext& command = *static_cast<const AssetCommandContext*>(context);
    Allocator& allocator = *command.allocator;
    EditArgs parsed(allocator);
    EditGrammar grammar;
    grammar.force = true;
    bool done = false;
    auto usage = CheckUsage(output, ParseEdit(args, grammar, parsed), parsed, DeleteUsage, done);
    if (done) {
        return usage;
    }

    EditSession session(allocator, *command.modules);
    Result<int32> failure = Result<int32>::Success(1);
    if (!OpenSession(output, allocator, DeleteName, parsed.project, parsed.wait, true, session,
                     failure)) {
        return failure;
    }

    AssetDiagnostic diagnostic;
    AssetDeleteResult result(allocator);
    auto deleted = session.service->Delete(session.lock, parsed.positional[0], parsed.force, result,
                                           &session.warnings, &diagnostic);
    // 디스크 참조자 줄(지웠으면 경고 — force, 아니면 실패의 근거).
    for (usize index = 0; index < result.referencerIds.Size(); ++index) {
        auto written =
            Fail(output, allocator, DeleteName, 0, "ReferencedBy: {}: {}{} references {}",
                 result.referencerPaths[index].View(), deleted ? "warning " : "",
                 result.referencerIds[index], result.path.View());
        if (!written) {
            return written;
        }
    }

    if (auto warned = WriteWarnings(output, allocator, DeleteName, session.warnings); !warned) {
        return warned;
    }

    if (!deleted) {
        return WriteFailure(output, allocator, DeleteName, parsed.positional[0],
                            session.diagnostics, diagnostic, deleted.Error());
    }

    StringBuilder<LineBytes> line(allocator);
    if (auto formatted =
            line.TryAppendFormat("{}: deleted {} {}\n", DeleteName, result.path.View(), result.id);
        !formatted) {
        return Result<int32>::Failure(formatted.Error());
    }

    if (auto written = Write(output, CommandletStream::Output, line.View(), 0); !written) {
        return written;
    }

    return FlushCache(output, allocator, DeleteName, session);
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

    EditSession session(allocator, *command.modules);
    Result<int32> failure = Result<int32>::Success(1);
    if (!OpenSession(output, allocator, InfoName, parsed.project, parsed.wait, false, session,
                     failure)) {
        return failure;
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
