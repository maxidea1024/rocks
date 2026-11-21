#include <Core/Algorithms.h>
#include <Core/Move.h>
#include <Core/Text/Path.h>
#include <SparksCompiler/PackageManifest.h>

#include "ConformanceInternal.h"

// corpus·schema 폴더 탐색과 사례 짝 맞추기다(Docs/Design/SparksLanguage.ko.md 13.1절).

namespace rocks::sparks::conformance {

namespace {

// 폴더 사례 안 폴더 중첩의 상한이다(corpus는 우리가 쓰지만 끝없는 재귀를 막는다).
constexpr uint32 MaxCaseFolderDepth = 8;

// 디렉터리 항목 하나다(열거 결과를 이름 순으로 정렬해 쓴다).
struct DirectoryItem {
    String name;
    bool directory = false;
};

struct ListContext {
    Allocator* allocator = nullptr;
    Array<DirectoryItem>* items = nullptr;
    ErrorCode error = ErrorCode::None;
};

bool CollectItem(const platform::DirectoryEntry& entry, void* context) noexcept {
    auto& list = *static_cast<ListContext*>(context);
    DirectoryItem item{String(*list.allocator), entry.info.type == platform::FileType::Directory};
    if (auto assigned = item.name.TryAssign(StringView(entry.name, entry.nameBytes)); !assigned) {
        list.error = assigned.Error();
        return false;
    }

    if (auto pushed = list.items->TryPushBack(Move(item)); !pushed) {
        list.error = pushed.Error();
        return false;
    }

    return true;
}

bool ItemLess(const DirectoryItem& left, const DirectoryItem& right) noexcept {
    return StringView::CompareBytes(left.name.View(), right.name.View()) < 0;
}

// 폴더 항목을 이름 순으로 모은다. Result 실패는 할당 실패, 값 false는 파일 오류(status에 원인).
Result<bool> ListDirectory(Allocator& allocator, platform::FileSystem& files, StringView path,
                           Array<DirectoryItem>& items, platform::FileStatus& status) noexcept {
    ListContext context{&allocator, &items, ErrorCode::None};
    status = files.EnumerateDirectory(path.Data(), path.ByteCount(), CollectItem, &context);
    if (context.error != ErrorCode::None) {
        return Result<bool>::Failure(context.error);
    }

    if (!status) {
        return Result<bool>::Success(false);
    }

    algo::Sort(items.View(), ItemLess);
    return Result<bool>::Success(true);
}

// 파일 전체를 읽는다. Result 실패는 할당 실패, 값 false는 읽지 못한 것(problem에 이유).
Result<bool> ReadWholeFile(platform::FileSystem& files, StringView path, Array<char>& out,
                           StringBuilderBase& problem) noexcept {
    platform::File file;
    if (auto opened =
            file.Open(files, path.Data(), path.ByteCount(), platform::FileOpenMode::ReadExisting);
        !opened) {
        if (auto appended = problem.TryAppend("cannot open the file"); !appended) {
            return Result<bool>::Failure(appended.Error());
        }

        return Result<bool>::Success(false);
    }

    StringView reason;
    uint64 size = 0;
    if (!file.Size(size)) {
        reason = "cannot read the file size";
    } else if (size > detail::MaxCorpusFileBytes) {
        reason = "the file is larger than a corpus file may be";
    } else if (auto resized = out.TryResize(static_cast<usize>(size)); !resized) {
        static_cast<void>(file.Close());
        return Result<bool>::Failure(resized.Error());
    } else {
        const platform::FileTransfer read = file.ReadExact(out.Data(), out.Size());
        if (!read.status || read.bytes != out.Size()) {
            reason = "cannot read the file";
        }
    }

    if (!file.Close() && reason.IsEmpty()) {
        reason = "cannot close the file";
    }

    if (reason.IsEmpty()) {
        return Result<bool>::Success(true);
    }

    if (auto appended = problem.TryAppend(reason); !appended) {
        return Result<bool>::Failure(appended.Error());
    }

    return Result<bool>::Success(false);
}

bool HasExtension(StringView name, StringView extension) noexcept {
    return name.ByteCount() > extension.ByteCount() &&
           name.EndsWith(extension, StringComparison::CaseSensitive);
}

Result<void> AddLayoutFailure(Report& report, Allocator& allocator, StringView areaName,
                              StringView name, StringView reason) noexcept {
    StringBuilder<detail::MessageInlineBytes> message(allocator);
    if (auto formatted = message.TryAppendFormat("{}/{}: {}", areaName, name, reason); !formatted) {
        return formatted;
    }

    return report.TryAddFailure(message.View());
}

// 읽은 파일 하나다(경로는 진단 경로, 내용은 bytes).
struct LoadedFile {
    String path;
    Array<char> bytes;
};

// 파일 하나를 읽어 files에 더한다. 읽기 실패는 failureName과 이유로 기록하고 false다.
Result<bool> LoadFile(Allocator& allocator, platform::FileSystem& files, StringView fullPath,
                      StringView path, StringView areaName, StringView failureName,
                      Array<LoadedFile>& loaded, Report& report) noexcept {
    LoadedFile file{String(allocator), Array<char>(allocator)};
    if (auto assigned = file.path.TryAssign(path); !assigned) {
        return Result<bool>::Failure(assigned.Error());
    }

    StringBuilder<detail::MessageInlineBytes> problem(allocator);
    auto read = ReadWholeFile(files, fullPath, file.bytes, problem);
    if (!read) {
        return Result<bool>::Failure(read.Error());
    }

    if (!read.Value()) {
        if (auto added = AddLayoutFailure(report, allocator, areaName, failureName, problem.View());
            !added) {
            return Result<bool>::Failure(added.Error());
        }

        return Result<bool>::Success(false);
    }

    if (auto pushed = loaded.TryPushBack(Move(file)); !pushed) {
        return Result<bool>::Failure(pushed.Error());
    }

    return Result<bool>::Success(true);
}

// 폴더 사례의 파일을 모두 읽는다(사례 폴더 기준 경로, 이름 순). 배치 오류는 실패 항목으로 기록하고
// false다.
Result<bool> LoadFolderCase(Allocator& allocator, platform::FileSystem& files,
                            StringView folderPath, StringView relative, uint32 depth,
                            StringView areaName, StringView caseName, Array<LoadedFile>& loaded,
                            Report& report) noexcept {
    if (depth > MaxCaseFolderDepth) {
        if (auto added = AddLayoutFailure(report, allocator, areaName, caseName,
                                          "folders are nested too deeply");
            !added) {
            return Result<bool>::Failure(added.Error());
        }

        return Result<bool>::Success(false);
    }

    Array<DirectoryItem> items(allocator);
    platform::FileStatus status;
    auto listed = ListDirectory(allocator, files, folderPath, items, status);
    if (!listed) {
        return Result<bool>::Failure(listed.Error());
    }

    if (!listed.Value()) {
        if (auto added =
                AddLayoutFailure(report, allocator, areaName, caseName, "cannot list the folder");
            !added) {
            return Result<bool>::Failure(added.Error());
        }

        return Result<bool>::Success(false);
    }

    for (const DirectoryItem& item : items) {
        auto full = Path::Combine(allocator, folderPath, item.name.View());
        if (!full) {
            return Result<bool>::Failure(full.Error());
        }

        StringBuilder<detail::MessageInlineBytes> path(allocator);
        path.Append(relative).Append(item.name.View());
        if (path.HasFailed()) {
            return Result<bool>::Failure(path.Status().Error());
        }

        if (item.directory) {
            path.Append("/");
            if (path.HasFailed()) {
                return Result<bool>::Failure(path.Status().Error());
            }

            auto nested = LoadFolderCase(allocator, files, full.Value().View(), path.View(),
                                         depth + 1, areaName, caseName, loaded, report);
            if (!nested || !nested.Value()) {
                return nested;
            }

            continue;
        }

        StringBuilder<detail::MessageInlineBytes> failureName(allocator);
        failureName.Append(caseName).Append("/").Append(path.View());
        if (failureName.HasFailed()) {
            return Result<bool>::Failure(failureName.Status().Error());
        }

        auto read = LoadFile(allocator, files, full.Value().View(), path.View(), areaName,
                             failureName.View(), loaded, report);
        if (!read || !read.Value()) {
            return read;
        }
    }

    return Result<bool>::Success(true);
}

// 폴더 사례의 배치(13.1절)를 본다. 틀리면 reason에 이유를 쓰고 false다.
bool CheckFolderLayout(const Array<LoadedFile>& loaded, StringBuilderBase& reason) noexcept {
    if (loaded.IsEmpty()) {
        reason.Append("the case folder has no files");
        return false;
    }

    bool rootPackage = false;
    for (const LoadedFile& file : loaded) {
        rootPackage = rootPackage || file.path.View() == ManifestFileName;
    }

    for (const LoadedFile& file : loaded) {
        const StringView path = file.path.View();
        const usize slash = path.Find("/", StringComparison::CaseSensitive);
        if (path.EndsWith(ManifestFileName, StringComparison::CaseSensitive) &&
            (path.ByteCount() == ManifestFileName.ByteCount() ||
             path.Data()[path.ByteCount() - ManifestFileName.ByteCount() - 1] == '/')) {
            // 패키지 루트에만 둔다: 루트 패키지면 사례 루트, 아니면 "<패키지>/".
            const bool atRoot = path.ByteCount() == ManifestFileName.ByteCount();
            const bool atPackage = slash != StringView::NotFound &&
                                   slash + 1 + ManifestFileName.ByteCount() == path.ByteCount();
            if (rootPackage ? !atRoot : !atPackage) {
                reason.AppendFormat("'{}' is not at a package root", path);
                return false;
            }

            continue;
        }

        if (!HasExtension(path, detail::SourceExtension)) {
            reason.AppendFormat("unexpected file '{}' (only .spark and SparksPackage.json5)", path);
            return false;
        }

        if (!rootPackage && slash == StringView::NotFound) {
            reason.AppendFormat("'{}' is at the case root, which has no SparksPackage.json5", path);
            return false;
        }
    }

    if (!rootPackage) {
        // 패키지 폴더마다 manifest가 있어야 한다.
        for (const LoadedFile& file : loaded) {
            const StringView path = file.path.View();
            const usize slash = path.Find("/", StringComparison::CaseSensitive);
            const StringView package(path.Data(), slash);
            bool hasManifest = false;
            for (const LoadedFile& other : loaded) {
                const StringView otherPath = other.path.View();
                hasManifest =
                    hasManifest ||
                    (otherPath.ByteCount() ==
                         package.ByteCount() + 1 + ManifestFileName.ByteCount() &&
                     otherPath.StartsWith(package, StringComparison::CaseSensitive) &&
                     otherPath.EndsWith(ManifestFileName, StringComparison::CaseSensitive));
            }

            if (!hasManifest) {
                reason.AppendFormat("package folder '{}' has no SparksPackage.json5", package);
                return false;
            }
        }
    }

    return true;
}

// 사례 하나를 돈다(파일 사례 또는 폴더 사례). syntaxTreeName·namesName은 `.ast`·`.names`가 없으면
// 비어 있다.
Result<void> RunPairedCase(Allocator& allocator, platform::FileSystem& files, StringView areaPath,
                           StringView areaName, StringView stem, StringView expectName,
                           StringView sourceName, StringView folderName, StringView syntaxTreeName,
                           StringView namesName, StringView typesName, StringView irName,
                           Span<const CaseFile> schemas, Report& report) noexcept {
    Array<LoadedFile> loaded(allocator);
    // 0: .expect, 1: .spark 또는 없음, 2: .ast 또는 없음, 3: .names 또는 없음, 4: .types 또는 없음,
    // 5: .ir 또는 없음
    const StringView names[] = {expectName, sourceName, syntaxTreeName,
                                namesName,  typesName,  irName};
    bool present[] = {false, false, false, false, false, false};
    for (usize index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        if (names[index].IsEmpty()) {
            continue;
        }

        auto full = Path::Combine(allocator, areaPath, names[index]);
        if (!full) {
            return Result<void>::Failure(full.Error());
        }

        auto read = LoadFile(allocator, files, full.Value().View(), names[index], areaName,
                             names[index], loaded, report);
        if (!read) {
            return Result<void>::Failure(read.Error());
        }

        if (!read.Value()) {
            return Result<void>::Success();
        }

        present[index] = true;
    }

    auto textOf = [&loaded](StringView name) noexcept {
        for (const LoadedFile& file : loaded) {
            if (file.path.View() == name) {
                return StringView(file.bytes.Data(), file.bytes.Size());
            }
        }

        return StringView();
    };

    StringBuilder<detail::MessageInlineBytes> caseName(allocator);
    CaseInput input;
    input.expect = textOf(expectName);
    input.schemas = schemas;
    Array<LoadedFile> folderFiles(allocator);
    Array<CaseFile> caseFiles(allocator);
    if (!folderName.IsEmpty()) {
        caseName.AppendFormat("{}/{}", areaName, stem);
        if (caseName.HasFailed()) {
            return Result<void>::Failure(caseName.Status().Error());
        }

        auto folderPath = Path::Combine(allocator, areaPath, folderName);
        if (!folderPath) {
            return Result<void>::Failure(folderPath.Error());
        }

        auto read = LoadFolderCase(allocator, files, folderPath.Value().View(), "", 0, areaName,
                                   folderName, folderFiles, report);
        if (!read) {
            return Result<void>::Failure(read.Error());
        }

        if (!read.Value()) {
            return Result<void>::Success();
        }

        StringBuilder<detail::MessageInlineBytes> reason(allocator);
        if (!CheckFolderLayout(folderFiles, reason)) {
            if (reason.HasFailed()) {
                return Result<void>::Failure(reason.Status().Error());
            }

            return AddLayoutFailure(report, allocator, areaName, folderName, reason.View());
        }

        for (const LoadedFile& file : folderFiles) {
            if (auto pushed = caseFiles.TryPushBack(
                    CaseFile{file.path.View(), StringView(file.bytes.Data(), file.bytes.Size())});
                !pushed) {
                return pushed;
            }
        }

        input.folder = true;
        input.files = caseFiles.View();
    } else {
        caseName.AppendFormat("{}/{}{}", areaName, stem, detail::SourceExtension);
        if (caseName.HasFailed()) {
            return Result<void>::Failure(caseName.Status().Error());
        }

        input.source = textOf(sourceName);
        input.hasSyntaxTree = present[2];
        input.syntaxTree = textOf(syntaxTreeName);
    }

    input.hasNames = present[3];
    input.names = textOf(namesName);
    input.hasTypes = present[4];
    input.types = textOf(typesName);
    input.hasIr = present[5];
    input.ir = textOf(irName);
    input.name = caseName.View();
    return RunCase(allocator, input, report);
}

// 이름에서 확장자를 뗀 사례 이름(stem)과 그 항목 번호다.
struct CaseItem {
    StringView stem;
    usize item = 0;
};

bool CaseItemLess(const CaseItem& left, const CaseItem& right) noexcept {
    return StringView::CompareBytes(left.stem, right.stem) < 0;
}

// 영역 폴더 하나의 사례를 돈다. 짝은 stem(확장자를 뗀 이름, 폴더는 이름 그대로)으로 맞춘다.
Result<void> RunArea(Allocator& allocator, platform::FileSystem& files, StringView areaPath,
                     StringView areaName, Span<const CaseFile> schemas, Report& report) noexcept {
    Array<DirectoryItem> items(allocator);
    platform::FileStatus status;
    auto listed = ListDirectory(allocator, files, areaPath, items, status);
    if (!listed) {
        return Result<void>::Failure(listed.Error());
    }

    if (!listed.Value()) {
        StringBuilder<detail::MessageInlineBytes> message(allocator);
        if (auto formatted = message.TryAppendFormat("{}: cannot list the folder", areaName);
            !formatted) {
            return formatted;
        }

        return report.TryAddFailure(message.View());
    }

    // 0: .spark, 1: 폴더, 2: .expect, 3: .ast, 4: .names, 5: .types, 6: .ir
    constexpr usize KindCount = 7;
    Array<CaseItem> lists[KindCount] = {Array<CaseItem>(allocator), Array<CaseItem>(allocator),
                                        Array<CaseItem>(allocator), Array<CaseItem>(allocator),
                                        Array<CaseItem>(allocator), Array<CaseItem>(allocator),
                                        Array<CaseItem>(allocator)};
    for (usize index = 0; index < items.Size(); ++index) {
        const StringView name = items[index].name.View();
        usize kind = KindCount;
        StringView extension;
        if (items[index].directory) {
            kind = 1;
        } else if (HasExtension(name, detail::SourceExtension)) {
            kind = 0;
            extension = detail::SourceExtension;
        } else if (HasExtension(name, detail::ExpectExtension)) {
            kind = 2;
            extension = detail::ExpectExtension;
        } else if (HasExtension(name, detail::SyntaxTreeExtension)) {
            kind = 3;
            extension = detail::SyntaxTreeExtension;
        } else if (HasExtension(name, detail::NamesExtension)) {
            kind = 4;
            extension = detail::NamesExtension;
        } else if (HasExtension(name, detail::TypesExtension)) {
            kind = 5;
            extension = detail::TypesExtension;
        } else if (HasExtension(name, detail::IrExtension)) {
            kind = 6;
            extension = detail::IrExtension;
        }

        if (kind == KindCount) {
            if (auto added =
                    AddLayoutFailure(report, allocator, areaName, name,
                                     "unexpected file (only .spark, .expect, .ast, .names, "
                                     ".types, .ir and case folders)");
                !added) {
                return added;
            }

            continue;
        }

        const CaseItem item{StringView(name.Data(), name.ByteCount() - extension.ByteCount()),
                            index};
        if (auto pushed = lists[kind].TryPushBack(item); !pushed) {
            return pushed;
        }
    }

    for (Array<CaseItem>& list : lists) {
        algo::Sort(list.View(), CaseItemLess);
    }

    // 네 목록을 stem 순으로 함께 훑는다(같은 stem이 한 사례다).
    usize next[KindCount] = {0, 0, 0, 0, 0, 0, 0};
    while (true) {
        bool any = false;
        StringView stem;
        for (usize kind = 0; kind < KindCount; ++kind) {
            if (next[kind] == lists[kind].Size()) {
                continue;
            }

            const StringView candidate = lists[kind][next[kind]].stem;
            if (!any || StringView::CompareBytes(candidate, stem) < 0) {
                stem = candidate;
                any = true;
            }
        }

        if (!any) {
            break;
        }

        StringView present[KindCount];
        for (usize kind = 0; kind < KindCount; ++kind) {
            if (next[kind] < lists[kind].Size() && lists[kind][next[kind]].stem == stem) {
                present[kind] = items[lists[kind][next[kind]].item].name.View();
                ++next[kind];
            }
        }

        const StringView& sourceName = present[0];
        const StringView& folderName = present[1];
        const StringView& expectName = present[2];
        StringView syntaxTreeName = present[3];
        const StringView namesName = present[4];
        const StringView typesName = present[5];
        const StringView irName = present[6];
        if (sourceName.IsEmpty() && folderName.IsEmpty()) {
            const StringView orphans[] = {expectName, syntaxTreeName, namesName, typesName, irName};
            for (const StringView& orphan : orphans) {
                if (orphan.IsEmpty()) {
                    continue;
                }

                if (auto added = AddLayoutFailure(report, allocator, areaName, orphan,
                                                  "missing the .spark file or case folder");
                    !added) {
                    return added;
                }
            }

            continue;
        }

        if (!sourceName.IsEmpty() && !folderName.IsEmpty()) {
            if (auto added = AddLayoutFailure(report, allocator, areaName, folderName,
                                              "a file case and a folder case share the name");
                !added) {
                return added;
            }

            continue;
        }

        if (!folderName.IsEmpty() && !syntaxTreeName.IsEmpty()) {
            if (auto added = AddLayoutFailure(report, allocator, areaName, syntaxTreeName,
                                              ".ast is only for file cases");
                !added) {
                return added;
            }

            syntaxTreeName = StringView();
        }

        if (expectName.IsEmpty()) {
            if (auto added = AddLayoutFailure(report, allocator, areaName,
                                              sourceName.IsEmpty() ? folderName : sourceName,
                                              "missing the .expect file");
                !added) {
                return added;
            }

            continue;
        }

        if (auto ran = RunPairedCase(allocator, files, areaPath, areaName, stem, expectName,
                                     sourceName, folderName, syntaxTreeName, namesName, typesName,
                                     irName, schemas, report);
            !ran) {
            return ran;
        }
    }

    return Result<void>::Success();
}

// schema 폴더의 `*.sparkbind.json`을 읽는다. 다른 파일은 실패 항목이다.
Result<void> LoadSchemas(Allocator& allocator, platform::FileSystem& files,
                         StringView schemaDirectory, Array<LoadedFile>& loaded,
                         Report& report) noexcept {
    Array<DirectoryItem> items(allocator);
    platform::FileStatus status;
    auto listed = ListDirectory(allocator, files, schemaDirectory, items, status);
    if (!listed) {
        return Result<void>::Failure(listed.Error());
    }

    if (!listed.Value()) {
        return Result<void>::Failure(status.code == platform::FileCode::NotFound
                                         ? ErrorCode::NotFound
                                         : ErrorCode::InvalidArgument);
    }

    for (const DirectoryItem& item : items) {
        const StringView name = item.name.View();
        StringBuilder<detail::MessageInlineBytes> path(allocator);
        path.Append(detail::SchemaPathPrefix).Append(name);
        if (path.HasFailed()) {
            return Result<void>::Failure(path.Status().Error());
        }

        if (item.directory || !HasExtension(name, detail::SchemaExtension)) {
            StringBuilder<detail::MessageInlineBytes> message(allocator);
            if (auto formatted = message.TryAppendFormat(
                    "{}: unexpected item (only .sparkbind.json)", path.View());
                !formatted) {
                return formatted;
            }

            if (auto added = report.TryAddFailure(message.View()); !added) {
                return added;
            }

            continue;
        }

        auto full = Path::Combine(allocator, schemaDirectory, name);
        if (!full) {
            return Result<void>::Failure(full.Error());
        }

        auto read = LoadFile(allocator, files, full.Value().View(), path.View(), "Schema", name,
                             loaded, report);
        if (!read) {
            return Result<void>::Failure(read.Error());
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<void> RunCorpus(Allocator& allocator, platform::FileSystem& files,
                       StringView corpusDirectory, StringView schemaDirectory,
                       Report& report) noexcept {
    Array<LoadedFile> schemaFiles(allocator);
    if (!schemaDirectory.IsEmpty()) {
        if (auto loaded = LoadSchemas(allocator, files, schemaDirectory, schemaFiles, report);
            !loaded) {
            return loaded;
        }
    }

    Array<CaseFile> schemas(allocator);
    for (const LoadedFile& file : schemaFiles) {
        if (auto pushed = schemas.TryPushBack(
                CaseFile{file.path.View(), StringView(file.bytes.Data(), file.bytes.Size())});
            !pushed) {
            return pushed;
        }
    }

    Array<DirectoryItem> areas(allocator);
    platform::FileStatus status;
    auto listed = ListDirectory(allocator, files, corpusDirectory, areas, status);
    if (!listed) {
        return Result<void>::Failure(listed.Error());
    }

    if (!listed.Value()) {
        return Result<void>::Failure(status.code == platform::FileCode::NotFound
                                         ? ErrorCode::NotFound
                                         : ErrorCode::InvalidArgument);
    }

    StringBuilder<detail::MessageInlineBytes> message(allocator);
    for (const DirectoryItem& area : areas) {
        if (!area.directory) {
            message.Clear();
            if (auto formatted = message.TryAppendFormat(
                    "{}: unexpected file at the corpus root (cases live in area folders)",
                    area.name.View());
                !formatted) {
                return formatted;
            }

            if (auto added = report.TryAddFailure(message.View()); !added) {
                return added;
            }

            continue;
        }

        auto areaPath = Path::Combine(allocator, corpusDirectory, area.name.View());
        if (!areaPath) {
            return Result<void>::Failure(areaPath.Error());
        }

        if (auto ran = RunArea(allocator, files, areaPath.Value().View(), area.name.View(),
                               schemas.View(), report);
            !ran) {
            return ran;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::sparks::conformance
