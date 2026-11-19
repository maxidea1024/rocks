#include "FlatBoundsInternal.h"

#include <Core/Math/MathUtility.h>
#include <Platform/Fatal.h>
#include <Platform/PlatformMath.h>

namespace rocks::spatial {

namespace {

constexpr uint32 NoRecord = NumericLimits<uint32>::Max();

bool IsIntegral(double value) noexcept {
    return math::IsFinite(value) && math::Floor(value) == value;
}

// 그리기 거리 입력 검사다(4절): 유한·0 이상, 상한이 0(무제한)이 아니면 하한 이하.
bool IsValidDrawRange(float minimum, float maximum) noexcept {
    if (!math::IsFinite(minimum) || !math::IsFinite(maximum) || !(minimum >= 0.0f) ||
        !(maximum >= 0.0f)) {
        return false;
    }
    return maximum == 0.0f || minimum <= maximum;
}

bool IsDistanceLimited(float minimum, float maximum) noexcept {
    return minimum > 0.0f || maximum > 0.0f;
}

// 상한 0(무제한)은 page 배열에 +Inf로 둔다. 질의의 상한 판정이 따로 분기하지 않는다.
float StoredDrawMax(float maximum) noexcept {
    return maximum == 0.0f ? platform::PlatformMath::Infinity<float>() : maximum;
}

void Require(bool condition, const char* message) noexcept {
    if (!condition) {
        platform::Fatal(message);
    }
}

} // namespace

// ---- 생성·이동 ----

Result<FlatBoundsSet> FlatBoundsSet::TryCreate(const FlatBoundsConfig& config,
                                               Allocator& allocator) noexcept {
    const auto invalid = []() noexcept {
        return Result<FlatBoundsSet>::Failure(ErrorCode::InvalidArgument);
    };
    if (config.pageCapacity < FlatVisibilityBitsPerWord || config.pageCapacity > 65536 ||
        config.pageCapacity % FlatVisibilityBitsPerWord != 0) {
        return invalid();
    }
    // 타일 원점 key × tileSize + anchor가 double로 정확하려면 두 값이 정수이고 합이 2^53 미만이어야
    // 한다(key는 int32).
    if (!IsIntegral(config.tileSize) || config.tileSize <= 0.0) {
        return invalid();
    }
    const double keySpan = config.tileSize * 0x1.0p31;
    for (uint32 axis = 0; axis < 3; ++axis) {
        const double anchor = config.anchor[axis];
        if (!IsIntegral(anchor) || keySpan + math::Abs(anchor) >= 0x1.0p53) {
            return invalid();
        }
    }
    if (!math::IsFinite(config.largeRelativeExtent) || !(config.largeRelativeExtent > 0.0) ||
        config.largeRelativeExtent > 0x1.0p60) {
        return invalid();
    }
    if (config.maxElements == 0 || config.maxElements > SlotArray<detail::FlatRecord>::MaxSlots ||
        config.maxGeneration == 0) {
        return invalid();
    }
    return Result<FlatBoundsSet>::Success(FlatBoundsSet(config, allocator));
}

FlatBoundsSet::FlatBoundsSet(const FlatBoundsConfig& config, Allocator& allocator) noexcept
    : config_(config), allocator_(&allocator),
      wordsPerPage_(config.pageCapacity / FlatVisibilityBitsPerWord),
      records_(allocator, config.maxGeneration), tiles_(allocator),
      // page 슬롯(page × pageCapacity + 위치)이 목록 슬롯 영역(FlatBoundsListSlotBase 이상)과
      // 겹치지 않도록 page 수를 제한한다.
      pages_(allocator, FlatBoundsListSlotBase / config.pageCapacity), minX_(allocator),
      minY_(allocator), minZ_(allocator), maxX_(allocator), maxY_(allocator), maxZ_(allocator),
      drawMin_(allocator), drawMax_(allocator), filter_(allocator), record_(allocator),
      userValue_(allocator), always_(allocator), list_(allocator) {
    epoch_ = NextMutationEpoch();
}

FlatBoundsSet::FlatBoundsSet(FlatBoundsSet&& other) noexcept
    : config_(other.config_), allocator_(other.allocator_), wordsPerPage_(other.wordsPerPage_),
      records_(*other.allocator_, other.config_.maxGeneration), tiles_(*other.allocator_),
      pages_(*other.allocator_, FlatBoundsListSlotBase / other.config_.pageCapacity),
      minX_(*other.allocator_), minY_(*other.allocator_), minZ_(*other.allocator_),
      maxX_(*other.allocator_), maxY_(*other.allocator_), maxZ_(*other.allocator_),
      drawMin_(*other.allocator_), drawMax_(*other.allocator_), filter_(*other.allocator_),
      record_(*other.allocator_), userValue_(*other.allocator_), always_(*other.allocator_),
      list_(*other.allocator_) {
    other.CheckNoOpenScope("FlatBoundsSet moved while a read scope is open");
    TakeFrom(other);
}

FlatBoundsSet& FlatBoundsSet::operator=(FlatBoundsSet&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    CheckNoOpenScope("FlatBoundsSet overwritten while a read scope is open");
    other.CheckNoOpenScope("FlatBoundsSet moved while a read scope is open");
    config_ = other.config_;
    allocator_ = other.allocator_;
    wordsPerPage_ = other.wordsPerPage_;
    TakeFrom(other);
    return *this;
}

FlatBoundsSet::~FlatBoundsSet() noexcept {
    CheckNoOpenScope("FlatBoundsSet destroyed while a read scope is open");
}

void FlatBoundsSet::TakeFrom(FlatBoundsSet& other) noexcept {
    // 컨테이너 이동 대입은 이 객체의 옛 저장소를 돌려주고 원본 저장소를 넘겨받는다. 원본은 같은
    // 설정의 빈 집합으로 남도록 빈 컨테이너를 다시 묶는다.
    Allocator& source = *other.allocator_;
    records_ = Move(other.records_);
    tiles_ = Move(other.tiles_);
    pages_ = Move(other.pages_);
    minX_ = Move(other.minX_);
    minY_ = Move(other.minY_);
    minZ_ = Move(other.minZ_);
    maxX_ = Move(other.maxX_);
    maxY_ = Move(other.maxY_);
    maxZ_ = Move(other.maxZ_);
    drawMin_ = Move(other.drawMin_);
    drawMax_ = Move(other.drawMax_);
    filter_ = Move(other.filter_);
    record_ = Move(other.record_);
    userValue_ = Move(other.userValue_);
    always_ = Move(other.always_);
    list_ = Move(other.list_);
    largeCount_ = other.largeCount_;
    counters_ = other.counters_;

    other.records_ = SlotArray<detail::FlatRecord>(source, other.config_.maxGeneration);
    other.tiles_ = HashMap<detail::FlatTileKey, detail::FlatTile, detail::FlatTileKeyHash>(source);
    other.pages_ = IndexPool<detail::FlatPageHeader>(source, FlatBoundsListSlotBase /
                                                                 other.config_.pageCapacity);
    other.minX_ = Array<float>(source);
    other.minY_ = Array<float>(source);
    other.minZ_ = Array<float>(source);
    other.maxX_ = Array<float>(source);
    other.maxY_ = Array<float>(source);
    other.maxZ_ = Array<float>(source);
    other.drawMin_ = Array<float>(source);
    other.drawMax_ = Array<float>(source);
    other.filter_ = Array<uint32>(source);
    other.record_ = Array<uint32>(source);
    other.userValue_ = Array<uint64>(source);
    other.always_ = Array<uint64>(source);
    other.list_ = Array<detail::FlatListEntry>(source);
    other.largeCount_ = 0;
    other.counters_ = Counters{};
    // 받은 쪽과 원본 모두 새 세대다. 옮기기 전의 어느 쪽 비트 집합도 받아들이지 않는다.
    epoch_ = NextMutationEpoch();
    other.epoch_ = NextMutationEpoch();
}

void FlatBoundsSet::CheckNoOpenScope(const char* message) const noexcept {
    access_.CheckIdle(message);
}

Result<void> FlatBoundsSet::CheckHandle(FlatBoundsHandle handle) const noexcept {
    if (handle.IsNull()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (!records_.IsValid(handle.slot)) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }
    return Result<void>::Success();
}

// ---- 배치 ----

FlatBoundsSet::Target FlatBoundsSet::TargetOf(const Box<double>& bounds) const noexcept {
    Target target;
    // 중심은 반씩 더해 계산한다(min + max의 overflow를 피한다). key는 floor다(0 방향 절삭 금지,
    // 10.4절 규약 1).
    int32 key[3] = {0, 0, 0};
    double origin[3] = {0.0, 0.0, 0.0};
    for (uint32 axis = 0; axis < 3; ++axis) {
        const double center = bounds.min[axis] * 0.5 + bounds.max[axis] * 0.5;
        const double cell = math::Floor((center - config_.anchor[axis]) / config_.tileSize);
        if (!(cell >= static_cast<double>(NumericLimits<int32>::Min())) ||
            !(cell <= static_cast<double>(NumericLimits<int32>::Max()))) {
            target.list = true;
            target.large = false;
            return target;
        }
        key[axis] = static_cast<int32>(cell);
        origin[axis] = cell * config_.tileSize + config_.anchor[axis];
    }
    target.origin = Vec3d(origin[0], origin[1], origin[2]);
    target.key = detail::FlatTileKey{key[0], key[1], key[2]};
    // 큰 원소 판정(5절): 타일 상대 좌표의 최대 절댓값이 기준을 넘으면 목록에 둔다. 그래서 page의
    // R은 이 값(float 한두 ULP 여유)으로 묶이고 δ가 큰 원소 하나 때문에 page 전체에서 커지지
    // 않는다.
    double extent = 0.0;
    for (uint32 axis = 0; axis < 3; ++axis) {
        extent = math::Max(extent, math::Abs(bounds.min[axis] - target.origin[axis]));
        extent = math::Max(extent, math::Abs(bounds.max[axis] - target.origin[axis]));
    }
    if (extent > config_.largeRelativeExtent) {
        target.list = true;
        target.large = true;
    }
    return target;
}

Result<void> FlatBoundsSet::EnsurePageArrays(uint32 pageSlots) noexcept {
    // page 배열은 page 색인 높이 × pageCapacity로 함께 자란다. 성장은 두 배 규칙이라 page 하나씩
    // 늘어도 재할당은 상환 O(1)이다.
    const usize required = static_cast<usize>(pageSlots) * config_.pageCapacity;
    // 앞선 확보가 일부 배열만 늘리고 실패했을 수 있으므로 모든 배열을 본다.
    const usize capacities[] = {
        minX_.Capacity(),    minY_.Capacity(),      minZ_.Capacity(),
        maxX_.Capacity(),    maxY_.Capacity(),      maxZ_.Capacity(),
        drawMin_.Capacity(), drawMax_.Capacity(),   filter_.Capacity(),
        record_.Capacity(),  userValue_.Capacity(), always_.Capacity() * FlatVisibilityBitsPerWord};
    bool enough = true;
    for (const usize capacity : capacities) {
        enough = enough && capacity >= required;
    }
    if (enough) {
        return Result<void>::Success();
    }
    usize target = minX_.Capacity() * 2;
    target = target < required ? required : target;
    const usize words = target / FlatVisibilityBitsPerWord;
    // 배열마다 확보한다. 실패하면 앞의 배열만 늘어 있을 수 있다(위의 검사가 모든 배열을 다시 본다).
    Result<void> result = minX_.TryReserve(target);
    if (result) {
        result = minY_.TryReserve(target);
    }
    if (result) {
        result = minZ_.TryReserve(target);
    }
    if (result) {
        result = maxX_.TryReserve(target);
    }
    if (result) {
        result = maxY_.TryReserve(target);
    }
    if (result) {
        result = maxZ_.TryReserve(target);
    }
    if (result) {
        result = drawMin_.TryReserve(target);
    }
    if (result) {
        result = drawMax_.TryReserve(target);
    }
    if (result) {
        result = filter_.TryReserve(target);
    }
    if (result) {
        result = record_.TryReserve(target);
    }
    if (result) {
        result = userValue_.TryReserve(target);
    }
    if (result) {
        result = always_.TryReserve(words);
    }
    if (!result) {
        return result;
    }
    return Result<void>::Success();
}

Result<void> FlatBoundsSet::ReserveFor(const Target& target) noexcept {
    if (target.list) {
        return list_.TryReserve(list_.Size() + 1);
    }
    // 타일 등록은 여기서 미리 한다. HashMap 삽입은 예약 뒤에도 삭제 칸 정리로 할당할 수 있으므로
    // Place가 할당하지 않도록 빈 타일 항목을 먼저 넣는다. 뒤의 확보가 실패하면 그 항목을 지운다.
    bool inserted = false;
    detail::FlatTile* tile = tiles_.Find(target.key);
    if (tile == nullptr) {
        auto added = tiles_.FindOrAdd(target.key, detail::FlatTile{});
        if (!added) {
            return Result<void>::Failure(added.Error());
        }
        inserted = true;
        tile = tiles_.Find(target.key);
    }
    const bool needsPage = tile->lastPage == detail::FlatNoPage ||
                           pages_[tile->lastPage].count == config_.pageCapacity;
    if (!needsPage) {
        return Result<void>::Success();
    }
    auto reserved = pages_.TryReserveAdditional(1);
    if (reserved) {
        const uint32 slots = pages_.SlotCount() + (pages_.FreeCount() != 0 ? 0u : 1u);
        reserved = EnsurePageArrays(slots);
    }
    if (!reserved && inserted) {
        (void)tiles_.Erase(target.key);
    }
    return reserved;
}

uint32 FlatBoundsSet::Place(uint32 record, const Target& target) noexcept {
    detail::FlatRecord& data = records_.ElementAt(record);
    if (target.list) {
        detail::FlatListEntry entry;
        entry.bounds = data.bounds;
        entry.filter = data.filter;
        entry.record = record;
        entry.minDraw = data.minDraw;
        entry.maxDraw = data.maxDraw;
        entry.alwaysVisible = data.alwaysVisible;
        entry.large = target.large;
        entry.userValue = data.userValue;
        const uint32 index = static_cast<uint32>(list_.Size());
        Require(static_cast<bool>(list_.TryPushBack(entry)),
                "FlatBoundsSet list grew after reserve");
        data.page = detail::FlatNoPage;
        data.position = index;
        data.large = target.large;
        largeCount_ += target.large ? 1u : 0u;
        ++counters_.listInsertions;
        return FlatBoundsListSlotBase + index;
    }

    detail::FlatTile* tile = tiles_.Find(target.key);
    Require(tile != nullptr, "FlatBoundsSet tile was not registered by ReserveFor");
    uint32 page = tile->lastPage;
    if (page == detail::FlatNoPage || pages_[page].count == config_.pageCapacity) {
        const uint32 previous = page;
        page = pages_.AllocateReserved();
        if (pages_.SlotCount() * config_.pageCapacity > minX_.Size()) {
            const usize size = static_cast<usize>(pages_.SlotCount()) * config_.pageCapacity;
            bool grown = minX_.TryResize(size) && minY_.TryResize(size) && minZ_.TryResize(size) &&
                         maxX_.TryResize(size) && maxY_.TryResize(size) && maxZ_.TryResize(size) &&
                         drawMin_.TryResize(size) && drawMax_.TryResize(size) &&
                         filter_.TryResize(size) && record_.TryResize(size) &&
                         userValue_.TryResize(size) &&
                         always_.TryResize(size / FlatVisibilityBitsPerWord);
            Require(grown, "FlatBoundsSet page arrays grew after reserve");
        }
        detail::FlatPageHeader& header = pages_[page];
        header.key = target.key;
        header.origin = target.origin;
        header.previous = previous;
        if (previous == detail::FlatNoPage) {
            tile->firstPage = page;
        } else {
            pages_[previous].next = page;
        }
        tile->lastPage = page;
        ++tile->pageCount;
        // 새 page의 항상 보임 bit는 0에서 시작한다(해제 전 내용이 남아 있을 수 있다).
        for (uint32 word = 0; word < wordsPerPage_; ++word) {
            always_[page * wordsPerPage_ + word] = 0;
        }
        ++counters_.pageAllocations;
    }
    detail::FlatPageHeader& header = pages_[page];
    const uint32 position = header.count++;
    data.page = page;
    data.position = position;
    data.large = false;
    WritePageElement(page, position, record);
    return page * config_.pageCapacity + position;
}

void FlatBoundsSet::WritePageBounds(uint32 page, uint32 position,
                                    const Box<double>& bounds) noexcept {
    // 타일 원점 o 기준 double 차이를 바깥쪽으로 float 반올림한 뒤 한 ULP 더 바깥쪽으로 옮긴다.
    // double 뺄셈은 최근접 반올림이라 외향이 아니므로 이 보정이 "저장 경계 ⊇ B − o"를 문자 그대로
    // 성립시킨다 (5절, LooseOctree 6.3절과 같다).
    detail::FlatPageHeader& header = pages_[page];
    const Vec3d& origin = header.origin;
    const Box<float> relative =
        Box<double>(bounds.min - origin, bounds.max - origin).ToFloatOutward();
    const uint32 slot = page * config_.pageCapacity + position;
    minX_[slot] = math::NextDown(relative.min.x);
    minY_[slot] = math::NextDown(relative.min.y);
    minZ_[slot] = math::NextDown(relative.min.z);
    maxX_[slot] = math::NextUp(relative.max.x);
    maxY_[slot] = math::NextUp(relative.max.y);
    maxZ_[slot] = math::NextUp(relative.max.z);
    const float values[6] = {minX_[slot], minY_[slot], minZ_[slot],
                             maxX_[slot], maxY_[slot], maxZ_[slot]};
    for (const float value : values) {
        header.radius = MaxMagnitude(header.radius, value);
    }
}

void FlatBoundsSet::WritePageElement(uint32 page, uint32 position, uint32 record) noexcept {
    const detail::FlatRecord& data = records_.ElementAt(record);
    const uint32 slot = page * config_.pageCapacity + position;
    WritePageBounds(page, position, data.bounds);
    drawMin_[slot] = data.minDraw;
    drawMax_[slot] = StoredDrawMax(data.maxDraw);
    filter_[slot] = data.filter;
    record_[slot] = record;
    userValue_[slot] = data.userValue;
    SetAlwaysBit(slot, data.alwaysVisible);
    pages_[page].distanceLimited += IsDistanceLimited(data.minDraw, data.maxDraw) ? 1u : 0u;
}

void FlatBoundsSet::CopyPageElement(uint32 fromSlot, uint32 toSlot) noexcept {
    // 같은 타일 안의 이동이므로 상대 float 경계를 그대로 옮긴다(같은 원점 기준).
    minX_[toSlot] = minX_[fromSlot];
    minY_[toSlot] = minY_[fromSlot];
    minZ_[toSlot] = minZ_[fromSlot];
    maxX_[toSlot] = maxX_[fromSlot];
    maxY_[toSlot] = maxY_[fromSlot];
    maxZ_[toSlot] = maxZ_[fromSlot];
    drawMin_[toSlot] = drawMin_[fromSlot];
    drawMax_[toSlot] = drawMax_[fromSlot];
    filter_[toSlot] = filter_[fromSlot];
    record_[toSlot] = record_[fromSlot];
    userValue_[toSlot] = userValue_[fromSlot];
    SetAlwaysBit(toSlot, ((always_[fromSlot / FlatVisibilityBitsPerWord] >>
                           (fromSlot % FlatVisibilityBitsPerWord)) &
                          1u) != 0);
}

void FlatBoundsSet::SetAlwaysBit(uint32 slot, bool value) noexcept {
    const uint64 bit = uint64{1} << (slot % FlatVisibilityBitsPerWord);
    uint64& word = always_[slot / FlatVisibilityBitsPerWord];
    word = value ? (word | bit) : (word & ~bit);
}

void FlatBoundsSet::FreePage(uint32 page) noexcept {
    pages_.Free(page);
    ++counters_.pageReleases;
}

uint32 FlatBoundsSet::Unplace(uint32 record) noexcept {
    const detail::FlatRecord data = records_.ElementAt(record);
    if (data.page == detail::FlatNoPage) {
        // 목록도 swap-remove다.
        largeCount_ -= data.large ? 1u : 0u;
        const uint32 last = static_cast<uint32>(list_.Size()) - 1;
        uint32 moved = NoRecord;
        if (data.position != last) {
            list_[data.position] = list_[last];
            moved = list_[data.position].record;
            records_.ElementAt(moved).position = data.position;
        }
        Require(static_cast<bool>(list_.TryResize(last)), "FlatBoundsSet list shrink failed");
        return moved;
    }

    // 그 타일 마지막 page의 마지막 원소로 빈자리를 채운다(5절). 마지막 page만 덜 찰 수 있다는
    // 조건이 유지된다.
    const uint32 page = data.page;
    const uint32 capacity = config_.pageCapacity;
    pages_[page].distanceLimited -= IsDistanceLimited(data.minDraw, data.maxDraw) ? 1u : 0u;
    detail::FlatTile* tile = tiles_.Find(pages_[page].key);
    Require(tile != nullptr, "FlatBoundsSet page tile is missing");
    const uint32 lastPage = tile->lastPage;
    const uint32 lastPosition = pages_[lastPage].count - 1;
    uint32 moved = NoRecord;
    if (page != lastPage || data.position != lastPosition) {
        const uint32 from = lastPage * capacity + lastPosition;
        const uint32 to = page * capacity + data.position;
        moved = record_[from];
        const detail::FlatRecord& movedData = records_.ElementAt(moved);
        const bool limited = IsDistanceLimited(movedData.minDraw, movedData.maxDraw);
        pages_[lastPage].distanceLimited -= limited ? 1u : 0u;
        pages_[page].distanceLimited += limited ? 1u : 0u;
        CopyPageElement(from, to);
        const float values[6] = {minX_[to], minY_[to], minZ_[to], maxX_[to], maxY_[to], maxZ_[to]};
        for (const float value : values) {
            pages_[page].radius = MaxMagnitude(pages_[page].radius, value);
        }
        detail::FlatRecord& movedRecord = records_.ElementAt(moved);
        movedRecord.page = page;
        movedRecord.position = data.position;
    }
    detail::FlatPageHeader& last = pages_[lastPage];
    --last.count;
    if (last.count == 0) {
        const uint32 previous = last.previous;
        if (previous == detail::FlatNoPage) {
            tile->firstPage = detail::FlatNoPage;
        } else {
            pages_[previous].next = detail::FlatNoPage;
        }
        tile->lastPage = previous;
        --tile->pageCount;
        const detail::FlatTileKey key = last.key;
        const bool tileEmpty = tile->pageCount == 0;
        FreePage(lastPage);
        if (tileEmpty) {
            (void)tiles_.Erase(key);
        }
    }
    return moved;
}

uint32 FlatBoundsSet::SlotOfRecord(uint32 record) const noexcept {
    const detail::FlatRecord& data = records_.ElementAt(record);
    return data.page == detail::FlatNoPage ? FlatBoundsListSlotBase + data.position
                                           : data.page * config_.pageCapacity + data.position;
}

void FlatBoundsSet::NoteMove(FlatBoundsMoves* moves, uint32 record) const noexcept {
    if (moves == nullptr || record == NoRecord) {
        return;
    }
    FlatBoundsMove& move = moves->moves[moves->count++];
    move.handle = FlatBoundsHandle{records_.HandleAt(record)};
    move.newSlot = SlotOfRecord(record);
}

// ---- 변경 ----

Result<FlatBoundsHandle> FlatBoundsSet::TryAdd(const FlatBoundsDesc& desc,
                                               FlatBoundsMoves* moves) noexcept {
    WriterGuard guard(*this);
    if (moves != nullptr) {
        moves->count = 0;
    }
    if (!desc.bounds.IsValid() || !IsValidDrawRange(desc.minDrawDistance, desc.maxDrawDistance)) {
        return Result<FlatBoundsHandle>::Failure(ErrorCode::InvalidArgument);
    }
    if (records_.Size() >= config_.maxElements) {
        return Result<FlatBoundsHandle>::Failure(ErrorCode::CapacityExceeded);
    }
    const Target target = TargetOf(desc.bounds);
    // 할당이 필요한 것을 모두 먼저 확보한다. 그 뒤의 레코드 추가는 할당하지 않으므로(폐기
    // slot만으로 한도에 닿은 CapacityExceeded만 남는다) 실패해도 논리 상태가 그대로다.
    if (records_.FreeSlotCount() == 0) {
        auto records = records_.TryEnsureCapacity(records_.SlotCount() + 1);
        if (!records) {
            return Result<FlatBoundsHandle>::Failure(records.Error());
        }
    }
    auto reserved = ReserveFor(target);
    if (!reserved) {
        return Result<FlatBoundsHandle>::Failure(reserved.Error());
    }
    detail::FlatRecord record;
    record.bounds = desc.bounds;
    record.filter = desc.filterMask;
    record.minDraw = desc.minDrawDistance;
    record.maxDraw = desc.maxDrawDistance;
    record.alwaysVisible = desc.alwaysVisible;
    record.userValue = desc.userValue;
    auto added = records_.TryAdd(record);
    if (!added) {
        // ReserveFor가 미리 넣은 빈 타일 항목을 지운다.
        if (!target.list) {
            const detail::FlatTile* tile = tiles_.Find(target.key);
            if (tile != nullptr && tile->pageCount == 0) {
                (void)tiles_.Erase(target.key);
            }
        }
        return Result<FlatBoundsHandle>::Failure(added.Error());
    }
    const uint32 index = added.Value().index;
    (void)Place(index, target);
    NoteMove(moves, index);
    return Result<FlatBoundsHandle>::Success(FlatBoundsHandle{added.Value()});
}

Result<void> FlatBoundsSet::TryRemove(FlatBoundsHandle handle, FlatBoundsMoves* moves) noexcept {
    WriterGuard guard(*this);
    if (moves != nullptr) {
        moves->count = 0;
    }
    auto checked = CheckHandle(handle);
    if (!checked) {
        return checked;
    }
    const uint32 moved = Unplace(handle.slot.index);
    Require(static_cast<bool>(records_.TryRemove(handle.slot)),
            "FlatBoundsSet record removal failed after a handle check");
    NoteMove(moves, moved);
    return Result<void>::Success();
}

Result<void> FlatBoundsSet::TryUpdateBounds(FlatBoundsHandle handle, const Box<double>& bounds,
                                            FlatBoundsMoves* moves) noexcept {
    WriterGuard guard(*this);
    if (moves != nullptr) {
        moves->count = 0;
    }
    auto checked = CheckHandle(handle);
    if (!checked) {
        return checked;
    }
    if (!bounds.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    const uint32 index = handle.slot.index;
    detail::FlatRecord& record = records_.ElementAt(index);
    const Target target = TargetOf(bounds);
    const bool inList = record.page == detail::FlatNoPage;

    // 같은 타일의 page 원소: 그 자리의 상대 float만 다시 쓴다(R은 늘기만 한다).
    if (!inList && !target.list && pages_[record.page].key == target.key) {
        record.bounds = bounds;
        WritePageBounds(record.page, record.position, bounds);
        ++counters_.inPlaceUpdates;
        return Result<void>::Success();
    }
    // 목록 안에 남는 원소: 그 자리의 double 경계와 종류(큰 원소·범위 밖)만 고친다.
    if (inList && target.list) {
        detail::FlatListEntry& entry = list_[record.position];
        largeCount_ -= entry.large ? 1u : 0u;
        largeCount_ += target.large ? 1u : 0u;
        entry.bounds = bounds;
        entry.large = target.large;
        record.bounds = bounds;
        record.large = target.large;
        ++counters_.inPlaceUpdates;
        return Result<void>::Success();
    }

    // 다른 타일 또는 목록과 page 사이의 이동: 새 자리를 먼저 확보하고(실패하면 옛 자리 그대로), 새
    // 자리에 붙인 뒤 옛 자리에서 뺀다. 두 자리는 서로 다른 타일이거나 page와 목록이라 빈자리
    // 채우기가 새 자리를 건드리지 않는다.
    auto reserved = ReserveFor(target);
    if (!reserved) {
        return reserved;
    }
    detail::FlatRecord previous = record;
    record.bounds = bounds;
    (void)Place(index, target);
    // Unplace는 레코드의 저장 위치를 읽으므로 옛 위치를 잠시 되돌려 놓는다.
    detail::FlatRecord& current = records_.ElementAt(index);
    const detail::FlatRecord placed = current;
    current.page = previous.page;
    current.position = previous.position;
    current.large = previous.large;
    const uint32 moved = Unplace(index);
    detail::FlatRecord& restored = records_.ElementAt(index);
    restored.page = placed.page;
    restored.position = placed.position;
    restored.large = placed.large;
    ++counters_.relocations;
    NoteMove(moves, index);
    NoteMove(moves, moved);
    return Result<void>::Success();
}

Result<void> FlatBoundsSet::TrySetFilter(FlatBoundsHandle handle, uint32 filterMask) noexcept {
    WriterGuard guard(*this);
    auto checked = CheckHandle(handle);
    if (!checked) {
        return checked;
    }
    detail::FlatRecord& record = records_.ElementAt(handle.slot.index);
    record.filter = filterMask;
    if (record.page == detail::FlatNoPage) {
        list_[record.position].filter = filterMask;
    } else {
        filter_[record.page * config_.pageCapacity + record.position] = filterMask;
    }
    return Result<void>::Success();
}

Result<void> FlatBoundsSet::TrySetDrawDistance(FlatBoundsHandle handle, float minDistance,
                                               float maxDistance) noexcept {
    WriterGuard guard(*this);
    auto checked = CheckHandle(handle);
    if (!checked) {
        return checked;
    }
    if (!IsValidDrawRange(minDistance, maxDistance)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    detail::FlatRecord& record = records_.ElementAt(handle.slot.index);
    if (record.page == detail::FlatNoPage) {
        list_[record.position].minDraw = minDistance;
        list_[record.position].maxDraw = maxDistance;
    } else {
        const uint32 slot = record.page * config_.pageCapacity + record.position;
        detail::FlatPageHeader& header = pages_[record.page];
        header.distanceLimited -= IsDistanceLimited(record.minDraw, record.maxDraw) ? 1u : 0u;
        header.distanceLimited += IsDistanceLimited(minDistance, maxDistance) ? 1u : 0u;
        drawMin_[slot] = minDistance;
        drawMax_[slot] = StoredDrawMax(maxDistance);
    }
    record.minDraw = minDistance;
    record.maxDraw = maxDistance;
    return Result<void>::Success();
}

Result<void> FlatBoundsSet::TrySetAlwaysVisible(FlatBoundsHandle handle,
                                                bool alwaysVisible) noexcept {
    WriterGuard guard(*this);
    auto checked = CheckHandle(handle);
    if (!checked) {
        return checked;
    }
    detail::FlatRecord& record = records_.ElementAt(handle.slot.index);
    record.alwaysVisible = alwaysVisible;
    if (record.page == detail::FlatNoPage) {
        list_[record.position].alwaysVisible = alwaysVisible;
    } else {
        SetAlwaysBit(record.page * config_.pageCapacity + record.position, alwaysVisible);
    }
    return Result<void>::Success();
}

Result<void> FlatBoundsSet::TrySetUserValue(FlatBoundsHandle handle, uint64 userValue) noexcept {
    WriterGuard guard(*this);
    auto checked = CheckHandle(handle);
    if (!checked) {
        return checked;
    }
    detail::FlatRecord& record = records_.ElementAt(handle.slot.index);
    record.userValue = userValue;
    if (record.page == detail::FlatNoPage) {
        list_[record.position].userValue = userValue;
    } else {
        userValue_[record.page * config_.pageCapacity + record.position] = userValue;
    }
    return Result<void>::Success();
}

Result<void> FlatBoundsSet::TryReserve(uint32 count) noexcept {
    WriterGuard guard(*this);
    if (count > config_.maxElements) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }
    auto records = records_.TryEnsureCapacity(count);
    if (!records) {
        return records;
    }
    const uint32 pageCount = (count + config_.pageCapacity - 1) / config_.pageCapacity;
    auto pages = pages_.TryReserve(pageCount);
    if (!pages) {
        return pages;
    }
    return EnsurePageArrays(pageCount);
}

void FlatBoundsSet::Clear() noexcept {
    WriterGuard guard(*this);
    records_.Clear();
    tiles_.Clear();
    pages_.Clear();
    minX_.Clear();
    minY_.Clear();
    minZ_.Clear();
    maxX_.Clear();
    maxY_.Clear();
    maxZ_.Clear();
    drawMin_.Clear();
    drawMax_.Clear();
    filter_.Clear();
    record_.Clear();
    userValue_.Clear();
    always_.Clear();
    list_.Clear();
    largeCount_ = 0;
}

// ---- 읽기 창구 ----

uint32 FlatBoundsSet::PageSlotCount() const noexcept {
    return pages_.SlotCount();
}

uint32 FlatBoundsSet::WordCount() const noexcept {
    const uint32 listWords = static_cast<uint32>((list_.Size() + FlatVisibilityBitsPerWord - 1) /
                                                 FlatVisibilityBitsPerWord);
    return pages_.SlotCount() * wordsPerPage_ + listWords;
}

uint32 FlatBoundsReader::SlotCapacity() const noexcept {
    return set_->pages_.SlotCount() * set_->config_.pageCapacity +
           static_cast<uint32>(set_->list_.Size());
}

uint32 FlatBoundsReader::WordCount() const noexcept {
    return set_->WordCount();
}

Result<uint32> FlatBoundsReader::TrySlotOf(FlatBoundsHandle handle) const noexcept {
    auto checked = set_->CheckHandle(handle);
    if (!checked) {
        return Result<uint32>::Failure(checked.Error());
    }
    return Result<uint32>::Success(set_->SlotOfRecord(handle.slot.index));
}

bool FlatBoundsReader::IsValid(FlatBoundsHandle handle) const noexcept {
    return static_cast<bool>(set_->CheckHandle(handle));
}

uint32 FlatBoundsReader::Count() const noexcept {
    return static_cast<uint32>(set_->records_.Size());
}

} // namespace rocks::spatial
