#include "LooseOctreeInternal.h"

#include <Core/Diagnostics/Log.h>
#include <Core/Math/MathUtility.h>
#include <Platform/Memory.h>

namespace rocks::spatial {

namespace {

// 노드 번호가 OutOfRangeNode·NoNode와 겹치지 않도록 블록 수를 제한한다(가장 큰 노드 번호 1 + 8b +
// 7이 OutOfRangeNode보다 작다).
constexpr uint32 MaxNodeBlocks = (detail::OutOfRangeNode - 8) / 8;

bool IsPowerOfTwo(double value) noexcept {
    if (!math::IsFinite(value) || value <= 0.0) {
        return false;
    }
    const uint64 bits = platform::BitCast<uint64>(value);
    const uint64 exponent = (bits >> 52) & 0x7ffu;
    const uint64 mantissa = bits & 0xfffffffffffffull;
    return exponent != 0 && mantissa == 0;
}

bool IsIntegral(double value) noexcept {
    return math::IsFinite(value) && math::Floor(value) == value;
}

} // namespace

// ---- 생성·이동 ----

Result<LooseOctree> LooseOctree::TryCreate(const LooseOctreeConfig& config,
                                           Allocator& allocator) noexcept {
    const auto invalid = []() noexcept {
        return Result<LooseOctree>::Failure(ErrorCode::InvalidArgument);
    };

    if (config.maxRootLevel > LooseOctreeLevelLimit || config.initialRootLevel < 1 ||
        config.initialRootLevel > config.maxRootLevel) {
        return invalid();
    }
    // 검색 스택 정적 용량이 깊이 상한(maxRootLevel + 1)에서 유도한 필요량 이상인지 확인한다.
    static_assert(7 * LooseOctreeLevelLimit + 1 <= LooseOctreeSearchStackCapacity,
                  "Search stack capacity must cover the deepest supported tree");
    const double cell = config.minCellSize;
    const double floatMinimum = static_cast<double>(NumericLimits<float>::Min());
    if (!IsPowerOfTwo(cell) || cell < floatMinimum * 16777216.0) {
        return invalid();
    }
    const double scaledLooseness = config.looseness * 64.0;
    if (!math::IsFinite(config.looseness) || config.looseness < 1.0 || config.looseness > 2.0 ||
        !IsIntegral(scaledLooseness)) {
        return invalid();
    }
    if (config.splitThreshold < 4 || config.mergeThreshold > config.splitThreshold / 2) {
        return invalid();
    }
    if (config.maxProxies == 0 || config.maxProxies > SlotArray<detail::ProxyRecord>::MaxSlots ||
        config.maxProxyGeneration == 0) {
        return invalid();
    }
    if (config.parentWarnCount == 0 || config.outOfRangeWarnCount == 0 ||
        config.pendingLimitFloor == 0) {
        return invalid();
    }

    // 격자 계산의 정확 범위: |anchor| + 2^(maxRootLevel+1) · minCellSize < 2^52 · minCellSize.
    double rootSpan = cell;
    for (uint32 level = 0; level <= config.maxRootLevel; ++level) {
        rootSpan *= 2.0;
    }
    const double exactLimit = cell * 0x1.0p52;
    for (uint32 axis = 0; axis < 3; ++axis) {
        const double anchor = config.anchor[axis];
        if (!IsIntegral(anchor / cell) || math::Abs(anchor) + rootSpan >= exactLimit) {
            return invalid();
        }
    }
    // 저장 float의 overflow 방지: k · 2^maxRootLevel · minCellSize가 float 정규 범위 안이어야 한다.
    if (config.looseness * (rootSpan * 0.5) > static_cast<double>(NumericLimits<float>::Max())) {
        return invalid();
    }

    // 내부 컨테이너의 할당을 세는 adapter를 받은 allocator에서 하나 만든다(6.10절 할당 횟수·피크).
    // 컨테이너가 그 주소를 쥐므로 옥트리 객체 밖에 둔다(옥트리를 옮겨도 주소가 같다).
    auto memory =
        allocator.Allocate(sizeof(detail::CountingAllocator), alignof(detail::CountingAllocator));
    if (!memory) {
        return Result<LooseOctree>::Failure(memory.Error());
    }
    if (memory.Value() == nullptr) {
        platform::Fatal("Allocator returned null for a nonzero allocation");
    }
    detail::CountingAllocator* counting =
        InitializeAt<detail::CountingAllocator>(memory.Value(), allocator);
    return Result<LooseOctree>::Success(LooseOctree(config, allocator, counting));
}

LooseOctree::LooseOctree(const LooseOctreeConfig& config, Allocator& backing,
                         detail::CountingAllocator* counting) noexcept
    : config_(config), backing_(&backing), counting_(counting),
      records_(*counting, config.maxProxyGeneration), searchBlocks_(*counting, MaxNodeBlocks),
      manageBlocks_(*counting, MaxNodeBlocks), chunks_(*counting), outOfRange_(*counting) {
    InitializeTables();
    const int64 origin[3] = {initialOrigin_, initialOrigin_, initialOrigin_};
    ResetRoot(config.initialRootLevel, origin);
}

LooseOctree::LooseOctree(LooseOctree&& other) noexcept
    : config_(other.config_), backing_(other.backing_), counting_(nullptr),
      records_(*other.backing_, other.config_.maxProxyGeneration),
      searchBlocks_(*other.backing_, MaxNodeBlocks), manageBlocks_(*other.backing_, MaxNodeBlocks),
      chunks_(*other.backing_), outOfRange_(*other.backing_) {
    other.CheckNoOpenScope("LooseOctree moved while a read scope is open");
    InitializeTables();
    TakeFrom(other);
}

LooseOctree& LooseOctree::operator=(LooseOctree&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    CheckNoOpenScope("LooseOctree overwritten while a read scope is open");
    other.CheckNoOpenScope("LooseOctree moved while a read scope is open");
    ReleaseStorage();
    config_ = other.config_;
    backing_ = other.backing_;
    InitializeTables();
    TakeFrom(other);
    return *this;
}

LooseOctree::~LooseOctree() noexcept {
    CheckNoOpenScope("LooseOctree destroyed while a read scope is open");
    ReleaseStorage();
}

void LooseOctree::InitializeTables() noexcept {
    double size = config_.minCellSize;
    for (uint32 level = 0; level <= LooseOctreeLevelLimit; ++level) {
        cellSize_[level] = size;
        looseHalf_[level] = config_.looseness * (size * 0.5);
        size *= 2.0;
    }
    halfMinCell_ = config_.minCellSize * 0.5;
    initialOrigin_ = -(int64{1} << (config_.initialRootLevel - 1));
    gridLimit_ = int64{1} << (config_.maxRootLevel + 1);
}

void LooseOctree::TakeFrom(LooseOctree& other) noexcept {
    // 이 객체의 컨테이너는 비어 있다(저장소 없음). 저장소와 계수 adapter를 함께 넘겨받는다.
    records_ = Move(other.records_);
    searchBlocks_ = Move(other.searchBlocks_);
    manageBlocks_ = Move(other.manageBlocks_);
    chunks_ = Move(other.chunks_);
    outOfRange_ = Move(other.outOfRange_);
    counting_ = other.counting_;
    rootSearch_ = other.rootSearch_;
    rootManage_ = other.rootManage_;
    queueHead_ = other.queueHead_;
    queueTail_ = other.queueTail_;
    queueLength_ = other.queueLength_;
    rootShrinkObservations_ = other.rootShrinkObservations_;
    outOfRangeWarned_ = other.outOfRangeWarned_;
    counters_ = other.counters_;
    peaks_ = other.peaks_;

    // 원본은 같은 설정의 빈 트리로 남는다. 컨테이너는 받은 allocator에 직접 묶어 두고, 다음 추가가
    // 새 계수 adapter를 만든다(EnsureCountingAllocator). 여기서는 할당하지 않는다.
    other.counting_ = nullptr;
    other.RebindContainers(*other.backing_);
    other.queueHead_ = detail::NoNode;
    other.queueTail_ = detail::NoNode;
    other.queueLength_ = 0;
    other.outOfRangeWarned_ = false;
    other.counters_ = Counters{};
    other.peaks_ = Peaks{};
    const int64 origin[3] = {other.initialOrigin_, other.initialOrigin_, other.initialOrigin_};
    other.ResetRoot(other.config_.initialRootLevel, origin);
}

void LooseOctree::RebindContainers(Allocator& allocator) noexcept {
    // 대입이 옛 저장소를 옛 allocator로 돌려준 뒤 빈 컨테이너를 allocator에 묶는다.
    records_ = SlotArray<detail::ProxyRecord>(allocator, config_.maxProxyGeneration);
    searchBlocks_ = IndexPool<detail::SearchBlock>(allocator, MaxNodeBlocks);
    manageBlocks_ = IndexPool<detail::ManageBlock>(allocator, MaxNodeBlocks);
    chunks_ = IndexPool<detail::ElementChunk>(allocator);
    outOfRange_ = Array<detail::OutOfRangeEntry>(allocator);
}

void LooseOctree::ReleaseStorage() noexcept {
    // 컨테이너 저장소를 adapter로 먼저 돌려준 뒤 adapter를 없앤다.
    RebindContainers(*backing_);
    if (counting_ != nullptr) {
        detail::CountingAllocator* counting = counting_;
        counting_ = nullptr;
        DestroyAt(counting);
        if (!backing_->Deallocate(counting)) {
            platform::Fatal("LooseOctree counting allocator was not owned by its allocator");
        }
    }
}

Result<void> LooseOctree::EnsureCountingAllocator() noexcept {
    if (counting_ != nullptr) {
        return Result<void>::Success();
    }
    // 이동된 원본이다. 컨테이너가 비어 있으므로 새 adapter에 다시 묶기만 하면 된다.
    auto memory =
        backing_->Allocate(sizeof(detail::CountingAllocator), alignof(detail::CountingAllocator));
    if (!memory) {
        return Result<void>::Failure(memory.Error());
    }
    if (memory.Value() == nullptr) {
        platform::Fatal("Allocator returned null for a nonzero allocation");
    }
    counting_ = InitializeAt<detail::CountingAllocator>(memory.Value(), *backing_);
    RebindContainers(*counting_);
    return Result<void>::Success();
}

void LooseOctree::NotePeaks() noexcept {
    const auto raise = [](usize& peak, usize value) noexcept {
        peak = value > peak ? value : peak;
    };
    raise(peaks_.recordBytes, records_.Capacity() * sizeof(detail::ProxyRecord));
    raise(peaks_.chunkBytes, chunks_.Capacity() * sizeof(detail::ElementChunk));
    raise(peaks_.nodeBytes, searchBlocks_.Capacity() * sizeof(detail::SearchBlock) +
                                manageBlocks_.Capacity() * sizeof(detail::ManageBlock));
    raise(peaks_.outOfRangeBytes, outOfRange_.Capacity() * sizeof(detail::OutOfRangeEntry));
}

void LooseOctree::CheckNoOpenScope(const char* message) const noexcept {
    if (platform::AtomicLoad(&readers_) != 0 || platform::AtomicLoad(&writer_) != 0) {
        platform::Fatal(message);
    }
}

// ---- 읽기 범위 ----

SpatialReadScope::SpatialReadScope(const LooseOctree& tree) noexcept : tree_(&tree) {
    platform::AtomicFetchAdd(&tree.readers_, 1u);
    if (platform::AtomicLoad(&tree.writer_) != 0) {
        platform::Fatal("LooseOctree read scope opened during a mutation");
    }
}

SpatialReadScope::~SpatialReadScope() noexcept {
    platform::AtomicFetchSub(&tree_->readers_, 1u);
}

// ---- 노드 접근·격자 ----

detail::SearchNode& LooseOctree::Search(uint32 node) noexcept {
    return node == detail::RootNode ? rootSearch_
                                    : searchBlocks_[BlockOf(node)].nodes[ChildSlotOf(node)];
}

detail::ManageNode& LooseOctree::Manage(uint32 node) noexcept {
    return node == detail::RootNode ? rootManage_
                                    : manageBlocks_[BlockOf(node)].nodes[ChildSlotOf(node)];
}

const detail::ManageNode& LooseOctree::Manage(uint32 node) const noexcept {
    return node == detail::RootNode ? rootManage_
                                    : manageBlocks_[BlockOf(node)].nodes[ChildSlotOf(node)];
}

uint32 LooseOctree::ParentOf(uint32 node) const noexcept {
    return node == detail::RootNode ? detail::NoNode : manageBlocks_[BlockOf(node)].parent;
}

Box<double> LooseOctree::CellBoundsOf(uint32 level, const int64 (&origin)[3]) const noexcept {
    Vec3d minimum;
    Vec3d maximum;
    const int64 span = int64{1} << level;
    minimum.x = config_.anchor.x + static_cast<double>(origin[0]) * config_.minCellSize;
    minimum.y = config_.anchor.y + static_cast<double>(origin[1]) * config_.minCellSize;
    minimum.z = config_.anchor.z + static_cast<double>(origin[2]) * config_.minCellSize;
    maximum.x = config_.anchor.x + static_cast<double>(origin[0] + span) * config_.minCellSize;
    maximum.y = config_.anchor.y + static_cast<double>(origin[1] + span) * config_.minCellSize;
    maximum.z = config_.anchor.z + static_cast<double>(origin[2] + span) * config_.minCellSize;
    return Box<double>(minimum, maximum);
}

uint32 LooseOctree::ChildIndexFor(const Vec3d& point, const Vec3d& center) noexcept {
    // 축마다 노드 중심 이상이면 양의 쪽이다(세 축 모두 같은 규칙, 6.2절 배치 규칙).
    uint32 child = 0;
    child |= point.x >= center.x ? 1u : 0u;
    child |= point.y >= center.y ? 2u : 0u;
    child |= point.z >= center.z ? 4u : 0u;
    return child;
}

Vec3d LooseOctree::BoxCenter(const Box<double>& box) noexcept {
    // min/2 + max/2로 합의 overflow를 피한다(Box::Center와 같은 식).
    return box.Center();
}

uint32 LooseOctree::Descend(uint32 start, const Box<double>& bounds) const noexcept {
    const Vec3d center = BoxCenter(bounds);
    uint32 node = start;
    for (;;) {
        const detail::SearchNode& search = Search(node);
        if (search.childBlock == detail::NoBlock) {
            return node;
        }
        const detail::ManageNode& manage = Manage(node);
        const uint32 child = ChildNode(
            search.childBlock, ChildIndexFor(center, CenterOf(manage.level, manage.origin)));
        const detail::ManageNode& childManage = Manage(child);
        if (!LooseBoundsOf(childManage.level, childManage.origin).Contains(bounds)) {
            return node;
        }
        node = child;
    }
}

// ---- 루트 배치(6.5절) ----

LooseOctree::Placement LooseOctree::PlanPlacement(const Box<double>& bounds) const noexcept {
    Placement placement;
    const Vec3d center = BoxCenter(bounds);
    uint32 level = rootManage_.level;
    int64 origin[3] = {rootManage_.origin[0], rootManage_.origin[1], rootManage_.origin[2]};

    if (rootSearch_.subtreeCount == 0) {
        // 빈 트리: 중심을 포함하는 initialRootLevel 격자 셀로 루트를 다시 잡는다. 정수 원점은
        // o = o0 + 2^R0 · floor((x − x0) / L_R0)이다. 격자 범위를 넘는 중심은 범위 밖이다.
        const uint32 initialLevel = config_.initialRootLevel;
        const double cellSize = cellSize_[initialLevel];
        const int64 span = int64{1} << initialLevel;
        const double limit = static_cast<double>(gridLimit_ / span + 1);
        for (uint32 axis = 0; axis < 3; ++axis) {
            const double base =
                config_.anchor[axis] + static_cast<double>(initialOrigin_) * config_.minCellSize;
            const double steps = math::Floor((center[axis] - base) / cellSize);
            if (!(math::Abs(steps) <= limit)) {
                placement.outOfRange = true;
                return placement;
            }
            origin[axis] = initialOrigin_ + static_cast<int64>(steps) * span;
            if (origin[axis] < -gridLimit_ || origin[axis] + span > gridLimit_) {
                placement.outOfRange = true;
                return placement;
            }
        }
        level = initialLevel;
        placement.reset = rootSearch_.childBlock != detail::NoBlock ||
                          rootManage_.level != initialLevel || rootManage_.origin[0] != origin[0] ||
                          rootManage_.origin[1] != origin[1] || rootManage_.origin[2] != origin[2];
        placement.resetOrigin[0] = origin[0];
        placement.resetOrigin[1] = origin[1];
        placement.resetOrigin[2] = origin[2];
    }

    // 수용 판정: 방향 규칙을 정수 원점으로 모의한다. 트리를 바꾸지 않는다(O(maxRootLevel)).
    for (;;) {
        if (LooseBoundsOf(level, origin).Contains(bounds) &&
            CellBoundsOf(level, origin).Contains(center)) {
            break;
        }
        if (level == config_.maxRootLevel) {
            placement.outOfRange = true;
            return placement;
        }
        const Vec3d rootCenter = CenterOf(level, origin);
        const int64 span = int64{1} << level;
        for (uint32 axis = 0; axis < 3; ++axis) {
            if (center[axis] < rootCenter[axis]) {
                origin[axis] -= span;
            }
        }
        ++level;
        const int64 grown = int64{1} << level;
        for (uint32 axis = 0; axis < 3; ++axis) {
            if (origin[axis] < -gridLimit_ || origin[axis] + grown > gridLimit_) {
                placement.outOfRange = true;
                return placement;
            }
        }
        ++placement.expansions;
    }
    placement.expansionNeedsBlocks =
        placement.expansions != 0 && !placement.reset &&
        (rootSearch_.subtreeCount != 0 || rootSearch_.childBlock != detail::NoBlock);
    return placement;
}

Result<void> LooseOctree::ReservePlacement(const Placement& placement) noexcept {
    if (placement.outOfRange) {
        return outOfRange_.TryEnsureCapacity(outOfRange_.Size() + 1);
    }
    if (placement.expansionNeedsBlocks) {
        auto search = searchBlocks_.TryReserveAdditional(placement.expansions);
        if (!search) {
            return search;
        }
        auto manage = manageBlocks_.TryReserveAdditional(placement.expansions);
        if (!manage) {
            return manage;
        }
    }
    return chunks_.TryReserveAdditional(1);
}

void LooseOctree::ApplyRootChanges(const Placement& placement, const Vec3d& center) noexcept {
    if (placement.reset) {
        FreeAllStructure();
        ResetRoot(config_.initialRootLevel, placement.resetOrigin);
    }
    for (uint32 step = 0; step < placement.expansions; ++step) {
        ExpandRootOnce(center);
    }
}

void LooseOctree::ResetRoot(uint32 level, const int64 (&origin)[3]) noexcept {
    rootSearch_ = detail::SearchNode{};
    rootManage_ = detail::ManageNode{};
    rootManage_.level = static_cast<uint8>(level);
    rootManage_.origin[0] = origin[0];
    rootManage_.origin[1] = origin[1];
    rootManage_.origin[2] = origin[2];
    rootShrinkObservations_ = 0;
}

void LooseOctree::ExpandRootOnce(const Vec3d& center) noexcept {
    const uint32 level = rootManage_.level;
    const Vec3d rootCenter = CenterOf(level, rootManage_.origin);
    const int64 span = int64{1} << level;
    int64 origin[3];
    uint32 oldRootSlot = 0;
    for (uint32 axis = 0; axis < 3; ++axis) {
        // 중심 쪽으로 넓힌다. 음의 방향으로 넓히면 옛 루트는 새 루트의 양의 쪽 자식이 된다.
        if (center[axis] < rootCenter[axis]) {
            origin[axis] = rootManage_.origin[axis] - span;
            oldRootSlot |= 1u << axis;
        } else {
            origin[axis] = rootManage_.origin[axis];
        }
    }
    ++counters_.rootExpansions;
    // 확장은 축소 이력을 처음부터 다시 센다(확장 직후의 축소 왕복을 막는다).
    rootShrinkObservations_ = 0;

    if (rootSearch_.subtreeCount == 0) {
        // 내용이 없는 루트는 옮길 것이 없다. 남은 빈 블록만 버리고 새 루트로 바꾼다.
        FreeEmptySubtreeBlocks(detail::RootNode);
        const bool queued = (rootManage_.flags & detail::NodeQueued) != 0;
        if (queued) {
            Unlink(detail::RootNode);
        }
        rootManage_.level = static_cast<uint8>(level + 1);
        rootManage_.origin[0] = origin[0];
        rootManage_.origin[1] = origin[1];
        rootManage_.origin[2] = origin[2];
        rootManage_.flags = 0;
        return;
    }

    // 블록은 호출자가 미리 확보했다(ReservePlacement). 두 풀은 같은 순서로 할당·반환하므로 같은
    // 번호를 준다.
    const uint32 block = searchBlocks_.AllocateReserved();
    if (manageBlocks_.AllocateReserved() != block) {
        platform::Fatal("LooseOctree node block pools diverged");
    }
    manageBlocks_[block].parent = detail::RootNode;
    for (uint32 child = 0; child < 8; ++child) {
        detail::ManageNode& manage = manageBlocks_[block].nodes[child];
        manage.level = static_cast<uint8>(level);
        ChildOrigin(level + 1, origin, child, manage.origin);
    }

    // 옛 루트의 내용(직접 저장 청크, 자식 블록, 플래그, 관리 데이터)을 새 블록의 해당 자식 자리로
    // 옮긴다. 같은 셀이므로 float 상대 경계는 그대로다.
    const uint32 target = ChildNode(block, oldRootSlot);
    const bool queued = (rootManage_.flags & detail::NodeQueued) != 0;
    if (queued) {
        Unlink(detail::RootNode);
    }
    Search(target) = rootSearch_;
    detail::ManageNode moved = rootManage_;
    moved.queuePrevious = detail::NoNode;
    moved.queueNext = detail::NoNode;
    Manage(target) = moved;
    for (uint32 chunk = rootSearch_.firstChunk; chunk != detail::NoChunk;
         chunk = chunks_[chunk].next) {
        const detail::ElementChunk& elements = chunks_[chunk];
        for (uint32 entry = 0; entry < elements.count; ++entry) {
            records_.ElementAt(elements.slot[entry]).node = target;
        }
    }
    if (rootSearch_.childBlock != detail::NoBlock) {
        manageBlocks_[rootSearch_.childBlock].parent = target;
    }
    if (queued) {
        Enqueue(target);
    }

    const uint32 subtree = rootSearch_.subtreeCount;
    rootSearch_ = detail::SearchNode{};
    rootSearch_.childBlock = block;
    rootSearch_.subtreeCount = subtree;
    rootManage_ = detail::ManageNode{};
    rootManage_.level = static_cast<uint8>(level + 1);
    rootManage_.origin[0] = origin[0];
    rootManage_.origin[1] = origin[1];
    rootManage_.origin[2] = origin[2];
    rootManage_.childMask = static_cast<uint8>(1u << oldRootSlot);
}

// ---- 청크·경로 ----

LooseOctree::EntryData LooseOctree::EntryOfRecord(const detail::ProxyRecord& record,
                                                  SlotHandle handle) const noexcept {
    EntryData data;
    data.bounds = record.bounds;
    data.filter = record.filter;
    data.slot = handle.index;
    data.generation = handle.generation;
    data.userValue = record.userValue;
    return data;
}

void LooseOctree::WriteEntryBounds(uint32 node, uint32 chunk, uint32 entry,
                                   const Box<double>& bounds) noexcept {
    // 노드 중심 m 기준 double 차이를 바깥쪽으로 float 반올림한 뒤 한 ULP 더 바깥쪽으로 옮긴다.
    // double 뺄셈은 최근접 반올림이라 외향이 아니므로 이 보정이 불변 조건 7을 문자 그대로
    // 성립시킨다(6.3절).
    const detail::ManageNode& manage = Manage(node);
    const Vec3d center = CenterOf(manage.level, manage.origin);
    const Box<float> relative =
        Box<double>(bounds.min - center, bounds.max - center).ToFloatOutward();
    detail::ElementChunk& elements = chunks_[chunk];
    elements.minX[entry] = math::NextDown(relative.min.x);
    elements.minY[entry] = math::NextDown(relative.min.y);
    elements.minZ[entry] = math::NextDown(relative.min.z);
    elements.maxX[entry] = math::NextUp(relative.max.x);
    elements.maxY[entry] = math::NextUp(relative.max.y);
    elements.maxZ[entry] = math::NextUp(relative.max.z);
}

void LooseOctree::WriteEntry(uint32 node, uint32 chunk, uint32 entry,
                             const EntryData& data) noexcept {
    detail::ElementChunk& elements = chunks_[chunk];
    elements.filter[entry] = data.filter;
    elements.slot[entry] = data.slot;
    elements.generation[entry] = data.generation;
    elements.userValue[entry] = data.userValue;
    WriteEntryBounds(node, chunk, entry, data.bounds);
}

void LooseOctree::AppendEntry(uint32 node, const EntryData& data, uint32& chunkOut,
                              uint32& entryOut) noexcept {
    detail::ManageNode& manage = Manage(node);
    detail::SearchNode& search = Search(node);
    if (manage.lastChunk == detail::NoChunk ||
        chunks_[manage.lastChunk].count == LooseOctreeChunkCapacity) {
        // 청크는 호출자가 미리 확보했다.
        const uint32 chunk = chunks_.AllocateReserved();
        chunks_[chunk].previous = manage.lastChunk;
        if (manage.lastChunk != detail::NoChunk) {
            chunks_[manage.lastChunk].next = chunk;
        } else {
            search.firstChunk = chunk;
        }
        manage.lastChunk = chunk;
    }
    chunkOut = manage.lastChunk;
    entryOut = chunks_[chunkOut].count++;
    WriteEntry(node, chunkOut, entryOut, data);
    ++search.directCount;

    if (search.directCount > config_.parentWarnCount && (manage.flags & detail::NodeWarned) == 0) {
        manage.flags |= detail::NodeWarned;
        ++counters_.parentWarnEvents;
        Log::Warning(LogSpatialIndex,
                     "Node {Node} at level {Level} stores {Count} proxies directly (limit {Limit})",
                     node, static_cast<uint32>(manage.level), search.directCount,
                     config_.parentWarnCount);
    }
}

void LooseOctree::RemoveEntry(uint32 node, uint32 chunk, uint32 entry) noexcept {
    detail::ManageNode& manage = Manage(node);
    detail::SearchNode& search = Search(node);
    const uint32 lastChunk = manage.lastChunk;
    detail::ElementChunk& last = chunks_[lastChunk];
    const uint32 lastEntry = last.count - 1;
    if (chunk != lastChunk || entry != lastEntry) {
        // 마지막 프록시를 빈 자리로 옮기고(swap-remove) 그 레코드의 위치를 고친다.
        detail::ElementChunk& hole = chunks_[chunk];
        hole.minX[entry] = last.minX[lastEntry];
        hole.minY[entry] = last.minY[lastEntry];
        hole.minZ[entry] = last.minZ[lastEntry];
        hole.maxX[entry] = last.maxX[lastEntry];
        hole.maxY[entry] = last.maxY[lastEntry];
        hole.maxZ[entry] = last.maxZ[lastEntry];
        hole.filter[entry] = last.filter[lastEntry];
        hole.slot[entry] = last.slot[lastEntry];
        hole.generation[entry] = last.generation[lastEntry];
        hole.userValue[entry] = last.userValue[lastEntry];
        detail::ProxyRecord& moved = records_.ElementAt(hole.slot[entry]);
        moved.chunk = chunk;
        moved.entry = entry;
    }
    --last.count;
    if (last.count == 0) {
        const uint32 previous = last.previous;
        chunks_.Free(lastChunk);
        if (previous != detail::NoChunk) {
            chunks_[previous].next = detail::NoChunk;
        } else {
            search.firstChunk = detail::NoChunk;
        }
        manage.lastChunk = previous;
    }
    --search.directCount;
    if (search.directCount <= config_.parentWarnCount / 2) {
        manage.flags &= static_cast<uint8>(~detail::NodeWarned);
    }
}

bool LooseOctree::ShouldCollapse(uint32 node) const noexcept {
    const detail::SearchNode& search = Search(node);
    return search.childBlock != detail::NoBlock &&
           (search.subtreeCount <= config_.mergeThreshold || Manage(node).childMask == 0);
}

void LooseOctree::AdjustPath(uint32 from, uint32 stopExclusive, bool increment) noexcept {
    uint32 node = from;
    while (node != stopExclusive && node != detail::NoNode) {
        detail::SearchNode& search = Search(node);
        const uint32 parent = ParentOf(node);
        const uint8 bit =
            node == detail::RootNode ? 0 : static_cast<uint8>(1u << ChildSlotOf(node));
        if (increment) {
            ++search.subtreeCount;
            if (search.subtreeCount == 1 && parent != detail::NoNode) {
                Manage(parent).childMask |= bit;
            }
        } else {
            --search.subtreeCount;
            if (search.subtreeCount == 0 && parent != detail::NoNode) {
                detail::ManageNode& parentManage = Manage(parent);
                parentManage.childMask &= static_cast<uint8>(~bit);
                // 경로 끝(stopExclusive) 위의 부모도 점유 마스크가 0이 되면 회수 대상이다.
                if (parentManage.childMask == 0) {
                    Enqueue(parent);
                }
            }
            // 병합·빈 블록 회수 후보를 유지보수 대기열에 넣는다(6.5절 삭제 4단계).
            if (ShouldCollapse(node)) {
                Enqueue(node);
            }
        }
        node = parent;
    }
}

void LooseOctree::NoteHeldChange(uint32 node) noexcept {
    detail::ManageNode& manage = Manage(node);
    if ((manage.flags & detail::NodeHeld) == 0) {
        return;
    }
    const uint32 direct = Search(node).directCount;
    if (direct <= config_.splitThreshold) {
        ReleaseHold(node);
        return;
    }
    ++manage.changesSinceHold;
    // 제거·경계 갱신 호출은 분할 때문에 실패하면 안 되므로 재평가는 표시만 하고 유지보수나 다음
    // 삽입이 수행한다(6.4절 보류 상태).
    if (manage.changesSinceHold >= manage.reevaluationInterval ||
        static_cast<uint64>(direct) >= static_cast<uint64>(manage.heldCount) * 2) {
        manage.flags |= detail::NodeReevaluationPending;
        Enqueue(node);
    }
}

void LooseOctree::AfterDirectGrowth(uint32 node, bool evaluateNow) noexcept {
    if (Search(node).childBlock != detail::NoBlock) {
        return;
    }
    detail::ManageNode& manage = Manage(node);
    const uint32 direct = Search(node).directCount;
    bool due = (manage.flags & detail::NodeReevaluationPending) != 0;
    bool reevaluation = due;
    if ((manage.flags & detail::NodeHeld) != 0) {
        ++manage.changesSinceHold;
        if (static_cast<uint64>(direct) >= static_cast<uint64>(manage.heldCount) * 2 ||
            manage.changesSinceHold >= manage.reevaluationInterval) {
            due = true;
            reevaluation = true;
        }
    } else if (direct > config_.splitThreshold) {
        due = true;
    }
    if (!due) {
        return;
    }
    if (direct <= config_.splitThreshold) {
        // 대기 표시 뒤 수가 줄었다. 평가할 것이 없다.
        ReleaseHold(node);
        return;
    }
    if (!evaluateNow) {
        // 경계 갱신은 분할 평가를 하지 않고 재평가 대기만 표시한다. 유지보수나 다음 삽입이 수행한다
        // (6.4절 보류 상태: 갱신 호출이 분할 비용을 지지 않게 한다).
        manage.flags |= detail::NodeReevaluationPending;
        Enqueue(node);
        return;
    }
    manage.flags &= static_cast<uint8>(~detail::NodeReevaluationPending);
    if (reevaluation) {
        ++counters_.reevaluations;
    }
    EvaluateSplits(node);
}

void LooseOctree::InsertIntoTree(uint32 start, uint32 stopExclusive, uint32 slot,
                                 bool evaluateSplit) noexcept {
    detail::ProxyRecord& record = records_.ElementAt(slot);
    const uint32 target = Descend(start, record.bounds);
    uint32 chunk = 0;
    uint32 entry = 0;
    AppendEntry(target, EntryOfRecord(record, records_.HandleAt(slot)), chunk, entry);
    record.node = target;
    record.chunk = chunk;
    record.entry = entry;
    AdjustPath(target, stopExclusive, true);
    AfterDirectGrowth(target, evaluateSplit);
}

void LooseOctree::NoteOutOfRangeGrowth() noexcept {
    ++counters_.outOfRangeInsertions;
    if (outOfRange_.Size() > config_.outOfRangeWarnCount && !outOfRangeWarned_) {
        outOfRangeWarned_ = true;
        ++counters_.outOfRangeWarnEvents;
        Log::Warning(LogSpatialIndex,
                     "Out-of-range list holds {Count} proxies (limit {Limit}); they are tested by "
                     "every query",
                     static_cast<uint64>(outOfRange_.Size()), config_.outOfRangeWarnCount);
    }
}

void LooseOctree::MoveToOutOfRange(uint32 slot) noexcept {
    detail::ProxyRecord& record = records_.ElementAt(slot);
    const SlotHandle handle = records_.HandleAt(slot);
    detail::OutOfRangeEntry entry;
    entry.bounds = record.bounds;
    entry.filter = record.filter;
    entry.slot = handle.index;
    entry.generation = handle.generation;
    entry.userValue = record.userValue;
    // 용량은 호출자가 확보했다.
    if (!outOfRange_.TryPushBack(entry)) {
        platform::Fatal("LooseOctree out-of-range list lost its reserved capacity");
    }
    record.node = detail::OutOfRangeNode;
    record.chunk = static_cast<uint32>(outOfRange_.Size() - 1);
    record.entry = 0;
    NoteOutOfRangeGrowth();
}

void LooseOctree::RemoveFromOutOfRange(uint32 index) noexcept {
    if (!outOfRange_.TryRemoveAtSwap(index)) {
        platform::Fatal("LooseOctree out-of-range index is invalid");
    }
    if (index < outOfRange_.Size()) {
        records_.ElementAt(outOfRange_[index].slot).chunk = index;
    }
    if (outOfRange_.Size() <= config_.outOfRangeWarnCount / 2) {
        outOfRangeWarned_ = false;
    }
}

Result<void> LooseOctree::CheckHandle(SpatialProxyHandle handle) const noexcept {
    if (handle.IsNull()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (!records_.IsValid(handle.slot)) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }
    return Result<void>::Success();
}

// ---- 변경 API ----

Result<SpatialProxyHandle> LooseOctree::TryAdd(const SpatialProxyDesc& desc) noexcept {
    WriterGuard guard(*this);
    if (!desc.bounds.IsValid()) {
        return Result<SpatialProxyHandle>::Failure(ErrorCode::InvalidArgument);
    }
    if (records_.Size() >= config_.maxProxies) {
        return Result<SpatialProxyHandle>::Failure(ErrorCode::CapacityExceeded);
    }
    auto counted = EnsureCountingAllocator();
    if (!counted) {
        return Result<SpatialProxyHandle>::Failure(counted.Error());
    }

    // 실패할 수 있는 확보(레코드 slot, 범위 밖 목록 또는 청크와 확장 블록)를 모두 먼저 한다. 그
    // 뒤로는 할당이 없으므로 논리 상태가 반쯤 바뀐 채 실패하지 않는다.
    if (records_.FreeSlotCount() == 0) {
        if (records_.SlotCount() == SlotArray<detail::ProxyRecord>::MaxSlots) {
            return Result<SpatialProxyHandle>::Failure(ErrorCode::CapacityExceeded);
        }
        auto reserved = records_.TryEnsureCapacity(records_.SlotCount() + 1);
        if (!reserved) {
            return Result<SpatialProxyHandle>::Failure(reserved.Error());
        }
    }
    const Placement placement = PlanPlacement(desc.bounds);
    auto reserved = ReservePlacement(placement);
    if (!reserved) {
        return Result<SpatialProxyHandle>::Failure(reserved.Error());
    }

    detail::ProxyRecord record;
    record.bounds = desc.bounds;
    record.filter = desc.filterMask;
    record.userValue = desc.userValue;
    auto added = records_.TryEmplace(record);
    if (!added) {
        platform::Fatal("LooseOctree record slot was reserved but adding failed");
    }
    const SlotHandle handle = added.Value();

    if (placement.outOfRange) {
        MoveToOutOfRange(handle.index);
    } else {
        ApplyRootChanges(placement, BoxCenter(desc.bounds));
        InsertIntoTree(detail::RootNode, detail::NoNode, handle.index, true);
    }
    EnforcePendingLimit();
    return Result<SpatialProxyHandle>::Success(SpatialProxyHandle{handle});
}

Result<void> LooseOctree::TryRemove(SpatialProxyHandle handle) noexcept {
    WriterGuard guard(*this);
    auto checked = CheckHandle(handle);
    if (!checked) {
        return checked;
    }
    const detail::ProxyRecord& record = records_[handle.slot];
    if (record.node == detail::OutOfRangeNode) {
        RemoveFromOutOfRange(record.chunk);
    } else {
        const uint32 node = record.node;
        RemoveEntry(node, record.chunk, record.entry);
        AdjustPath(node, detail::NoNode, false);
        NoteHeldChange(node);
    }
    if (!records_.TryRemove(handle.slot)) {
        platform::Fatal("LooseOctree failed to release a live record");
    }
    EnforcePendingLimit();
    return Result<void>::Success();
}

Result<void> LooseOctree::TryUpdateBounds(SpatialProxyHandle handle,
                                          const Box<double>& bounds) noexcept {
    WriterGuard guard(*this);
    auto checked = CheckHandle(handle);
    if (!checked) {
        return checked;
    }
    if (!bounds.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    const uint32 slot = handle.slot.index;
    const Vec3d center = BoxCenter(bounds);

    // 범위 밖 목록의 프록시는 수용 판정을 다시 해서 한도 안이면 트리에 넣는다.
    if (records_.ElementAt(slot).node == detail::OutOfRangeNode) {
        const Placement placement = PlanPlacement(bounds);
        if (placement.outOfRange) {
            detail::ProxyRecord& record = records_.ElementAt(slot);
            record.bounds = bounds;
            outOfRange_[record.chunk].bounds = bounds;
            return Result<void>::Success();
        }
        auto reserved = ReservePlacement(placement);
        if (!reserved) {
            return reserved;
        }
        detail::ProxyRecord& record = records_.ElementAt(slot);
        RemoveFromOutOfRange(record.chunk);
        record.bounds = bounds;
        ApplyRootChanges(placement, center);
        InsertIntoTree(detail::RootNode, detail::NoNode, slot, false);
        ++counters_.reinserts;
        EnforcePendingLimit();
        return Result<void>::Success();
    }

    const uint32 node = records_.ElementAt(slot).node;
    const detail::ManageNode& manage = Manage(node);
    if (LooseBoundsOf(manage.level, manage.origin).Contains(bounds)) {
        // 제자리 갱신: 지금 노드의 확장 경계 안이면 경계만 고친다(구조 변경 없음).
        detail::ProxyRecord& record = records_.ElementAt(slot);
        record.bounds = bounds;
        WriteEntryBounds(node, record.chunk, record.entry, bounds);
        ++counters_.inPlaceUpdates;
        NoteHeldChange(node);
        const detail::SearchNode& search = Search(node);
        if (search.childBlock != detail::NoBlock) {
            const uint32 child = ChildNode(
                search.childBlock, ChildIndexFor(center, CenterOf(manage.level, manage.origin)));
            const detail::ManageNode& childManage = Manage(child);
            if (LooseBoundsOf(childManage.level, childManage.origin).Contains(bounds)) {
                Manage(node).flags |= detail::NodeRelocationPending;
                Enqueue(node);
            }
        }
        EnforcePendingLimit();
        return Result<void>::Success();
    }

    // 벗어났다: 새 AABB를 포함하는 첫 조상 A를 찾는다. 루트에도 들어가지 않거나 A가 루트인데 중심이
    // 루트 기본 셀 밖이면 루트 조건(확장 또는 범위 밖 목록)으로 처리한다.
    uint32 ancestor = ParentOf(node);
    while (ancestor != detail::NoNode) {
        const detail::ManageNode& ancestorManage = Manage(ancestor);
        if (LooseBoundsOf(ancestorManage.level, ancestorManage.origin).Contains(bounds)) {
            break;
        }
        ancestor = ParentOf(ancestor);
    }
    const bool needsRoot = ancestor == detail::NoNode ||
                           (ancestor == detail::RootNode &&
                            !CellBoundsOf(rootManage_.level, rootManage_.origin).Contains(center));
    Placement placement;
    if (needsRoot) {
        placement = PlanPlacement(bounds);
        if (placement.outOfRange) {
            auto reserved = outOfRange_.TryEnsureCapacity(outOfRange_.Size() + 1);
            if (!reserved) {
                return reserved;
            }
            const detail::ProxyRecord& record = records_.ElementAt(slot);
            RemoveEntry(node, record.chunk, record.entry);
            AdjustPath(node, detail::NoNode, false);
            NoteHeldChange(node);
            records_.ElementAt(slot).bounds = bounds;
            MoveToOutOfRange(slot);
            ++counters_.reinserts;
            EnforcePendingLimit();
            return Result<void>::Success();
        }
        auto reserved = ReservePlacement(placement);
        if (!reserved) {
            return reserved;
        }
    } else {
        auto reserved = chunks_.TryReserveAdditional(1);
        if (!reserved) {
            return reserved;
        }
    }

    if (needsRoot) {
        // 트리에 이 프록시가 있으므로 빈 트리 재설정은 없다. 확장이 옛 루트 내용을 옮기면 노드
        // 번호가 바뀌므로 레코드에서 다시 읽는다.
        ApplyRootChanges(placement, center);
        ancestor = detail::RootNode;
    }
    const uint32 current = records_.ElementAt(slot).node;
    {
        const detail::ProxyRecord& record = records_.ElementAt(slot);
        RemoveEntry(current, record.chunk, record.entry);
    }
    AdjustPath(current, ancestor, false);
    NoteHeldChange(current);
    records_.ElementAt(slot).bounds = bounds;
    InsertIntoTree(ancestor, ancestor, slot, false);
    ++counters_.reinserts;
    EnforcePendingLimit();
    return Result<void>::Success();
}

Result<void> LooseOctree::TrySetFilter(SpatialProxyHandle handle, uint32 filterMask) noexcept {
    WriterGuard guard(*this);
    auto checked = CheckHandle(handle);
    if (!checked) {
        return checked;
    }
    detail::ProxyRecord& record = records_[handle.slot];
    record.filter = filterMask;
    if (record.node == detail::OutOfRangeNode) {
        outOfRange_[record.chunk].filter = filterMask;
    } else {
        chunks_[record.chunk].filter[record.entry] = filterMask;
    }
    return Result<void>::Success();
}

Result<void> LooseOctree::TrySetUserValue(SpatialProxyHandle handle, uint64 userValue) noexcept {
    WriterGuard guard(*this);
    auto checked = CheckHandle(handle);
    if (!checked) {
        return checked;
    }
    detail::ProxyRecord& record = records_[handle.slot];
    record.userValue = userValue;
    if (record.node == detail::OutOfRangeNode) {
        outOfRange_[record.chunk].userValue = userValue;
    } else {
        chunks_[record.chunk].userValue[record.entry] = userValue;
    }
    return Result<void>::Success();
}

Result<void> LooseOctree::TryReserve(uint32 proxyCount) noexcept {
    WriterGuard guard(*this);
    if (proxyCount > config_.maxProxies) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }
    auto counted = EnsureCountingAllocator();
    if (!counted) {
        return counted;
    }
    auto records = records_.TryReserve(proxyCount);
    if (!records) {
        return records;
    }
    const uint32 chunkCount = proxyCount / LooseOctreeChunkCapacity +
                              (proxyCount % LooseOctreeChunkCapacity != 0 ? 1 : 0);
    return chunks_.TryReserve(chunkCount);
}

void LooseOctree::Clear() noexcept {
    WriterGuard guard(*this);
    records_.Clear();
    outOfRange_.Clear();
    outOfRangeWarned_ = false;
    FreeAllStructure();
    const int64 origin[3] = {initialOrigin_, initialOrigin_, initialOrigin_};
    ResetRoot(config_.initialRootLevel, origin);
}

} // namespace rocks::spatial
