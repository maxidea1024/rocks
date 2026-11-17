#pragma once

#include <Base/NumericLimits.h>
#include <Object/ObjectIdentity.h>
#include <Object/ReferenceObserver.h>

// ObjectRegistry 생성 설정과 GC·통계 보고 값이다. GC 단계(GcPhase)와 slice 예산(GcBudget), Registry
// 생성 설정(RegistryOptions), 통계(ObjectStats), 한 cycle 결과(GcReport)와 증분 진행(GcProgress)을
// 둔다. Object/ObjectRegistry.h가 include한다.
namespace rocks {

class NamePool;
class WorkerScheduler;

enum class GcPhase : uint8 {
    Idle,
    Marking,
    Sweeping,
    Reclaiming
};

// 증분 수집 slice 하나의 예산이다(Docs/Design/GarbageCollectionScale.ko.md GC-S2). 기본은 시간
// 한도(1ms)이고 maxObjects는 상한 장치일 뿐이다.
// - 시간 예산은 soft deadline이다. Trace·소멸자 한 번을 중간에 중단하지 않고, 표식은 객체
//   128개마다·sweep은 chunk(64칸)마다·회수는 객체마다 시계를 본다. Begin의 root scan·잠금 획득·회수
//   후보 탐색도 hard deadline으로 제한하지 않는다.
// - maxObjects는 작업 단위의 합이다: trace한 객체 하나, 분류한 slot chunk(64칸) 하나, 회수한 객체
//   하나가 각각 1이다. 0은 잘못된 설정(InvalidArgument)이다.
// - maxNanoseconds=0은 시간 한도를 끄되 작업량 한도는 유지한다(시험의 결정적 진행).
//
// 사용 예(프레임마다 2ms):
//   registry.StepCollection(access, GcBudget{GcBudget::DefaultMaxObjects, 2000000});
struct GcBudget {
    static constexpr uint32 DefaultMaxObjects = 1u << 20;

    uint32 maxObjects = DefaultMaxObjects;
    uint64 maxNanoseconds = 1000000;
};

// 한도는 생성 시 고정한다. maxGeneration을 낮추면 슬롯 고갈 회귀 시험을 할 수 있다.
struct RegistryOptions {
    // 슬롯 수 상한이다. 기본값은 UE MaxUObjects 기본과 같은 2M이다(Docs/Design/
    // GarbageCollectionScale.ko.md 4절). 슬롯 저장소는 쓰는 만큼만 chunk·GC 페이지 단위로 늘어난다.
    uint32 maxSlots = 2097152;
    uint32 maxGeneration = NumericLimits<uint32>::Max();
    // 기본값은 scope 없는 객체 자체 호출에 Registry의 문맥을 사용한다. World/PIE dispatcher가
    // 실행 문맥을 설정하는 구성은 true로 하여 무인자 생명주기·타입 조회 API의 scope 누락을
    // 거부한다. 명시적 ObjectAccessContext를 받는 Registry API의 검증에는 영향을 주지 않는다.
    bool requireLifecycleScope = false;
    // 생성 뒤 바꾸지 않는 객체 정책이다(Policy()로 어느 스레드에서나 읽는다).
    RegistryPolicy policy;
    // 붙이면 패키지 사이 강한 간선 다중도 변화를 이 표에 기록한다(ReferenceObserver.h,
    // Docs/Design/ObjectFoundations.ko.md 3절). 패키지 정책 Registry에만 붙고, Registry보다 오래
    // 살아야 하며 Registry 하나에만 붙는다 — 정책 없는 Registry에 주거나 이미 붙은 관찰자를 주면
    // Registry 생성이 Fatal이다(생성자가 실패를 돌려줄 수 없다). Registry가 소멸하면 떨어진다.
    // 편집은 Registry API 지점에서, Commit·Discard는 타입 Trace로 세므로 Registry API로 쓰는 모든
    // 참조를 Trace가 방문해야 한다(패키지 정책 Registry가 완전한 필드 배치 타입만 만드는 이유다).
    ReferenceObserver* referenceObserver = nullptr;
    // 패키지 정책 Registry에서 Discard하지 않은(Alive) 객체가 GC로 Retire되면 Debug에서 Fatal로
    // 멈춘다(그 경로를 시험하는 Registry만 false로 둔다). 계수 ObjectStats::retiredWithoutDiscard는
    // 언제나 센다.
    bool assertRetiredWithoutDiscard = true;
    // Name 필드(ValueKind::Name — 리플렉션 B9c2)의 토큰 pool이다(빌림 — Registry보다 오래 살고 쓰는
    // 동안 실행 중이어야 한다, 생성 뒤 바꾸지 않는다). Engine이 pool 하나를 에셋·World Registry에
    // 같이 준다(예: options.names = &names). 없으면 Name 필드에 None만 둘 수 있다(None이 아닌
    // 토큰의 쓰기·저장·복원 Unsupported). 쓰기는 토큰 domain이 이 pool인지 보고(다르면
    // WrongContext) 그래프 복원 2a가 이 pool로 intern한다(ReflectionExtensions.ko.md 4·4.1절).
    NamePool* names = nullptr;
    // 클러스터 기능을 켠다(Docs/Design/GarbageCollectionScale.ko.md 13절). 켜면 슬롯마다 클러스터
    // 장부 칸(28 B)을 GC 페이지와 함께 두고, 엔진 계층(World)이 생성 트랜잭션에서 클러스터 루트를
    // 지정하고 가입시킬 수 있다(ConstructionContext::SetClusterRoot·JoinCluster). 루트가 살면 소속
    // 객체가 모두 살고(루트 → 소속 암묵 강한 간선), 표식은 소속 사이 간선을 따라가지 않는다. 켜면
    // maxSlots는 ClusterSlotLimit 이하여야 한다(아니면 Registry 생성이 Fatal).
    bool clusters = false;
    // 패키지 정책 Registry에서 패키지마다 클러스터를 자동으로 만든다(기본 — 13.3절). 패키지 객체가
    // 루트이고 outermost가 그 패키지인 객체가 생성 Commit 때 가입하며, 부분 트리 Discard가 이탈,
    // 패키지 Discard가 해체다. clusters와 관계없이 켜진다. 끄는 것은 측정 비교용이다.
    bool packageClusters = true;
    // 영구 창을 연 채 Registry를 만든다(Docs/Design/GarbageCollectionScale.ko.md 13.7절). 켜면
    // ObjectRegistry::ClosePermanentRegion 전까지 Commit되는 객체가 모두 영구 영역(언제나 표식된
    // 특수 클러스터)에 들어가 표식·sweep 비용이 없다. 영구 객체는 완전한 필드 배치 타입이어야 하고
    // (아니면 Commit이 Unsupported), 강한 간선 대상(outer 포함)은 영구 객체뿐이며(어기는 쓰기·
    // Commit은 Unsupported), RequestDestroy·Discard도 Unsupported다. Shutdown 때만 해체한다.
    // 패키지 정책과 함께 켜면 Registry 생성이 Fatal이다(자식 고리가 간선이라 창을 닫은 뒤 만든
    // 자식이 영구 → 일반 간선이 된다). Engine Registry가 서브시스템 초기화 동안만 연다.
    bool permanentRegion = false;
    // 참조 위치 표(M3)를 쓴다(기본). 끄면 모든 타입을 trace로 표식한다(측정 비교용).
    bool referenceTables = true;
    // Debug에서 Begin·Collect마다 클러스터 장부(소속·inCount·outCount·경계·detached 목록)를
    // 처음부터 다시 계산해 비교하고, 어긋나면 Fatal로 멈춘다(비용이 커서 soak·시험만 켠다).
    // Release·Development에서는 무시한다.
    bool verifyClusterLedger = false;
    // verifyClusterLedger와 함께 켜면 같은 검증에서 소속 불변식도 본다: SetClusterRoot로 지정한
    // 클러스터의 루트가 아닌 소속은 같은 클러스터 소속 객체에서 오는 간선이 하나 이상이어야
    // 한다(13.8절 — 소유 간선이 모두 클러스터 안인 World Level이 켠다). 어기면 Fatal(고아
    // 소속)이다.
    bool verifyClusterOwnership = false;
    // 클러스터를 켠 Registry의 슬롯 수 상한이다(작업 큐 항목의 클러스터 표지 비트와 장부 칸의 상태
    // 비트를 남긴다).
    static constexpr uint32 ClusterSlotLimit = 1u << 29;
};

struct ObjectStats {
    uint32 liveObjects = 0;
    uint32 reservedObjects = 0;
    uint32 destroyRequested = 0;
    uint32 retiredObjects = 0;
    uint32 roots = 0;
    uint32 borrows = 0;
    uint32 backendHolds = 0;
    // 쥐고 있는 encode 고정(EncodePin)의 합이다. 0이 아니면 Shutdown이 Busy다.
    uint32 encodePins = 0;
    uint32 chunks = 0;
    // 이름 색인 항목 수다(이름 있는 객체와 대표 자식 — Reserved 포함). Retire되거나, Discard되거나,
    // cutoff white 객체의 항목을 같은 이름의 새 객체가 대신할 때 빠진다.
    uint32 namedObjects = 0;
    // 패키지 정책 Registry에서 Discard 없이(Alive 상태로 — 패키지 포함) GC로 Retire된 객체 수의
    // 누계다. 언제나 0이어야 한다(AssetManager가 Discard 없이 패키지 RootRef를 놓은 버그). 종료
    // (Shutdown 시작) 뒤의 회수는 세지 않는다.
    uint32 retiredWithoutDiscard = 0;
    // 살아 있는 클러스터 수와 그 소속 객체 수의 합이다(클러스터를 켠 Registry만 0이 아니다).
    uint32 clusters = 0;
    uint32 clusterMembers = 0;
};

// 성공한 한 cycle의 결과다. 시간은 실제 collector slice의 합(ns)이며 mutator 시간은 제외한다.
// phase별 시간은 durationNanoseconds의 내역이다: begin은 Begin(작업 큐 예약·epoch·루트 수집,
// Collect는 같은 구간), mark는 표식, sweep은 white 판정과 Retire, reclaim은 소멸자·해제를 포함한
// 회수다. 증분 수집은 slice마다 그 phase에 쓴 시간을 더한다(잠금 대기와 slice 진입·종료 처리는 어느
// phase에도 넣지 않으므로 넷의 합은 durationNanoseconds 이하다). GC 벤치(Programs/GcBench)와 진단이
// 쓴다.
struct GcReport {
    uint32 examined = 0;
    uint32 marked = 0;
    uint32 retired = 0;
    uint32 reclaimed = 0;
    uint64 durationNanoseconds = 0;
    uint64 beginNanoseconds = 0;
    uint64 markNanoseconds = 0;
    uint64 sweepNanoseconds = 0;
    uint64 reclaimNanoseconds = 0;
};

// StepCollection slice 하나의 시간 내역(ns)이다. 증분 수집의 Step 시간이 예산을 넘는 원인을 나눠 볼
// 때 쓴다(Programs/GcBench). 값은 이 slice 하나의 것이고 누적하지 않는다.
// - setup: 표식 전 준비(장벽 큐 옮기기, 병렬이면 씨앗 블록 묶기와 task 제출)
// - marking: 표식(병렬이면 소유 스레드가 참여 루프에서 나올 때까지)
// - drain: 병렬 표식에서 소유 스레드가 나온 뒤 worker task를 모두 회수할 때까지
// - requeue: 병렬 표식에서 남은 회색 객체를 다음 slice로 넘기기
// - sweep·reclaim: 분류와 회수
// grayCarried는 다음 slice로 넘긴 회색 객체 수다.
struct GcSliceTimes {
    uint64 setup = 0;
    uint64 marking = 0;
    uint64 drain = 0;
    uint64 requeue = 0;
    uint64 sweep = 0;
    uint64 reclaim = 0;
    uint32 grayCarried = 0;
};

struct GcProgress {
    GcPhase phase = GcPhase::Idle;
    uint32 processed = 0;
    GcReport report;
    GcSliceTimes slice;

    bool IsComplete() const noexcept {
        return phase == GcPhase::Idle;
    }
};

} // namespace rocks
