#include <Core/Algorithms.h>
#include <SparksCompiler/SyntaxTree.h>
#include <SparksTestHost/TestRunner.h>
#include "CommandInternal.h"

// `sparks` 명령 실행이다(Docs/Design/SparksTools.ko.md 2절·7절·8절·9절 — check·dump·build·ids·fmt·
// test).
namespace rocks::sparks::tools {

namespace detail {

Result<void> ModuleOrder(const Workspace& workspace, bool selectedOnly,
                         Array<usize>& order) noexcept {
    const Span<const WorkspaceFile> files = workspace.Files();
    for (usize at = 0; at < files.Size(); ++at) {
        const WorkspaceFile& file = files[at];
        if (file.kind == WorkspaceFileKind::Module && (!selectedOnly || file.selected)) {
            if (auto pushed = order.TryPushBack(at); !pushed) {
                return pushed;
            }
        }
    }

    algo::Sort(order.View(), [&](usize left, usize right) noexcept {
        return StringView::CompareBytes(files[left].path.View(), files[right].path.View()) < 0;
    });
    return Result<void>::Success();
}

void AppendSummary(StringBuilderBase& text, const CommandContext& context) noexcept {
    usize modules = 0;
    for (const WorkspaceFile& file : context.workspace.Files()) {
        modules += file.kind == WorkspaceFileKind::Module ? 1 : 0;
    }

    text.AppendFormat("{} errors in {} modules\n", context.diagnostics.Count(), modules);
}

int32 ReportProblem(CommandContext& context, StringView path, StringView message,
                    int32 code) noexcept {
    StringBuilder<OutputInlineBytes> text(context.allocator);
    text.AppendFormat("{}: {}\n", path.IsEmpty() ? StringView("sparks") : path, message);
    if (text.HasFailed() || !context.output.WriteError(text.View())) {
        return ExitError;
    }

    return code;
}

} // namespace detail

namespace {

using detail::CommandArguments;
using detail::CommandContext;
using detail::DumpKind;
using detail::OutputInlineBytes;
using detail::WorkspaceFile;

constexpr StringView Usage = R"(usage: sparks <command> [options] <path>...
commands:
  check    check the packages and print the diagnostics
  dump     print a dump of each module: --ast, --names, --types, --ir or --bytecode
  build    write each module's bytecode to --out <folder>/<package>/<module>.sparkc
  ids      issue the pending stable IDs into each package's .rocksids.json
  fmt      rewrite each module in the official format (--check: only list the files that differ)
  test     run the @test functions of the modules on the headless test host
options:
  --schema <folder>   engine binding schemas (*.sparkbind.json); may repeat
  --out <folder>      the output folder of 'build'
  --check             'fmt' writes nothing and fails when a file is not formatted
  --max-frames <N>    'test' lets a waiting test run N frames (1 to 1000000, default 600)
a path is a folder (every package below it, or the package it is in) or a .spark file (its package;
dump, fmt and test use the modules below the path only).
)";

// 사용법·오류 문장의 "1 to 1000000"은 이 상한이다.
static_assert(testing::MaxTestFrames == 1000000, "update the --max-frames usage text");

// `--max-frames` 값(10진수 1–MaxTestFrames — 앞의 0은 된다)이다. 틀리면 0.
uint32 ParseFrames(StringView text) noexcept {
    // 10진 자리 하나의 기수다.
    constexpr uint32 DecimalBase = 10;
    if (text.IsEmpty()) {
        return 0;
    }

    uint32 value = 0;
    for (usize at = 0; at < text.ByteCount(); ++at) {
        const char digit = text.Data()[at];
        if (digit < '0' || digit > '9') {
            return 0;
        }

        value = value * DecimalBase + static_cast<uint32>(digit - '0');
        if (value > testing::MaxTestFrames) {
            return 0; // 넘치기 전에 멈춘다
        }
    }

    return value;
}

// 사용법 오류: 문장과 사용법을 오류 스트림에 쓴다.
int32 UsageError(CommandOutput& output, Allocator& allocator, StringView message) noexcept {
    StringBuilder<OutputInlineBytes> text(allocator);
    text.AppendFormat("sparks: {}\n", message);
    text.Append(Usage);
    if (text.HasFailed()) {
        return ExitError;
    }

    return output.WriteError(text.View()) ? ExitUsage : ExitError;
}

// 인자를 나눈다. 값 false는 사용법 오류(message에 이유).
bool Parse(Span<const StringView> input, CommandArguments& out, StringView& message,
           bool& outOfMemory) noexcept {
    outOfMemory = false;
    out.command = input[0];
    // 명령 이름을 먼저 본다(옵션 오류보다 앞 — `sparks frob --out o`는 알 수 없는 명령).
    if (out.command != "check" && out.command != "dump" && out.command != "build" &&
        out.command != "ids" && out.command != "fmt" && out.command != "test") {
        message = "unknown command";
        return false;
    }

    for (usize at = 1; at < input.Size(); ++at) {
        const StringView argument = input[at];
        if (argument == "--schema") {
            if (at + 1 == input.Size()) {
                message = "'--schema' needs a folder";
                return false;
            }

            outOfMemory = !out.schemas.TryPushBack(input[++at]);
            if (outOfMemory) {
                return false;
            }

            continue;
        }

        if (argument == "--check") {
            if (out.command != "fmt" || out.check) {
                message = out.command != "fmt" ? "'--check' is only for 'fmt'"
                                               : "'--check' is given twice";
                return false;
            }

            out.check = true;
            continue;
        }

        if (argument == "--max-frames") {
            if (out.command != "test" || out.maxFrames != 0) {
                message = out.command != "test" ? "'--max-frames' is only for 'test'"
                                                : "'test' takes one --max-frames";
                return false;
            }

            out.maxFrames = at + 1 < input.Size() ? ParseFrames(input[++at]) : 0;
            if (out.maxFrames == 0) {
                message = "'--max-frames' needs a number from 1 to 1000000";
                return false;
            }

            continue;
        }

        if (argument == "--out") {
            if (out.command != "build" || out.hasOut || at + 1 == input.Size()) {
                message = out.command != "build" ? "'--out' is only for 'build'"
                          : out.hasOut           ? "'build' takes one --out"
                                                 : "'--out' needs a folder";
                return false;
            }

            out.out = input[++at];
            out.hasOut = true;
            continue;
        }

        const DumpKind kind = argument == "--ast"        ? DumpKind::Syntax
                              : argument == "--names"    ? DumpKind::Names
                              : argument == "--types"    ? DumpKind::Types
                              : argument == "--ir"       ? DumpKind::Ir
                              : argument == "--bytecode" ? DumpKind::Bytecode
                                                         : DumpKind::None;
        if (kind != DumpKind::None) {
            if (out.command != "dump" || out.dump != DumpKind::None) {
                message = out.command != "dump" ? "a dump option is only for 'dump'"
                                                : "'dump' takes one dump option";
                return false;
            }

            out.dump = kind;
            continue;
        }

        if (argument.StartsWith("--")) {
            message = "unknown option";
            return false;
        }

        outOfMemory = !out.paths.TryPushBack(argument);
        if (outOfMemory) {
            return false;
        }
    }

    if (out.command == "build" && !out.hasOut) {
        message = "'build' needs --out <folder>";
        return false;
    }

    if (out.command == "dump" && out.dump == DumpKind::None) {
        message = "'dump' needs --ast, --names, --types, --ir or --bytecode";
        return false;
    }

    if (out.paths.IsEmpty()) {
        message = "no path given";
        return false;
    }

    return true;
}

CompilationStage StageOf(DumpKind kind) noexcept {
    switch (kind) {
    case DumpKind::Syntax:
        return CompilationStage::Syntax;
    case DumpKind::Names:
        return CompilationStage::Names;
    case DumpKind::Types:
        return CompilationStage::Types;
    default:
        return CompilationStage::All;
    }
}

// 모듈 하나의 덤프를 붙인다. 값 false는 그 모듈의 덤프가 없다(그 단계 전에 멈춤·진단).
Result<bool> AppendDump(StringBuilderBase& out, DumpKind kind, const SourceSet& sources,
                        const Compilation& compilation, uint32 module) noexcept {
    switch (kind) {
    case DumpKind::Syntax: {
        const SyntaxTree* tree = compilation.ModuleTree(module);
        if (tree == nullptr) {
            return Result<bool>::Success(false);
        }

        if (auto appended = AppendSyntaxDump(out, sources, *tree); !appended) {
            return Result<bool>::Failure(appended.Error());
        }

        return Result<bool>::Success(true);
    }
    case DumpKind::Names:
    case DumpKind::Types:
    case DumpKind::Ir:
    case DumpKind::Bytecode: {
        auto appended = kind == DumpKind::Names   ? compilation.AppendNameDump(out, module)
                        : kind == DumpKind::Types ? compilation.AppendTypeDump(out, module)
                        : kind == DumpKind::Ir    ? compilation.AppendIrDump(out, module)
                                                  : compilation.AppendBytecodeDump(out, module);
        if (!appended) {
            if (appended.Error() == ErrorCode::InvalidArgument) {
                return Result<bool>::Success(false);
            }

            return Result<bool>::Failure(appended.Error());
        }

        return Result<bool>::Success(true);
    }
    default:
        return Result<bool>::Success(false);
    }
}

} // namespace

int32 RunCommand(Allocator& allocator, platform::FileSystem& files,
                 Span<const StringView> arguments, CommandOutput& output) noexcept {
    if (arguments.IsEmpty()) {
        return UsageError(output, allocator, "no command given");
    }

    CommandArguments parsed(allocator);
    StringView message;
    bool outOfMemory = false;
    if (!Parse(arguments, parsed, message, outOfMemory)) {
        return outOfMemory ? ExitError : UsageError(output, allocator, message);
    }

    // 작업 공간(T3).
    CommandContext context(allocator, files, parsed, output);
    StringBuilder<OutputInlineBytes> problem(allocator);
    bool ioFailure = false;
    auto loaded = context.workspace.Load(files, parsed.paths.View(), parsed.schemas.View(), problem,
                                         ioFailure);
    if (!loaded) {
        return ExitError;
    }

    if (!loaded.Value()) {
        problem.Append("\n");
        if (problem.HasFailed() || !output.WriteError(problem.View())) {
            return ExitError;
        }

        return ioFailure ? ExitError : ExitFailure;
    }

    // fmt는 컴파일하지 않는다(파일마다 구문만 — 8.3절).
    if (parsed.command == "fmt") {
        StringBuilder<OutputInlineBytes> text(allocator);
        const int32 code = detail::RunFmt(context, text);
        if (code == ExitError || text.HasFailed() || !output.WriteOutput(text.View())) {
            return ExitError;
        }

        return code;
    }

    if (!context.workspace.AddTo(context.sources, context.compilation, context.diagnostics,
                                 context.modules, context.packages) ||
        !context.compilation.SetLastStage(StageOf(parsed.dump)) ||
        !context.compilation.Check(context.diagnostics)) {
        return ExitError;
    }

    context.diagnostics.Sort(context.sources);
    StringBuilder<OutputInlineBytes> text(allocator);
    if (!AppendDiagnostics(text, context.sources, context.diagnostics)) {
        return ExitError;
    }

    int32 code = context.diagnostics.Count() == 0 ? ExitSuccess : ExitFailure;
    if (parsed.command == "dump") {
        // 모듈 목록(진단 경로 byte 순 — 파일 인자는 고른 모듈만).
        Array<usize> order(allocator);
        if (!detail::ModuleOrder(context.workspace, true, order)) {
            return ExitError;
        }

        const Span<const WorkspaceFile> loadedFiles = context.workspace.Files();
        for (const usize at : order) {
            text.AppendFormat("== {}\n", loadedFiles[at].path.View());
            auto dumped = AppendDump(text, parsed.dump, context.sources, context.compilation,
                                     context.modules[at]);
            if (!dumped) {
                return ExitError;
            }

            if (!dumped.Value()) {
                text.Append("(no dump: the module stopped before this stage)\n");
            }
        }
    } else if (parsed.command == "build") {
        code = detail::RunBuild(context, text);
    } else if (parsed.command == "ids") {
        code = detail::RunIds(context, text);
    } else if (parsed.command == "test") {
        code = detail::RunTest(context, text);
    } else {
        detail::AppendSummary(text, context);
    }

    if (text.HasFailed() || !output.WriteOutput(text.View())) {
        return ExitError;
    }

    return code;
}

} // namespace rocks::sparks::tools
