#pragma once

#include "FlatScan.h"

// 비교군 C "TOctree2 설계 기반 독립 구현"(Docs/Design/SpatialIndex.ko.md 9.1절)이다. Unreal의
// 성능이 아니다. UE 5.8.4 GenericOctree.h(TOctree2)를 읽어 얻은 설계 사실만 따르고 코드는 옮기지
// 않았다.
//
// 따르는 설계 사실(스펙 3.2절, GenericOctree.h·.inl):
// - 노드는 {자식 블록 시작 색인, 서브트리 요소 수} 8 byte이고 경계를 저장하지 않는다. 순회 중 노드
//   문맥(중심, 반폭, 자식 반폭, 자식 중심 거리)으로 계산한다.
// - 세 배열: 노드, 부모 링크(8개 블록당 1개), 노드별 요소 배열. 자식 8개는 인접 블록으로 할당하고
//   해제한 블록 번호는 스택으로 재사용한다. 배열은 줄이지 않는다.
// - looseness는 분모 16 고정: 자식 반폭 = 부모 반폭/2 · (1 + 1/16), 자식 중심 거리 = 부모 반폭 −
//   자식 반폭(자식의 확장 경계가 부모 경계 안쪽 면에 닿는다). 루트는 확장하지 않은 반폭이다.
// - 배치: 가장 가까운 자식 하나만 검사하고, 들어가지 않으면 현재 노드에 둔다(내부 노드 상한 없음).
//   자식 번호 bit 0·1·2 = x·y·z가 중심보다 큼.
// - 분할: 잎이 MaxElementsPerLeaf를 넘고 반폭이 최소 잎 반폭보다 크면 분할하고 기존 요소를 모두
//   재삽입한다. 최소 잎 반폭 = 루트 반폭 · ((1 + 1/16)/2)^MaxNodeDepth.
// - 삭제: swap-remove, 옮겨진 요소의 ID를 SetElementId 콜백으로 통지, 부모 링크로 루트까지 수를
// 줄이며
//   MinInclusiveElementsPerNode 미만이 된 가장 위 노드로 하위 요소를 모은다(모인 요소마다 콜백).
// - 요소 ID = {노드, 요소 색인}이고 세대 번호가 없다. 이동 API가 없다(삭제 후 재삽입).
// - 루트는 고정이다(확장·범위 검사 없음).
// - 검색은 재귀다. 박스 검색은 서브트리 수 0인 노드를 건너뛰고, 요소를 center·extent 식
//   (|Δcenter| > extent 합이면 밖)으로 판정하며, 교차 자식 집합을 양의 쪽 자식은 질의 max > 자식
//   min(엄격), 음의 쪽 자식은 질의 min ≤ 자식 max로 구한다. 첫 요소 검색은 찾으면 멈춘다.
//   구·프러스텀· ray는 TOctree2에 전용 질의가 없어 노드 경계 predicate 순회(자식 0–7 고정 순서)로
//   한다.
// - 의미 상수: 씬 광원 옥트리 MaxElementsPerLeaf 16, 프리미티브 옥트리 256,
// MinInclusiveElementsPerNode
//   7, MaxNodeDepth 12(Renderer LightSceneInfo.h·PrimitiveSceneInfo.h).
//
// 원본과 다른 점: 원본의 SIMD 4-lane double(VectorRegister4Double) 판정을 같은 식의 scalar double로
// 계산한다(Register4f는 float뿐이고 플랫폼 경계 밖 intrinsic을 쓰지 않는다). 필터 mask 검사를 요소
// 판정 앞에 더했다(공통 조건). 요소는 {center·extent double 4성분씩, 식별 값, 필터} 72 byte다.
namespace rocks::spatialbench::scene {

inline constexpr uint32 NoIndex = NumericLimits<uint32>::Max();

struct CenterExtent {
    double center[4];
    double extent[4];

    static CenterExtent FromBox(const Box<double>& box) noexcept {
        CenterExtent value;
        value.center[0] = box.min.x * 0.5 + box.max.x * 0.5;
        value.center[1] = box.min.y * 0.5 + box.max.y * 0.5;
        value.center[2] = box.min.z * 0.5 + box.max.z * 0.5;
        value.center[3] = 0.0;
        value.extent[0] = (box.max.x - box.min.x) * 0.5;
        value.extent[1] = (box.max.y - box.min.y) * 0.5;
        value.extent[2] = (box.max.z - box.min.z) * 0.5;
        value.extent[3] = 0.0;
        return value;
    }

    Box<double> ToBox() const noexcept {
        return Box<double>(
            Vec3d(center[0] - extent[0], center[1] - extent[1], center[2] - extent[2]),
            Vec3d(center[0] + extent[0], center[1] + extent[1], center[2] + extent[2]));
    }
};

// 두 center·extent 박스의 교차(축마다 |Δcenter| ≤ extent 합)다.
inline bool Intersect(const CenterExtent& a, const CenterExtent& b) noexcept {
    for (uint32 axis = 0; axis < 4; ++axis) {
        const double difference = a.center[axis] - b.center[axis];
        const double distance = difference < 0.0 ? -difference : difference;
        if (distance > a.extent[axis] + b.extent[axis]) {
            return false;
        }
    }
    return true;
}

struct Element {
    CenterExtent bounds;
    uint32 id;
    uint32 filter;
};

struct ElementId {
    uint32 node = NoIndex;
    uint32 index = NoIndex;
};

// 노드 문맥(순회 중 계산)이다.
struct NodeContext {
    double center[3];
    double extent;
    double childExtent;
    double childCenterOffset;

    static NodeContext Make(const double (&center)[3], double extent) noexcept;
    NodeContext Child(uint32 child) const noexcept;
    // 요소가 들어가는 가장 가까운 자식이다. 들어가지 않으면 NoIndex.
    uint32 ContainingChild(const CenterExtent& bounds) const noexcept;
    // 질의와 교차하는 자식 bit 집합(bit i = 자식 i)이다.
    uint32 IntersectingChildren(const CenterExtent& query) const noexcept;

    Box<double> Bounds() const noexcept {
        return Box<double>(Vec3d(center[0] - extent, center[1] - extent, center[2] - extent),
                           Vec3d(center[0] + extent, center[1] + extent, center[2] + extent));
    }
};

class Octree2 {
public:
    static constexpr uint32 MinInclusiveElementsPerNode = 7;
    static constexpr uint32 MaxNodeDepth = 12;

    explicit Octree2(Allocator& allocator) noexcept
        : allocator_(&allocator), nodes_(allocator), parentLinks_(allocator), elements_(allocator),
          freeList_(allocator) {
    }

    // 루트 중심·반폭(확장 없음)과 잎 용량으로 만든다. idStore는 식별 값 → ElementId 외부 저장소다.
    bool Create(const double (&center)[3], double extent, uint32 maxElementsPerLeaf,
                Array<ElementId>& idStore) noexcept;
    void Destroy() noexcept;
    bool AddElement(const Element& element) noexcept;
    void RemoveElement(ElementId id) noexcept;

    // 박스 검색(FindElementsWithBoundsTest 대응).
    template <bool Counting, typename Emit>
    void FindWithBoundsTest(const CenterExtent& query, uint32 mask, Emit& emit,
                            QueryCounters& counters) const noexcept {
        FindWithBoundsTestInternal<Counting>(0, root_, query, mask, emit, counters);
    }

    // 첫 요소 검색(FindFirstElementWithBoundsTest 대응).
    template <bool Counting>
    bool FindFirstWithBoundsTest(const CenterExtent& query, uint32 mask,
                                 QueryCounters& counters) const noexcept {
        return FindFirstInternal<Counting>(0, root_, query, mask, counters);
    }

    // 노드 경계 predicate 순회(FindElementsWithPredicate 대응). predicate.Node(노드 경계 Box)가
    // 참인 노드의 요소를 predicate.Element에 넘긴다.
    template <bool Counting, typename Predicate>
    void FindWithPredicate(Predicate& predicate, uint32 mask,
                           QueryCounters& counters) const noexcept {
        FindWithPredicateInternal<Counting>(0, root_, predicate, mask, counters);
    }

    void Measure(uint64& nodes, uint32& maxDepth, uint64& reservedBytes,
                 uint32& maxElements) const noexcept;

private:
    struct Node {
        uint32 childNodes = NoIndex;
        uint32 inclusiveNumElements = 0;
    };

    static_assert(sizeof(Node) == 8, "TOctree2 node layout is 8 bytes");

    bool IsLeaf(uint32 node) const noexcept {
        return nodes_[node].childNodes == NoIndex;
    }

    void SetElementId(const Element& element, ElementId id) noexcept {
        (*idStore_)[element.id] = id;
    }

    uint32 AllocateEightNodes() noexcept;
    void FreeEightNodes(uint32 index) noexcept;
    void AddElementInternal(uint32 node, const NodeContext& context, const Element& element,
                            Array<Element>& temp) noexcept;
    void CollapseNodesInternal(uint32 node, Array<Element>& collapsed) noexcept;
    void MeasureInternal(uint32 node, uint32 depth, uint64& nodes, uint32& maxDepth,
                         uint32& maxElements) const noexcept;

    template <bool Counting, typename Emit>
    void FindWithBoundsTestInternal(uint32 node, const NodeContext& context,
                                    const CenterExtent& query, uint32 mask, Emit& emit,
                                    QueryCounters& counters) const noexcept {
        if (nodes_[node].inclusiveNumElements == 0) {
            return;
        }
        if constexpr (Counting) {
            ++counters.visitedNodes;
        }
        const Array<Element>& elements = elements_[node];
        for (usize index = 0; index < elements.Size(); ++index) {
            const Element& element = elements[index];
            if ((element.filter & mask) == 0) {
                continue;
            }
            if constexpr (Counting) {
                ++counters.testedProxies;
            }
            if (Intersect(element.bounds, query)) {
                emit(element);
            }
        }
        if (!IsLeaf(node)) {
            const uint32 subset = context.IntersectingChildren(query);
            const uint32 childStart = nodes_[node].childNodes;
            for (uint32 child = 0; child < 8; ++child) {
                if ((subset & (1u << child)) != 0) {
                    FindWithBoundsTestInternal<Counting>(childStart + child, context.Child(child),
                                                         query, mask, emit, counters);
                }
            }
        }
    }

    template <bool Counting>
    bool FindFirstInternal(uint32 node, const NodeContext& context, const CenterExtent& query,
                           uint32 mask, QueryCounters& counters) const noexcept {
        if (nodes_[node].inclusiveNumElements == 0) {
            return false;
        }
        if constexpr (Counting) {
            ++counters.visitedNodes;
        }
        const Array<Element>& elements = elements_[node];
        for (usize index = 0; index < elements.Size(); ++index) {
            const Element& element = elements[index];
            if ((element.filter & mask) == 0) {
                continue;
            }
            if constexpr (Counting) {
                ++counters.testedProxies;
            }
            if (Intersect(element.bounds, query)) {
                return true;
            }
        }
        if (!IsLeaf(node)) {
            const uint32 subset = context.IntersectingChildren(query);
            const uint32 childStart = nodes_[node].childNodes;
            for (uint32 child = 0; child < 8; ++child) {
                if ((subset & (1u << child)) != 0 &&
                    FindFirstInternal<Counting>(childStart + child, context.Child(child), query,
                                                mask, counters)) {
                    return true;
                }
            }
        }
        return false;
    }

    template <bool Counting, typename Predicate>
    void FindWithPredicateInternal(uint32 node, const NodeContext& context, Predicate& predicate,
                                   uint32 mask, QueryCounters& counters) const noexcept {
        if (nodes_[node].inclusiveNumElements == 0) {
            return;
        }
        if constexpr (Counting) {
            ++counters.visitedNodes;
        }
        if (!predicate.Node(context.Bounds())) {
            return;
        }
        const Array<Element>& elements = elements_[node];
        for (usize index = 0; index < elements.Size(); ++index) {
            const Element& element = elements[index];
            if ((element.filter & mask) == 0) {
                continue;
            }
            if constexpr (Counting) {
                ++counters.testedProxies;
            }
            predicate.Element(element);
        }
        if (!IsLeaf(node)) {
            const uint32 childStart = nodes_[node].childNodes;
            for (uint32 child = 0; child < 8; ++child) {
                FindWithPredicateInternal<Counting>(childStart + child, context.Child(child),
                                                    predicate, mask, counters);
            }
        }
    }

    Allocator* allocator_;
    NodeContext root_{};
    double minLeafExtent_ = 0.0;
    uint32 maxElementsPerLeaf_ = 16;
    Array<Node> nodes_;
    Array<uint32> parentLinks_;
    Array<Array<Element>> elements_;
    Array<uint32> freeList_;
    Array<ElementId>* idStore_ = nullptr;
};

} // namespace rocks::spatialbench::scene

namespace rocks::spatialbench {

// 비교군 C의 벤치 API 래퍼다. 외부 ID 저장소(식별 값 → ElementId)를 갖고, 삭제·이동은 그 저장소로
// 요소 ID를 찾아 RemoveElement(+ AddElement)한다. ID 콜백과 저장소 갱신은 측정 구간 안이다.
class SceneIndex {
public:
    SceneIndex(Allocator& allocator, uint32 leafCapacity, const double (&center)[3],
               double extent) noexcept
        : tree_(allocator), ids_(allocator), leafCapacity_(leafCapacity), extent_(extent) {
        center_[0] = center[0];
        center_[1] = center[1];
        center_[2] = center[2];
        name_[0] = 'C';
        uint32 position = 1;
        char digits[12];
        uint32 count = 0;
        for (uint32 value = leafCapacity; value != 0; value /= 10) {
            digits[count++] = static_cast<char>('0' + value % 10);
        }
        while (count != 0) {
            name_[position++] = digits[--count];
        }
        name_[position] = '\0';
    }

    const char* Name() const noexcept {
        return name_;
    }

    double RootExtent() const noexcept {
        return extent_;
    }

    bool Create(uint32 capacity) noexcept {
        ids_.Clear();
        for (uint32 index = 0; index < capacity; ++index) {
            if (!ids_.TryPushBack(scene::ElementId{})) {
                return false;
            }
        }
        return tree_.Create(center_, extent_, leafCapacity_, ids_);
    }

    void Destroy() noexcept {
        tree_.Destroy();
        ids_.Empty();
    }

    bool Insert(uint32 id, const Box<double>& bounds, uint32 filter) noexcept {
        if (id >= ids_.Size()) {
            return false;
        }
        return tree_.AddElement(scene::Element{scene::CenterExtent::FromBox(bounds), id, filter});
    }

    bool Remove(uint32 id, const Box<double>&) noexcept {
        tree_.RemoveElement(ids_[id]);
        ids_[id] = scene::ElementId{};
        return true;
    }

    bool Update(uint32 id, const Box<double>&, const Box<double>& bounds) noexcept {
        tree_.RemoveElement(ids_[id]);
        return tree_.AddElement(
            scene::Element{scene::CenterExtent::FromBox(bounds), id, ProxyFilter(id)});
    }

    void Maintain(uint32) noexcept {
    }

    void MaintainAll() noexcept {
    }

    void BeginRead() noexcept {
    }

    void EndRead() noexcept {
    }

    template <bool Counting>
    void QueryBox(const Box<double>& box, uint32 mask, ResultBuffer& out,
                  QueryCounters& counters) const noexcept {
        auto emit = [&](const scene::Element& element) noexcept { out.Push(element.id); };
        tree_.FindWithBoundsTest<Counting>(scene::CenterExtent::FromBox(box), mask, emit, counters);
    }

    template <bool Counting>
    void QuerySphere(const Sphere<double>& sphere, uint32 mask, ResultBuffer& out,
                     QueryCounters& counters) const noexcept {
        struct Predicate {
            const Sphere<double>* sphere;
            ResultBuffer* out;
            bool Node(const Box<double>& bounds) const noexcept {
                return Overlap(*sphere, bounds) == QueryResult::Hit;
            }
            void Element(const scene::Element& element) noexcept {
                if (Overlap(*sphere, element.bounds.ToBox()) == QueryResult::Hit) {
                    out->Push(element.id);
                }
            }
        } predicate{&sphere, &out};
        tree_.FindWithPredicate<Counting>(predicate, mask, counters);
    }

    template <bool Counting>
    void QueryFrustum(const ConvexVolume<double>& volume, uint32 mask, ResultBuffer& out,
                      QueryCounters& counters) const noexcept {
        struct Predicate {
            const ConvexVolume<double>* volume;
            ResultBuffer* out;
            bool Node(const Box<double>& bounds) const noexcept {
                return Overlap(*volume, bounds) == QueryResult::Hit;
            }
            void Element(const scene::Element& element) noexcept {
                if (Overlap(*volume, element.bounds.ToBox()) == QueryResult::Hit) {
                    out->Push(element.id);
                }
            }
        } predicate{&volume, &out};
        tree_.FindWithPredicate<Counting>(predicate, mask, counters);
    }

    template <bool Counting>
    void QueryRay(const Ray<double>& ray, double maxDistance, uint32 mask, ResultBuffer& out,
                  QueryCounters& counters) const noexcept {
        struct Predicate {
            const Ray<double>* ray;
            double maxDistance;
            ResultBuffer* out;
            bool Node(const Box<double>& bounds) const noexcept {
                return Raycast(*ray, bounds, maxDistance).IsHit();
            }
            void Element(const scene::Element& element) noexcept {
                if (Raycast(*ray, element.bounds.ToBox(), maxDistance).IsHit()) {
                    out->Push(element.id);
                }
            }
        } predicate{&ray, maxDistance, &out};
        tree_.FindWithPredicate<Counting>(predicate, mask, counters);
    }

    // TOctree2에 최근접이 없다. predicate 순회에서 노드 진입 거리가 현재 최단보다 크면 가지를 친다.
    template <bool Counting>
    bool QueryNearest(const Ray<double>& ray, double maxDistance, uint32 mask, uint32& id,
                      double& distance, QueryCounters& counters) const noexcept {
        struct Predicate {
            const Ray<double>* ray;
            double best;
            bool found;
            uint32 id;
            bool Node(const Box<double>& bounds) const noexcept {
                return Raycast(*ray, bounds, best).IsHit();
            }
            void Element(const scene::Element& element) noexcept {
                double entry = 0.0;
                if (RayEntry(*ray, element.bounds.ToBox(), best, entry) &&
                    (!found || entry < best)) {
                    found = true;
                    best = entry;
                    id = element.id;
                }
            }
        } predicate{&ray, maxDistance, false, 0};
        tree_.FindWithPredicate<Counting>(predicate, mask, counters);
        distance = predicate.best;
        id = predicate.id;
        return predicate.found;
    }

    template <bool Counting>
    bool AnyBox(const Box<double>& box, uint32 mask, QueryCounters& counters) const noexcept {
        return tree_.FindFirstWithBoundsTest<Counting>(scene::CenterExtent::FromBox(box), mask,
                                                       counters);
    }

    template <bool Counting>
    uint64 CountBox(const Box<double>& box, uint32 mask, QueryCounters& counters) const noexcept {
        uint64 total = 0;
        auto emit = [&](const scene::Element&) noexcept { ++total; };
        tree_.FindWithBoundsTest<Counting>(scene::CenterExtent::FromBox(box), mask, emit, counters);
        return total;
    }

    void Describe(JsonWriter& json) const noexcept {
        uint64 nodes = 0;
        uint32 maxDepth = 0;
        uint64 reservedBytes = 0;
        uint32 maxElements = 0;
        tree_.Measure(nodes, maxDepth, reservedBytes, maxElements);
        json.FieldInteger("nodes", nodes);
        json.FieldInteger("maxDepth", maxDepth);
        json.FieldInteger("reservedBytes",
                          reservedBytes + ids_.Capacity() * sizeof(scene::ElementId));
        json.FieldInteger("maxElementsPerNode", maxElements);
        json.Field("rootExtent", extent_);
        json.Field("rootCenterX", center_[0]);
        json.Field("rootCenterY", center_[1]);
        json.Field("rootCenterZ", center_[2]);
        json.FieldInteger("leafCapacity", leafCapacity_);
    }

private:
    scene::Octree2 tree_;
    Array<scene::ElementId> ids_;
    uint32 leafCapacity_;
    double center_[3];
    double extent_;
    char name_[16];
};

} // namespace rocks::spatialbench
