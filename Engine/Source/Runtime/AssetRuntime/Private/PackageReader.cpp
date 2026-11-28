#include "PackageInternal.h"

#include <AssetRuntime/PackageRangeReader.h>
#include <AssetRuntime/PackageReader.h>
#include <Compression/CompressedBlocks.h>
#include <Compression/Compression.h>
#include <Core/Algorithms.h>
#include <Core/Assert.h>
#include <Core/Encoding/Crc32.h>
#include <Core/Text/NameComponent.h>
#include <Object/ObjectIdentity.h>
#include <Object/TypeRegistry.h>
#include <ObjectSerialization/ClassDefaults.h>
#include <Serialization/Record.h>
#include <Serialization/RecordList.h>

namespace rocks {

namespace {

using namespace package_detail;

constexpr uint32 SectionKindValue(PackageSectionKind kind) noexcept {
    return static_cast<uint32>(kind);
}

// 손상 판정 하나를 진단과 함께 돌려준다.
template <class T>
Result<T> Corrupt(AssetDiagnostic* failure, AssetId asset, uint32 section, uint64 detail) noexcept {
    return FailAsset<T>(failure, AssetDiagnosticCode::CorruptPackage, ErrorCode::CorruptData, asset,
                        section, detail);
}

template <class T>
Result<T> Unsupported(AssetDiagnostic* failure, AssetDiagnosticCode code, AssetId asset,
                      uint32 section, uint64 detail) noexcept {
    return FailAsset<T>(failure, code, ErrorCode::Unsupported, asset, section, detail);
}

// 할당·자원 실패는 손상이 아니다(진단 코드 None, 원래 오류 보존).
template <class T>
Result<T> Resource(AssetDiagnostic* failure, ErrorCode error, AssetId asset,
                   uint32 section) noexcept {
    return FailAsset<T>(failure, AssetDiagnosticCode::None, error, asset, section, 0);
}

Guid LoadGuid(const uint8* bytes) noexcept {
    return Guid{LoadLittleEndian<uint64>(bytes), LoadLittleEndian<uint64>(bytes + 8)};
}

bool AllZero(Span<const uint8> bytes) noexcept {
    for (uint8 value : bytes) {
        if (value != 0) {
            return false;
        }
    }

    return true;
}

// 섹션 표·내용 해석에 쓰는 Archive 한도다(문자열 4 KiB, 작업량은 섹션 상한 기준).
ArchiveLimits TableLimits() noexcept {
    ArchiveLimits limits;
    limits.maxTotalBytes = static_cast<usize>(PackageMaxSectionBytes);
    limits.maxValueBytes = PackageMaxStringBytes;
    limits.maxOperations = NumericLimits<uint64>::Max();
    limits.maxWork = NumericLimits<uint64>::Max();
    return limits;
}

// 고정 배치 표 섹션의 reader다. 표 섹션 크기는 섹션 표 검증이 PackageMaxSectionBytes 이하로
// 확인했으므로 Create의 실패(크기 초과)는 일어나지 않는다.
ArchiveReader OpenTable(Span<const uint8> bytes) noexcept {
    auto archive = ArchiveReader::Create(bytes, TableLimits());
    ROCKS_ASSERT(archive.IsOk());
    return Move(archive.Value());
}

RecordLimits SummaryLimits() noexcept {
    RecordLimits limits;
    limits.archive = TableLimits();
    limits.archive.maxValueBytes = static_cast<usize>(PackageMaxSectionBytes);
    limits.maxFields = 16;
    return limits;
}

// Record 필드 payload를 고정 폭 값으로 읽는다(wire가 다르거나 크기가 다르면 실패).
Result<uint32> FieldU32(const FieldView& field) noexcept {
    if (field.wireType != static_cast<uint32>(WireType::U32) || field.payload.Size() != 4) {
        return Result<uint32>::Failure(ErrorCode::CorruptData);
    }

    return Result<uint32>::Success(LoadLittleEndian<uint32>(field.payload.Data()));
}

Result<Guid> FieldGuid(const FieldView& field) noexcept {
    if (field.wireType != static_cast<uint32>(WireType::Guid) || field.payload.Size() != 16) {
        return Result<Guid>::Failure(ErrorCode::CorruptData);
    }

    return Result<Guid>::Success(LoadGuid(field.payload.Data()));
}

// 버전 표(RecordList[{id, version}])를 읽는다. id 오름차순·중복 없음이어야 한다.
template <class Entry>
Result<void> ReadVersionList(const RecordReader& parent, const FieldView& field,
                             Array<Entry>& out) noexcept {
    RecordListLimits listLimits;
    listLimits.maxItems =
        PackageMaxExports > PackageMaxObjectImports ? PackageMaxExports : PackageMaxObjectImports;
    listLimits.maxPayloadBytes = static_cast<usize>(PackageMaxSectionBytes);
    listLimits.maxItemBytes = static_cast<usize>(PackageMaxSectionBytes);
    auto list = RecordListReader::OpenField(parent, field, VersionItemSchema, 1, 1, listLimits);
    if (!list) {
        return Result<void>::Failure(list.Error());
    }

    if (auto reserved = out.TryReserve(list.Value().ItemCount()); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < list.Value().ItemCount(); ++index) {
        auto item = list.Value().OpenItem(index, 2);
        if (!item || item.Value().FieldCount() != 2) {
            return Result<void>::Failure(item ? ErrorCode::CorruptData : item.Error());
        }

        auto idField = item.Value().Find(VersionItemIdField);
        auto versionField = item.Value().Find(VersionItemVersionField);
        if (!idField || !versionField) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        auto id = FieldGuid(idField.Value());
        auto version = FieldU32(versionField.Value());
        if (!id || !version) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        if (!out.IsEmpty() && !GuidLess(out[out.Size() - 1].type, id.Value())) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        Entry entry;
        entry.type = id.Value();
        entry.version = version.Value();
        // 항목 수만큼 위에서 예약했다.
        (void)out.TryPushBack(entry);
    }

    return Result<void>::Success();
}

// 클래스 기본값 목록(RecordList[{id, fingerprint}] — A5c)을 읽는다. id 오름차순·중복 없음이어야
// 한다(export 타입 집합과의 대조는 ClassDefaultsWithinTypes).
Result<void> ReadClassDefaultsList(const RecordReader& parent, const FieldView& field,
                                   Array<PackageClassDefaults>& out) noexcept {
    RecordListLimits listLimits;
    listLimits.maxItems = PackageMaxExports;
    listLimits.maxPayloadBytes = static_cast<usize>(PackageMaxSectionBytes);
    listLimits.maxItemBytes = static_cast<usize>(PackageMaxSectionBytes);
    auto list =
        RecordListReader::OpenField(parent, field, ClassDefaultsItemSchema, 1, 1, listLimits);
    if (!list) {
        return Result<void>::Failure(list.Error());
    }

    if (auto reserved = out.TryReserve(list.Value().ItemCount()); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < list.Value().ItemCount(); ++index) {
        auto item = list.Value().OpenItem(index, 2);
        if (!item || item.Value().FieldCount() != 2) {
            return Result<void>::Failure(item ? ErrorCode::CorruptData : item.Error());
        }

        auto idField = item.Value().Find(ClassDefaultsItemIdField);
        auto digestField = item.Value().Find(ClassDefaultsItemFingerprintField);
        if (!idField || !digestField) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        auto id = FieldGuid(idField.Value());
        const FieldView& digest = digestField.Value();
        if (!id || digest.wireType != static_cast<uint32>(WireType::Bytes) ||
            digest.payload.Size() != Sha256Digest::Bytes) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        if (!out.IsEmpty() && !GuidLess(out[out.Size() - 1].type, id.Value())) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        PackageClassDefaults row;
        row.type = id.Value();
        for (usize at = 0; at < Sha256Digest::Bytes; ++at) {
            row.fingerprint.bytes[at] = digest.payload[at];
        }

        // 항목 수만큼 위에서 예약했다.
        (void)out.TryPushBack(row);
    }

    return Result<void>::Success();
}

// 클래스 기본값 행이 모두 타입 표(= export 타입 집합)에 있는지다(둘 다 정렬 — 한 번 훑기).
bool ClassDefaultsWithinTypes(const PackageSummary& summary) noexcept {
    usize at = 0;
    for (const PackageClassDefaults& row : summary.classDefaults) {
        while (at < summary.types.Size() && GuidLess(summary.types[at].type, row.type)) {
            ++at;
        }

        if (at == summary.types.Size() || !(summary.types[at].type == row.type)) {
            return false;
        }
    }

    return true;
}

// 버전 표 항목의 정렬 키 이름을 맞추는 대리 항목이다(커스텀 버전은 id를 type 칸으로 읽는다).
struct CustomVersionRow {
    Guid type{};
    uint32 version = 0;
};

Result<EngineVersion> ReadEngineVersion(Allocator& allocator, const RecordReader& parent,
                                        const FieldView& field) noexcept {
    auto record = parent.OpenRecordField(allocator, field, EngineVersionSchema, 1, 1, 3);
    if (!record || record.Value().FieldCount() != 3) {
        return Result<EngineVersion>::Failure(record ? ErrorCode::CorruptData : record.Error());
    }

    uint32 parts[3] = {};
    const Guid ids[3] = {EngineVersionMajorField, EngineVersionMinorField, EngineVersionPatchField};
    for (usize index = 0; index < 3; ++index) {
        auto part = record.Value().Find(ids[index]);
        auto value = part ? FieldU32(part.Value()) : Result<uint32>::Failure(part.Error());
        if (!value || value.Value() > NumericLimits<uint16>::Max()) {
            return Result<EngineVersion>::Failure(ErrorCode::CorruptData);
        }

        parts[index] = value.Value();
    }

    return Result<EngineVersion>::Success(EngineVersion{static_cast<uint16>(parts[0]),
                                                        static_cast<uint16>(parts[1]),
                                                        static_cast<uint16>(parts[2])});
}

Result<void> ReadSummary(Allocator& allocator, Span<const uint8> bytes, PackageKind kind,
                         PackageSummary& summary) noexcept {
    auto reader = RecordReader::Open(allocator, bytes, SummarySchema, SummarySchemaVersion,
                                     SummarySchemaVersion, SummaryLimits());
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    RecordReader& record = reader.Value();
    const usize expected = kind == PackageKind::Editor ? 5 : 6;
    if (record.FieldCount() != expected) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    auto types = record.Find(SummaryTypesField);
    auto customs = record.Find(SummaryCustomVersionsField);
    auto defaults = record.Find(SummaryClassDefaultsField);
    if (!types || !customs || !defaults) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    if (auto read = ReadClassDefaultsList(record, defaults.Value(), summary.classDefaults); !read) {
        return read;
    }

    if (auto read = ReadVersionList(record, types.Value(), summary.types); !read) {
        return read;
    }

    Array<CustomVersionRow> customRows(allocator);
    if (auto read = ReadVersionList(record, customs.Value(), customRows); !read) {
        return read;
    }

    if (auto reserved = summary.customVersions.TryReserve(customRows.Size()); !reserved) {
        return reserved;
    }

    for (const CustomVersionRow& row : customRows) {
        (void)summary.customVersions.TryPushBack(PackageCustomVersion{row.type, row.version});
    }

    if (kind == PackageKind::Editor) {
        auto saved = record.Find(SummarySavedByField);
        auto compatible = record.Find(SummaryCompatibleWithField);
        if (!saved || !compatible) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        auto savedBy = ReadEngineVersion(allocator, record, saved.Value());
        auto compatibleWith = ReadEngineVersion(allocator, record, compatible.Value());
        if (!savedBy || !compatibleWith) {
            return Result<void>::Failure(savedBy ? compatibleWith.Error() : savedBy.Error());
        }

        summary.savedBy = savedBy.Value();
        summary.compatibleWith = compatibleWith.Value();
        // compatibleWith는 savedBy의 기준선(주·부·0)과 정확히 같다(writer 규칙 — 같은 객체로 풀리는
        // 다른 byte를 받지 않는다, AS 5.7절 15).
        if (!(summary.compatibleWith == summary.savedBy.CompatibleBaseline())) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        return Result<void>::Success();
    }

    auto profile = record.Find(SummaryProfileIdField);
    auto settings = record.Find(SummaryProfileSettingsField);
    auto cooker = record.Find(SummaryCookerVersionField);
    if (!profile || !settings || !cooker) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    const FieldView& profileField = profile.Value();
    if (profileField.wireType != static_cast<uint32>(WireType::String) ||
        profileField.payload.Size() == 0 || profileField.payload.Size() > PackageMaxStringBytes) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    const StringView profileText(reinterpret_cast<const char*>(profileField.payload.Data()),
                                 profileField.payload.Size());
    if (!profileText.IsValidUtf8()) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    if (auto assigned = summary.profileId.TryAssign(profileText); !assigned) {
        return assigned;
    }

    const FieldView& settingsField = settings.Value();
    if (settingsField.wireType != static_cast<uint32>(WireType::Bytes) ||
        settingsField.payload.Size() != Sha256Digest::Bytes) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        summary.profileSettings.bytes[index] = settingsField.payload[index];
    }

    auto cookerVersion = FieldU32(cooker.Value());
    if (!cookerVersion) {
        return Result<void>::Failure(cookerVersion.Error());
    }

    summary.cookerVersion = cookerVersion.Value();
    return Result<void>::Success();
}

// 클래스 하나의 기본값 판정 결과다(A5c).
struct ClassDefaultsJudgement {
    // 저장 당시와 같다(둘 다 지문이 같거나 둘 다 레코드 없음).
    bool same = false;
    // 지금 레코드 실패다(reason이 이유).
    bool failed = false;
    ClassDefaultsReason reason = ClassDefaultsReason::None;
};

// lease 클래스의 저장 당시(summary 행)와 지금 레코드를 맞춘다. 오류는 레코드를 만드는 일시
// 실패(할당 등)다.
Result<ClassDefaultsJudgement> JudgeClassDefaults(Allocator& allocator, const TypeLease& lease,
                                                  const PackageSummary& summary,
                                                  bool cooked) noexcept {
    // 행은 TypeId 오름차순이다(ReadClassDefaultsList가 확인했다) — 이진 탐색.
    const TypeId type = lease.Id();
    const usize at = algo::LowerBound(summary.classDefaults.View(), type,
                                      [](const PackageClassDefaults& row, TypeId key) noexcept {
                                          return GuidLess(row.type, key);
                                      });
    const PackageClassDefaults* stored =
        at < summary.classDefaults.Size() && summary.classDefaults[at].type == type
            ? &summary.classDefaults[at]
            : nullptr;

    auto status = ClassifyClassDefaults(lease, allocator);
    if (!status) {
        return Result<ClassDefaultsJudgement>::Failure(status.Error());
    }

    ClassDefaultsJudgement judgement;
    switch (status.Value().availability) {
    case ClassDefaultsAvailability::Present: {
        const Sha256Digest now = status.Value().defaults->Fingerprint(
            cooked ? ClassDefaultsConfiguration::Cooked : ClassDefaultsConfiguration::Editor);
        judgement.same = stored != nullptr && stored->fingerprint == now;
        break;
    }
    case ClassDefaultsAvailability::WithoutRecord:
        judgement.same = stored == nullptr;
        break;
    case ClassDefaultsAvailability::Failed:
        judgement.failed = true;
        judgement.reason = status.Value().diagnostic.reason;
        break;
    }

    return Result<ClassDefaultsJudgement>::Success(judgement);
}

// 해석 오류를 진단으로 바꾼다. 할당 실패는 자원 실패, 그 밖은 손상이다.
template <class T>
Result<T> SectionFailure(AssetDiagnostic* failure, ErrorCode error, AssetId asset,
                         PackageSectionKind kind, uint64 detail) noexcept {
    if (error == ErrorCode::OutOfMemory) {
        return Resource<T>(failure, error, asset, SectionKindValue(kind));
    }

    return Corrupt<T>(failure, asset, SectionKindValue(kind), detail);
}

Result<void> ReadExports(Span<const uint8> bytes, PackageContents& contents) noexcept {
    ArchiveReader in = OpenTable(bytes);
    auto count = in.ReadU32();
    if (!count || count.Value() == 0 || count.Value() > PackageMaxExports) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    if (auto reserved = contents.exports.TryReserve(count.Value()); !reserved) {
        return reserved;
    }

    for (uint32 index = 0; index < count.Value(); ++index) {
        auto name = in.ReadStringView();
        auto type = in.ReadGuid();
        auto outer = in.ReadU32();
        auto flags = in.ReadU32();
        if (!name || !type || !outer || !flags) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        PackageExportEntry entry;
        entry.name = name.Value();
        entry.type = type.Value();
        entry.outerIndex = outer.Value();
        entry.flags = flags.Value();
        (void)contents.exports.TryPushBack(entry);
    }

    if (!in.AtEnd()) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    return Result<void>::Success();
}

// export 표 구조를 검증한다: export 1만 Primary·빈 이름·outer 0, 나머지는 이름 있음·outer ≥
// 1·자기보다 앞, 정의된 flags만(EditorOnly는 v1 예약), 깊이 우선 순서(outer는 바로 앞 export이거나
// 그 조상), 같은 Outer 안 이름 byte 오름차순(중복 없음), 타입 0 아님.
Result<uint64> ValidateExports(Allocator& allocator, const PackageContents& contents) noexcept {
    const Array<PackageExportEntry>& exports = contents.exports;
    // 깊이 우선 경로(export index, 그 아래 마지막 자식 이름)다.
    struct PathFrame {
        uint32 index = 0;
        StringView lastChild{};
        bool hasChild = false;
    };

    Array<PathFrame> path(allocator);
    if (auto reserved = path.TryReserve(exports.Size() + 1); !reserved) {
        return Result<uint64>::Failure(reserved.Error());
    }

    (void)path.TryPushBack(PathFrame{});
    for (usize position = 0; position < exports.Size(); ++position) {
        const PackageExportEntry& entry = exports[position];
        const uint32 index = static_cast<uint32>(position + 1);
        const bool primary = (entry.flags & static_cast<uint32>(PackageExportFlags::Primary)) != 0;
        if ((entry.flags & ~static_cast<uint32>(PackageExportFlags::Primary)) != 0 ||
            entry.type == TypeId{}) {
            return Result<uint64>::Failure(ErrorCode::CorruptData);
        }

        if (index == 1) {
            if (!primary || entry.name.ByteCount() != 0 || entry.outerIndex != 0) {
                return Result<uint64>::Failure(ErrorCode::CorruptData);
            }
        } else if (primary || entry.name.ByteCount() == 0 || entry.outerIndex == 0 ||
                   entry.outerIndex >= index) {
            return Result<uint64>::Failure(ErrorCode::CorruptData);
        }

        // outer가 경로에 있어야 한다(깊이 우선). 경로를 outer까지 줄인다.
        while (!path.IsEmpty() && path[path.Size() - 1].index != entry.outerIndex) {
            (void)path.TryPopBack();
        }

        if (path.IsEmpty()) {
            return Result<uint64>::Failure(ErrorCode::CorruptData);
        }

        PathFrame& parent = path[path.Size() - 1];
        if (parent.hasChild && !BytesLess(parent.lastChild, entry.name)) {
            return Result<uint64>::Failure(ErrorCode::CorruptData);
        }

        parent.hasChild = true;
        parent.lastChild = entry.name;
        (void)path.TryPushBack(PathFrame{index, {}, false});
        (void)position;
    }

    return Result<uint64>::Success(0);
}

Result<void> ReadImports(Span<const uint8> bytes, PackageContents& contents) noexcept {
    ArchiveReader in = OpenTable(bytes);
    auto count = in.ReadU32();
    if (!count || count.Value() == 0 || count.Value() > PackageMaxObjectImports) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    if (auto reserved = contents.imports.TryReserve(count.Value()); !reserved) {
        return reserved;
    }

    for (uint32 index = 0; index < count.Value(); ++index) {
        auto asset = in.ReadGuid();
        auto outer = in.ReadU32();
        auto name = in.ReadStringView();
        auto type = in.ReadGuid();
        if (!asset || !outer || !name || !type) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        PackageImportEntry entry;
        entry.asset = AssetId{asset.Value()};
        entry.outerImportIndex = outer.Value();
        entry.name = name.Value();
        entry.type = type.Value();
        (void)contents.imports.TryPushBack(entry);
    }

    if (!in.AtEnd()) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    return Result<void>::Success();
}

// import 표를 검증한다(AS 5.1절 유일성, 5.2절 순서, 5.7절 13): 에셋 null·자기 에셋 금지, 타입 0
// 아님, 이름은 비었거나(outer 0인 대표 에셋 항목) 객체 이름 규칙(ValidateNameComponent — UTF-8 NFC,
// 1–64 byte, 허용 문자)을 지킨다, outer import는 자기보다 앞·같은 에셋·이름 있는 항목이고 outer가
// 있으면 이름 있음, 객체 경로(outer 경로 + '.' + 이름)는 MaxObjectPathBytes 이하, 순서는 assetId
// 오름차순이고 같은 에셋 안은 깊이 우선(outer는 바로 앞 항목이거나 그 outer 사슬의 항목, 또는
// 0)이며 같은 outer 아래 이름은 byte 엄격 오름차순(중복 없음 — 대표 에셋 항목은 빈 이름이라 그
// 에셋의 첫 항목)이다. 항목 수에 비례한다(경로를 만들지 않고 길이만 이어 센다).
Result<uint64> ValidateImports(Allocator& allocator, const PackageContents& contents) noexcept {
    const Array<PackageImportEntry>& imports = contents.imports;
    // 깊이 우선 경로(import 번호 — 0은 에셋의 대표 에셋 자리, 그 아래 마지막 자식 이름)다.
    struct PathFrame {
        uint32 index = 0;
        StringView lastChild{};
        bool hasChild = false;
    };

    Array<PathFrame> path(allocator);
    Array<uint32> pathBytes(allocator);
    if (auto reserved = path.TryReserve(imports.Size() + 1); !reserved) {
        return Result<uint64>::Failure(reserved.Error());
    }

    if (auto resized = pathBytes.TryResize(imports.Size()); !resized) {
        return Result<uint64>::Failure(resized.Error());
    }

    for (usize position = 0; position < imports.Size(); ++position) {
        const PackageImportEntry& entry = imports[position];
        if (entry.asset.IsNull() || entry.asset == contents.header.assetId ||
            entry.type == TypeId{}) {
            return Result<uint64>::Failure(ErrorCode::CorruptData);
        }

        if (entry.outerImportIndex != 0) {
            const usize outer = entry.outerImportIndex - 1;
            if (outer >= position || !(imports[outer].asset == entry.asset) ||
                imports[outer].name.ByteCount() == 0 || entry.name.ByteCount() == 0) {
                return Result<uint64>::Failure(ErrorCode::CorruptData);
            }
        }

        if (entry.name.ByteCount() != 0) {
            if (auto valid = ValidateNameComponent(entry.name, allocator); !valid) {
                return Result<uint64>::Failure(valid.Error());
            }
        }

        // 에셋이 바뀌면 더 큰 assetId여야 하고 깊이 우선 경로는 대표 에셋 자리에서 다시 시작한다.
        if (position == 0 || !(imports[position - 1].asset == entry.asset)) {
            if (position > 0 && !GuidLess(imports[position - 1].asset.value, entry.asset.value)) {
                return Result<uint64>::Failure(ErrorCode::CorruptData);
            }

            path.Clear();
            (void)path.TryPushBack(PathFrame{});
        }

        while (!path.IsEmpty() && path[path.Size() - 1].index != entry.outerImportIndex) {
            (void)path.TryPopBack();
        }

        if (path.IsEmpty()) {
            return Result<uint64>::Failure(ErrorCode::CorruptData);
        }

        PathFrame& parent = path[path.Size() - 1];
        if (parent.hasChild && !BytesLess(parent.lastChild, entry.name)) {
            return Result<uint64>::Failure(ErrorCode::CorruptData);
        }

        parent.hasChild = true;
        parent.lastChild = entry.name;
        (void)path.TryPushBack(PathFrame{static_cast<uint32>(position + 1), {}, false});
        // 이름은 64 byte 이하라 길이 합은 넘치지 않는다(경로 상한에서 멈춘다).
        const usize bytes = entry.outerImportIndex == 0 ? entry.name.ByteCount()
                                                        : pathBytes[entry.outerImportIndex - 1] +
                                                              1 + entry.name.ByteCount();
        if (bytes > MaxObjectPathBytes) {
            return Result<uint64>::Failure(ErrorCode::CorruptData);
        }

        pathBytes[position] = static_cast<uint32>(bytes);
    }

    return Result<uint64>::Success(0);
}

// Dependencies 항목 하나({assetId Guid, kind u32, expectedType Guid, flags u32})와 BulkIndex 항목
// 하나({bulkId, flags, offset u64, storedBytes u64, rawBytes u64, codec, crc32, sha256})의 byte다.
constexpr uint64 DependencyEntryBytes = 2 * sizeof(Guid) + 2 * sizeof(uint32);
constexpr uint64 BulkIndexEntryBytes =
    4 * sizeof(uint32) + 3 * sizeof(uint64) + Sha256Digest::Bytes;
static_assert(DependencyEntryBytes == 40, "Dependencies entry layout changed");
static_assert(BulkIndexEntryBytes == 72, "BulkIndex entry layout changed");

Result<void> ReadDependencies(Span<const uint8> bytes, PackageContents& contents) noexcept {
    ArchiveReader in = OpenTable(bytes);
    auto count = in.ReadU32();
    // 섹션 크기 = count 칸 + count × 항목 크기다(고정 배치 — 의존 수는 import 수 + Soft 수라 따로
    // 상한을 두지 않고 섹션 크기가 묶는다). 크기가 맞으면 아래 칸 읽기는 실패하지 않는다.
    if (!count || count.Value() == 0 ||
        bytes.Size() != sizeof(uint32) + uint64{count.Value()} * DependencyEntryBytes) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    if (auto reserved = contents.dependencies.TryReserve(count.Value()); !reserved) {
        return reserved;
    }

    for (uint32 index = 0; index < count.Value(); ++index) {
        auto asset = in.ReadGuid();
        auto kind = in.ReadU32();
        auto expected = in.ReadGuid();
        auto flags = in.ReadU32();
        ROCKS_ASSERT(asset && kind && expected && flags);

        if (kind.Value() < static_cast<uint32>(PackageDependencyKind::Hard) ||
            kind.Value() > static_cast<uint32>(PackageDependencyKind::ImportOwner) ||
            (flags.Value() & ~DependencyRequiredFlag) != 0) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        PackageDependencyEntry entry;
        entry.asset = AssetId{asset.Value()};
        entry.kind = static_cast<PackageDependencyKind>(kind.Value());
        entry.expectedType = expected.Value();
        entry.required = (flags.Value() & DependencyRequiredFlag) != 0;
        (void)contents.dependencies.TryPushBack(entry);
    }

    ROCKS_ASSERT(in.AtEnd());

    return Result<void>::Success();
}

Result<uint64> ValidateDependencies(const PackageContents& contents) noexcept {
    const Array<PackageDependencyEntry>& dependencies = contents.dependencies;
    for (usize index = 0; index < dependencies.Size(); ++index) {
        const PackageDependencyEntry& entry = dependencies[index];
        if (entry.asset.IsNull() || entry.asset == contents.header.assetId) {
            return Result<uint64>::Failure(ErrorCode::CorruptData);
        }

        if (contents.header.kind == PackageKind::Cooked &&
            entry.kind != PackageDependencyKind::Hard &&
            entry.kind != PackageDependencyKind::Soft) {
            return Result<uint64>::Failure(ErrorCode::CorruptData);
        }

        // Hard만 expectedType을 가지고 Hard는 반드시 가진다(writer 규칙 — 대상 대표 에셋 타입, AS
        // 5.7절 5·14).
        if ((entry.kind == PackageDependencyKind::Hard) != (entry.expectedType != TypeId{})) {
            return Result<uint64>::Failure(ErrorCode::CorruptData);
        }

        if (index > 0 && !DependencyLess(dependencies[index - 1], entry)) {
            return Result<uint64>::Failure(ErrorCode::CorruptData);
        }
    }

    return Result<uint64>::Success(0);
}

Result<void> ReadBulkIndex(Span<const uint8> bytes, PackageContents& contents) noexcept {
    ArchiveReader in = OpenTable(bytes);
    auto count = in.ReadU32();
    // 섹션 크기 = count 칸 + count × 항목 크기다(고정 배치). 크기가 맞으면 칸 읽기는 실패하지
    // 않는다.
    if (!count || count.Value() == 0 ||
        bytes.Size() != sizeof(uint32) + uint64{count.Value()} * BulkIndexEntryBytes) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    if (auto reserved = contents.bulkEntries.TryReserve(count.Value()); !reserved) {
        return reserved;
    }

    for (uint32 index = 0; index < count.Value(); ++index) {
        auto id = in.ReadU32();
        auto flags = in.ReadU32();
        auto offset = in.ReadU64();
        auto stored = in.ReadU64();
        auto raw = in.ReadU64();
        auto codec = in.ReadU32();
        auto crc = in.ReadU32();
        auto sha = in.ReadRaw(Sha256Digest::Bytes);
        ROCKS_ASSERT(id && flags && offset && stored && raw && codec && crc && sha);

        PackageBulkEntry entry;
        entry.bulkId = id.Value();
        entry.flags = flags.Value();
        entry.offset = offset.Value();
        entry.storedBytes = stored.Value();
        entry.rawBytes = raw.Value();
        if (codec.Value() > static_cast<uint32>(BulkCodec::Zstd)) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        entry.codec = static_cast<BulkCodec>(codec.Value());
        entry.crc32 = crc.Value();
        for (usize byte = 0; byte < Sha256Digest::Bytes; ++byte) {
            entry.sha256.bytes[byte] = sha.Value()[byte];
        }

        (void)contents.bulkEntries.TryPushBack(entry);
    }

    ROCKS_ASSERT(in.AtEnd());

    return Result<void>::Success();
}

} // namespace

namespace package_detail {

// BulkIndex 배치를 Bulk 섹션 크기와 대조한다: bulkId 1부터 연속, offset = 앞 entry 끝을 정렬한
// 값(첫 entry 0), 마지막 끝 = 섹션 크기, codec None이면 stored = raw, 정의된 flags만(쿠킹은 0).
// 내용(entry 사이 패딩 0·저장 CRC)은 보지 않는다(ValidateBulk·VerifyPackageFile이 본다).
Result<void> ValidateBulkLayout(const PackageContents& contents, uint64 bulkBytes) noexcept {
    const uint64 alignment = contents.header.bulkAlignment;
    uint64 end = 0;
    for (usize index = 0; index < contents.bulkEntries.Size(); ++index) {
        const PackageBulkEntry& entry = contents.bulkEntries[index];
        const uint64 expectedOffset = index == 0 ? 0 : AlignUp(end, alignment);
        if (entry.bulkId != index + 1 || entry.offset != expectedOffset ||
            (entry.flags & ~static_cast<uint32>(BulkEntryFlags::EditorOnly)) != 0 ||
            (contents.header.kind == PackageKind::Cooked && entry.flags != 0) ||
            (entry.codec == BulkCodec::None && entry.storedBytes != entry.rawBytes) ||
            entry.offset > bulkBytes || entry.storedBytes > bulkBytes - entry.offset) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        end = entry.offset + entry.storedBytes;
    }

    if (end != bulkBytes) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    return Result<void>::Success();
}

} // namespace package_detail

namespace {

// BulkIndex를 Bulk 섹션 내용과 대조한다: 배치(ValidateBulkLayout), entry 사이 패딩 0, 저장 CRC.
Result<uint64> ValidateBulk(const PackageContents& contents) noexcept {
    if (auto layout = package_detail::ValidateBulkLayout(contents, contents.bulk.Size()); !layout) {
        return Result<uint64>::Failure(layout.Error());
    }

    uint64 end = 0;
    for (const PackageBulkEntry& entry : contents.bulkEntries) {
        if (!AllZero(contents.bulk.Slice(static_cast<usize>(end),
                                         static_cast<usize>(entry.offset - end)))) {
            return Result<uint64>::Failure(ErrorCode::CorruptData);
        }

        const Span<const uint8> stored = contents.bulk.Slice(static_cast<usize>(entry.offset),
                                                             static_cast<usize>(entry.storedBytes));
        if (Crc32(stored) != entry.crc32) {
            return Result<uint64>::Failure(ErrorCode::CorruptData);
        }

        end = entry.offset + entry.storedBytes;
    }

    return Result<uint64>::Success(0);
}

// Summary 타입 표가 export 타입 집합과 정확히 같은지 본다(정렬은 읽을 때 확인했다). 할당 실패는
// 그 오류다(손상으로 분류하지 않는다).
Result<bool> TypeTableMatchesExports(Allocator& allocator,
                                     const PackageContents& contents) noexcept {
    Array<TypeId> types(allocator);
    if (auto reserved = types.TryReserve(contents.exports.Size()); !reserved) {
        return Result<bool>::Failure(reserved.Error());
    }

    for (const PackageExportEntry& entry : contents.exports) {
        (void)types.TryPushBack(entry.type);
    }

    types.Sort(
        [](const TypeId& left, const TypeId& right) noexcept { return GuidLess(left, right); });
    usize unique = 0;
    for (usize index = 0; index < types.Size(); ++index) {
        if (unique == 0 || !(types[unique - 1] == types[index])) {
            types[unique++] = types[index];
        }
    }

    if (unique != contents.summary.types.Size()) {
        return Result<bool>::Success(false);
    }

    for (usize index = 0; index < unique; ++index) {
        if (!(types[index] == contents.summary.types[index].type)) {
            return Result<bool>::Success(false);
        }
    }

    return Result<bool>::Success(true);
}

// Zstd entry의 저장 byte(블록 컨테이너)를 raw에 푼다. 컨테이너 형식·Zstd가 아닌 codec·사전
// 컨테이너·rawSize ≠ raw 크기(할당 전에 본다 — 해제 결과 크기가 아니라 헤더로)·블록 해제 실패는
// InvalidArgument, 할당 실패는 OutOfMemory다.
// 블록 컨테이너 오류를 bulk 해제 오류로 바꾼다: 할당 실패는 그대로, 나머지(형식·CRC·손상)는
// 저장 byte가 틀린 것이라 CorruptData다(AssetSystem 19장).
Result<void> ContainerFailure(ErrorCode error) noexcept {
    return Result<void>::Failure(error == ErrorCode::OutOfMemory ? ErrorCode::OutOfMemory
                                                                 : ErrorCode::CorruptData);
}

Result<void> DecodeBulkContainer(Span<const uint8> stored, Span<uint8> raw,
                                 Allocator& allocator) noexcept {
    auto reader = CompressedBlocksReader::TryOpen(stored, allocator);
    if (!reader) {
        return ContainerFailure(reader.Error());
    }

    if (reader.Value().Format() != CompressionFormat::Zstd ||
        reader.Value().RawSize() != raw.Size()) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    auto context = CompressedBlocksReadContext::TryCreate(reader.Value(), allocator);
    if (!context) {
        return Result<void>::Failure(context.Error());
    }

    if (auto read = reader.Value().TryRead(0, raw, stored, context.Value()); !read) {
        return ContainerFailure(read.Error());
    }

    return Result<void>::Success();
}

} // namespace

namespace package_detail {

Result<void> ValidateTagsRecord(Allocator& scratch, Span<const uint8> bytes) noexcept {
    auto header = ReadRecordHeader(bytes);
    if (!header) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    RecordLimits limits;
    limits.archive = TableLimits();
    limits.archive.maxValueBytes = static_cast<usize>(PackageMaxSectionBytes);
    limits.maxFields = NumericLimits<uint32>::Max();
    auto record =
        RecordReader::Open(scratch, bytes, header.Value().schema, header.Value().schemaVersion,
                           header.Value().schemaVersion, limits);
    if (!record) {
        return Result<void>::Failure(record.Error() == ErrorCode::OutOfMemory
                                         ? ErrorCode::OutOfMemory
                                         : ErrorCode::CorruptData);
    }

    Guid previous{};
    for (usize index = 0; index < record.Value().FieldCount(); ++index) {
        auto field = record.Value().Next();
        ROCKS_ASSERT(field.IsOk());
        if (index > 0 && !GuidLess(previous, field.Value().id)) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        previous = field.Value().id;
    }

    return Result<void>::Success();
}

} // namespace package_detail

const char* AssetDiagnosticName(AssetDiagnosticCode code) noexcept {
    switch (code) {
    case AssetDiagnosticCode::None:
        return "None";
    case AssetDiagnosticCode::DuplicateAssetId:
        return "DuplicateAssetId";
    case AssetDiagnosticCode::DuplicatePath:
        return "DuplicatePath";
    case AssetDiagnosticCode::CaseCollision:
        return "CaseCollision";
    case AssetDiagnosticCode::InvalidPath:
        return "InvalidPath";
    case AssetDiagnosticCode::MissingFile:
        return "MissingFile";
    case AssetDiagnosticCode::UnregisteredFile:
        return "UnregisteredFile";
    case AssetDiagnosticCode::TypeMismatch:
        return "TypeMismatch";
    case AssetDiagnosticCode::MissingDependency:
        return "MissingDependency";
    case AssetDiagnosticCode::UnsupportedCycle:
        return "UnsupportedCycle";
    case AssetDiagnosticCode::UnpackagedReference:
        return "UnpackagedReference";
    case AssetDiagnosticCode::CorruptPackage:
        return "CorruptPackage";
    case AssetDiagnosticCode::UnsupportedVersion:
        return "UnsupportedVersion";
    case AssetDiagnosticCode::NewerVersion:
        return "NewerVersion";
    case AssetDiagnosticCode::ExternalModification:
        return "ExternalModification";
    case AssetDiagnosticCode::NotDurable:
        return "NotDurable";
    case AssetDiagnosticCode::CorruptEntry:
        return "CorruptEntry";
    case AssetDiagnosticCode::IncompatibleCook:
        return "IncompatibleCook";
    case AssetDiagnosticCode::NeverCookReferenced:
        return "NeverCookReferenced";
    case AssetDiagnosticCode::EventBindingNotCookable:
        return "EventBindingNotCookable";
    case AssetDiagnosticCode::ContentNotTrusted:
        return "ContentNotTrusted";
    case AssetDiagnosticCode::InvalidMeshData:
        return "InvalidMeshData";
    case AssetDiagnosticCode::NonFiniteAttribute:
        return "NonFiniteAttribute";
    case AssetDiagnosticCode::UvOutOfRange:
        return "UvOutOfRange";
    case AssetDiagnosticCode::NonNormalizedWeights:
        return "NonNormalizedWeights";
    case AssetDiagnosticCode::TooManyInfluences:
        return "TooManyInfluences";
    case AssetDiagnosticCode::MaterialSlotFallback:
        return "MaterialSlotFallback";
    case AssetDiagnosticCode::NegativeBuildScale:
        return "NegativeBuildScale";
    case AssetDiagnosticCode::InfluencesTrimmed:
        return "InfluencesTrimmed";
    case AssetDiagnosticCode::MissingInfluences:
        return "MissingInfluences";
    case AssetDiagnosticCode::InvalidCollisionData:
        return "InvalidCollisionData";
    case AssetDiagnosticCode::UnsupportedCollisionShape:
        return "UnsupportedCollisionShape";
    case AssetDiagnosticCode::UnsupportedSetting:
        return "UnsupportedSetting";
    case AssetDiagnosticCode::InvalidSetting:
        return "InvalidSetting";
    case AssetDiagnosticCode::InvalidSkeletonData:
        return "InvalidSkeletonData";
    case AssetDiagnosticCode::SkeletonMismatch:
        return "SkeletonMismatch";
    case AssetDiagnosticCode::InvalidClipData:
        return "InvalidClipData";
    case AssetDiagnosticCode::UnknownBoneTrack:
        return "UnknownBoneTrack";
    case AssetDiagnosticCode::EventPropertiesDropped:
        return "EventPropertiesDropped";
    case AssetDiagnosticCode::CorruptBundle:
        return "CorruptBundle";
    case AssetDiagnosticCode::UnsupportedBundleVersion:
        return "UnsupportedBundleVersion";
    case AssetDiagnosticCode::UnknownField:
        return "UnknownField";
    case AssetDiagnosticCode::UnsupportedAsset:
        return "UnsupportedAsset";
    case AssetDiagnosticCode::UnrealPathRenamed:
        return "UnrealPathRenamed";
    case AssetDiagnosticCode::UnrealPathTooLong:
        return "UnrealPathTooLong";
    case AssetDiagnosticCode::UnrealPathCollision:
        return "UnrealPathCollision";
    case AssetDiagnosticCode::MissingUnrealDependency:
        return "MissingUnrealDependency";
    case AssetDiagnosticCode::UnsupportedSourceLayout:
        return "UnsupportedSourceLayout";
    case AssetDiagnosticCode::GammaMismatch:
        return "GammaMismatch";
    case AssetDiagnosticCode::UnrealNameNormalized:
        return "UnrealNameNormalized";
    case AssetDiagnosticCode::RootLost:
        return "RootLost";
    case AssetDiagnosticCode::AliasResolved:
        return "AliasResolved";
    case AssetDiagnosticCode::AliasInUse:
        return "AliasInUse";
    case AssetDiagnosticCode::AliasSearchTruncated:
        return "AliasSearchTruncated";
    case AssetDiagnosticCode::CorruptAliasTable:
        return "CorruptAliasTable";
    case AssetDiagnosticCode::ReferencedBy:
        return "ReferencedBy";
    case AssetDiagnosticCode::OrphanedOutput:
        return "OrphanedOutput";
    case AssetDiagnosticCode::MissingSource:
        return "MissingSource";
    case AssetDiagnosticCode::SourceChanged:
        return "SourceChanged";
    case AssetDiagnosticCode::InvalidSource:
        return "InvalidSource";
    case AssetDiagnosticCode::UnsupportedSourceFormat:
        return "UnsupportedSourceFormat";
    case AssetDiagnosticCode::AbsoluteSourcePath:
        return "AbsoluteSourcePath";
    case AssetDiagnosticCode::ImporterOutdated:
        return "ImporterOutdated";
    case AssetDiagnosticCode::InvalidImportOption:
        return "InvalidImportOption";
    case AssetDiagnosticCode::UnknownProperty:
        return "UnknownProperty";
    case AssetDiagnosticCode::NuggetMismatch:
        return "NuggetMismatch";
    case AssetDiagnosticCode::InvalidClassDefaults:
        return "InvalidClassDefaults";
    case AssetDiagnosticCode::LossySource:
        return "LossySource";
    case AssetDiagnosticCode::NonSrgbSource:
        return "NonSrgbSource";
    case AssetDiagnosticCode::IgnoredSourceChannels:
        return "IgnoredSourceChannels";
    case AssetDiagnosticCode::IgnoredSourceMips:
        return "IgnoredSourceMips";
    case AssetDiagnosticCode::IgnoredSourceImages:
        return "IgnoredSourceImages";
    case AssetDiagnosticCode::SrgbIgnored:
        return "SrgbIgnored";
    case AssetDiagnosticCode::PreserveBorderWithWrap:
        return "PreserveBorderWithWrap";
    case AssetDiagnosticCode::NuggetConflict:
        return "NuggetConflict";
    case AssetDiagnosticCode::SourceCleared:
        return "SourceCleared";
    case AssetDiagnosticCode::IgnoredSourceChunks:
        return "IgnoredSourceChunks";
    case AssetDiagnosticCode::EncoderUnavailable:
        return "EncoderUnavailable";
    case AssetDiagnosticCode::PlatformFormatFallback:
        return "PlatformFormatFallback";
    case AssetDiagnosticCode::CurveImportRowSkipped:
        return "CurveImportRowSkipped";
    case AssetDiagnosticCode::CurveTableUnusedRows:
        return "CurveTableUnusedRows";
    case AssetDiagnosticCode::MaterialTranslationFailed:
        return "MaterialTranslationFailed";
    }

    return "Unknown";
}

bool AssetDiagnosticIsWarning(AssetDiagnosticCode code) noexcept {
    switch (code) {
    case AssetDiagnosticCode::NonSrgbSource:
    case AssetDiagnosticCode::IgnoredSourceChannels:
    case AssetDiagnosticCode::IgnoredSourceMips:
    case AssetDiagnosticCode::IgnoredSourceImages:
    case AssetDiagnosticCode::SrgbIgnored:
    case AssetDiagnosticCode::PreserveBorderWithWrap:
    case AssetDiagnosticCode::NuggetConflict:
    case AssetDiagnosticCode::SourceCleared:
    case AssetDiagnosticCode::IgnoredSourceChunks:
    case AssetDiagnosticCode::EncoderUnavailable:
    case AssetDiagnosticCode::PlatformFormatFallback:
    case AssetDiagnosticCode::CurveImportRowSkipped:
    case AssetDiagnosticCode::CurveTableUnusedRows:
    case AssetDiagnosticCode::NotDurable:
    case AssetDiagnosticCode::NonFiniteAttribute:
    case AssetDiagnosticCode::UvOutOfRange:
    case AssetDiagnosticCode::NonNormalizedWeights:
    case AssetDiagnosticCode::MaterialSlotFallback:
    case AssetDiagnosticCode::NegativeBuildScale:
    case AssetDiagnosticCode::InfluencesTrimmed:
    case AssetDiagnosticCode::MissingInfluences:
    case AssetDiagnosticCode::UnsupportedCollisionShape:
    case AssetDiagnosticCode::UnsupportedSetting:
    case AssetDiagnosticCode::UnknownBoneTrack:
    case AssetDiagnosticCode::EventPropertiesDropped:
    case AssetDiagnosticCode::UnknownField:
    case AssetDiagnosticCode::UnsupportedAsset:
    case AssetDiagnosticCode::UnrealPathRenamed:
    case AssetDiagnosticCode::MissingUnrealDependency:
    case AssetDiagnosticCode::GammaMismatch:
    case AssetDiagnosticCode::UnrealNameNormalized:
    case AssetDiagnosticCode::AliasResolved:
    case AssetDiagnosticCode::AliasInUse:
    case AssetDiagnosticCode::AliasSearchTruncated:
    case AssetDiagnosticCode::OrphanedOutput:
    case AssetDiagnosticCode::AbsoluteSourcePath:
        return true;
    default:
        return false;
    }
}

Result<PackageHeader> ReadPackageHeader(Span<const uint8> headerBytes, uint64 fileBytes,
                                        AssetDiagnostic* failure) noexcept {
    // 1. 크기·magic·headerCrc·flags — 버전 필드를 믿기 전에 본다.
    if (headerBytes.Size() < PackageHeaderBytes || fileBytes < PackageHeaderBytes) {
        return Corrupt<PackageHeader>(failure, {}, 0, 0);
    }

    const uint8* bytes = headerBytes.Data();
    for (usize index = 0; index < 4; ++index) {
        if (bytes[index] != PackageMagic[index]) {
            return Corrupt<PackageHeader>(failure, {}, 0, 0);
        }
    }

    if (Crc32(Span<const uint8>(bytes, 60)) != LoadLittleEndian<uint32>(bytes + 60)) {
        return Corrupt<PackageHeader>(failure, {}, 0, 60);
    }

    const uint32 flags = LoadLittleEndian<uint32>(bytes + 8);
    if (flags != static_cast<uint32>(PackageKind::Editor) &&
        flags != static_cast<uint32>(PackageKind::Cooked)) {
        return Corrupt<PackageHeader>(failure, {}, 0, 8);
    }

    PackageHeader header;
    header.kind = static_cast<PackageKind>(flags);
    header.assetId = AssetId{LoadGuid(bytes + 32)};

    // 2. formatVersion(AS 5.3절 컨테이너 축).
    header.formatVersion = LoadLittleEndian<uint16>(bytes + 4);
    if (header.formatVersion > PackageFormatVersion) {
        return Unsupported<PackageHeader>(failure,
                                          header.kind == PackageKind::Editor
                                              ? AssetDiagnosticCode::NewerVersion
                                              : AssetDiagnosticCode::UnsupportedVersion,
                                          header.assetId, 0, 4);
    }

    if (header.formatVersion < PackageFormatVersion) {
        return Unsupported<PackageHeader>(failure, AssetDiagnosticCode::UnsupportedVersion,
                                          header.assetId, 0, 4);
    }

    // 3. 헤더 나머지.
    header.sectionCount = LoadLittleEndian<uint32>(bytes + 12);
    header.sectionTableOffset = LoadLittleEndian<uint64>(bytes + 16);
    header.fileBytes = LoadLittleEndian<uint64>(bytes + 24);
    header.bulkAlignment = LoadLittleEndian<uint32>(bytes + 48);
    const uint64 tableEnd =
        header.sectionTableOffset + uint64{PackageSectionEntryBytes} * header.sectionCount + 4;
    if (LoadLittleEndian<uint16>(bytes + 6) != PackageHeaderBytes || header.assetId.IsNull() ||
        header.sectionCount == 0 || header.sectionCount > PackageMaxSections ||
        header.sectionTableOffset < PackageHeaderBytes ||
        header.sectionTableOffset > PackageMaxFileBytes ||
        header.sectionTableOffset % PackageSectionAlignment != 0 || header.fileBytes != tableEnd ||
        header.fileBytes != fileBytes || header.fileBytes > PackageMaxFileBytes ||
        !IsPowerOfTwo(header.bulkAlignment) || header.bulkAlignment < PackageMinBulkAlignment ||
        header.bulkAlignment > PackageMaxBulkAlignment ||
        !AllZero(Span<const uint8>(bytes + 52, 8))) {
        return Corrupt<PackageHeader>(failure, header.assetId, 0, 0);
    }

    return Result<PackageHeader>::Success(header);
}

namespace {

// 읽는 범위다(AS 7.1절 13·15): Full은 모든 byte(패딩·모든 섹션 내용), Edit는 Bulk 섹션 내용만
// 빼고(에디터 지연 열기 — BulkIndex 배치는 섹션 크기로 본다), Index는 Objects·Bulk·BulkIndex
// 내용을 뺀다(색인). Full이 아니면 섹션 사이 패딩도 읽지 않는다.
enum class ReadScope {
    Full,
    Edit,
    Index
};

// 패키지 byte의 공급원이다(전체 파일 span 또는 범위 읽기 — AS 7.1절 13). Range의 view는 공급원이
// 살아 있는 동안 유효하다.
class PackageByteSource {
public:
    virtual uint64 FileBytes() const noexcept = 0;
    virtual ReadScope Scope() const noexcept = 0;
    virtual Result<Span<const uint8>> Range(uint64 offset, uint64 bytes) noexcept = 0;

protected:
    ~PackageByteSource() = default;
};

class SpanByteSource final : public PackageByteSource {
public:
    explicit SpanByteSource(Span<const uint8> file) noexcept : file_(file) {
    }

    uint64 FileBytes() const noexcept override {
        return file_.Size();
    }

    ReadScope Scope() const noexcept override {
        return ReadScope::Full;
    }

    Result<Span<const uint8>> Range(uint64 offset, uint64 bytes) noexcept override {
        return Result<Span<const uint8>>::Success(
            file_.Slice(static_cast<usize>(offset), static_cast<usize>(bytes)));
    }

private:
    Span<const uint8> file_;
};

// 범위마다 소유 버퍼를 하나 만든다(버퍼 표가 자라도 각 버퍼의 byte는 옮겨지지 않는다).
class RangeByteSource final : public PackageByteSource {
public:
    RangeByteSource(PackageRangeReader& reader, Array<Array<uint8>>& buffers,
                    ReadScope scope) noexcept
        : reader_(reader), buffers_(buffers), scope_(scope) {
    }

    uint64 FileBytes() const noexcept override {
        return reader_.FileBytes();
    }

    ReadScope Scope() const noexcept override {
        return scope_;
    }

    Result<Span<const uint8>> Range(uint64 offset, uint64 bytes) noexcept override {
        Array<uint8> buffer(buffers_.GetAllocator());
        if (auto sized = buffer.TryResize(static_cast<usize>(bytes)); !sized) {
            return Result<Span<const uint8>>::Failure(sized.Error());
        }

        if (auto read = reader_.ReadAt(offset, Span<uint8>(buffer.Data(), buffer.Size())); !read) {
            return Result<Span<const uint8>>::Failure(read.Error());
        }

        if (auto pushed = buffers_.TryPushBack(Move(buffer)); !pushed) {
            return Result<Span<const uint8>>::Failure(pushed.Error());
        }

        const Array<uint8>& stored = buffers_[buffers_.Size() - 1];
        return Result<Span<const uint8>>::Success(Span<const uint8>(stored.Data(), stored.Size()));
    }

private:
    PackageRangeReader& reader_;
    Array<Array<uint8>>& buffers_;
    ReadScope scope_;
};

bool SkipsContent(const PackageByteSource& source, PackageSectionKind kind) noexcept {
    if (source.Scope() == ReadScope::Full) {
        return false;
    }

    if (source.Scope() == ReadScope::Edit) {
        return kind == PackageSectionKind::Bulk;
    }

    return kind == PackageSectionKind::Objects || kind == PackageSectionKind::Bulk ||
           kind == PackageSectionKind::BulkIndex;
}

Result<PackageContents> ReadPackageFrom(Allocator& allocator, PackageByteSource& source,
                                        const PackageReadOptions& options,
                                        AssetDiagnostic* failure) noexcept {
    const uint64 fileBytes = source.FileBytes();
    auto headerBytes =
        source.Range(0, fileBytes < PackageHeaderBytes ? fileBytes : PackageHeaderBytes);
    if (!headerBytes) {
        return Resource<PackageContents>(failure, headerBytes.Error(), AssetId{}, 0);
    }

    auto header = ReadPackageHeader(headerBytes.Value(), fileBytes, failure);
    if (!header) {
        return Result<PackageContents>::Failure(header.Error());
    }

    PackageContents contents(allocator);
    contents.header = header.Value();
    const AssetId asset = contents.header.assetId;
    const bool cooked = contents.header.kind == PackageKind::Cooked;
    if (contents.header.kind != options.expectedKind) {
        return options.expectedKind == PackageKind::Cooked
                   ? Unsupported<PackageContents>(failure, AssetDiagnosticCode::UnsupportedVersion,
                                                  asset, 0, 8)
                   : Corrupt<PackageContents>(failure, asset, 0, 8);
    }

    // 3. 섹션 표.
    const usize tableOffset = static_cast<usize>(contents.header.sectionTableOffset);
    const usize tableBytes = PackageSectionEntryBytes * contents.header.sectionCount;
    auto tableRead = source.Range(tableOffset, tableBytes + sizeof(uint32));
    if (!tableRead) {
        return Resource<PackageContents>(failure, tableRead.Error(), asset, 0);
    }

    const Span<const uint8> table = tableRead.Value().Slice(0, tableBytes);
    if (Crc32(table) != LoadLittleEndian<uint32>(tableRead.Value().Data() + tableBytes)) {
        return Corrupt<PackageContents>(failure, asset, 0, tableOffset);
    }

    if (auto reserved = contents.sections.TryReserve(contents.header.sectionCount); !reserved) {
        return Resource<PackageContents>(failure, reserved.Error(), asset, 0);
    }

    uint64 end = PackageHeaderBytes;
    uint32 previousKind = 0;
    bool present[PackageKnownSectionKinds + 1] = {};
    for (uint32 index = 0; index < contents.header.sectionCount; ++index) {
        const uint8* row = table.Data() + usize{PackageSectionEntryBytes} * index;
        PackageSectionEntry entry;
        const uint32 kind = LoadLittleEndian<uint32>(row);
        entry.kind = static_cast<PackageSectionKind>(kind);
        entry.flags = LoadLittleEndian<uint32>(row + 4);
        entry.offset = LoadLittleEndian<uint64>(row + 8);
        entry.bytes = LoadLittleEndian<uint64>(row + 16);
        entry.crc32 = LoadLittleEndian<uint32>(row + 24);
        entry.schemaVersion = LoadLittleEndian<uint32>(row + 28);
        const uint64 alignment = kind == SectionKindValue(PackageSectionKind::Bulk)
                                     ? contents.header.bulkAlignment
                                     : PackageSectionAlignment;
        const bool perEntry =
            (entry.flags & static_cast<uint32>(PackageSectionFlags::PerEntryChecksum)) != 0;
        if (kind <= previousKind || (entry.flags & ~PackageSectionFlagsMask) != 0 ||
            entry.offset < PackageHeaderBytes ||
            entry.offset > contents.header.sectionTableOffset ||
            entry.bytes > contents.header.sectionTableOffset - entry.offset ||
            (entry.bytes == 0 && kind != SectionKindValue(PackageSectionKind::Bulk)) ||
            entry.offset != AlignUp(end, alignment) ||
            (kind != SectionKindValue(PackageSectionKind::Bulk) &&
             entry.bytes > PackageMaxSectionBytes) ||
            (perEntry && entry.crc32 != 0)) {
            return Corrupt<PackageContents>(failure, asset, kind, index);
        }

        // 섹션 사이 패딩은 전체 읽기만 본다(부분 읽기는 그 구간을 읽지 않는다 — AS 7.1절 13).
        if (source.Scope() == ReadScope::Full) {
            auto gap = source.Range(end, entry.offset - end);
            if (!AllZero(gap.Value())) {
                return Corrupt<PackageContents>(failure, asset, kind, end);
            }
        }

        if (IsKnownPackageSection(kind)) {
            present[kind] = true;
        }

        end = entry.offset + entry.bytes;
        previousKind = kind;
        (void)contents.sections.TryPushBack(entry);
    }

    if (contents.header.sectionTableOffset != AlignUp(end, PackageSectionAlignment)) {
        return Corrupt<PackageContents>(failure, asset, 0, end);
    }

    if (source.Scope() == ReadScope::Full) {
        auto tail = source.Range(end, contents.header.sectionTableOffset - end);
        if (!AllZero(tail.Value())) {
            return Corrupt<PackageContents>(failure, asset, 0, end);
        }
    }

    // 항상 있어야 하는 섹션, 함께 있어야 하는 Bulk·BulkIndex, 쿠킹에 없어야 하는 섹션.
    if (!present[SectionKindValue(PackageSectionKind::Summary)] ||
        !present[SectionKindValue(PackageSectionKind::Exports)] ||
        !present[SectionKindValue(PackageSectionKind::Objects)] ||
        present[SectionKindValue(PackageSectionKind::Bulk)] !=
            present[SectionKindValue(PackageSectionKind::BulkIndex)] ||
        (cooked && (present[SectionKindValue(PackageSectionKind::ImportRecord)] ||
                    present[SectionKindValue(PackageSectionKind::Tags)]))) {
        return Corrupt<PackageContents>(failure, asset, 0, 0);
    }

    // 4. 섹션별: 모르는 종류·flags·schemaVersion·CRC·내용.
    for (const PackageSectionEntry& entry : contents.sections) {
        const uint32 kind = SectionKindValue(entry.kind);
        const bool required =
            (entry.flags & static_cast<uint32>(PackageSectionFlags::Required)) != 0;
        if (!IsKnownPackageSection(kind)) {
            if (cooked || required) {
                return Unsupported<PackageContents>(failure,
                                                    cooked ? AssetDiagnosticCode::UnsupportedVersion
                                                           : AssetDiagnosticCode::NewerVersion,
                                                    asset, kind, 0);
            }

            contents.readOnly = true;
            continue;
        }

        if (entry.flags != PackageSectionRequiredFlags(entry.kind)) {
            return Corrupt<PackageContents>(failure, asset, kind, 0);
        }

        if (entry.schemaVersion != PackageSectionSchemaVersion) {
            if (cooked || entry.schemaVersion < PackageSectionSchemaVersion) {
                return Unsupported<PackageContents>(failure,
                                                    AssetDiagnosticCode::UnsupportedVersion, asset,
                                                    kind, entry.schemaVersion);
            }

            if (required) {
                return Unsupported<PackageContents>(failure, AssetDiagnosticCode::NewerVersion,
                                                    asset, kind, entry.schemaVersion);
            }

            contents.readOnly = true;
            continue;
        }

        // 부분 읽기는 Objects·Bulk·BulkIndex 내용을 읽지 않는다(표 판정은 위에서 마쳤다).
        if (SkipsContent(source, entry.kind)) {
            continue;
        }

        auto sectionRead = source.Range(entry.offset, entry.bytes);
        if (!sectionRead) {
            return Resource<PackageContents>(failure, sectionRead.Error(), asset, kind);
        }

        const Span<const uint8> bytes = sectionRead.Value();
        if (entry.kind != PackageSectionKind::Bulk && Crc32(bytes) != entry.crc32) {
            return Corrupt<PackageContents>(failure, asset, kind, 0);
        }

        Result<void> read = Result<void>::Success();
        switch (entry.kind) {
        case PackageSectionKind::Summary:
            read = ReadSummary(allocator, bytes, contents.header.kind, contents.summary);
            break;
        case PackageSectionKind::Exports:
            read = ReadExports(bytes, contents);
            break;
        case PackageSectionKind::ObjectImports:
            read = ReadImports(bytes, contents);
            break;
        case PackageSectionKind::Dependencies:
            read = ReadDependencies(bytes, contents);
            break;
        case PackageSectionKind::Objects:
            contents.objects = bytes;
            break;
        case PackageSectionKind::ImportRecord:
            // 내용 스키마는 A2(AssetDatabase)가 정한다. 여기서는 Record envelope만 확인한다.
            if (auto record = ReadRecordHeader(bytes); !record) {
                read = Result<void>::Failure(ErrorCode::CorruptData);
            }

            contents.importRecord = bytes;
            break;
        case PackageSectionKind::Tags:
            // 키(필드 ID) 엄격 오름차순(AS 5.2절 — 값 스키마는 A2).
            read = package_detail::ValidateTagsRecord(allocator, bytes);
            contents.tags = bytes;
            break;

        case PackageSectionKind::Bulk:
            contents.bulk = bytes;
            break;
        case PackageSectionKind::BulkIndex:
            read = ReadBulkIndex(bytes, contents);
            break;
        }

        if (!read) {
            return SectionFailure<PackageContents>(failure, read.Error(), asset, entry.kind, 0);
        }
    }

    // 섹션 사이 대조(내용 검증).
    if (auto valid = ValidateExports(allocator, contents); !valid) {
        return SectionFailure<PackageContents>(failure, valid.Error(), asset,
                                               PackageSectionKind::Exports, 0);
    }

    if (auto valid = ValidateImports(allocator, contents); !valid) {
        return SectionFailure<PackageContents>(failure, valid.Error(), asset,
                                               PackageSectionKind::ObjectImports, 0);
    }

    if (auto valid = ValidateDependencies(contents); !valid) {
        return SectionFailure<PackageContents>(failure, valid.Error(), asset,
                                               PackageSectionKind::Dependencies, 0);
    }

    // Bulk 판정: 전체 읽기는 내용까지, 편집 읽기는 섹션 크기로 배치만(내용은 지연 읽기가 entry마다
    // 본다), 색인은 BulkIndex를 읽지 않았다.
    Result<uint64> bulkValid = Result<uint64>::Success(0);
    if (source.Scope() == ReadScope::Full) {
        bulkValid = ValidateBulk(contents);
    } else if (source.Scope() == ReadScope::Edit) {
        uint64 bulkBytes = 0;
        for (const PackageSectionEntry& entry : contents.sections) {
            bulkBytes = entry.kind == PackageSectionKind::Bulk ? entry.bytes : bulkBytes;
        }

        if (auto layout = package_detail::ValidateBulkLayout(contents, bulkBytes); !layout) {
            bulkValid = Result<uint64>::Failure(layout.Error());
        }
    }

    if (!bulkValid) {
        return SectionFailure<PackageContents>(failure, bulkValid.Error(), asset,
                                               PackageSectionKind::BulkIndex, 0);
    }

    auto typesMatch = TypeTableMatchesExports(allocator, contents);
    if (!typesMatch) {
        return Resource<PackageContents>(failure, typesMatch.Error(), asset,
                                         SectionKindValue(PackageSectionKind::Summary));
    }

    if (!typesMatch.Value() || !ClassDefaultsWithinTypes(contents.summary)) {
        return Corrupt<PackageContents>(failure, asset,
                                        SectionKindValue(PackageSectionKind::Summary), 0);
    }

    // 버전 축(AS 5.3절): 엔진·타입·커스텀 버전.
    if (!cooked && !options.engineVersion.IsEmpty() &&
        !options.engineVersion.CanLoadPackageCompatibleWith(contents.summary.compatibleWith)) {
        return Unsupported<PackageContents>(failure, AssetDiagnosticCode::NewerVersion, asset,
                                            SectionKindValue(PackageSectionKind::Summary), 0);
    }

    if (options.types != nullptr) {
        for (usize index = 0; index < contents.summary.types.Size(); ++index) {
            const PackageTypeVersion& row = contents.summary.types[index];
            // 미등록(NotFound)·클래스가 아닌 타입(TypeMismatch)은 "현재 버전 없음"이다. 그 밖의
            // 실패(종료된 TypeRegistry의 ShuttingDown, lease 수 Overflow)는 파일 탓이 아니다.
            auto lease = options.types->Acquire(row.type);
            if (!lease && lease.Error() != ErrorCode::NotFound &&
                lease.Error() != ErrorCode::TypeMismatch) {
                return Resource<PackageContents>(failure, lease.Error(), asset, 0);
            }

            const uint32 current = lease ? lease.Value().Schema().version : 0;
            if (cooked) {
                if (!lease || current != row.version) {
                    return Unsupported<PackageContents>(
                        failure, AssetDiagnosticCode::UnsupportedVersion, asset,
                        SectionKindValue(PackageSectionKind::Summary), index);
                }
            } else if (!lease || row.version > current) {
                contents.readOnly = true;
                if (!lease) {
                    contents.unavailableExportType = true;
                }
            }

            if (!lease) {
                continue;
            }

            // 클래스 기본값 판정(A5c): 저장 쪽(행 + 지문 / 행 없음)과 지금 쪽(그 구성의 레코드
            // 있음 + 지문 / 레코드 없는 타입 / 레코드 실패)을 맞춘다.
            auto judged = JudgeClassDefaults(allocator, lease.Value(), contents.summary, cooked);
            if (!judged) {
                return Resource<PackageContents>(failure, judged.Error(), asset, 0);
            }

            if (judged.Value().same) {
                continue;
            }

            if (cooked) {
                return Unsupported<PackageContents>(
                    failure, AssetDiagnosticCode::UnsupportedVersion, asset,
                    SectionKindValue(PackageSectionKind::Summary), index);
            }

            // 타입 표 순서로 돌므로 두 목록은 TypeId 오름차순이다.
            Result<void> noted = Result<void>::Success();
            if (judged.Value().failed) {
                noted = contents.unavailableDefaults.TryPushBack(
                    PackageUnavailableDefaults{row.type, judged.Value().reason});
            } else {
                noted = contents.changedDefaults.TryPushBack(row.type);
            }

            if (!noted) {
                return Resource<PackageContents>(failure, noted.Error(), asset, 0);
            }
        }
    }

    // 커스텀 버전: 쿠킹은 표 전체가 같아야 하고, 에디터는 높거나 모르는 id면 읽기 전용이다.
    const Span<const PackageCustomVersion> currentCustom = options.customVersions;
    if (cooked) {
        bool same = currentCustom.Size() == contents.summary.customVersions.Size();
        for (usize index = 0; same && index < currentCustom.Size(); ++index) {
            same = currentCustom[index].id == contents.summary.customVersions[index].id &&
                   currentCustom[index].version == contents.summary.customVersions[index].version;
        }

        if (!same) {
            return Unsupported<PackageContents>(failure, AssetDiagnosticCode::UnsupportedVersion,
                                                asset,
                                                SectionKindValue(PackageSectionKind::Summary), 0);
        }
    } else {
        for (const PackageCustomVersion& stored : contents.summary.customVersions) {
            bool known = false;
            for (const PackageCustomVersion& now : currentCustom) {
                if (now.id == stored.id) {
                    known = stored.version <= now.version;
                    break;
                }
            }

            if (!known) {
                contents.readOnly = true;
            }
        }
    }

    // 지문: 헤더 + 섹션 표(+ tableCrc).
    Sha256 hash;
    hash.Update(headerBytes.Value());
    hash.Update(tableRead.Value());
    contents.fingerprint.digest = hash.Finish();
    return Result<PackageContents>::Success(Move(contents));
}

} // namespace

Result<PackageContents> ReadPackage(Allocator& allocator, Span<const uint8> file,
                                    const PackageReadOptions& options,
                                    AssetDiagnostic* failure) noexcept {
    SpanByteSource source(file);
    return ReadPackageFrom(allocator, source, options, failure);
}

Result<PackageIndexView> ReadPackageIndex(Allocator& allocator, PackageRangeReader& reader,
                                          const PackageReadOptions& options,
                                          AssetDiagnostic* failure) noexcept {
    PackageIndexView view(allocator);
    RangeByteSource source(reader, view.buffers, ReadScope::Index);
    auto contents = ReadPackageFrom(allocator, source, options, failure);
    if (!contents) {
        return Result<PackageIndexView>::Failure(contents.Error());
    }

    view.contents = Move(contents.Value());
    return Result<PackageIndexView>::Success(Move(view));
}

Result<PackageIndexView> ReadPackageForEdit(Allocator& allocator, PackageRangeReader& reader,
                                            const PackageReadOptions& options,
                                            AssetDiagnostic* failure) noexcept {
    PackageIndexView view(allocator);
    RangeByteSource source(reader, view.buffers, ReadScope::Edit);
    auto contents = ReadPackageFrom(allocator, source, options, failure);
    if (!contents) {
        return Result<PackageIndexView>::Failure(contents.Error());
    }

    view.contents = Move(contents.Value());
    return Result<PackageIndexView>::Success(Move(view));
}

Result<void> DecodePackageBulkEntry(const PackageBulkEntry& entry, Span<const uint8> stored,
                                    Span<uint8> raw, Allocator& allocator) noexcept {
    if (stored.Size() != entry.storedBytes || raw.Size() != entry.rawBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (entry.codec == BulkCodec::None) {
        for (usize index = 0; index < raw.Size(); ++index) {
            raw[index] = stored[index];
        }
    } else if (auto decoded = DecodeBulkContainer(stored, raw, allocator); !decoded) {
        return decoded;
    }

    if (!(Sha256::Hash(Span<const uint8>(raw.Data(), raw.Size())) == entry.sha256)) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    return Result<void>::Success();
}

Result<void> DecodePackageBulkEntry(const PackageBulkEntry& entry, Span<const uint8> stored,
                                    Array<uint8>& out, Allocator& allocator) noexcept {
    if (stored.Size() != entry.storedBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (entry.codec == BulkCodec::Zstd) {
        auto reader = CompressedBlocksReader::TryOpen(stored, allocator);
        if (!reader) {
            return ContainerFailure(reader.Error());
        }

        // 형식은 해제(DecodeBulkContainer)가 본다 — 여기서는 할당 크기만 먼저 확인한다.
        if (reader.Value().RawSize() != entry.rawBytes) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }
    }

    if (auto sized = out.TryResize(static_cast<usize>(entry.rawBytes)); !sized) {
        return sized;
    }

    return DecodePackageBulkEntry(entry, stored, out.View(), allocator);
}

Result<void> DecodePackageBulk(const PackageContents& contents, Array<uint8>& out,
                               Array<PackageBulkSlot>& slots, uint64 maxRawBytes,
                               AssetDiagnostic* failure) noexcept {
    const AssetId asset = contents.header.assetId;
    const uint32 section = SectionKindValue(PackageSectionKind::Bulk);
    uint64 total = 0;
    for (const PackageBulkEntry& entry : contents.bulkEntries) {
        if (entry.rawBytes > maxRawBytes - total) {
            return FailAsset<void>(failure, AssetDiagnosticCode::None, ErrorCode::CapacityExceeded,
                                   asset, section, entry.bulkId);
        }

        total += entry.rawBytes;
    }

    const usize baseBytes = out.Size();
    const usize baseSlots = slots.Size();
    auto restore = [&]() noexcept {
        (void)out.TryResize(baseBytes);
        (void)slots.TryResize(baseSlots);
    };

    if (auto reserved = slots.TryReserve(baseSlots + contents.bulkEntries.Size()); !reserved) {
        return Resource<void>(failure, reserved.Error(), asset, section);
    }

    auto grown = out.TryAppendUninitialized(static_cast<usize>(total));
    if (!grown) {
        return Resource<void>(failure, grown.Error(), asset, section);
    }

    usize cursor = baseBytes;
    for (const PackageBulkEntry& entry : contents.bulkEntries) {
        const Span<const uint8> stored = contents.bulk.Slice(static_cast<usize>(entry.offset),
                                                             static_cast<usize>(entry.storedBytes));
        const Span<uint8> raw(out.Data() + cursor, static_cast<usize>(entry.rawBytes));
        if (auto decoded = DecodePackageBulkEntry(entry, stored, raw, out.GetAllocator());
            !decoded) {
            restore();
            if (decoded.Error() == ErrorCode::OutOfMemory) {
                return Resource<void>(failure, decoded.Error(), asset, section);
            }

            return Corrupt<void>(failure, asset, section, entry.bulkId);
        }

        (void)slots.TryPushBack(PackageBulkSlot{cursor - baseBytes, entry.rawBytes, entry.sha256});
        cursor += raw.Size();
    }

    return Result<void>::Success();
}

namespace {

// 범위 [offset, offset + bytes)를 scratch 조각으로 읽어 CRC32를 계산하거나(zero = false) 모두 0인지
// 본다(zero = true — 결과 0이면 모두 0). 읽기 오류는 그 오류다.
Result<uint32> StreamRange(PackageRangeReader& reader, uint64 offset, uint64 bytes,
                           Span<uint8> scratch, bool zero) noexcept {
    uint32 crc = 0;
    while (bytes != 0) {
        const usize piece = bytes < scratch.Size() ? static_cast<usize>(bytes) : scratch.Size();
        const Span<uint8> chunk(scratch.Data(), piece);
        if (auto read = reader.ReadAt(offset, chunk); !read) {
            return Result<uint32>::Failure(read.Error());
        }

        const Span<const uint8> view(chunk.Data(), chunk.Size());
        if (zero) {
            if (!AllZero(view)) {
                return Result<uint32>::Success(1);
            }
        } else {
            crc = Crc32(view, crc);
        }

        offset += piece;
        bytes -= piece;
    }

    return Result<uint32>::Success(crc);
}

} // namespace

Result<PackageFingerprint> ReadPackageFingerprint(Allocator& allocator, PackageRangeReader& reader,
                                                  AssetDiagnostic* failure) noexcept {
    using Out = PackageFingerprint;
    const uint64 fileBytes = reader.FileBytes();
    uint8 header[PackageHeaderBytes] = {};
    const usize headerBytes =
        fileBytes < PackageHeaderBytes ? static_cast<usize>(fileBytes) : PackageHeaderBytes;
    if (auto read = reader.ReadAt(0, Span<uint8>(header, headerBytes)); !read) {
        (void)Resource<void>(failure, read.Error(), AssetId{}, 0);
        return Result<Out>::Failure(read.Error());
    }

    auto parsed = ReadPackageHeader(Span<const uint8>(header, headerBytes), fileBytes, failure);
    if (!parsed) {
        return Result<Out>::Failure(parsed.Error());
    }

    const AssetId asset = parsed.Value().assetId;
    const usize tableBytes = PackageSectionEntryBytes * parsed.Value().sectionCount;
    Array<uint8> table(allocator);
    if (auto sized = table.TryResize(tableBytes + sizeof(uint32)); !sized) {
        (void)Resource<void>(failure, sized.Error(), asset, 0);
        return Result<Out>::Failure(sized.Error());
    }

    if (auto read = reader.ReadAt(parsed.Value().sectionTableOffset, table.View()); !read) {
        (void)Resource<void>(failure, read.Error(), asset, 0);
        return Result<Out>::Failure(read.Error());
    }

    if (Crc32(table.View().Slice(0, tableBytes)) !=
        LoadLittleEndian<uint32>(table.Data() + tableBytes)) {
        (void)Corrupt<void>(failure, asset, 0, parsed.Value().sectionTableOffset);
        return Result<Out>::Failure(ErrorCode::CorruptData);
    }

    Sha256 hash;
    hash.Update(Span<const uint8>(header, headerBytes));
    hash.Update(table.View());
    PackageFingerprint fingerprint;
    fingerprint.digest = hash.Finish();
    return Result<Out>::Success(fingerprint);
}

Result<void> VerifyPackageFile(Allocator& allocator, PackageRangeReader& reader,
                               Span<uint8> scratch, AssetDiagnostic* failure) noexcept {
    if (scratch.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 헤더·표·읽는 섹션은 부분 읽기와 같은 판정이다(버전 축은 보지 않는다 — AS 7.1절 15).
    PackageReadOptions options;
    options.expectedKind = PackageKind::Editor;
    auto view = ReadPackageIndex(allocator, reader, options, failure);
    if (!view) {
        return Result<void>::Failure(view.Error());
    }

    PackageContents& contents = view.Value().contents;
    const AssetId asset = contents.header.assetId;
    // 섹션 사이 패딩과 건너뛴 섹션(Objects·Bulk·BulkIndex)의 CRC — 전체 읽기와 같은 진단이다.
    uint64 end = PackageHeaderBytes;
    const PackageSectionEntry* bulkIndex = nullptr;
    const PackageSectionEntry* bulk = nullptr;
    for (const PackageSectionEntry& entry : contents.sections) {
        const uint32 kind = SectionKindValue(entry.kind);
        auto gap = StreamRange(reader, end, entry.offset - end, scratch, true);
        if (!gap) {
            return Resource<void>(failure, gap.Error(), asset, kind);
        }

        if (gap.Value() != 0) {
            return Corrupt<void>(failure, asset, kind, end);
        }

        // Objects·BulkIndex는 Required 섹션이라 schemaVersion이 현재 값이다(다르면 부분 읽기가 이미
        // 거부했다 — 5.3절).
        const bool skipped = entry.kind == PackageSectionKind::Objects ||
                             entry.kind == PackageSectionKind::BulkIndex;
        if (skipped) {
            auto crc = StreamRange(reader, entry.offset, entry.bytes, scratch, false);
            if (!crc) {
                return Resource<void>(failure, crc.Error(), asset, kind);
            }

            if (crc.Value() != entry.crc32) {
                return Corrupt<void>(failure, asset, kind, 0);
            }
        }

        bulkIndex = entry.kind == PackageSectionKind::BulkIndex ? &entry : bulkIndex;
        bulk = entry.kind == PackageSectionKind::Bulk ? &entry : bulk;
        end = entry.offset + entry.bytes;
    }

    auto tail = StreamRange(reader, end, contents.header.sectionTableOffset - end, scratch, true);
    if (!tail) {
        return Resource<void>(failure, tail.Error(), asset, 0);
    }

    if (tail.Value() != 0) {
        return Corrupt<void>(failure, asset, 0, end);
    }

    if (bulkIndex == nullptr) {
        return Result<void>::Success();
    }

    // BulkIndex 해석·배치, entry 사이 패딩·저장 CRC(압축 해제·SHA-256은 하지 않는다).
    Array<uint8> indexBytes(allocator);
    if (auto sized = indexBytes.TryResize(static_cast<usize>(bulkIndex->bytes)); !sized) {
        return Resource<void>(failure, sized.Error(), asset, SectionKindValue(bulkIndex->kind));
    }

    if (auto read = reader.ReadAt(bulkIndex->offset, indexBytes.View()); !read) {
        return Resource<void>(failure, read.Error(), asset, SectionKindValue(bulkIndex->kind));
    }

    // BulkIndex는 Required 섹션이라 다른 schemaVersion은 부분 읽기가 이미 거부했다(5.3절).
    if (auto parsed = ReadBulkIndex(indexBytes.View(), contents); !parsed) {
        return SectionFailure<void>(failure, parsed.Error(), asset, PackageSectionKind::BulkIndex,
                                    0);
    }

    if (auto layout = package_detail::ValidateBulkLayout(contents, bulk->bytes); !layout) {
        return SectionFailure<void>(failure, layout.Error(), asset, PackageSectionKind::BulkIndex,
                                    0);
    }

    uint64 entryEnd = 0;
    for (const PackageBulkEntry& entry : contents.bulkEntries) {
        auto gap =
            StreamRange(reader, bulk->offset + entryEnd, entry.offset - entryEnd, scratch, true);
        auto crc = gap && gap.Value() == 0 ? StreamRange(reader, bulk->offset + entry.offset,
                                                         entry.storedBytes, scratch, false)
                                           : Result<uint32>::Success(entry.crc32 ^ 1u);
        const ErrorCode error = !gap ? gap.Error() : (!crc ? crc.Error() : ErrorCode::None);
        if (error != ErrorCode::None) {
            return Resource<void>(failure, error, asset, SectionKindValue(bulk->kind));
        }

        if (crc.Value() != entry.crc32) {
            return SectionFailure<void>(failure, ErrorCode::CorruptData, asset,
                                        PackageSectionKind::BulkIndex, 0);
        }

        entryEnd = entry.offset + entry.storedBytes;
    }

    return Result<void>::Success();
}

} // namespace rocks
