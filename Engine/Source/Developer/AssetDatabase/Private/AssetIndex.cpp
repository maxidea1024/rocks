#include "AssetDatabaseInternal.h"

#include <AssetDatabase/AssetIndex.h>
#include <AssetRuntime/PackageRangeReader.h>
#include <Core/FileError.h>
#include <Core/Text/StringBuilder.h>
#include <Object/AssetId.h>
#include <Object/TypeRegistry.h>

// 에디터 패키지 색인이다(Docs/Design/AssetSystem.ko.md 7절, 7.1절 11·13 — A2b).
namespace rocks {

namespace {

using asset_database_detail::AppendAll;

// 경로 임시 저장소의 인라인 byte 수다(넘으면 allocator로 자란다).
constexpr usize IndexPathBytes = 512;

// 마운트 "/Game"의 경로 키다(논리 경로 키의 첫 구성 요소 — 7.1절 1).
constexpr StringView GameMountKey = "game";

int CompareId(AssetId a, AssetId b) noexcept {
    if (a.value.high != b.value.high) {
        return a.value.high < b.value.high ? -1 : 1;
    }

    if (a.value.low != b.value.low) {
        return a.value.low < b.value.low ? -1 : 1;
    }

    return 0;
}

// 열거한 자식 하나다.
struct ChildEntry {
    explicit ChildEntry(Allocator& allocator) noexcept : name(allocator) {
    }

    String name;
    platform::FileInfo info;
};

struct EnumerateContext {
    Array<ChildEntry>* children;
    ErrorCode error = ErrorCode::None;
};

bool CollectChild(const platform::DirectoryEntry& entry, void* raw) noexcept {
    auto& context = *static_cast<EnumerateContext*>(raw);
    ChildEntry child(context.children->GetAllocator());
    if (auto assigned = child.name.TryAssign(StringView(entry.name, entry.nameBytes)); !assigned) {
        context.error = assigned.Error();
        return false;
    }

    child.info = entry.info;
    if (auto pushed = context.children->TryPushBack(Move(child)); !pushed) {
        context.error = pushed.Error();
        return false;
    }

    return true;
}

// 진단 하나를 만든다.
Result<void> AddDiagnostic(Array<AssetIndexDiagnostic>& out, AssetDiagnosticCode code,
                           ErrorCode error, StringView fileName) noexcept {
    AssetIndexDiagnostic diagnostic(out.GetAllocator());
    diagnostic.code = code;
    diagnostic.error = error;
    if (auto assigned = diagnostic.fileName.TryAssign(fileName); !assigned) {
        return assigned;
    }

    return out.TryPushBack(Move(diagnostic));
}

// 정렬 순서 order에서 같은 값(same)이 이웃한 항목을 모두 flag로 표시하고 code 진단을 남긴다. 겹친
// 상대(related)는 앞 이웃이 같으면 그것, 아니면 뒤 이웃이다(같은 값이 셋 이상이어도 각자 이웃
// 하나).
template <class TSame>
Result<void> DiagnoseNeighbors(Array<AssetIndexEntry>& entries, Span<const usize> order, TSame same,
                               bool AssetIndexEntry::* flag, AssetDiagnosticCode code,
                               Array<AssetIndexDiagnostic>& diagnostics) noexcept {
    for (usize position = 0; position < order.Size(); ++position) {
        const bool before = position > 0 && same(order[position - 1], order[position]);
        const bool after =
            position + 1 < order.Size() && same(order[position], order[position + 1]);
        if (!before && !after) {
            continue;
        }

        AssetIndexEntry& entry = entries[order[position]];
        entry.*flag = true;
        if (auto added =
                AddDiagnostic(diagnostics, code, ErrorCode::AlreadyExists, entry.fileName.View());
            !added) {
            return added;
        }

        AssetIndexDiagnostic& diagnostic = diagnostics[diagnostics.Size() - 1];
        diagnostic.asset = entry.id;
        const usize partner = order[before ? position - 1 : position + 1];
        if (auto assigned = diagnostic.related.TryAssign(entries[partner].fileName.View());
            !assigned) {
            return assigned;
        }
    }

    return Result<void>::Success();
}

// 경로 키 base(디렉터리) 아래 항목을 out에 붙인다. 항목 키가 "<base>/"로 시작하면 그 아래다(구성
// 요소 경계 — "/Game/Char"는 "/Game/Characters"를 포함하지 않는다). recursive가 거짓이면 바로
// 아래만.
Result<void> AppendUnder(Span<const AssetIndexEntry> entries, StringView base, bool recursive,
                         Array<const AssetIndexEntry*>& out) noexcept {
    for (const AssetIndexEntry& entry : entries) {
        const StringView key = entry.key.View();
        if (key.ByteCount() <= base.ByteCount() + 1 ||
            !key.StartsWith(base, StringComparison::CaseSensitive) ||
            key.Data()[base.ByteCount()] != '/') {
            continue;
        }

        const StringView rest(key.Data() + base.ByteCount() + 1,
                              key.ByteCount() - base.ByteCount() - 1);
        if (!recursive && rest.Contains("/", StringComparison::CaseSensitive)) {
            continue;
        }

        if (auto pushed = out.TryPushBack(&entry); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

// index 표를 less로 삽입 정렬한다(안정 정렬 — 같은 순위는 원래 순서).
template <class TLess>
void SortIndices(Array<usize>& order, TLess less) noexcept {
    for (usize index = 1; index < order.Size(); ++index) {
        const usize value = order[index];
        usize at = index;
        while (at > 0 && less(value, order[at - 1])) {
            order[at] = order[at - 1];
            --at;
        }

        order[at] = value;
    }
}

} // namespace

AssetIndex::AssetIndex(Allocator& allocator, const ProjectLayout& layout) noexcept
    : allocator_(&allocator), layout_(&layout), entries_(allocator), byId_(allocator),
      diagnostics_(allocator) {
}

Result<void> AssetIndex::Rebuild(platform::FileSystem& files,
                                 const PackageReadOptions& options) noexcept {
    entries_.Clear();
    byId_.Clear();
    diagnostics_.Clear();
    Array<AssetIndexEntry> found(*allocator_);
    Array<AssetIndexDiagnostic> diagnostics(*allocator_);
    const StringView content = layout_->ContentDirectory();
    // 깊이 우선으로 디렉터리를 연다(Content 상대 경로 표 — 빈 문자열은 Content 자신).
    Array<String> pending(*allocator_);
    if (auto pushed = pending.TryPushBack(String(*allocator_)); !pushed) {
        return pushed;
    }

    PackageReadOptions editor = options;
    editor.expectedKind = PackageKind::Editor;
    while (!pending.IsEmpty()) {
        String relativeDirectory = Move(pending[pending.Size() - 1]);
        (void)pending.TryPopBack();
        StringBuilder<IndexPathBytes> directory(*allocator_);
        const StringView directoryPieces[] = {
            content, relativeDirectory.View().IsEmpty() ? "" : "/", relativeDirectory.View()};
        if (auto built = AppendAll(directory, directoryPieces); !built) {
            return built;
        }

        Array<ChildEntry> children(*allocator_);
        EnumerateContext context{&children};
        if (auto listed = files.EnumerateDirectory(
                directory.View().Data(), directory.View().ByteCount(), &CollectChild, &context);
            !listed) {
            // 없는 디렉터리는 빈 디렉터리다(Content가 없거나 열거 사이에 지워졌다 — 그 안의
            // 파일도 없다).
            if (listed.code == platform::FileCode::NotFound) {
                continue;
            }

            return Result<void>::Failure(FileErrorCode(listed));
        }

        if (context.error != ErrorCode::None) {
            return Result<void>::Failure(context.error);
        }

        for (ChildEntry& child : children) {
            String relative(*allocator_);
            const StringView relativePieces[] = {relativeDirectory.View(),
                                                 relativeDirectory.View().IsEmpty() ? "" : "/",
                                                 child.name.View()};
            for (StringView piece : relativePieces) {
                if (auto appended = relative.TryAppend(piece); !appended) {
                    return appended;
                }
            }

            // 링크는 따라가지 않는다(순환·프로젝트 밖으로 나가지 않게).
            if (child.info.isLink) {
                continue;
            }

            if (child.info.type == platform::FileType::Directory) {
                if (auto pushed = pending.TryPushBack(Move(relative)); !pushed) {
                    return pushed;
                }

                continue;
            }

            // 저장 임시 파일("<이름>.rasset.<guid>.tmp")은 확장자가 달라 여기서 걸러진다.
            if (child.info.type != platform::FileType::File ||
                !child.name.View().EndsWith(EditorPackageExtension)) {
                continue;
            }

            LogicalPathFailure pathFailure;
            auto path = layout_->TryMakeLogicalPath(relative.View(), &pathFailure);
            if (!path) {
                if (pathFailure.error == LogicalPathError::None) {
                    return Result<void>::Failure(path.Error());
                }

                if (auto added = AddDiagnostic(diagnostics, AssetDiagnosticCode::InvalidPath,
                                               path.Error(), relative.View());
                    !added) {
                    return added;
                }

                diagnostics[diagnostics.Size() - 1].pathError = pathFailure.error;
                continue;
            }

            StringBuilder<IndexPathBytes> full(*allocator_);
            const StringView fullPieces[] = {content, "/", relative.View()};
            if (auto built = AppendAll(full, fullPieces); !built) {
                return built;
            }

            FilePackageRangeReader reader;
            AssetDiagnostic failure;
            auto opened = reader.Open(files, full.View());
            Result<PackageIndexView> view =
                opened ? ReadPackageIndex(*allocator_, reader, editor, &failure)
                       : Result<PackageIndexView>::Failure(opened.Error());

            if (!view) {
                if (view.Error() == ErrorCode::OutOfMemory) {
                    return Result<void>::Failure(view.Error());
                }

                // 열거 뒤 사라진 파일은 MissingFile, 그 밖의 IO 실패는 진단 None과 그 오류다
                // (쿠킹 로더 10.1절과 같은 규칙 — 색인에서 빼고 계속한다).
                const AssetDiagnosticCode code =
                    failure.code == AssetDiagnosticCode::None && view.Error() == ErrorCode::NotFound
                        ? AssetDiagnosticCode::MissingFile
                        : failure.code;
                if (auto added = AddDiagnostic(diagnostics, code, view.Error(), relative.View());
                    !added) {
                    return added;
                }

                diagnostics[diagnostics.Size() - 1].section = failure.section;
                diagnostics[diagnostics.Size() - 1].detail = failure.detail;
                diagnostics[diagnostics.Size() - 1].asset = failure.asset;
                continue;
            }

            PackageContents& contents = view.Value().contents;
            AssetIndexEntry entry(*allocator_);
            entry.id = contents.header.assetId;
            entry.type = contents.exports[0].type;
            entry.fingerprint = contents.fingerprint;
            entry.fileBytes = reader.FileBytes();
            entry.modifiedTicks = child.info.modifiedTicks;
            entry.readOnly = contents.readOnly;
            if (auto assigned = entry.path.TryAssign(path.Value().Text()); !assigned) {
                return assigned;
            }

            if (auto assigned = entry.key.TryAssign(path.Value().Key()); !assigned) {
                return assigned;
            }

            entry.fileName = Move(relative);
            if (auto appended = entry.dependencies.TryAppend(contents.dependencies.View());
                !appended) {
                return appended;
            }

            auto tags = ReadPackageTags(*allocator_, contents.tags);
            if (tags) {
                entry.tags = Move(tags.Value());
            } else if (tags.Error() == ErrorCode::OutOfMemory) {
                return Result<void>::Failure(tags.Error());
            } else {
                // 다른 스키마의 Tags는 형식으로 유효하다 — 태그 없이 색인하고 진단만 남긴다.
                const AssetDiagnosticCode code = tags.Error() == ErrorCode::Unsupported
                                                     ? AssetDiagnosticCode::UnsupportedVersion
                                                     : AssetDiagnosticCode::CorruptPackage;
                if (auto added =
                        AddDiagnostic(diagnostics, code, tags.Error(), entry.fileName.View());
                    !added) {
                    return added;
                }

                diagnostics[diagnostics.Size() - 1].asset = entry.id;
                diagnostics[diagnostics.Size() - 1].section =
                    static_cast<uint32>(PackageSectionKind::Tags);
            }

            if (auto pushed = found.TryPushBack(Move(entry)); !pushed) {
                return pushed;
            }
        }
    }

    // 경로 키 → 파일 이름 순으로 정렬한다(완료 순서와 무관 — 7.1절 9).
    Array<usize> order(*allocator_);
    if (auto reserved = order.TryReserve(found.Size()); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < found.Size(); ++index) {
        (void)order.TryPushBack(index);
    }

    SortIndices(order, [&](usize left, usize right) noexcept {
        const int key = StringView::CompareBytes(found[left].key.View(), found[right].key.View());
        return key != 0 ? key < 0
                        : StringView::CompareBytes(found[left].fileName.View(),
                                                   found[right].fileName.View()) < 0;
    });
    Array<AssetIndexEntry> sorted(*allocator_);
    if (auto reserved = sorted.TryReserve(found.Size()); !reserved) {
        return reserved;
    }

    for (usize index : order) {
        (void)sorted.TryPushBack(Move(found[index]));
    }

    // 같은 경로 키(DuplicatePath — 경로 키 순에서 이웃)와 같은 id(DuplicateAssetId — id 순에서
    // 이웃)를 모두 진단한다.
    Array<usize> byId(*allocator_);
    if (auto reserved = byId.TryReserve(sorted.Size()); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < sorted.Size(); ++index) {
        (void)byId.TryPushBack(index);
    }

    if (auto diagnosed = DiagnoseNeighbors(
            sorted, byId.View(),
            [&](usize left, usize right) noexcept {
                return sorted[left].key.View() == sorted[right].key.View();
            },
            &AssetIndexEntry::duplicatePath, AssetDiagnosticCode::DuplicatePath, diagnostics);
        !diagnosed) {
        return diagnosed;
    }

    SortIndices(byId, [&](usize left, usize right) noexcept {
        return CompareId(sorted[left].id, sorted[right].id) < 0;
    });
    if (auto diagnosed = DiagnoseNeighbors(
            sorted, byId.View(),
            [&](usize left, usize right) noexcept {
                return CompareId(sorted[left].id, sorted[right].id) == 0;
            },
            &AssetIndexEntry::duplicateId, AssetDiagnosticCode::DuplicateAssetId, diagnostics);
        !diagnosed) {
        return diagnosed;
    }

    // 진단은 파일 이름 → 코드 순이다.
    Array<usize> diagnosticOrder(*allocator_);
    if (auto reserved = diagnosticOrder.TryReserve(diagnostics.Size()); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < diagnostics.Size(); ++index) {
        (void)diagnosticOrder.TryPushBack(index);
    }

    SortIndices(diagnosticOrder, [&](usize left, usize right) noexcept {
        const int name = StringView::CompareBytes(diagnostics[left].fileName.View(),
                                                  diagnostics[right].fileName.View());
        return name != 0 ? name < 0
                         : static_cast<uint32>(diagnostics[left].code) <
                               static_cast<uint32>(diagnostics[right].code);
    });
    Array<AssetIndexDiagnostic> sortedDiagnostics(*allocator_);
    if (auto reserved = sortedDiagnostics.TryReserve(diagnostics.Size()); !reserved) {
        return reserved;
    }

    for (usize index : diagnosticOrder) {
        (void)sortedDiagnostics.TryPushBack(Move(diagnostics[index]));
    }

    entries_ = Move(sorted);
    byId_ = Move(byId);
    diagnostics_ = Move(sortedDiagnostics);
    return Result<void>::Success();
}

Result<const AssetIndexEntry*> AssetIndex::FindById(AssetId id) const noexcept {
    using Out = const AssetIndexEntry*;
    usize low = 0;
    usize high = byId_.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (CompareId(entries_[byId_[middle]].id, id) < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    if (low == byId_.Size() || CompareId(entries_[byId_[low]].id, id) != 0) {
        return Result<Out>::Failure(ErrorCode::NotFound);
    }

    const AssetIndexEntry& entry = entries_[byId_[low]];
    if (entry.duplicateId) {
        return Result<Out>::Failure(ErrorCode::AlreadyExists);
    }

    return Result<Out>::Success(&entry);
}

Result<const AssetIndexEntry*> AssetIndex::FindByPath(const LogicalPath& path) const noexcept {
    using Out = const AssetIndexEntry*;
    usize low = 0;
    usize high = entries_.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (StringView::CompareBytes(entries_[middle].key.View(), path.Key()) < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    if (low == entries_.Size() || entries_[low].key.View() != path.Key()) {
        return Result<Out>::Failure(ErrorCode::NotFound);
    }

    if (entries_[low].duplicatePath) {
        return Result<Out>::Failure(ErrorCode::AlreadyExists);
    }

    return Result<Out>::Success(&entries_[low]);
}

Result<void> AssetIndex::ListUnder(StringView directory, bool recursive,
                                   Array<const AssetIndexEntry*>& out) const noexcept {
    if (directory.ByteCount() == GameMount.ByteCount() &&
        directory.StartsWith(GameMount, StringComparison::IgnoreAsciiCase)) {
        return AppendUnder(entries_.View(), GameMountKey, recursive, out);
    }

    auto parsed = ParseLogicalPath(*allocator_, directory);
    if (!parsed) {
        return Result<void>::Failure(parsed.Error());
    }

    return AppendUnder(entries_.View(), parsed.Value().Key(), recursive, out);
}

Result<void> AssetIndex::ListByType(TypeId type, TypeRegistry* types,
                                    Array<const AssetIndexEntry*>& out) const noexcept {
    if (type == TypeId{}) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (const AssetIndexEntry& entry : entries_) {
        bool match = entry.type == type;
        if (!match && types != nullptr) {
            auto derived = types->IsA(entry.type, type);
            if (derived) {
                match = derived.Value();
            } else if (derived.Error() != ErrorCode::NotFound &&
                       derived.Error() != ErrorCode::TypeMismatch) {
                return Result<void>::Failure(derived.Error());
            }
        }

        if (match) {
            if (auto pushed = out.TryPushBack(&entry); !pushed) {
                return pushed;
            }
        }
    }

    return Result<void>::Success();
}

Result<void> AssetIndex::FindByTag(StringView key, StringView value,
                                   Array<const AssetIndexEntry*>& out) const noexcept {
    for (const AssetIndexEntry& entry : entries_) {
        for (const PackageTag& tag : entry.tags) {
            if (tag.key.View() == key && tag.value.View() == value) {
                if (auto pushed = out.TryPushBack(&entry); !pushed) {
                    return pushed;
                }

                break;
            }
        }
    }

    return Result<void>::Success();
}

Result<void> AssetIndex::Dependencies(AssetId id, uint32 kinds,
                                      Array<const PackageDependencyEntry*>& out) const noexcept {
    auto entry = FindById(id);
    if (!entry) {
        return Result<void>::Failure(entry.Error());
    }

    for (const PackageDependencyEntry& dependency : entry.Value()->dependencies) {
        if ((kinds & DependencyKindBit(dependency.kind)) != 0) {
            if (auto pushed = out.TryPushBack(&dependency); !pushed) {
                return pushed;
            }
        }
    }

    return Result<void>::Success();
}

Result<void> AssetIndex::Referencers(AssetId id, uint32 kinds,
                                     Array<const AssetIndexEntry*>& out) const noexcept {
    for (const AssetIndexEntry& entry : entries_) {
        for (const PackageDependencyEntry& dependency : entry.dependencies) {
            if (CompareId(dependency.asset, id) == 0 &&
                (kinds & DependencyKindBit(dependency.kind)) != 0) {
                if (auto pushed = out.TryPushBack(&entry); !pushed) {
                    return pushed;
                }

                break;
            }
        }
    }

    return Result<void>::Success();
}

Result<void> AssetIndex::Verify(TypeRegistry* types,
                                Array<AssetIndexDiagnostic>& out) const noexcept {
    for (const AssetIndexEntry& entry : entries_) {
        for (const PackageDependencyEntry& dependency : entry.dependencies) {
            if (dependency.kind != PackageDependencyKind::Hard &&
                dependency.kind != PackageDependencyKind::Soft) {
                continue;
            }

            auto target = FindById(dependency.asset);
            AssetDiagnosticCode code = AssetDiagnosticCode::None;
            if (!target) {
                // 같은 id가 둘이면(AlreadyExists) 대상은 있다 — DuplicateAssetId가 이미 진단했다.
                code = target.Error() == ErrorCode::NotFound
                           ? AssetDiagnosticCode::MissingDependency
                           : AssetDiagnosticCode::None;
            } else if (dependency.kind == PackageDependencyKind::Hard &&
                       !(target.Value()->type == dependency.expectedType)) {
                // Hard는 반드시 expectedType을 가진다(패키지 읽기가 검사한다 — 5.2절).
                bool derived = false;
                if (types != nullptr) {
                    auto isA = types->IsA(target.Value()->type, dependency.expectedType);
                    if (isA) {
                        derived = isA.Value();
                    } else if (isA.Error() != ErrorCode::NotFound &&
                               isA.Error() != ErrorCode::TypeMismatch) {
                        return Result<void>::Failure(isA.Error());
                    }
                }

                code = derived ? AssetDiagnosticCode::None : AssetDiagnosticCode::TypeMismatch;
            }

            if (code == AssetDiagnosticCode::None) {
                continue;
            }

            if (auto added = AddDiagnostic(out, code,
                                           code == AssetDiagnosticCode::MissingDependency
                                               ? ErrorCode::NotFound
                                               : ErrorCode::TypeMismatch,
                                           entry.fileName.View());
                !added) {
                return added;
            }

            out[out.Size() - 1].asset = entry.id;
            out[out.Size() - 1].relatedAsset = dependency.asset;
        }
    }

    return Result<void>::Success();
}

Result<void> AppendAssetIndexDiagnosticLine(StringBuilderBase& line,
                                            const AssetIndexDiagnostic& diagnostic) noexcept {
    const char* code = diagnostic.code == AssetDiagnosticCode::None
                           ? ErrorCodeName(diagnostic.error)
                           : AssetDiagnosticName(diagnostic.code);
    if (auto formatted = line.TryAppendFormat("{}: Content/{}: ", code, diagnostic.fileName.View());
        !formatted) {
        return formatted;
    }

    switch (diagnostic.code) {
    case AssetDiagnosticCode::None:
        return line.TryAppend("cannot read the package file\n");
    case AssetDiagnosticCode::InvalidPath:
        return line.TryAppendFormat("invalid logical path ({})\n",
                                    LogicalPathErrorName(diagnostic.pathError));
    case AssetDiagnosticCode::MissingFile:
        return line.TryAppend("the file disappeared during the scan\n");
    case AssetDiagnosticCode::DuplicateAssetId:
        return line.TryAppendFormat("asset id {} is also in 'Content/{}'\n", diagnostic.asset,
                                    diagnostic.related.View());
    case AssetDiagnosticCode::DuplicatePath:
        return line.TryAppendFormat("the logical path is also used by 'Content/{}'\n",
                                    diagnostic.related.View());
    case AssetDiagnosticCode::MissingDependency:
        return line.TryAppendFormat("dependency {} is not in the index\n", diagnostic.relatedAsset);
    case AssetDiagnosticCode::TypeMismatch:
        return line.TryAppendFormat("dependency {} has a different representative type\n",
                                    diagnostic.relatedAsset);
    default:
        return line.TryAppendFormat("package check failed (section {}, detail {}, {})\n",
                                    diagnostic.section, diagnostic.detail,
                                    ErrorCodeName(diagnostic.error));
    }
}

} // namespace rocks
