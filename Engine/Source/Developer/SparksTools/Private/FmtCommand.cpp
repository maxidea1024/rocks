#include <SparksCompiler/Formatter.h>
#include "CommandInternal.h"
#include "ToolFiles.h"

// `sparks fmt`다(Docs/Design/SparksTools.ko.md 8.3절): 작업 공간 모듈 원문을 공식 서식으로 바꾼다
// (`--check`는 쓰지 않고 다른 파일을 알린다). 컴파일하지 않는다.
namespace rocks::sparks::tools::detail {

int32 RunFmt(CommandContext& context, StringBuilderBase& text) noexcept {
    Array<usize> order(context.allocator);
    if (!ModuleOrder(context.workspace, true, order)) {
        return ExitError;
    }

    const Span<const WorkspaceFile> files = context.workspace.Files();
    StringBuilder<OutputInlineBytes> lines(context.allocator);
    uint32 changed = 0;
    uint32 broken = 0;
    for (const usize at : order) {
        const WorkspaceFile& file = files[at];
        const StringView original(file.text.Data(), file.text.Size());
        const usize before = context.diagnostics.Count();
        auto id = context.sources.TryAdd(file.path.View(), original, context.diagnostics);
        if (!id) {
            return ExitError;
        }

        // 원문 검사 진단(S0101–S0103)이나 어휘·구문 진단이 있으면 건드리지 않는다.
        StringBuilder<OutputInlineBytes> formatted(context.allocator);
        bool done = false;
        if (context.diagnostics.Count() == before) {
            auto result = FormatSource(context.allocator, context.sources, id.Value(),
                                       context.diagnostics, formatted);
            if (!result) {
                return ExitError;
            }

            done = result.Value();
        }

        if (!done) {
            ++broken;
            continue;
        }

        if (formatted.View() == original) {
            continue;
        }

        ++changed;
        if (context.arguments.check) {
            lines.AppendFormat("{}: not formatted\n", file.path.View());
            continue;
        }

        auto replaced = ReplaceFile(context.allocator, context.files, file.full.View(),
                                    BytesOf(formatted.View()));
        if (!replaced) {
            return ExitError;
        }

        if (!replaced.Value()) {
            return ReportProblem(context, file.full.View(),
                                 replaced.Value().code == platform::FileCode::NotDurable
                                     ? NotDurableMessage
                                     : StringView("cannot write the file"),
                                 ExitError);
        }
    }

    context.diagnostics.Sort(context.sources);
    if (!AppendDiagnostics(text, context.sources, context.diagnostics)) {
        return ExitError;
    }

    text.Append(lines.View());
    if (broken != 0) {
        text.AppendFormat("{} errors in {} files (not formatted)\n", context.diagnostics.Count(),
                          broken);
    }

    if (context.arguments.check) {
        text.AppendFormat("{} of {} files need formatting\n", changed, order.Size());
    } else {
        text.AppendFormat("formatted {} of {} files\n", changed, order.Size());
    }

    if (lines.HasFailed()) {
        return ExitError;
    }

    return broken != 0 || (context.arguments.check && changed != 0) ? ExitFailure : ExitSuccess;
}

} // namespace rocks::sparks::tools::detail
