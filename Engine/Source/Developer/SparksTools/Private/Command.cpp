#include <Core/Algorithms.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/SyntaxTree.h>
#include <SparksTools/SparksCommand.h>
#include "Workspace.h"

// `sparks` 명령 실행이다(Docs/Design/SparksTools.ko.md 2절 — LC8a: check·dump).
namespace rocks::sparks::tools {

namespace {

using detail::Workspace;
using detail::WorkspaceFile;
using detail::WorkspaceFileKind;

// 출력 builder의 inline 크기다(넘치면 allocator에서 늘린다).
constexpr usize OutputInlineBytes = 4096;

constexpr StringView Usage = R"(usage: sparks <command> [options] <path>...
commands:
  check    check the packages and print the diagnostics
  dump     print a dump of each module: --ast, --names, --types, --ir or --bytecode
options:
  --schema <folder>   engine binding schemas (*.sparkbind.json); may repeat
a path is a folder (every package below it, or the package it is in) or a .spark file (its package;
dump prints that module only).
)";

enum class DumpKind : uint8 {
    None,
    Syntax,
    Names,
    Types,
    Ir,
    Bytecode
};

// 명령 인자 분해 결과다(T5).
struct Arguments {
    explicit Arguments(Allocator& allocator) noexcept : schemas(allocator), paths(allocator) {
    }

    StringView command;
    Array<StringView> schemas;
    Array<StringView> paths;
    DumpKind dump = DumpKind::None;
};

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
bool Parse(Span<const StringView> input, Arguments& out, StringView& message,
           bool& outOfMemory) noexcept {
    outOfMemory = false;
    out.command = input[0];
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

    if (out.command != "check" && out.command != "dump") {
        message = "unknown command";
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

    Arguments parsed(allocator);
    StringView message;
    bool outOfMemory = false;
    if (!Parse(arguments, parsed, message, outOfMemory)) {
        return outOfMemory ? ExitError : UsageError(output, allocator, message);
    }

    // 작업 공간(T3).
    Workspace workspace(allocator);
    StringBuilder<OutputInlineBytes> problem(allocator);
    bool ioFailure = false;
    auto loaded =
        workspace.Load(files, parsed.paths.View(), parsed.schemas.View(), problem, ioFailure);
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

    SourceSet sources(allocator);
    DiagnosticList diagnostics(allocator);
    Compilation compilation(allocator, sources);
    Array<uint32> modules(allocator);
    if (!workspace.AddTo(sources, compilation, diagnostics, modules) ||
        !compilation.SetLastStage(StageOf(parsed.dump)) || !compilation.Check(diagnostics)) {
        return ExitError;
    }

    diagnostics.Sort(sources);
    StringBuilder<OutputInlineBytes> text(allocator);
    if (!AppendDiagnostics(text, sources, diagnostics)) {
        return ExitError;
    }

    // 모듈 목록(진단 경로 byte 순 — dump 파일 인자는 고른 모듈만).
    Array<usize> order(allocator);
    bool selected = false;
    for (const WorkspaceFile& file : workspace.Files()) {
        selected = selected || (file.selected && parsed.command == "dump");
    }

    for (usize at = 0; at < workspace.Files().Size(); ++at) {
        const WorkspaceFile& file = workspace.Files()[at];
        if (file.kind == WorkspaceFileKind::Module && (!selected || file.selected) &&
            !order.TryPushBack(at)) {
            return ExitError;
        }
    }

    const Span<const WorkspaceFile> loadedFiles = workspace.Files();
    algo::Sort(order.View(), [&](usize left, usize right) noexcept {
        return StringView::CompareBytes(loadedFiles[left].path.View(),
                                        loadedFiles[right].path.View()) < 0;
    });
    if (parsed.command == "dump") {
        for (const usize at : order) {
            text.AppendFormat("== {}\n", loadedFiles[at].path.View());
            auto dumped = AppendDump(text, parsed.dump, sources, compilation, modules[at]);
            if (!dumped) {
                return ExitError;
            }

            if (!dumped.Value()) {
                text.Append("(no dump: the module stopped before this stage)\n");
            }
        }
    } else {
        text.AppendFormat("{} errors in {} modules\n", diagnostics.Count(), order.Size());
    }

    if (text.HasFailed() || !output.WriteOutput(text.View())) {
        return ExitError;
    }

    return diagnostics.Count() == 0 ? ExitSuccess : ExitFailure;
}

} // namespace rocks::sparks::tools
