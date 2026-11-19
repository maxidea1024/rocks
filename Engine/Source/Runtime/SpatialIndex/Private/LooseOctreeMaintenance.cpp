#include "LooseOctreeInternal.h"

#include <Platform/Clock.h>

namespace rocks::spatial {

namespace {

// 시간 예산의 시계 확인 간격(항목 수)이다(6.6절 예산).
constexpr uint32 ClockCheckInterval = 16;

// 루트 축소 이력: 조건이 이 횟수만큼 연속으로 관측되어야 한 단계 줄인다(6.5절 루트 축소).
constexpr uint32 RootShrinkObservationsRequired = 4;

uint32 PopCount8(uint8 mask) noexcept {
    uint32 count = 0;
    for (uint32 bit = 0; bit < 8; ++bit) {
        count += (mask >> bit) & 1u;
    }
    return count;
}

} // namespace

// ---- 대기열(노드 관리 데이터 안의 이중 연결 목록) ----

void LooseOctree::Enqueue(uint32 node) noexcept {
    detail::ManageNode& manage = Manage(node);
    if ((manage.flags & detail::NodeQueued) != 0) {
        return;
    }
    manage.flags |= detail::NodeQueued;
    manage.queuePrevious = queueTail_;
    manage.queueNext = detail::NoNode;
    if (queueTail_ != detail::NoNode) {
        Manage(queueTail_).queueNext = node;
    } else {
        queueHead_ = node;
    }
    queueTail_ = node;
    ++queueLength_;
}

void LooseOctree::Unlink(uint32 node) noexcept {
    detail::ManageNode& manage = Manage(node);
    if ((manage.flags & detail::NodeQueued) == 0) {
        return;
    }
    if (manage.queuePrevious != detail::NoNode) {
        Manage(manage.queuePrevious).queueNext = manage.queueNext;
    } else {
        queueHead_ = manage.queueNext;
    }
    if (manage.queueNext != detail::NoNode) {
        Manage(manage.queueNext).queuePrevious = manage.queuePrevious;
    } else {
        queueTail_ = manage.queuePrevious;
    }
    manage.queuePrevious = detail::NoNode;
    manage.queueNext = detail::NoNode;
    manage.flags &= static_cast<uint8>(~detail::NodeQueued);
    --queueLength_;
}

uint32 LooseOctree::PendingLimit() const noexcept {
    const uint64 nodes = 1 + static_cast<uint64>(searchBlocks_.LiveCount()) * 8;
    const uint64 quarter = nodes / 4;
    return quarter > config_.pendingLimitFloor ? static_cast<uint32>(quarter)
                                               : config_.pendingLimitFloor;
}

void LooseOctree::EnforcePendingLimit() noexcept {
    // 누적 방지: 대기량이 한도를 넘으면 한도 아래로 내려갈 만큼을 그 자리에서 작업 수 예산으로
    // 처리한다. 항목 하나의 비용이 묶여 있으므로 추가 지연은 (초과량 × 항목 비용 상한)이다(6.6절).
    const uint32 limit = PendingLimit();
    if (queueLength_ <= limit) {
        return;
    }
    MaintenanceReport report;
    const uint32 processed = ProcessQueue(queueLength_ - limit, 0, report);
    ++counters_.forcedMaintenanceRuns;
    counters_.forcedMaintenanceItems += processed;
}

// ---- 유지보수(6.6절) ----

MaintenanceReport LooseOctree::RunMaintenance(const MaintenanceBudget& budget) noexcept {
    WriterGuard guard(*this);
    MaintenanceReport report;
    const uint64 start = platform::MonotonicNanoseconds();
    // 작업 수·시간 예산이 모두 0이면 대기열 처리와 루트 확인을 미룬다. 축소 요청은 그와 별개로
    // 수행한다.
    if (budget.operations != 0 || budget.timeNanoseconds != 0) {
        const uint32 maxItems =
            budget.operations != 0 ? budget.operations : NumericLimits<uint32>::Max();
        // 0은 "시간 한도 없음"이다. 덧셈은 포화시켜 아주 큰 예산이 과거 시각으로 돌지 않게 한다.
        uint64 deadline = 0;
        if (budget.timeNanoseconds != 0) {
            deadline = budget.timeNanoseconds > NumericLimits<uint64>::Max() - start
                           ? NumericLimits<uint64>::Max()
                           : start + budget.timeNanoseconds;
        }
        ProcessQueue(maxItems, deadline, report);
        CheckRoot(report);
    }
    if (budget.shrink) {
        // SlotArray는 handle 세대 기록을 지키려고 저장소를 줄이지 않는다(옛 handle이 다시
        // 유효해지는 것을 막는다). 나머지 배열과 풀만 줄인다.
        const bool chunks = static_cast<bool>(chunks_.TryShrink());
        const bool search = static_cast<bool>(searchBlocks_.TryShrink());
        const bool manage = static_cast<bool>(manageBlocks_.TryShrink());
        const bool outOfRange = static_cast<bool>(outOfRange_.TryShrink());
        const bool large = static_cast<bool>(large_.TryShrink());
        report.shrunk = chunks && search && manage && outOfRange && large;
    }
    NotePeaks();
    report.remaining = queueLength_;
    report.elapsedNanoseconds = platform::MonotonicNanoseconds() - start;
    return report;
}

uint32 LooseOctree::ProcessQueue(uint32 maxItems, uint64 deadline,
                                 MaintenanceReport& report) noexcept {
    uint32 processed = 0;
    while (queueHead_ != detail::NoNode && processed < maxItems) {
        if (deadline != 0 && processed != 0 && processed % ClockCheckInterval == 0 &&
            platform::MonotonicNanoseconds() >= deadline) {
            break;
        }
        const uint32 node = queueHead_;
        Unlink(node);
        ++processed;
        ++report.processed;
        // 청크 확보 실패로 항목을 다시 대기시켰으면 이번 실행을 멈춘다. 같은 실패를 예산만큼
        // 되풀이하지 않는다(다음 유지보수가 다시 시도한다).
        if (!ProcessItem(node, report)) {
            break;
        }
    }
    return processed;
}

bool LooseOctree::ProcessItem(uint32 node, MaintenanceReport& report) noexcept {
    // 처리 시점에 조건을 다시 확인하고, 사라진 조건은 버린다.
    if (ShouldCollapse(node)) {
        const CollapseOutcome outcome = TryCollapseChildren(node, report);
        if (outcome != CollapseOutcome::Collapsed) {
            return outcome != CollapseOutcome::Stalled;
        }
    }
    detail::ManageNode& manage = Manage(node);
    const bool leaf = Search(node).childBlock == detail::NoBlock;
    if ((manage.flags & detail::NodeReevaluationPending) != 0) {
        manage.flags &= static_cast<uint8>(~detail::NodeReevaluationPending);
        // 보류 중인 잎의 재평가, 또는 경계 갱신이 미룬 분할 평가다.
        if (leaf) {
            if (Search(node).directCount > config_.splitThreshold) {
                ++counters_.reevaluations;
                ++report.reevaluations;
                EvaluateSplits(node);
            } else {
                ReleaseHold(node);
            }
        }
    }
    detail::ManageNode& after = Manage(node);
    if ((after.flags & detail::NodeRelocationPending) != 0) {
        after.flags &= static_cast<uint8>(~detail::NodeRelocationPending);
        if (Search(node).childBlock != detail::NoBlock) {
            return RelocateDown(node, report);
        }
    }
    return true;
}

LooseOctree::CollapseOutcome LooseOctree::TryCollapseChildren(uint32 node,
                                                              MaintenanceReport& report) noexcept {
    const uint32 block = Search(node).childBlock;
    // 아래에서 위로만 진행한다. 잎이 아닌 자식이 있으면 그 자식을 먼저 처리하게 등록하고 이번
    // 항목은 다시 대기시킨다. 그래서 항목 하나의 비용은 블록 하나와 프록시 mergeThreshold개로
    // 묶인다.
    bool deferred = false;
    for (uint32 child = 0; child < 8; ++child) {
        const uint32 childNode = ChildNode(block, child);
        if (Search(childNode).childBlock != detail::NoBlock) {
            Enqueue(childNode);
            deferred = true;
        }
    }
    if (deferred) {
        Enqueue(node);
        return CollapseOutcome::Deferred;
    }

    detail::SearchNode& search = Search(node);
    const uint32 moving = search.subtreeCount - search.directCount;
    if (moving == 0) {
        // 빈 블록 회수: 자식 8개가 모두 빈 잎이다.
        FreeBlock(block);
        Search(node).childBlock = detail::NoBlock;
        Manage(node).childMask = 0;
        ++counters_.reclaims;
        ++report.reclaims;
    } else {
        // 병합: 자식의 프록시를 이 노드로 모은다. 자식의 확장 경계는 이 노드의 것에 포함되므로(불변
        // 조건 2) 모은 프록시도 불변 조건 1을 지킨다. 청크를 확보하지 못하면 건너뛰고 다시
        // 대기시킨다.
        const uint32 chunksNeeded = moving / LooseOctreeChunkCapacity + 2;
        if (!chunks_.TryReserveAdditional(chunksNeeded)) {
            Enqueue(node);
            return CollapseOutcome::Stalled;
        }
        for (uint32 child = 0; child < 8; ++child) {
            const uint32 childNode = ChildNode(block, child);
            uint32 chunk = Search(childNode).firstChunk;
            while (chunk != detail::NoChunk) {
                const uint32 next = chunks_[chunk].next;
                const uint32 count = chunks_[chunk].count;
                for (uint32 entry = 0; entry < count; ++entry) {
                    const uint32 slot = chunks_[chunk].slot[entry];
                    detail::ProxyRecord& record = records_.ElementAt(slot);
                    uint32 newChunk = 0;
                    uint32 newEntry = 0;
                    AppendEntry(node, EntryOfRecord(record, records_.HandleAt(slot)), newChunk,
                                newEntry);
                    record.node = node;
                    record.chunk = newChunk;
                    record.entry = newEntry;
                }
                chunks_.Free(chunk);
                chunk = next;
            }
            detail::SearchNode& childSearch = Search(childNode);
            childSearch.firstChunk = detail::NoChunk;
            childSearch.directCount = 0;
            childSearch.subtreeCount = 0;
            Manage(childNode).lastChunk = detail::NoChunk;
        }
        FreeBlock(block);
        Search(node).childBlock = detail::NoBlock;
        Manage(node).childMask = 0;
        ++counters_.merges;
        ++report.merges;
    }
    // 잎이 되었으므로 재배치 표시는 의미가 없다.
    Manage(node).flags &= static_cast<uint8>(~detail::NodeRelocationPending);
    const uint32 parent = ParentOf(node);
    if (parent != detail::NoNode && ShouldCollapse(parent)) {
        Enqueue(parent);
    }
    return CollapseOutcome::Collapsed;
}

bool LooseOctree::RelocateDown(uint32 node, MaintenanceReport& report) noexcept {
    // 직접 저장 프록시 중 자식에 들어가는 것을 가장 깊은 노드까지 내린다. 제거는 마지막 프록시를 빈
    // 자리로 옮기므로(swap-remove) 옮긴 자리를 다시 검사한다.
    uint32 relocated = 0;
    uint32 chunk = Search(node).firstChunk;
    uint32 entry = 0;
    bool complete = true;
    while (chunk != detail::NoChunk) {
        if (entry >= chunks_[chunk].count) {
            chunk = chunks_[chunk].next;
            entry = 0;
            continue;
        }
        const uint32 slot = chunks_[chunk].slot[entry];
        detail::ProxyRecord& record = records_.ElementAt(slot);
        const uint32 destination = Descend(node, record.bounds);
        if (destination == node) {
            ++entry;
            continue;
        }
        if (!chunks_.TryReserveAdditional(1)) {
            complete = false;
            break;
        }
        const bool last = chunk == Manage(node).lastChunk && entry + 1 == chunks_[chunk].count;
        const EntryData data = EntryOfRecord(record, records_.HandleAt(slot));
        RemoveEntry(node, chunk, entry);
        uint32 newChunk = 0;
        uint32 newEntry = 0;
        AppendEntry(destination, data, newChunk, newEntry);
        record.node = destination;
        record.chunk = newChunk;
        record.entry = newEntry;
        AdjustPath(destination, node, true);
        ++relocated;
        // 내려간 잎이 임계를 넘으면 평가한다. 분할은 이 노드의 청크를 건드리지 않는다.
        const detail::SearchNode& destinationSearch = Search(destination);
        if (destinationSearch.childBlock == detail::NoBlock &&
            destinationSearch.directCount > config_.splitThreshold &&
            (Manage(destination).flags & detail::NodeHeld) == 0) {
            EvaluateSplits(destination);
        }
        if (last) {
            break;
        }
    }
    counters_.relocatedProxies += relocated;
    report.relocatedProxies += relocated;
    if (!complete) {
        Manage(node).flags |= detail::NodeRelocationPending;
        Enqueue(node);
    }
    return complete;
}

void LooseOctree::CheckRoot(MaintenanceReport& report) noexcept {
    if (rootSearch_.subtreeCount == 0) {
        // 빈 트리: 루트를 initialRootLevel로 되돌리고 모든 블록을 해제한다.
        const bool fresh = rootSearch_.childBlock == detail::NoBlock &&
                           rootManage_.level == config_.initialRootLevel &&
                           rootManage_.origin[0] == initialOrigin_ &&
                           rootManage_.origin[1] == initialOrigin_ &&
                           rootManage_.origin[2] == initialOrigin_;
        if (!fresh) {
            FreeAllStructure();
            const int64 origin[3] = {initialOrigin_, initialOrigin_, initialOrigin_};
            ResetRoot(config_.initialRootLevel, origin);
            ++counters_.emptyResets;
            report.emptyReset = true;
        }
        rootShrinkObservations_ = 0;
        return;
    }
    // 루트에 직접 저장된 프록시가 있으면 축소하지 않는다(그 프록시들은 자식의 확장 경계에 들어가지
    // 않아 루트에 있으므로 축소하면 불변 조건 1이 깨진다).
    const bool shrinkable =
        rootManage_.level > config_.initialRootLevel && rootSearch_.directCount == 0 &&
        rootSearch_.childBlock != detail::NoBlock && PopCount8(rootManage_.childMask) == 1;
    if (!shrinkable) {
        rootShrinkObservations_ = 0;
        return;
    }
    ++rootShrinkObservations_;
    if (rootShrinkObservations_ >= RootShrinkObservationsRequired) {
        ShrinkRootOnce();
        rootShrinkObservations_ = 0;
        ++counters_.rootShrinks;
        ++report.rootShrinks;
    }
}

void LooseOctree::ShrinkRootOnce() noexcept {
    const uint32 block = rootSearch_.childBlock;
    uint32 kept = 0;
    while (((rootManage_.childMask >> kept) & 1u) == 0) {
        ++kept;
    }
    for (uint32 child = 0; child < 8; ++child) {
        if (child != kept) {
            FreeEmptySubtreeBlocks(ChildNode(block, child));
        }
    }

    // 확장의 역이다. 점유된 자식의 내용을 루트 자리로 옮기고 레코드·부모 링크·대기열 항목을 고친다.
    const uint32 source = ChildNode(block, kept);
    const bool rootQueued = (rootManage_.flags & detail::NodeQueued) != 0;
    const bool sourceQueued = (Manage(source).flags & detail::NodeQueued) != 0;
    Unlink(detail::RootNode);
    Unlink(source);
    rootSearch_ = Search(source);
    rootManage_ = Manage(source);
    rootManage_.queuePrevious = detail::NoNode;
    rootManage_.queueNext = detail::NoNode;
    for (uint32 chunk = rootSearch_.firstChunk; chunk != detail::NoChunk;
         chunk = chunks_[chunk].next) {
        const detail::ElementChunk& elements = chunks_[chunk];
        for (uint32 entry = 0; entry < elements.count; ++entry) {
            records_.ElementAt(elements.slot[entry]).node = detail::RootNode;
        }
    }
    if (rootSearch_.childBlock != detail::NoBlock) {
        manageBlocks_[rootSearch_.childBlock].parent = detail::RootNode;
    }
    // 옮긴 자리를 빈 잎으로 만든 뒤 블록을 해제한다(해제는 그 블록 노드의 대기 항목을 지운다).
    Search(source) = detail::SearchNode{};
    Manage(source).lastChunk = detail::NoChunk;
    FreeBlock(block);
    if (rootQueued || sourceQueued) {
        Enqueue(detail::RootNode);
    }
}

void LooseOctree::FreeBlock(uint32 block) noexcept {
    // 해제한 노드나 재사용된 블록 번호를 가리키는 대기 항목이 남지 않게 한다(불변 조건 9).
    for (uint32 child = 0; child < 8; ++child) {
        const uint32 node = ChildNode(block, child);
        const detail::SearchNode& search = Search(node);
        if (search.directCount != 0 || search.childBlock != detail::NoBlock) {
            platform::Fatal("LooseOctree freed a node block that still holds content");
        }
        Unlink(node);
    }
    searchBlocks_.Free(block);
    manageBlocks_.Free(block);
}

void LooseOctree::FreeEmptySubtreeBlocks(uint32 node) noexcept {
    // 서브트리 프록시 수가 0인 가지의 블록을 아래에서부터 해제한다. 스택 깊이는 level 수 이하다.
    InlineArray<uint32, LooseOctreeSearchStackCapacity> stack;
    stack.PushBack(node);
    while (!stack.IsEmpty()) {
        const uint32 current = stack.Back();
        const uint32 block = Search(current).childBlock;
        if (block == detail::NoBlock) {
            stack.PopBack();
            continue;
        }
        bool descended = false;
        for (uint32 child = 0; child < 8 && !descended; ++child) {
            const uint32 childNode = ChildNode(block, child);
            if (Search(childNode).childBlock != detail::NoBlock) {
                stack.PushBack(childNode);
                descended = true;
            }
        }
        if (descended) {
            continue;
        }
        FreeBlock(block);
        Search(current).childBlock = detail::NoBlock;
        Manage(current).childMask = 0;
        stack.PopBack();
    }
}

void LooseOctree::FreeAllStructure() noexcept {
    // 트리에 프록시가 없을 때만 부른다(청크도 없다). 색인을 0부터 다시 발급해 결정성을 지킨다.
    searchBlocks_.Clear();
    manageBlocks_.Clear();
    chunks_.Clear();
    queueHead_ = detail::NoNode;
    queueTail_ = detail::NoNode;
    queueLength_ = 0;
    rootManage_.flags &= static_cast<uint8>(~detail::NodeQueued);
    rootManage_.queuePrevious = detail::NoNode;
    rootManage_.queueNext = detail::NoNode;
    rootShrinkObservations_ = 0;
}

} // namespace rocks::spatial
