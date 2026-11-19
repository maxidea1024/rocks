#include "PackageInternal.h"

#include <AssetRuntime/BulkData.generated.h>
#include <AssetRuntime/Package.h>
#include <AssetRuntime/PackageWriter.h>
#include <Compression/CompressedBlocks.h>
#include <Compression/Compression.h>
#include <Core/Algorithms.h>
#include <Core/Assert.h>
#include <Core/Containers/HashMap.h>
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
          softUses_(allocator), bulk_(allocator), bulkIds_(allocator) {
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
            // 빈 값은 크기·해시도 0이어야 한다(로드가 손상으로 거부하는 값을 쓰지 않는다).
            if (IsMalformedEmptyBulkRef(value.rawBytes, value.sha256)) {
                return Result<const void*>::Failure(ErrorCode::InvalidArgument);
            }

            return Result<const void*>::Success(in);
        }

        const bool editorOnly = (frame.effectiveFlags & EditorOnlyFlag) != 0;
        uint32 newId = 0;
        if (const uint32* known = bulkIds_.Find(value.bulkId)) {
            // 같은 옛 id는 같은 새 id다. 크기·해시가 다르면 메모리 상태가 모순이다.
            BulkDraft& draft = bulk_[*known - 1];
            if (!(draft.original == value)) {
                return Result<const void*>::Failure(ErrorCode::InvalidArgument);
            }

            draft.editorOnly = draft.editorOnly && editorOnly;
            newId = *known;
        } else {
            if (bulk_.Size() >= NumericLimits<uint32>::Max() - 1) {
                return Result<const void*>::Failure(ErrorCode::CapacityExceeded);
            }

            if (auto pushed = bulk_.TryPushBack(BulkDraft{value, editorOnly}); !pushed) {
                return Result<const void*>::Failure(pushed.Error());
            }

            newId = static_cast<uint32>(bulk_.Size());
            if (auto inserted = bulkIds_.Insert(value.bulkId, newId); !inserted) {
                return Result<const void*>::Failure(inserted.Error());
            }
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

        // 대상과 그 조상(outermost 포함)은 그래프 저장이 Exclude 정책에서 WriteUnsaveable로 생존·
        // 타입을 먼저 확인했다(같은 소유 스레드, 그 사이 Registry 변경 없음 — AS 5.7절 19). 그래서
        // 아래 조회 단언은 걸리지 않는다.
        auto outermost = registry_.GetOutermost(access_, info.target);
        ROCKS_ASSERT(outermost.IsOk());

        // 자기 패키지 객체는 import 대상이 아니다.
        if (outermost.Value() == package_) {
            return Result<ExternalAdmission>::Success(ExternalAdmission::Reject);
        }

        auto type = registry_.AcquireType(access_, outermost.Value());
        ROCKS_ASSERT(type.IsOk());

        if (!type.Value().IsNativeA(NativeTypeKey<Package>())) {
            unpackaged_ = true;
            return Result<ExternalAdmission>::Success(ExternalAdmission::Reject);
        }

        // 다른 Package 객체 자체도 import 대상이 아니다(대상은 대표 에셋과 그 아래 객체 — AS 5.7절
        // 13).
        if (outermost.Value() == info.target) {
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
    // 옛 bulkId → 새 bulkId다(참조 수에 비례하게 — 같은 옛 id를 찾느라 bulk_를 훑지 않는다).
    HashMap<uint32, uint32> bulkIds_;
    BulkDataRef substituted_{};
    bool unpackaged_ = false;
};

// import 초안이다(외부 참조 대상 하나 또는 그 Outer 사슬의 한 단계 — 객체마다 하나).
struct ImportDraft {
    explicit ImportDraft(Allocator& allocator) noexcept : name(allocator) {
    }

    AssetId asset{};
    String name;
    TypeId type{};
    // Outer 사슬 위 초안 번호(1부터)다. 대표 에셋 바로 아래 객체와 대표 에셋 자신은 0이다(outer
    // import 0 — AS 5.2절).
    uint32 parent = 0;
    // 이 항목이 외부 참조 slot이면 그 번호(1부터), Outer 사슬로만 생긴 항목이면 0이다.
    uint32 slot = 0;
};

// 외부 참조 대상이 속한 패키지다(대표 에셋과 그 타입 — Hard 의존 expectedType).
struct ImportPackage {
    AssetId asset{};
    ObjectHandle representative{};
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
    ROCKS_ASSERT(pointer.IsOk());

    return Result<AssetId>::Success(pointer.Value()->Id());
}

// import 초안 모음이다. 객체마다 초안 하나이고 Outer 사슬은 이미 있는 초안에서 멈추며 패키지
// 조회는 패키지마다 한 번이다 — 외부 참조 대상과 그 Outer 수에 비례한다(AS 5.7절 13).
class ImportBuilder {
public:
    ImportBuilder(Allocator& allocator, ObjectRegistry& registry,
                  const ObjectAccessContext& access) noexcept
        : allocator_(allocator), registry_(registry), access_(access), drafts_(allocator),
          byObject_(allocator), packages_(allocator), types_(allocator), chain_(allocator) {
    }

    // 외부 참조 slot(1부터)의 대상과 그 Outer 사슬(대표 에셋 아래)을 더한다. 대상과 그 조상은
    // 그래프 저장이 Exclude 정책에서 WriteUnsaveable로 생존·타입을 먼저 확인했으므로(같은 소유
    // 스레드, 그 사이 Registry 변경 없음) 이 함수와 PackageOf의 조회 단언은 걸리지 않는다(AS 5.7절
    // 19).
    //
    // 오류: 대상 패키지가 null id면 InvalidArgument, 대표 에셋이 없으면 NotFound, Registry 조회의
    // 오류와 할당 실패는 그대로다.
    Result<void> Add(const GraphExternal& external, uint32 slot) noexcept {
        auto outermost = registry_.GetOutermost(access_, external.target);
        ROCKS_ASSERT(outermost.IsOk());

        auto package = PackageOf(outermost.Value());
        if (!package) {
            return Result<void>::Failure(package.Error());
        }

        // 대상부터 위로 걷되 이미 초안이 있는 객체나 대표 에셋에서 멈춘다.
        chain_.Clear();
        uint32 attach = 0;
        for (ObjectHandle cursor = external.target;;) {
            if (const uint32* known = byObject_.Find(cursor)) {
                attach = *known;
                break;
            }

            if (auto pushed = chain_.TryPushBack(cursor); !pushed) {
                return pushed;
            }

            if (cursor == package.Value().representative) {
                break;
            }

            auto outer = registry_.GetOuter(access_, cursor);
            ROCKS_ASSERT(outer.IsOk());

            if (outer.Value() == package.Value().representative) {
                break;
            }

            // 대표 에셋이 있는 패키지는 이름 있는 자식을 가질 수 없으므로(정체성 규칙 8) 사슬은
            // 패키지에 닿기 전에 대표 에셋을 지난다.
            ROCKS_ASSERT(!(outer.Value() == outermost.Value()));

            cursor = outer.Value();
        }

        // 위에서부터 초안을 만든다(부모 번호가 먼저 정해진다).
        for (usize position = chain_.Size(); position > 0; --position) {
            const ObjectHandle object = chain_[position - 1];
            auto added = drafts_.TryEmplaceBack(allocator_);
            if (!added) {
                return Result<void>::Failure(added.Error());
            }

            ImportDraft& draft = *added.Value();
            draft.asset = package.Value().asset;
            draft.parent = attach;
            if (auto name = registry_.GetName(access_, object, draft.name); !name) {
                return name;
            }

            if (object == external.target) {
                draft.type = external.type;
            } else {
                auto type = registry_.AcquireType(access_, object);
                ROCKS_ASSERT(type.IsOk());

                draft.type = type.Value().Id();
            }

            attach = static_cast<uint32>(drafts_.Size());
            if (auto inserted = byObject_.Insert(object, attach); !inserted) {
                return Result<void>::Failure(inserted.Error());
            }
        }

        drafts_[attach - 1].slot = slot;
        return Result<void>::Success();
    }

    Array<ImportDraft>& Drafts() noexcept {
        return drafts_;
    }

    // asset의 대표 에셋 타입이다. Hard 의존은 모두 Add를 거친 외부 참조에서 나오므로 그 패키지의
    // 타입이 언제나 있다(Add가 보지 않은 asset으로 부르는 것은 호출자 오류다).
    TypeId RepresentativeType(AssetId asset) const noexcept {
        const TypeId* type = types_.Find(asset.value);
        ROCKS_ASSERT(type != nullptr);
        return *type;
    }

    // ObjectImports 순서(AS 5.2절 — assetId 오름차순, 같은 에셋 안은 깊이 우선이고 같은 outer
    // 아래는 이름 byte 오름차순)의 초안 번호(0부터)를 order에 채운다. 자식 목록은 (asset, parent,
    // 이름) 정렬 한 번으로 만든다.
    Result<void> Order(Array<uint32>& order) noexcept {
        const usize count = drafts_.Size();
        Array<uint32> sorted(allocator_);
        Array<uint32> begin(allocator_);
        Array<uint32> end(allocator_);
        struct Frame {
            uint32 cursor = 0;
            uint32 end = 0;
        };

        Array<Frame> stack(allocator_);
        if (auto resized = sorted.TryResize(count); !resized) {
            return resized;
        }

        if (auto resized = begin.TryResize(count + 1); !resized) {
            return resized;
        }

        if (auto resized = end.TryResize(count + 1); !resized) {
            return resized;
        }

        if (auto reserved = order.TryReserve(count); !reserved) {
            return reserved;
        }

        if (auto reserved = stack.TryReserve(count); !reserved) {
            return reserved;
        }

        for (usize index = 0; index < count; ++index) {
            sorted[index] = static_cast<uint32>(index);
        }

        const Array<ImportDraft>& drafts = drafts_;
        sorted.Sort([&drafts](uint32 left, uint32 right) noexcept {
            const ImportDraft& a = drafts[left];
            const ImportDraft& b = drafts[right];
            if (a.asset.value != b.asset.value) {
                return GuidLess(a.asset.value, b.asset.value);
            }

            if (a.parent != b.parent) {
                return a.parent < b.parent;
            }

            return BytesLess(a.name.View(), b.name.View());
        });
        for (usize position = 0; position < count; ++position) {
            const uint32 parent = drafts_[sorted[position]].parent;
            if (parent != 0) {
                if (end[parent] == 0) {
                    begin[parent] = static_cast<uint32>(position);
                }

                end[parent] = static_cast<uint32>(position + 1);
            }
        }

        // 에셋마다 parent 0 항목이 정렬 순서에서 먼저 온다. 각 뿌리에서 깊이 우선으로 내보낸다.
        for (usize position = 0; position < count; ++position) {
            const uint32 root = sorted[position];
            if (drafts_[root].parent != 0) {
                continue;
            }

            (void)order.TryPushBack(root);
            (void)stack.TryPushBack(Frame{begin[root + 1], end[root + 1]});
            while (!stack.IsEmpty()) {
                Frame& top = stack[stack.Size() - 1];
                if (top.cursor >= top.end) {
                    (void)stack.TryPopBack();
                    continue;
                }

                const uint32 child = sorted[top.cursor++];
                (void)order.TryPushBack(child);
                (void)stack.TryPushBack(Frame{begin[child + 1], end[child + 1]});
            }
        }

        return Result<void>::Success();
    }

private:
    Result<ImportPackage> PackageOf(ObjectHandle outermost) noexcept {
        if (const ImportPackage* known = packages_.Find(outermost)) {
            return Result<ImportPackage>::Success(*known);
        }

        auto asset = PackageIdOf(registry_, access_, outermost);
        ROCKS_ASSERT(asset.IsOk());

        if (asset.Value().IsNull()) {
            return Result<ImportPackage>::Failure(ErrorCode::InvalidArgument);
        }

        auto representative = registry_.FindObject(access_, outermost, StringView());
        if (!representative) {
            return Result<ImportPackage>::Failure(representative.Error());
        }

        auto type = registry_.AcquireType(access_, representative.Value());
        ROCKS_ASSERT(type.IsOk());

        const ImportPackage package{asset.Value(), representative.Value(), type.Value().Id()};
        if (auto inserted = packages_.Insert(outermost, package); !inserted) {
            return Result<ImportPackage>::Failure(inserted.Error());
        }

        if (auto inserted = types_.InsertOrAssign(package.asset.value, package.type); !inserted) {
            return Result<ImportPackage>::Failure(inserted.Error());
        }

        return Result<ImportPackage>::Success(package);
    }

    Allocator& allocator_;
    ObjectRegistry& registry_;
    ObjectAccessContext access_;
    Array<ImportDraft> drafts_;
    HashMap<ObjectHandle, uint32> byObject_;
    HashMap<ObjectHandle, ImportPackage> packages_;
    HashMap<Guid, TypeId> types_;
    Array<ObjectHandle> chain_;
};

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
Result<void> AppendZerosTo(PackageSink& sink, uint64 count) noexcept {
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
        if (auto zeros = AppendZerosTo(sink_, PackageHeaderBytes); !zeros) {
            return zeros;
        }

        position_ = PackageHeaderBytes;
        return Result<void>::Success();
    }

    Result<void> Pad(uint64 alignment) noexcept {
        const uint64 aligned = AlignUp(position_, alignment);
        if (auto zeros = AppendZerosTo(sink_, aligned - position_); !zeros) {
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

    // 0 byte를 count개 붙인다(나중에 WriteAt으로 채울 자리).
    Result<void> AppendZeros(uint64 count) noexcept {
        if (auto zeros = AppendZerosTo(sink_, count); !zeros) {
            return zeros;
        }

        position_ += count;
        return Result<void>::Success();
    }

    // 이미 쓴 범위 [offset, offset + bytes.Size())를 덮어쓴다.
    Result<void> WriteAt(uint64 offset, Span<const uint8> bytes) noexcept {
        return sink_.WriteAt(offset, bytes);
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

private:
    PackageSink& sink_;
    Array<PackageSectionEntry> sections_;
    uint64 position_ = 0;
};

// bulk entry를 Bulk 섹션에 쓰고 BulkIndex 항목을 모은다. Zstd는 entry마다 블록 컨테이너(RCBK —
// PackageBulkBlockSizeLog2 블록)로 블록 하나씩 압축해 sink에 흘려 쓰고, 인덱스 자리는 비워 뒀다가
// 끝에 WriteAt으로 채운다 — 압축 메모리는 블록 하나다(AS 5.5절 3, 5.7절 6). 저장 CRC는 인덱스와
// payload의 CRC를 이어 붙여(Crc32Combine) 만든다.
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
    Result<CompressedBlocksSettings> blocks =
        Result<CompressedBlocksSettings>::Failure(ErrorCode::None);
    if (options.bulkCodec == BulkCodec::Zstd) {
        // 기본 Zstd 설정은 상수이고 블록 크기는 범위 안이라 실패하지 않는다.
        auto settings = CompressionSettings::TryDefault(CompressionFormat::Zstd);
        ROCKS_ASSERT(settings.IsOk());
        blocks = CompressedBlocksSettings::TryMake(settings.Value(), PackageBulkBlockSizeLog2);
        ROCKS_ASSERT(blocks.IsOk());
    }

    Array<uint8> payload(allocator);
    Array<uint8> container(allocator);
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

        if (position > 0) {
            if (auto padded = out.Pad(alignment); !padded) {
                return padded;
            }
        }

        const Span<const uint8> bytes = raw.Value();
        PackageBulkEntry entry;
        entry.bulkId = static_cast<uint32>(position + 1);
        // 쿠킹은 EditorOnly 필드를 쓰지 않으므로(치환 hook이 불리지 않는다) EditorOnly 초안이 없다.
        entry.flags = draft.editorOnly ? static_cast<uint32>(BulkEntryFlags::EditorOnly) : 0;
        entry.offset = out.Position() - start;
        entry.rawBytes = bytes.Size();
        entry.sha256 = draft.original.sha256;
        // 0 byte entry는 압축할 것이 없어 언제나 None이다.
        if (options.bulkCodec == BulkCodec::Zstd && !bytes.IsEmpty()) {
            auto writer = CompressedBlocksWriter::TryBegin(blocks.Value(), bytes.Size(), allocator);
            if (!writer) {
                return Result<void>::Failure(writer.Error());
            }

            const usize indexBytes = writer.Value().IndexBytes();
            const uint64 entryStart = out.Position();
            if (auto reserved = out.AppendZeros(indexBytes); !reserved) {
                return reserved;
            }

            constexpr usize blockBytes = usize{1} << PackageBulkBlockSizeLog2;
            uint32 payloadCrc = 0;
            uint64 payloadBytes = 0;
            for (usize offset = 0; offset < bytes.Size(); offset += blockBytes) {
                const usize length =
                    bytes.Size() - offset < blockBytes ? bytes.Size() - offset : blockBytes;
                payload.Clear();
                if (auto appended =
                        writer.Value().TryAppendBlock(bytes.Slice(offset, length), payload);
                    !appended) {
                    return Result<void>::Failure(appended.Error());
                }

                payloadCrc = Crc32(payload.View(), payloadCrc);
                payloadBytes += payload.Size();
                if (auto written = out.Append(payload.View()); !written) {
                    return written;
                }
            }

            if (auto resized = container.TryResize(indexBytes); !resized) {
                return resized;
            }

            // 모든 블록을 넣었고 container는 정확히 IndexBytes다(TryFinishIndex의 실패 조건이
            // 없다).
            auto finished = writer.Value().TryFinishIndex(container.View());
            ROCKS_ASSERT(finished.IsOk());

            if (auto patched = out.WriteAt(entryStart, container.View()); !patched) {
                return patched;
            }

            entry.storedBytes = indexBytes + payloadBytes;
            entry.codec = BulkCodec::Zstd;
            entry.crc32 = Crc32Combine(Crc32(container.View()), payloadCrc, payloadBytes);
        } else {
            entry.storedBytes = bytes.Size();
            entry.codec = BulkCodec::None;
            entry.crc32 = Crc32(bytes);
            if (auto appended = out.Append(bytes); !appended) {
                return appended;
            }
        }

        if (auto pushed = index.TryPushBack(entry); !pushed) {
            return pushed;
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

Result<void> ValidateOptions(Allocator& scratch, const PackageWriteOptions& options,
                             uint32& alignment) noexcept {
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

    if (!options.importRecord.IsEmpty() && !ReadRecordHeader(options.importRecord)) {
        return InvalidOption<void>();
    }

    // Tags는 reader와 같은 규칙(키 엄격 오름차순)으로 검사한다 — 로드가 거부할 byte를 쓰지 않는다.
    if (!options.tags.IsEmpty()) {
        if (auto valid = ValidateTagsRecord(scratch, options.tags); !valid) {
            return valid.Error() == ErrorCode::OutOfMemory
                       ? Result<void>::Failure(ErrorCode::OutOfMemory)
                       : InvalidOption<void>();
        }
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

    // 항목마다 그 종류를 정한 쓰임이 적어도 하나 있으므로 항목 수는 쓰임 수 이하다. 미리 잡아
    // 아래 추가는 할당하지 않는다.
    if (auto reserved = out.TryReserve(out.Size() + sorted.Size()); !reserved) {
        return reserved;
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
            (void)out.TryPushBack(PackageDependencyEntry{tally.asset, PackageDependencyKind::Hard,
                                                         TypeId{}, tally.hardRequired});
        } else if (tally.hardEditor) {
            editorEntry = true;
            editorRequired = tally.hardEditorRequired;
        }

        if (tally.soft) {
            (void)out.TryPushBack(PackageDependencyEntry{tally.asset, PackageDependencyKind::Soft,
                                                         TypeId{}, tally.softRequired});
        } else if (tally.softEditor) {
            editorEntry = true;
            editorRequired = editorRequired || tally.softEditorRequired;
        }

        if (editorEntry) {
            (void)out.TryPushBack(PackageDependencyEntry{
                tally.asset, PackageDependencyKind::EditorOnly, TypeId{}, editorRequired});
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
    if (auto valid = ValidateOptions(allocator, options, alignment); !valid) {
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
        ROCKS_ASSERT(outer.IsOk());

        // DepthFirst 순서라 outer는 언제나 경로 위에 있다(맨 아래는 패키지).
        while (!(path[path.Size() - 1].handle == outer.Value())) {
            (void)path.TryPopBack();
            ROCKS_ASSERT(!path.IsEmpty());
        }

        const Frame parent = path[path.Size() - 1];
        auto type = registry.AcquireType(access, inner);
        ROCKS_ASSERT(type.IsOk());

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

            // 첫 export의 부모는 언제나 패키지다: 저장되는 부모는 DepthFirst에서 자식보다 먼저
            // export되고, 제외된 부모의 자식은 함께 제외된다.
            const bool primary = exports.IsEmpty();
            ROCKS_ASSERT(!primary || parent.exportIndex == 0);
            if (primary && (name.Value()->ByteCount() != 0 ||
                            !type.Value().IsNativeA(NativeTypeKey<Asset>()))) {
                // export 1은 패키지 바로 아래 빈 이름 Asset(대표 에셋)이어야 한다.
                return InvalidOption<Out>();
            }

            // 다른 export는 대표 에셋 아래다: 대표 자식이 있는 Outer(패키지)는 이름 있는 자식을
            // 가질 수 없다(정체성 규칙 8 — ObjectFoundations 1.5절). reader도 outer 0인 하위
            // export를 거부한다.
            ROCKS_ASSERT(primary || parent.exportIndex != 0);

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
            // 타입 표는 export마다 모은 뒤 정렬·중복 제거한다(export 수 × 타입 수로 찾지 않는다).
            if (auto pushed = typeVersions.TryPushBack(
                    PackageTypeVersion{entry.type, type.Value().Schema().version});
                !pushed) {
                return Result<Out>::Failure(pushed.Error());
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
    usize uniqueTypes = 0;
    for (usize index = 0; index < typeVersions.Size(); ++index) {
        if (uniqueTypes == 0 || !(typeVersions[uniqueTypes - 1].type == typeVersions[index].type)) {
            typeVersions[uniqueTypes++] = typeVersions[index];
        }
    }

    (void)typeVersions.TryResize(uniqueTypes);

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

    // 3. ObjectImports(AS 5.2절 순서 — 외부 참조 대상과 그 Outer 사슬, 객체마다 한 항목).
    const Span<const GraphExternal> externals = saved.Value().Externals();
    ImportBuilder importBuilder(allocator, registry, access);
    for (usize slot = 0; slot < externals.Size(); ++slot) {
        if (auto added = importBuilder.Add(externals[slot], static_cast<uint32>(slot + 1));
            !added) {
            return Result<Out>::Failure(added.Error());
        }
    }

    const Array<ImportDraft>& drafts = importBuilder.Drafts();
    if (drafts.Size() > PackageMaxObjectImports) {
        return Result<Out>::Failure(ErrorCode::CapacityExceeded);
    }

    // order: import 번호 − 1 → 초안 번호, importOf: 초안 번호 → import 번호(1부터).
    Array<uint32> order(allocator);
    Array<uint32> importOf(allocator);
    Array<uint32> indexBySlot(allocator);
    if (auto ordered = importBuilder.Order(order); !ordered) {
        return Result<Out>::Failure(ordered.Error());
    }

    if (auto resized = importOf.TryResize(drafts.Size()); !resized) {
        return Result<Out>::Failure(resized.Error());
    }

    if (auto resized = indexBySlot.TryResize(externals.Size()); !resized) {
        return Result<Out>::Failure(resized.Error());
    }

    for (usize position = 0; position < order.Size(); ++position) {
        importOf[order[position]] = static_cast<uint32>(position + 1);
    }

    for (usize index = 0; index < drafts.Size(); ++index) {
        if (drafts[index].slot != 0) {
            indexBySlot[drafts[index].slot - 1] = importOf[index];
        }
    }

    if (auto assigned = saved.Value().AssignExternalIndices(indexBySlot.View(),
                                                            static_cast<uint32>(drafts.Size()));
        !assigned) {
        return Result<Out>::Failure(assigned.Error());
    }

    // 4. Dependencies.
    Array<DependencyUse> uses(allocator);
    if (auto reserved = uses.TryReserve(externals.Size() + hooks.SoftUses().Size()); !reserved) {
        return Result<Out>::Failure(reserved.Error());
    }

    for (usize slot = 0; slot < externals.Size(); ++slot) {
        DependencyUse use;
        use.asset = drafts[order[indexBySlot[slot] - 1]].asset;
        use.hard = true;
        use.editorOnly = (externals[slot].commonFlags & EditorOnlyFlag) != 0;
        use.required = externals[slot].required;
        (void)uses.TryPushBack(use);
    }

    (void)uses.TryAppend(hooks.SoftUses());

    Array<PackageDependencyEntry> dependencies(allocator);
    if (auto merged = MergeDependencyUses(allocator, uses.View(), selfId, dependencies); !merged) {
        return Result<Out>::Failure(merged.Error());
    }

    for (PackageDependencyEntry& entry : dependencies) {
        if (entry.kind == PackageDependencyKind::Hard) {
            entry.expectedType = importBuilder.RepresentativeType(entry.asset);
        }
    }

    if (auto reserved =
            dependencies.TryReserve(dependencies.Size() + options.extraDependencies.Size());
        !reserved) {
        return Result<Out>::Failure(reserved.Error());
    }

    for (const PackageDependencyEntry& extra : options.extraDependencies) {
        if (extra.asset == selfId) {
            return InvalidOption<Out>();
        }

        (void)dependencies.TryPushBack(extra);
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
        for (usize position = 0; written && position < order.Size(); ++position) {
            const ImportDraft& draft = drafts[order[position]];
            written = importsBytes.WriteGuid(draft.asset.value);
            if (written) {
                written = importsBytes.WriteU32(draft.parent == 0 ? 0 : importOf[draft.parent - 1]);
            }

            if (written) {
                written = importsBytes.WriteString(draft.name.View());
            }

            if (written) {
                written = importsBytes.WriteGuid(draft.type);
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
        const usize rows = typeVersions.Size() > options.customVersions.Size()
                               ? typeVersions.Size()
                               : options.customVersions.Size();
        if (auto reserved = ids.TryReserve(rows); !reserved) {
            return Result<Out>::Failure(reserved.Error());
        }

        if (auto reserved = versions.TryReserve(rows); !reserved) {
            return Result<Out>::Failure(reserved.Error());
        }

        for (const PackageTypeVersion& row : typeVersions) {
            (void)ids.TryPushBack(row.type);
            (void)versions.TryPushBack(row.version);
        }

        if (auto list = BuildVersionList(allocator, ids.View(), versions.View(), summary,
                                         SummaryTypesField);
            !list) {
            return Result<Out>::Failure(list.Error());
        }

        ids.Clear();
        versions.Clear();
        for (const PackageCustomVersion& row : options.customVersions) {
            (void)ids.TryPushBack(row.id);
            (void)versions.TryPushBack(row.version);
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
