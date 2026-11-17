#pragma once

#include <Core/Error.h>
#include <Core/Memory/Allocator.h>

// 장기 GC smoke 시뮬레이터다(Docs/Design/GarbageCollectionScale.ko.md 9절 — GC-S0). 결정적 seed로
// tick마다 객체 생성(Outer 유무)·참조 필드와 ObjectRefArray 변경·루트 추가와 반환·RequestDestroy·
// Discard·Borrow·backend lease·encode 고정을 섞고, 증분 수집(Begin·Step·Cancel)·정지형 Collect·
// DrainRetired를 끼워 넣는다. 간선·루트·보유 계수를 독립 모델로 들고 다니며 주기적으로 대조한다.
//
// 검사하는 불변식:
// - 모델에서 도달하는 객체는 언제나 살아 있다(Alive·DestroyRequested — cutoff·Retired·회수 없음).
// - 살아 있는 객체의 필드·배열 내용이 모델과 같고, 살아 있는 객체의 간선이 회수된 객체를 가리키지
//   않는다.
// - 모델에서 도달하지 않고 보유 계수가 없는 객체는, 그렇게 관찰된 뒤 시작한 cycle 하나가 끝나면
//   회수되어 있다(쓰레기가 무한히 남지 않는다 — 장벽이 남긴 floating garbage는 다음 cycle에 회수).
// - Registry 통계(Stats)의 루트·Borrow·보유 계수·상태별 객체 수가 모델과 같고 소멸자 수가 관찰한
//   회수 수와 같다.
// - 모든 API 결과가 모델이 예측한 성공·오류와 같다.
// - 끝에서 모든 보유를 놓고 수집하면 객체·계수가 0이 되고 Shutdown이 성공한다.
//
// 다른 스레드의 반환(backend lease·RootRef·EncodePin)은 helper 스레드에 명시적으로 넘기고 끝날
// 때까지 기다린다(시간·경쟁 운에 기대지 않는다). 일부는 표식 slice 안에서(수동 Trace 탐침 객체의
// Trace가 넘긴다) 실행되어 Docs/Design/GarbageCollectionScale.ko.md 7.1절의 cross-thread 경로를
// 덮는다.
namespace rocks {

class WorkerScheduler;

// 실행 설정이다.
// - seed: 무작위 선택의 seed(같은 seed·설정이면 직렬 수집에서 같은 실행이 된다).
// - ticks: 실행할 tick 수.
// - maxObjects: 동시에 추적하는 객체 수 상한(모델 표 크기 — 넘으면 생성하지 않는다).
// - verifyInterval: 전체 대조 간격(tick). cycle이 끝날 때마다도 대조한다.
// - timeBudget: true면 Step 예산을 시간(100us)으로, false면 객체 수(1–64개 무작위)로 준다.
// - scheduler: 주면 StepCollection에 넘긴다(병렬 trace 경로). 시작된 scheduler여야 한다.
struct GcSoakOptions {
    uint64 seed = 1;
    uint64 ticks = 2000;
    uint32 maxObjects = 1500;
    uint32 verifyInterval = 16;
    bool timeBudget = false;
    WorkerScheduler* scheduler = nullptr;
};

// 실행 결과다. failure가 nullptr가 아니면 첫 불변식 위반의 설명(정적 문자열)이고 그 tick·모델 객체
// 번호·관련 오류가 함께 있다.
struct GcSoakReport {
    uint64 ticks = 0;
    uint64 created = 0;
    uint64 reclaimed = 0;
    uint64 cyclesBegun = 0;
    uint64 cyclesCompleted = 0;
    uint64 collects = 0;
    uint64 cancels = 0;
    uint64 drains = 0;
    uint64 steps = 0;
    uint64 verifications = 0;
    uint64 edgeWrites = 0;
    uint64 rejectedWrites = 0;
    uint64 rootChanges = 0;
    uint64 destroys = 0;
    uint64 discards = 0;
    uint64 holds = 0;
    uint64 crossThreadInSlice = 0;
    uint64 crossThreadOutside = 0;
    uint64 busyChecks = 0;
    uint32 peakObjects = 0;
    const char* failure = nullptr;
    uint64 failureTick = 0;
    uint32 failureObject = 0;
    ErrorCode failureError = ErrorCode::None;
};

// 시뮬레이터를 실행한다. 자체 TypeRegistry·ObjectRegistry를 allocator로 만들고 끝에서 모두 정리한다
// (호출 뒤 allocator의 살아 있는 할당이 0이어야 한다 — 호출자가 확인한다). 불변식 위반이 없으면
// true다. 위반이 생기면 그 tick에서 멈추고 보유를 모두 놓은 뒤 정리한다. 정리 중 Registry가
// Shutdown을 거부할 만큼 상태가 망가졌으면 Registry 소멸자가 Fatal로 멈춘다. 소유 스레드(호출
// 스레드)에서 부른다. 같은 프로세스에서 동시에 두 번 부르지 않는다(소멸 계수와 탐침 통로가
// 전역이다).
//
// 사용 예:
//   SystemAllocator allocator;
//   GcSoakOptions options;
//   options.seed = 7;
//   GcSoakReport report;
//   if (!RunGcSoak(allocator, options, report)) { /* report.failure, report.failureTick */ }
bool RunGcSoak(Allocator& allocator, const GcSoakOptions& options, GcSoakReport& report) noexcept;

// 클러스터 장기 smoke 설정이다(GC-S7, Docs/Design/GarbageCollectionScale.ko.md 13.8절).
// - packages: true면 패키지 정책 Registry(패키지 클러스터 — 생성·부분/전체 Discard·Discard 없는
//   루트 반환·패키지 사이 간선), false면 World형(Level 지정 클러스터 — Actor·Component 생성·파괴,
//   World API 밖 Actor 파괴, Level 밖 일반 객체)이다.
// - seed·ticks·maxObjects·scheduler는 GcSoakOptions와 같다. tick 하나는 증분 cycle 하나(slice마다
//   무작위 객체 예산, slice 사이 변경)이고 7 tick마다 정지형 Collect로 정확성을 본다.
struct GcClusterSoakReport;

struct GcClusterSoakOptions {
    uint64 seed = 1;
    uint64 ticks = 200;
    uint32 maxObjects = 1500;
    bool packages = false;
    WorkerScheduler* scheduler = nullptr;
    // 진행 표시(선택): progressInterval tick마다 그때까지의 보고로 부른다(긴 실행이 멈췄는지 느린지
    // 구별하려고 — GcBench가 출력한다).
    void (*progress)(const GcClusterSoakReport&, void* context) noexcept = nullptr;
    void* progressContext = nullptr;
    uint64 progressInterval = 0;
};

struct GcClusterSoakReport {
    uint64 ticks = 0;
    uint64 created = 0;
    uint64 reclaimed = 0;
    uint64 cyclesBegun = 0;
    uint64 cyclesCompleted = 0;
    uint64 collects = 0;
    uint64 steps = 0;
    uint64 verifications = 0;
    uint64 edgeWrites = 0;
    uint64 rejectedWrites = 0;
    uint64 destroys = 0;
    uint64 discards = 0;
    const char* failure = nullptr;
    uint64 failureTick = 0;
    uint32 failureObject = 0;
    ErrorCode failureError = ErrorCode::None;
};

// 클러스터 장기 smoke를 실행한다. 모델(간선·outer·소속·파괴·Discard·루트)을 들고 매 slice 뒤 회수된
// 객체가 oracle(소속 간선과 패키지 고리 포함)에서 닿지 않는지, 정지형 Collect 뒤 생존 집합이
// oracle과 같은지 본다. Debug에서는 Registry가 Begin·Collect마다 클러스터 장부(와 World형이면 소속
// 불변식)를 다시 세어 어긋나면 Fatal로 멈춘다. 위반이 없으면 true다. 소유 스레드에서 부른다.
//
// 사용 예:
//   GcClusterSoakOptions options;
//   options.packages = true;
//   GcClusterSoakReport report;
//   if (!RunGcClusterSoak(allocator, options, report)) { /* report.failure */ }
bool RunGcClusterSoak(Allocator& allocator, const GcClusterSoakOptions& options,
                      GcClusterSoakReport& report) noexcept;

} // namespace rocks
