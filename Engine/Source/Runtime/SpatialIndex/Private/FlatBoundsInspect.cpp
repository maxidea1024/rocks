#include "FlatBoundsInternal.h"

#include <Core/Math/MathUtility.h>
#include <Platform/PlatformMath.h>

namespace rocks::spatial {

namespace {

bool SameBox(const Box<double>& left, const Box<double>& right) noexcept {
    return left.min.x == right.min.x && left.min.y == right.min.y && left.min.z == right.min.z &&
           left.max.x == right.max.x && left.max.y == right.max.y && left.max.z == right.max.z;
}

bool Limited(float minimum, float maximum) noexcept {
    return minimum > 0.0f || maximum > 0.0f;
}

} // namespace

FlatBoundsStats FlatBoundsSet::CollectStats() const noexcept {
    FlatBoundsStats stats;
    stats.elementCount = static_cast<uint32>(records_.Size());
    stats.largeCount = largeCount_;
    stats.outOfRangeCount = static_cast<uint32>(list_.Size()) - largeCount_;
    stats.pageElementCount = stats.elementCount - static_cast<uint32>(list_.Size());
    stats.tileCount = static_cast<uint32>(tiles_.Count());
    stats.livePageCount = pages_.LiveCount();
    stats.freePageCount = pages_.FreeCount();
    stats.pageSlotCount = pages_.SlotCount();
    stats.wordCount = WordCount();
    double radiusSum = 0.0;
    for (uint32 page = 0; page < pages_.SlotCount(); ++page) {
        if (!pages_.IsLive(page)) {
            continue;
        }
        const detail::FlatPageHeader& header = pages_[page];
        stats.maxPageRadius = math::Max(stats.maxPageRadius, header.radius);
        radiusSum += static_cast<double>(header.radius);
        if (header.previous == detail::FlatNoPage) {
            const detail::FlatTile* tile = tiles_.Find(header.key);
            if (tile != nullptr) {
                stats.maxPagesPerTile = tile->pageCount > stats.maxPagesPerTile
                                            ? tile->pageCount
                                            : stats.maxPagesPerTile;
            }
        }
    }
    stats.meanPageRadius =
        stats.livePageCount == 0 ? 0.0 : radiusSum / static_cast<double>(stats.livePageCount);

    stats.recordBytesUsed = records_.Size() * sizeof(detail::FlatRecord);
    stats.recordBytesReserved = records_.Capacity() * sizeof(detail::FlatRecord);
    // 원소당 page 데이터: 경계 24, 거리 8, 필터 4, record 4, 사용자 값 8 byte + 항상 보임 1 bit.
    constexpr usize elementBytes =
        6 * sizeof(float) + 2 * sizeof(float) + 2 * sizeof(uint32) + sizeof(uint64);
    const usize pageElements = static_cast<usize>(pages_.SlotCount()) * config_.pageCapacity;
    stats.pageBytesUsed = pageElements * elementBytes + pageElements / 8 +
                          static_cast<usize>(pages_.SlotCount()) * sizeof(detail::FlatPageHeader);
    stats.pageBytesReserved = minX_.Capacity() * elementBytes +
                              always_.Capacity() * sizeof(uint64) +
                              pages_.Capacity() * sizeof(detail::FlatPageHeader);
    stats.tileMapBytesReserved =
        tiles_.SlotCount() *
        (sizeof(detail::FlatTileKey) + sizeof(detail::FlatTile) + sizeof(uint64) + 1);
    stats.listBytesUsed = list_.Size() * sizeof(detail::FlatListEntry);
    stats.listBytesReserved = list_.Capacity() * sizeof(detail::FlatListEntry);

    stats.inPlaceUpdates = counters_.inPlaceUpdates;
    stats.relocations = counters_.relocations;
    stats.listInsertions = counters_.listInsertions;
    stats.pageAllocations = counters_.pageAllocations;
    stats.pageReleases = counters_.pageReleases;
    return stats;
}

Result<FlatBoundsValidationReport> FlatBoundsSet::ValidateStructure() const noexcept {
    FlatBoundsValidationReport report;
    const auto fail = [&](uint32 invariant, uint32 page) noexcept {
        report.invariant = invariant;
        report.page = page;
        return Result<FlatBoundsValidationReport>::Success(report);
    };
    const uint32 capacity = config_.pageCapacity;

    // ---- 타일·page 구조 ----
    // 타일마다 첫 page에서 연결을 따라가며 확인한다(HashMap 순회는 const여도 내부 계수를 바꾸므로
    // 읽기 단계의 동시 Validate를 위해 쓰지 않는다).
    uint64 pageElements = 0;
    uint32 walkedPages = 0;
    uint32 walkedTiles = 0;
    for (uint32 page = 0; page < pages_.SlotCount(); ++page) {
        if (!pages_.IsLive(page)) {
            continue;
        }
        const detail::FlatPageHeader& header = pages_[page];
        if (header.count == 0 || header.count > capacity) {
            return fail(FlatInvariantPageStructure, page);
        }
        const detail::FlatTile* tile = tiles_.Find(header.key);
        if (tile == nullptr) {
            return fail(FlatInvariantPageStructure, page);
        }
        if (header.previous != detail::FlatNoPage) {
            continue;
        }
        if (tile->firstPage != page) {
            return fail(FlatInvariantPageStructure, page);
        }
        ++walkedTiles;
        uint32 previous = detail::FlatNoPage;
        uint32 count = 0;
        for (uint32 current = page; current != detail::FlatNoPage; current = pages_[current].next) {
            if (!pages_.IsLive(current) || count > pages_.LiveCount()) {
                return fail(FlatInvariantPageStructure, current);
            }
            const detail::FlatPageHeader& link = pages_[current];
            if (link.previous != previous || !(link.key == header.key)) {
                return fail(FlatInvariantPageStructure, current);
            }
            // 마지막 page만 덜 찰 수 있다.
            if (link.next != detail::FlatNoPage && link.count != capacity) {
                return fail(FlatInvariantPageStructure, current);
            }
            uint32 limited = 0;
            for (uint32 position = 0; position < link.count; ++position) {
                const uint32 slot = current * capacity + position;
                limited += Limited(drawMin_[slot],
                                   drawMax_[slot] == platform::PlatformMath::Infinity<float>()
                                       ? 0.0f
                                       : drawMax_[slot])
                               ? 1u
                               : 0u;
            }
            if (limited != link.distanceLimited) {
                return fail(FlatInvariantAttributes, current);
            }
            pageElements += link.count;
            previous = current;
            ++count;
        }
        if (count != tile->pageCount || previous != tile->lastPage) {
            return fail(FlatInvariantPageStructure, page);
        }
        walkedPages += count;
    }
    if (walkedPages != pages_.LiveCount() || walkedTiles != tiles_.Count()) {
        return fail(FlatInvariantPageStructure, FlatBoundsNoPage);
    }
    if (pageElements + list_.Size() != records_.Size()) {
        return fail(FlatInvariantCount, FlatBoundsNoPage);
    }

    // ---- 목록 ----
    uint32 large = 0;
    for (uint32 index = 0; index < list_.Size(); ++index) {
        const detail::FlatListEntry& entry = list_[index];
        const detail::FlatRecord* record = records_.FindAt(entry.record);
        if (record == nullptr || record->page != detail::FlatNoPage || record->position != index) {
            return fail(FlatInvariantRecordLink, FlatBoundsNoPage);
        }
        large += entry.large ? 1u : 0u;
    }
    if (large != largeCount_) {
        return fail(FlatInvariantListPlacement, FlatBoundsNoPage);
    }

    // ---- 레코드 ----
    for (uint32 index = 0; index < records_.SlotCount(); ++index) {
        const detail::FlatRecord* found = records_.FindAt(index);
        if (found == nullptr) {
            continue;
        }
        const detail::FlatRecord& record = *found;
        const Target target = TargetOf(record.bounds);
        if (record.page == detail::FlatNoPage) {
            if (record.position >= list_.Size() || list_[record.position].record != index) {
                return fail(FlatInvariantRecordLink, FlatBoundsNoPage);
            }
            const detail::FlatListEntry& entry = list_[record.position];
            if (!SameBox(entry.bounds, record.bounds) || entry.filter != record.filter ||
                entry.minDraw != record.minDraw || entry.maxDraw != record.maxDraw ||
                entry.alwaysVisible != record.alwaysVisible ||
                entry.userValue != record.userValue || entry.large != record.large) {
                return fail(FlatInvariantAttributes, FlatBoundsNoPage);
            }
            if (!target.list || target.large != record.large) {
                return fail(FlatInvariantListPlacement, FlatBoundsNoPage);
            }
            continue;
        }
        const uint32 page = record.page;
        if (!pages_.IsLive(page) || record.position >= pages_[page].count) {
            return fail(FlatInvariantRecordLink, page);
        }
        const uint32 slot = page * capacity + record.position;
        if (record_[slot] != index) {
            return fail(FlatInvariantRecordLink, page);
        }
        const bool always =
            ((always_[slot / FlatVisibilityBitsPerWord] >> (slot % FlatVisibilityBitsPerWord)) &
             1u) != 0;
        const float storedMax =
            record.maxDraw == 0.0f ? platform::PlatformMath::Infinity<float>() : record.maxDraw;
        if (filter_[slot] != record.filter || userValue_[slot] != record.userValue ||
            drawMin_[slot] != record.minDraw || drawMax_[slot] != storedMax ||
            always != record.alwaysVisible) {
            return fail(FlatInvariantAttributes, page);
        }
        if (target.list) {
            return fail(FlatInvariantListPlacement, page);
        }
        const detail::FlatPageHeader& header = pages_[page];
        if (!(target.key == header.key)) {
            return fail(FlatInvariantStoredBounds, page);
        }
        // 저장 float 경계가 정확한 상대 경계를 바깥에서 엄격히 포함한다: min_f < (min − o), max_f >
        // (max − o)(double 계산값 기준, LooseOctree 불변 조건 7과 같다). double 뺄셈은 최근접
        // 반올림이라 등호를 허용하면 한 ULP 바깥 보정이 빠진 저장 값도 통과하므로 엄격 부등식으로
        // 본다(보정이 있으면 저장 값은 반올림 값보다 float 한 ULP 이상 바깥이다). DAZ 환경에서도
        // 거짓 보고가 없도록 정수 키로 비교한다(FloatBelow·FloatAbove). 그리고 R 안에 있다.
        const Vec3d lower = record.bounds.min - header.origin;
        const Vec3d upper = record.bounds.max - header.origin;
        const float stored[6] = {minX_[slot], minY_[slot], minZ_[slot],
                                 maxX_[slot], maxY_[slot], maxZ_[slot]};
        if (!FloatBelow(stored[0], lower.x) || !FloatBelow(stored[1], lower.y) ||
            !FloatBelow(stored[2], lower.z) || !FloatAbove(stored[3], upper.x) ||
            !FloatAbove(stored[4], upper.y) || !FloatAbove(stored[5], upper.z)) {
            return fail(FlatInvariantStoredBounds, page);
        }
        for (const float value : stored) {
            if (platform::BitCast<uint32>(MaxMagnitude(header.radius, value)) !=
                platform::BitCast<uint32>(header.radius)) {
                return fail(FlatInvariantStoredBounds, page);
            }
        }
    }
    return Result<FlatBoundsValidationReport>::Success(report);
}

Result<FlatBoundsDesc> FlatBoundsSet::TryGetElement(FlatBoundsHandle handle) const noexcept {
    auto checked = CheckHandle(handle);
    if (!checked) {
        return Result<FlatBoundsDesc>::Failure(checked.Error());
    }
    const detail::FlatRecord& record = records_.ElementAt(handle.slot.index);
    FlatBoundsDesc desc;
    desc.bounds = record.bounds;
    desc.filterMask = record.filter;
    desc.minDrawDistance = record.minDraw;
    desc.maxDrawDistance = record.maxDraw;
    desc.alwaysVisible = record.alwaysVisible;
    desc.userValue = record.userValue;
    return Result<FlatBoundsDesc>::Success(desc);
}

Result<FlatBoundsPlacement> FlatBoundsSet::TryPlacement(FlatBoundsHandle handle) const noexcept {
    auto checked = CheckHandle(handle);
    if (!checked) {
        return Result<FlatBoundsPlacement>::Failure(checked.Error());
    }
    const detail::FlatRecord& record = records_.ElementAt(handle.slot.index);
    FlatBoundsPlacement placement;
    placement.slot = SlotOfRecord(handle.slot.index);
    placement.inList = record.page == detail::FlatNoPage;
    if (placement.inList) {
        placement.large = record.large;
    } else {
        placement.tileOrigin = pages_[record.page].origin;
        placement.pageRadius = pages_[record.page].radius;
    }
    return Result<FlatBoundsPlacement>::Success(placement);
}

Result<uint64> FlatBoundsSet::TryUserValueAtSlot(uint32 slot) const noexcept {
    if (IsFlatBoundsListSlot(slot)) {
        const uint32 index = slot - FlatBoundsListSlotBase;
        if (index >= list_.Size()) {
            return Result<uint64>::Failure(ErrorCode::NotFound);
        }
        return Result<uint64>::Success(list_[index].userValue);
    }
    const uint32 page = slot / config_.pageCapacity;
    const uint32 position = slot % config_.pageCapacity;
    if (page >= pages_.SlotCount() || !pages_.IsLive(page) || position >= pages_[page].count) {
        return Result<uint64>::Failure(ErrorCode::NotFound);
    }
    return Result<uint64>::Success(userValue_[slot]);
}

} // namespace rocks::spatial
