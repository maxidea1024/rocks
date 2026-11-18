#include "LooseOctreeInternal.h"

#include <Core/Math/MathUtility.h>
#include <Platform/PlatformMath.h>

namespace rocks::spatial {

namespace {

// 분할 평가 한 번이 다루는 단일 자식 체인과 그 끝 노드의 위치다. 체인 노드 j(0이 평가 노드)는
// level − j에 있고 chainChild[j]가 다음 체인 노드로 가는 자식 번호다.
struct ChainPlan {
    uint32 length = 0;
    uint32 chainChild[LooseOctreeLevelLimit + 1] = {};
    int64 origins[LooseOctreeLevelLimit + 2][3] = {};
};

} // namespace

void LooseOctree::Hold(uint32 node) noexcept {
    detail::ManageNode& manage = Manage(node);
    const uint32 direct = Search(node).directCount;
    ++counters_.holds;
    if ((manage.flags & detail::NodeHeld) == 0) {
        // 처음 보류: 그 시점의 직접 저장 수 n0와 재평가 간격 n0 / 2를 기록한다(6.4절 보류 상태).
        manage.flags |= detail::NodeHeld;
        manage.reevaluationInterval = direct / 2 != 0 ? direct / 2 : 1;
    } else {
        // 재평가가 다시 보류로 끝났다: 간격을 두 배로(상한 n0 · 2^8) 늘려 같은 분석의 반복을
        // 상각한다.
        const uint64 doubled = static_cast<uint64>(manage.reevaluationInterval) * 2;
        const uint64 cap = static_cast<uint64>(direct != 0 ? direct : 1) * 256;
        const uint64 interval = doubled < cap ? doubled : cap;
        manage.reevaluationInterval = interval > NumericLimits<uint32>::Max()
                                          ? NumericLimits<uint32>::Max()
                                          : static_cast<uint32>(interval);
    }
    manage.heldCount = direct;
    manage.changesSinceHold = 0;
    manage.flags &= static_cast<uint8>(~detail::NodeReevaluationPending);
}

void LooseOctree::ReleaseHold(uint32 node) noexcept {
    detail::ManageNode& manage = Manage(node);
    manage.flags &= static_cast<uint8>(~(detail::NodeHeld | detail::NodeReevaluationPending));
    manage.heldCount = 0;
    manage.changesSinceHold = 0;
    manage.reevaluationInterval = 0;
}

void LooseOctree::EvaluateSplits(uint32 node) noexcept {
    // 분할로 생긴 자식이 splitThreshold를 넘으면 같은 호출 안에서 이어서 평가한다. 꺼낸 노드마다 더
    // 깊은 자식을 최대 8개 쌓으므로 깊이 상한에서 유도한 검색 스택 용량을 넘지 않는다.
    InlineArray<uint32, LooseOctreeSearchStackCapacity> pending;
    pending.PushBack(node);
    bool first = true;
    while (!pending.IsEmpty()) {
        const uint32 current = pending.PopBack();
        const detail::SearchNode& search = Search(current);
        if (search.childBlock != detail::NoBlock) {
            continue;
        }
        // 첫 노드는 호출자가 평가 시점을 정했다(보류 중 재평가 포함). 이어지는 자식은 임계를 넘을
        // 때만.
        if (!first && (search.directCount <= config_.splitThreshold ||
                       (Manage(current).flags & detail::NodeHeld) != 0)) {
            continue;
        }
        first = false;
        TrySplitOnce(current, pending);
    }
}

bool LooseOctree::TrySplitOnce(
    uint32 node, InlineArray<uint32, LooseOctreeSearchStackCapacity>& cascade) noexcept {
    ++counters_.splitEvaluations;
    const uint32 level = Manage(node).level;
    const uint32 halfThreshold = config_.splitThreshold / 2;
    // 판정 1: 자식 기본 셀이 minCellSize보다 작아지면 보류한다.
    if (level == 0) {
        Hold(node);
        return false;
    }

    ChainPlan chain;
    for (uint32 axis = 0; axis < 3; ++axis) {
        chain.origins[0][axis] = Manage(node).origin[axis];
    }
    const Vec3d nodeCenter = CenterOf(level, chain.origins[0]);
    Box<double> childLoose[8];
    for (uint32 child = 0; child < 8; ++child) {
        int64 origin[3];
        ChildOrigin(level, chain.origins[0], child, origin);
        childLoose[child] = LooseBoundsOf(level - 1, origin);
    }

    // 1차 계산(할당 없음, O(직접 저장 수)): 하강 수 d, 자식별 분포, 하강할 프록시의 최대 반 변 e,
    // 중심의 level 0 정수 셀 좌표(노드 원점 기준, 노드 셀 안으로 자름) 범위.
    uint32 descending = 0;
    uint32 perChild[8] = {};
    double maxHalfExtent = 0.0;
    int64 cellMinimum[3] = {NumericLimits<int64>::Max(), NumericLimits<int64>::Max(),
                            NumericLimits<int64>::Max()};
    int64 cellMaximum[3] = {NumericLimits<int64>::Min(), NumericLimits<int64>::Min(),
                            NumericLimits<int64>::Min()};
    Vec3d representative;
    const int64 cellsPerSide = int64{1} << level;
    const Box<double> nodeCell = CellBoundsOf(level, chain.origins[0]);
    for (uint32 chunk = Search(node).firstChunk; chunk != detail::NoChunk;
         chunk = chunks_[chunk].next) {
        const detail::ElementChunk& elements = chunks_[chunk];
        for (uint32 entry = 0; entry < elements.count; ++entry) {
            const Box<double>& bounds = records_.ElementAt(elements.slot[entry]).bounds;
            const Vec3d center = BoxCenter(bounds);
            const uint32 child = ChildIndexFor(center, nodeCenter);
            if (!childLoose[child].Contains(bounds)) {
                continue;
            }
            if (descending == 0) {
                representative = center;
            }
            ++descending;
            ++perChild[child];
            const Vec3d half = bounds.HalfExtents();
            maxHalfExtent = math::Max(maxHalfExtent, math::Max(half.x, math::Max(half.y, half.z)));
            for (uint32 axis = 0; axis < 3; ++axis) {
                // 확장 경계 띠의 중심은 노드 셀 끝 칸으로 자른다. 자른 칸도 배치 규칙과 같은 쪽
                // 자식을 고른다(셀 밖 양의 쪽은 모든 level에서 양의 쪽).
                const double offset =
                    math::Floor((center[axis] - nodeCell.min[axis]) / config_.minCellSize);
                int64 cell = 0;
                if (offset >= static_cast<double>(cellsPerSide)) {
                    cell = cellsPerSide - 1;
                } else if (offset > 0.0) {
                    cell = static_cast<int64>(offset);
                }
                cellMinimum[axis] = cellMinimum[axis] < cell ? cellMinimum[axis] : cell;
                cellMaximum[axis] = cellMaximum[axis] > cell ? cellMaximum[axis] : cell;
            }
        }
    }

    // 판정 2: 대부분이 부모에 남는 분할은 순회 비용만 늘린다.
    if (descending < halfThreshold) {
        Hold(node);
        return false;
    }
    uint32 occupied = 0;
    for (uint32 child = 0; child < 8; ++child) {
        occupied += perChild[child] != 0 ? 1u : 0u;
    }

    // 판정 3·4: 둘 이상으로 나뉘면 한 level만, 모두 한 자식으로 가면 실제 분리 level ℓ_sep을 구해
    // 그 위 level ℓ_sep + 1의 노드(목표 T)까지 단일 자식 체인을 만든다.
    uint32 targetLevel = level;
    if (occupied < 2) {
        uint64 differing = 0;
        for (uint32 axis = 0; axis < 3; ++axis) {
            differing |= static_cast<uint64>(cellMinimum[axis] ^ cellMaximum[axis]);
        }
        if (differing == 0) {
            // 모든 중심이 같은 level 0 셀에 있다: 분리 level이 없다.
            Hold(node);
            return false;
        }
        const uint32 separation = 63 - platform::PlatformMath::CountLeadingZeros64(differing);
        // 하강 보장 level ℓ_fit: L_ℓ ≥ 2e / (k − 1)인 가장 낮은 ℓ. k = 1이면 크기 보장이 없으므로
        // 0으로 두고 아래의 재계수로 판정한다.
        uint32 fitLevel = 0;
        if (config_.looseness > 1.0) {
            const double required = 2.0 * maxHalfExtent / (config_.looseness - 1.0);
            while (fitLevel <= level && cellSize_[fitLevel] < required) {
                ++fitLevel;
            }
        }
        if (separation < fitLevel || separation + 1 >= level) {
            Hold(node);
            return false;
        }
        targetLevel = separation + 1;
    }

    // 체인 경로: 대표 중심을 배치 규칙으로 따라간다.
    chain.length = level - targetLevel;
    for (uint32 step = 0; step < chain.length; ++step) {
        const uint32 stepLevel = level - step;
        const Vec3d stepCenter = CenterOf(stepLevel, chain.origins[step]);
        chain.chainChild[step] = ChildIndexFor(representative, stepCenter);
        ChildOrigin(stepLevel, chain.origins[step], chain.chainChild[step],
                    chain.origins[step + 1]);
    }

    // 2차 계산(체인이 있을 때): 만들 구조에서 배치 규칙을 모의해 목표 T의 자식으로 가는 수 d′와
    // 분포를 다시 센다. 체인 중간에서 형제 잎으로 빠지는 프록시는 옮기지만 d′에 넣지 않는다.
    uint32 moved = descending;
    if (chain.length != 0) {
        uint32 targetPerChild[8] = {};
        uint32 targetDescending = 0;
        moved = 0;
        const Vec3d targetCenter = CenterOf(targetLevel, chain.origins[chain.length]);
        Box<double> targetChildLoose[8];
        for (uint32 child = 0; child < 8; ++child) {
            int64 origin[3];
            ChildOrigin(targetLevel, chain.origins[chain.length], child, origin);
            targetChildLoose[child] = LooseBoundsOf(targetLevel - 1, origin);
        }
        for (uint32 chunk = Search(node).firstChunk; chunk != detail::NoChunk;
             chunk = chunks_[chunk].next) {
            const detail::ElementChunk& elements = chunks_[chunk];
            for (uint32 entry = 0; entry < elements.count; ++entry) {
                const Box<double>& bounds = records_.ElementAt(elements.slot[entry]).bounds;
                const Vec3d center = BoxCenter(bounds);
                bool leaves = false;
                bool onChain = true;
                for (uint32 step = 0; step < chain.length && onChain; ++step) {
                    const uint32 stepLevel = level - step;
                    const uint32 child =
                        ChildIndexFor(center, CenterOf(stepLevel, chain.origins[step]));
                    int64 origin[3];
                    ChildOrigin(stepLevel, chain.origins[step], child, origin);
                    if (!LooseBoundsOf(stepLevel - 1, origin).Contains(bounds)) {
                        onChain = false;
                    } else if (child != chain.chainChild[step]) {
                        // 체인 형제 잎으로 내려간다.
                        leaves = true;
                        onChain = false;
                    } else {
                        leaves = true;
                    }
                }
                if (onChain) {
                    const uint32 child = ChildIndexFor(center, targetCenter);
                    if (targetChildLoose[child].Contains(bounds)) {
                        ++targetDescending;
                        ++targetPerChild[child];
                        leaves = true;
                    }
                }
                moved += leaves ? 1u : 0u;
            }
        }
        uint32 targetOccupied = 0;
        for (uint32 child = 0; child < 8; ++child) {
            targetOccupied += targetPerChild[child] != 0 ? 1u : 0u;
        }
        if (targetDescending < halfThreshold || targetOccupied < 2) {
            Hold(node);
            return false;
        }
    }

    // 분할 수행: 필요한 블록과 청크를 모두 먼저 확보한다. 하나라도 실패하면 분할하지 않고
    // 보류한다(삽입 호출은 성공한다). 청크 상한: 목적지마다 ⌈n / C⌉ ≤ n / C + 1이고 목적지는 T의
    // 자식 8개와 체인 level마다 형제 7개, 체인 노드다.
    const uint32 blocks = chain.length + 1;
    const uint32 destinations = 8 * (chain.length + 1);
    const uint32 chunksNeeded = moved / LooseOctreeChunkCapacity + destinations + 1;
    if (!searchBlocks_.TryReserveAdditional(blocks) ||
        !manageBlocks_.TryReserveAdditional(blocks) ||
        !chunks_.TryReserveAdditional(chunksNeeded)) {
        Hold(node);
        return false;
    }

    // 체인 블록을 만든다.
    uint32 chainNode = node;
    for (uint32 step = 0; step <= chain.length; ++step) {
        const uint32 stepLevel = level - step;
        const uint32 block = searchBlocks_.AllocateReserved();
        if (manageBlocks_.AllocateReserved() != block) {
            platform::Fatal("LooseOctree node block pools diverged");
        }
        manageBlocks_[block].parent = chainNode;
        for (uint32 child = 0; child < 8; ++child) {
            detail::ManageNode& manage = manageBlocks_[block].nodes[child];
            manage.level = static_cast<uint8>(stepLevel - 1);
            ChildOrigin(stepLevel, chain.origins[step], child, manage.origin);
        }
        Search(chainNode).childBlock = block;
        if (step < chain.length) {
            chainNode = ChildNode(block, chain.chainChild[step]);
        }
    }

    // 프록시를 옮긴다. 남는 프록시는 같은 청크 목록 앞쪽으로 순서를 유지하며 모으고(읽기 위치 ≥
    // 쓰기 위치), 내려가는 프록시는 목적지 노드에 붙이며 float 경계를 새 노드 중심 기준으로
    // double에서 다시 계산한다.
    uint32 writeChunk = Search(node).firstChunk;
    uint32 writeEntry = 0;
    uint32 lastWrittenChunk = detail::NoChunk;
    uint32 kept = 0;
    for (uint32 readChunk = Search(node).firstChunk; readChunk != detail::NoChunk;
         readChunk = chunks_[readChunk].next) {
        const uint32 readCount = chunks_[readChunk].count;
        for (uint32 readEntry = 0; readEntry < readCount; ++readEntry) {
            const uint32 slot = chunks_[readChunk].slot[readEntry];
            detail::ProxyRecord& record = records_.ElementAt(slot);
            const uint32 destination = Descend(node, record.bounds);
            if (destination == node) {
                if (writeChunk != readChunk || writeEntry != readEntry) {
                    detail::ElementChunk& from = chunks_[readChunk];
                    detail::ElementChunk& to = chunks_[writeChunk];
                    to.minX[writeEntry] = from.minX[readEntry];
                    to.minY[writeEntry] = from.minY[readEntry];
                    to.minZ[writeEntry] = from.minZ[readEntry];
                    to.maxX[writeEntry] = from.maxX[readEntry];
                    to.maxY[writeEntry] = from.maxY[readEntry];
                    to.maxZ[writeEntry] = from.maxZ[readEntry];
                    to.filter[writeEntry] = from.filter[readEntry];
                    to.slot[writeEntry] = from.slot[readEntry];
                    to.generation[writeEntry] = from.generation[readEntry];
                    to.userValue[writeEntry] = from.userValue[readEntry];
                }
                record.chunk = writeChunk;
                record.entry = writeEntry;
                lastWrittenChunk = writeChunk;
                ++kept;
                ++writeEntry;
                if (writeEntry == LooseOctreeChunkCapacity) {
                    writeChunk = chunks_[writeChunk].next;
                    writeEntry = 0;
                }
                continue;
            }
            uint32 chunk = 0;
            uint32 entry = 0;
            AppendEntry(destination, EntryOfRecord(record, records_.HandleAt(slot)), chunk, entry);
            record.node = destination;
            record.chunk = chunk;
            record.entry = entry;
            AdjustPath(destination, node, true);
        }
    }

    // 남은 프록시 뒤의 청크를 돌려준다. 마지막으로 쓴 청크의 사용 수를 고친다.
    detail::SearchNode& search = Search(node);
    detail::ManageNode& manage = Manage(node);
    uint32 release = detail::NoChunk;
    if (kept == 0) {
        release = search.firstChunk;
        search.firstChunk = detail::NoChunk;
        manage.lastChunk = detail::NoChunk;
    } else {
        detail::ElementChunk& last = chunks_[lastWrittenChunk];
        last.count = kept - (kept - 1) / LooseOctreeChunkCapacity * LooseOctreeChunkCapacity;
        release = last.next;
        last.next = detail::NoChunk;
        manage.lastChunk = lastWrittenChunk;
    }
    while (release != detail::NoChunk) {
        const uint32 next = chunks_[release].next;
        chunks_.Free(release);
        release = next;
    }
    search.directCount = kept;
    if (kept <= config_.parentWarnCount / 2) {
        manage.flags &= static_cast<uint8>(~detail::NodeWarned);
    }
    ReleaseHold(node);
    ++counters_.splits;
    if (chain.length != 0) {
        ++counters_.chainSplits;
    }

    // 목표 T의 자식과 체인 형제 중 임계를 넘는 잎을 이어서 평가한다. 스택이 가득 차면 다음 삽입이
    // 평가한다(정확성과 무관).
    uint32 parentNode = node;
    for (uint32 step = 0; step <= chain.length; ++step) {
        const uint32 block = Search(parentNode).childBlock;
        for (uint32 child = 0; child < 8; ++child) {
            const uint32 candidate = ChildNode(block, child);
            const detail::SearchNode& candidateSearch = Search(candidate);
            if (candidateSearch.childBlock == detail::NoBlock &&
                candidateSearch.directCount > config_.splitThreshold) {
                (void)cascade.TryPushBack(candidate);
            }
        }
        if (step < chain.length) {
            parentNode = ChildNode(block, chain.chainChild[step]);
        }
    }
    return true;
}

} // namespace rocks::spatial
