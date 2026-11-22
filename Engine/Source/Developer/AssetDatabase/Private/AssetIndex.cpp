#include "AssetDatabaseInternal.h"

#include <AssetDatabase/AssetIndex.h>
#include <AssetDatabase/AssetIndexCache.h>
#include <AssetDatabase/ImportRecord.h>
#include <AssetRuntime/PackageRangeReader.h>
#include <Core/Algorithms.h>
#include <Core/Assert.h>
#include <Core/FileError.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/Unicode.h>
#include <Object/AssetId.h>
#include <Object/TypeRegistry.h>
#include <Task/ParallelFor.h>

// 에디터 패키지 색인이다(Docs/Design/AssetSystem.ko.md 7절, 7.1절 11·13 — A2b).
namespace rocks {

namespace {

using asset_database_detail::AppendAll;
using asset_database_detail::AssignDisplayName;
using asset_database_detail::AssignDisplayNameAs;
using asset_database_detail::IsContentRelativeName;
using asset_database_detail::NameSet;

// 경로 임시 저장소의 인라인 byte 수다(넘으면 allocator로 자란다).
constexpr usize IndexPathBytes = 512;
// 병렬 읽기 batch 하나의 최소 파일 수다(파일 하나는 열기 + 범위 읽기 몇 번 — 분배 비용을 나눈다).
constexpr usize ScanBatchFiles = 8;

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
        if (auto reserved = summary.outputs.TryReserve(record.outputs.Size()); !reserved) {
            return reserved;
        }

        for (const ImportOutput& output : record.outputs) {
            (void)summary.outputs.TryPushBack(output.asset);
        }
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
// 정렬 순서 order(확정 위치)에서 같은 값(same)이 이웃한 항목을 모두 flags에 flag로 표시하고 code
// 진단을 남긴다. 겹친 상대(related)는 앞 이웃이 같으면 그것, 아니면 뒤 이웃이다(같은 값이 셋
// 이상이어도 각자 이웃 하나).
template <class TSame>
Result<void> DiagnoseNeighbors(Span<AssetIndexEntry* const> entries, Span<const usize> order,
                               TSame same, Array<uint8>& flags, uint8 flag,
                               AssetDiagnosticCode code,
                               Array<AssetIndexDiagnostic>& diagnostics) noexcept {
    for (usize position = 0; position < order.Size(); ++position) {
        const bool before = position > 0 && same(order[position - 1], order[position]);
        const bool after =
            position + 1 < order.Size() && same(order[position], order[position + 1]);
        if (!before && !after) {
            continue;
        }

        const AssetIndexEntry& entry = *entries[order[position]];
        flags[order[position]] = static_cast<uint8>(flags[order[position]] | flag);
        if (auto added =
                AddDiagnostic(diagnostics, code, ErrorCode::AlreadyExists, entry.fileName.View());
            !added) {
            return added;
        }

        AssetIndexDiagnostic& diagnostic = diagnostics[diagnostics.Size() - 1];
        diagnostic.asset = entry.id;
        const usize partner = order[before ? position - 1 : position + 1];
        if (auto assigned =
                AssignDisplayName(diagnostic.related, entries[partner]->fileName.View());
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

// 같은 id·경로 진단인지다(게시가 전체에서 다시 만든다).
bool IsNeighborDiagnostic(const AssetIndexDiagnostic& diagnostic) noexcept {
    return diagnostic.code == AssetDiagnosticCode::DuplicateAssetId ||
           diagnostic.code == AssetDiagnosticCode::DuplicatePath;
}

// name(유효한 UTF-8 — 입력 이름은 검사했고 항목·진단 이름은 늘 유효하다)의 비교 키다: 정규화 파일
// 시스템이면 NFC를 scratch에 만들어 그 view, 아니면 name 그대로다(복사하지 않는다). 다음 호출
// 전까지 유효하다. 오류: 할당 실패(유효한 UTF-8의 정규화가 실패하는 유일한 경우).
Result<StringView> CompareKey(StringView name, bool normalizes, String& scratch) noexcept {
    if (!normalizes) {
        return Result<StringView>::Success(name);
    }

    StringBuilder<IndexPathBytes> normalized(scratch.GetAllocator());
    if (auto made = TryNormalize(NormalizationForm::NFC, name, normalized); !made) {
        return Result<StringView>::Failure(made.Error());
    }

    if (auto assigned = scratch.TryAssign(normalized.View()); !assigned) {
        return Result<StringView>::Failure(assigned.Error());
    }

    return Result<StringView>::Success(scratch.View());
}

// names의 비교 키(keys — names와 같은 첨자)와 키 → 첨자 순 위치(order)를 만든다.
Result<void> SortByKey(Span<const StringView> names, bool normalizes, Array<String>& keys,
                       Array<usize>& order) noexcept {
    String scratch(keys.GetAllocator());
    if (auto reserved = keys.TryReserve(names.Size()); !reserved) {
        return reserved;
    }

    for (StringView name : names) {
        auto key = CompareKey(name, normalizes, scratch);
        if (!key) {
            return Result<void>::Failure(key.Error());
        }

        String copy(keys.GetAllocator());
        if (auto assigned = copy.TryAssign(key.Value()); !assigned) {
            return assigned;
        }

        (void)keys.TryPushBack(Move(copy));
    }

    if (auto made = MakeOrder(order, names.Size()); !made) {
        return made;
    }

    algo::Sort(order.View(), [&](usize left, usize right) noexcept {
        const int compared = StringView::CompareBytes(keys[left].View(), keys[right].View());
        return compared != 0 ? compared < 0 : left < right;
    });
    return Result<void>::Success();
}

} // namespace

void AssetIndexRefresh::Reset() noexcept {
    ready_ = false;
    added_.Clear();
    fileDiagnostics_.Clear();
    neighbors_.Clear();
    entryOrder_.Clear();
    diagnosticOrder_.Clear();
    entries_.Clear();
    diagnostics_.Clear();
}

AssetIndex::AssetIndex(Allocator& allocator, const ProjectLayout& layout,
                       WorkerScheduler* scheduler) noexcept
    : allocator_(&allocator), layout_(&layout), scheduler_(scheduler), entries_(allocator),
      byId_(allocator), diagnostics_(allocator) {
}

// 패키지 파일 하나(Content 상대 이름 relative)를 부분 읽기해 found에 항목을, diagnostics에 그
// 파일의 진단을 더한다(Rebuild·Refresh 공용). 이름 규칙 위반·패키지 판정·IO 실패는 진단이고 항목을
// 더하지 않는다. 오류: 할당 실패.
Result<void> AssetIndex::IndexFile(platform::FileSystem& files, const PackageReadOptions& editor,
                                   String&& relative, StringView readPath, int64 modifiedTicks,
                                   Array<AssetIndexEntry>& found,
                                   Array<AssetIndexDiagnostic>& diagnostics) const noexcept {
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

    // 읽을 경로는 Content/relative, 또는 준 readPath(같은 내용의 다른 이름 — 저장의 임시 파일)다.
    StringBuilder<IndexPathBytes> full(*allocator_);
    const StringView fullPieces[] = {content, "/", relative.View()};
    if (readPath.IsEmpty()) {
        if (auto built = AppendAll(full, fullPieces); !built) {
            return built;
        }
    }

    FilePackageRangeReader reader;
    AssetDiagnostic failure;
    auto opened = reader.Open(files, readPath.IsEmpty() ? full.View() : readPath);
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

// 같은 경로 표시(flags bit)와 같은 id 표시다.
constexpr uint8 DuplicatePathFlag = 1;
constexpr uint8 DuplicateIdFlag = 2;

Result<void> AssetIndex::PlanPublish(Span<AssetIndexEntry* const> entries,
                                     Span<AssetIndexDiagnostic* const> diagnostics,
                                     AssetIndexRefresh& plan) const noexcept {
    // 경로 키 → 파일 이름 순으로 정렬한다(완료 순서와 무관 — 7.1절 9). Core 정렬은 안정 정렬이
    // 아니지만 Content 상대 이름은 파일마다 달라 이 비교가 전순서다.
    Array<usize> order(*allocator_);
    if (auto made = MakeOrder(order, entries.Size()); !made) {
        return made;
    }

    algo::Sort(order.View(), [&](usize left, usize right) noexcept {
        int compared =
            StringView::CompareBytes(entries[left]->key.View(), entries[right]->key.View());
        if (compared == 0) {
            compared = StringView::CompareBytes(entries[left]->fileName.View(),
                                                entries[right]->fileName.View());
        }

        return compared < 0;
    });
    if (auto reserved = plan.entryOrder_.TryReserve(entries.Size()); !reserved) {
        return reserved;
    }

    for (usize index : order) {
        (void)plan.entryOrder_.TryPushBack(entries[index]);
    }

    if (auto sized = plan.flags_.TryResize(entries.Size()); !sized) {
        return sized;
    }

    for (uint8& flag : plan.flags_) {
        flag = 0;
    }

    // 같은 경로 키(DuplicatePath — 경로 키 순에서 이웃)와 같은 id(DuplicateAssetId — id 순에서
    // 이웃)를 모두 진단한다. 위치는 확정 순서다.
    const Span<AssetIndexEntry* const> sorted = plan.entryOrder_.View();
    Array<usize> byId(*allocator_);
    if (auto made = MakeOrder(byId, sorted.Size()); !made) {
        return made;
    }

    if (auto diagnosed = DiagnoseNeighbors(
            sorted, byId.View(),
            [&](usize left, usize right) noexcept {
                return sorted[left]->key.View() == sorted[right]->key.View();
            },
            plan.flags_, DuplicatePathFlag, AssetDiagnosticCode::DuplicatePath, plan.neighbors_);
        !diagnosed) {
        return diagnosed;
    }

    // id → 경로 순 위치(같은 id는 경로 순).
    algo::Sort(byId.View(), [&](usize left, usize right) noexcept {
        const int compared = CompareId(sorted[left]->id, sorted[right]->id);
        return compared != 0 ? compared < 0 : left < right;
    });
    if (auto diagnosed = DiagnoseNeighbors(
            sorted, byId.View(),
            [&](usize left, usize right) noexcept {
                return CompareId(sorted[left]->id, sorted[right]->id) == 0;
            },
            plan.flags_, DuplicateIdFlag, AssetDiagnosticCode::DuplicateAssetId, plan.neighbors_);
        !diagnosed) {
        return diagnosed;
    }

    // 진단은 파일 이름 → 코드 → 칸 순이다. 한 이름(파일·디렉터리)의 진단은 (코드, 칸)마다
    // 하나뿐이라(경로·패키지 판정 진단은 서로 배타이고 그 밖의 항목 진단과도 배타, 항목 진단은
    // Tags·ImportRecord·Dependencies 칸마다 하나, 같은 id·경로 진단은 각각 하나 — 칸 0) 이 비교가
    // 전순서다.
    const usize diagnosticCount = diagnostics.Size() + plan.neighbors_.Size();
    if (auto reserved = plan.diagnosticOrder_.TryReserve(diagnosticCount); !reserved) {
        return reserved;
    }

    for (AssetIndexDiagnostic* diagnostic : diagnostics) {
        (void)plan.diagnosticOrder_.TryPushBack(diagnostic);
    }

    for (AssetIndexDiagnostic& diagnostic : plan.neighbors_) {
        (void)plan.diagnosticOrder_.TryPushBack(&diagnostic);
    }

    algo::Sort(plan.diagnosticOrder_.View(), [](const AssetIndexDiagnostic* left,
                                                const AssetIndexDiagnostic* right) noexcept {
        const int name = StringView::CompareBytes(left->fileName.View(), right->fileName.View());
        if (name != 0) {
            return name < 0;
        }

        if (left->code != right->code) {
            return static_cast<uint32>(left->code) < static_cast<uint32>(right->code);
        }

        return left->section < right->section;
    });
    if (auto reserved = plan.entries_.TryReserve(sorted.Size()); !reserved) {
        return reserved;
    }

    if (auto reserved = plan.diagnostics_.TryReserve(diagnosticCount); !reserved) {
        return reserved;
    }

    plan.byId_ = Move(byId);
    plan.revision_ = revision_;
    plan.ready_ = true;
    return Result<void>::Success();
}

void AssetIndex::CommitPublish(AssetIndexRefresh& plan) noexcept {
    ROCKS_ASSERT(plan.ready_ && plan.revision_ == revision_);
    for (usize position = 0; position < plan.entryOrder_.Size(); ++position) {
        AssetIndexEntry& entry = *plan.entryOrder_[position];
        entry.duplicatePath = (plan.flags_[position] & DuplicatePathFlag) != 0;
        entry.duplicateId = (plan.flags_[position] & DuplicateIdFlag) != 0;
        (void)plan.entries_.TryPushBack(Move(entry));
    }

    for (AssetIndexDiagnostic* diagnostic : plan.diagnosticOrder_) {
        (void)plan.diagnostics_.TryPushBack(Move(*diagnostic));
    }

    entries_ = Move(plan.entries_);
    byId_ = Move(plan.byId_);
    diagnostics_ = Move(plan.diagnostics_);
    ++revision_;
    plan.ready_ = false;
}

Result<bool> AssetIndex::IndexCached(platform::FileSystem& files, const AssetIndexCache& cache,
                                     const String& relative, int64 modifiedTicks,
                                     Array<AssetIndexEntry>& found) const noexcept {
    // 이름이 캐시에 없으면 읽지 않는다(지문 읽기 비용 없이 부분 읽기로 간다).
    if (cache.Count() == 0) {
        return Result<bool>::Success(false);
    }

    String full(*allocator_);
    const StringView pieces[] = {layout_->ContentDirectory(), "/", relative.View()};
    for (StringView piece : pieces) {
        if (auto appended = full.TryAppend(piece); !appended) {
            return Result<bool>::Failure(appended.Error());
        }
    }

    FilePackageRangeReader reader;
    if (auto opened = reader.Open(files, full.View()); !opened) {
        return Result<bool>::Success(false);
    }

    auto fingerprint = ReadPackageFingerprint(*allocator_, reader);
    if (!fingerprint) {
        return fingerprint.Error() == ErrorCode::OutOfMemory
                   ? Result<bool>::Failure(fingerprint.Error())
                   : Result<bool>::Success(false);
    }

    const AssetIndexCacheEntry* cached = cache.Find(relative.View(), fingerprint.Value());
    if (cached == nullptr) {
        return Result<bool>::Success(false);
    }

    // 논리 경로·key는 캐시에 두지 않고 이름에서 다시 만든다(신뢰하지 않는 입력 — 7.1절 16).
    auto path = layout_->TryMakeLogicalPath(relative.View());
    if (!path) {
        return path.Error() == ErrorCode::OutOfMemory ? Result<bool>::Failure(path.Error())
                                                      : Result<bool>::Success(false);
    }

    AssetIndexEntry entry(*allocator_);
    entry.id = cached->id;
    entry.type = cached->type;
    entry.fingerprint = cached->fingerprint;
    entry.fileBytes = reader.FileBytes();
    entry.modifiedTicks = modifiedTicks;
    entry.readOnly = cached->readOnly;
    entry.importRecord.present = cached->importRecord.present;
    entry.importRecord.owner = cached->importRecord.owner;
    entry.importRecord.importerId = cached->importRecord.importerId;
    entry.importRecord.importerVersion = cached->importRecord.importerVersion;
    Result<void> step = entry.importRecord.outputs.TryAppend(cached->importRecord.outputs.View());
    if (step) {
        step = entry.path.TryAssign(path.Value().Text());
    }
    if (step) {
        step = entry.key.TryAssign(path.Value().Key());
    }

    if (step) {
        step = entry.fileName.TryAssign(relative.View());
    }

    if (step) {
        step = entry.dependencies.TryAppend(cached->dependencies.View());
    }

    if (step) {
        step = entry.importRecord.importerName.TryAssign(cached->importRecord.importerName.View());
    }

    if (step) {
        step = entry.tags.TryReserve(cached->tags.Size());
    }

    for (usize index = 0; step && index < cached->tags.Size(); ++index) {
        PackageTag tag(*allocator_);
        step = tag.key.TryAssign(cached->tags[index].key.View());
        if (step) {
            step = tag.value.TryAssign(cached->tags[index].value.View());
        }

        if (step) {
            (void)entry.tags.TryPushBack(Move(tag));
        }
    }

    if (step) {
        step = found.TryPushBack(Move(entry));
    }

    if (!step) {
        return Result<bool>::Failure(step.Error());
    }

    return Result<bool>::Success(true);
}

Result<void> AssetIndex::ScanTree(platform::FileSystem& files, const PackageReadOptions& editor,
                                  const AssetIndexCache* cache, StringView start,
                                  StringView renameTo, Array<AssetIndexEntry>& found,
                                  Array<AssetIndexDiagnostic>& diagnostics) const noexcept {
    const StringView content = layout_->ContentDirectory();
    // 이름 바꾸기(renameTo — 7.1절 17 A2e2): start 아래 이름을 renameTo 아래 이름으로 색인하고
    // 내용은 지금 이름(start 아래)에서 읽는다(이동 전에 준비한다). 이름 앞부분을 바꾼다.
    auto named = [&](StringView relative, Array<char>& out) noexcept -> Result<void> {
        out.Clear();
        const StringView head = renameTo.IsEmpty() ? relative : renameTo;
        const StringView tail = renameTo.IsEmpty()
                                    ? StringView()
                                    : StringView(relative.Data() + start.ByteCount(),
                                                 relative.ByteCount() - start.ByteCount());
        if (auto appended = out.TryAppend(Span<const char>(head.Data(), head.ByteCount()));
            !appended) {
            return appended;
        }

        return out.TryAppend(Span<const char>(tail.Data(), tail.ByteCount()));
    };
    // 이름 바꾸기면 읽을 경로(Content/relative)다.
    auto readPathOf = [&](StringView relative, String& out) noexcept -> Result<void> {
        out.Clear();
        if (renameTo.IsEmpty()) {
            return Result<void>::Success();
        }

        const StringView pieces[] = {content, "/", relative};
        for (StringView piece : pieces) {
            if (auto appended = out.TryAppend(piece); !appended) {
                return appended;
            }
        }

        return Result<void>::Success();
    };
    Array<char> mapped(*allocator_);
    // 깊이 우선으로 디렉터리를 연다(Content 상대 경로 표 — 빈 문자열은 Content 자신). 패키지
    // 후보는 모아 두고 걷기가 끝난 뒤 읽는다(ReadCandidates — scheduler가 있으면 병렬).
    Array<String> pending(*allocator_);
    Array<ScanCandidate> candidates(*allocator_);
    String first(*allocator_);
    if (auto assigned = first.TryAssign(start); !assigned) {
        return assigned;
    }

    // 하위 트리의 시작 이름은 먼저 Stat한다: 없으면 비어 있고, 링크·그 밖의 종류면 그 아래가 없고,
    // 패키지 파일이면 그 파일을 읽는다. 그 밖의 Stat 실패는 하위 디렉터리 열거 실패와 같은
    // 진단이다.
    if (!start.IsEmpty()) {
        String full(*allocator_);
        const StringView pieces[] = {content, "/", start};
        for (StringView piece : pieces) {
            if (auto appended = full.TryAppend(piece); !appended) {
                return appended;
            }
        }

        platform::FileInfo info;
        const platform::FileStatus stat =
            files.Stat(full.View().Data(), full.View().ByteCount(), info);
        if (!stat) {
            if (stat.code == platform::FileCode::NotFound) {
                return Result<void>::Success();
            }

            if (auto renamed = named(start, mapped); !renamed) {
                return renamed;
            }

            return AddDiagnostic(diagnostics, AssetDiagnosticCode::None, FileErrorCode(stat),
                                 StringView(mapped.Data(), mapped.Size()));
        }

        if (info.isLink) {
            return Result<void>::Success();
        }

        if (info.type == platform::FileType::File && start.EndsWith(EditorPackageExtension)) {
            String name(*allocator_);
            if (auto renamed = named(start, mapped); !renamed) {
                return renamed;
            }

            if (auto assigned = name.TryAssign(StringView(mapped.Data(), mapped.Size()));
                !assigned) {
                return assigned;
            }

            return IndexFile(files, editor, Move(name),
                             renameTo.IsEmpty() ? StringView() : full.View(), info.modifiedTicks,
                             found, diagnostics);
        }

        if (info.type != platform::FileType::Directory) {
            return Result<void>::Success();
        }
    }

    if (auto pushed = pending.TryPushBack(Move(first)); !pushed) {
        return pushed;
    }

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

            if (auto renamed = named(relativeDirectory.View(), mapped); !renamed) {
                return renamed;
            }

            if (auto added =
                    AddDiagnostic(diagnostics, AssetDiagnosticCode::None, FileErrorCode(listed),
                                  StringView(mapped.Data(), mapped.Size()));
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

                    if (auto renamed =
                            named(StringView(rawRelative.Data(), rawRelative.Size()), mapped);
                        !renamed) {
                        return renamed;
                    }

                    if (auto added = AddDiagnostic(diagnostics, AssetDiagnosticCode::InvalidPath,
                                                   ErrorCode::InvalidArgument,
                                                   StringView(mapped.Data(), mapped.Size()));
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

            ScanCandidate candidate(*allocator_);
            if (auto read = readPathOf(relative.View(), candidate.readPath); !read) {
                return read;
            }

            if (auto renamed = named(relative.View(), mapped); !renamed) {
                return renamed;
            }

            if (auto assigned =
                    candidate.relative.TryAssign(StringView(mapped.Data(), mapped.Size()));
                !assigned) {
                return assigned;
            }

            candidate.modifiedTicks = child.info.modifiedTicks;
            if (auto pushed = candidates.TryPushBack(Move(candidate)); !pushed) {
                return pushed;
            }
        }
    }

    return ReadCandidates(files, editor, cache, candidates, found, diagnostics);
}

Result<void> AssetIndex::ReadCandidates(platform::FileSystem& files,
                                        const PackageReadOptions& editor,
                                        const AssetIndexCache* cache,
                                        Array<ScanCandidate>& candidates,
                                        Array<AssetIndexEntry>& found,
                                        Array<AssetIndexDiagnostic>& diagnostics) const noexcept {
    // 후보마다 결과 칸(항목 0·1개 + 그 파일 진단)을 둔다 — 작업마다 겹치지 않는 칸에 쓴다.
    Array<ScanSlot> slots(*allocator_);
    if (auto reserved = slots.TryReserve(candidates.Size()); !reserved) {
        return reserved;
    }

    for (usize at = 0; at < candidates.Size(); ++at) {
        (void)slots.TryPushBack(ScanSlot(*allocator_));
    }

    auto read = [&](usize begin, usize end) noexcept {
        for (usize at = begin; at < end; ++at) {
            ScanCandidate& candidate = candidates[at];
            ScanSlot& slot = slots[at];
            if (cache != nullptr) {
                auto reused = IndexCached(files, *cache, candidate.relative,
                                          candidate.modifiedTicks, slot.found);
                if (!reused) {
                    return reused.Error();
                }

                if (reused.Value()) {
                    continue;
                }
            }

            if (auto indexed =
                    IndexFile(files, editor, Move(candidate.relative), candidate.readPath.View(),
                              candidate.modifiedTicks, slot.found, slot.diagnostics);
                !indexed) {
                return indexed.Error();
            }
        }

        return ErrorCode::None;
    };
    if (scheduler_ != nullptr) {
        ParallelForOptions options;
        options.minBatchSize = ScanBatchFiles;
        if (auto done = ParallelFor(*scheduler_, *allocator_, candidates.Size(), read, options);
            !done) {
            return done;
        }
    } else if (const ErrorCode failed = read(0, candidates.Size()); failed != ErrorCode::None) {
        return Result<void>::Failure(failed);
    }

    // 후보 순서대로 모은다(게시가 정렬하므로 결과는 직렬과 같다).
    usize entries = 0;
    usize problems = 0;
    for (const ScanSlot& slot : slots) {
        entries += slot.found.Size();
        problems += slot.diagnostics.Size();
    }

    if (auto reserved = found.TryReserve(found.Size() + entries); !reserved) {
        return reserved;
    }

    if (auto reserved = diagnostics.TryReserve(diagnostics.Size() + problems); !reserved) {
        return reserved;
    }

    for (ScanSlot& slot : slots) {
        for (AssetIndexEntry& entry : slot.found) {
            (void)found.TryPushBack(Move(entry));
        }

        for (AssetIndexDiagnostic& diagnostic : slot.diagnostics) {
            (void)diagnostics.TryPushBack(Move(diagnostic));
        }
    }

    return Result<void>::Success();
}

Result<void> AssetIndex::Rebuild(platform::FileSystem& files, const PackageReadOptions& options,
                                 const AssetIndexCache* cache) noexcept {
    entries_.Clear();
    byId_.Clear();
    diagnostics_.Clear();
    Array<AssetIndexEntry> found(*allocator_);
    Array<AssetIndexDiagnostic> diagnostics(*allocator_);
    PackageReadOptions editor = options;
    editor.expectedKind = PackageKind::Editor;
    // 색인은 형식 판정만 한다(타입 축은 열기가 — 캐시 키에 타입 버전이 없다, 7.1절 16).
    editor.types = nullptr;
    if (auto scanned =
            ScanTree(files, editor, cache, StringView(), StringView(), found, diagnostics);
        !scanned) {
        return scanned;
    }

    // 게시(같은 id·경로 진단 — 실패하면 비운 색인 그대로).
    Array<AssetIndexEntry*> entryPlaces(*allocator_);
    Array<AssetIndexDiagnostic*> diagnosticPlaces(*allocator_);
    if (auto reserved = entryPlaces.TryReserve(found.Size()); !reserved) {
        return reserved;
    }

    if (auto reserved = diagnosticPlaces.TryReserve(diagnostics.Size()); !reserved) {
        return reserved;
    }

    for (AssetIndexEntry& entry : found) {
        (void)entryPlaces.TryPushBack(&entry);
    }

    for (AssetIndexDiagnostic& diagnostic : diagnostics) {
        (void)diagnosticPlaces.TryPushBack(&diagnostic);
    }

    AssetIndexRefresh plan(*allocator_);
    if (auto planned = PlanPublish(entryPlaces.View(), diagnosticPlaces.View(), plan); !planned) {
        return planned;
    }

    CommitPublish(plan);
    return Result<void>::Success();
}

Result<void> AssetIndex::Refresh(platform::FileSystem& files, StringView fileName,
                                 const PackageReadOptions& options) noexcept {
    AssetIndexRefresh plan(*allocator_);
    if (auto prepared = PrepareRefresh(files, fileName, StringView(), options, plan); !prepared) {
        return prepared;
    }

    CommitRefresh(plan);
    return Result<void>::Success();
}

Result<void> AssetIndex::RereadFile(platform::FileSystem& files, const PackageReadOptions& editor,
                                    StringView fileName, StringView readPath,
                                    Array<AssetIndexEntry>& found,
                                    Array<AssetIndexDiagnostic>& diagnostics) const noexcept {
    String full(*allocator_);
    const StringView pieces[] = {layout_->ContentDirectory(), "/", fileName};
    if (readPath.IsEmpty()) {
        for (StringView piece : pieces) {
            if (auto appended = full.TryAppend(piece); !appended) {
                return appended;
            }
        }
    }

    // 없어진 파일은 빠지고, 링크·일반 파일이 아닌 것은 색인하지 않는다(Rebuild와 같다). 그 밖의
    // Stat 실패는 IO 진단이다.
    const StringView statPath = readPath.IsEmpty() ? full.View() : readPath;
    platform::FileInfo info;
    const platform::FileStatus stat = files.Stat(statPath.Data(), statPath.ByteCount(), info);
    if (!stat) {
        return stat.code == platform::FileCode::NotFound
                   ? Result<void>::Success()
                   : AddDiagnostic(diagnostics, AssetDiagnosticCode::None, FileErrorCode(stat),
                                   fileName);
    }

    if (info.isLink || info.type != platform::FileType::File) {
        return Result<void>::Success();
    }

    String relative(*allocator_);
    if (auto assigned = relative.TryAssign(fileName); !assigned) {
        return assigned;
    }

    return IndexFile(files, editor, Move(relative), readPath, info.modifiedTicks, found,
                     diagnostics);
}

Result<void> AssetIndex::PrepareRefresh(platform::FileSystem& files, StringView fileName,
                                        StringView readPath, const PackageReadOptions& options,
                                        AssetIndexRefresh& plan) noexcept {
    plan.Reset();
    if (fileName.ByteCount() <= EditorPackageExtension.ByteCount() ||
        !fileName.EndsWith(EditorPackageExtension)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    String display(*allocator_);
    if (auto assigned = AssignDisplayName(display, fileName); !assigned) {
        return assigned;
    }

    PackageReadOptions editor = options;
    editor.expectedKind = PackageKind::Editor;
    editor.types = nullptr;
    if (auto reread =
            RereadFile(files, editor, fileName, readPath, plan.added_, plan.fileDiagnostics_);
        !reread) {
        return reread;
    }

    // 지금 색인의 다른 파일 항목·진단(같은 id·경로 진단 제외 — 게시가 다시 만든다)과 새 것을
    // 자리로 모은다.
    Array<AssetIndexEntry*> entryPlaces(*allocator_);
    Array<AssetIndexDiagnostic*> diagnosticPlaces(*allocator_);
    if (auto reserved = entryPlaces.TryReserve(entries_.Size() + plan.added_.Size()); !reserved) {
        return reserved;
    }

    if (auto reserved =
            diagnosticPlaces.TryReserve(diagnostics_.Size() + plan.fileDiagnostics_.Size());
        !reserved) {
        return reserved;
    }

    // 자리 포인터는 확정이 옮길 대상이다(준비는 색인을 바꾸지 않고 가리키기만 한다).
    for (AssetIndexEntry& entry : entries_) {
        if (!(entry.fileName.View() == fileName)) {
            (void)entryPlaces.TryPushBack(&entry);
        }
    }

    for (AssetIndexEntry& entry : plan.added_) {
        (void)entryPlaces.TryPushBack(&entry);
    }

    for (AssetIndexDiagnostic& diagnostic : diagnostics_) {
        if (!(diagnostic.fileName.View() == display.View()) && !IsNeighborDiagnostic(diagnostic)) {
            (void)diagnosticPlaces.TryPushBack(&diagnostic);
        }
    }

    for (AssetIndexDiagnostic& diagnostic : plan.fileDiagnostics_) {
        (void)diagnosticPlaces.TryPushBack(&diagnostic);
    }

    return PlanPublish(entryPlaces.View(), diagnosticPlaces.View(), plan);
}

Result<void> AssetIndex::RefreshPaths(platform::FileSystem& files,
                                      const AssetIndexRefreshPaths& paths,
                                      const PackageReadOptions& options) noexcept {
    AssetIndexRefresh plan(*allocator_);
    if (auto prepared = PrepareRefreshPaths(files, paths, options, plan); !prepared) {
        return prepared;
    }

    CommitRefresh(plan);
    return Result<void>::Success();
}

Result<void> AssetIndex::PrepareRefreshPaths(platform::FileSystem& files,
                                             const AssetIndexRefreshPaths& paths,
                                             const PackageReadOptions& options,
                                             AssetIndexRefresh& plan) noexcept {
    plan.Reset();
    if (!paths.mayDefer.IsEmpty() &&
        (paths.mayDefer.Size() != paths.files.Size() || paths.deferred == nullptr)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (StringView file : paths.files) {
        if (!IsContentRelativeName(file) ||
            file.ByteCount() <= EditorPackageExtension.ByteCount() ||
            !file.EndsWith(EditorPackageExtension)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    for (StringView subtree : paths.subtrees) {
        if (!subtree.IsEmpty() && !IsContentRelativeName(subtree)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    for (const AssetIndexMove& move : paths.moves) {
        if (!IsContentRelativeName(move.from) || !IsContentRelativeName(move.to)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    for (StringView name : paths.removed) {
        if (!IsContentRelativeName(name)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    const bool normalizes = paths.normalizesNames;
    // 정규화 키의 임시 저장소다(정규화하지 않으면 이름을 그대로 쓴다 — 항목마다 복사하지 않는다).
    String scratch(*allocator_);
    PackageReadOptions editor = options;
    editor.expectedKind = PackageKind::Editor;
    editor.types = nullptr;

    // 하위 트리 집합과 다시 검색할 뿌리(같은 키는 하나, 다른 트리 아래의 트리는 뺀다 — 원래
    // 철자로 연다), 그 표시 이름 집합(진단 대조 — 유효한 UTF-8 이름 아래 진단의 표시와 잘못된
    // UTF-8 이름 아래 진단의 표시 둘 다).
    Array<String> treeKeys(*allocator_);
    Array<usize> treeOrder(*allocator_);
    if (auto sorted = SortByKey(paths.subtrees, normalizes, treeKeys, treeOrder); !sorted) {
        return sorted;
    }

    NameSet trees(*allocator_);
    for (const String& key : treeKeys) {
        if (auto added = trees.Add(key.View()); !added) {
            return added;
        }
    }

    trees.Finish();
    Array<StringView> roots(*allocator_);
    for (usize first = 0; first < treeOrder.Size();) {
        usize last = first + 1;
        while (last < treeOrder.Size() &&
               treeKeys[treeOrder[last]].View() == treeKeys[treeOrder[first]].View()) {
            ++last;
        }

        if (!trees.Above(treeKeys[treeOrder[first]].View())) {
            if (auto pushed = roots.TryPushBack(paths.subtrees[treeOrder[first]]); !pushed) {
                return pushed;
            }
        }

        first = last;
    }

    NameSet treeDisplays(*allocator_);
    for (StringView root : roots) {
        String display(*allocator_);
        String escaped(*allocator_);
        if (auto assigned = AssignDisplayNameAs(display, root, false); !assigned) {
            return assigned;
        }

        if (auto assigned = AssignDisplayNameAs(escaped, root, true); !assigned) {
            return assigned;
        }

        const String* shownNames[] = {&display, &escaped};
        for (const String* shown : shownNames) {
            if (auto key = CompareKey(shown->View(), normalizes, scratch); !key) {
                return Result<void>::Failure(key.Error());
            } else if (auto added = treeDisplays.Add(key.Value()); !added) {
                return added;
            }
        }
    }

    // 이름 바꾸기의 두 이름과 지우는 이름(빼기 — 그 이름과 그 아래)과 표시 이름이다. 같은 키가
    // 아니면 이름 바꾸기의 두 이름은 서로의 아래일 수 없다(디렉터리를 자기 안으로·자기 위로
    // 옮기기).
    NameSet moved(*allocator_);
    auto dropName = [&](StringView name) noexcept -> Result<void> {
        auto key = CompareKey(name, normalizes, scratch);
        if (!key) {
            return Result<void>::Failure(key.Error());
        }

        if (auto added = moved.Add(key.Value()); !added) {
            return added;
        }

        String display(*allocator_);
        String escaped(*allocator_);
        if (auto assigned = AssignDisplayNameAs(display, name, false); !assigned) {
            return assigned;
        }

        if (auto assigned = AssignDisplayNameAs(escaped, name, true); !assigned) {
            return assigned;
        }

        const String* shownNames[] = {&display, &escaped};
        for (const String* shown : shownNames) {
            if (auto shownKey = CompareKey(shown->View(), normalizes, scratch); !shownKey) {
                return Result<void>::Failure(shownKey.Error());
            } else if (auto added = treeDisplays.Add(shownKey.Value()); !added) {
                return added;
            }
        }

        return Result<void>::Success();
    };
    for (const AssetIndexMove& move : paths.moves) {
        String fromKey(*allocator_);
        auto from = CompareKey(move.from, normalizes, scratch);
        if (!from) {
            return Result<void>::Failure(from.Error());
        }

        if (auto assigned = fromKey.TryAssign(from.Value()); !assigned) {
            return assigned;
        }

        auto to = CompareKey(move.to, normalizes, scratch);
        if (!to) {
            return Result<void>::Failure(to.Error());
        }

        NameSet one(*allocator_);
        if (auto added = one.Add(fromKey.View()); !added) {
            return added;
        }

        one.Finish();
        if (fromKey.View() != to.Value() && one.Covers(to.Value())) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        NameSet other(*allocator_);
        if (auto added = other.Add(to.Value()); !added) {
            return added;
        }

        other.Finish();
        if (fromKey.View() != to.Value() && other.Covers(fromKey.View())) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const StringView names[] = {move.from, move.to};
        for (StringView name : names) {
            if (auto dropped = dropName(name); !dropped) {
                return dropped;
            }
        }
    }

    for (StringView name : paths.removed) {
        if (auto dropped = dropName(name); !dropped) {
            return dropped;
        }
    }

    moved.Finish();
    treeDisplays.Finish();

    // 파일을 키 순으로 묶는다(같은 키는 한 번 — 처음 철자로 읽는다). 하위 트리가 덮는 파일은 읽지
    // 않는다.
    Array<String> fileKeys(*allocator_);
    Array<usize> order(*allocator_);
    if (auto sorted = SortByKey(paths.files, normalizes, fileKeys, order); !sorted) {
        return sorted;
    }

    NameSet refreshed(*allocator_);
    NameSet refreshedDisplays(*allocator_);
    for (usize first = 0; first < order.Size();) {
        const StringView name = paths.files[order[first]];
        const StringView key = fileKeys[order[first]].View();
        usize last = first + 1;
        bool defer = !paths.mayDefer.IsEmpty();
        while (last < order.Size() && fileKeys[order[last]].View() == key) {
            ++last;
        }

        for (usize at = first; defer && at < last; ++at) {
            defer = paths.mayDefer[order[at]];
        }

        if (trees.Covers(key)) {
            first = last;
            continue;
        }

        const usize entriesBefore = plan.added_.Size();
        const usize diagnosticsBefore = plan.fileDiagnostics_.Size();
        if (auto reread =
                RereadFile(files, editor, name, StringView(), plan.added_, plan.fileDiagnostics_);
            !reread) {
            return reread;
        }

        // 쓰는 중: 패키지를 읽지 못해(항목 없음) 진단이 하나 — 손상이거나 열기·읽기가 Busy다.
        // 읽혀서 항목이 있는 파일의 진단(Tags·ImportRecord 손상 등)은 다 쓴 파일이라 미루지 않는다.
        bool inProgress = false;
        if (plan.added_.Size() == entriesBefore &&
            plan.fileDiagnostics_.Size() == diagnosticsBefore + 1) {
            const AssetIndexDiagnostic& diagnostic = plan.fileDiagnostics_[diagnosticsBefore];
            inProgress = diagnostic.code == AssetDiagnosticCode::CorruptPackage ||
                         (diagnostic.code == AssetDiagnosticCode::None &&
                          diagnostic.error == ErrorCode::Busy);
        }

        if (defer && inProgress) {
            // 다시 읽은 진단을 버리고 지금 항목·진단을 그대로 둔다.
            (void)plan.fileDiagnostics_.TryPopBack();
            for (usize at = first; at < last; ++at) {
                if (auto pushed = paths.deferred->TryPushBack(order[at]); !pushed) {
                    return pushed;
                }
            }

            first = last;
            continue;
        }

        String display(*allocator_);
        if (auto assigned = AssignDisplayName(display, name); !assigned) {
            return assigned;
        }

        if (auto added = refreshed.Add(key); !added) {
            return added;
        }

        if (auto shown = CompareKey(display.View(), normalizes, scratch); !shown) {
            return Result<void>::Failure(shown.Error());
        } else if (auto added = refreshedDisplays.Add(shown.Value()); !added) {
            return added;
        }

        first = last;
    }

    refreshed.Finish();
    refreshedDisplays.Finish();
    for (StringView root : roots) {
        if (auto scanned = ScanTree(files, editor, nullptr, root, StringView(), plan.added_,
                                    plan.fileDiagnostics_);
            !scanned) {
            return scanned;
        }
    }

    // 이름 바꾸기: 지금 from의 내용을 to 이름으로 읽는다(이동 전 준비).
    for (const AssetIndexMove& move : paths.moves) {
        if (auto scanned = ScanTree(files, editor, nullptr, move.from, move.to, plan.added_,
                                    plan.fileDiagnostics_);
            !scanned) {
            return scanned;
        }
    }

    // 지금 색인에서 다시 읽은 이름(파일과 하위 트리 아래)의 항목·진단과 같은 id·경로 진단을 빼고
    // 나머지와 새 것을 자리로 모은다(자리 포인터는 확정이 옮길 대상이다).
    Array<AssetIndexEntry*> entryPlaces(*allocator_);
    Array<AssetIndexDiagnostic*> diagnosticPlaces(*allocator_);
    if (auto reserved = entryPlaces.TryReserve(entries_.Size() + plan.added_.Size()); !reserved) {
        return reserved;
    }

    if (auto reserved =
            diagnosticPlaces.TryReserve(diagnostics_.Size() + plan.fileDiagnostics_.Size());
        !reserved) {
        return reserved;
    }

    for (AssetIndexEntry& entry : entries_) {
        auto key = CompareKey(entry.fileName.View(), normalizes, scratch);
        if (!key) {
            return Result<void>::Failure(key.Error());
        }

        if (!refreshed.Contains(key.Value()) && !trees.Covers(key.Value()) &&
            !moved.Covers(key.Value())) {
            (void)entryPlaces.TryPushBack(&entry);
        }
    }

    for (AssetIndexEntry& entry : plan.added_) {
        (void)entryPlaces.TryPushBack(&entry);
    }

    for (AssetIndexDiagnostic& diagnostic : diagnostics_) {
        if (IsNeighborDiagnostic(diagnostic)) {
            continue;
        }

        auto key = CompareKey(diagnostic.fileName.View(), normalizes, scratch);
        if (!key) {
            return Result<void>::Failure(key.Error());
        }

        if (!refreshedDisplays.Contains(key.Value()) && !treeDisplays.Covers(key.Value())) {
            (void)diagnosticPlaces.TryPushBack(&diagnostic);
        }
    }

    for (AssetIndexDiagnostic& diagnostic : plan.fileDiagnostics_) {
        (void)diagnosticPlaces.TryPushBack(&diagnostic);
    }

    return PlanPublish(entryPlaces.View(), diagnosticPlaces.View(), plan);
}

Result<void> AssetIndex::MarkRootLost() noexcept {
    Array<AssetIndexDiagnostic> lost(*allocator_);
    if (auto added =
            AddDiagnostic(lost, AssetDiagnosticCode::RootLost, ErrorCode::NotFound, StringView());
        !added) {
        return added;
    }

    entries_.Clear();
    byId_.Clear();
    diagnostics_ = Move(lost);
    ++revision_;
    return Result<void>::Success();
}

void AssetIndex::CommitRefresh(AssetIndexRefresh& plan) noexcept {
    CommitPublish(plan);
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

namespace {

// ImportRecord를 읽지 못한 진단인지다(칸 ImportRecord — 색인 IndexImportRecord의
// UnsupportedVersion· CorruptPackage, 패키지 판정의 그 섹션 손상).
bool IsUnreadableRecord(const AssetIndexDiagnostic& diagnostic) noexcept {
    return diagnostic.section == static_cast<uint32>(PackageSectionKind::ImportRecord);
}

bool ContainsId(Span<const AssetId> sorted, AssetId id) noexcept {
    const usize at = algo::LowerBound(sorted, id, [](AssetId left, AssetId right) noexcept {
        return CompareId(left, right) < 0;
    });
    return at < sorted.Size() && CompareId(sorted[at], id) == 0;
}

} // namespace

Result<void> AssetIndex::MakeOrphanFacts(OrphanFacts& facts) const noexcept {
    for (const AssetIndexDiagnostic& diagnostic : diagnostics_) {
        if (diagnostic.asset.IsNull()) {
            facts.unidentified = true;
            continue;
        }

        if (auto pushed = facts.seen.TryPushBack(diagnostic.asset); !pushed) {
            return pushed;
        }

        if (IsUnreadableRecord(diagnostic)) {
            if (auto pushed = facts.unreadableRecords.TryPushBack(diagnostic.asset); !pushed) {
                return pushed;
            }
        }
    }

    auto less = [](AssetId left, AssetId right) noexcept { return CompareId(left, right) < 0; };
    facts.seen.Sort(less);
    facts.unreadableRecords.Sort(less);
    return Result<void>::Success();
}

AssetOrphanReason AssetIndex::OrphanReasonWith(const AssetIndexEntry& entry,
                                               const OrphanFacts* facts) const noexcept {
    const AssetIndexImportSummary& record = entry.importRecord;
    if (!record.present || record.owner == entry.id) {
        return AssetOrphanReason::None;
    }

    const AssetId ownerId = record.owner;
    // 진단 사실(표가 없으면 진단을 훑는다).
    bool unidentified = false;
    bool seen = false;
    bool unreadableRecord = false;
    if (facts != nullptr) {
        unidentified = facts->unidentified;
        seen = ContainsId(facts->seen.View(), ownerId);
        unreadableRecord = ContainsId(facts->unreadableRecords.View(), ownerId);
    } else {
        for (const AssetIndexDiagnostic& diagnostic : diagnostics_) {
            unidentified = unidentified || diagnostic.asset.IsNull();
            const bool owner = diagnostic.asset == ownerId;
            seen = seen || owner;
            unreadableRecord = unreadableRecord || (owner && IsUnreadableRecord(diagnostic));
        }
    }

    // owner 항목(같은 id 범위 — byId_는 id 순)이다.
    const usize low =
        algo::LowerBound(byId_.View(), ownerId, [&](usize index, AssetId key) noexcept {
            return CompareId(entries_[index].id, key) < 0;
        });
    usize high = low;
    bool anyOwned = false;
    for (; high < byId_.Size() && CompareId(entries_[byId_[high]].id, ownerId) == 0; ++high) {
        const AssetIndexEntry& owner = entries_[byId_[high]];
        if (!owner.importRecord.present || !(owner.importRecord.owner == owner.id)) {
            continue;
        }

        anyOwned = true;
        for (AssetId output : owner.importRecord.outputs) {
            if (output == entry.id) {
                return AssetOrphanReason::None;
            }
        }
    }

    if (high == low) {
        // owner가 색인에 없다 — 읽지 못한 패키지가 owner일 수 있으면 확정하지 않는다.
        return unidentified || seen ? AssetOrphanReason::Undetermined
                                    : AssetOrphanReason::OwnerMissing;
    }

    // owner 쪽 기록을 읽지 못했으면(그 기록이 이 출력을 적었을 수 있다) 확정하지 않는다.
    if (unreadableRecord) {
        return AssetOrphanReason::Undetermined;
    }

    return anyOwned ? AssetOrphanReason::NotListed : AssetOrphanReason::OwnerNotImporter;
}

AssetOrphanReason AssetIndex::OrphanReason(const AssetIndexEntry& entry) const noexcept {
    return OrphanReasonWith(entry, nullptr);
}

Result<void> AssetIndex::Orphans(Array<const AssetIndexEntry*>& out) const noexcept {
    OrphanFacts facts(*allocator_);
    if (auto made = MakeOrphanFacts(facts); !made) {
        return made;
    }

    for (const AssetIndexEntry& entry : entries_) {
        const AssetOrphanReason reason = OrphanReasonWith(entry, &facts);
        if (reason == AssetOrphanReason::None || reason == AssetOrphanReason::Undetermined) {
            continue;
        }

        if (auto pushed = out.TryPushBack(&entry); !pushed) {
            return pushed;
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

    // 고아 출력(경고 — 7.1절 17 A2e4, 판정할 수 없는 것은 뺀다).
    OrphanFacts facts(*allocator_);
    if (auto made = MakeOrphanFacts(facts); !made) {
        return made;
    }

    for (const AssetIndexEntry& entry : entries_) {
        const AssetOrphanReason reason = OrphanReasonWith(entry, &facts);
        if (reason == AssetOrphanReason::None || reason == AssetOrphanReason::Undetermined) {
            continue;
        }

        if (auto added = AddDiagnostic(out, AssetDiagnosticCode::OrphanedOutput,
                                       ErrorCode::NotFound, entry.fileName.View());
            !added) {
            return added;
        }

        AssetIndexDiagnostic& orphan = out[out.Size() - 1];
        orphan.asset = entry.id;
        orphan.relatedAsset = entry.importRecord.owner;
        orphan.detail = static_cast<uint64>(reason);
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
    case AssetDiagnosticCode::RootLost:
        return line.TryAppend("the content directory disappeared\n");
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
    case AssetDiagnosticCode::OrphanedOutput: {
        const StringView why =
            diagnostic.detail == static_cast<uint64>(AssetOrphanReason::OwnerMissing)
                ? StringView("is not in the index")
            : diagnostic.detail == static_cast<uint64>(AssetOrphanReason::OwnerNotImporter)
                ? StringView("has no import record of its own")
                : StringView("does not list this output");
        return line.TryAppendFormat("warning the owning import {} {}\n", diagnostic.relatedAsset,
                                    why);
    }
    default:
        return line.TryAppendFormat("package check failed (section {}, detail {}, {})\n",
                                    diagnostic.section, diagnostic.detail,
                                    ErrorCodeName(diagnostic.error));
    }
}

} // namespace rocks
