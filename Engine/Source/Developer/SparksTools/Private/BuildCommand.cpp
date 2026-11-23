#include <Core/Text/Path.h>
#include <SparksVm/BytecodeFile.h>
#include "CommandInternal.h"
#include "ToolFiles.h"

// `sparks build`다(Docs/Design/SparksTools.ko.md T7·7.1절): 진단 없는 컴파일의 모듈마다 `.sparkc`를
// 출력 폴더에 원자적으로 쓴다(내용이 같으면 쓰지 않는다).
namespace rocks::sparks::tools::detail {

namespace {

constexpr StringView BytecodeExtension = ".sparkc";

} // namespace

int32 RunBuild(CommandContext& context, StringBuilderBase& text) noexcept {
    AppendSummary(text, context);
    if (context.diagnostics.Count() != 0) {
        return ExitFailure; // 진단이 있으면 아무것도 쓰지 않는다
    }

    Array<usize> order(context.allocator);
    if (!ModuleOrder(context.workspace, false, order)) {
        return ExitError;
    }

    const Span<const WorkspaceFile> files = context.workspace.Files();
    Array<uint8> bytes(context.allocator);
    uint32 written = 0;
    for (const usize at : order) {
        const WorkspaceFile& file = files[at];
        const BytecodeModule* module = context.compilation.ModuleBytecode(context.modules[at]);
        if (module == nullptr) {
            return ExitError; // 진단 없는 컴파일의 모듈은 모두 bytecode가 있다
        }

        bytes.Clear();
        if (auto encoded = WriteBytecodeFile(*module, bytes); !encoded) {
            return encoded.Error() == ErrorCode::OutOfMemory
                       ? ExitError
                       : ReportProblem(context, file.path.View(),
                                       "the module is too large for a .sparkc file", ExitError);
        }

        // `<출력>/<패키지 이름>/<모듈 경로의 .spark를 .sparkc로>`.
        const StringView modulePath = file.modulePath.View();
        String relative(context.allocator);
        if (!relative.TryAppend(
                context.compilation.Manifest(context.packages[file.package]).Name()) ||
            !relative.TryAppend("/") ||
            !relative.TryAppend(StringView(modulePath.Data(),
                                           modulePath.ByteCount() - SourceExtension.ByteCount())) ||
            !relative.TryAppend(BytecodeExtension)) {
            return ExitError;
        }

        auto target = Path::Combine(context.allocator, context.arguments.out, relative.View());
        if (!target) {
            return target.Error() == ErrorCode::OutOfMemory
                       ? ExitError
                       : ReportProblem(context, context.arguments.out,
                                       "cannot use the output folder", ExitError);
        }

        const StringView targetPath = target.Value().View();
        auto folder = Path::Parent(targetPath);
        if (!folder) {
            return folder.Error() == ErrorCode::OutOfMemory
                       ? ExitError
                       : ReportProblem(context, context.arguments.out,
                                       "cannot use the output folder", ExitError);
        }

        if (!folder.Value().IsEmpty() &&
            !platform::MakeDirectoryTree(context.files, folder.Value().Data(),
                                         folder.Value().ByteCount())) {
            return ReportProblem(context, folder.Value(), "cannot create the folder", ExitError);
        }

        auto compared = CompareFile(context.allocator, context.files, targetPath, bytes.View());
        if (!compared) {
            return ExitError;
        }

        if (!compared.Value().status) {
            return ReportProblem(context, targetPath, "cannot read the file", ExitError);
        }

        if (compared.Value().same) {
            continue;
        }

        auto replaced = ReplaceFile(context.allocator, context.files, targetPath, bytes.View());
        if (!replaced) {
            return ExitError;
        }

        if (!replaced.Value()) {
            return ReportProblem(context, targetPath,
                                 replaced.Value().code == platform::FileCode::NotDurable
                                     ? NotDurableMessage
                                     : StringView("cannot write the file"),
                                 ExitError);
        }

        ++written;
    }

    text.AppendFormat("wrote {} of {} modules\n", written, order.Size());
    return ExitSuccess;
}

} // namespace rocks::sparks::tools::detail
