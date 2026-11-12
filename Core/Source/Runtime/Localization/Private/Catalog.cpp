#include "LocalizationInternal.h"
#include <Base/Checked.h>

namespace rocks::detail {

Result<void> RetainCatalog(CatalogData* data) noexcept {
    platform::LockGuard lock(data->gate);
    if (data->references == ~uint64(0)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }
    ++data->references;
    return Result<void>::Success();
}

void ReleaseCatalog(CatalogData* data) noexcept {
    if (!data) {
        return;
    }
    bool last = false;
    {
        platform::LockGuard lock(data->gate);
        if (data->references == 0) {
            platform::Fatal("Catalog reference underflow");
        }
        last = --data->references == 0;
    }
    if (last) {
        Allocator* allocator = data->allocator;
        DestroyAt(data);
        if (!allocator->Deallocate(data)) {
            platform::Fatal("Catalog allocation ownership mismatch");
        }
    }
}

} // namespace rocks::detail

namespace rocks {

CatalogSnapshot::CatalogSnapshot(CatalogSnapshot&& other) noexcept : data_(other.data_) {
    other.data_ = nullptr;
}

CatalogSnapshot& CatalogSnapshot::operator=(CatalogSnapshot&& other) noexcept {
    if (this != &other) {
        detail::CatalogData* previous = data_;
        data_ = other.data_;
        other.data_ = nullptr;
        detail::ReleaseCatalog(previous);
    }
    return *this;
}

CatalogSnapshot::~CatalogSnapshot() noexcept {
    Reset();
}

void CatalogSnapshot::Reset() noexcept {
    detail::CatalogData* previous = data_;
    data_ = nullptr;
    detail::ReleaseCatalog(previous);
}

bool CatalogSnapshot::IsValid() const noexcept {
    return data_ != nullptr;
}

StringView CatalogSnapshot::Culture() const noexcept {
    return data_ ? data_->culture.View() : StringView{};
}

uint64 CatalogSnapshot::Revision() const noexcept {
    return data_ ? data_->revision : 0;
}

usize CatalogSnapshot::EntryCount() const noexcept {
    return data_ ? data_->entries.Size() : 0;
}

Result<CatalogSnapshot> CatalogSnapshot::Clone() const noexcept {
    if (!data_) {
        return Result<CatalogSnapshot>::Failure(ErrorCode::InvalidArgument);
    }
    auto retained = detail::RetainCatalog(data_);
    if (!retained) {
        return Result<CatalogSnapshot>::Failure(retained.Error());
    }
    return Result<CatalogSnapshot>::Success(CatalogSnapshot(data_));
}

Result<CatalogSnapshot> CatalogSnapshot::Create(Allocator& allocator, StringView culture,
                                                uint64 revision, Span<const CatalogEntry> entries,
                                                CatalogLimits limits) noexcept {
    if (culture.IsEmpty() || revision == 0 || limits.maxEntries > 4096 ||
        limits.maxTotalBytes > 16 * 1024 * 1024) {
        return Result<CatalogSnapshot>::Failure(ErrorCode::InvalidArgument);
    }
    if (entries.Size() > limits.maxEntries || culture.ByteCount() > limits.maxTotalBytes) {
        return Result<CatalogSnapshot>::Failure(ErrorCode::CapacityExceeded);
    }
    auto validCulture = culture.ValidateUtf8();
    if (!validCulture) {
        return Result<CatalogSnapshot>::Failure(validCulture.Error());
    }
    usize bytes = culture.ByteCount();
    for (usize index = 0; index < entries.Size(); ++index) {
        if (entries[index].id == TextId{} || entries[index].sourceRevision == 0) {
            return Result<CatalogSnapshot>::Failure(ErrorCode::InvalidArgument);
        }
        if (!TryAdd(bytes, entries[index].translation.ByteCount(), bytes)) {
            return Result<CatalogSnapshot>::Failure(ErrorCode::Overflow);
        }
        if (bytes > limits.maxTotalBytes) {
            return Result<CatalogSnapshot>::Failure(ErrorCode::CapacityExceeded);
        }
        auto valid = entries[index].translation.ValidateUtf8();
        if (!valid) {
            return Result<CatalogSnapshot>::Failure(valid.Error());
        }
        for (usize prior = 0; prior < index; ++prior) {
            if (entries[prior].id == entries[index].id) {
                return Result<CatalogSnapshot>::Failure(ErrorCode::AlreadyExists);
            }
        }
    }
    auto memory = allocator.Allocate(sizeof(detail::CatalogData), alignof(detail::CatalogData));
    if (!memory) {
        return Result<CatalogSnapshot>::Failure(memory.Error());
    }
    if (!memory.Value()) {
        platform::Fatal("Allocator returned null Catalog data");
    }
    CatalogSnapshot owned(ConstructAt<detail::CatalogData>(memory.Value(), allocator));
    owned.data_->revision = revision;
    auto copied = owned.data_->culture.TryAssign(culture);
    if (!copied) {
        return Result<CatalogSnapshot>::Failure(copied.Error());
    }
    auto reserved = owned.data_->entries.TryReserve(entries.Size());
    if (!reserved) {
        return Result<CatalogSnapshot>::Failure(reserved.Error());
    }
    for (usize index = 0; index < entries.Size(); ++index) {
        auto added = owned.data_->entries.TryEmplaceBack(allocator);
        if (!added) {
            return Result<CatalogSnapshot>::Failure(added.Error());
        }
        added.Value()->id = entries[index].id;
        added.Value()->sourceRevision = entries[index].sourceRevision;
        auto translated = added.Value()->translation.TryAssign(entries[index].translation);
        if (!translated) {
            return Result<CatalogSnapshot>::Failure(translated.Error());
        }
    }
    return Result<CatalogSnapshot>::Success(Move(owned));
}

CatalogStore::~CatalogStore() noexcept {
    Close();
}

Result<void> CatalogStore::Publish(const CatalogSnapshot& snapshot) noexcept {
    {
        platform::LockGuard lock(gate_);
        if (closed_) {
            return Result<void>::Failure(ErrorCode::ShuttingDown);
        }
    }
    auto clone = snapshot.Clone();
    if (!clone) {
        return Result<void>::Failure(clone.Error());
    }
    CatalogSnapshot previous;
    {
        platform::LockGuard lock(gate_);
        if (closed_) {
            return Result<void>::Failure(ErrorCode::ShuttingDown);
        }
        previous = Move(current_);
        current_ = Move(clone.Value());
    }
    // 이전 snapshot의 마지막 해제에서 allocator가 재진입해도 게시된 새 상태는 덮어쓰지 않는다.
    return Result<void>::Success();
}

Result<CatalogSnapshot> CatalogStore::Acquire() const noexcept {
    platform::LockGuard lock(gate_);
    if (closed_) {
        return Result<CatalogSnapshot>::Failure(ErrorCode::ShuttingDown);
    }
    if (!current_.IsValid()) {
        return Result<CatalogSnapshot>::Failure(ErrorCode::NotFound);
    }
    // Clone은 계수 gate만 사용하고 할당/해제 콜백을 실행하지 않는다.
    return current_.Clone();
}

void CatalogStore::Close() noexcept {
    CatalogSnapshot previous;
    {
        platform::LockGuard lock(gate_);
        closed_ = true;
        previous = Move(current_);
    }
}

} // namespace rocks
