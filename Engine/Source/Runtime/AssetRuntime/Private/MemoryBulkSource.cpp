#include <AssetRuntime/MemoryBulkSource.h>

#include <Core/Assert.h>

namespace rocks {

Result<BulkDataRef> MemoryBulkSource::Add(Array<uint8>&& bytes) noexcept {
    // bulkId는 1부터이고 0은 빈 ref라 entry 수는 uint32 최댓값 − 1까지다.
    if (entries_.Size() >= NumericLimits<uint32>::Max() - 1) {
        return Result<BulkDataRef>::Failure(ErrorCode::CapacityExceeded);
    }

    // 자리를 먼저 잡아 두면 아래 이동이 실패하지 않는다(실패해도 bytes가 호출자에게 남는다).
    if (auto reserved = entries_.TryEnsureCapacity(entries_.Size() + 1); !reserved) {
        return Result<BulkDataRef>::Failure(reserved.Error());
    }

    BulkDataRef ref;
    ref.bulkId = static_cast<uint32>(entries_.Size() + 1);
    ref.rawBytes = bytes.Size();
    ref.sha256 = Sha256::Hash(bytes.View());
    auto pushed = entries_.TryPushBack(Move(bytes));
    ROCKS_ASSERT(pushed.IsOk());
    return Result<BulkDataRef>::Success(ref);
}

Result<BulkDataRef> MemoryBulkSource::AddCopy(Span<const uint8> bytes) noexcept {
    Array<uint8> copy(*allocator_);
    if (auto appended = copy.TryAppend(bytes); !appended) {
        return Result<BulkDataRef>::Failure(appended.Error());
    }

    return Add(Move(copy));
}

Result<Span<const uint8>> MemoryBulkSource::ReadBulk(const BulkDataRef& ref) noexcept {
    if (ref.bulkId == 0 || ref.bulkId > entries_.Size()) {
        return Result<Span<const uint8>>::Failure(ErrorCode::NotFound);
    }

    return Result<Span<const uint8>>::Success(entries_[ref.bulkId - 1].View());
}

} // namespace rocks
