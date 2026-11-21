#include "AssetDatabaseInternal.h"

#include <AssetDatabase/AssetIndex.h>
#include <AssetDatabase/ImportRecord.h>
#include <AssetRuntime/PackageRangeReader.h>
#include <Core/Algorithms.h>
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
    explicit ChildEntry(Allocator& allocator) noexcept : name(allocator), raw(allocator) {
    }

    // OS 이름이다. 유효한 UTF-8이면 name, 아니면(POSIX) byte 그대로 raw에 두고 invalidName이
    // 참이다(그 이름으로는 논리 경로도, 다시 열 경로도 만들 수 없다 — String은 UTF-8만 받는다).
    String name;
    Array<char> raw;
    bool invalidName = false;
    platform::FileInfo info;
};

struct EnumerateContext {
    Array<ChildEntry>* children;
    ErrorCode error = ErrorCode::None;
};

// 진단의 표시 이름을 out에 쓴다(AssetIndexDiagnostic::fileName). 한 줄·유효한 UTF-8이고 서로 다른
// OS 이름이 같은 표시 이름이 되지 않게 역슬래시·제어 문자(0x00–0x1f·0x7f)와, 이름이 유효한
// UTF-8이 아니면 0x80 이상 byte를 16진(역슬래시 x 두 자리)으로 적는다. 나머지는 그대로다(유효한
// UTF-8 이름의 비ASCII 문자 포함). 길이를 먼저 세어 버퍼를 한 번 잡고 채운다.
Result<void> AssignDisplayName(String& out, StringView raw) noexcept {
    constexpr uint8 FirstPrintable = 0x20;
    constexpr uint8 Delete = 0x7f;
    constexpr uint8 FirstNonAscii = 0x80;
    constexpr usize EscapeBytes = 4;
    constexpr char Digits[] = "0123456789abcdef";
    constexpr uint32 NibbleBits = 4;
    constexpr uint8 NibbleMask = 0x0f;
    const bool validUtf8 = raw.IsValidUtf8();
    auto escaped = [&](uint8 value) noexcept {
        return value < FirstPrintable || value == Delete || value == '\\' ||
               (!validUtf8 && value >= FirstNonAscii);
    };
    usize bytes = 0;
    for (usize index = 0; index < raw.ByteCount(); ++index) {
        bytes += escaped(static_cast<uint8>(raw.Data()[index])) ? EscapeBytes : 1;
    }

    Array<char> text(out.GetAllocator());
    if (auto sized = text.TryResize(bytes); !sized) {
        return sized;
    }

    usize at = 0;
    for (usize index = 0; index < raw.ByteCount(); ++index) {
        const uint8 value = static_cast<uint8>(raw.Data()[index]);
        if (!escaped(value)) {
            text[at++] = raw.Data()[index];
            continue;
        }

        text[at++] = '\\';
        text[at++] = 'x';
        text[at++] = Digits[value >> NibbleBits];
        text[at++] = Digits[value & NibbleMask];
    }

    return out.TryAssign(StringView(text.Data(), text.Size()));
}

bool CollectChild(const platform::DirectoryEntry& entry, void* raw) noexcept {
    auto& context = *static_cast<EnumerateContext*>(raw);
    ChildEntry child(context.children->GetAllocator());
    const StringView name(entry.name, entry.nameBytes);
    child.invalidName = !name.IsValidUtf8();
    if (auto assigned = child.invalidName
                            ? child.raw.TryAppend(Span<const char>(entry.name, entry.nameBytes))
                            : child.name.TryAssign(name);
        !assigned) {
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

// 진단 하나를 만든다(fileName은 OS 상대 이름 byte — 표시 이름으로 바꿔 둔다).
Result<void> AddDiagnostic(Array<AssetIndexDiagnostic>& out, AssetDiagnosticCode code,
                           ErrorCode error, StringView fileName) noexcept {
    AssetIndexDiagnostic diagnostic(out.GetAllocator());
    diagnostic.code = code;
    diagnostic.error = error;
    if (auto assigned = AssignDisplayName(diagnostic.fileName, fileName); !assigned) {
        return assigned;
    }

    return out.TryPushBack(Move(diagnostic));
}

// 항목의 ImportRecord를 요약하고 ImportOwner 의존과 대조한다(7.1절 15 — A2c2a). 읽지 못한 기록(다른
// 스키마·더 새 버전 UnsupportedVersion, 형식 위반 CorruptPackage — 둘 다 칸 ImportRecord)은 요약
// 없이 진단만 남기고 대조하지 않는다(참조 기록인지 알 수 없다). 대조 위반은 CorruptPackage(칸
// Dependencies)다. 항목은 어느 경우에도 색인한다(조회되어야 고칠 수 있다). 오류: 할당 실패.
Result<void> IndexImportRecord(Allocator& allocator, Span<const uint8> bytes,
                               AssetIndexEntry& entry,
                               Array<AssetIndexDiagnostic>& diagnostics) noexcept {
    ImportRecord record(allocator);
    const ImportRecord* checked = nullptr;
    if (!bytes.IsEmpty()) {
        auto read = ReadImportRecord(allocator, entry.id, bytes);
        if (!read) {
            if (read.Error() == ErrorCode::OutOfMemory) {
                return Result<void>::Failure(read.Error());
            }

            const AssetDiagnosticCode code = read.Error() == ErrorCode::Unsupported
                                                 ? AssetDiagnosticCode::UnsupportedVersion
                                                 : AssetDiagnosticCode::CorruptPackage;
            if (auto added = AddDiagnostic(diagnostics, code, read.Error(), entry.fileName.View());
                !added) {
                return added;
            }

            diagnostics[diagnostics.Size() - 1].asset = entry.id;
            diagnostics[diagnostics.Size() - 1].section =
                static_cast<uint32>(PackageSectionKind::ImportRecord);
            return Result<void>::Success();
        }

        record = Move(read.Value());
        checked = &record;
        AssetIndexImportSummary& summary = entry.importRecord;
        summary.present = true;
        summary.owner = record.owner;
        summary.importerId = record.importerId;
        summary.importerName = Move(record.importerName);
        summary.importerVersion = record.importerVersion;
        summary.outputCount = record.outputs.Size();
    }

    if (auto owner = CheckImportOwner(entry.id, checked, entry.dependencies.View()); !owner) {
        if (auto added = AddDiagnostic(diagnostics, AssetDiagnosticCode::CorruptPackage,
                                       owner.Error(), entry.fileName.View());
            !added) {
            return added;
        }

        diagnostics[diagnostics.Size() - 1].asset = entry.id;
        diagnostics[diagnostics.Size() - 1].section =
            static_cast<uint32>(PackageSectionKind::Dependencies);
    }

    return Result<void>::Success();
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
        if (auto assigned = AssignDisplayName(diagnostic.related, entries[partner].fileName.View());
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

// 0 … count - 1 index 표를 만든다.
Result<void> MakeOrder(Array<usize>& order, usize count) noexcept {
    if (auto reserved = order.TryReserve(count); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < count; ++index) {
        (void)order.TryPushBack(index);
    }

    return Result<void>::Success();
}

} // namespace

AssetIndex::AssetIndex(Allocator& allocator, const ProjectLayout& layout) noexcept
    : allocator_(&allocator), layout_(&layout), entries_(allocator), byId_(allocator),
      diagnostics_(allocator) {
}

// 패키지 파일 하나(Content 상대 이름 relative)를 부분 읽기해 found에 항목을, diagnostics에 그
// 파일의 진단을 더한다(Rebuild·Refresh 공용). 이름 규칙 위반·패키지 판정·IO 실패는 진단이고 항목을
// 더하지 않는다. 오류: 할당 실패.
Result<void> AssetIndex::IndexFile(platform::FileSystem& files, const PackageReadOptions& editor,
                                   String&& relative, int64 modifiedTicks,
                                   Array<AssetIndexEntry>& found,
                                   Array<AssetIndexDiagnostic>& diagnostics) noexcept {
    const StringView content = layout_->ContentDirectory();
    LogicalPathFailure pathFailure;
    auto path = layout_->TryMakeLogicalPath(relative.View(), &pathFailure);
    if (!path) {
        if (path.Error() == ErrorCode::OutOfMemory) {
            return Result<void>::Failure(path.Error());
        }

        if (auto added = AddDiagnostic(diagnostics, AssetDiagnosticCode::InvalidPath, path.Error(),
                                       relative.View());
            !added) {
            return added;
        }

        // 확장자뿐인 이름(".rasset")은 배치가 "패키지 파일 아님"(사유 None)으로 보지만
        // 확장자 검사는 통과했다 — 이름이 비었으므로 하위 디렉터리의 같은 이름
        // ("Dir/.rasset" → 끝 '/')과 같은 EmptyComponent로 진단한다.
        diagnostics[diagnostics.Size() - 1].pathError = pathFailure.error == LogicalPathError::None
                                                            ? LogicalPathError::EmptyComponent
                                                            : pathFailure.error;
        return Result<void>::Success();
    }

    StringBuilder<IndexPathBytes> full(*allocator_);
    const StringView fullPieces[] = {content, "/", relative.View()};
    if (auto built = AppendAll(full, fullPieces); !built) {
        return built;
    }

    FilePackageRangeReader reader;
    AssetDiagnostic failure;
    auto opened = reader.Open(files, full.View());
    Result<PackageIndexView> view = opened ? ReadPackageIndex(*allocator_, reader, editor, &failure)
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
        if (auto added = AddDiagnostic(diagnostics, code, view.Error(), relative.View()); !added) {
            return added;
        }

        diagnostics[diagnostics.Size() - 1].section = failure.section;
        diagnostics[diagnostics.Size() - 1].detail = failure.detail;
        diagnostics[diagnostics.Size() - 1].asset = failure.asset;
        return Result<void>::Success();
    }

    PackageContents& contents = view.Value().contents;
    AssetIndexEntry entry(*allocator_);
    entry.id = contents.header.assetId;
    entry.type = contents.exports[0].type;
    entry.fingerprint = contents.fingerprint;
    entry.fileBytes = reader.FileBytes();
    entry.modifiedTicks = modifiedTicks;
    entry.readOnly = contents.readOnly;
    if (auto assigned = entry.path.TryAssign(path.Value().Text()); !assigned) {
        return assigned;
    }

    if (auto assigned = entry.key.TryAssign(path.Value().Key()); !assigned) {
        return assigned;
    }

    entry.fileName = Move(relative);
    if (auto appended = entry.dependencies.TryAppend(contents.dependencies.View()); !appended) {
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
        if (auto added = AddDiagnostic(diagnostics, code, tags.Error(), entry.fileName.View());
            !added) {
            return added;
        }

        diagnostics[diagnostics.Size() - 1].asset = entry.id;
        diagnostics[diagnostics.Size() - 1].section = static_cast<uint32>(PackageSectionKind::Tags);
    }

    if (auto indexed = IndexImportRecord(*allocator_, contents.importRecord, entry, diagnostics);
        !indexed) {
        return indexed;
    }

    if (auto pushed = found.TryPushBack(Move(entry)); !pushed) {
        return pushed;
    }
    return Result<void>::Success();
}

// found(순서 없음)와 diagnostics(이름 진단·파일 진단)를 정렬하고 같은 id·경로 진단을 더해 게시한다
// (Rebuild·Refresh 공용 — 같은 id·경로 표시는 여기서 처음부터 다시 정한다).
Result<void> AssetIndex::Publish(Array<AssetIndexEntry>& found,
                                 Array<AssetIndexDiagnostic>& diagnostics) noexcept {
    for (AssetIndexEntry& entry : found) {
        entry.duplicateId = false;
        entry.duplicatePath = false;
    }

    // 경로 키 → 파일 이름 순으로 정렬한다(완료 순서와 무관 — 7.1절 9). Core 정렬은 안정 정렬이
    // 아니지만 Content 상대 이름은 파일마다 달라 이 비교가 전순서다.
    Array<usize> order(*allocator_);
    if (auto made = MakeOrder(order, found.Size()); !made) {
        return made;
    }

    algo::Sort(order.View(), [&](usize left, usize right) noexcept {
        int compared = StringView::CompareBytes(found[left].key.View(), found[right].key.View());
        if (compared == 0) {
            compared =
                StringView::CompareBytes(found[left].fileName.View(), found[right].fileName.View());
        }

        return compared < 0;
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
    if (auto made = MakeOrder(byId, sorted.Size()); !made) {
        return made;
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

    // id → 경로 순 위치(같은 id는 경로 순).
    algo::Sort(byId.View(), [&](usize left, usize right) noexcept {
        const int compared = CompareId(sorted[left].id, sorted[right].id);
        return compared != 0 ? compared < 0 : left < right;
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

    // 진단은 파일 이름 → 코드 → 칸 순이다. 한 이름(파일·디렉터리)의 진단은 (코드, 칸)마다
    // 하나뿐이라(경로·패키지 판정 진단은 서로 배타이고 그 밖의 항목 진단과도 배타, 항목 진단은
    // Tags· ImportRecord·Dependencies 칸마다 하나, 같은 id·경로 진단은 각각 하나 — 칸 0) 이 비교가
    // 전순서다.
    Array<usize> diagnosticOrder(*allocator_);
    if (auto made = MakeOrder(diagnosticOrder, diagnostics.Size()); !made) {
        return made;
    }

    algo::Sort(diagnosticOrder.View(), [&](usize left, usize right) noexcept {
        const int name = StringView::CompareBytes(diagnostics[left].fileName.View(),
                                                  diagnostics[right].fileName.View());
        if (name != 0) {
            return name < 0;
        }

        if (diagnostics[left].code != diagnostics[right].code) {
            return static_cast<uint32>(diagnostics[left].code) <
                   static_cast<uint32>(diagnostics[right].code);
        }

        return diagnostics[left].section < diagnostics[right].section;
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

            // Content 자신의 열거 실패는 검색 실패다. 하위 디렉터리의 실패(권한 거부·검색 중
            // 파일로 바뀜)는 파일 IO 실패처럼 진단 None과 원본 오류를 남기고 그 디렉터리만
            // 뺀다(7.1절 14).
            if (relativeDirectory.View().IsEmpty()) {
                return Result<void>::Failure(FileErrorCode(listed));
            }

            if (auto added = AddDiagnostic(diagnostics, AssetDiagnosticCode::None,
                                           FileErrorCode(listed), relativeDirectory.View());
                !added) {
                return added;
            }

            continue;
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

            // 유효한 UTF-8이 아닌 이름(POSIX)은 논리 경로가 될 수 없고 그 이름으로 다시 열 수도
            // 없다: 디렉터리·패키지 파일이면 InvalidPath(InvalidComponent — 이름 규칙의
            // InvalidCharacter와 같은 사유)로 진단하고 내려가지 않는다. 그 밖의 파일은 무시한다.
            if (child.invalidName) {
                const bool package =
                    child.info.type == platform::FileType::File &&
                    StringView(child.raw.Data(), child.raw.Size()).EndsWith(EditorPackageExtension);
                if (child.info.type == platform::FileType::Directory || package) {
                    Array<char> rawRelative(*allocator_);
                    const StringView rawPieces[] = {relativeDirectory.View(),
                                                    relativeDirectory.View().IsEmpty() ? "" : "/",
                                                    StringView(child.raw.Data(), child.raw.Size())};
                    for (StringView piece : rawPieces) {
                        if (auto appended = rawRelative.TryAppend(
                                Span<const char>(piece.Data(), piece.ByteCount()));
                            !appended) {
                            return appended;
                        }
                    }

                    if (auto added =
                            AddDiagnostic(diagnostics, AssetDiagnosticCode::InvalidPath,
                                          ErrorCode::InvalidArgument,
                                          StringView(rawRelative.Data(), rawRelative.Size()));
                        !added) {
                        return added;
                    }

                    diagnostics[diagnostics.Size() - 1].pathError =
                        LogicalPathError::InvalidComponent;
                }

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

            if (auto indexed = IndexFile(files, editor, Move(relative), child.info.modifiedTicks,
                                         found, diagnostics);
                !indexed) {
                return indexed;
            }
        }
    }

    return Publish(found, diagnostics);
}

Result<const AssetIndexEntry*> AssetIndex::FindById(AssetId id) const noexcept {
    using Out = const AssetIndexEntry*;
    const usize low = algo::LowerBound(byId_.View(), id, [&](usize index, AssetId key) noexcept {
        return CompareId(entries_[index].id, key) < 0;
    });
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
    const usize low = algo::LowerBound(
        entries_.View(), path.Key(), [](const AssetIndexEntry& entry, StringView key) noexcept {
            return StringView::CompareBytes(entry.key.View(), key) < 0;
        });

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
            } else if (types != nullptr && dependency.kind == PackageDependencyKind::Hard &&
                       !(target.Value()->type == dependency.expectedType)) {
                // 형식 대조는 로드와 같은 판정(expectedType의 파생이면 받음 — 5.7절 14)이라
                // TypeRegistry가 있을 때만 한다. 없으면 파생 여부를 알 수 없어 대조하지 않는다.
                // Hard는 반드시 expectedType을 가진다(패키지 읽기가 검사한다 — 5.2절).
                bool derived = false;
                auto isA = types->IsA(target.Value()->type, dependency.expectedType);
                if (isA) {
                    derived = isA.Value();
                } else if (isA.Error() != ErrorCode::NotFound &&
                           isA.Error() != ErrorCode::TypeMismatch) {
                    return Result<void>::Failure(isA.Error());
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
    if (auto formatted = line.TryAppendFormat("{}: Content/", code); !formatted) {
        return formatted;
    }

    if (auto appended = line.TryAppend(diagnostic.fileName.View()); !appended) {
        return appended;
    }

    if (auto appended = line.TryAppend(": "); !appended) {
        return appended;
    }

    switch (diagnostic.code) {
    case AssetDiagnosticCode::None:
        return line.TryAppend("cannot read the file or directory\n");
    case AssetDiagnosticCode::InvalidPath:
        return line.TryAppendFormat("invalid logical path ({})\n",
                                    LogicalPathErrorName(diagnostic.pathError));
    case AssetDiagnosticCode::MissingFile:
        return line.TryAppend("the file disappeared during the scan\n");
    case AssetDiagnosticCode::DuplicateAssetId:
    case AssetDiagnosticCode::DuplicatePath: {
        auto formatted =
            diagnostic.code == AssetDiagnosticCode::DuplicateAssetId
                ? line.TryAppendFormat("asset id {} is also in 'Content/", diagnostic.asset)
                : line.TryAppend("the logical path is also used by 'Content/");
        if (!formatted) {
            return formatted;
        }

        if (auto appended = line.TryAppend(diagnostic.related.View()); !appended) {
            return appended;
        }

        return line.TryAppend("'\n");
    }
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
