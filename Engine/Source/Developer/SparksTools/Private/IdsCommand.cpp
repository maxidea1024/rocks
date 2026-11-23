#include <Core/Algorithms.h>
#include <Core/Move.h>
#include <Core/Text/Path.h>
#include "CommandInternal.h"
#include "IdManifest.h"
#include "ToolFiles.h"

// `sparks ids`다(Docs/Design/SparksTools.ko.md T8·7.2절): 발급 대기 ID를 패키지 ID manifest에 발급·
// 이름 변경하고, 새 manifest로 다시 컴파일해 확인한 뒤 C# 도구와 같은 잠금·교체 규약으로 쓴다.
namespace rocks::sparks::tools::detail {

namespace {

constexpr StringView LockSuffix = ".lock";

// 패키지 하나의 편집 상태다.
struct PackageEdit {
    explicit PackageEdit(Allocator& allocator) noexcept
        : entries(allocator), text(allocator), path(allocator), display(allocator) {
    }

    Array<IdEntry> entries;
    String text;    // 새 manifest 원문
    String path;    // 파일 시스템 경로
    String display; // 진단 경로
    bool touched = false;
    uint32 added = 0;
    uint32 renamed = 0;
};

// 이름 키의 패키지(첫 `/` 앞 이름)를 작업 공간 패키지 번호로 찾는다. 없으면 NoPackage.
uint32 PackageOf(const CommandContext& context, StringView key) noexcept {
    usize slash = 0;
    while (slash < key.ByteCount() && key.Data()[slash] != '/') {
        ++slash;
    }

    const StringView name(key.Data(), slash);
    for (uint32 package = 0; package < context.workspace.PackageCount(); ++package) {
        if (context.compilation.Manifest(context.packages[package]).Name() == name) {
            return package;
        }
    }

    return NoPackage;
}

// 편집할 패키지마다 기존 항목과 대상 경로를 채운다. 값은 계속하면 ExitSuccess, 아니면 종료 코드.
int32 PrepareEdits(CommandContext& context, Array<PackageEdit>& edits) noexcept {
    const Span<const WorkspaceFile> files = context.workspace.Files();
    for (uint32 index = 0; index < context.compilation.PendingIdCount(); ++index) {
        const uint32 package = PackageOf(context, context.compilation.PendingIdAt(index).name);
        if (package == NoPackage) {
            return ReportProblem(context, StringView(),
                                 "a pending ID names a package that is not loaded", ExitFailure);
        }

        edits[package].touched = true;
    }

    for (uint32 package = 0; package < edits.Size(); ++package) {
        PackageEdit& edit = edits[package];
        if (!edit.touched) {
            continue;
        }

        const uint32 ids = context.workspace.IdsFile(package);
        if (ids != NoPackage) {
            const WorkspaceFile& file = files[ids];
            auto read = ReadIdEntries(context.allocator,
                                      StringView(file.text.Data(), file.text.Size()), edit.entries);
            if (!read) {
                return ExitError;
            }

            if (!read.Value()) {
                return ReportProblem(context, file.path.View(), "cannot read the ID manifest",
                                     ExitFailure);
            }

            if (!edit.path.TryAssign(file.full.View()) ||
                !edit.display.TryAssign(file.path.View())) {
                return ExitError;
            }

            continue;
        }

        // 새 manifest: 패키지 뿌리의 `<패키지 이름>.rocksids.json`.
        String name(context.allocator);
        if (!name.TryAppend(context.compilation.Manifest(context.packages[package]).Name()) ||
            !name.TryAppend(IdsExtension)) {
            return ExitError;
        }

        auto full =
            Path::Combine(context.allocator, context.workspace.RootPath(package), name.View());
        if (!full) {
            return ExitError;
        }

        edit.path = Move(full.Value());
        const StringView root = context.workspace.RootDisplay(package);
        if ((!root.IsEmpty() && (!edit.display.TryAppend(root) || !edit.display.TryAppend("/"))) ||
            !edit.display.TryAppend(name.View())) {
            return ExitError;
        }
    }

    return ExitSuccess;
}

// 대기를 세 차례로 적용한다: type rename(멤버 승계), 멤버 rename, 새 발급. 값은 계속하면
// ExitSuccess, 아니면 종료 코드.
int32 ApplyPending(CommandContext& context, Array<PackageEdit>& edits) noexcept {
    const Compilation& compilation = context.compilation;
    for (uint32 pass = 0; pass < 3; ++pass) {
        for (uint32 index = 0; index < compilation.PendingIdCount(); ++index) {
            const PendingId pending = compilation.PendingIdAt(index);
            const bool type = pending.kind == "type";
            const bool rename = !pending.formerName.IsEmpty();
            const uint32 wanted = rename ? (type ? 0 : 1) : 2;
            if (wanted != pass) {
                continue;
            }

            PackageEdit& edit = edits[PackageOf(context, pending.name)];
            if (rename) {
                auto renamed = RenameIdEntry(context.allocator, edit.entries, pending.kind,
                                             pending.formerName, pending.name);
                if (!renamed) {
                    return ExitError;
                }

                if (!renamed.Value()) {
                    StringBuilder<OutputInlineBytes> message(context.allocator);
                    message.AppendFormat("cannot rename {} {} to {}", pending.kind,
                                         pending.formerName, pending.name);
                    if (message.HasFailed()) {
                        return ExitError;
                    }

                    return ReportProblem(context, edit.display.View(), message.View(), ExitFailure);
                }

                ++edit.renamed;
                continue;
            }

            StringBuilder<GuidDigits> id(context.allocator);
            if (!AppendNewGuidDigits(id)) {
                return ExitError;
            }

            auto added =
                AddIdEntry(context.allocator, edit.entries, pending.kind, pending.name, id.View());
            if (!added) {
                return ExitError;
            }

            if (!added.Value()) {
                StringBuilder<OutputInlineBytes> message(context.allocator);
                message.AppendFormat("cannot add {} {}: the name is taken", pending.kind,
                                     pending.name);
                if (message.HasFailed()) {
                    return ExitError;
                }

                return ReportProblem(context, edit.display.View(), message.View(), ExitFailure);
            }

            ++edit.added;
        }
    }

    for (PackageEdit& edit : edits) {
        if (!edit.touched) {
            continue;
        }

        StringBuilder<OutputInlineBytes> text(context.allocator);
        if (!AppendIdManifest(context.allocator, text, edit.entries.View()) ||
            !edit.text.TryAssign(text.View())) {
            return ExitError;
        }

        if (edit.text.ByteCount() > MaxIdManifestBytes) {
            return ReportProblem(context, edit.display.View(),
                                 "the ID manifest would exceed 16 MiB", ExitFailure);
        }
    }

    return ExitSuccess;
}

// 새 manifest로 작업 공간을 다시 컴파일해 진단·대기가 없는지 본다.
int32 Verify(CommandContext& context, const Array<PackageEdit>& edits) noexcept {
    Array<Workspace::IdsOverride> overrides(context.allocator);
    for (const PackageEdit& edit : edits) {
        Workspace::IdsOverride entry;
        if (edit.touched) {
            entry.text = edit.text.View();
            entry.path = edit.display.View();
        }

        if (!overrides.TryPushBack(entry)) {
            return ExitError;
        }
    }

    SourceSet sources(context.allocator);
    DiagnosticList diagnostics(context.allocator);
    Compilation compilation(context.allocator, sources);
    Array<uint32> modules(context.allocator);
    Array<uint32> packages(context.allocator);
    if (!context.workspace.AddTo(sources, compilation, diagnostics, modules, packages,
                                 overrides.View()) ||
        !compilation.Check(diagnostics)) {
        return ExitError;
    }

    if (diagnostics.Count() != 0 || compilation.PendingIdCount() != 0) {
        // 원인을 보인다: 새 manifest로 컴파일한 진단과 남은 대기(내부 불일치 — 쓰지 않는다).
        diagnostics.Sort(sources);
        StringBuilder<OutputInlineBytes> text(context.allocator);
        text.Append("sparks: issuing the IDs did not settle the packages\n");
        if (!AppendDiagnostics(text, sources, diagnostics)) {
            return ExitError;
        }

        for (uint32 index = 0; index < compilation.PendingIdCount(); ++index) {
            const PendingId pending = compilation.PendingIdAt(index);
            text.AppendFormat("still pending: {} {}\n", pending.kind, pending.name);
        }

        if (text.HasFailed() || !context.output.WriteError(text.View())) {
            return ExitError;
        }

        return ExitFailure;
    }

    return ExitSuccess;
}

// manifest 하나를 C# StableIdManifest.Update와 같은 규약으로 쓴다: `<대상>.lock` CreateNew → 적재
// 때 byte와 같은지(없던 파일은 여전히 없는지) → 임시 파일 교체 → 잠금 해제.
int32 WriteManifest(CommandContext& context, const PackageEdit& edit, uint32 package) noexcept {
    String lockPath(context.allocator);
    if (!lockPath.TryAppend(edit.path.View()) || !lockPath.TryAppend(LockSuffix)) {
        return ExitError;
    }

    platform::File lock;
    const platform::FileStatus locked = lock.Open(
        context.files, lockPath.Data(), lockPath.ByteCount(), platform::FileOpenMode::CreateNew);
    if (!locked) {
        if (locked.code == platform::FileCode::AlreadyExists) {
            return ReportProblem(context, lockPath.View(),
                                 "the manifest is locked by another tool (delete the lock file "
                                 "if no tool is running)",
                                 ExitFailure);
        }

        return ReportProblem(context, lockPath.View(), "cannot create the lock file", ExitError);
    }

    int32 code = ExitSuccess;
    const uint32 ids = context.workspace.IdsFile(package);
    Span<const uint8> before;
    if (ids != NoPackage) {
        const WorkspaceFile& file = context.workspace.Files()[ids];
        before =
            Span<const uint8>(reinterpret_cast<const uint8*>(file.text.Data()), file.text.Size());
    }

    auto compared = CompareFile(context.allocator, context.files, edit.path.View(), before);
    if (!compared) {
        code = ExitError;
    } else if (!compared.Value().status) {
        code = ReportProblem(context, edit.path.View(), "cannot read the file", ExitError);
    } else if (compared.Value().exists != (ids != NoPackage) ||
               (ids != NoPackage && !compared.Value().same)) {
        code = ReportProblem(context, edit.display.View(),
                             "the manifest changed while the IDs were issued", ExitFailure);
    } else {
        auto replaced = ReplaceFile(context.allocator, context.files, edit.path.View(),
                                    BytesOf(edit.text.View()));
        if (!replaced) {
            code = ExitError;
        } else if (!replaced.Value()) {
            code = ReportProblem(context, edit.path.View(),
                                 replaced.Value().code == platform::FileCode::NotDurable
                                     ? NotDurableMessage
                                     : StringView("cannot write the file"),
                                 ExitError);
        }
    }

    // 잠금 해제(실패 경로 포함).
    const bool closed = static_cast<bool>(lock.Close());
    const bool deleted =
        static_cast<bool>(context.files.Delete(lockPath.Data(), lockPath.ByteCount()));
    if ((!closed || !deleted) && code == ExitSuccess) {
        code = ReportProblem(context, lockPath.View(), "cannot delete the lock file", ExitError);
    }

    return code;
}

} // namespace

int32 RunIds(CommandContext& context, StringBuilderBase& text) noexcept {
    AppendSummary(text, context);
    if (context.diagnostics.Count() != 0) {
        return ExitFailure; // 오류가 있는 컴파일에서는 발급하지 않는다(SparksBytecode 6.1절)
    }

    if (context.compilation.PendingIdCount() == 0) {
        text.Append("no pending IDs\n");
        return ExitSuccess;
    }

    Array<PackageEdit> edits(context.allocator);
    for (uint32 package = 0; package < context.workspace.PackageCount(); ++package) {
        if (!edits.TryPushBack(PackageEdit(context.allocator))) {
            return ExitError;
        }
    }

    if (const int32 code = PrepareEdits(context, edits); code != ExitSuccess) {
        return code;
    }

    if (const int32 code = ApplyPending(context, edits); code != ExitSuccess) {
        return code;
    }

    if (const int32 code = Verify(context, edits); code != ExitSuccess) {
        return code;
    }

    // manifest 진단 경로 byte 순으로 쓴다(출력 순서가 인자 순서와 무관 — T4).
    Array<uint32> order(context.allocator);
    for (uint32 package = 0; package < edits.Size(); ++package) {
        if (edits[package].touched && !order.TryPushBack(package)) {
            return ExitError;
        }
    }

    algo::Sort(order.View(), [&](uint32 left, uint32 right) noexcept {
        return StringView::CompareBytes(edits[left].display.View(), edits[right].display.View()) <
               0;
    });
    for (const uint32 package : order) {
        const PackageEdit& edit = edits[package];
        if (const int32 code = WriteManifest(context, edit, package); code != ExitSuccess) {
            return code;
        }

        text.AppendFormat("{}: {} new, {} renamed\n", edit.display.View(), edit.added,
                          edit.renamed);
    }

    return ExitSuccess;
}

} // namespace rocks::sparks::tools::detail
