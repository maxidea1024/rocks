#include "PackageInternal.h"

#include <AssetRuntime/BulkData.generated.h>
#include <AssetRuntime/Package.h>
#include <AssetRuntime/PackageWriter.h>
#include <Compression/Compression.h>
#include <Core/Algorithms.h>
#include <Core/Encoding/Crc32.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Serialization/RecordList.h>

namespace rocks {

namespace {

using namespace package_detail;

constexpr uint32 EditorOnlyFlag = static_cast<uint32>(FieldFlags::EditorOnly);
constexpr uint32 RequiredFlag = static_cast<uint32>(FieldFlags::Required);

ArchiveLimits SectionLimits() noexcept {
    ArchiveLimits limits;
    limits.maxTotalBytes = static_cast<usize>(PackageMaxSectionBytes);
    limits.maxValueBytes = static_cast<usize>(PackageMaxSectionBytes);
    limits.maxOperations = NumericLimits<uint64>::Max();
    limits.maxWork = NumericLimits<uint64>::Max();
    return limits;
}

template <class T>
Result<T> InvalidOption() noexcept {
    return Result<T>::Failure(ErrorCode::InvalidArgument);
}

// 저장 전 bulk ref 하나와 새 번호다(첫 출현 순 = 새 id 순).
struct BulkDraft {
    BulkDataRef original{};
    bool editorOnly = true;
};

// writer의 그래프 hook이다: 다른 패키지 객체는 import, weak 밖 대상은 null, Soft 참조 수집,
// BulkDataRef 재번호.
class WriterHooks final : public FieldCodecHooks {
public:
    WriterHooks(Allocator& allocator, ObjectRegistry& registry, const ObjectAccessContext& access,
                ObjectHandle package, AssetId self) noexcept
        : registry_(registry), access_(access), package_(package), self_(self),
          softUses_(allocator), bulk_(allocator) {
    }

    Result<void> CollectSoftReference(const SoftReferenceInfo& info) noexcept override {
        if (info.asset == self_) {
            return Result<void>::Success();
        }

        DependencyUse use;
        use.asset = info.asset;
        use.hard = false;
        use.editorOnly = (info.effectiveFlags & EditorOnlyFlag) != 0;
        use.required = (info.effectiveFlags & RequiredFlag) != 0;
        return softUses_.TryPushBack(use);
    }

    Result<const void*> SubstituteStruct(const FieldCodecFrame& frame, TypeId type,
                                         const void* nativeKey, const void* in) noexcept override {
        (void)nativeKey;
        if (!(type == GeneratedType<BulkDataRef>::Id())) {
            return Result<const void*>::Success(in);
        }

        const BulkDataRef& value = *static_cast<const BulkDataRef*>(in);
        if (value.IsEmpty()) {
            return Result<const void*>::Success(in);
        }

        const bool editorOnly = (frame.effectiveFlags & EditorOnlyFlag) != 0;
        uint32 newId = 0;
        for (usize index = 0; index < bulk_.Size(); ++index) {
            if (bulk_[index].original.bulkId == value.bulkId) {
                // 같은 옛 id는 같은 새 id다. 크기·해시가 다르면 메모리 상태가 모순이다.
                if (!(bulk_[index].original == value)) {
                    return Result<const void*>::Failure(ErrorCode::InvalidArgument);
                }

                bulk_[index].editorOnly = bulk_[index].editorOnly && editorOnly;
                newId = static_cast<uint32>(index + 1);
                break;
            }
        }

        if (newId == 0) {
            if (bulk_.Size() >= NumericLimits<uint32>::Max() - 1) {
                return Result<const void*>::Failure(ErrorCode::CapacityExceeded);
            }

            if (auto pushed = bulk_.TryPushBack(BulkDraft{value, editorOnly}); !pushed) {
                return Result<const void*>::Failure(pushed.Error());
            }

            newId = static_cast<uint32>(bulk_.Size());
        }

        substituted_ = value;
        substituted_.bulkId = newId;
        return Result<const void*>::Success(&substituted_);
    }

    Result<ExternalAdmission>
    AdmitExternalReference(const ExternalReferenceInfo& info) noexcept override {
        if (info.weak) {
            return Result<ExternalAdmission>::Success(ExternalAdmission::Null);
        }

        auto outermost = registry_.GetOutermost(access_, info.target);
        if (!outermost) {
            return Result<ExternalAdmission>::Failure(outermost.Error());
        }

        if (outermost.Value() == package_) {
            return Result<ExternalAdmission>::Success(ExternalAdmission::Reject);
        }

        auto type = registry_.AcquireType(access_, outermost.Value());
        if (!type) {
            return Result<ExternalAdmission>::Failure(type.Error());
        }

        if (!type.Value().IsNativeA(NativeTypeKey<Package>())) {
            unpackaged_ = true;
            return Result<ExternalAdmission>::Success(ExternalAdmission::Reject);
        }

        return Result<ExternalAdmission>::Success(ExternalAdmission::Import);
    }

    Span<const DependencyUse> SoftUses() const noexcept {
        return softUses_.View();
    }

    Span<const BulkDraft> Bulk() const noexcept {
        return bulk_.View();
    }

    bool SawUnpackaged() const noexcept {
        return unpackaged_;
    }

private:
    ObjectRegistry& registry_;
    ObjectAccessContext access_;
    ObjectHandle package_;
    AssetId self_;
    Array<DependencyUse> softUses_;
    Array<BulkDraft> bulk_;
    BulkDataRef substituted_{};
    bool unpackaged_ = false;
};

// import 초안이다(대상 하나 또는 그 Outer 사슬의 한 단계).
struct ImportDraft {
    explicit ImportDraft(Allocator& allocator) noexcept : path(allocator), name(allocator) {
    }

    AssetId asset{};
    String path;
    String name;
    TypeId type{};
    // 이 항목이 외부 참조 slot이면 그 번호(1부터), Outer 사슬로만 생긴 항목이면 0이다.
    uint32 slot = 0;
};

bool DraftLess(const ImportDraft& left, const ImportDraft& right) noexcept {
    if (left.asset.value != right.asset.value) {
        return GuidLess(left.asset.value, right.asset.value);
    }

    return BytesLess(left.path.View(), right.path.View());
}

// 에셋마다 대표 에셋 타입이다(Hard 의존 expectedType).
struct RepresentativeType {
    AssetId asset{};
    TypeId type{};
};

// 패키지 handle의 AssetId다.
Result<AssetId> PackageIdOf(ObjectRegistry& registry, const ObjectAccessContext& access,
                            ObjectHandle package) noexcept {
    auto borrow = registry.Resolve<Package>(access, package);
    if (!borrow) {
        return Result<AssetId>::Failure(borrow.Error());
    }

    auto pointer = borrow.Value().Get();
    if (!pointer) {
        return Result<AssetId>::Failure(pointer.Error());
    }

    return Result<AssetId>::Success(pointer.Value()->Id());
}

// 외부 참조 대상 하나와 그 Outer 사슬(대표 에셋 아래)을 import 초안으로 더한다.
Result<void> AddImportDrafts(Allocator& allocator, ObjectRegistry& registry,
                             const ObjectAccessContext& access, const GraphExternal& external,
                             uint32 slot, Array<ImportDraft>& drafts,
                             Array<RepresentativeType>& representatives) noexcept {
    auto outermost = registry.GetOutermost(access, external.target);
    if (!outermost) {
        return Result<void>::Failure(outermost.Error());
    }

    auto asset = PackageIdOf(registry, access, outermost.Value());
    if (!asset) {
        return Result<void>::Failure(asset.Error());
    }

    if (asset.Value().IsNull()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    bool known = false;
    for (const RepresentativeType& entry : representatives) {
        known = known || entry.asset == asset.Value();
    }

    if (!known) {
        auto representative = registry.FindObject(access, outermost.Value(), StringView());
        if (!representative) {
            return Result<void>::Failure(representative.Error());
        }

        auto type = registry.AcquireType(access, representative.Value());
        if (!type) {
            return Result<void>::Failure(type.Error());
        }

        if (auto pushed =
                representatives.TryPushBack(RepresentativeType{asset.Value(), type.Value().Id()});
            !pushed) {
            return pushed;
        }
    }

    // 대상부터 대표 에셋 바로 아래까지(대상이 대표 에셋이면 대상만) 사슬을 걷는다.
    for (ObjectHandle cursor = external.target;;) {
        auto outer = registry.GetOuter(access, cursor);
        if (!outer) {
            return Result<void>::Failure(outer.Error());
        }

        const bool isTarget = cursor == external.target;
        const bool isRepresentative = outer.Value() == outermost.Value();
        if (isTarget || !isRepresentative) {
            auto pushed = drafts.TryEmplaceBack(allocator);
            if (!pushed) {
                return Result<void>::Failure(pushed.Error());
            }

            ImportDraft& draft = *pushed.Value();
            draft.asset = asset.Value();
            draft.slot = isTarget ? slot : 0;
            if (auto path = registry.GetObjectPath(access, cursor, draft.path); !path) {
                return path;
            }

            if (auto name = registry.GetName(access, cursor, draft.name); !name) {
                return name;
            }

            if (isTarget) {
                draft.type = external.type;
            } else {
                auto type = registry.AcquireType(access, cursor);
                if (!type) {
                    return Result<void>::Failure(type.Error());
                }

                draft.type = type.Value().Id();
            }
        }

        if (isRepresentative) {
            break;
        }

        cursor = outer.Value();
    }

    return Result<void>::Success();
}

// 초안을 (assetId, 경로) 순으로 정렬하고 같은 항목을 합친다.
void MergeDrafts(Array<ImportDraft>& drafts) noexcept {
    drafts.Sort(DraftLess);
    usize unique = 0;
    for (usize index = 0; index < drafts.Size(); ++index) {
        if (unique > 0 && drafts[unique - 1].asset == drafts[index].asset &&
            drafts[unique - 1].path.View() == drafts[index].path.View()) {
            if (drafts[index].slot != 0) {
                drafts[unique - 1].slot = drafts[index].slot;
            }

            continue;
        }

        if (unique != index) {
            drafts[unique] = Move(drafts[index]);
        }

        ++unique;
    }

    if (unique < drafts.Size()) {
        (void)drafts.TryRemoveAt(unique, drafts.Size() - unique);
    }
}

// 경로의 마지막 '.' 앞(Outer 경로)이다. '.'가 없으면 빈 view와 false다.
bool ParentPath(StringView path, StringView& parent) noexcept {
    for (usize index = path.ByteCount(); index > 0; --index) {
        if (path.Data()[index - 1] == '.') {
            parent = StringView(path.Data(), index - 1);
            return true;
        }
    }

    return false;
}

Result<void> BuildVersionList(Allocator& allocator, Span<const Guid> ids,
                              Span<const uint32> versions, RecordBuilder& parent,
                              Guid field) noexcept {
    RecordListLimits listLimits;
    listLimits.maxItems = PackageMaxObjectImports;
    listLimits.maxPayloadBytes = static_cast<usize>(PackageMaxSectionBytes);
    RecordListBuilder list(allocator, listLimits);
    for (usize index = 0; index < ids.Size(); ++index) {
        RecordBuilder item(allocator, VersionItemSchema, 1);
        uint8 guid[16];
        StoreLittleEndian<uint64>(guid, ids[index].high);
        StoreLittleEndian<uint64>(guid + 8, ids[index].low);
        uint8 version[4];
        StoreLittleEndian<uint32>(version, versions[index]);
        if (auto added = item.AddField(VersionItemIdField, WireType::Guid, guid); !added) {
            return added;
        }

        if (auto added = item.AddField(VersionItemVersionField, WireType::U32, version); !added) {
            return added;
        }

        auto built = item.Build();
        if (!built) {
            return Result<void>::Failure(built.Error());
        }

        if (auto added = list.AddItem(built.Value().View()); !added) {
            return added;
        }
    }

    return list.Finish(parent, field);
}

Result<void> AddEngineVersion(Allocator& allocator, RecordBuilder& parent, Guid field,
                              EngineVersion version) noexcept {
    RecordBuilder record(allocator, EngineVersionSchema, 1);
    const Guid ids[3] = {EngineVersionMajorField, EngineVersionMinorField, EngineVersionPatchField};
    const uint32 values[3] = {version.majorVersion, version.minorVersion, version.patchVersion};
    for (usize index = 0; index < 3; ++index) {
        uint8 bytes[4];
        StoreLittleEndian<uint32>(bytes, values[index]);
        if (auto added = record.AddField(ids[index], WireType::U32, bytes); !added) {
            return added;
        }
    }

    auto built = record.Build();
    if (!built) {
        return Result<void>::Failure(built.Error());
    }

    return parent.AddField(field, WireType::Record, built.Value().View());
}

// 0 byte를 count개 붙인다.
Result<void> AppendZeros(PackageSink& sink, uint64 count) noexcept {
    static constexpr uint8 Zeros[512] = {};
    while (count > 0) {
        const uint64 chunk = count < sizeof(Zeros) ? count : sizeof(Zeros);
        if (auto appended = sink.Append(Span<const uint8>(Zeros, static_cast<usize>(chunk)));
            !appended) {
            return appended;
        }

        count -= chunk;
    }

    return Result<void>::Success();
}

// sink 위치를 추적하며 섹션을 쓰고 표 항목을 모은다.
class SectionWriter {
public:
    SectionWriter(PackageSink& sink, Allocator& allocator) noexcept
        : sink_(sink), sections_(allocator) {
    }

    Result<void> Begin() noexcept {
        position_ = 0;
        if (auto zeros = AppendZeros(sink_, PackageHeaderBytes); !zeros) {
            return zeros;
        }

        position_ = PackageHeaderBytes;
        return Result<void>::Success();
    }

    Result<void> Pad(uint64 alignment) noexcept {
        const uint64 aligned = AlignUp(position_, alignment);
        if (auto zeros = AppendZeros(sink_, aligned - position_); !zeros) {
            return zeros;
        }

        position_ = aligned;
        return Result<void>::Success();
    }

    Result<void> Append(Span<const uint8> bytes) noexcept {
        if (auto appended = sink_.Append(bytes); !appended) {
            return appended;
        }

        position_ += bytes.Size();
        return Result<void>::Success();
    }

    // 비-Bulk 섹션 하나를 8 정렬로 쓴다.
    Result<void> WriteSection(PackageSectionKind kind, Span<const uint8> bytes) noexcept {
        if (bytes.Size() > PackageMaxSectionBytes) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        if (auto padded = Pad(PackageSectionAlignment); !padded) {
            return padded;
        }

        PackageSectionEntry entry;
        entry.kind = kind;
        entry.flags = PackageSectionRequiredFlags(kind);
        entry.offset = position_;
        entry.bytes = bytes.Size();
        entry.crc32 = Crc32(bytes);
        if (auto pushed = sections_.TryPushBack(entry); !pushed) {
            return pushed;
        }

        return Append(bytes);
    }

    // 직접 쓴 섹션(Bulk)의 표 항목을 더한다.
    Result<void> AddEntry(const PackageSectionEntry& entry) noexcept {
        return sections_.TryPushBack(entry);
    }

    uint64 Position() const noexcept {
        return position_;
    }

    Span<const PackageSectionEntry> Sections() const noexcept {
        return sections_.View();
    }

    PackageSink& Sink() noexcept {
        return sink_;
    }

private:
    PackageSink& sink_;
    Array<PackageSectionEntry> sections_;
    uint64 position_ = 0;
};

// bulk entry를 Bulk 섹션에 쓰고 BulkIndex 항목을 모은다.
Result<void> WriteBulk(Allocator& allocator, SectionWriter& out, const PackageWriteOptions& options,
                       uint32 alignment, Span<const BulkDraft> drafts,
                       Array<PackageBulkEntry>& index, AssetId self,
                       AssetDiagnostic* failure) noexcept {
    if (drafts.IsEmpty()) {
        return Result<void>::Success();
    }

    if (options.bulkSource == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto padded = out.Pad(alignment); !padded) {
        return padded;
    }

    const uint64 start = out.Position();
    Array<uint8> compressed(allocator);
    Result<CompressionSettings> settings = Result<CompressionSettings>::Failure(ErrorCode::None);
    if (options.bulkCodec == BulkCodec::Zstd) {
        settings = CompressionSettings::TryDefault(CompressionFormat::Zstd);
        if (!settings) {
            return Result<void>::Failure(settings.Error());
        }
    }

    for (usize position = 0; position < drafts.Size(); ++position) {
        const BulkDraft& draft = drafts[position];
        auto raw = options.bulkSource->ReadBulk(draft.original);
        if (!raw) {
            return Result<void>::Failure(raw.Error());
        }

        if (raw.Value().Size() != draft.original.rawBytes ||
            !(Sha256::Hash(raw.Value()) == draft.original.sha256)) {
            return FailAsset<void>(
                failure, AssetDiagnosticCode::CorruptPackage, ErrorCode::InvalidArgument, self,
                static_cast<uint32>(PackageSectionKind::Bulk), draft.original.bulkId);
        }

        Span<const uint8> stored = raw.Value();
        BulkCodec codec = BulkCodec::None;
        if (options.bulkCodec == BulkCodec::Zstd) {
            compressed.Clear();
            auto packed = TryCompressAppend(settings.Value(), raw.Value(), compressed);
            if (!packed) {
                return Result<void>::Failure(packed.Error());
            }

            // 압축이 줄이지 못하면 원본 그대로 둔다(결정적 — 같은 입력이면 같은 선택).
            if (compressed.Size() < raw.Value().Size()) {
                stored = compressed.View();
                codec = BulkCodec::Zstd;
            }
        }

        if (position > 0) {
            if (auto padded = out.Pad(alignment); !padded) {
                return padded;
            }
        }

        PackageBulkEntry entry;
        entry.bulkId = static_cast<uint32>(position + 1);
        entry.flags = draft.editorOnly && options.kind == PackageKind::Editor
                          ? static_cast<uint32>(BulkEntryFlags::EditorOnly)
                          : 0;
        entry.offset = out.Position() - start;
        entry.storedBytes = stored.Size();
        entry.rawBytes = raw.Value().Size();
        entry.codec = codec;
        entry.crc32 = Crc32(stored);
        entry.sha256 = draft.original.sha256;
        if (auto pushed = index.TryPushBack(entry); !pushed) {
            return pushed;
        }

        if (auto appended = out.Append(stored); !appended) {
            return appended;
        }
    }

    PackageSectionEntry section;
    section.kind = PackageSectionKind::Bulk;
    section.flags = PackageSectionRequiredFlags(PackageSectionKind::Bulk);
    section.offset = start;
    section.bytes = out.Position() - start;
    section.crc32 = 0;
    return out.AddEntry(section);
}

Result<void> ValidateOptions(const PackageWriteOptions& options, uint32& alignment) noexcept {
    const bool editor = options.kind == PackageKind::Editor;
    if (options.kind != PackageKind::Editor && options.kind != PackageKind::Cooked) {
        return InvalidOption<void>();
    }

    alignment = options.bulkAlignment != 0
                    ? options.bulkAlignment
                    : (editor ? PackageEditorBulkAlignment : PackageDefaultCookedBulkAlignment);
    if (!IsPowerOfTwo(alignment) || alignment < PackageMinBulkAlignment ||
        alignment > PackageMaxBulkAlignment ||
        (options.bulkCodec != BulkCodec::None && options.bulkCodec != BulkCodec::Zstd)) {
        return InvalidOption<void>();
    }

    if (editor) {
        if (options.engineVersion.IsEmpty() || options.profileId.ByteCount() != 0) {
            return InvalidOption<void>();
        }
    } else if (options.profileId.ByteCount() == 0 ||
               options.profileId.ByteCount() > PackageMaxStringBytes ||
               !options.profileId.IsValidUtf8() || !options.importRecord.IsEmpty() ||
               !options.tags.IsEmpty() || !options.extraDependencies.IsEmpty()) {
        return InvalidOption<void>();
    }

    for (usize index = 1; index < options.customVersions.Size(); ++index) {
        if (!GuidLess(options.customVersions[index - 1].id, options.customVersions[index].id)) {
            return InvalidOption<void>();
        }
    }

    for (const PackageDependencyEntry& extra : options.extraDependencies) {
        if (extra.asset.IsNull() || extra.expectedType != TypeId{} ||
            (extra.kind != PackageDependencyKind::Build &&
             extra.kind != PackageDependencyKind::ImportOwner)) {
            return InvalidOption<void>();
        }
    }

    if ((!options.importRecord.IsEmpty() && !ReadRecordHeader(options.importRecord)) ||
        (!options.tags.IsEmpty() && !ReadRecordHeader(options.tags))) {
        return InvalidOption<void>();
    }

    return Result<void>::Success();
}

} // namespace

Result<void> MemoryPackageSink::Append(Span<const uint8> bytes) noexcept {
    return bytes_.TryAppend(bytes);
}

Result<void> MemoryPackageSink::WriteAt(uint64 offset, Span<const uint8> bytes) noexcept {
    if (offset > bytes_.Size() || bytes.Size() > bytes_.Size() - offset) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize index = 0; index < bytes.Size(); ++index) {
        bytes_[static_cast<usize>(offset) + index] = bytes[index];
    }

    return Result<void>::Success();
}

namespace package_detail {

Result<void> MergeDependencyUses(Allocator& scratch, Span<const DependencyUse> uses, AssetId self,
                                 Array<PackageDependencyEntry>& out) noexcept {
    // 에셋별 집계다.
    struct Tally {
        AssetId asset{};
        bool hard = false;
        bool hardRequired = false;
        bool hardEditor = false;
        bool hardEditorRequired = false;
        bool soft = false;
        bool softRequired = false;
        bool softEditor = false;
        bool softEditorRequired = false;
    };

    Array<DependencyUse> sorted(scratch);
    if (auto copied = sorted.TryAppend(uses); !copied) {
        return copied;
    }

    sorted.Sort([](const DependencyUse& left, const DependencyUse& right) noexcept {
        return GuidLess(left.asset.value, right.asset.value);
    });
    for (usize begin = 0; begin < sorted.Size();) {
        Tally tally;
        tally.asset = sorted[begin].asset;
        usize end = begin;
        for (; end < sorted.Size() && sorted[end].asset == tally.asset; ++end) {
            const DependencyUse& use = sorted[end];
            if (use.hard) {
                (use.editorOnly ? tally.hardEditor : tally.hard) = true;
                (use.editorOnly ? tally.hardEditorRequired : tally.hardRequired) |= use.required;
            } else {
                (use.editorOnly ? tally.softEditor : tally.soft) = true;
                (use.editorOnly ? tally.softEditorRequired : tally.softRequired) |= use.required;
            }
        }

        begin = end;
        if (tally.asset == self || tally.asset.IsNull()) {
            continue;
        }

        // 계열마다 종류를 정한다. EditorOnly 계열 둘은 한 항목으로 합친다.
        bool editorEntry = false;
        bool editorRequired = false;
        if (tally.hard) {
            if (auto pushed = out.TryPushBack(PackageDependencyEntry{
                    tally.asset, PackageDependencyKind::Hard, TypeId{}, tally.hardRequired});
                !pushed) {
                return pushed;
            }
        } else if (tally.hardEditor) {
            editorEntry = true;
            editorRequired = tally.hardEditorRequired;
        }

        if (tally.soft) {
            if (auto pushed = out.TryPushBack(PackageDependencyEntry{
                    tally.asset, PackageDependencyKind::Soft, TypeId{}, tally.softRequired});
                !pushed) {
                return pushed;
            }
        } else if (tally.softEditor) {
            editorEntry = true;
            editorRequired = editorRequired || tally.softEditorRequired;
        }

        if (editorEntry) {
            if (auto pushed = out.TryPushBack(PackageDependencyEntry{
                    tally.asset, PackageDependencyKind::EditorOnly, TypeId{}, editorRequired});
                !pushed) {
                return pushed;
            }
        }
    }

    return Result<void>::Success();
}

} // namespace package_detail

Result<PackageWriteResult> WritePackage(Allocator& allocator, ObjectRegistry& registry,
                                        const ObjectAccessContext& access, ObjectHandle package,
                                        const PackageWriteOptions& options, PackageSink& sink,
                                        AssetDiagnostic* failure) noexcept {
    using Out = PackageWriteResult;
    uint32 alignment = 0;
    if (auto valid = ValidateOptions(options, alignment); !valid) {
        return Result<Out>::Failure(valid.Error());
    }

    auto self = PackageIdOf(registry, access, package);
    if (!self) {
        return Result<Out>::Failure(self.Error());
    }

    if (self.Value().IsNull()) {
        return InvalidOption<Out>();
    }

    const AssetId selfId = self.Value();

    // 1. export 집합(깊이 우선, 저장 불가 부분 트리 제외).
    Array<ObjectHandle> inners(allocator);
    if (auto collected = registry.CollectInners(access, package, InnerOrder::DepthFirst, inners);
        !collected) {
        return Result<Out>::Failure(collected.Error());
    }

    struct Frame {
        ObjectHandle handle{};
        uint32 exportIndex = 0;
        bool excluded = false;
    };

    Array<Frame> path(allocator);
    Array<ObjectHandle> exports(allocator);
    Array<PackageExportEntry> exportEntries(allocator);
    Array<String> names(allocator);
    Array<PackageTypeVersion> typeVersions(allocator);
    if (auto reserved = path.TryReserve(inners.Size() + 1); !reserved) {
        return Result<Out>::Failure(reserved.Error());
    }

    (void)path.TryPushBack(Frame{package, 0, false});
    for (ObjectHandle inner : inners) {
        auto outer = registry.GetOuter(access, inner);
        if (!outer) {
            return Result<Out>::Failure(outer.Error());
        }

        while (!path.IsEmpty() && !(path[path.Size() - 1].handle == outer.Value())) {
            (void)path.TryPopBack();
        }

        if (path.IsEmpty()) {
            return Result<Out>::Failure(ErrorCode::InvalidArgument);
        }

        const Frame parent = path[path.Size() - 1];
        auto type = registry.AcquireType(access, inner);
        if (!type) {
            return Result<Out>::Failure(type.Error());
        }

        const bool excluded = parent.excluded || IsUnsaveableType(type.Value());
        Frame frame{inner, 0, excluded};
        if (!excluded) {
            if (exports.Size() >= PackageMaxExports) {
                return Result<Out>::Failure(ErrorCode::CapacityExceeded);
            }

            auto name = names.TryEmplaceBack(allocator);
            if (!name) {
                return Result<Out>::Failure(name.Error());
            }

            if (auto got = registry.GetName(access, inner, *name.Value()); !got) {
                return Result<Out>::Failure(got.Error());
            }

            const bool primary = exports.IsEmpty();
            if (primary && (name.Value()->ByteCount() != 0 || parent.exportIndex != 0 ||
                            !type.Value().IsNativeA(NativeTypeKey<Asset>()))) {
                // export 1은 패키지 바로 아래 빈 이름 Asset(대표 에셋)이어야 한다.
                return InvalidOption<Out>();
            }

            if (auto pushed = exports.TryPushBack(inner); !pushed) {
                return Result<Out>::Failure(pushed.Error());
            }

            PackageExportEntry entry;
            entry.type = type.Value().Id();
            entry.outerIndex = parent.exportIndex;
            entry.flags = primary ? static_cast<uint32>(PackageExportFlags::Primary) : 0;
            if (auto pushed = exportEntries.TryPushBack(entry); !pushed) {
                return Result<Out>::Failure(pushed.Error());
            }

            frame.exportIndex = static_cast<uint32>(exports.Size());
            bool known = false;
            for (const PackageTypeVersion& row : typeVersions) {
                known = known || row.type == entry.type;
            }

            if (!known) {
                if (auto pushed = typeVersions.TryPushBack(
                        PackageTypeVersion{entry.type, type.Value().Schema().version});
                    !pushed) {
                    return Result<Out>::Failure(pushed.Error());
                }
            }
        }

        (void)path.TryPushBack(frame);
    }

    if (exports.IsEmpty()) {
        return InvalidOption<Out>();
    }

    for (usize index = 0; index < exportEntries.Size(); ++index) {
        exportEntries[index].name = names[index].View();
    }

    typeVersions.Sort([](const PackageTypeVersion& left, const PackageTypeVersion& right) noexcept {
        return GuidLess(left.type, right.type);
    });

    // 2. Objects(graph v2).
    WriterHooks hooks(allocator, registry, access, package, selfId);
    ObjectGraphSaveOptions save;
    save.unsaveable = UnsaveablePolicy::Exclude;
    save.policy.rejectMissingRequired = true;
    save.policy.excludedFlags = options.kind == PackageKind::Cooked ? EditorOnlyFlag : 0;
    save.hooks = &hooks;
    save.externals = GraphExternalPolicy::Admit;
    save.writeNodeTable = false;
    auto saved = SaveObjectGraphSet(allocator, registry, access, exports.View(), save);
    if (!saved) {
        if (hooks.SawUnpackaged()) {
            return FailAsset<Out>(failure, AssetDiagnosticCode::UnpackagedReference, saved.Error(),
                                  selfId, static_cast<uint32>(PackageSectionKind::Objects), 0);
        }

        return Result<Out>::Failure(saved.Error());
    }

    // 3. ObjectImports.
    const Span<const GraphExternal> externals = saved.Value().Externals();
    Array<ImportDraft> drafts(allocator);
    Array<RepresentativeType> representatives(allocator);
    for (usize slot = 0; slot < externals.Size(); ++slot) {
        if (auto added = AddImportDrafts(allocator, registry, access, externals[slot],
                                         static_cast<uint32>(slot + 1), drafts, representatives);
            !added) {
            return Result<Out>::Failure(added.Error());
        }
    }

    MergeDrafts(drafts);
    if (drafts.Size() > PackageMaxObjectImports) {
        return Result<Out>::Failure(ErrorCode::CapacityExceeded);
    }

    Array<uint32> indexBySlot(allocator);
    Array<uint32> outerIndex(allocator);
    if (auto resized = indexBySlot.TryResize(externals.Size()); !resized) {
        return Result<Out>::Failure(resized.Error());
    }

    if (auto resized = outerIndex.TryResize(drafts.Size()); !resized) {
        return Result<Out>::Failure(resized.Error());
    }

    for (usize index = 0; index < drafts.Size(); ++index) {
        const ImportDraft& draft = drafts[index];
        if (draft.slot != 0) {
            indexBySlot[draft.slot - 1] = static_cast<uint32>(index + 1);
        }

        StringView parent;
        outerIndex[index] = 0;
        if (ParentPath(draft.path.View(), parent)) {
            // 같은 에셋의 앞 항목 중 경로가 parent인 것(정렬되어 있으므로 반드시 앞에 있다).
            for (usize back = index; back > 0; --back) {
                if (drafts[back - 1].asset == draft.asset &&
                    drafts[back - 1].path.View() == parent) {
                    outerIndex[index] = static_cast<uint32>(back);
                    break;
                }
            }

            if (outerIndex[index] == 0) {
                return Result<Out>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    if (auto assigned = saved.Value().AssignExternalIndices(indexBySlot.View(),
                                                            static_cast<uint32>(drafts.Size()));
        !assigned) {
        return Result<Out>::Failure(assigned.Error());
    }

    // 4. Dependencies.
    Array<DependencyUse> uses(allocator);
    for (usize slot = 0; slot < externals.Size(); ++slot) {
        const ImportDraft* draft = nullptr;
        for (const ImportDraft& candidate : drafts) {
            if (candidate.slot == slot + 1) {
                draft = &candidate;
                break;
            }
        }

        DependencyUse use;
        use.asset = draft->asset;
        use.hard = true;
        use.editorOnly = (externals[slot].commonFlags & EditorOnlyFlag) != 0;
        use.required = externals[slot].required;
        if (auto pushed = uses.TryPushBack(use); !pushed) {
            return Result<Out>::Failure(pushed.Error());
        }
    }

    if (auto copied = uses.TryAppend(hooks.SoftUses()); !copied) {
        return Result<Out>::Failure(copied.Error());
    }

    Array<PackageDependencyEntry> dependencies(allocator);
    if (auto merged = MergeDependencyUses(allocator, uses.View(), selfId, dependencies); !merged) {
        return Result<Out>::Failure(merged.Error());
    }

    for (PackageDependencyEntry& entry : dependencies) {
        if (entry.kind != PackageDependencyKind::Hard) {
            continue;
        }

        for (const RepresentativeType& representative : representatives) {
            if (representative.asset == entry.asset) {
                entry.expectedType = representative.type;
            }
        }
    }

    for (const PackageDependencyEntry& extra : options.extraDependencies) {
        if (extra.asset == selfId) {
            return InvalidOption<Out>();
        }

        if (auto pushed = dependencies.TryPushBack(extra); !pushed) {
            return Result<Out>::Failure(pushed.Error());
        }
    }

    dependencies.Sort(DependencyLess);
    for (usize index = 1; index < dependencies.Size(); ++index) {
        if (!DependencyLess(dependencies[index - 1], dependencies[index])) {
            return InvalidOption<Out>();
        }
    }

    // 5. 섹션 byte.
    ArchiveWriter exportsBytes(allocator, SectionLimits());
    Result<void> written = exportsBytes.WriteU32(static_cast<uint32>(exportEntries.Size()));
    for (const PackageExportEntry& entry : exportEntries) {
        if (written) {
            written = exportsBytes.WriteString(entry.name);
        }

        if (written) {
            written = exportsBytes.WriteGuid(entry.type);
        }

        if (written) {
            written = exportsBytes.WriteU32(entry.outerIndex);
        }

        if (written) {
            written = exportsBytes.WriteU32(entry.flags);
        }
    }

    ArchiveWriter importsBytes(allocator, SectionLimits());
    if (written && !drafts.IsEmpty()) {
        written = importsBytes.WriteU32(static_cast<uint32>(drafts.Size()));
        for (usize index = 0; written && index < drafts.Size(); ++index) {
            written = importsBytes.WriteGuid(drafts[index].asset.value);
            if (written) {
                written = importsBytes.WriteU32(outerIndex[index]);
            }

            if (written) {
                written = importsBytes.WriteString(drafts[index].name.View());
            }

            if (written) {
                written = importsBytes.WriteGuid(drafts[index].type);
            }
        }
    }

    ArchiveWriter dependencyBytes(allocator, SectionLimits());
    if (written && !dependencies.IsEmpty()) {
        written = dependencyBytes.WriteU32(static_cast<uint32>(dependencies.Size()));
        for (usize index = 0; written && index < dependencies.Size(); ++index) {
            const PackageDependencyEntry& entry = dependencies[index];
            written = dependencyBytes.WriteGuid(entry.asset.value);
            if (written) {
                written = dependencyBytes.WriteU32(static_cast<uint32>(entry.kind));
            }

            if (written) {
                written = dependencyBytes.WriteGuid(entry.expectedType);
            }

            if (written) {
                written = dependencyBytes.WriteU32(entry.required ? DependencyRequiredFlag : 0);
            }
        }
    }

    if (!written) {
        return Result<Out>::Failure(written.Error());
    }

    RecordLimits summaryLimits;
    summaryLimits.archive = SectionLimits();
    RecordBuilder summary(allocator, SummarySchema, SummarySchemaVersion, summaryLimits);
    {
        Array<Guid> ids(allocator);
        Array<uint32> versions(allocator);
        for (const PackageTypeVersion& row : typeVersions) {
            if (!ids.TryPushBack(row.type) || !versions.TryPushBack(row.version)) {
                return Result<Out>::Failure(ErrorCode::OutOfMemory);
            }
        }

        if (auto list = BuildVersionList(allocator, ids.View(), versions.View(), summary,
                                         SummaryTypesField);
            !list) {
            return Result<Out>::Failure(list.Error());
        }

        ids.Clear();
        versions.Clear();
        for (const PackageCustomVersion& row : options.customVersions) {
            if (!ids.TryPushBack(row.id) || !versions.TryPushBack(row.version)) {
                return Result<Out>::Failure(ErrorCode::OutOfMemory);
            }
        }

        if (auto list = BuildVersionList(allocator, ids.View(), versions.View(), summary,
                                         SummaryCustomVersionsField);
            !list) {
            return Result<Out>::Failure(list.Error());
        }
    }

    if (options.kind == PackageKind::Editor) {
        if (auto added =
                AddEngineVersion(allocator, summary, SummarySavedByField, options.engineVersion);
            !added) {
            return Result<Out>::Failure(added.Error());
        }

        if (auto added = AddEngineVersion(allocator, summary, SummaryCompatibleWithField,
                                          options.engineVersion.CompatibleBaseline());
            !added) {
            return Result<Out>::Failure(added.Error());
        }
    } else {
        uint8 cooker[4];
        StoreLittleEndian<uint32>(cooker, options.cookerVersion);
        const Span<const uint8> profile(reinterpret_cast<const uint8*>(options.profileId.Data()),
                                        options.profileId.ByteCount());
        Result<void> added = summary.AddField(SummaryProfileIdField, WireType::String, profile);
        if (added) {
            added = summary.AddField(
                SummaryProfileSettingsField, WireType::Bytes,
                Span<const uint8>(options.profileSettings.bytes, Sha256Digest::Bytes));
        }

        if (added) {
            added = summary.AddField(SummaryCookerVersionField, WireType::U32, cooker);
        }

        if (!added) {
            return Result<Out>::Failure(added.Error());
        }
    }

    auto summaryBytes = summary.Build();
    if (!summaryBytes) {
        return Result<Out>::Failure(summaryBytes.Error());
    }

    Array<uint8> objects = saved.Value().TakeBytes();

    // 6. 물리 배치: 헤더 자리 → 섹션(kind 순) → 섹션 표 → 헤더.
    SectionWriter out(sink, allocator);
    Result<void> step = out.Begin();
    if (step) {
        step = out.WriteSection(PackageSectionKind::Summary, summaryBytes.Value().View());
    }

    if (step) {
        step = out.WriteSection(PackageSectionKind::Exports, exportsBytes.View());
    }

    if (step && !drafts.IsEmpty()) {
        step = out.WriteSection(PackageSectionKind::ObjectImports, importsBytes.View());
    }

    if (step && !dependencies.IsEmpty()) {
        step = out.WriteSection(PackageSectionKind::Dependencies, dependencyBytes.View());
    }

    if (step) {
        step = out.WriteSection(PackageSectionKind::Objects, objects.View());
    }

    if (step && !options.importRecord.IsEmpty()) {
        step = out.WriteSection(PackageSectionKind::ImportRecord, options.importRecord);
    }

    if (step && !options.tags.IsEmpty()) {
        step = out.WriteSection(PackageSectionKind::Tags, options.tags);
    }

    Array<PackageBulkEntry> bulkIndex(allocator);
    if (step) {
        step =
            WriteBulk(allocator, out, options, alignment, hooks.Bulk(), bulkIndex, selfId, failure);
    }

    if (step && !bulkIndex.IsEmpty()) {
        ArchiveWriter indexBytes(allocator, SectionLimits());
        step = indexBytes.WriteU32(static_cast<uint32>(bulkIndex.Size()));
        for (usize index = 0; step && index < bulkIndex.Size(); ++index) {
            const PackageBulkEntry& entry = bulkIndex[index];
            step = indexBytes.WriteU32(entry.bulkId);
            if (step) {
                step = indexBytes.WriteU32(entry.flags);
            }

            if (step) {
                step = indexBytes.WriteU64(entry.offset);
            }

            if (step) {
                step = indexBytes.WriteU64(entry.storedBytes);
            }

            if (step) {
                step = indexBytes.WriteU64(entry.rawBytes);
            }

            if (step) {
                step = indexBytes.WriteU32(static_cast<uint32>(entry.codec));
            }

            if (step) {
                step = indexBytes.WriteU32(entry.crc32);
            }

            if (step) {
                step =
                    indexBytes.WriteRaw(Span<const uint8>(entry.sha256.bytes, Sha256Digest::Bytes));
            }
        }

        if (step) {
            step = out.WriteSection(PackageSectionKind::BulkIndex, indexBytes.View());
        }
    }

    if (step) {
        step = out.Pad(PackageSectionAlignment);
    }

    if (!step) {
        return Result<Out>::Failure(step.Error());
    }

    const Span<const PackageSectionEntry> sections = out.Sections();
    const uint64 tableOffset = out.Position();
    Array<uint8> table(allocator);
    if (auto grown = table.TryResize(PackageSectionEntryBytes * sections.Size() + 4); !grown) {
        return Result<Out>::Failure(grown.Error());
    }

    for (usize index = 0; index < sections.Size(); ++index) {
        uint8* row = table.Data() + PackageSectionEntryBytes * index;
        const PackageSectionEntry& entry = sections[index];
        StoreLittleEndian<uint32>(row, static_cast<uint32>(entry.kind));
        StoreLittleEndian<uint32>(row + 4, entry.flags);
        StoreLittleEndian<uint64>(row + 8, entry.offset);
        StoreLittleEndian<uint64>(row + 16, entry.bytes);
        StoreLittleEndian<uint32>(row + 24, entry.crc32);
        StoreLittleEndian<uint32>(row + 28, PackageSectionSchemaVersion);
    }

    const usize tableBytes = PackageSectionEntryBytes * sections.Size();
    StoreLittleEndian<uint32>(table.Data() + tableBytes,
                              Crc32(Span<const uint8>(table.Data(), tableBytes)));
    if (auto appended = out.Append(table.View()); !appended) {
        return Result<Out>::Failure(appended.Error());
    }

    uint8 header[PackageHeaderBytes] = {};
    for (usize index = 0; index < 4; ++index) {
        header[index] = PackageMagic[index];
    }

    StoreLittleEndian<uint16>(header + 4, PackageFormatVersion);
    StoreLittleEndian<uint16>(header + 6, static_cast<uint16>(PackageHeaderBytes));
    StoreLittleEndian<uint32>(header + 8, static_cast<uint32>(options.kind));
    StoreLittleEndian<uint32>(header + 12, static_cast<uint32>(sections.Size()));
    StoreLittleEndian<uint64>(header + 16, tableOffset);
    StoreLittleEndian<uint64>(header + 24, out.Position());
    StoreLittleEndian<uint64>(header + 32, selfId.value.high);
    StoreLittleEndian<uint64>(header + 40, selfId.value.low);
    StoreLittleEndian<uint32>(header + 48, alignment);
    StoreLittleEndian<uint32>(header + 60, Crc32(Span<const uint8>(header, 60)));
    if (auto patched = sink.WriteAt(0, Span<const uint8>(header, PackageHeaderBytes)); !patched) {
        return Result<Out>::Failure(patched.Error());
    }

    PackageWriteResult result(allocator);
    result.fileBytes = out.Position();
    Sha256 fingerprint;
    fingerprint.Update(Span<const uint8>(header, PackageHeaderBytes));
    fingerprint.Update(table.View());
    result.fingerprint.digest = fingerprint.Finish();
    for (usize index = 0; index < hooks.Bulk().Size(); ++index) {
        if (auto pushed = result.bulkRemap.TryPushBack(BulkRemapEntry{
                hooks.Bulk()[index].original.bulkId, static_cast<uint32>(index + 1)});
            !pushed) {
            return Result<Out>::Failure(pushed.Error());
        }
    }

    return Result<Out>::Success(Move(result));
}

} // namespace rocks
