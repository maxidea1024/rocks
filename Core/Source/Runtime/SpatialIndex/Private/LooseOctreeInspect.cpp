#include "LooseOctreeInternal.h"

namespace rocks::spatial {

namespace {

uint32 HistogramBucket(uint32 count) noexcept {
    // 구간 0은 0개, 구간 i(i ≥ 1)는 [4^(i−1), 4^i)다.
    uint32 bucket = 0;
    uint64 bound = 1;
    while (bucket + 1 < OctreeDirectHistogramBuckets && count >= bound) {
        ++bucket;
        bound *= 4;
    }
    return bucket;
}

} // namespace

// ---- LooseOctreeReader ----

Result<BufferQueryOutcome>
LooseOctreeReader::QueryBoxInto(const Box<double>& box, uint32 filterMask,
                                Span<SpatialProxyHit> out) const noexcept {
    if (!box.IsValid()) {
        return Result<BufferQueryOutcome>::Failure(ErrorCode::InvalidArgument);
    }
    BufferQueryOutcome outcome;
    const auto sink = [&](const SpatialProxyHit& hit) noexcept {
        if (outcome.total < out.Size()) {
            out[static_cast<usize>(outcome.total)] = hit;
            ++outcome.written;
        }
        ++outcome.total;
        return QueryControl::Continue;
    };
    tree_->VisitBox(box, filterMask, sink, nullptr);
    outcome.truncated = outcome.total > out.Size();
    return Result<BufferQueryOutcome>::Success(outcome);
}

Result<bool> LooseOctreeReader::AnyInBox(const Box<double>& box, uint32 filterMask) const noexcept {
    if (!box.IsValid()) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }
    const auto sink = [](const SpatialProxyHit&) noexcept { return QueryControl::Stop; };
    return Result<bool>::Success(tree_->VisitBox(box, filterMask, sink, nullptr).stopped);
}

Result<uint64> LooseOctreeReader::CountInBox(const Box<double>& box,
                                             uint32 filterMask) const noexcept {
    if (!box.IsValid()) {
        return Result<uint64>::Failure(ErrorCode::InvalidArgument);
    }
    uint64 count = 0;
    const auto sink = [&](const SpatialProxyHit&) noexcept {
        ++count;
        return QueryControl::Continue;
    };
    tree_->VisitBox(box, filterMask, sink, nullptr);
    return Result<uint64>::Success(count);
}

bool LooseOctreeReader::IsValid(SpatialProxyHandle handle) const noexcept {
    return tree_->IsValidHandle(handle);
}

Result<SpatialProxyDesc> LooseOctreeReader::TryGet(SpatialProxyHandle handle) const noexcept {
    return tree_->TryGetProxy(handle);
}

uint32 LooseOctreeReader::Count() const noexcept {
    return tree_->ProxyCount();
}

OctreeStats LooseOctreeReader::Stats() const noexcept {
    return tree_->CollectStats();
}

Result<void> LooseOctreeReader::Validate(ValidationFailure* failure) const noexcept {
    return tree_->ValidateStructure(failure);
}

// ---- 조회 ----

bool LooseOctree::IsValidHandle(SpatialProxyHandle handle) const noexcept {
    return !handle.IsNull() && records_.IsValid(handle.slot);
}

Result<SpatialProxyDesc> LooseOctree::TryGetProxy(SpatialProxyHandle handle) const noexcept {
    auto checked = CheckHandle(handle);
    if (!checked) {
        return Result<SpatialProxyDesc>::Failure(checked.Error());
    }
    const detail::ProxyRecord& record = records_[handle.slot];
    SpatialProxyDesc desc;
    desc.bounds = record.bounds;
    desc.filterMask = record.filter;
    desc.userValue = record.userValue;
    return Result<SpatialProxyDesc>::Success(desc);
}

uint32 LooseOctree::ProxyCount() const noexcept {
    return static_cast<uint32>(records_.Size());
}

SpatialNodeInfo LooseOctree::DescribeNode(uint32 node, uint32 depth) const noexcept {
    const detail::SearchNode& search = Search(node);
    const detail::ManageNode& manage = Manage(node);
    SpatialNodeInfo info;
    info.node = node;
    info.parent = ParentOf(node);
    info.depth = depth;
    info.level = manage.level;
    info.cell = CellBoundsOf(manage.level, manage.origin);
    info.loose = LooseBoundsOf(manage.level, manage.origin);
    info.directCount = search.directCount;
    info.subtreeCount = search.subtreeCount;
    info.leaf = search.childBlock == detail::NoBlock;
    info.held = (manage.flags & detail::NodeHeld) != 0;
    return info;
}

OctreeStats LooseOctree::CollectStats() const noexcept {
    OctreeStats stats;
    stats.proxyCount = static_cast<uint32>(records_.Size());
    stats.outOfRangeCount = static_cast<uint32>(outOfRange_.Size());
    stats.rootLevel = rootManage_.level;

    struct Pending {
        uint32 node;
        uint32 depth;
    };
    InlineArray<Pending, LooseOctreeSearchStackCapacity> stack;
    stack.PushBack(Pending{detail::RootNode, 1});
    while (!stack.IsEmpty()) {
        const Pending current = stack.PopBack();
        const detail::SearchNode& search = Search(current.node);
        const detail::ManageNode& manage = Manage(current.node);
        ++stats.nodeCount;
        stats.maxDepth = current.depth > stats.maxDepth ? current.depth : stats.maxDepth;
        stats.maxDirectCount =
            search.directCount > stats.maxDirectCount ? search.directCount : stats.maxDirectCount;
        ++stats.directHistogram[HistogramBucket(search.directCount)];
        if ((manage.flags & detail::NodeHeld) != 0) {
            ++stats.heldNodeCount;
        }
        if (search.directCount > config_.parentWarnCount) {
            ++stats.overParentWarnNodeCount;
        }
        if (search.childBlock == detail::NoBlock) {
            ++stats.leafCount;
            continue;
        }
        stats.interiorDirectCount += search.directCount;
        for (uint32 child = 8; child != 0; --child) {
            stack.PushBack(Pending{ChildNode(search.childBlock, child - 1), current.depth + 1});
        }
    }

    // 메모리는 원소 크기 × 개수다. 컨테이너 자체의 머리 정보와 SlotArray slot의 세대·연결 칸(8
    // byte)은 넣지 않는다.
    stats.recordBytesUsed = records_.Size() * sizeof(detail::ProxyRecord);
    stats.recordBytesReserved = records_.Capacity() * sizeof(detail::ProxyRecord);
    stats.chunkBytesUsed = static_cast<usize>(chunks_.LiveCount()) * sizeof(detail::ElementChunk);
    stats.chunkBytesReserved = chunks_.Capacity() * sizeof(detail::ElementChunk);
    const usize blockBytes = sizeof(detail::SearchBlock) + sizeof(detail::ManageBlock);
    stats.nodeBytesUsed = static_cast<usize>(searchBlocks_.LiveCount()) * blockBytes;
    stats.nodeBytesReserved = searchBlocks_.Capacity() * sizeof(detail::SearchBlock) +
                              manageBlocks_.Capacity() * sizeof(detail::ManageBlock);
    stats.outOfRangeBytesUsed = outOfRange_.Size() * sizeof(detail::OutOfRangeEntry);
    stats.outOfRangeBytesReserved = outOfRange_.Capacity() * sizeof(detail::OutOfRangeEntry);
    stats.freeChunkCount = chunks_.FreeCount();
    stats.freeBlockCount = searchBlocks_.FreeCount();

    stats.splits = counters_.splits;
    stats.chainSplits = counters_.chainSplits;
    stats.merges = counters_.merges;
    stats.reclaims = counters_.reclaims;
    stats.reinserts = counters_.reinserts;
    stats.inPlaceUpdates = counters_.inPlaceUpdates;
    stats.relocatedProxies = counters_.relocatedProxies;
    stats.rootExpansions = counters_.rootExpansions;
    stats.rootShrinks = counters_.rootShrinks;
    stats.emptyResets = counters_.emptyResets;
    stats.splitEvaluations = counters_.splitEvaluations;
    stats.holds = counters_.holds;
    stats.reevaluations = counters_.reevaluations;
    stats.forcedMaintenanceRuns = counters_.forcedMaintenanceRuns;
    stats.forcedMaintenanceItems = counters_.forcedMaintenanceItems;
    stats.parentWarnEvents = counters_.parentWarnEvents;
    stats.outOfRangeWarnEvents = counters_.outOfRangeWarnEvents;
    stats.outOfRangeInsertions = counters_.outOfRangeInsertions;

    stats.pendingTotal = queueLength_;
    for (uint32 node = queueHead_; node != detail::NoNode; node = Manage(node).queueNext) {
        const uint8 flags = Manage(node).flags;
        if ((flags & detail::NodeReevaluationPending) != 0) {
            ++stats.pendingReevaluations;
        }
        if ((flags & detail::NodeRelocationPending) != 0) {
            ++stats.pendingRelocations;
        }
        if ((flags & (detail::NodeReevaluationPending | detail::NodeRelocationPending)) == 0) {
            ++stats.pendingStructural;
        }
    }
    return stats;
}

// ---- 검증(6.2절 불변 조건 1–9) ----

Result<void> LooseOctree::ValidateStructure(ValidationFailure* failure) const noexcept {
    const auto fail = [failure](uint32 invariant, uint32 node) noexcept {
        if (failure != nullptr) {
            failure->invariant = invariant;
            failure->node = node;
        }
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    };

    // 각 항목은 부모에서 유도한 위치를 함께 싣는다(불변 조건 8).
    struct Pending {
        int64 origin[3];
        uint32 node;
        uint32 level;
    };
    InlineArray<Pending, LooseOctreeSearchStackCapacity> stack;
    Pending root;
    root.origin[0] = rootManage_.origin[0];
    root.origin[1] = rootManage_.origin[1];
    root.origin[2] = rootManage_.origin[2];
    root.node = detail::RootNode;
    root.level = rootManage_.level;
    if (root.level > config_.maxRootLevel) {
        return fail(8, detail::RootNode);
    }
    stack.PushBack(root);

    uint64 proxiesSeen = 0;
    uint32 blocksSeen = 0;
    uint32 chunksSeen = 0;
    uint32 queuedSeen = 0;
    while (!stack.IsEmpty()) {
        const Pending current = stack.PopBack();
        const uint32 node = current.node;
        const detail::SearchNode& search = Search(node);
        const detail::ManageNode& manage = Manage(node);
        if (manage.level != current.level || manage.origin[0] != current.origin[0] ||
            manage.origin[1] != current.origin[1] || manage.origin[2] != current.origin[2]) {
            return fail(8, node);
        }
        if ((manage.flags & detail::NodeQueued) != 0) {
            ++queuedSeen;
        }
        const Box<double> loose = LooseBoundsOf(manage.level, manage.origin);
        const Vec3d center = CenterOf(manage.level, manage.origin);

        // 직접 저장 프록시와 청크 목록(조밀성·연결·사용 수).
        uint32 direct = 0;
        uint32 previous = detail::NoChunk;
        for (uint32 chunk = search.firstChunk; chunk != detail::NoChunk;) {
            if (!chunks_.IsLive(chunk)) {
                return fail(0, node);
            }
            const detail::ElementChunk& elements = chunks_[chunk];
            if (elements.previous != previous || elements.count == 0 ||
                elements.count > LooseOctreeChunkCapacity ||
                (elements.next != detail::NoChunk && elements.count != LooseOctreeChunkCapacity)) {
                return fail(0, node);
            }
            ++chunksSeen;
            if (chunksSeen > chunks_.LiveCount()) {
                return fail(0, node);
            }
            for (uint32 entry = 0; entry < elements.count; ++entry) {
                const uint32 slot = elements.slot[entry];
                const SlotHandle current = records_.HandleAt(slot);
                if (current.IsNull() || current.generation != elements.generation[entry]) {
                    return fail(3, node);
                }
                const detail::ProxyRecord& record = records_.ElementAt(slot);
                if (record.node != node || record.chunk != chunk || record.entry != entry) {
                    return fail(3, node);
                }
                if (!loose.Contains(record.bounds)) {
                    return fail(1, node);
                }
                if (record.filter != elements.filter[entry] ||
                    record.userValue != elements.userValue[entry]) {
                    return fail(0, node);
                }
                // 불변 조건 7: min_f < (min − 중심), max_f > (max − 중심)(double 계산값 기준,
                // 엄격).
                const Vec3d relativeMin = record.bounds.min - center;
                const Vec3d relativeMax = record.bounds.max - center;
                if (!(static_cast<double>(elements.minX[entry]) < relativeMin.x) ||
                    !(static_cast<double>(elements.minY[entry]) < relativeMin.y) ||
                    !(static_cast<double>(elements.minZ[entry]) < relativeMin.z) ||
                    !(static_cast<double>(elements.maxX[entry]) > relativeMax.x) ||
                    !(static_cast<double>(elements.maxY[entry]) > relativeMax.y) ||
                    !(static_cast<double>(elements.maxZ[entry]) > relativeMax.z)) {
                    return fail(7, node);
                }
                ++direct;
                ++proxiesSeen;
            }
            previous = chunk;
            chunk = elements.next;
        }
        if (direct != search.directCount || manage.lastChunk != previous) {
            return fail(0, node);
        }

        if (search.childBlock == detail::NoBlock) {
            if (search.subtreeCount != search.directCount) {
                return fail(5, node);
            }
            if (manage.childMask != 0) {
                return fail(6, node);
            }
            continue;
        }

        // 잎이 아닌 노드는 자식 블록을 갖고 그 부모 링크는 이 노드를 가리킨다(불변 조건 8).
        const uint32 block = search.childBlock;
        if (current.level == 0 || !searchBlocks_.IsLive(block) || !manageBlocks_.IsLive(block) ||
            manageBlocks_[block].parent != node) {
            return fail(8, node);
        }
        ++blocksSeen;
        if (blocksSeen > searchBlocks_.LiveCount()) {
            return fail(8, node);
        }
        uint64 childSum = 0;
        uint8 mask = 0;
        for (uint32 child = 0; child < 8; ++child) {
            const uint32 childNode = ChildNode(block, child);
            Pending next;
            ChildOrigin(current.level, current.origin, child, next.origin);
            next.node = childNode;
            next.level = current.level - 1;
            // 불변 조건 2: 자식의 확장 경계는 부모의 확장 경계에 포함된다.
            if (!loose.Contains(LooseBoundsOf(next.level, next.origin))) {
                return fail(2, childNode);
            }
            const uint32 childSubtree = Search(childNode).subtreeCount;
            childSum += childSubtree;
            if (childSubtree != 0) {
                mask |= static_cast<uint8>(1u << child);
            }
            stack.PushBack(next);
        }
        if (static_cast<uint64>(search.subtreeCount) != search.directCount + childSum) {
            return fail(5, node);
        }
        if (manage.childMask != mask) {
            return fail(6, node);
        }
    }

    // 도달할 수 있는 블록·청크가 풀의 사용량과 같다(누수·이중 소속 없음).
    if (blocksSeen != searchBlocks_.LiveCount() || blocksSeen != manageBlocks_.LiveCount() ||
        chunksSeen != chunks_.LiveCount()) {
        return fail(0, detail::NoNode);
    }

    // 범위 밖 목록.
    for (usize index = 0; index < outOfRange_.Size(); ++index) {
        const detail::OutOfRangeEntry& entry = outOfRange_[index];
        const SlotHandle current = records_.HandleAt(entry.slot);
        if (current.IsNull() || current.generation != entry.generation) {
            return fail(3, detail::NoNode);
        }
        const detail::ProxyRecord& record = records_.ElementAt(entry.slot);
        if (record.node != detail::OutOfRangeNode || record.chunk != index ||
            !(record.bounds.min == entry.bounds.min) || !(record.bounds.max == entry.bounds.max) ||
            record.filter != entry.filter || record.userValue != entry.userValue) {
            return fail(3, detail::NoNode);
        }
        ++proxiesSeen;
    }

    // 불변 조건 4: 살아 있는 프록시는 정확히 한 곳에 있다. 위치마다 레코드가 그 위치를
    // 가리키는지(3) 확인했으므로 수가 같으면 일대일이다.
    if (proxiesSeen != records_.Size()) {
        return fail(4, detail::NoNode);
    }

    // 불변 조건 9: 대기열 항목은 살아 있는 노드를 가리키고 노드당 한 번만 있다.
    uint32 length = 0;
    uint32 previous = detail::NoNode;
    for (uint32 node = queueHead_; node != detail::NoNode;) {
        if (length >= queueLength_) {
            return fail(9, node);
        }
        if (node != detail::RootNode && node != detail::OutOfRangeNode &&
            (!searchBlocks_.IsLive(BlockOf(node)) || !manageBlocks_.IsLive(BlockOf(node)))) {
            return fail(9, node);
        }
        if (node == detail::OutOfRangeNode) {
            return fail(9, node);
        }
        const detail::ManageNode& manage = Manage(node);
        if ((manage.flags & detail::NodeQueued) == 0 || manage.queuePrevious != previous) {
            return fail(9, node);
        }
        ++length;
        previous = node;
        node = manage.queueNext;
    }
    if (previous != queueTail_ || length != queueLength_ || length != queuedSeen) {
        return fail(9, detail::NoNode);
    }
    return Result<void>::Success();
}

} // namespace rocks::spatial
