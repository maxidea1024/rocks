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

// 시간 예산은 soft deadline이다. Trace/소멸자 한 번을 중간에 중단하지 않는다.
// Begin의 root scan·잠금 획득·회수 후보 탐색도 hard deadline으로 제한하지 않는다.
// maxObjects는 trace·slot 분류·실제 회수의 합이며 0은 잘못된 설정이다.
// maxNanoseconds=0은 시간 한도를 끄되 객체 작업량 한도는 유지한다.
struct GcBudget {
    uint32 maxObjects = 64;
    uint64 maxNanoseconds = 1000000;
};

// 한도는 생성 시 고정한다. maxGeneration을 낮추면 슬롯 고갈 회귀 시험을 할 수 있다.
struct RegistryOptions {
    uint32 maxSlots = 1048576;
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

struct GcProgress {
    GcPhase phase = GcPhase::Idle;
    uint32 processed = 0;
    GcReport report;

    bool IsComplete() const noexcept {
        return phase == GcPhase::Idle;
    }
};

} // namespace rocks
