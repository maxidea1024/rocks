#include "Workspace.h"
#include <Core/Algorithms.h>
#include <Core/Move.h>
#include <Core/Text/Path.h>
#include <SparksCompiler/PackageManifest.h>

namespace rocks::sparks::tools::detail {

namespace {

constexpr StringView SchemaExtension = ".sparkbind.json";

// 폴더 항목 하나다(열거 결과를 이름 순으로 정렬해 쓴다).
struct Entry {
    String name;
    bool directory = false;
};

struct ListContext {
    Allocator* allocator = nullptr;
    Array<Entry>* entries = nullptr;
    ErrorCode error = ErrorCode::None;
};

bool CollectEntry(const platform::DirectoryEntry& item, void* context) noexcept {
    auto& list = *static_cast<ListContext*>(context);
    Entry entry{String(*list.allocator), item.info.type == platform::FileType::Directory};
    if (auto assigned = entry.name.TryAssign(StringView(item.name, item.nameBytes)); !assigned) {
        list.error = assigned.Error();
        return false;
    }

    if (auto pushed = list.entries->TryPushBack(Move(entry)); !pushed) {
        list.error = pushed.Error();
        return false;
    }

    return true;
}

bool EntryLess(const Entry& left, const Entry& right) noexcept {
    return StringView::CompareBytes(left.name.View(), right.name.View()) < 0;
}

// 폴더 항목을 이름 byte 순으로 모은다. 값 false는 파일 오류(status), Result 실패는 할당 실패다.
Result<bool> List(Allocator& allocator, platform::FileSystem& files, StringView path,
                  Array<Entry>& entries, platform::FileStatus& status) noexcept {
    ListContext context{&allocator, &entries, ErrorCode::None};
    status = files.EnumerateDirectory(path.Data(), path.ByteCount(), CollectEntry, &context);
    if (context.error != ErrorCode::None) {
        return Result<bool>::Failure(context.error);
    }

    if (!status) {
        return Result<bool>::Success(false);
    }

    algo::Sort(entries.View(), EntryLess);
    return Result<bool>::Success(true);
}

bool HasExtension(StringView name, StringView extension) noexcept {
    return name.ByteCount() > extension.ByteCount() &&
           name.EndsWith(extension, StringComparison::CaseSensitive);
}

// 진단 경로 표기다: `\`를 `/`로, 끝의 `/`는 뗀다(뿌리 `/`는 둔다).
Result<void> Display(StringView path, String& out) noexcept {
    usize count = path.ByteCount();
    while (count > 1 && (path.Data()[count - 1] == '/' || path.Data()[count - 1] == '\\')) {
        --count;
    }

    out.Clear();
    for (usize at = 0; at < count; ++at) {
        const char c = path.Data()[at] == '\\' ? '/' : path.Data()[at];
        if (auto appended = out.TryAppend(StringView(&c, 1)); !appended) {
            return appended;
        }
    }

    return Result<void>::Success();
}

// left와 right를 `/`로 잇는다(left가 비면 right).
Result<void> Join(StringView left, StringView right, String& out) noexcept {
    out.Clear();
    if (!left.IsEmpty()) {
        if (auto appended = out.TryAppend(left); !appended) {
            return appended;
        }

        if (!left.EndsWith("/") && !left.EndsWith("\\")) {
            if (auto appended = out.TryAppend("/"); !appended) {
                return appended;
            }
        }
    }

    return out.TryAppend(right);
}

} // namespace

Workspace::Workspace(Allocator& allocator) noexcept
    : allocator_(&allocator), files_(allocator), roots_(allocator), rootDisplays_(allocator) {
}

Result<bool> Workspace::ReadInto(platform::FileSystem& files, StringView full, WorkspaceFile& file,
                                 StringBuilderBase& problem, bool& ioFailure) noexcept {
    platform::File handle;
    StringView reason;
    if (auto opened =
            handle.Open(files, full.Data(), full.ByteCount(), platform::FileOpenMode::ReadExisting);
        !opened) {
        reason = "cannot open the file";
    } else {
        uint64 size = 0;
        if (!handle.Size(size)) {
            reason = "cannot read the file size";
        } else if (size > MaxWorkspaceFileBytes) {
            size = MaxWorkspaceFileBytes; // 원문 상한 초과는 컴파일러가 S0103으로 알린다
        }

        if (reason.IsEmpty()) {
            if (auto resized = file.text.TryResize(static_cast<usize>(size)); !resized) {
                static_cast<void>(handle.Close());
                return Result<bool>::Failure(resized.Error());
            }

            const platform::FileTransfer read =
                handle.ReadExact(file.text.Data(), file.text.Size());
            if (!read.status || read.bytes != file.text.Size()) {
                reason = "cannot read the file";
            }
        }

        if (!handle.Close() && reason.IsEmpty()) {
            reason = "cannot close the file";
        }
    }

    if (reason.IsEmpty()) {
        return Result<bool>::Success(true);
    }

    ioFailure = true;
    if (auto appended = problem.TryAppendFormat("{}: {}", file.path.View(), reason); !appended) {
        return Result<bool>::Failure(appended.Error());
    }

    return Result<bool>::Success(false);
}

Result<bool> Workspace::Walk(platform::FileSystem& files, StringView full, StringView display,
                             uint32 depth, uint32 package, StringBuilderBase& problem,
                             bool& ioFailure) noexcept {
    Array<Entry> entries(*allocator_);
    platform::FileStatus status;
    auto listed = List(*allocator_, files, full, entries, status);
    if (!listed) {
        return Result<bool>::Failure(listed.Error());
    }

    if (!listed.Value()) {
        ioFailure = true;
        if (auto appended = problem.TryAppendFormat("{}: cannot list the folder", display);
            !appended) {
            return Result<bool>::Failure(appended.Error());
        }

        return Result<bool>::Success(false);
    }

    // 이 폴더에 manifest가 있으면 새 패키지 뿌리다(이미 읽은 뿌리면 건너뛴다).
    bool root = false;
    for (const Entry& entry : entries) {
        root = root || (!entry.directory && entry.name.View() == ManifestFileName);
    }

    if (root) {
        for (const String& known : roots_) {
            if (known.View() == full) {
                return Result<bool>::Success(true);
            }
        }

        String rootPath(*allocator_);
        String rootDisplay(*allocator_);
        if (!rootPath.TryAssign(full) || !rootDisplay.TryAssign(display) ||
            !roots_.TryPushBack(Move(rootPath)) || !rootDisplays_.TryPushBack(Move(rootDisplay))) {
            return Result<bool>::Failure(ErrorCode::OutOfMemory);
        }

        package = static_cast<uint32>(roots_.Size() - 1);
    }

    bool ids = false;
    for (const Entry& entry : entries) {
        String childFull(*allocator_);
        String childDisplay(*allocator_);
        if (!Join(full, entry.name.View(), childFull) ||
            !Join(display, entry.name.View(), childDisplay)) {
            return Result<bool>::Failure(ErrorCode::OutOfMemory);
        }

        if (entry.directory) {
            if (entry.name.View().StartsWith(".")) {
                continue; // 숨은 폴더(`.git`·`.vs` 등)
            }

            if (depth + 1 > MaxWorkspaceDepth) {
                if (auto appended =
                        problem.TryAppendFormat("{}: folders are nested too deeply", display);
                    !appended) {
                    return Result<bool>::Failure(appended.Error());
                }

                return Result<bool>::Success(false);
            }

            auto walked = Walk(files, childFull.View(), childDisplay.View(), depth + 1, package,
                               problem, ioFailure);
            if (!walked || !walked.Value()) {
                return walked;
            }

            continue;
        }

        WorkspaceFileKind kind = WorkspaceFileKind::Module;
        if (root && entry.name.View() == ManifestFileName) {
            kind = WorkspaceFileKind::Manifest;
        } else if (root && HasExtension(entry.name.View(), IdsExtension)) {
            if (ids) {
                if (auto appended = problem.TryAppendFormat(
                        "{}: a package has more than one ID manifest", childDisplay.View());
                    !appended) {
                    return Result<bool>::Failure(appended.Error());
                }

                return Result<bool>::Success(false);
            }

            ids = true;
            kind = WorkspaceFileKind::Ids;
        } else if (!HasExtension(entry.name.View(), SourceExtension)) {
            continue;
        }

        if (package == NoPackage) {
            if (auto appended = problem.TryAppendFormat(
                    "{}: the file is outside every package (no SparksPackage.json5 above it)",
                    childDisplay.View());
                !appended) {
                return Result<bool>::Failure(appended.Error());
            }

            return Result<bool>::Success(false);
        }

        WorkspaceFile file(*allocator_);
        file.kind = kind;
        file.package = package;
        if (!file.path.TryAssign(childDisplay.View()) || !file.full.TryAssign(childFull.View())) {
            return Result<bool>::Failure(ErrorCode::OutOfMemory);
        }

        if (kind == WorkspaceFileKind::Module) {
            // 패키지 뿌리 기준 경로(진단 경로에서 뿌리 표기와 `/`를 뗀다).
            const StringView rootDisplay = rootDisplays_[package].View();
            const usize skip = rootDisplay.IsEmpty() ? 0 : rootDisplay.ByteCount() + 1;
            if (!file.modulePath.TryAssign(
                    StringView(childDisplay.Data() + skip, childDisplay.ByteCount() - skip))) {
                return Result<bool>::Failure(ErrorCode::OutOfMemory);
            }
        }

        auto read = ReadInto(files, childFull.View(), file, problem, ioFailure);
        if (!read || !read.Value()) {
            return read;
        }

        if (auto pushed = files_.TryPushBack(Move(file)); !pushed) {
            return Result<bool>::Failure(pushed.Error());
        }
    }

    return Result<bool>::Success(true);
}

Result<bool> Workspace::LoadPath(platform::FileSystem& files, StringView path,
                                 StringBuilderBase& problem, bool& ioFailure) noexcept {
    String display(*allocator_);
    if (auto made = Display(path, display); !made) {
        return Result<bool>::Failure(made.Error());
    }

    platform::FileInfo info;
    if (!files.Stat(path.Data(), path.ByteCount(), info) ||
        (info.type != platform::FileType::Directory &&
         !(info.type == platform::FileType::File && HasExtension(path, SourceExtension)))) {
        if (auto appended =
                problem.TryAppendFormat("{}: not a folder or a .spark file", display.View());
            !appended) {
            return Result<bool>::Failure(appended.Error());
        }

        return Result<bool>::Success(false);
    }

    // 그 자신(폴더)이나 조상 폴더에 manifest가 있으면 그 패키지 뿌리부터 읽는다.
    const bool file = info.type == platform::FileType::File;
    String folder(*allocator_);
    if (file) {
        auto parent = Path::Parent(path);
        if (!parent ||
            !folder.TryAssign(parent.Value().IsEmpty() ? StringView(".") : parent.Value())) {
            return Result<bool>::Failure(ErrorCode::OutOfMemory);
        }
    } else if (!folder.TryAssign(path)) {
        return Result<bool>::Failure(ErrorCode::OutOfMemory);
    }

    String root(*allocator_);
    for (uint32 depth = 0; depth < MaxWorkspaceDepth && !folder.IsEmpty(); ++depth) {
        String manifest(*allocator_);
        if (!Join(folder.View(), ManifestFileName, manifest)) {
            return Result<bool>::Failure(ErrorCode::OutOfMemory);
        }

        bool exists = false;
        if (files.FileExists(manifest.Data(), manifest.ByteCount(), exists) && exists) {
            if (!root.TryAssign(folder.View())) {
                return Result<bool>::Failure(ErrorCode::OutOfMemory);
            }

            break;
        }

        auto parent = Path::Parent(folder.View());
        if (!parent || parent.Value().ByteCount() == folder.ByteCount()) {
            break;
        }

        String next(*allocator_);
        if (!next.TryAssign(parent.Value())) {
            return Result<bool>::Failure(ErrorCode::OutOfMemory);
        }

        folder = Move(next);
    }

    // 진단 경로는 찾은 뿌리 표기 + 뿌리 아래 경로다(뿌리는 인자 경로에서 부모를 거슬러 만들었으므로
    // 파일 인자의 진단 경로는 인자 표기와 같다).
    StringView walkFull = root.IsEmpty() ? path : root.View();
    String walkDisplay(*allocator_);
    if (auto made = Display(walkFull, walkDisplay); !made) {
        return Result<bool>::Failure(made.Error());
    }

    // 지금 폴더(`.`)가 뿌리면 진단 경로에 `./`를 붙이지 않는다(인자 `Main.spark`와 맞게).
    if (walkDisplay.View() == ".") {
        walkDisplay.Clear();
    }

    auto walked = Walk(files, walkFull, walkDisplay.View(), 0, NoPackage, problem, ioFailure);
    if (!walked || !walked.Value()) {
        return walked;
    }

    // 파일 인자: 그 모듈을 고른다(이미 읽은 패키지면 기존 항목에서 찾는다).
    if (file) {
        bool found = false;
        for (WorkspaceFile& entry : files_) {
            if (entry.kind == WorkspaceFileKind::Module && entry.path.View() == display.View()) {
                entry.selected = true;
                found = true;
            }
        }

        if (!found) {
            if (auto appended = problem.TryAppendFormat("{}: the file is outside every package",
                                                        display.View());
                !appended) {
                return Result<bool>::Failure(appended.Error());
            }

            return Result<bool>::Success(false);
        }
    }

    return Result<bool>::Success(true);
}

Result<bool> Workspace::Load(platform::FileSystem& files, Span<const StringView> paths,
                             Span<const StringView> schemaFolders, StringBuilderBase& problem,
                             bool& ioFailure) noexcept {
    ioFailure = false;
    for (const StringView path : paths) {
        auto loaded = LoadPath(files, path, problem, ioFailure);
        if (!loaded || !loaded.Value()) {
            return loaded;
        }
    }

    if (roots_.IsEmpty()) {
        if (auto appended = problem.TryAppend("no package found (a package folder has a "
                                              "SparksPackage.json5)");
            !appended) {
            return Result<bool>::Failure(appended.Error());
        }

        return Result<bool>::Success(false);
    }

    for (const StringView folder : schemaFolders) {
        String display(*allocator_);
        if (auto made = Display(folder, display); !made) {
            return Result<bool>::Failure(made.Error());
        }

        Array<Entry> entries(*allocator_);
        platform::FileStatus status;
        auto listed = List(*allocator_, files, folder, entries, status);
        if (!listed) {
            return Result<bool>::Failure(listed.Error());
        }

        if (!listed.Value()) {
            ioFailure = true;
            if (auto appended =
                    problem.TryAppendFormat("{}: cannot list the schema folder", display.View());
                !appended) {
                return Result<bool>::Failure(appended.Error());
            }

            return Result<bool>::Success(false);
        }

        for (const Entry& entry : entries) {
            if (entry.directory || !HasExtension(entry.name.View(), SchemaExtension)) {
                continue;
            }

            WorkspaceFile file(*allocator_);
            file.kind = WorkspaceFileKind::Schema;
            if (!Join(folder, entry.name.View(), file.full) ||
                !Join(display.View(), entry.name.View(), file.path)) {
                return Result<bool>::Failure(ErrorCode::OutOfMemory);
            }

            auto read = ReadInto(files, file.full.View(), file, problem, ioFailure);
            if (!read || !read.Value()) {
                return read;
            }

            if (auto pushed = files_.TryPushBack(Move(file)); !pushed) {
                return Result<bool>::Failure(pushed.Error());
            }
        }
    }

    return Result<bool>::Success(true);
}

uint32 Workspace::IdsFile(uint32 package) const noexcept {
    for (usize at = 0; at < files_.Size(); ++at) {
        if (files_[at].kind == WorkspaceFileKind::Ids && files_[at].package == package) {
            return static_cast<uint32>(at);
        }
    }

    return NoPackage;
}

Result<void> Workspace::AddTo(SourceSet& sources, Compilation& compilation,
                              DiagnosticList& diagnostics, Array<uint32>& modules,
                              Array<uint32>& packages, Span<const IdsOverride> ids) noexcept {
    if (auto resized = modules.TryResize(files_.Size()); !resized) {
        return resized;
    }

    for (uint32& module : modules) {
        module = NoModule;
    }

    // schema → 패키지(manifest·ID manifest) → 모듈 순(패키지 번호가 모듈보다 먼저 있어야 한다).
    if (auto resized = packages.TryResize(roots_.Size()); !resized) {
        return resized;
    }

    auto idsText = [&](uint32 package) noexcept {
        return ids.IsEmpty() ? StringView() : ids[package].text;
    };

    const WorkspaceFileKind order[] = {WorkspaceFileKind::Schema, WorkspaceFileKind::Manifest,
                                       WorkspaceFileKind::Ids, WorkspaceFileKind::Module};
    for (const WorkspaceFileKind kind : order) {
        if (kind == WorkspaceFileKind::Module) {
            // 파일이 없던 패키지의 새 ID manifest(ids 확인)다.
            for (uint32 package = 0; package < ids.Size(); ++package) {
                if (ids[package].text.IsEmpty() || IdsFile(package) != NoPackage) {
                    continue;
                }

                auto id = sources.TryAdd(ids[package].path, ids[package].text, diagnostics);
                if (!id) {
                    return Result<void>::Failure(id.Error());
                }

                if (auto added = compilation.AddIdManifest(packages[package], id.Value()); !added) {
                    return added;
                }
            }
        }

        for (usize at = 0; at < files_.Size(); ++at) {
            const WorkspaceFile& file = files_[at];
            if (file.kind != kind) {
                continue;
            }

            const StringView replaced =
                kind == WorkspaceFileKind::Ids ? idsText(file.package) : StringView();
            auto id = sources.TryAdd(
                file.path.View(),
                replaced.IsEmpty() ? StringView(file.text.Data(), file.text.Size()) : replaced,
                diagnostics);
            if (!id) {
                return Result<void>::Failure(id.Error());
            }

            switch (kind) {
            case WorkspaceFileKind::Schema:
                if (auto added = compilation.AddBindingSchema(id.Value()); !added) {
                    return added;
                }

                break;
            case WorkspaceFileKind::Manifest: {
                auto package = compilation.AddPackage(id.Value());
                if (!package) {
                    return Result<void>::Failure(package.Error());
                }

                packages[file.package] = package.Value();
                break;
            }
            case WorkspaceFileKind::Ids:
                if (auto added = compilation.AddIdManifest(packages[file.package], id.Value());
                    !added) {
                    return added;
                }

                break;
            case WorkspaceFileKind::Module: {
                auto module = compilation.AddModule(packages[file.package], id.Value(),
                                                    file.modulePath.View());
                if (!module) {
                    return Result<void>::Failure(module.Error());
                }

                modules[at] = module.Value();
                break;
            }
            }
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::sparks::tools::detail
