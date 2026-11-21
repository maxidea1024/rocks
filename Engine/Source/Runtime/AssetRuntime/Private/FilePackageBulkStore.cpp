#include <AssetRuntime/FilePackageBulkStore.h>
#include <AssetRuntime/PackageRangeReader.h>
#include <Core/Assert.h>
#include <Core/Encoding/Crc32.h>
#include <Core/FileError.h>

// 패키지 bulk 저장소 구현이다(Docs/Design/AssetSystem.ko.md 6.1절, 7.1절 15 — A2c1).
namespace rocks {

namespace {

Result<void> Fail(AssetDiagnostic* failure, AssetDiagnosticCode code, ErrorCode error,
                  AssetId asset, uint32 section, uint64 detail) noexcept {
    if (failure != nullptr) {
        failure->code = code;
        failure->error = error;
        failure->asset = asset;
        failure->section = section;
        failure->detail = detail;
    }

    return Result<void>::Failure(error);
}

constexpr uint32 BulkSection = static_cast<uint32>(PackageSectionKind::Bulk);

} // namespace

// 열린 핸들 하나다.
struct PackageFileCache::Entry {
    explicit Entry(Allocator& allocator) noexcept : path(allocator) {
    }

    String path;
    FilePackageRangeReader reader;
    PackageFingerprint fingerprint{};
    uint64 lastUse = 0;
};

PackageFileCache::PackageFileCache(Allocator& allocator, platform::FileSystem& files,
                                   usize capacity) noexcept
    : allocator_(&allocator), files_(&files), capacity_(capacity), entries_(allocator) {
}

PackageFileCache::~PackageFileCache() = default;

Result<PackageRangeReader*> PackageFileCache::Acquire(StringView path,
                                                      const PackageFingerprint& expected,
                                                      AssetDiagnostic* failure) noexcept {
    using Out = PackageRangeReader*;
    if (capacity_ == 0) {
        return Result<Out>::Failure(ErrorCode::InvalidArgument);
    }

    ++clock_;
    for (UniquePtr<Entry>& entry : entries_) {
        if (entry->path.View() == path) {
            if (!(entry->fingerprint == expected)) {
                (void)Fail(failure, AssetDiagnosticCode::ExternalModification,
                           ErrorCode::StaleHandle, AssetId{}, 0, 0);
                Close(path);
                return Result<Out>::Failure(ErrorCode::StaleHandle);
            }

            entry->lastUse = clock_;
            return Result<Out>::Success(&entry->reader);
        }
    }

    // 자리가 없으면 가장 오래 쓰지 않은 핸들을 닫는다.
    if (entries_.Size() == capacity_) {
        usize oldest = 0;
        for (usize index = 1; index < entries_.Size(); ++index) {
            oldest = entries_[index]->lastUse < entries_[oldest]->lastUse ? index : oldest;
        }

        (void)entries_.TryRemoveAt(oldest);
    }

    auto made = UniquePtr<Entry>::TryMake(*allocator_, *allocator_);
    if (!made) {
        return Result<Out>::Failure(made.Error());
    }

    Entry& entry = *made.Value();
    if (auto assigned = entry.path.TryAssign(path); !assigned) {
        return Result<Out>::Failure(assigned.Error());
    }

    if (auto opened = entry.reader.Open(*files_, path); !opened) {
        (void)Fail(failure, AssetDiagnosticCode::None, opened.Error(), AssetId{}, 0, 0);
        return Result<Out>::Failure(opened.Error());
    }

    auto fingerprint = ReadPackageFingerprint(*allocator_, entry.reader, failure);
    if (!fingerprint) {
        return Result<Out>::Failure(fingerprint.Error());
    }

    if (!(fingerprint.Value() == expected)) {
        (void)Fail(failure, AssetDiagnosticCode::ExternalModification, ErrorCode::StaleHandle,
                   AssetId{}, 0, 0);
        return Result<Out>::Failure(ErrorCode::StaleHandle);
    }

    entry.fingerprint = fingerprint.Value();
    entry.lastUse = clock_;
    if (auto pushed = entries_.TryPushBack(Move(made.Value())); !pushed) {
        return Result<Out>::Failure(pushed.Error());
    }

    return Result<Out>::Success(&entries_[entries_.Size() - 1]->reader);
}

void PackageFileCache::CloseUnder(StringView directory) noexcept {
    for (usize index = entries_.Size(); index > 0; --index) {
        const StringView path = entries_[index - 1]->path.View();
        if (path.ByteCount() > directory.ByteCount() &&
            path.StartsWith(directory, StringComparison::CaseSensitive) &&
            path.Data()[directory.ByteCount()] == '/') {
            (void)entries_.TryRemoveAt(index - 1);
        }
    }
}

void PackageFileCache::Close(StringView path) noexcept {
    for (usize index = 0; index < entries_.Size(); ++index) {
        if (entries_[index]->path.View() == path) {
            (void)entries_.TryRemoveAt(index);
            return;
        }
    }
}

// id 하나의 bulk다. 파일 entry(fileId != 0)이거나 메모리에만 있는 것이다. raw는 처음 읽을 때
// 채우고 저장소 수명까지 옮기지 않는다(span 유효).
struct FilePackageBulkStore::Slot {
    explicit Slot(Allocator& allocator) noexcept : raw(allocator) {
    }

    PackageBulkEntry entry{};
    uint32 fileId = 0;
    uint64 fileOffset = 0;
    uint64 rawBytes = 0;
    Sha256Digest sha256{};
    bool loaded = false;
    Array<uint8> raw;
};

FilePackageBulkStore::FilePackageBulkStore(Allocator& allocator, PackageFileCache& cache) noexcept
    : allocator_(&allocator), cache_(&cache), path_(allocator), slots_(allocator),
      table_(allocator) {
}

FilePackageBulkStore::~FilePackageBulkStore() {
    // 닫은 패키지의 핸들이 캐시에 남아 디렉터리 이동·삭제를 막지 않게 닫는다.
    if (!path_.IsEmpty()) {
        cache_->Close(path_.View());
    }
}

Result<void> FilePackageBulkStore::Attach(StringView path, const PackageFingerprint& fingerprint,
                                          AssetId asset, uint64 bulkSectionOffset,
                                          Span<const PackageBulkEntry> entries) noexcept {
    if (path.IsEmpty() || IsAttached() || table_.Count() != 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 실패하면 호출 전 slot까지만 남긴다(Detach 뒤의 slot은 이미 준 span의 raw를 갖는다 — 지우지
    // 않는다).
    const usize base = slots_.Size();
    if (auto reserved = slots_.TryReserve(base + entries.Size()); !reserved) {
        return reserved;
    }

    if (auto reserved = table_.Reserve(entries.Size()); !reserved) {
        return reserved;
    }

    if (auto assigned = path_.TryAssign(path); !assigned) {
        return assigned;
    }

    for (const PackageBulkEntry& entry : entries) {
        auto slot = UniquePtr<Slot>::TryMake(*allocator_, *allocator_);
        if (!slot) {
            (void)slots_.TryResize(base);
            table_.Clear();
            path_.Clear();
            return Result<void>::Failure(slot.Error());
        }

        slot.Value()->entry = entry;
        slot.Value()->fileId = entry.bulkId;
        slot.Value()->fileOffset = bulkSectionOffset + entry.offset;
        slot.Value()->rawBytes = entry.rawBytes;
        slot.Value()->sha256 = entry.sha256;
        (void)table_.Insert(entry.bulkId, slots_.Size());
        (void)slots_.TryPushBack(Move(slot.Value()));
    }

    fingerprint_ = fingerprint;
    asset_ = asset;
    ++revision_;
    return Result<void>::Success();
}

Result<FilePackageBulkStore::Slot*>
FilePackageBulkStore::FindSlot(const BulkDataRef& ref) noexcept {
    if (ref.IsEmpty()) {
        return Result<Slot*>::Failure(ErrorCode::InvalidArgument);
    }

    const usize* index = table_.Find(ref.bulkId);
    if (index == nullptr) {
        return Result<Slot*>::Failure(ErrorCode::NotFound);
    }

    Slot& slot = *slots_[*index];
    if (slot.rawBytes != ref.rawBytes || !(slot.sha256 == ref.sha256)) {
        return Result<Slot*>::Failure(ErrorCode::TypeMismatch);
    }

    return Result<Slot*>::Success(&slot);
}

Result<void> FilePackageBulkStore::Load(Slot& slot, AssetDiagnostic* failure) noexcept {
    auto reader = cache_->Acquire(path_.View(), fingerprint_, failure);
    if (!reader) {
        if (failure != nullptr) {
            failure->asset = asset_;
            failure->section = BulkSection;
            failure->detail = slot.fileId;
        }

        return Result<void>::Failure(reader.Error());
    }

    Array<uint8> stored(*allocator_);
    if (auto sized = stored.TryResize(static_cast<usize>(slot.entry.storedBytes)); !sized) {
        return Fail(failure, AssetDiagnosticCode::None, sized.Error(), asset_, BulkSection,
                    slot.fileId);
    }

    if (auto read = reader.Value()->ReadAt(slot.fileOffset, stored.View()); !read) {
        return Fail(failure, AssetDiagnosticCode::None, read.Error(), asset_, BulkSection,
                    slot.fileId);
    }

    if (Crc32(stored.View()) != slot.entry.crc32) {
        return Fail(failure, AssetDiagnosticCode::CorruptPackage, ErrorCode::InvalidArgument,
                    asset_, BulkSection, slot.fileId);
    }

    // raw를 잡기 전에 Zstd 컨테이너 헤더의 rawSize를 대조한다(손상된 표로 큰 할당을 하지 않게).
    Array<uint8> raw(*allocator_);
    if (auto decoded = DecodePackageBulkEntry(slot.entry, stored.View(), raw, *allocator_);
        !decoded) {
        return decoded.Error() == ErrorCode::OutOfMemory
                   ? Fail(failure, AssetDiagnosticCode::None, decoded.Error(), asset_, BulkSection,
                          slot.fileId)
                   : Fail(failure, AssetDiagnosticCode::CorruptPackage, ErrorCode::InvalidArgument,
                          asset_, BulkSection, slot.fileId);
    }

    slot.raw = Move(raw);
    slot.loaded = true;
    return Result<void>::Success();
}

Result<Span<const uint8>> FilePackageBulkStore::Read(const BulkDataRef& ref,
                                                     AssetDiagnostic* failure) noexcept {
    using Out = Span<const uint8>;
    auto slot = FindSlot(ref);
    if (!slot) {
        return Result<Out>::Failure(slot.Error());
    }

    if (!slot.Value()->loaded) {
        if (auto loaded = Load(*slot.Value(), failure); !loaded) {
            return Result<Out>::Failure(loaded.Error());
        }
    }

    return Result<Out>::Success(slot.Value()->raw.View());
}

Result<Span<const uint8>> FilePackageBulkStore::ReadBulk(const BulkDataRef& ref) noexcept {
    return Read(ref, nullptr);
}

Result<BulkDataRef> FilePackageBulkStore::AddUnsaved(Array<uint8>&& raw) noexcept {
    Array<uint8> bytes = Move(raw);
    if (nextUnsaved_ >= UnsavedBulkIdBit) {
        return Result<BulkDataRef>::Failure(ErrorCode::CapacityExceeded);
    }

    auto slot = UniquePtr<Slot>::TryMake(*allocator_, *allocator_);
    if (!slot) {
        return Result<BulkDataRef>::Failure(slot.Error());
    }

    if (auto reserved = slots_.TryReserve(slots_.Size() + 1); !reserved) {
        return Result<BulkDataRef>::Failure(reserved.Error());
    }

    BulkDataRef ref;
    ref.bulkId = UnsavedBulkIdBit | nextUnsaved_;
    ref.rawBytes = bytes.Size();
    ref.sha256 = Sha256::Hash(bytes.View());
    if (auto inserted = table_.Insert(ref.bulkId, slots_.Size()); !inserted) {
        return Result<BulkDataRef>::Failure(inserted.Error());
    }

    slot.Value()->rawBytes = ref.rawBytes;
    slot.Value()->sha256 = ref.sha256;
    slot.Value()->loaded = true;
    slot.Value()->raw = Move(bytes);
    (void)slots_.TryPushBack(Move(slot.Value()));
    ++nextUnsaved_;
    ++revision_;
    return Result<BulkDataRef>::Success(ref);
}

Result<BulkDataRef> FilePackageBulkStore::CopyFrom(PackageBulkStore& source, const BulkDataRef& ref,
                                                   AssetDiagnostic* failure) noexcept {
    auto bytes = source.Read(ref, failure);
    if (!bytes) {
        return Result<BulkDataRef>::Failure(bytes.Error());
    }

    Array<uint8> copy(*allocator_);
    if (auto appended = copy.TryAppend(bytes.Value()); !appended) {
        return Result<BulkDataRef>::Failure(appended.Error());
    }

    return AddUnsaved(Move(copy));
}

Result<void> FilePackageBulkStore::Rebind(StringView path, const PackageFingerprint& fingerprint,
                                          uint64 bulkSectionOffset,
                                          Span<const PackageBulkEntry> entries,
                                          Span<const BulkRemapEntry> remap) noexcept {
    BulkRebindPlan plan(*allocator_);
    if (auto prepared = PrepareRebind(plan, path, fingerprint, bulkSectionOffset, entries, remap);
        !prepared) {
        return prepared;
    }

    CommitRebind(plan);
    return Result<void>::Success();
}

Result<void> FilePackageBulkStore::PrepareRebind(BulkRebindPlan& plan, StringView path,
                                                 const PackageFingerprint& fingerprint,
                                                 uint64 bulkSectionOffset,
                                                 Span<const PackageBulkEntry> entries,
                                                 Span<const BulkRemapEntry> remap) const noexcept {
    plan.ready_ = false;
    plan.table_.Clear();
    plan.updates_.Clear();
    plan.kept_.Clear();
    if (path.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 먼저 모두 검사하고 새 표·갱신 목록을 만든다(저장소는 바꾸지 않는다).
    if (auto reserved = plan.table_.Reserve(remap.Size()); !reserved) {
        return reserved;
    }

    if (auto reserved = plan.updates_.TryReserve(remap.Size()); !reserved) {
        return reserved;
    }

    for (const BulkRemapEntry& row : remap) {
        const usize* index = table_.Find(row.oldId);
        if (index == nullptr || row.newId == 0 || row.newId > entries.Size()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const PackageBulkEntry& entry = entries[row.newId - 1];
        const Slot& slot = *slots_[*index];
        if (entry.bulkId != row.newId || entry.rawBytes != slot.rawBytes ||
            !(entry.sha256 == slot.sha256)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 자리를 미리 잡았다(Reserve) — 같은 옛 id 두 번은 앞 항목이 남는다(writer는 유일하게
        // 준다).
        (void)plan.table_.Insert(row.oldId, *index);
        (void)plan.updates_.TryPushBack(
            BulkRebindPlan::Update{*index, entry, row.newId, bulkSectionOffset + entry.offset});
    }

    if (auto assigned = plan.path_.TryAssign(path); !assigned) {
        return assigned;
    }

    if (auto sized = plan.kept_.TryResize(slots_.Size()); !sized) {
        return sized;
    }

    for (usize index = 0; index < plan.kept_.Size(); ++index) {
        plan.kept_[index] = false;
    }

    plan.table_.ForEach(
        [&](const uint32&, const usize& index) noexcept { plan.kept_[index] = true; });
    plan.fingerprint_ = fingerprint;
    plan.revision_ = revision_;
    plan.ready_ = true;
    return Result<void>::Success();
}

void FilePackageBulkStore::CommitRebind(BulkRebindPlan& plan) noexcept {
    ROCKS_ASSERT(plan.ready_ && plan.revision_ == revision_);
    for (const BulkRebindPlan::Update& update : plan.updates_) {
        Slot& slot = *slots_[update.slot];
        slot.entry = update.entry;
        slot.fileId = update.fileId;
        slot.fileOffset = update.fileOffset;
    }

    // 표에서 빠진 id의 slot은 파일 연결만 끊는다(이미 푼 raw는 남는다).
    for (usize index = 0; index < plan.kept_.Size(); ++index) {
        if (!plan.kept_[index]) {
            slots_[index]->fileId = 0;
        }
    }

    if (!path_.IsEmpty()) {
        cache_->Close(path_.View());
    }

    table_ = Move(plan.table_);
    path_ = Move(plan.path_);
    fingerprint_ = plan.fingerprint_;
    ++revision_;
    plan.ready_ = false;
}

Result<void> FilePackageBulkStore::Detach(AssetDiagnostic* failure) noexcept {
    for (UniquePtr<Slot>& slot : slots_) {
        if (!slot->loaded && slot->fileId != 0) {
            if (auto loaded = Load(*slot, failure); !loaded) {
                return loaded;
            }
        }
    }

    if (!path_.IsEmpty()) {
        cache_->Close(path_.View());
    }

    path_.Clear();
    ++revision_;
    return Result<void>::Success();
}

Result<uint32> FilePackageBulkStore::FileBulkId(const BulkDataRef& ref) const noexcept {
    const usize* index = table_.Find(ref.bulkId);
    if (index == nullptr) {
        return Result<uint32>::Failure(ErrorCode::NotFound);
    }

    return Result<uint32>::Success(slots_[*index]->fileId);
}

} // namespace rocks
