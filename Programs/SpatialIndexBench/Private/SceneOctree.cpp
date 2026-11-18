#include "SceneOctree.h"

#include <Core/Move.h>
#include <Platform/Fatal.h>
#include <Platform/PlatformMath.h>

namespace rocks::spatialbench::scene {

namespace {

constexpr double LoosenessDenominator = 16.0;

} // namespace

NodeContext NodeContext::Make(const double (&center)[3], double extent) noexcept {
    NodeContext context;
    context.center[0] = center[0];
    context.center[1] = center[1];
    context.center[2] = center[2];
    context.extent = extent;
    const double tightChildExtent = extent * 0.5;
    context.childExtent = tightChildExtent * (1.0 + 1.0 / LoosenessDenominator);
    context.childCenterOffset = extent - context.childExtent;
    return context;
}

NodeContext NodeContext::Child(uint32 child) const noexcept {
    double childCenter[3];
    for (uint32 axis = 0; axis < 3; ++axis) {
        childCenter[axis] =
            center[axis] + ((child & (1u << axis)) != 0 ? childCenterOffset : -childCenterOffset);
    }
    return Make(childCenter, childExtent);
}

uint32 NodeContext::ContainingChild(const CenterExtent& bounds) const noexcept {
    for (uint32 axis = 0; axis < 3; ++axis) {
        const double negativeDifference = bounds.center[axis] - (center[axis] - childCenterOffset);
        const double positiveDifference = (center[axis] + childCenterOffset) - bounds.center[axis];
        const double minDifference =
            positiveDifference < negativeDifference ? positiveDifference : negativeDifference;
        if (bounds.extent[axis] + minDifference > childExtent) {
            return NoIndex;
        }
    }
    uint32 index = 0;
    for (uint32 axis = 0; axis < 3; ++axis) {
        index |= bounds.center[axis] > center[axis] ? (1u << axis) : 0u;
    }
    return index;
}

uint32 NodeContext::IntersectingChildren(const CenterExtent& query) const noexcept {
    uint32 positive = 0;
    uint32 negative = 0;
    for (uint32 axis = 0; axis < 3; ++axis) {
        const double queryMax = query.center[axis] + query.extent[axis];
        const double queryMin = query.center[axis] - query.extent[axis];
        const double positiveChildMin = (center[axis] + childCenterOffset) - childExtent;
        const double negativeChildMax = (center[axis] - childCenterOffset) + childExtent;
        positive |= queryMax > positiveChildMin ? (1u << axis) : 0u;
        negative |= queryMin <= negativeChildMax ? (1u << axis) : 0u;
    }
    uint32 subset = 0;
    for (uint32 child = 0; child < 8; ++child) {
        // 자식 i는 축마다 i의 bit가 1이면 양의 쪽, 0이면 음의 쪽 교차가 필요하다.
        const uint32 needPositive = child;
        const uint32 needNegative = ~child & 7u;
        if ((positive & needPositive) == needPositive &&
            (negative & needNegative) == needNegative) {
            subset |= 1u << child;
        }
    }
    return subset;
}

bool Octree2::Create(const double (&center)[3], double extent, uint32 maxElementsPerLeaf,
                     Array<ElementId>& idStore) noexcept {
    Destroy();
    root_ = NodeContext::Make(center, extent);
    maxElementsPerLeaf_ = maxElementsPerLeaf;
    idStore_ = &idStore;
    minLeafExtent_ = extent * platform::PlatformMath::Pow((1.0 + 1.0 / LoosenessDenominator) / 2.0,
                                                          static_cast<double>(MaxNodeDepth));
    return nodes_.TryPushBack(Node{}) && elements_.TryEmplaceBack(*allocator_);
}

void Octree2::Destroy() noexcept {
    nodes_.Empty();
    parentLinks_.Empty();
    elements_.Empty();
    freeList_.Empty();
}

uint32 Octree2::AllocateEightNodes() noexcept {
    if (freeList_.Size() != 0) {
        const uint32 block = freeList_[freeList_.Size() - 1];
        (void)freeList_.TryRemoveAt(freeList_.Size() - 1);
        return block * 8 + 1;
    }
    const uint32 index = static_cast<uint32>(nodes_.Size());
    for (uint32 child = 0; child < 8; ++child) {
        if (!nodes_.TryPushBack(Node{}) || !elements_.TryEmplaceBack(*allocator_)) {
            platform::Fatal("TOctree2 design port: node allocation failed");
        }
    }
    if (!parentLinks_.TryPushBack(NoIndex)) {
        platform::Fatal("TOctree2 design port: parent link allocation failed");
    }
    return index;
}

void Octree2::FreeEightNodes(uint32 index) noexcept {
    for (uint32 child = 0; child < 8; ++child) {
        nodes_[index + child] = Node{};
    }
    parentLinks_[(index - 1) / 8] = NoIndex;
    if (!freeList_.TryPushBack((index - 1) / 8)) {
        platform::Fatal("TOctree2 design port: free list allocation failed");
    }
}

bool Octree2::AddElement(const Element& element) noexcept {
    // 분할 때 기존 요소를 옮겨 둘 임시 저장소다(호출마다 비어 있게 시작한다).
    Array<Element> temp(*allocator_);
    AddElementInternal(0, root_, element, temp);
    return true;
}

void Octree2::AddElementInternal(uint32 node, const NodeContext& context, const Element& element,
                                 Array<Element>& temp) noexcept {
    ++nodes_[node].inclusiveNumElements;
    if (IsLeaf(node)) {
        if (elements_[node].Size() + 1 > maxElementsPerLeaf_ && context.extent > minLeafExtent_) {
            temp = Move(elements_[node]);
            elements_[node] = Array<Element>(*allocator_);

            const uint32 childStart = AllocateEightNodes();
            parentLinks_[(childStart - 1) / 8] = node;
            nodes_[node].childNodes = childStart;
            nodes_[node].inclusiveNumElements = 0;

            // 재삽입 중 같은 temp가 다시 분할에 쓰일 수 없다(자식 하나가 받는 수가 용량 이하).
            for (usize index = 0; index < temp.Size(); ++index) {
                const Element moved = temp[index];
                AddElementInternal(node, context, moved, temp);
            }
            temp.Clear();
            AddElementInternal(node, context, element, temp);
            return;
        }
        const uint32 elementIndex = static_cast<uint32>(elements_[node].Size());
        if (!elements_[node].TryPushBack(element)) {
            platform::Fatal("TOctree2 design port: element allocation failed");
        }
        SetElementId(element, ElementId{node, elementIndex});
        return;
    }
    const uint32 child = context.ContainingChild(element.bounds);
    if (child == NoIndex) {
        const uint32 elementIndex = static_cast<uint32>(elements_[node].Size());
        if (!elements_[node].TryPushBack(element)) {
            platform::Fatal("TOctree2 design port: element allocation failed");
        }
        SetElementId(element, ElementId{node, elementIndex});
        return;
    }
    AddElementInternal(nodes_[node].childNodes + child, context.Child(child), element, temp);
}

void Octree2::CollapseNodesInternal(uint32 node, Array<Element>& collapsed) noexcept {
    for (usize index = 0; index < elements_[node].Size(); ++index) {
        if (!collapsed.TryPushBack(elements_[node][index])) {
            platform::Fatal("TOctree2 design port: collapse allocation failed");
        }
    }
    elements_[node].Clear();
    if (!IsLeaf(node)) {
        const uint32 childStart = nodes_[node].childNodes;
        for (uint32 child = 0; child < 8; ++child) {
            CollapseNodesInternal(childStart + child, collapsed);
        }
        nodes_[node].childNodes = NoIndex;
        FreeEightNodes(childStart);
    }
}

void Octree2::RemoveElement(ElementId id) noexcept {
    Array<Element>& elements = elements_[id.node];
    // swap-remove(줄이지 않음).
    (void)elements.TryRemoveAtSwap(id.index);
    if (id.index < elements.Size()) {
        SetElementId(elements[id.index], id);
    }

    uint32 collapseNode = NoIndex;
    uint32 node = id.node;
    while (true) {
        --nodes_[node].inclusiveNumElements;
        if (nodes_[node].inclusiveNumElements < MinInclusiveElementsPerNode) {
            collapseNode = node;
        }
        if (node == 0) {
            break;
        }
        node = parentLinks_[(node - 1) / 8];
    }

    if (collapseNode != NoIndex && !IsLeaf(collapseNode)) {
        if (elements_[collapseNode].Size() < nodes_[collapseNode].inclusiveNumElements) {
            Array<Element> temp(*allocator_);
            if (!temp.TryReserve(nodes_[collapseNode].inclusiveNumElements)) {
                platform::Fatal("TOctree2 design port: collapse allocation failed");
            }
            CollapseNodesInternal(collapseNode, temp);
            elements_[collapseNode] = Move(temp);
            for (usize index = 0; index < elements_[collapseNode].Size(); ++index) {
                SetElementId(elements_[collapseNode][index],
                             ElementId{collapseNode, static_cast<uint32>(index)});
            }
        }
    }
}

void Octree2::MeasureInternal(uint32 node, uint32 depth, uint64& nodes, uint32& maxDepth,
                              uint32& maxElements) const noexcept {
    ++nodes;
    maxDepth = depth > maxDepth ? depth : maxDepth;
    const uint32 count = static_cast<uint32>(elements_[node].Size());
    maxElements = count > maxElements ? count : maxElements;
    if (!IsLeaf(node)) {
        for (uint32 child = 0; child < 8; ++child) {
            MeasureInternal(nodes_[node].childNodes + child, depth + 1, nodes, maxDepth,
                            maxElements);
        }
    }
}

void Octree2::Measure(uint64& nodes, uint32& maxDepth, uint64& reservedBytes,
                      uint32& maxElements) const noexcept {
    MeasureInternal(0, 1, nodes, maxDepth, maxElements);
    reservedBytes = nodes_.Capacity() * sizeof(Node) + parentLinks_.Capacity() * sizeof(uint32) +
                    elements_.Capacity() * sizeof(Array<Element>) +
                    freeList_.Capacity() * sizeof(uint32);
    for (usize index = 0; index < elements_.Size(); ++index) {
        reservedBytes += elements_[index].Capacity() * sizeof(Element);
    }
}

} // namespace rocks::spatialbench::scene
