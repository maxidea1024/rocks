#pragma once

#include "../Private/PackageInternal.h"
#include "AssetFixtureValues.h"

#include <Serialization/Archive.h>
#include <Serialization/Record.h>
#include <Serialization/RecordList.h>

// 패키지 byte를 섹션 단위로 고쳐 다시 배치하는 시험 도우미다(포맷 필드 변조 시험). 유효한 패키지를
// 섹션 목록으로 풀고(Parse), 섹션 내용·flags·schemaVersion을 바꾸거나 섹션을 더하고 빼서, writer와
// 같은 배치 규칙(8 정렬·Bulk는 bulkAlignment 정렬·0 패딩·CRC·섹션 표·헤더)으로 다시 만든다(Build).
// 그래서 변조 하나가 CRC·배치 검사가 아니라 노리는 내용 검사에 닿는다.
namespace rocks::asset_test {

struct EditableSection {
    explicit EditableSection(Allocator& allocator) noexcept : bytes(allocator) {
    }

    uint32 kind = 0;
    uint32 flags = 0;
    uint32 schemaVersion = PackageSectionSchemaVersion;
    Array<uint8> bytes;
};

struct EditablePackage {
    explicit EditablePackage(Allocator& allocatorIn) noexcept
        : allocator(allocatorIn), sections(allocatorIn) {
    }

    Allocator& allocator;
    uint8 header[PackageHeaderBytes] = {};
    Array<EditableSection> sections;

    // file(유효한 패키지)을 푼다.
    bool Parse(Span<const uint8> file) noexcept {
        if (file.Size() < PackageHeaderBytes) {
            return false;
        }

        for (usize index = 0; index < PackageHeaderBytes; ++index) {
            header[index] = file[index];
        }

        const usize table = static_cast<usize>(LoadLittleEndian<uint64>(file.Data() + 16));
        const uint32 count = LoadLittleEndian<uint32>(file.Data() + 12);
        sections.Clear();
        for (uint32 index = 0; index < count; ++index) {
            const uint8* row = file.Data() + table + PackageSectionEntryBytes * index;
            auto added = sections.TryEmplaceBack(allocator);
            if (!added) {
                return false;
            }

            EditableSection& section = *added.Value();
            section.kind = LoadLittleEndian<uint32>(row);
            section.flags = LoadLittleEndian<uint32>(row + 4);
            section.schemaVersion = LoadLittleEndian<uint32>(row + 28);
            const uint64 offset = LoadLittleEndian<uint64>(row + 8);
            const uint64 bytes = LoadLittleEndian<uint64>(row + 16);
            if (!section.bytes.TryAppend(
                    file.Slice(static_cast<usize>(offset), static_cast<usize>(bytes)))) {
                return false;
            }
        }

        return true;
    }

    EditableSection* Find(PackageSectionKind kind) noexcept {
        for (EditableSection& section : sections) {
            if (section.kind == static_cast<uint32>(kind)) {
                return &section;
            }
        }

        return nullptr;
    }

    bool Remove(PackageSectionKind kind) noexcept {
        for (usize index = 0; index < sections.Size(); ++index) {
            if (sections[index].kind == static_cast<uint32>(kind)) {
                return sections.TryRemoveAt(index, 1).IsOk();
            }
        }

        return false;
    }

    // kind 순서 자리에 새 섹션을 넣는다(같은 kind가 있으면 그 뒤).
    EditableSection* Insert(uint32 kind, uint32 flags, Span<const uint8> bytes) noexcept {
        usize position = 0;
        while (position < sections.Size() && sections[position].kind <= kind) {
            ++position;
        }

        EditableSection section(allocator);
        section.kind = kind;
        section.flags = flags;
        if (!section.bytes.TryAppend(bytes) || !sections.TryInsert(Move(section), position)) {
            return nullptr;
        }

        return &sections[position];
    }

    bool Replace(PackageSectionKind kind, Span<const uint8> bytes) noexcept {
        EditableSection* section = Find(kind);
        if (section == nullptr) {
            return false;
        }

        section->bytes.Clear();
        return section->bytes.TryAppend(bytes).IsOk();
    }

    // writer와 같은 규칙으로 다시 배치한다. 헤더의 sectionCount·sectionTableOffset·fileBytes·
    // headerCrc는 다시 계산하고 나머지 헤더 칸은 그대로 둔다.
    Array<uint8> Build() const noexcept {
        Array<uint8> file(allocator);
        const uint32 alignment = LoadLittleEndian<uint32>(header + 48);
        (void)file.TryResize(PackageHeaderBytes);
        struct Row {
            uint64 offset;
            uint64 bytes;
            uint32 crc;
        };

        Array<Row> rows(allocator);
        for (const EditableSection& section : sections) {
            const bool bulk = section.kind == static_cast<uint32>(PackageSectionKind::Bulk);
            const uint64 align = bulk ? alignment : PackageSectionAlignment;
            (void)file.TryResize(static_cast<usize>(package_detail::AlignUp(file.Size(), align)));
            const uint32 crc =
                (section.flags & static_cast<uint32>(PackageSectionFlags::PerEntryChecksum)) != 0
                    ? 0
                    : Crc32(section.bytes.View());
            (void)rows.TryPushBack(Row{file.Size(), section.bytes.Size(), crc});
            (void)file.TryAppend(section.bytes.View());
        }

        (void)file.TryResize(
            static_cast<usize>(package_detail::AlignUp(file.Size(), PackageSectionAlignment)));
        const uint64 table = file.Size();
        for (usize index = 0; index < sections.Size(); ++index) {
            uint8 row[PackageSectionEntryBytes] = {};
            StoreLittleEndian<uint32>(row, sections[index].kind);
            StoreLittleEndian<uint32>(row + 4, sections[index].flags);
            StoreLittleEndian<uint64>(row + 8, rows[index].offset);
            StoreLittleEndian<uint64>(row + 16, rows[index].bytes);
            StoreLittleEndian<uint32>(row + 24, rows[index].crc);
            StoreLittleEndian<uint32>(row + 28, sections[index].schemaVersion);
            (void)file.TryAppend(Span<const uint8>(row, PackageSectionEntryBytes));
        }

        uint8 tableCrc[4];
        StoreLittleEndian<uint32>(
            tableCrc, Crc32(Span<const uint8>(file.Data() + table,
                                              PackageSectionEntryBytes * sections.Size())));
        (void)file.TryAppend(Span<const uint8>(tableCrc, 4));
        for (usize index = 0; index < PackageHeaderBytes; ++index) {
            file[index] = header[index];
        }

        StoreLittleEndian<uint32>(file.Data() + 12, static_cast<uint32>(sections.Size()));
        StoreLittleEndian<uint64>(file.Data() + 16, table);
        StoreLittleEndian<uint64>(file.Data() + 24, file.Size());
        StoreLittleEndian<uint32>(file.Data() + 60, Crc32(Span<const uint8>(file.Data(), 60)));
        return file;
    }
};

// ---- 표 섹션 직렬화(writer와 같은 고정 배치) ----

inline ArchiveLimits EditLimits() noexcept {
    ArchiveLimits limits;
    limits.maxTotalBytes = static_cast<usize>(PackageMaxSectionBytes) * 2;
    limits.maxValueBytes = static_cast<usize>(PackageMaxSectionBytes);
    limits.maxOperations = NumericLimits<uint64>::Max();
    limits.maxWork = NumericLimits<uint64>::Max();
    return limits;
}

inline Array<uint8> ExportsBytes(Allocator& allocator, Span<const PackageExportEntry> entries,
                                 uint32 count) noexcept {
    ArchiveWriter out(allocator, EditLimits());
    (void)out.WriteU32(count);
    for (const PackageExportEntry& entry : entries) {
        (void)out.WriteString(entry.name);
        (void)out.WriteGuid(entry.type);
        (void)out.WriteU32(entry.outerIndex);
        (void)out.WriteU32(entry.flags);
    }

    return Copy(allocator, out.View());
}

inline Array<uint8> ImportsBytes(Allocator& allocator, Span<const PackageImportEntry> entries,
                                 uint32 count) noexcept {
    ArchiveWriter out(allocator, EditLimits());
    (void)out.WriteU32(count);
    for (const PackageImportEntry& entry : entries) {
        (void)out.WriteGuid(entry.asset.value);
        (void)out.WriteU32(entry.outerImportIndex);
        (void)out.WriteString(entry.name);
        (void)out.WriteGuid(entry.type);
    }

    return Copy(allocator, out.View());
}

inline Array<uint8> DependenciesBytes(Allocator& allocator,
                                      Span<const PackageDependencyEntry> entries, uint32 count,
                                      uint32 extraFlags = 0, uint32 kindOverride = 0) noexcept {
    ArchiveWriter out(allocator, EditLimits());
    (void)out.WriteU32(count);
    for (const PackageDependencyEntry& entry : entries) {
        (void)out.WriteGuid(entry.asset.value);
        (void)out.WriteU32(kindOverride != 0 ? kindOverride : static_cast<uint32>(entry.kind));
        (void)out.WriteGuid(entry.expectedType);
        (void)out.WriteU32((entry.required ? package_detail::DependencyRequiredFlag : 0) |
                           extraFlags);
    }

    return Copy(allocator, out.View());
}

inline Array<uint8> BulkIndexBytes(Allocator& allocator, Span<const PackageBulkEntry> entries,
                                   uint32 count, uint32 codecOverride = 0) noexcept {
    ArchiveWriter out(allocator, EditLimits());
    (void)out.WriteU32(count);
    for (const PackageBulkEntry& entry : entries) {
        (void)out.WriteU32(entry.bulkId);
        (void)out.WriteU32(entry.flags);
        (void)out.WriteU64(entry.offset);
        (void)out.WriteU64(entry.storedBytes);
        (void)out.WriteU64(entry.rawBytes);
        (void)out.WriteU32(codecOverride != 0 ? codecOverride : static_cast<uint32>(entry.codec));
        (void)out.WriteU32(entry.crc32);
        (void)out.WriteRaw(Span<const uint8>(entry.sha256.bytes, Sha256Digest::Bytes));
    }

    return Copy(allocator, out.View());
}

// 배열 항목을 복사해 고칠 수 있게 한다(PackageContents의 항목은 file byte를 빌린다).
template <class T>
Array<T> CopyEntries(Allocator& allocator, const Array<T>& entries) noexcept {
    Array<T> copy(allocator);
    (void)copy.TryAppend(entries.View());
    return copy;
}

// ---- Summary Record(고장 하나를 넣어 다시 만든다) ----

enum class SummaryFault {
    None,
    Schema,
    SchemaVersion,
    ExtraField,
    NoTypes,
    NoCustoms,
    ItemFieldCount,
    ItemOneField,
    ItemNoId,
    ItemNoVersion,
    ItemIdWire,
    ItemVersionWire,
    // wire 종류는 맞고 payload 크기만 틀리다(Record reader는 크기를 wire 종류와 대조하지 않는다).
    ItemIdSize,
    ItemVersionSize,
    TypesUnsorted,
    TypesDuplicate,
    CustomsUnsorted,
    TypesNotList,
    NoSavedBy,
    NoCompatible,
    VersionFieldCount,
    VersionNoPart,
    VersionPartRange,
    VersionWire,
    VersionNotRecord,
    CompatPatch,
    CompatNewer,
    // compatibleWith가 savedBy 기준선보다 낮다(부 −1 → 1.1.0, 주 −1 → 0.9.0 — AS 5.7절 15).
    CompatOlderMinor,
    CompatOlderMajor,
    NoProfile,
    NoSettings,
    NoCooker,
    ProfileWire,
    ProfileEmpty,
    ProfileLong,
    ProfileUtf8,
    SettingsWire,
    SettingsSize,
    CookerWire,
    CookerSize,
    Count
};

struct SummaryInput {
    Span<const PackageTypeVersion> types;
    Span<const PackageCustomVersion> customs;
    bool editor = false;
    // 참이면 항목 고장(Item*·정렬)을 타입 표 대신 커스텀 버전 표에 넣는다.
    bool faultCustoms = false;
};

inline Result<void> AddVersionItem(Allocator& allocator, RecordListBuilder& list, Guid id,
                                   uint32 version, SummaryFault fault) noexcept {
    RecordBuilder item(allocator, package_detail::VersionItemSchema, 1);
    uint8 guid[16];
    StoreLittleEndian<uint64>(guid, id.high);
    StoreLittleEndian<uint64>(guid + 8, id.low);
    uint8 value[4];
    StoreLittleEndian<uint32>(value, version);
    Result<void> added = Result<void>::Success();
    if (fault == SummaryFault::ItemIdWire) {
        added = item.AddField(package_detail::VersionItemIdField, WireType::U32, value);
    } else if (fault == SummaryFault::ItemIdSize) {
        added = item.AddField(package_detail::VersionItemIdField, WireType::Guid,
                              Span<const uint8>(guid, sizeof(guid) - 1));
    } else if (fault != SummaryFault::ItemNoId) {
        added = item.AddField(package_detail::VersionItemIdField, WireType::Guid, guid);
    } else {
        added = item.AddField(Guid{7, 7}, WireType::Guid, guid);
    }

    if (added && fault == SummaryFault::ItemOneField) {
        auto built = item.Build();
        return built ? list.AddItem(built.Value().View()) : Result<void>::Failure(built.Error());
    }

    if (added && fault != SummaryFault::ItemNoVersion) {
        const usize versionBytes =
            fault == SummaryFault::ItemVersionSize ? sizeof(value) - 1 : sizeof(value);
        added = fault == SummaryFault::ItemVersionWire
                    ? item.AddField(package_detail::VersionItemVersionField, WireType::Guid, guid)
                    : item.AddField(package_detail::VersionItemVersionField, WireType::U32,
                                    Span<const uint8>(value, versionBytes));
    } else if (added) {
        added = item.AddField(Guid{8, 8}, WireType::U32, value);
    }

    if (added && fault == SummaryFault::ItemFieldCount) {
        added = item.AddField(Guid{9, 9}, WireType::U32, value);
    }

    if (!added) {
        return added;
    }

    auto built = item.Build();
    return built ? list.AddItem(built.Value().View()) : Result<void>::Failure(built.Error());
}

inline Result<void> AddEngine(Allocator& allocator, RecordBuilder& parent, Guid field,
                              EngineVersion version, SummaryFault fault) noexcept {
    if (fault == SummaryFault::VersionNotRecord) {
        uint8 value[4] = {};
        return parent.AddField(field, WireType::U32, value);
    }

    RecordBuilder record(allocator, package_detail::EngineVersionSchema, 1);
    const Guid ids[3] = {package_detail::EngineVersionMajorField,
                         package_detail::EngineVersionMinorField,
                         package_detail::EngineVersionPatchField};
    const uint32 values[3] = {version.majorVersion,
                              fault == SummaryFault::VersionPartRange ? 70000u
                                                                      : version.minorVersion,
                              version.patchVersion};
    for (usize index = 0; index < 3; ++index) {
        if (fault == SummaryFault::VersionFieldCount && index == 2) {
            break;
        }

        uint8 bytes[16] = {};
        StoreLittleEndian<uint32>(bytes, values[index]);
        const Guid id =
            fault == SummaryFault::VersionNoPart && index == 1 ? Guid{5, 5} : ids[index];
        Result<void> added = fault == SummaryFault::VersionWire && index == 0
                                 ? record.AddField(id, WireType::Guid, Span<const uint8>(bytes, 16))
                                 : record.AddField(id, WireType::U32, Span<const uint8>(bytes, 4));
        if (!added) {
            return added;
        }
    }

    auto built = record.Build();
    return built ? parent.AddField(field, WireType::Record, built.Value().View())
                 : Result<void>::Failure(built.Error());
}

// fault 하나를 넣은 Summary Record byte다(None이면 writer와 같은 byte).
inline Array<uint8> SummaryBytes(Allocator& allocator, const SummaryInput& input,
                                 SummaryFault fault) noexcept {
    RecordLimits limits;
    limits.archive = EditLimits();
    RecordBuilder summary(
        allocator, fault == SummaryFault::Schema ? Guid{3, 3} : package_detail::SummarySchema,
        fault == SummaryFault::SchemaVersion ? package_detail::SummarySchemaVersion + 1
                                             : package_detail::SummarySchemaVersion,
        limits);
    bool ok = true;
    auto list = [&](Guid field, bool types) noexcept {
        RecordListBuilder rows(allocator);
        const usize count = types ? input.types.Size() : input.customs.Size();
        for (usize index = 0; ok && index < count; ++index) {
            usize position = index;
            if ((fault == SummaryFault::TypesUnsorted && types) ||
                (fault == SummaryFault::CustomsUnsorted && !types)) {
                position = count - 1 - index;
            }

            const Guid id = types ? input.types[position].type : input.customs[position].id;
            const uint32 version =
                types ? input.types[position].version : input.customs[position].version;
            const SummaryFault itemFault = types != input.faultCustoms ? fault : SummaryFault::None;
            ok = AddVersionItem(allocator, rows, id, version, itemFault).IsOk();
            if (ok && types && fault == SummaryFault::TypesDuplicate && index == 0) {
                ok = AddVersionItem(allocator, rows, id, version, SummaryFault::None).IsOk();
            }
        }

        if (ok && types && fault == SummaryFault::TypesNotList) {
            uint8 value[4] = {};
            ok = summary.AddField(field, WireType::U32, value).IsOk();
            return;
        }

        ok = ok && rows.Finish(summary, field).IsOk();
    };

    list(fault == SummaryFault::NoTypes ? Guid{11, 11} : package_detail::SummaryTypesField, true);
    list(fault == SummaryFault::NoCustoms ? Guid{12, 12}
                                          : package_detail::SummaryCustomVersionsField,
         false);
    if (input.editor) {
        const EngineVersion saved = TestEngineVersion;
        EngineVersion compatible = saved.CompatibleBaseline();
        if (fault == SummaryFault::CompatPatch) {
            compatible.patchVersion = 1;
        }

        if (fault == SummaryFault::CompatNewer) {
            compatible.minorVersion = static_cast<uint16>(saved.minorVersion + 1);
        }

        if (fault == SummaryFault::CompatOlderMinor) {
            compatible.minorVersion = static_cast<uint16>(saved.minorVersion - 1);
        }

        if (fault == SummaryFault::CompatOlderMajor) {
            constexpr uint16 OlderMinor = 9;
            compatible.majorVersion = static_cast<uint16>(saved.majorVersion - 1);
            compatible.minorVersion = OlderMinor;
        }

        ok = ok && AddEngine(allocator, summary,
                             fault == SummaryFault::NoSavedBy ? Guid{13, 13}
                                                              : package_detail::SummarySavedByField,
                             saved, fault)
                       .IsOk();
        ok = ok && AddEngine(allocator, summary,
                             fault == SummaryFault::NoCompatible
                                 ? Guid{14, 14}
                                 : package_detail::SummaryCompatibleWithField,
                             compatible, SummaryFault::None)
                       .IsOk();
    } else {
        char longProfile[PackageMaxStringBytes + 1];
        for (char& value : longProfile) {
            value = 'p';
        }

        const uint8 invalid[2] = {0xC3, 0x28};
        Span<const uint8> profile(reinterpret_cast<const uint8*>("Windows"), 7);
        if (fault == SummaryFault::ProfileEmpty) {
            profile = Span<const uint8>();
        } else if (fault == SummaryFault::ProfileLong) {
            profile =
                Span<const uint8>(reinterpret_cast<const uint8*>(longProfile), sizeof(longProfile));
        } else if (fault == SummaryFault::ProfileUtf8) {
            profile = Span<const uint8>(invalid, 2);
        }

        const Sha256Digest settings =
            Sha256::Hash(Span<const uint8>(reinterpret_cast<const uint8*>("windows-profile"), 15));
        uint8 cooker[4];
        StoreLittleEndian<uint32>(cooker, 7);
        ok = ok &&
             summary
                 .AddField(fault == SummaryFault::NoProfile ? Guid{15, 15}
                                                            : package_detail::SummaryProfileIdField,
                           fault == SummaryFault::ProfileWire ? WireType::Bytes : WireType::String,
                           profile)
                 .IsOk();
        ok = ok &&
             summary
                 .AddField(fault == SummaryFault::NoSettings
                               ? Guid{16, 16}
                               : package_detail::SummaryProfileSettingsField,
                           fault == SummaryFault::SettingsWire ? WireType::String : WireType::Bytes,
                           Span<const uint8>(settings.bytes, fault == SummaryFault::SettingsSize
                                                                 ? Sha256Digest::Bytes - 1
                                                                 : Sha256Digest::Bytes))
                 .IsOk();
        ok = ok && summary
                       .AddField(fault == SummaryFault::NoCooker
                                     ? Guid{17, 17}
                                     : package_detail::SummaryCookerVersionField,
                                 fault == SummaryFault::CookerWire ? WireType::U64 : WireType::U32,
                                 Span<const uint8>(cooker, fault == SummaryFault::CookerSize
                                                               ? sizeof(cooker) - 1
                                                               : sizeof(cooker)))
                       .IsOk();
    }

    if (ok && fault == SummaryFault::ExtraField) {
        uint8 value[4] = {};
        ok = summary.AddField(Guid{18, 18}, WireType::U32, value).IsOk();
    }

    auto built = summary.Build();
    if (!ok || !built) {
        return Array<uint8>(allocator);
    }

    return Move(built.Value());
}

inline bool EditorFault(SummaryFault fault) noexcept {
    return fault >= SummaryFault::NoSavedBy && fault <= SummaryFault::CompatOlderMajor;
}

inline bool CookedFault(SummaryFault fault) noexcept {
    return fault >= SummaryFault::NoProfile && fault <= SummaryFault::CookerSize;
}

// ---- Objects 섹션의 Record 필드 고치기 ----

// type(클래스 또는 struct) 스키마의 필드 name의 ID다.
inline Guid FieldIdOf(TypeRegistry& types, TypeId type, StringView name) noexcept {
    auto lease = types.Acquire(type);
    if (!lease) {
        lease = types.AcquireValueType(type, TypeKind::Struct);
    }

    if (!lease) {
        return Guid{};
    }

    for (const FieldSchema& field : lease.Value().Schema().fields) {
        if (StringView(field.name.Data(), field.name.Size()) == name) {
            return field.id;
        }
    }

    return Guid{};
}

// Objects 섹션에서 필드 id의 첫 TLV를 찾아 payload offset(섹션 기준)을 돌려준다. TLV는
// fieldGuid16(high·low LE)·type4·byteLength8·payload다(Serialization Record).
inline constexpr usize RecordFieldIdBytes = 16;
// TLV에서 payload가 시작하는 위치다(id 16 + type 4 + byteLength 8).
inline constexpr usize RecordFieldPayloadOffset =
    RecordFieldIdBytes + sizeof(uint32) + sizeof(uint64);

inline usize FieldPayload(Span<const uint8> objects, Guid id) noexcept {
    uint8 pattern[RecordFieldIdBytes];
    StoreLittleEndian<uint64>(pattern, id.high);
    StoreLittleEndian<uint64>(pattern + sizeof(uint64), id.low);
    for (usize offset = 0; offset + RecordFieldPayloadOffset <= objects.Size(); ++offset) {
        bool same = true;
        for (usize index = 0; same && index < RecordFieldIdBytes; ++index) {
            same = objects[offset + index] == pattern[index];
        }

        if (same) {
            return offset + RecordFieldPayloadOffset;
        }
    }

    return IndexNone;
}

// Objects 섹션의 필드 id 첫 payload에 u32 value를 쓴 패키지다(섹션 CRC는 다시 맞춘다).
inline Array<uint8> WithObjectsU32(Allocator& allocator, const Array<uint8>& file, Guid id,
                                   uint32 value) noexcept {
    EditablePackage edit(allocator);
    if (!edit.Parse(file.View())) {
        return Array<uint8>(allocator);
    }

    EditableSection* objects = edit.Find(PackageSectionKind::Objects);
    const usize payload = objects != nullptr ? FieldPayload(objects->bytes.View(), id) : IndexNone;
    if (payload == IndexNone) {
        return Array<uint8>(allocator);
    }

    StoreLittleEndian<uint32>(objects->bytes.Data() + payload, value);
    return edit.Build();
}

} // namespace rocks::asset_test
