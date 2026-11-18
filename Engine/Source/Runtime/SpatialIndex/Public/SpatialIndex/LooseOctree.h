#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/IndexPool.h>
#include <Core/Containers/InlineArray.h>
#include <Core/Containers/SlotArray.h>
#include <Core/Containers/Span.h>
#include <Core/Diagnostics/AccessOverlapChecker.h>
#include <Core/Math/Intersection.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <SpatialIndex/LooseOctreeTypes.h>

namespace rocks::spatial {

class LooseOctree;
class LooseOctreeReader;
class SpatialReadScope;

namespace detail {

// 내부 컨테이너 할당을 세는 adapter다(Private/LooseOctreeInternal.h).
class CountingAllocator;

// 저장 구조(Docs/Design/SpatialIndex.ko.md 6.3절)다. LooseOctree 내부 표현이며 공개 API가 아니다.
// 템플릿 검색이 헤더에서 읽으므로 여기에 둔다.

inline constexpr uint32 NoChunk = NumericLimits<uint32>::Max();
inline constexpr uint32 NoBlock = NumericLimits<uint32>::Max();
inline constexpr uint32 NoNode = LooseOctreeNoNode;
// 범위 밖 목록에 있는 프록시 레코드의 노드 칸 값이다.
inline constexpr uint32 OutOfRangeNode = NoNode - 1;
// 루트는 객체 안에 따로 두고 번호 0을 쓴다. 자식 블록 b의 자식 i는 1 + 8b + i다.
inline constexpr uint32 RootNode = 0;

// 읽기·변경 단계 위반의 Fatal 메시지다(6.9절). 이동·대입·파괴 메시지는 LooseOctree가 CheckIdle에
// 넘긴다.
inline constexpr AccessOverlapMessages LooseOctreeAccessMessages{
    .writeDuringRead = "LooseOctree mutation while a read scope is open",
    .readDuringWrite = "LooseOctree read scope opened during a mutation",
    .writeDuringWrite = "LooseOctree mutation overlaps another mutation",
    .destroyedWhileOpen = "LooseOctree destroyed while a read scope is open",
    .unbalancedRelease = "LooseOctree access released without a matching acquire"};

// 노드의 검색용 데이터(16 byte)다. 검색은 이것과 요소 청크만 읽는다.
struct SearchNode {
    uint32 childBlock = NoBlock;
    uint32 firstChunk = NoChunk;
    uint32 directCount = 0;
    uint32 subtreeCount = 0;
};

static_assert(sizeof(SearchNode) == 16, "SearchNode must stay 16 bytes");

struct SearchBlock {
    SearchNode nodes[8];
};

// 노드 관리 상태 플래그다.
enum NodeFlag : uint8 {
    // 분할 평가가 보류로 끝나 재평가 조건을 기다린다.
    NodeHeld = 1u << 0,
    // 제거·갱신이 재평가 조건을 채워 유지보수의 재평가를 기다린다.
    NodeReevaluationPending = 1u << 1,
    // 직접 저장 프록시 중 자식에 들어갈 수 있는 것이 생겼다(제자리 갱신).
    NodeRelocationPending = 1u << 2,
    // 유지보수 대기열에 있다.
    NodeQueued = 1u << 3,
    // 직접 저장 수가 parentWarnCount를 넘어 진단을 냈다(절반 이하로 줄면 지운다).
    NodeWarned = 1u << 4
};

// 노드의 관리용 데이터다. 위치(level, 정수 원점)를 두어 제자리 갱신이 확장 경계를 O(1)에 구한다.
struct ManageNode {
    // 격자 단위(minCellSize) 정수 원점이다. 기본 셀은 각 축
    // [anchor + origin · minCellSize, anchor + (origin + 2^level) · minCellSize]다.
    int64 origin[3] = {0, 0, 0};
    uint32 lastChunk = NoChunk;
    // 보류 시점의 직접 저장 수 n0다.
    uint32 heldCount = 0;
    // 보류 이후 이 노드에서의 삽입·제거·경계 갱신 횟수다.
    uint32 changesSinceHold = 0;
    uint32 reevaluationInterval = 0;
    uint32 queuePrevious = NoNode;
    uint32 queueNext = NoNode;
    uint8 level = 0;
    uint8 childMask = 0;
    uint8 flags = 0;
};

struct ManageBlock {
    ManageNode nodes[8];
    // 이 블록을 자식으로 가진 노드다.
    uint32 parent = NoNode;
};

// 노드 하나의 직접 저장 프록시를 담는 고정 크기 청크다(SoA, 프록시당 44 byte). 경계는 소속 노드
// 중심 기준 상대 좌표이며 한 float ULP 바깥쪽으로 보정되어 있다(불변 조건 7). 청크는 노드마다 연결
// 목록이고 마지막 청크만 덜 찰 수 있다.
struct ElementChunk {
    float minX[LooseOctreeChunkCapacity];
    float minY[LooseOctreeChunkCapacity];
    float minZ[LooseOctreeChunkCapacity];
    float maxX[LooseOctreeChunkCapacity];
    float maxY[LooseOctreeChunkCapacity];
    float maxZ[LooseOctreeChunkCapacity];
    uint32 filter[LooseOctreeChunkCapacity];
    uint32 slot[LooseOctreeChunkCapacity];
    uint32 generation[LooseOctreeChunkCapacity];
    uint64 userValue[LooseOctreeChunkCapacity];
    uint32 count = 0;
    uint32 next = NoChunk;
    uint32 previous = NoChunk;
};

// 프록시의 관리용 레코드다(SlotArray 원소). 정확한 double AABB와 저장 위치를 가진다. 범위 밖 목록에
// 있으면 node는 OutOfRangeNode이고 chunk는 목록 색인이다.
struct ProxyRecord {
    Box<double> bounds;
    uint32 node = NoNode;
    uint32 chunk = NoChunk;
    uint32 entry = 0;
    uint32 filter = 0;
    uint64 userValue = 0;
};

// 범위 밖 목록 원소다. 기준 노드 중심이 없으므로 double 경계를 그대로 둔다.
struct OutOfRangeEntry {
    Box<double> bounds;
    uint32 filter = 0;
    uint32 slot = 0;
    uint32 generation = 0;
    uint64 userValue = 0;
};

// 명시적 검색 스택의 항목이다. 위치를 함께 실어 검색이 관리용 데이터를 읽지 않게 한다. entry는 ray
// 질의의 노드 확장 경계 진입 거리이고 다른 형상은 0이다.
struct SearchStackEntry {
    int64 origin[3];
    double entry;
    uint32 node;
    uint8 level;
    bool contained;
};

// 노드 확장 경계와 질의 형상의 관계다.
enum class NodeClass : uint8 {
    Outside,
    Inside,
    Intersecting
};

constexpr NodeClass ToNodeClass(Containment containment) noexcept {
    return containment == Containment::Outside  ? NodeClass::Outside
           : containment == Containment::Inside ? NodeClass::Inside
                                                : NodeClass::Intersecting;
}

// ---- 질의 형상(검색 순회의 정책) ----
// Classify(노드 확장 경계, 진입 거리 출력)는 노드를 제외·완전 포함·걸침으로 나누고, Test(프록시
// AABB, 진입 거리 출력)는 프록시 하나가 후보인지 정한다. 둘 다 같은 Core 술어(Intersection.h)를
// 쓴다. 그 술어들은 box 좌표에 대해 단조인 고정 순서 식이므로(ClassifyContainment 설명) 확장 경계가
// Outside면 그 안의 어떤 프록시도 후보가 아니고, Inside면 그 안의 모든 프록시가 후보다. 그래서 노드
// 가지치기와 완전 포함 생략이 프록시 단위 술어와 같은 결과를 낸다(반올림까지 포함해 누락이 없다).

// 박스 질의다. 프록시 판정은 청크의 float 상대 경계로 한다(UsesChunkBounds).
struct BoxQueryShape {
    static constexpr bool Ordered = false;
    static constexpr bool UsesChunkBounds = true;
    static constexpr SpatialBatchShape Kind = SpatialBatchShape::Box;
    Box<double> box;

    NodeClass Classify(const Box<double>& loose, double& entry) const noexcept {
        entry = 0.0;
        // 닫힌 집합 규약: 접촉은 교차이고 "밖"은 엄격 부등식이다(6.8절).
        if (loose.min.x > box.max.x || loose.max.x < box.min.x || loose.min.y > box.max.y ||
            loose.max.y < box.min.y || loose.min.z > box.max.z || loose.max.z < box.min.z) {
            return NodeClass::Outside;
        }
        return box.Contains(loose) ? NodeClass::Inside : NodeClass::Intersecting;
    }

    // 범위 밖 목록(정확한 double 경계)에만 쓴다.
    bool Test(const Box<double>& bounds, double& entry) const noexcept {
        entry = 0.0;
        return Overlap(box, bounds) == QueryResult::Hit;
    }
};

struct SphereQueryShape {
    static constexpr bool Ordered = false;
    static constexpr bool UsesChunkBounds = false;
    static constexpr SpatialBatchShape Kind = SpatialBatchShape::Sphere;
    Sphere<double> sphere;

    NodeClass Classify(const Box<double>& loose, double& entry) const noexcept {
        entry = 0.0;
        return ToNodeClass(ClassifyContainment(sphere, loose));
    }

    bool Test(const Box<double>& bounds, double& entry) const noexcept {
        entry = 0.0;
        return Overlap(sphere, bounds) == QueryResult::Hit;
    }
};

// 볼록 영역(프러스텀) 질의다. 후보 술어는 "어느 평면에 대해서도 엄격히 바깥이 아닌 AABB"다(6.8절).
struct VolumeQueryShape {
    static constexpr bool Ordered = false;
    static constexpr bool UsesChunkBounds = false;
    static constexpr SpatialBatchShape Kind = SpatialBatchShape::ConvexVolume;
    const ConvexVolume<double>* volume;

    NodeClass Classify(const Box<double>& loose, double& entry) const noexcept {
        entry = 0.0;
        return ToNodeClass(ClassifyContainment(*volume, loose));
    }

    bool Test(const Box<double>& bounds, double& entry) const noexcept {
        entry = 0.0;
        return Overlap(*volume, bounds) == QueryResult::Hit;
    }
};

// ray 질의다(maxDistance까지의 선분). 완전 포함 분류가 없고, 자식을 진입 거리 순으로 방문한다.
struct RayQueryShape {
    static constexpr bool Ordered = true;
    static constexpr bool UsesChunkBounds = false;
    Ray<double> ray;
    double maxDistance;

    NodeClass Classify(const Box<double>& loose, double& entry) const noexcept {
        return Test(loose, entry) ? NodeClass::Intersecting : NodeClass::Outside;
    }

    bool Test(const Box<double>& bounds, double& entry) const noexcept {
        const RayHit<double> hit = Raycast(ray, bounds, maxDistance);
        entry = hit.distance;
        return hit.IsHit();
    }
};

} // namespace detail

// 범용 영역 질의용 loose octree다(Docs/Design/SpatialIndex.ko.md 6절). 프록시(AABB·필터 mask·사용자
// 값)를 정확히 한 곳(노드 하나 또는 범위 밖 목록)에 저장하고, 박스 질의로 AABB 교차 후보를
// 돌려준다. 정밀 형상 교차는 호출자가 한다.
//
// 소유와 수명
// - 소비자(World·RenderScene·도구)가 인스턴스를 소유한다. 전역 singleton이 아니다.
// - Allocator를 빌린다. allocator는 옥트리보다 오래 살아야 한다.
// - 복사할 수 없고 이동할 수 있다. 이동 뒤 기존 handle은 대상에서 유효하다. 이동된 원본은 빈
//   상태이며 새 handle을 세대 1부터 발급하므로 원본에 옛 handle을 쓰면 검출하지 않는다(SlotArray
//   규칙). 다른 인스턴스가 발급한 handle도 검출하지 않는다.
// - 읽기 범위(SpatialReadScope)가 열려 있는 동안 이동·파괴하면 platform::Fatal이다.
//
// 동시성(6.9절)
// - 변경 단계: 한 스레드만 변경 함수(Try*·RunMaintenance·Clear)를 부른다.
// - 읽기 단계: SpatialReadScope로 얻은 LooseOctreeReader로만 검색한다. 여러 스레드가 동시에
//   검색해도 된다(검색은 트리 상태를 바꾸지 않는다).
// - 두 단계가 겹치면 platform::Fatal이다: 읽기 범위가 열린 동안의 변경, 변경 중 읽기 범위 생성,
//   변경 함수끼리의 겹침. visitor 안에서 같은 트리를 다시 검색하는 것은 허용하고 변경은 위반이다.
//
// 실패 계약(6.1절)
// - 빈 handle은 InvalidArgument, 낡은 handle은 StaleHandle, 유효하지 않은 AABB는 InvalidArgument,
//   프록시 수 한도 초과는 CapacityExceeded, 노드·청크 색인 폭 초과는 Overflow, 할당 실패는
//   OutOfMemory다.
// - 실패한 변경 호출은 논리 상태(프록시 집합, 각 프록시의 AABB·필터·사용자 값·handle)를 바꾸지
//   않는다. 구조(루트 level, 분할 여부)는 달라질 수 있으나 불변 조건은 모두 성립한다.
// - 루트 확장 한도 안에 들어가지 않는 위치·크기는 오류가 아니다. 범위 밖 목록에 넣고 성공한다.
//
// 결과 순서: 같은 초기 상태에 같은 연산열(작업 수 예산의 RunMaintenance 포함)을 적용하면 같은
// 순서가 나온다. 시간 예산 유지보수는 순서를 보장하지 않는다.
//
// 사용 예:
//   LooseOctreeConfig config;   // 기본값: 128 cm 최소 셀, 초기 루트 level 10
//   Result<LooseOctree> created = LooseOctree::TryCreate(config, allocator);
//   if (!created) {
//       return Result<void>::Failure(created.Error());
//   }
//   LooseOctree octree = Move(created.Value());
//
//   SpatialProxyDesc desc;
//   desc.bounds = bounds;
//   desc.filterMask = FilterLocalLightReceiver;
//   desc.userValue = entityId;
//   Result<SpatialProxyHandle> added = octree.TryAdd(desc);   // 변경 단계
//   ...
//   octree.RunMaintenance(MaintenanceBudget::Operations(256));
//   {
//       SpatialReadScope read(octree);                         // 읽기 단계
//       LooseOctreeReader reader = read.Reader();
//       Result<QueryOutcome> queried = reader.QueryBox(
//           lightBounds, FilterLocalLightReceiver, [&](const SpatialProxyHit& hit) noexcept {
//               receivers.TryPushBack(hit.userValue);
//               return QueryControl::Continue;
//           });
//   }
class LooseOctree {
public:
    // 설정을 검사하고 빈 옥트리를 만든다. 내부 할당 계수 adapter 하나(수십 byte)를 allocator에서
    // 할당한다. 컨테이너 저장소는 첫 변경에서 잡는다. 할당 실패는 그 allocator 오류(OutOfMemory
    // 등)다. 오류(InvalidArgument): LooseOctreeConfig의 범위 위반, |anchor| + 2^(maxRootLevel+1) ·
    // minCellSize가 2^52 · minCellSize 이상(격자 계산의 정확 범위 밖), k · 2^maxRootLevel ·
    // minCellSize가 float 정규 범위 밖.
    [[nodiscard]] static Result<LooseOctree> TryCreate(const LooseOctreeConfig& config,
                                                       Allocator& allocator) noexcept;

    LooseOctree(LooseOctree&& other) noexcept;
    LooseOctree& operator=(LooseOctree&& other) noexcept;
    LooseOctree(const LooseOctree&) = delete;
    LooseOctree& operator=(const LooseOctree&) = delete;
    ~LooseOctree() noexcept;

    // ---- 변경 단계 전용 ----

    // 프록시를 등록하고 handle을 돌려준다. 저장 위치는 호출 안에서 정해지며 다음 읽기 단계에서 바로
    // 검색된다. 잎의 직접 저장 수가 splitThreshold를 넘으면 분할을 평가한다(할당 실패는 보류로
    // 처리하고 성공한다). 루트 확장 한도 밖이면 범위 밖 목록에 넣고 성공한다. 오류: desc.bounds가
    // 유효하지 않으면 InvalidArgument, 프록시 수가 maxProxies에 닿았거나 handle slot이 모두
    // 폐기되었으면 CapacityExceeded, 색인 폭 초과는 Overflow, 할당 실패는 OutOfMemory. 실패하면
    // 논리 상태는 그대로다.
    [[nodiscard]] Result<SpatialProxyHandle> TryAdd(const SpatialProxyDesc& desc) noexcept;

    // 프록시를 지운다. 할당하지 않으므로 handle 오류 외에는 실패하지 않는다. 병합·빈 블록 회수는
    // 유지보수 대기열에 넣는다. 오류: 빈 handle은 InvalidArgument, 낡은 handle(이중 삭제 포함)은
    // StaleHandle이며 아무것도 바꾸지 않는다.
    [[nodiscard]] Result<void> TryRemove(SpatialProxyHandle handle) noexcept;

    // 프록시 AABB를 바꾼다. 새 AABB가 지금 노드의 확장 경계에 들어가면 제자리에서 고치고(구조 변경
    // 없음), 벗어나면 그것을 포함하는 첫 조상에서 다시 내려가 옮긴다. 순간 이동·크기 변경도 같은
    // 경로다. handle은 그대로다. 옮겨 간 잎이 분할 임계를 넘어도 이 호출은 분할을 평가하지 않고
    // 재평가 대기만 표시한다(유지보수나 다음 TryAdd가 평가한다). 오류: handle 오류는 TryRemove와
    // 같고, bounds가 유효하지 않으면 InvalidArgument, 할당 실패는 OutOfMemory(프록시는 옛 AABB로 옛
    // 위치에 남는다).
    [[nodiscard]] Result<void> TryUpdateBounds(SpatialProxyHandle handle,
                                               const Box<double>& bounds) noexcept;

    // 필터 mask를 바꾼다(0이면 어떤 질의에도 걸리지 않는다). 할당하지 않는다. 오류: handle 오류.
    [[nodiscard]] Result<void> TrySetFilter(SpatialProxyHandle handle, uint32 filterMask) noexcept;

    // 사용자 값을 바꾼다. 할당하지 않는다. 오류: handle 오류.
    [[nodiscard]] Result<void> TrySetUserValue(SpatialProxyHandle handle,
                                               uint64 userValue) noexcept;

    // 프록시 proxyCount개의 레코드 저장소와 그만큼의 요소 청크를 미리 잡는다(순간적인 대량 생성
    // 전). 오류: proxyCount가 maxProxies를 넘으면 CapacityExceeded, 할당 실패는 OutOfMemory(이미
    // 잡은 용량은 남을 수 있고 논리 상태는 그대로다).
    [[nodiscard]] Result<void> TryReserve(uint32 proxyCount) noexcept;

    // 유지보수(병합·빈 블록 회수·분할 재평가·하강 재배치·루트 축소·빈 트리 재설정)를 예산 안에서
    // 한다 (6.6절). 수행하지 않아도 검색 결과는 정확하다. 실패하는 작업(청크 확보 실패)은 다시
    // 대기시킨다.
    MaintenanceReport RunMaintenance(const MaintenanceBudget& budget) noexcept;

    // 모든 프록시를 지우고 루트를 초기 상태로 되돌린다. handle 세대 번호는 유지되므로 지우기 전
    // handle은 StaleHandle이 된다. 예약 용량은 유지한다. 누적 통계는 지우지 않는다.
    void Clear() noexcept;

private:
    friend class LooseOctreeReader;
    friend class SpatialReadScope;

    class WriterGuard;
    struct Placement;

    LooseOctree(const LooseOctreeConfig& config, Allocator& backing,
                detail::CountingAllocator* counting) noexcept;

    // ---- 생성·이동·저장소 ----
    void InitializeTables() noexcept;
    void TakeFrom(LooseOctree& other) noexcept;
    void RebindContainers(Allocator& allocator) noexcept;
    void ReleaseStorage() noexcept;
    Result<void> EnsureCountingAllocator() noexcept;
    void NotePeaks() noexcept;

    // ---- 노드 접근 ----
    detail::SearchNode& Search(uint32 node) noexcept;
    const detail::SearchNode& Search(uint32 node) const noexcept;
    detail::ManageNode& Manage(uint32 node) noexcept;
    const detail::ManageNode& Manage(uint32 node) const noexcept;
    uint32 ParentOf(uint32 node) const noexcept;
    static uint32 ChildNode(uint32 block, uint32 child) noexcept;
    static uint32 BlockOf(uint32 node) noexcept;
    static uint32 ChildSlotOf(uint32 node) noexcept;

    // ---- 격자 계산(6.2절) ----
    double CenterCoordinate(uint32 axis, uint32 level, int64 origin) const noexcept;
    Vec3d CenterOf(uint32 level, const int64 (&origin)[3]) const noexcept;
    Box<double> LooseBoundsOf(uint32 level, const int64 (&origin)[3]) const noexcept;
    Box<double> CellBoundsOf(uint32 level, const int64 (&origin)[3]) const noexcept;
    static uint32 ChildIndexFor(const Vec3d& point, const Vec3d& center) noexcept;
    static void ChildOrigin(uint32 level, const int64 (&origin)[3], uint32 child,
                            int64 (&out)[3]) noexcept;
    static Vec3d BoxCenter(const Box<double>& box) noexcept;

    // ---- 배치·루트 ----
    Placement PlanPlacement(const Box<double>& bounds) const noexcept;
    Result<void> ReservePlacement(const Placement& placement) noexcept;
    void ApplyRootChanges(const Placement& placement, const Vec3d& center) noexcept;
    void ExpandRootOnce(const Vec3d& center) noexcept;
    void ResetRoot(uint32 level, const int64 (&origin)[3]) noexcept;
    uint32 Descend(uint32 start, const Box<double>& bounds) const noexcept;

    // ---- 청크·경로 ----
    struct EntryData {
        Box<double> bounds;
        uint32 filter;
        uint32 slot;
        uint32 generation;
        uint64 userValue;
    };

    void AppendEntry(uint32 node, const EntryData& data, uint32& chunkOut,
                     uint32& entryOut) noexcept;
    void WriteEntry(uint32 node, uint32 chunk, uint32 entry, const EntryData& data) noexcept;
    void WriteEntryBounds(uint32 node, uint32 chunk, uint32 entry,
                          const Box<double>& bounds) noexcept;
    void RemoveEntry(uint32 node, uint32 chunk, uint32 entry) noexcept;
    EntryData EntryOfRecord(const detail::ProxyRecord& record, SlotHandle handle) const noexcept;
    void AdjustPath(uint32 from, uint32 stopExclusive, bool increment) noexcept;
    // 직접 저장 수가 늘어난 잎의 분할 평가 시점을 정한다. evaluateNow가 거짓(경계 갱신)이면 재평가
    // 대기만 표시한다.
    void AfterDirectGrowth(uint32 node, bool evaluateNow) noexcept;
    void NoteHeldChange(uint32 node) noexcept;
    void InsertIntoTree(uint32 start, uint32 stopExclusive, uint32 slot,
                        bool evaluateSplit) noexcept;
    void MoveToOutOfRange(uint32 slot) noexcept;
    void RemoveFromOutOfRange(uint32 index) noexcept;
    void NoteOutOfRangeGrowth() noexcept;
    Result<void> CheckHandle(SpatialProxyHandle handle) const noexcept;

    // ---- 분할(6.4절) ----
    void EvaluateSplits(uint32 node) noexcept;
    bool TrySplitOnce(uint32 node,
                      InlineArray<uint32, LooseOctreeSearchStackCapacity>& cascade) noexcept;
    void Hold(uint32 node) noexcept;
    void ReleaseHold(uint32 node) noexcept;

    // ---- 유지보수(6.6절) ----
    void Enqueue(uint32 node) noexcept;
    void Unlink(uint32 node) noexcept;
    uint32 PendingLimit() const noexcept;
    void EnforcePendingLimit() noexcept;
    uint32 ProcessQueue(uint32 maxItems, uint64 deadline, MaintenanceReport& report) noexcept;
    // 병합·회수 항목 처리 결과다. Stalled는 청크 확보 실패로 항목을 다시 대기시켰다는 뜻이다.
    enum class CollapseOutcome : uint8 {
        Collapsed,
        Deferred,
        Stalled
    };

    bool ProcessItem(uint32 node, MaintenanceReport& report) noexcept;
    CollapseOutcome TryCollapseChildren(uint32 node, MaintenanceReport& report) noexcept;
    bool RelocateDown(uint32 node, MaintenanceReport& report) noexcept;
    void CheckRoot(MaintenanceReport& report) noexcept;
    void ShrinkRootOnce() noexcept;
    void FreeBlock(uint32 block) noexcept;
    void FreeEmptySubtreeBlocks(uint32 node) noexcept;
    void FreeAllStructure() noexcept;
    bool ShouldCollapse(uint32 node) const noexcept;

    // ---- 읽기(LooseOctreeReader가 부른다) ----
    template <typename Shape, typename Policy>
    void Walk(const Shape& shape, Policy& policy, QueryStats* stats) const noexcept;
    template <typename Shape, typename Emit>
    bool ForEachNodeCandidate(uint32 nodeIndex, uint32 level, const int64 (&origin)[3],
                              bool contained, const Shape& shape, uint32 filterMask,
                              QueryStats* stats, Emit& emit) const noexcept;
    template <typename Shape, typename Emit>
    bool ForEachOutOfRangeCandidate(const Shape& shape, uint32 filterMask, QueryStats* stats,
                                    Emit& emit) const noexcept;
    template <typename Shape, typename Sink>
    QueryOutcome RunQuery(const Shape& shape, uint32 filterMask, Sink& sink,
                          QueryStats* stats) const noexcept;
    template <typename HitTest>
    NearestHit RunNearest(const detail::RayQueryShape& shape, uint32 filterMask, HitTest& hitTest,
                          QueryStats* stats) const noexcept;
    template <typename Shape>
    BufferQueryOutcome CollectBatches(const Shape& shape,
                                      Span<SpatialNodeBatch> out) const noexcept;
    template <typename Shape, typename Sink>
    Result<QueryOutcome> RunBatch(const SpatialNodeBatch& batch, const Shape& shape,
                                  uint32 filterMask, Sink& sink, QueryStats* stats) const noexcept;
    bool IsValidHandle(SpatialProxyHandle handle) const noexcept;
    Result<SpatialProxyDesc> TryGetProxy(SpatialProxyHandle handle) const noexcept;
    uint32 ProxyCount() const noexcept;
    OctreeStats CollectStats() const noexcept;
    Result<OctreeValidationReport> ValidateStructure() const noexcept;
    SpatialNodeInfo DescribeNode(uint32 node, uint32 depth) const noexcept;
    void CheckNoOpenScope(const char* message) const noexcept;

    struct Counters {
        uint64 splits = 0;
        uint64 chainSplits = 0;
        uint64 merges = 0;
        uint64 reclaims = 0;
        uint64 reinserts = 0;
        uint64 inPlaceUpdates = 0;
        uint64 relocatedProxies = 0;
        uint64 rootExpansions = 0;
        uint64 rootShrinks = 0;
        uint64 emptyResets = 0;
        uint64 splitEvaluations = 0;
        uint64 holds = 0;
        uint64 reevaluations = 0;
        uint64 forcedMaintenanceRuns = 0;
        uint64 forcedMaintenanceItems = 0;
        uint64 parentWarnEvents = 0;
        uint64 outOfRangeWarnEvents = 0;
        uint64 outOfRangeInsertions = 0;
    };

    // 배열별 예약 용량의 피크(byte)다. 변경 함수가 끝날 때마다 갱신한다.
    struct Peaks {
        usize recordBytes = 0;
        usize chunkBytes = 0;
        usize nodeBytes = 0;
        usize outOfRangeBytes = 0;
    };

    LooseOctreeConfig config_;
    // 받은 allocator와 그것을 감싼 계수 adapter다. 컨테이너는 adapter에 묶인다. 이동된 원본은
    // adapter가 없고(nullptr) 컨테이너가 받은 allocator에 직접 묶이며, 다음 추가가 adapter를 다시
    // 만든다.
    Allocator* backing_ = nullptr;
    detail::CountingAllocator* counting_ = nullptr;
    // 레벨별 기본 셀 한 변과 확장 경계 반 변이다(정확한 2진 유리수).
    double cellSize_[LooseOctreeLevelLimit + 1] = {};
    double looseHalf_[LooseOctreeLevelLimit + 1] = {};
    double halfMinCell_ = 0.0;
    // 초기 루트의 정수 원점(각 축 −2^(R0−1))이다.
    int64 initialOrigin_ = 0;
    // 노드 정수 좌표의 허용 범위 [−gridLimit_, gridLimit_]다(2^(maxRootLevel+1)).
    int64 gridLimit_ = 0;

    SlotArray<detail::ProxyRecord> records_;
    IndexPool<detail::SearchBlock> searchBlocks_;
    IndexPool<detail::ManageBlock> manageBlocks_;
    IndexPool<detail::ElementChunk> chunks_;
    Array<detail::OutOfRangeEntry> outOfRange_;
    detail::SearchNode rootSearch_;
    detail::ManageNode rootManage_;

    uint32 queueHead_ = detail::NoNode;
    uint32 queueTail_ = detail::NoNode;
    uint32 queueLength_ = 0;
    // 루트 축소 조건이 연속으로 관측된 유지보수 횟수다(4회에 한 단계 축소).
    uint32 rootShrinkObservations_ = 0;
    bool outOfRangeWarned_ = false;
    Counters counters_;
    Peaks peaks_;

    // 트리 상태의 세대다. 생성·이동·대입·변경 함수 시작마다 프로세스 전역 단조 계수에서 새 값을
    // 받는다(NextMutationEpoch). 노드 묶음이 모은 뒤의 변경과 다른 트리 상태를 검출한다.
    uint64 mutationEpoch_ = 0;

    // 6.9절 위반 검출(Core AccessOverlapChecker)이다. 읽기 범위(SpatialReadScope)는 여럿·중첩 허용,
    // 변경 함수(WriterGuard)는 단독이며 재귀하지 않는다(ReadsOnly). 이동·대입 대상은 새 검출기를
    // 쓰고 원본은 CheckNoOpenScope로 열린 범위가 없음을 확인한다.
    AccessOverlapChecker access_{AccessRecursion::ReadsOnly, detail::LooseOctreeAccessMessages};
};

// 읽기 단계의 검색 창구다. SpatialReadScope에서만 얻으며 그 범위가 살아 있는 동안만 쓴다. 복사해
// 여러 스레드에 넘겨도 된다. 범위 밖에 저장하거나 yield 경계를 넘겨 들고 있지 않는다.
//
// 모든 질의는 프록시 AABB와 질의 형상의 교차 후보를 돌려준다. 정확한 AABB 교차 집합을 빠짐없이
// 포함한다. 박스 질의는 float 상대 경계 때문에 초과 후보가 있을 수 있다(노드 level ℓ의 확장 경계 반
// 변 k·h_ℓ에 대한 float ULP 몇 개 폭, 6.8절). 구·볼록 영역·ray 질의는 정확한 double AABB에 Core
// 술어(Overlap·Raycast)를 적용하므로 후보가 그 술어의 결과와 같다(초과 후보 없음, 술어 자체의
// 반올림 한계는 Intersection.h 설명을 따른다). 필터는 공간 판정보다 먼저 적용하며 질의 mask 0은
// 아무것도 돌려주지 않는다(오류 아님). 질의 형상이 유효하지 않으면 아무것도 방문하지 않고
// InvalidArgument다. 검색은 힙 할당을 하지 않는다.
class LooseOctreeReader {
public:
    LooseOctreeReader(const LooseOctreeReader&) noexcept = default;
    LooseOctreeReader& operator=(const LooseOctreeReader&) noexcept = default;

    // box와 교차하는 후보마다 visitor(const SpatialProxyHit&)를 부른다. visitor는 QueryControl을
    // 돌려주며 Stop이면 즉시 끝낸다(stopped = true). visitor 안에서 같은 트리를 다시 검색해도
    // 되고(스택을 하나 더 쓴다), 변경하면 Fatal이다. stats가 있으면 계수를 더한다.
    //
    // 사용 예:
    //   auto queried = reader.QueryBox(bounds, mask, [&](const SpatialProxyHit& hit) noexcept {
    //       Touch(hit.userValue);
    //       return QueryControl::Continue;
    //   });
    template <typename Visitor>
    [[nodiscard]] Result<QueryOutcome> QueryBox(const Box<double>& box, uint32 filterMask,
                                                Visitor&& visitor,
                                                QueryStats* stats = nullptr) const noexcept {
        if (!box.IsValid()) {
            return Result<QueryOutcome>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<QueryOutcome>::Success(
            tree_->RunQuery(detail::BoxQueryShape{box}, filterMask, visitor, stats));
    }

    // sphere와 겹치는(중심에서 AABB까지 거리 ≤ 반지름, 접촉 포함) 프록시마다 visitor를 부른다.
    // 계약은 QueryBox와 같다. 후보는 정확한 double AABB에 Overlap(sphere, box)를 적용한 결과와 같다
    // (초과 후보 없음). 오류: sphere가 유효하지 않으면(비유한, 음수 반지름) InvalidArgument.
    template <typename Visitor>
    [[nodiscard]] Result<QueryOutcome> QuerySphere(const Sphere<double>& sphere, uint32 filterMask,
                                                   Visitor&& visitor,
                                                   QueryStats* stats = nullptr) const noexcept {
        if (!sphere.IsValid()) {
            return Result<QueryOutcome>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<QueryOutcome>::Success(
            tree_->RunQuery(detail::SphereQueryShape{sphere}, filterMask, visitor, stats));
    }

    // 볼록 영역(안쪽 normal 평면 집합, 프러스텀 등)의 어느 평면에 대해서도 엄격히 바깥이 아닌
    // AABB의 프록시마다 visitor를 부른다(6.8절 술어, 모서리 근처에서 정확한 볼록체 교차보다 넓다).
    // 후보는 Overlap(volume, box)와 같다. 오류: volume이 유효하지 않으면(평면 없음, 잘못된 평면)
    // InvalidArgument. volume은 호출 동안만 읽는다.
    template <typename Visitor>
    [[nodiscard]] Result<QueryOutcome> QueryFrustum(const ConvexVolume<double>& volume,
                                                    uint32 filterMask, Visitor&& visitor,
                                                    QueryStats* stats = nullptr) const noexcept {
        if (!volume.IsValid()) {
            return Result<QueryOutcome>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<QueryOutcome>::Success(
            tree_->RunQuery(detail::VolumeQueryShape{&volume}, filterMask, visitor, stats));
    }

    // ray의 [0, maxDistance] 구간이 AABB와 닿는(Raycast Hit, 시작점 내부·접촉 포함) 프록시마다
    // visitor를 부른다. 결과는 거리순이 아니다. 노드는 확장 경계 진입 거리가 가까운 자식부터(같으면
    // 자식 번호 오름차순, 원점이 여러 확장 경계 안이면 모두 0) 방문한다. maxDistance는 +Inf를
    // 허용한다. 오류: ray가 유효하지 않거나(Ray::IsValid) maxDistance가 음수·NaN이면
    // InvalidArgument.
    template <typename Visitor>
    [[nodiscard]] Result<QueryOutcome> QueryRay(const Ray<double>& ray, double maxDistance,
                                                uint32 filterMask, Visitor&& visitor,
                                                QueryStats* stats = nullptr) const noexcept {
        if (!IsValidRay(ray, maxDistance)) {
            return Result<QueryOutcome>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<QueryOutcome>::Success(
            tree_->RunQuery(detail::RayQueryShape{ray, maxDistance}, filterMask, visitor, stats));
    }

    // ray에 가장 가까운 프록시를 정밀 판정 콜백으로 찾는다(6.8절). 후보마다
    // hitTest(const SpatialProxyHit&, double currentMax)를 불러 RayHitResult를 받는다.
    // - 전제: hitTest가 돌려주는 거리는 Core Raycast(ray, 프록시 AABB, maxDistance)가 계산한 진입
    //   거리 이상이다(정밀 형상이 등록 AABB 안에 있음). 가지치기가 이 진입 거리로 하므로, 정밀
    //   거리를 다른 식으로 계산해 그보다 1–2 ULP 작을 수 있으면 그 진입 거리로 clamp해 돌려준다.
    //   어기면 답을 놓칠 수 있다.
    // - hit이 참이고 거리가 현재 최단보다 작을 때만(처음 후보는 maxDistance 이하일 때) 최단을
    //   줄인다. 같은 거리면 먼저 찾은 쪽이 남는다. NaN·음수·maxDistance 초과 거리는 받지 않는다.
    // - 노드는 확장 경계 진입 거리가 현재 최단보다 엄격히 크면 건너뛰고, 프록시는 AABB 진입 거리가
    //   현재 최단보다 엄격히 크면 hitTest를 부르지 않는다. AABB 진입 거리만으로 최단을 줄이지
    //   않는다.
    // - stats.results는 hitTest 호출 수다.
    // 오류는 QueryRay와 같다.
    //
    // 사용 예:
    //   auto nearest = reader.QueryRayNearest(ray, 10000.0, mask,
    //       [&](const SpatialProxyHit& hit, double currentMax) noexcept {
    //           return TraceMesh(hit.userValue, ray, currentMax);   // RayHitResult
    //       });
    template <typename HitTest>
    [[nodiscard]] Result<NearestHit> QueryRayNearest(const Ray<double>& ray, double maxDistance,
                                                     uint32 filterMask, HitTest&& hitTest,
                                                     QueryStats* stats = nullptr) const noexcept {
        if (!IsValidRay(ray, maxDistance)) {
            return Result<NearestHit>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<NearestHit>::Success(
            tree_->RunNearest(detail::RayQueryShape{ray, maxDistance}, filterMask, hitTest, stats));
    }

    // 후보를 out에 들어가는 만큼 쓰고 끝까지 세어 total을 돌려준다(결과를 조용히 버리지 않는다).
    // Any*는 첫 후보에서 끝나고 Count*는 결과를 내지 않고 센다. 오류는 같은 형상의 Query*와 같다.
    [[nodiscard]] Result<BufferQueryOutcome> QueryBoxInto(const Box<double>& box, uint32 filterMask,
                                                          Span<SpatialProxyHit> out) const noexcept;
    [[nodiscard]] Result<bool> AnyInBox(const Box<double>& box, uint32 filterMask) const noexcept;
    [[nodiscard]] Result<uint64> CountInBox(const Box<double>& box,
                                            uint32 filterMask) const noexcept;
    [[nodiscard]] Result<BufferQueryOutcome>
    QuerySphereInto(const Sphere<double>& sphere, uint32 filterMask,
                    Span<SpatialProxyHit> out) const noexcept;
    [[nodiscard]] Result<bool> AnyInSphere(const Sphere<double>& sphere,
                                           uint32 filterMask) const noexcept;
    [[nodiscard]] Result<uint64> CountInSphere(const Sphere<double>& sphere,
                                               uint32 filterMask) const noexcept;
    [[nodiscard]] Result<BufferQueryOutcome>
    QueryFrustumInto(const ConvexVolume<double>& volume, uint32 filterMask,
                     Span<SpatialProxyHit> out) const noexcept;
    [[nodiscard]] Result<bool> AnyInFrustum(const ConvexVolume<double>& volume,
                                            uint32 filterMask) const noexcept;
    [[nodiscard]] Result<uint64> CountInFrustum(const ConvexVolume<double>& volume,
                                                uint32 filterMask) const noexcept;
    [[nodiscard]] Result<BufferQueryOutcome> QueryRayInto(const Ray<double>& ray,
                                                          double maxDistance, uint32 filterMask,
                                                          Span<SpatialProxyHit> out) const noexcept;
    [[nodiscard]] Result<bool> AnyOnRay(const Ray<double>& ray, double maxDistance,
                                        uint32 filterMask) const noexcept;
    [[nodiscard]] Result<uint64> CountOnRay(const Ray<double>& ray, double maxDistance,
                                            uint32 filterMask) const noexcept;

    // 형상과 교차하는 노드를 직접 저장 프록시 단위 묶음으로 모은다(6.1절, 그림자 caster 수집처럼
    // 결과가 많은 질의를 worker에 나눌 때). out에 들어가는 만큼 쓰고 끝까지 세어 total을 돌려준다.
    // 범위 밖 목록이 비어 있지 않으면 마지막 묶음이다. 묶음 순서는 단일 질의의 방문 순서와 같으므로
    // 묶음별 QueryBatch 결과를 순서대로 이으면 같은 형상의 단일 질의 결과와 같다(필터는
    // QueryBatch가 적용). 묶음은 모은 트리의 이번 읽기 단계 안에서만 유효하다. 오류: 형상이
    // 유효하지 않으면 InvalidArgument.
    //
    // 사용 예(worker 분배):
    //   SpatialNodeBatch batches[256];
    //   auto collected = reader.CollectNodeBatches(casterVolume, Span<SpatialNodeBatch>(batches));
    //   // 각 worker가 자기 몫의 batches[i]마다:
    //   reader.QueryBatch(batches[i], casterVolume, mask, visitor);
    [[nodiscard]] Result<BufferQueryOutcome>
    CollectNodeBatches(const Box<double>& box, Span<SpatialNodeBatch> out) const noexcept;
    [[nodiscard]] Result<BufferQueryOutcome>
    CollectNodeBatches(const Sphere<double>& sphere, Span<SpatialNodeBatch> out) const noexcept;
    [[nodiscard]] Result<BufferQueryOutcome>
    CollectNodeBatches(const ConvexVolume<double>& volume,
                       Span<SpatialNodeBatch> out) const noexcept;

    // 묶음 하나의 후보마다 visitor를 부른다. 완전 포함 묶음은 공간 판정을 생략하고 필터만 적용한다.
    // 여러 스레드가 서로 다른 묶음을 동시에 처리해도 된다(검색은 트리를 바꾸지 않는다). 형상은
    // CollectNodeBatches에 준 것과 같아야 한다(종류만 검사한다). 오류: 다른 트리·다른 종류의
    // 형상으로 모은 묶음이거나 형상이 유효하지 않으면 InvalidArgument, 모은 뒤 트리가 변경되었으면
    // StaleHandle.
    template <typename Visitor>
    [[nodiscard]] Result<QueryOutcome>
    QueryBatch(const SpatialNodeBatch& batch, const Box<double>& box, uint32 filterMask,
               Visitor&& visitor, QueryStats* stats = nullptr) const noexcept {
        if (!box.IsValid()) {
            return Result<QueryOutcome>::Failure(ErrorCode::InvalidArgument);
        }
        return tree_->RunBatch(batch, detail::BoxQueryShape{box}, filterMask, visitor, stats);
    }

    template <typename Visitor>
    [[nodiscard]] Result<QueryOutcome>
    QueryBatch(const SpatialNodeBatch& batch, const Sphere<double>& sphere, uint32 filterMask,
               Visitor&& visitor, QueryStats* stats = nullptr) const noexcept {
        if (!sphere.IsValid()) {
            return Result<QueryOutcome>::Failure(ErrorCode::InvalidArgument);
        }
        return tree_->RunBatch(batch, detail::SphereQueryShape{sphere}, filterMask, visitor, stats);
    }

    template <typename Visitor>
    [[nodiscard]] Result<QueryOutcome>
    QueryBatch(const SpatialNodeBatch& batch, const ConvexVolume<double>& volume, uint32 filterMask,
               Visitor&& visitor, QueryStats* stats = nullptr) const noexcept {
        if (!volume.IsValid()) {
            return Result<QueryOutcome>::Failure(ErrorCode::InvalidArgument);
        }
        return tree_->RunBatch(batch, detail::VolumeQueryShape{&volume}, filterMask, visitor,
                               stats);
    }

    // handle이 살아 있는 프록시를 가리키는지다.
    [[nodiscard]] bool IsValid(SpatialProxyHandle handle) const noexcept;

    // 프록시의 지금 내용이다. 오류: 빈 handle은 InvalidArgument, 낡은 handle은 StaleHandle.
    [[nodiscard]] Result<SpatialProxyDesc> TryGet(SpatialProxyHandle handle) const noexcept;

    // 살아 있는 프록시 수(범위 밖 목록 포함)다.
    [[nodiscard]] uint32 Count() const noexcept;

    // 구조·메모리·누적 계수다(6.10절). O(노드 수)다.
    [[nodiscard]] OctreeStats Stats() const noexcept;

    // 6.2절 불변 조건 1–9와 저장 구조의 일관성을 모두 검사하고 첫 위반을 report에 담는다
    // (DebugAllocator::Validate 관례). 지금 구현은 할당하지 않으므로 호출 자체는 실패하지 않는다.
    // 손상 여부는 Value().IsValid()로 확인한다. O(노드 + 프록시)이므로 시험과 진단에서만 부른다.
    [[nodiscard]] Result<OctreeValidationReport> Validate() const noexcept;

    // 트리의 모든 노드를 깊이 우선(자식 0–7 순)으로 visitor(const SpatialNodeInfo&)에 넘긴다.
    // 디버그 그리기·통계용이다. visitor 안에서 같은 트리를 변경하면 Fatal이다.
    template <typename NodeVisitor>
    void VisitNodes(NodeVisitor&& visitor) const noexcept {
        struct Pending {
            uint32 node;
            uint32 depth;
        };
        InlineArray<Pending, LooseOctreeSearchStackCapacity> stack;
        stack.PushBack(Pending{detail::RootNode, 1});
        while (!stack.IsEmpty()) {
            const Pending current = stack.PopBack();
            const SpatialNodeInfo info = tree_->DescribeNode(current.node, current.depth);
            visitor(static_cast<const SpatialNodeInfo&>(info));
            const uint32 block = tree_->Search(current.node).childBlock;
            if (block != detail::NoBlock) {
                for (uint32 child = 8; child != 0; --child) {
                    stack.PushBack(
                        Pending{LooseOctree::ChildNode(block, child - 1), current.depth + 1});
                }
            }
        }
    }

private:
    friend class SpatialReadScope;

    explicit LooseOctreeReader(const LooseOctree& tree) noexcept : tree_(&tree) {
    }

    // ray 질의 입력 검사다. maxDistance는 0 이상(+Inf 허용)이어야 하고 NaN은 거부한다.
    static bool IsValidRay(const Ray<double>& ray, double maxDistance) noexcept {
        return ray.IsValid() && maxDistance >= 0.0;
    }

    // Into·Any·Count의 공통 구현이다(LooseOctreeQuery.cpp).
    template <typename Shape>
    BufferQueryOutcome IntoShape(const Shape& shape, uint32 filterMask,
                                 Span<SpatialProxyHit> out) const noexcept;
    template <typename Shape>
    bool AnyShape(const Shape& shape, uint32 filterMask) const noexcept;
    template <typename Shape>
    uint64 CountShape(const Shape& shape, uint32 filterMask) const noexcept;

    const LooseOctree* tree_;
};

// 읽기 단계 하나(예: 한 프레임의 질의 작업 전체)를 여는 범위다(6.9절). 트리의 접근 검출기(Core
// AccessOverlapChecker)에 읽기 범위를 연다: 변경 중이면 platform::Fatal이고, 범위가 살아 있는 동안
// 트리를 변경하면 그 변경 또는 이 범위의 종료가 Fatal이다. 이동·파괴도 Fatal이다. 같은 스레드에서
// 겹쳐 열어도 된다(visitor 안의 재검색). 검색 호출마다 atomic을 건드리지 않도록 범위는
// 질의 단위가 아니라 읽기 묶음 단위로 둔다. 범위는 복사·이동할 수 없고 만든 스레드에서
// 끝낸다(reader는 복사해 다른 스레드에 넘길 수 있다).
class SpatialReadScope {
public:
    explicit SpatialReadScope(const LooseOctree& tree) noexcept;
    ~SpatialReadScope() noexcept;

    SpatialReadScope(const SpatialReadScope&) = delete;
    SpatialReadScope& operator=(const SpatialReadScope&) = delete;
    SpatialReadScope(SpatialReadScope&&) = delete;
    SpatialReadScope& operator=(SpatialReadScope&&) = delete;

    // 이 범위의 reader다. 범위보다 오래 쓰지 않는다.
    [[nodiscard]] LooseOctreeReader Reader() const noexcept {
        return LooseOctreeReader(*tree_);
    }

private:
    const LooseOctree* tree_;
    ReadAccessScope access_;
};

// ---- 템플릿 구현 ----

inline const detail::SearchNode& LooseOctree::Search(uint32 node) const noexcept {
    return node == detail::RootNode ? rootSearch_
                                    : searchBlocks_[BlockOf(node)].nodes[ChildSlotOf(node)];
}

inline uint32 LooseOctree::ChildNode(uint32 block, uint32 child) noexcept {
    return 1 + block * 8 + child;
}

inline uint32 LooseOctree::BlockOf(uint32 node) noexcept {
    return (node - 1) / 8;
}

inline uint32 LooseOctree::ChildSlotOf(uint32 node) noexcept {
    return (node - 1) % 8;
}

inline double LooseOctree::CenterCoordinate(uint32 axis, uint32 level,
                                            int64 origin) const noexcept {
    // 중심 = anchor + (2 · origin + 2^level) · minCellSize / 2. 정수 부분은 2^53 미만이고 곱하는
    // 값이 2의 거듭제곱이라 곱이 정확하며, 격자 범위 검사(TryCreate)가 합의 정확성도 보장한다.
    const int64 doubled = origin * 2 + (int64{1} << level);
    return config_.anchor[axis] + static_cast<double>(doubled) * halfMinCell_;
}

inline Vec3d LooseOctree::CenterOf(uint32 level, const int64 (&origin)[3]) const noexcept {
    return Vec3d(CenterCoordinate(0, level, origin[0]), CenterCoordinate(1, level, origin[1]),
                 CenterCoordinate(2, level, origin[2]));
}

inline Box<double> LooseOctree::LooseBoundsOf(uint32 level,
                                              const int64 (&origin)[3]) const noexcept {
    // 삽입·갱신·검색·Validate가 모두 이 식 하나로 확장 경계를 계산한다(6.2절). 중심 ± 반 변은 각각
    // 한 번 반올림되고 반올림은 단조이므로 자식의 확장 경계는 부모의 것에 포함된다(불변 조건 2).
    const Vec3d center = CenterOf(level, origin);
    const Vec3d half = Vec3d::Splat(looseHalf_[level]);
    return Box<double>(center - half, center + half);
}

inline void LooseOctree::ChildOrigin(uint32 level, const int64 (&origin)[3], uint32 child,
                                     int64 (&out)[3]) noexcept {
    const int64 half = int64{1} << (level - 1);
    for (uint32 axis = 0; axis < 3; ++axis) {
        out[axis] = origin[axis] + (((child >> axis) & 1u) != 0 ? half : 0);
    }
}

// 질의 형상으로 트리를 깊이 우선 순회한다. 노드는 쌓을 때 분류해 제외 노드를 쌓지 않는다. 완전 포함
// 노드의 자식은 분류 없이 완전 포함이다. Shape::Ordered(ray)이면 형제를 진입 거리 오름차순(같으면
// 자식 번호 오름차순)으로 꺼낸다. policy.SkipEntry(진입 거리)가 참인 노드는 쌓을 때와 꺼낼 때 모두
// 버린다(최근접 ray의 가지치기). 직접 저장 프록시가 있는 노드마다 policy.OnNode를 부르고, 거짓이면
// 순회를 끝낸다.
template <typename Shape, typename Policy>
void LooseOctree::Walk(const Shape& shape, Policy& policy, QueryStats* stats) const noexcept {
    if (rootSearch_.subtreeCount == 0) {
        return;
    }
    detail::SearchStackEntry root;
    root.origin[0] = rootManage_.origin[0];
    root.origin[1] = rootManage_.origin[1];
    root.origin[2] = rootManage_.origin[2];
    root.node = detail::RootNode;
    root.level = rootManage_.level;
    if (stats != nullptr) {
        ++stats->visitedNodes;
    }
    const detail::NodeClass rootClass =
        shape.Classify(LooseBoundsOf(root.level, root.origin), root.entry);
    if (rootClass == detail::NodeClass::Outside || policy.SkipEntry(root.entry)) {
        return;
    }
    root.contained = rootClass == detail::NodeClass::Inside;

    InlineArray<detail::SearchStackEntry, LooseOctreeSearchStackCapacity> stack;
    stack.PushBack(root);
    while (!stack.IsEmpty()) {
        const detail::SearchStackEntry current = stack.PopBack();
        if (policy.SkipEntry(current.entry)) {
            continue;
        }
        const detail::SearchNode& node = Search(current.node);
        if (node.directCount != 0 &&
            !policy.OnNode(current.node, current.level, current.origin, current.contained)) {
            return;
        }
        if (node.childBlock == detail::NoBlock) {
            continue;
        }

        const detail::SearchBlock& block = searchBlocks_[node.childBlock];
        detail::SearchStackEntry children[8];
        uint32 count = 0;
        for (uint32 child = 0; child < 8; ++child) {
            // 서브트리가 빈 자식은 건너뛴다(점유 마스크와 같은 정보, 검색용 데이터만 읽는다).
            if (block.nodes[child].subtreeCount == 0) {
                continue;
            }
            detail::SearchStackEntry& next = children[count];
            ChildOrigin(current.level, current.origin, child, next.origin);
            next.node = ChildNode(node.childBlock, child);
            next.level = static_cast<uint8>(current.level - 1);
            next.contained = current.contained;
            next.entry = current.entry;
            if (!current.contained) {
                if (stats != nullptr) {
                    ++stats->visitedNodes;
                }
                const detail::NodeClass childClass =
                    shape.Classify(LooseBoundsOf(next.level, next.origin), next.entry);
                if (childClass == detail::NodeClass::Outside) {
                    continue;
                }
                next.contained = childClass == detail::NodeClass::Inside;
            }
            if (policy.SkipEntry(next.entry)) {
                continue;
            }
            ++count;
        }
        if constexpr (Shape::Ordered) {
            // 진입 거리 오름차순, 같으면 자식 번호 오름차순(삽입 정렬은 안정적이고 자식은
            // 번호순으로 모였다).
            for (uint32 index = 1; index < count; ++index) {
                const detail::SearchStackEntry moving = children[index];
                uint32 position = index;
                while (position != 0 && children[position - 1].entry > moving.entry) {
                    children[position] = children[position - 1];
                    --position;
                }
                children[position] = moving;
            }
        }
        // 먼저 볼 자식이 먼저 나오도록 역순으로 쌓는다.
        for (uint32 index = count; index != 0; --index) {
            stack.PushBack(children[index - 1]);
        }
    }
}

// 노드 하나의 직접 저장 프록시 중 후보를 emit(handle, userValue, filter, 진입 거리)에 넘긴다.
// emit이 거짓을 돌려주면 멈추고 거짓을 돌려준다. 필터를 먼저 적용한다. 완전 포함이면 공간 판정을
// 생략한다. 박스는 저장 float 상대 경계로 판정한다(질의를 확장 경계로 자른 뒤 노드 중심 기준
// float로 바깥쪽 반올림 — 저장 경계도 바깥쪽이므로 정확한 double 교차를 놓치지 않는다). 구·볼록
// 영역·ray는 레코드의 정확한 double AABB에 Core 술어(Overlap·Raycast)를 그대로 적용한다(6.8절 대안,
// 후보가 정답 술어와 정확히 같다).
template <typename Shape, typename Emit>
bool LooseOctree::ForEachNodeCandidate(uint32 nodeIndex, uint32 level, const int64 (&origin)[3],
                                       bool contained, const Shape& shape, uint32 filterMask,
                                       QueryStats* stats, Emit& emit) const noexcept {
    const detail::SearchNode& node = Search(nodeIndex);
    Box<float> query;
    if constexpr (Shape::UsesChunkBounds) {
        if (!contained) {
            const Box<double> loose = LooseBoundsOf(level, origin);
            const Vec3d center = CenterOf(level, origin);
            const Box<double> clipped(ComponentMax(shape.box.min, loose.min),
                                      ComponentMin(shape.box.max, loose.max));
            query = Box<double>(clipped.min - center, clipped.max - center).ToFloatOutward();
        }
    }
    for (uint32 chunkIndex = node.firstChunk; chunkIndex != detail::NoChunk;) {
        const detail::ElementChunk& chunk = chunks_[chunkIndex];
        for (uint32 entry = 0; entry < chunk.count; ++entry) {
            const uint32 filter = chunk.filter[entry];
            if ((filter & filterMask) == 0) {
                continue;
            }
            double proxyEntry = 0.0;
            if (contained) {
                if (stats != nullptr) {
                    ++stats->containedProxies;
                }
            } else {
                if (stats != nullptr) {
                    ++stats->testedProxies;
                }
                if constexpr (Shape::UsesChunkBounds) {
                    if (chunk.minX[entry] > query.max.x || chunk.maxX[entry] < query.min.x ||
                        chunk.minY[entry] > query.max.y || chunk.maxY[entry] < query.min.y ||
                        chunk.minZ[entry] > query.max.z || chunk.maxZ[entry] < query.min.z) {
                        continue;
                    }
                } else {
                    if (!shape.Test(records_.ElementAt(chunk.slot[entry]).bounds, proxyEntry)) {
                        continue;
                    }
                }
            }
            const SpatialProxyHandle handle{SlotHandle{chunk.slot[entry], chunk.generation[entry]}};
            if (!emit(handle, chunk.userValue[entry], filter, proxyEntry)) {
                return false;
            }
        }
        chunkIndex = chunk.next;
    }
    return true;
}

// 범위 밖 목록의 후보다. 기준 노드가 없으므로 모든 형상을 정확한 double 경계로 판정한다.
template <typename Shape, typename Emit>
bool LooseOctree::ForEachOutOfRangeCandidate(const Shape& shape, uint32 filterMask,
                                             QueryStats* stats, Emit& emit) const noexcept {
    for (usize index = 0; index < outOfRange_.Size(); ++index) {
        const detail::OutOfRangeEntry& entry = outOfRange_[index];
        if ((entry.filter & filterMask) == 0) {
            continue;
        }
        if (stats != nullptr) {
            ++stats->testedProxies;
        }
        double proxyEntry = 0.0;
        if (!shape.Test(entry.bounds, proxyEntry)) {
            continue;
        }
        const SpatialProxyHandle handle{SlotHandle{entry.slot, entry.generation}};
        if (!emit(handle, entry.userValue, entry.filter, proxyEntry)) {
            return false;
        }
    }
    return true;
}

template <typename Shape, typename Sink>
QueryOutcome LooseOctree::RunQuery(const Shape& shape, uint32 filterMask, Sink& sink,
                                   QueryStats* stats) const noexcept {
    QueryOutcome outcome;
    if (filterMask == 0) {
        return outcome;
    }
    auto emit = [&](SpatialProxyHandle handle, uint64 userValue, uint32 filter, double) noexcept {
        SpatialProxyHit hit;
        hit.handle = handle;
        hit.userValue = userValue;
        hit.filterMask = filter;
        ++outcome.visited;
        if (stats != nullptr) {
            ++stats->results;
        }
        if (sink(static_cast<const SpatialProxyHit&>(hit)) == QueryControl::Stop) {
            outcome.stopped = true;
            return false;
        }
        return true;
    };
    struct Policy {
        const LooseOctree* tree;
        const Shape* shape;
        uint32 filterMask;
        QueryStats* stats;
        decltype(emit)* emit;

        bool SkipEntry(double) const noexcept {
            return false;
        }

        bool OnNode(uint32 node, uint32 level, const int64 (&origin)[3], bool contained) noexcept {
            return tree->ForEachNodeCandidate(node, level, origin, contained, *shape, filterMask,
                                              stats, *emit);
        }
    };
    Policy policy{this, &shape, filterMask, stats, &emit};
    Walk(shape, policy, stats);
    if (!outcome.stopped) {
        (void)ForEachOutOfRangeCandidate(shape, filterMask, stats, emit);
    }
    return outcome;
}

// 최근접 ray(6.8절). 후보마다 hitTest(hit, 현재 최단 거리)를 부르고, hit이 참이며 거리가 현재
// 최단보다 작을 때만(처음은 maxDistance 이하일 때) 줄인다. 노드는 double 확장 경계 진입 거리가 현재
// 최단보다 엄격히 크면 쌓을 때와 꺼낼 때 모두 버리고, 프록시는 정확한 AABB 진입 거리가 현재
// 최단보다 엄격히 크면 hitTest를 부르지 않는다. 진입 거리는 단조 식(Raycast)이라 노드 진입 ≤ 그 안
// 프록시 진입이므로 가지치기가 답을 놓치지 않는다(전제: hitTest 거리 ≥ 프록시 AABB 진입 거리). AABB
// 진입 거리만으로는 최단을 줄이지 않는다.
template <typename HitTest>
NearestHit LooseOctree::RunNearest(const detail::RayQueryShape& shape, uint32 filterMask,
                                   HitTest& hitTest, QueryStats* stats) const noexcept {
    NearestHit nearest;
    if (filterMask == 0) {
        return nearest;
    }
    double best = shape.maxDistance;
    auto emit = [&](SpatialProxyHandle handle, uint64 userValue, uint32 filter,
                    double entry) noexcept {
        if (entry > best) {
            return true;
        }
        SpatialProxyHit hit;
        hit.handle = handle;
        hit.userValue = userValue;
        hit.filterMask = filter;
        if (stats != nullptr) {
            ++stats->results;
        }
        const RayHitResult result = hitTest(static_cast<const SpatialProxyHit&>(hit), best);
        // NaN·음수·maxDistance 초과 거리는 계약 위반이므로 받지 않는다.
        if (result.hit && result.distance >= 0.0 && result.distance <= shape.maxDistance &&
            (!nearest.found || result.distance < best)) {
            nearest.found = true;
            nearest.proxy = hit;
            nearest.distance = result.distance;
            best = result.distance;
        }
        return true;
    };
    struct Policy {
        const LooseOctree* tree;
        const detail::RayQueryShape* shape;
        uint32 filterMask;
        QueryStats* stats;
        decltype(emit)* emit;
        const double* best;

        bool SkipEntry(double entry) const noexcept {
            return entry > *best;
        }

        bool OnNode(uint32 node, uint32 level, const int64 (&origin)[3], bool contained) noexcept {
            return tree->ForEachNodeCandidate(node, level, origin, contained, *shape, filterMask,
                                              stats, *emit);
        }
    };
    Policy policy{this, &shape, filterMask, stats, &emit, &best};
    Walk(shape, policy, stats);
    (void)ForEachOutOfRangeCandidate(shape, filterMask, stats, emit);
    return nearest;
}

template <typename Shape>
BufferQueryOutcome LooseOctree::CollectBatches(const Shape& shape,
                                               Span<SpatialNodeBatch> out) const noexcept {
    BufferQueryOutcome outcome;
    auto record = [&](uint32 node, uint32 proxyCount, bool contained) noexcept {
        if (outcome.total < out.Size()) {
            SpatialNodeBatch& batch = out[static_cast<usize>(outcome.total)];
            batch.node = node;
            batch.proxyCount = proxyCount;
            batch.contained = contained;
            batch.shape = Shape::Kind;
            batch.tree = this;
            batch.epoch = mutationEpoch_;
            ++outcome.written;
        }
        ++outcome.total;
    };
    struct Policy {
        const LooseOctree* tree;
        decltype(record)* record;

        bool SkipEntry(double) const noexcept {
            return false;
        }

        bool OnNode(uint32 node, uint32, const int64 (&)[3], bool contained) noexcept {
            (*record)(node, tree->Search(node).directCount, contained);
            return true;
        }
    };
    Policy policy{this, &record};
    Walk(shape, policy, nullptr);
    if (outOfRange_.Size() != 0) {
        record(LooseOctreeOutOfRangeBatch, static_cast<uint32>(outOfRange_.Size()), false);
    }
    outcome.truncated = outcome.total > out.Size();
    return outcome;
}

template <typename Shape, typename Sink>
Result<QueryOutcome> LooseOctree::RunBatch(const SpatialNodeBatch& batch, const Shape& shape,
                                           uint32 filterMask, Sink& sink,
                                           QueryStats* stats) const noexcept {
    if (batch.tree != this || batch.shape != Shape::Kind) {
        return Result<QueryOutcome>::Failure(ErrorCode::InvalidArgument);
    }
    // 모은 뒤 변경이 있었으면 노드 번호와 프록시 배치가 달라졌을 수 있다.
    if (batch.epoch != mutationEpoch_) {
        return Result<QueryOutcome>::Failure(ErrorCode::StaleHandle);
    }
    const bool outOfRange = batch.node == LooseOctreeOutOfRangeBatch;
    if (!outOfRange && batch.node != detail::RootNode &&
        !searchBlocks_.IsLive(BlockOf(batch.node))) {
        return Result<QueryOutcome>::Failure(ErrorCode::InvalidArgument);
    }
    QueryOutcome outcome;
    if (filterMask == 0) {
        return Result<QueryOutcome>::Success(outcome);
    }
    auto emit = [&](SpatialProxyHandle handle, uint64 userValue, uint32 filter, double) noexcept {
        SpatialProxyHit hit;
        hit.handle = handle;
        hit.userValue = userValue;
        hit.filterMask = filter;
        ++outcome.visited;
        if (stats != nullptr) {
            ++stats->results;
        }
        if (sink(static_cast<const SpatialProxyHit&>(hit)) == QueryControl::Stop) {
            outcome.stopped = true;
            return false;
        }
        return true;
    };
    if (outOfRange) {
        (void)ForEachOutOfRangeCandidate(shape, filterMask, stats, emit);
    } else {
        const detail::ManageNode& manage = Manage(batch.node);
        (void)ForEachNodeCandidate(batch.node, manage.level, manage.origin, batch.contained, shape,
                                   filterMask, stats, emit);
    }
    return Result<QueryOutcome>::Success(outcome);
}

} // namespace rocks::spatial
