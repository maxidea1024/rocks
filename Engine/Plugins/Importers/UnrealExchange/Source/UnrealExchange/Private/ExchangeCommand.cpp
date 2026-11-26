#include <UnrealExchange/ExchangeCommand.h>

#include <Core/Text/StringBuilder.h>
#include <Platform/File.h>
#include <UnrealExchange/ExchangeIndexLookup.h>

// asset-import-unreal 명령이다(Docs/Design/UnrealExchange 10.2절, 10.5절 9번): 인자 해석,
// 세션·종류별 임포터, 묶음 읽기, --only 닫힘, 계획, 실행, 진단·결과 줄.

namespace rocks {

namespace {

using commandlet_host::EditSession;
using commandlet_host::LineBytes;

constexpr StringView ImportUnrealUsage =
    "usage: asset-import-unreal <bundle> [--root=/Game/Unreal] [--only=<objectPath>]...\n"
    "                           [--mesh-normals=unreal|rocks] [--dry-run]\n"
    "                           [--project=<path>] [--wait=<seconds>]\n";
constexpr StringView DefaultImportRoot = "/Game/Unreal";
// 사용법 오류의 종료 코드다(에셋 명령과 같다).
constexpr int32 UsageExitCode = 2;
// 명령 실패(묶음·계획·세션·실패한 에셋)의 종료 코드다.
constexpr int32 FailureExitCode = 1;

// 해석한 인자다.
struct ImportUnrealArgs {
    explicit ImportUnrealArgs(Allocator& allocator) noexcept : only(allocator) {
    }

    StringView bundle;
    StringView root = DefaultImportRoot;
    Array<StringView> only;
    StringView project;
    StringView wait;
    ExchangeImportOptions options;
    bool dryRun = false;
    bool help = false;
};

// "--name=value"의 값을 꺼낸다(이름이 맞으면 참). 같은 옵션 둘·빈 값은 seen·value로 호출자가 본다.
bool OptionValue(StringView argument, StringView name, StringView& value) noexcept {
    if (!argument.StartsWith(name, StringComparison::CaseSensitive) ||
        argument.ByteCount() <= name.ByteCount() || argument.Data()[name.ByteCount()] != '=') {
        return false;
    }

    const usize start = name.ByteCount() + 1;
    value = argument.SliceBytes(start, argument.ByteCount() - start);
    return true;
}

// 인자를 해석한다. 형식 오류면 거짓(사용법 오류)이다. 오류: 할당 실패.
Result<bool> ParseImportUnrealArgs(Span<const StringView> args, ImportUnrealArgs& out) noexcept {
    if (args.Size() == 1 && args[0] == "--help") {
        out.help = true;
        return Result<bool>::Success(true);
    }

    bool rootSet = false;
    bool projectSet = false;
    bool waitSet = false;
    bool normalsSet = false;
    bool dryRunSet = false;
    for (const StringView argument : args) {
        StringView value;
        if (!argument.StartsWith("--", StringComparison::CaseSensitive)) {
            if (!out.bundle.IsEmpty() || argument.IsEmpty()) {
                return Result<bool>::Success(false);
            }

            out.bundle = argument;
            continue;
        }

        if (argument == "--dry-run") {
            if (dryRunSet) {
                return Result<bool>::Success(false);
            }

            dryRunSet = true;
            continue;
        }

        if (OptionValue(argument, "--only", value)) {
            if (value.IsEmpty()) {
                return Result<bool>::Success(false);
            }

            if (auto pushed = out.only.TryPushBack(value); !pushed) {
                return Result<bool>::Failure(pushed.Error());
            }

            continue;
        }

        struct Single {
            StringView name;
            bool* seen;
            StringView* value;
        };
        StringView normals;
        const Single singles[] = {{"--root", &rootSet, &out.root},
                                  {"--project", &projectSet, &out.project},
                                  {"--wait", &waitSet, &out.wait},
                                  {"--mesh-normals", &normalsSet, &normals}};
        bool matched = false;
        for (const Single& single : singles) {
            if (!OptionValue(argument, single.name, value)) {
                continue;
            }

            if (*single.seen || value.IsEmpty()) {
                return Result<bool>::Success(false);
            }

            *single.seen = true;
            *single.value = value;
            matched = true;
        }

        if (!matched) {
            return Result<bool>::Success(false);
        }

        if (!normals.IsEmpty()) {
            if (normals != "unreal" && normals != "rocks") {
                return Result<bool>::Success(false);
            }

            out.options.meshNormals =
                normals == "rocks" ? ExchangeMeshNormals::Rocks : ExchangeMeshNormals::Unreal;
        }
    }

    // --wait는 다른 에셋 명령과 같이 해석 단계에서 본다(10진 숫자, 하루 이하).
    uint32 waitMilliseconds = 0;
    if (waitSet && !commandlet_host::ParseWaitMilliseconds(out.wait, waitMilliseconds)) {
        return Result<bool>::Success(false);
    }

    out.dryRun = dryRunSet;
    return Result<bool>::Success(!out.bundle.IsEmpty());
}

const char* SeverityName(ExchangeSeverity severity) noexcept {
    if (severity == ExchangeSeverity::Warning) {
        return "warning";
    }

    return severity == ExchangeSeverity::Info ? "info" : "error";
}

// 묶음 진단을 표준 오류에 쓴다.
Result<int32> WriteDiagnostics(CommandletOutput& output, Allocator& allocator,
                               const ExchangeBundle& bundle,
                               const ExchangeDiagnostics& diagnostics) noexcept {
    Span<const ExchangeAsset> assets = bundle.Assets();
    for (const ExchangeDiagnostic& item : diagnostics.Items()) {
        StringBuilder<LineBytes> line(allocator);
        const StringView target =
            item.asset < assets.Size() ? assets[item.asset].objectPath : StringView("bundle");
        auto formatted = line.TryAppendFormat(
            "{}: {}: {}: {} {}", AssetImportUnrealName, AssetDiagnosticName(item.code), target,
            SeverityName(item.severity), ExchangeIssueName(item.issue));
        if (formatted && !item.text.IsEmpty()) {
            formatted = line.TryAppendFormat(" {}", item.text.View());
        }

        if (formatted) {
            formatted = line.TryAppend("\n");
        }

        if (!formatted) {
            return Result<int32>::Failure(formatted.Error());
        }

        if (auto written = commandlet_host::Write(output, CommandletStream::Error, line.View(), 0);
            !written) {
            return written;
        }
    }

    if (diagnostics.Dropped() != 0) {
        return commandlet_host::Fail(output, allocator, AssetImportUnrealName, 0,
                                     "dropped {} diagnostics", diagnostics.Dropped());
    }

    return Result<int32>::Success(0);
}

// 서비스 경고를 표준 오류에 쓴다.
Result<int32> WriteServiceWarnings(CommandletOutput& output, Allocator& allocator,
                                   const Array<EditorWarning>& warnings) noexcept {
    for (const EditorWarning& warning : warnings) {
        if (auto written = commandlet_host::Fail(
                output, allocator, AssetImportUnrealName, 0, "{}: {}: warning",
                AssetDiagnosticName(warning.diagnostic.code), warning.Text());
            !written) {
            return written;
        }
    }

    return Result<int32>::Success(0);
}

// 결과의 동작 이름이다(보고 순서에는 고르지 않은 에셋이 없다).
const char* OutcomeVerb(ExchangeOutcomeState state) noexcept {
    switch (state) {
    case ExchangeOutcomeState::PlannedReimport:
        return "plan-reimport";
    case ExchangeOutcomeState::Created:
        return "created";
    default:
        return "reimported";
    }
}

// 에셋 결과 한 줄을 line에 붙인다.
Result<void> AppendOutcome(StringBuilderBase& line, const ExchangeImportPlan& plan,
                           const ExchangeAsset& asset, uint32 index,
                           const ExchangeAssetOutcome& outcome) noexcept {
    if (outcome.state == ExchangeOutcomeState::Unsupported ||
        outcome.state == ExchangeOutcomeState::Skipped) {
        return line.TryAppendFormat("{}: skipped {} {}\n", AssetImportUnrealName, asset.objectPath,
                                    outcome.state == ExchangeOutcomeState::Skipped ? "no-importer"
                                                                                   : "unsupported");
    }

    if (outcome.state == ExchangeOutcomeState::Failed) {
        return line.TryAppendFormat("{}: failed {} {}\n", AssetImportUnrealName, asset.objectPath,
                                    outcome.failure != AssetDiagnosticCode::None
                                        ? AssetDiagnosticName(outcome.failure)
                                        : ErrorCodeName(outcome.error));
    }

    // 새로 만들 에셋의 계획 줄에는 id가 없다.
    if (outcome.state == ExchangeOutcomeState::PlannedCreate) {
        return line.TryAppendFormat("{}: plan-create {} {}\n", AssetImportUnrealName,
                                    asset.objectPath, plan.TargetPath(index));
    }

    return line.TryAppendFormat("{}: {} {} {} {}\n", AssetImportUnrealName,
                                OutcomeVerb(outcome.state), asset.objectPath,
                                plan.TargetPath(index), outcome.id);
}

// 결과 줄과 끝 줄을 표준 출력에 쓴다.
Result<int32> WriteOutcomes(CommandletOutput& output, Allocator& allocator,
                            const ExchangeBundle& bundle, const ExchangeImportPlan& plan,
                            const ExchangeImportResult& result, bool dryRun) noexcept {
    for (const uint32 index : result.Order()) {
        StringBuilder<LineBytes> line(allocator);
        if (auto appended =
                AppendOutcome(line, plan, bundle.Assets()[index], index, result.Outcome(index));
            !appended) {
            return Result<int32>::Failure(appended.Error());
        }

        if (auto written = commandlet_host::Write(output, CommandletStream::Output, line.View(), 0);
            !written) {
            return written;
        }
    }

    const usize skipped = result.Count(ExchangeOutcomeState::Skipped) +
                          result.Count(ExchangeOutcomeState::Unsupported);
    const usize failed = result.Count(ExchangeOutcomeState::Failed);
    StringBuilder<LineBytes> summary(allocator);
    auto formatted =
        dryRun ? summary.TryAppendFormat(
                     "{}: {} to create, {} to reimport, {} failed, {} skipped\n",
                     AssetImportUnrealName, result.Count(ExchangeOutcomeState::PlannedCreate),
                     result.Count(ExchangeOutcomeState::PlannedReimport), failed, skipped)
               : summary.TryAppendFormat(
                     "{}: {} created, {} reimported, {} failed, {} skipped\n",
                     AssetImportUnrealName, result.Count(ExchangeOutcomeState::Created),
                     result.Count(ExchangeOutcomeState::Reimported), failed, skipped);
    if (!formatted) {
        return Result<int32>::Failure(formatted.Error());
    }

    return commandlet_host::Write(output, CommandletStream::Output, summary.View(),
                                  failed == 0 ? 0 : FailureExitCode);
}

// 묶음 진단을 쓰고 실패 줄(code 오류)을 쓴다. 할당 실패는 그 오류로 돌려준다.
Result<int32> StageFailure(CommandletOutput& output, Allocator& allocator,
                           const ExchangeBundle& bundle, const ExchangeDiagnostics& diagnostics,
                           ErrorCode error, StringView stage, StringView target) noexcept {
    if (error == ErrorCode::OutOfMemory) {
        return Result<int32>::Failure(error);
    }

    if (auto written = WriteDiagnostics(output, allocator, bundle, diagnostics); !written) {
        return written;
    }

    return commandlet_host::Fail(output, allocator, AssetImportUnrealName, FailureExitCode,
                                 "{}: {}: {}", ErrorCodeName(error), target, stage);
}

} // namespace

Result<int32> RunAssetImportUnrealCommand(void* context, Span<const StringView> args,
                                          CommandletOutput& output) noexcept {
    const UnrealExchangeCommandContext& command =
        *static_cast<const UnrealExchangeCommandContext*>(context);
    Allocator& allocator = *command.assets->allocator;
    ImportUnrealArgs parsed(allocator);
    auto valid = ParseImportUnrealArgs(args, parsed);
    if (!valid) {
        return Result<int32>::Failure(valid.Error());
    }

    if (parsed.help) {
        return commandlet_host::Write(output, CommandletStream::Output, ImportUnrealUsage, 0);
    }

    if (!valid.Value()) {
        return commandlet_host::Write(output, CommandletStream::Error, ImportUnrealUsage,
                                      UsageExitCode);
    }

    EditSession session(*command.assets);
    Result<int32> failure = Result<int32>::Success(FailureExitCode);
    if (!commandlet_host::OpenSession(output, allocator, AssetImportUnrealName, parsed.project,
                                      parsed.wait, !parsed.dryRun, session, failure)) {
        return failure;
    }

    ExchangeKindTable kinds;
    if (command.kinds.registerKinds != nullptr) {
        if (auto registered = command.kinds.registerKinds(command.kinds.state, kinds);
            !registered) {
            if (registered.Error() == ErrorCode::OutOfMemory) {
                return Result<int32>::Failure(registered.Error());
            }

            return commandlet_host::Fail(output, allocator, AssetImportUnrealName, FailureExitCode,
                                         "{}: kind importer registration failed",
                                         ErrorCodeName(registered.Error()));
        }
    }

    ExchangeBundle bundle(allocator);
    ExchangeDiagnostics diagnostics(allocator);
    if (auto read = ReadExchangeBundle(platform::PhysicalFileSystem(), parsed.bundle, {}, bundle,
                                       diagnostics);
        !read) {
        return StageFailure(output, allocator, bundle, diagnostics, read.Error(),
                            "failed to read the bundle", parsed.bundle);
    }

    Array<uint8> selection(allocator);
    uint32 missing = ExchangeNoIndex;
    if (auto selected = SelectExchangeClosure(bundle, parsed.only.View(), selection, &missing);
        !selected) {
        return StageFailure(output, allocator, bundle, diagnostics, selected.Error(),
                            "not in the bundle",
                            missing < parsed.only.Size() ? parsed.only[missing] : parsed.bundle);
    }

    ExchangeIndexLookup lookup(session.service->Index(), allocator);
    ExchangeOrderOptions order;
    order.root = parsed.root;
    order.existing = &lookup;
    order.selection = selection.View();
    ExchangeImportPlan plan(allocator);
    if (auto planned = PlanExchangeImport(bundle, order, plan, diagnostics); !planned) {
        return StageFailure(output, allocator, bundle, diagnostics, planned.Error(),
                            "planning failed", parsed.root);
    }

    ExchangeRunInput input;
    input.bundle = &bundle;
    input.plan = &plan;
    input.kinds = &kinds;
    input.options = &parsed.options;
    input.service = session.service.Get();
    input.lock = &session.lock;
    input.warnings = &session.warnings;
    ExchangeImportResult result(allocator);
    if (auto ran = RunExchangeImport(input, parsed.dryRun, result, diagnostics); !ran) {
        return Result<int32>::Failure(ran.Error());
    }

    if (auto written = WriteDiagnostics(output, allocator, bundle, diagnostics); !written) {
        return written;
    }

    if (auto written = WriteServiceWarnings(output, allocator, session.warnings); !written) {
        return written;
    }

    auto code = WriteOutcomes(output, allocator, bundle, plan, result, parsed.dryRun);
    if (!code || parsed.dryRun) {
        return code;
    }

    if (auto flushed = session.service->FlushCache(session.lock); !flushed) {
        if (flushed.Error() == ErrorCode::OutOfMemory) {
            return Result<int32>::Failure(flushed.Error());
        }

        if (auto written = commandlet_host::Fail(
                output, allocator, AssetImportUnrealName, 0, "{}: {}: warning CacheNotWritten",
                ErrorCodeName(flushed.Error()), session.layout.IntermediateDirectory());
            !written) {
            return written;
        }
    }

    return code;
}

} // namespace rocks
