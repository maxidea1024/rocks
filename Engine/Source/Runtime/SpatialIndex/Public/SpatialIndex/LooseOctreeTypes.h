#pragma once

#include <Base/NumericLimits.h>
#include <Base/Types.h>
#include <Core/Containers/SlotArray.h>
#include <Core/Math/Shapes.h>

// LooseOctree의 공개 값 타입이다(Docs/Design/SpatialIndex.ko.md 6절). 좌표·단위는 UE 5.8과
// 같다(왼손 +X 전방·+Y 오른쪽·+Z 위, cm). 모든 경계는 닫힌 집합이며 경계 접촉은 교차다.
namespace rocks::spatial {

// 요소 청크 하나에 담는 프록시 수다. 컴파일 시점 상수라 청크 크기가 고정되고 색인 풀의 원소가 된다
// (6.2절 끝). 측정에서 다른 값이 필요하면 템플릿 인자로 바꾼다.
inline constexpr uint32 LooseOctreeChunkCapacity = 16;

// 설정이 허용하는 maxRootLevel의 상한이다(6.2절 설정값 표). 검색 스택의 정적 용량이 이 값에서
// 나온다.
inline constexpr uint32 LooseOctreeLevelLimit = 31;

// 명시적 검색 스택의 정적 용량이다. 깊이 상한 D = maxRootLevel + 1에서 7 · (D − 1) + 1이
// 필요하다(6.8절 순회 방식). LooseOctreeLevelLimit에서 유도하므로 어떤 유효한 설정에서도 넘치지
// 않는다.
inline constexpr usize LooseOctreeSearchStackCapacity = 7 * LooseOctreeLevelLimit + 1;

// 프록시를 가리키는 세대 번호 handle이다. 발급한 LooseOctree에서만 의미가 있으며, 프록시를 지우거나
// Clear하면 낡은 handle(StaleHandle)이 된다. 다른 인스턴스가 발급한 handle은 검출하지 않는다.
// 기본값은 빈 handle이다.
struct SpatialProxyHandle {
    SlotHandle slot;

    [[nodiscard]] constexpr bool IsNull() const noexcept {
        return slot.generation == 0;
    }

    friend constexpr bool operator==(SpatialProxyHandle left, SpatialProxyHandle right) noexcept {
        return left.slot.index == right.slot.index && left.slot.generation == right.slot.generation;
    }
};

// 등록할 프록시의 내용이다.
struct SpatialProxyDesc {
    // 월드 AABB(cm). Box::IsValid()가 참이어야 한다(NaN·무한대·min > max는 InvalidArgument). 크기 0
    // (점·면)은 유효하다.
    Box<double> bounds;
    // 질의 mask와 AND가 0이 아닐 때만 후보가 된다. 0이면 어떤 질의에도 걸리지 않는다.
    uint32 filterMask = 0;
    // 호출자의 식별 값(EntityHandle 등)이다. 옥트리는 해석하지 않는다.
    uint64 userValue = 0;
};

// 질의 결과 하나다. visitor가 받는 참조는 그 호출 동안만 유효하다.
struct SpatialProxyHit {
    SpatialProxyHandle handle;
    uint64 userValue = 0;
    uint32 filterMask = 0;
};

// visitor의 반환값이다. Stop이면 순회를 즉시 끝낸다.
enum class QueryControl : uint8 {
    Continue,
    Stop
};

struct QueryOutcome {
    // visitor 호출 수다.
    uint32 visited = 0;
    // visitor가 Stop을 돌려줘 순회를 일찍 끝냈다.
    bool stopped = false;
};

struct BufferQueryOutcome {
    // out에 쓴 수다(out.Size() 이하).
    uint32 written = 0;
    // 전체 후보 수다. 버퍼가 모자라도 끝까지 센다.
    uint64 total = 0;
    // total > out.Size()이다. 결과를 조용히 버리지 않는다.
    bool truncated = false;
};

// QueryRayNearest의 hitTest가 돌려주는 정밀 판정 결과다. hit이 참이면 distance는 ray 원점에서 잰
// 거리(cm)이며 그 프록시 AABB의 ray 진입 거리 이상이어야 한다(6.8절 전제).
struct RayHitResult {
    bool hit = false;
    double distance = 0.0;
};

// QueryRayNearest의 결과다. found가 거짓이면 나머지는 의미가 없다.
struct NearestHit {
    bool found = false;
    SpatialProxyHit proxy;
    double distance = 0.0;
};

// 노드 묶음을 모은 질의 형상이다. QueryBatch가 같은 종류의 형상으로 불렸는지 확인한다.
enum class SpatialBatchShape : uint8 {
    Box,
    Sphere,
    ConvexVolume
};

// 노드 묶음 하나다(6.1절 CollectNodeBatches). 노드 하나의 직접 저장 프록시 또는 범위 밖 목록 전체를
// 가리킨다. 묶음끼리 프록시가 겹치지 않으므로 여러 worker가 QueryBatch로 나눠 처리해도 결과
// 합집합이 단일 질의와 같다. 모은 트리의 같은 읽기 단계 안에서만 유효하다(그 뒤 변경이 있으면
// QueryBatch가 StaleHandle). 필드는 진단·작업 분배용으로 읽을 수 있으나 바꾸지 않는다.
struct SpatialNodeBatch {
    // 노드 번호다. 범위 밖 목록 묶음이면 LooseOctreeOutOfRangeBatch, 큰 객체 목록 묶음이면
    // LooseOctreeLargeProxyBatch다.
    uint32 node = 0;
    // 묶음의 프록시 수(필터 적용 전)다. worker 분배의 무게로 쓴다.
    uint32 proxyCount = 0;
    // 노드 확장 경계가 형상 안에 완전히 들어가 프록시 공간 판정을 생략한다.
    bool contained = false;
    SpatialBatchShape shape = SpatialBatchShape::Box;
    // 모은 트리와 그때의 변경 세대다(QueryBatch가 확인한다).
    const void* tree = nullptr;
    uint64 epoch = 0;
};

// 질의 하나의 계수다. 호출자가 질의마다 넘기며 트리의 공유 상태에 쓰지 않으므로 병렬 검색에서
// 경합이 없다. 필드는 더하기만 하므로 여러 질의에 같은 구조체를 넘기면 합계가 된다.
struct QueryStats {
    // 형상과 판정한 노드 수(제외로 버린 노드 포함)다. 완전 포함 가지 아래의 노드는 판정 없이 세지
    // 않는다.
    uint64 visitedNodes = 0;
    // 공간 판정을 한 프록시 수다(박스는 float 상대 경계, 구·볼록 영역·ray는 정확한 double AABB에
    // Core 술어, 범위 밖 목록은 double).
    uint64 testedProxies = 0;
    // 완전 포함 노드에 있어 공간 판정을 생략한 프록시 수다.
    uint64 containedProxies = 0;
    // 후보로 낸 수다.
    uint64 results = 0;
};

// LooseOctree::TryCreate의 설정이다(6.2절 설정값 표). 길이는 cm다. 범위를 어기면 TryCreate가
// InvalidArgument를 돌려준다.
struct LooseOctreeConfig {
    // 격자 원점이자 초기 루트의 중심이다. minCellSize의 정수배여야 한다.
    Vec3d anchor = Vec3d(0.0, 0.0, 0.0);
    // level 0 기본 셀의 한 변이다. 2의 거듭제곱이고 float 최소 정규 값의 2^24배 이상이어야 한다.
    double minCellSize = 128.0;
    // 초기 루트 level이자 루트 축소의 하한이다. [1, maxRootLevel]. 기본 8(한 변 32,768 cm)은 OT6
    // 조합 측정(2026-11-19, 결정 8)에서 정했다. 데이터보다 훨씬 큰 초기 루트는 looseness가 작을 때
    // 크기 편차가 큰 분포의 구조를 나쁘게 했다.
    uint32 initialRootLevel = 8;
    // 루트 확장 한도다. LooseOctreeLevelLimit 이하. 최대 깊이는 maxRootLevel + 1이다.
    uint32 maxRootLevel = 24;
    // 확장 경계 배율 k다. [1, 2]에서 1/64 단위다(2진 유리수라 k · h가 정확하다). 기본 3/2는 OT6
    // 조합 측정(2026-11-19, 결정 8)에서 정했다(k = 2 대비 균일·군집·겹치는 과밀의 질의가 빨라지고
    // 크기 편차 분포가 나빠지지 않은 조합).
    double looseness = 1.5;
    // 잎의 분할 평가를 시작하는 직접 저장 수다(수용 한도가 아니다). 4 이상.
    uint32 splitThreshold = 16;
    // 병합 후보 기준(서브트리 프록시 수)이다. splitThreshold / 2(정수 나눗셈) 이하.
    uint32 mergeThreshold = 6;
    // 프록시 수 상한이다. 1 이상, SlotArray 한도 이하. 넘으면 TryAdd가 CapacityExceeded다.
    uint32 maxProxies = 1u << 20;
    // 핸들 세대 상한이다(SlotArray 규칙, 1 이상). 시험에서 retire를 검사하려고 낮춘다.
    uint32 maxProxyGeneration = NumericLimits<uint32>::Max();
    // 한 노드의 직접 저장 수가 이 값을 넘으면 진단 이벤트(경고 로그와 통계)를 낸다. 1 이상.
    uint32 parentWarnCount = 1024;
    // 범위 밖 목록이 이 값을 넘으면 진단 이벤트를 낸다. 1 이상.
    uint32 outOfRangeWarnCount = 64;
    // 유지보수 대기량 강제 처리 한도의 하한이다(한도 = max(이 값, 노드 수 / 4)). 1 이상.
    uint32 pendingLimitFloor = 64;
    // 큰 객체 목록의 기준 반 변(cm)이다(결정 8). 0이면 끔. 양수이면 AABB의 최대 반 변이 이 값보다
    // 큰 프록시를 트리 대신 큰 객체 목록(정확한 double 경계의 선형 목록, 모든 질의가 항상 판정)에
    // 둔다. 목록에서 트리로 돌아가는 것은 최대 반 변이 이 값의 LooseOctreeLargeProxyExitRatio배
    // 이하가 될 때다(이력: 기준 부근의 크기 변화가 목록과 트리를 오가지 않게 한다). 큰 객체는 루트
    // 크기에 영향을 주지 않는다. 0 이상의 유한값.
    double largeProxyHalfExtent = 0.0;
};

// 큰 객체 목록에서 트리로 돌아가는 크기 비율이다(largeProxyHalfExtent의 3/4).
inline constexpr double LooseOctreeLargeProxyExitRatio = 0.75;

// RunMaintenance의 예산이다(6.6절). operations와 timeNanoseconds가 모두 0이면 대기열을 처리하지
// 않고 루트 확인도 하지 않는다(유지보수를 미룸). shrink는 두 예산과 무관하게 수행한다.
struct MaintenanceBudget {
    // 처리할 대기열 항목 수의 상한이다. 결과가 결정적이다. 0이면 시간 예산만 본다.
    uint32 operations = 0;
    // 경과 시간 한도(ns)다. 0이면 시간 제한이 없다. 시계는 16개 항목마다 확인하므로 한도를 그만큼
    // 넘을 수 있고, 처리량이 실행마다 달라져 구조와 결과 순서가 결정적이지 않다.
    uint64 timeNanoseconds = 0;
    // 참이면 끝에 배열과 풀의 예약 용량을 사용량에 맞춘다(실패해도 내용은 그대로다).
    bool shrink = false;

    // 항목 count개를 처리하는 결정적 예산이다.
    static constexpr MaintenanceBudget Operations(uint32 count) noexcept {
        MaintenanceBudget budget;
        budget.operations = count;
        return budget;
    }

    // nanoseconds 동안 처리하는 예산이다(결정적이지 않다).
    static constexpr MaintenanceBudget Time(uint64 nanoseconds) noexcept {
        MaintenanceBudget budget;
        budget.timeNanoseconds = nanoseconds;
        return budget;
    }

    // 대기열을 모두 비우는 예산이다(시험·로딩 끝 등 시간 제약이 없는 곳).
    static constexpr MaintenanceBudget All() noexcept {
        return Operations(NumericLimits<uint32>::Max());
    }
};

// RunMaintenance 한 번의 결과다.
struct MaintenanceReport {
    // 꺼내 처리한 대기열 항목 수(조건이 사라져 버린 항목 포함)다.
    uint32 processed = 0;
    // 끝난 뒤 남은 대기열 길이다.
    uint32 remaining = 0;
    uint32 merges = 0;
    // 서브트리가 빈 자식 블록을 해제한 수다.
    uint32 reclaims = 0;
    uint32 reevaluations = 0;
    // 하강 재배치로 자식에 내린 프록시 수다.
    uint32 relocatedProxies = 0;
    uint32 rootShrinks = 0;
    // 프록시가 0개라 루트를 초기 상태로 되돌렸다.
    bool emptyReset = false;
    // 축소 요청을 수행했다.
    bool shrunk = false;
    uint64 elapsedNanoseconds = 0;
};

// 노드별 직접 저장 수 분포의 구간 수다. 구간 i(i ≥ 1)는 [4^(i−1), 4^i) 범위이고 구간 0은 0개,
// 마지막 구간은 그 이상 전부다.
inline constexpr uint32 OctreeDirectHistogramBuckets = 8;

// Stats()의 결과다(6.10절). 누적 계수는 생성 후(이동하면 함께 옮겨 감) 계속 늘어나며 Clear로도
// 지워지지 않는다.
struct OctreeStats {
    // ---- 현재 구조 ----
    uint32 proxyCount = 0;
    uint32 outOfRangeCount = 0;
    // 큰 객체 목록의 프록시 수다(largeProxyHalfExtent).
    uint32 largeProxyCount = 0;
    uint32 nodeCount = 0;
    uint32 leafCount = 0;
    // 루트가 깊이 1이다.
    uint32 maxDepth = 0;
    uint32 rootLevel = 0;
    uint32 maxDirectCount = 0;
    uint32 directHistogram[OctreeDirectHistogramBuckets] = {};
    // 잎이 아닌 노드에 직접 저장된 프록시 수다(내부 노드 저장 비율 = 이 값 / 트리 안 프록시 수).
    uint32 interiorDirectCount = 0;
    uint32 heldNodeCount = 0;
    // 직접 저장 수가 parentWarnCount를 넘는 노드 수다.
    uint32 overParentWarnNodeCount = 0;
    // ---- 메모리(byte) ----
    usize recordBytesUsed = 0;
    usize recordBytesReserved = 0;
    usize chunkBytesUsed = 0;
    usize chunkBytesReserved = 0;
    usize nodeBytesUsed = 0;
    usize nodeBytesReserved = 0;
    usize outOfRangeBytesUsed = 0;
    usize outOfRangeBytesReserved = 0;
    // 배열별 예약 용량의 피크(byte)다(생성 또는 이동 대상이 된 뒤부터).
    usize peakRecordBytesReserved = 0;
    usize peakChunkBytesReserved = 0;
    usize peakNodeBytesReserved = 0;
    usize peakOutOfRangeBytesReserved = 0;
    usize largeProxyBytesUsed = 0;
    usize largeProxyBytesReserved = 0;
    // 내부 컨테이너가 allocator에 요청한 할당·해제 횟수와 살아 있는·피크 byte(요청 크기 기준, 계수
    // adapter의 머리 정보 제외)다. 이동하면 함께 옮겨 가고 이동된 원본은 0부터 다시 센다.
    uint64 allocationCount = 0;
    uint64 deallocationCount = 0;
    usize liveAllocatedBytes = 0;
    usize peakAllocatedBytes = 0;
    // 풀 재사용 목록 길이다.
    uint32 freeChunkCount = 0;
    uint32 freeBlockCount = 0;
    // ---- 누적 계수 ----
    uint64 splits = 0;
    // 단일 자식 체인을 함께 만든 분할 수(splits에 포함)다.
    uint64 chainSplits = 0;
    uint64 merges = 0;
    uint64 reclaims = 0;
    // 경계 갱신에서 노드를 옮긴 수(범위 밖 목록과의 이동 포함)다.
    uint64 reinserts = 0;
    uint64 inPlaceUpdates = 0;
    // 하강 재배치로 내린 프록시 수다.
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
    // 범위 밖 목록에 넣은 횟수(추가·갱신)다.
    uint64 outOfRangeInsertions = 0;
    // 큰 객체 목록에 넣은 횟수(추가·갱신)다.
    uint64 largeProxyInsertions = 0;
    // ---- 유지보수 대기량 ----
    uint32 pendingTotal = 0;
    uint32 pendingReevaluations = 0;
    uint32 pendingRelocations = 0;
    // 대기열에 있으나 재평가·재배치 표시가 없는 항목(병합·회수 후보)이다.
    uint32 pendingStructural = 0;
};

// VisitNodes가 노드마다 넘기는 정보다. 디버그 그리기·통계용이며 그 호출 동안만 유효하다.
struct SpatialNodeInfo {
    uint32 node = 0;
    // 부모 노드 번호다. 루트는 LooseOctreeNoNode다.
    uint32 parent = 0;
    // 루트가 1이다.
    uint32 depth = 0;
    uint32 level = 0;
    // 기본 셀과 확장 경계(월드 cm)다.
    Box<double> cell;
    Box<double> loose;
    uint32 directCount = 0;
    uint32 subtreeCount = 0;
    bool leaf = false;
    bool held = false;
};

// 노드 번호 칸의 "없음" 표지다(SpatialNodeInfo::parent 등).
inline constexpr uint32 LooseOctreeNoNode = NumericLimits<uint32>::Max();

// 범위 밖 목록 전체를 가리키는 SpatialNodeBatch::node 값이다.
inline constexpr uint32 LooseOctreeOutOfRangeBatch = LooseOctreeNoNode - 1;

// 큰 객체 목록 전체를 가리키는 SpatialNodeBatch::node 값이다.
inline constexpr uint32 LooseOctreeLargeProxyBatch = LooseOctreeNoNode - 2;

// OctreeValidationReport::invariant 값이다. 1–9는 Docs/Design/SpatialIndex.ko.md 6.2절 불변 조건
// 번호이고, 그 밖의 저장 구조 일관성(청크 연결·사용 수, 풀 사용량과 도달 가능한 블록·청크 수, 범위
// 밖 목록, 레코드와 청크의 필터·사용자 값 대조)은 OctreeInvariantStorage로 구분한다.
inline constexpr uint32 OctreeInvariantNone = 0;
inline constexpr uint32 OctreeInvariantStorage = 10;

// LooseOctreeReader::Validate의 결과다(DebugAllocator::Validate와 같은 관례: Result는 검사 호출이
// 수행되었는지, 실제 손상 여부는 이 값이다). 첫 위반 하나만 담는다.
//
// 사용 예:
//   auto report = reader.Validate();
//   if (report && !report.Value().IsValid()) {
//       Log::Error(LogGame, "octree invariant {Invariant} broken at node {Node}",
//                  report.Value().invariant, report.Value().node);
//   }
struct OctreeValidationReport {
    // OctreeInvariantNone이면 위반이 없다. 1–9는 불변 조건 번호, OctreeInvariantStorage는 저장 구조
    // 일관성 위반이다.
    uint32 invariant = OctreeInvariantNone;
    // 위반을 발견한 노드 번호다. 노드에 속하지 않는 위반(범위 밖 목록·풀 사용량·전체 수)이면
    // LooseOctreeNoNode다.
    uint32 node = LooseOctreeNoNode;

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return invariant == OctreeInvariantNone;
    }
};

} // namespace rocks::spatial
