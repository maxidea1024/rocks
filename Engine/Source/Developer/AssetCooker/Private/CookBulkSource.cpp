#include <AssetCooker/CookBulkSource.h>
#include <AssetRuntime/BulkStore.h>
#include <Core/Encoding/Sha256.h>

// 쿠킹 처리기의 연쇄 bulk 원본이다(Docs/Design/AssetSystem 12.14절 9번).
namespace rocks {

Result<BulkDataRef> CookChainedBulkSource::Add(Span<const uint8> bytes,
                                               PackageBulkPlacement placement) noexcept {
    // 내려가는 id가 저장하지 않은 bulk의 최상위 bit 범위로 내려오지 않게 한다.
    if (entries_.Size() >= CookBulkIdFirst - UnsavedBulkIdBit) {
        return Result<BulkDataRef>::Failure(ErrorCode::CapacityExceeded);
    }

    if (auto pushed = entries_.TryPushBack(Entry{bytes, placement}); !pushed) {
        return Result<BulkDataRef>::Failure(pushed.Error());
    }

    BulkDataRef ref;
    ref.bulkId = CookBulkIdFirst - static_cast<uint32>(entries_.Size() - 1);
    ref.rawBytes = bytes.Size();
    ref.sha256 = Sha256::Hash(bytes);
    return Result<BulkDataRef>::Success(ref);
}

Result<Span<const uint8>> CookChainedBulkSource::ReadBulk(const BulkDataRef& ref) noexcept {
    if (const Entry* own = Find(ref.bulkId)) {
        return Result<Span<const uint8>>::Success(own->bytes);
    }

    if (next_ == nullptr) {
        return Result<Span<const uint8>>::Failure(ErrorCode::NotFound);
    }

    return next_->ReadBulk(ref);
}

Result<PackageStoredBulk> CookChainedBulkSource::ReadStoredBulk(const BulkDataRef& ref) noexcept {
    if (Find(ref.bulkId) != nullptr || next_ == nullptr) {
        return Result<PackageStoredBulk>::Failure(ErrorCode::NotFound);
    }

    return next_->ReadStoredBulk(ref);
}

PackageBulkPlacement CookChainedBulkSource::Placement(const BulkDataRef& ref) const noexcept {
    if (const Entry* own = Find(ref.bulkId)) {
        return own->placement;
    }

    return next_ != nullptr ? next_->Placement(ref) : PackageBulkPlacement::Package;
}

const CookChainedBulkSource::Entry* CookChainedBulkSource::Find(uint32 bulkId) const noexcept {
    if (bulkId == 0) {
        return nullptr;
    }

    const uint32 offset = CookBulkIdFirst - bulkId;
    return offset < entries_.Size() ? &entries_[offset] : nullptr;
}

} // namespace rocks
