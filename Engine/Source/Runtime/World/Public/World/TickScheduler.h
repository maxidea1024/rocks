#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/SlotArray.h>
#include <Object/ObjectHandle.h>

namespace rocks {

// 한 프레임의 Tick 실행 단계다. 열거 순서가 실행 순서다. Physics 단계만 고정 스텝(프레임당 0회
// 이상)으로 실행하고 나머지는 프레임당 한 번 가변 delta로 실행한다. LateUpdate(카메라 추적 등
// 후처리)는 PreRender(렌더 추출 준비)보다 먼저 와서 그 결과가 같은 프레임 렌더에 반영된다.
enum class TickPhase : uint8 {
    PrePhysics,
    Physics,
    PostPhysics,
    LateUpdate,
    PreRender,
    Count
};

inline constexpr uint32 TickPhaseCount = static_cast<uint32>(TickPhase::Count);

// "PrePhysics" 같은 단계 이름이다. 범위 밖이면 "Unknown"이다.
const char* TickPhaseName(TickPhase phase) noexcept;

// 등록 하나를 가리키는 값 식별자다. 해제되면 stale이 되어 이후 조작은 StaleHandle이다.
struct TickHandle {
    SlotHandle slot;

    bool IsNull() const noexcept {
        return slot.IsNull();
    }

    bool operator==(const TickHandle&) const noexcept = default;
};

// World가 정하는 Tick 대상의 종류와 hook이다. 스케줄러는 해석하지 않고 실행기에 그대로 넘긴다.
struct TickTarget {
    ObjectHandle object;
    uint8 kind = 0;
    uint8 hook = 0;
};

// 등록 정책이다.
struct TickRegistration {
    TickTarget target;
    // 요청 단계다. 실제로 실행하는 단계(실효 단계)는 선행 의존이 더 늦은 단계에 있으면 그 단계로
    // 늦춰진다(EffectivePhase).
    TickPhase phase = TickPhase::PrePhysics;
    // 같은 단계 안에서 선행 의존으로 묶이지 않은 등록끼리의 순서다. 작은 값이 먼저 실행된다. 같은
    // 값은 등록 순서를 따른다.
    int32 priority = 0;
    // 0이면 매 실행 기회마다, 양수면 누적 시간이 interval에 이르면 실행한다(초). 실행 시 delta는
    // 마지막 실행 이후 흐른 시간이다. 남은 누적은 다음 주기로 이월해 평균 주기를 interval에 맞춘다
    // (한 실행 기회에 여러 번 따라잡지는 않는다). 부동소수점 합산 오차는 상대 허용오차로 흡수한다.
    double intervalSeconds = 0.0;
    // World pause 중에도 unscaled delta로 실행한다. 기본은 pause 중 정지다.
    bool tickWhenPaused = false;
    // 등록 직후 활성 상태다. SetEnabled로 바꾼다.
    bool enabled = true;
};

// 실행기에 넘기는 한 번의 실행 정보다.
struct TickContext {
    TickPhase phase = TickPhase::PrePhysics;
    // 이 등록에 넘기는 delta(초). interval이 있으면 마지막 실행 이후 흐른 시간이다.
    double deltaSeconds = 0.0;
    // pause·scale을 적용하지 않은 이번 프레임 delta다.
    double unscaledDeltaSeconds = 0.0;
    uint64 frameIndex = 0;
    // Physics 단계에서 이번 실행의 고정 스텝 번호다. 다른 단계에서는 0이다.
    uint64 fixedStepIndex = 0;
    bool paused = false;
    // 실행 중인 등록이다. 실행기가 대상이 사라진 것을 발견하면 이 handle로 등록을 해제할 수 있다.
    TickHandle handle;
};

// Tick 실행기다. World가 구현해 대상 객체를 Borrow하고 hook을 호출한다. 실행 중 스케줄러의
// 등록·해제·활성·의존 변경은 허용한다.
class TickExecutor {
public:
    virtual void ExecuteTick(const TickTarget& target, const TickContext& context) noexcept = 0;

protected:
    ~TickExecutor() = default;
};

// 한 단계 실행에 필요한 프레임 정보다.
struct TickPhaseFrame {
    double deltaSeconds = 0.0;
    double unscaledDeltaSeconds = 0.0;
    uint64 frameIndex = 0;
    uint64 fixedStepIndex = 0;
    bool paused = false;
};

// 진단용 Tick 항목 정보다(World::DumpTickGraph — Docs/Design/EngineRuntime 22.1절).
struct TickEntryInfo {
    TickTarget target;
    // 요청 단계와 실효 단계다(다르면 선행 의존으로 늦춰졌다).
    TickPhase requested = TickPhase::PrePhysics;
    TickPhase effective = TickPhase::PrePhysics;
    int32 priority = 0;
    double intervalSeconds = 0.0;
    bool enabled = true;
    bool tickWhenPaused = false;
    // 단계 순서 배열에 아직 들지 않았다(다음 단계 경계에 재구성해 넣는다).
    bool pending = false;
    // 같은 단계·다른 단계의 선행 handle이다(TargetOf로 대상을 찾는다). 순회 호출 동안만 유효하다.
    Span<const TickHandle> prerequisites;
};

struct TickStats {
    uint32 registered = 0;
    uint32 enabled = 0;
    // BeginFrame 이후 실제로 실행한 수다.
    uint32 executedThisFrame = 0;
    // 아직 반영하지 않은 해제 수와 순서 재계산이 필요한 단계 수의 합이다.
    uint32 pendingChanges = 0;
    // 실효 단계 재계산이 할당 실패로 미뤄진 누적 횟수다. 그 프레임은 이전 실효 단계로 실행하고 다음
    // 프레임에 다시 계산한다.
    uint32 phaseRecomputeFailures = 0;
    // 실효 단계가 요청 단계와 다른(늦춰진) 항목 수다(마지막 재계산 기준 — 그 뒤 SetPhase로 요청만
    // 바뀐 항목은 다음 재계산에서 반영된다).
    uint32 demoted = 0;
};

namespace detail {
struct TickSchedulerTestAccess;
} // namespace detail

// 등록 기반 Tick 스케줄러다. 매 프레임 모든 객체를 순회하지 않고 등록된 항목만 단계별 밀집 배열로
// 실행한다.
//
// 사용 예:
//   TickScheduler ticks(allocator);
//   auto handle = ticks.Register({.target = {actor, kind, hook}, .phase = TickPhase::PrePhysics});
//   ticks.AddPrerequisite(handle.Value(), cameraTick);   // camera 먼저
//   ticks.BeginFrame();
//   ticks.RunPhase(TickPhase::PrePhysics, frame, executor);
//
// 계약(Docs/Design/EngineRuntime 8·15절):
// - 등록·해제·활성 변경·의존 변경은 언제든 호출할 수 있다. 실행 중(RunPhase 안) 변경의 반영 시점:
//   해제·비활성화는 즉시(남은 실행에서 빠진다), 활성화는 즉시(이번 단계의 아직 실행 전 위치면
//   이번에 실행된다), 새 등록·같은 단계 의존 변경·priority·interval 변경은 다음 단계 경계(Physics는
//   다음 고정 스텝)부터다. 실효 단계가 바뀌는 변경(단계를 넘는 의존, SetPhase)은 새 프레임의 첫
//   RunPhase 전에 반영한다(단계 경계에서 옮기면 한 프레임에 두 번 실행되거나 빠질 수 있다). 새
//   프레임은 TickPhaseFrame::frameIndex가 바뀐 것으로 안다(BeginFrame 호출 여부와 무관하다).
// - 실행 중 해제는 할당하지 않는다(표시만 하고 단계 경계에서 지운다).
// - 선행 의존은 단계와 무관하게 한 그래프에 둔다. 사이클(단계를 넘는 것 포함)을 만드는 의존 추가는
//   추가 시점에 CycleDetected로 거부하며 상태를 바꾸지 않는다. 실효 단계는 max(요청 단계, 모든
//   선행의 실효 단계)다(UE tick group demotion). 같은 실효 단계 안의 간선만 실행 순서를 정한다.
//   Physics 단계(고정 스텝) 등록과 다른 단계 등록 사이 의존은 고정 스텝 횟수가 달라
//   InvalidArgument다. 해제된 등록에 걸린 의존은 다음 순서 재계산 때 지운다.
// - 순서: 선행 의존을 지키는 범위에서 준비된 항목 중 (priority, 등록 순서)가 가장 작은 것부터
// 실행한다
//   (priority 최소 힙 위상 정렬). 의존으로 묶이지 않은 항목끼리는 항상 (priority, 등록 순서)를
//   따른다. 순서 계산은 등록 집합이 바뀐 단계에서만 다시 하며 단계 실행 중에는 할당하지 않는다.
// - 스레드: 동기화하지 않는다. World의 게임 스레드에서만 쓴다. 병렬 실행은 후속 범위다.
class ROCKS_WORLD_API TickScheduler {
public:
    explicit TickScheduler(Allocator& allocator) noexcept;
    TickScheduler(const TickScheduler&) = delete;
    TickScheduler& operator=(const TickScheduler&) = delete;

    // 등록을 추가한다. 단계 범위 밖·음수/비유한 interval은 InvalidArgument, 할당 실패는
    // OutOfMemory다. 닫힌(Close) 스케줄러는 ShuttingDown이다.
    Result<TickHandle> Register(const TickRegistration& registration) noexcept;

    // 등록을 해제한다. 이미 해제된 핸들은 StaleHandle이다. 실행 중이면 남은 실행에서 즉시 빠진다.
    Result<void> Unregister(TickHandle handle) noexcept;

    Result<void> SetEnabled(TickHandle handle, bool enabled) noexcept;

    // 요청 단계를 바꾼다. 다음 프레임의 첫 RunPhase 전에 반영한다. 범위 밖 단계, Physics로 또는
    // Physics에서 바꾸는 것(고정 스텝 등록과 가변 등록은 다른 종류다)은 InvalidArgument다.
    Result<void> SetPhase(TickHandle handle, TickPhase phase) noexcept;

    // 같은 단계 안 순서 값을 바꾼다. 다음 단계 경계에서 반영한다.
    Result<void> SetPriority(TickHandle handle, int32 priority) noexcept;

    // 실행 간격(초)을 바꾸고 누적을 0으로 되돌린다. 음수·비유한 값은 InvalidArgument다.
    Result<void> SetInterval(TickHandle handle, double intervalSeconds) noexcept;

    // dependent가 prerequisite보다 나중에 실행되게 한다. prerequisite가 더 늦은 단계면 dependent의
    // 실효 단계가 그 단계로 늦춰진다. 자기 자신, Physics 등록과 다른 단계 등록 사이 의존은
    // InvalidArgument, 사이클을 만들면 CycleDetected(상태 불변), 이미 있는 의존은 성공(중복 추가
    // 없음)이다. 해제된 핸들은 StaleHandle이다.
    Result<void> AddPrerequisite(TickHandle dependent, TickHandle prerequisite) noexcept;

    // 의존을 뺀다. 없는 의존은 NotFound, 해제된 dependent는 StaleHandle이다. 늦춰졌던 실효 단계는
    // 다음 프레임에 다시 계산한다.
    Result<void> RemovePrerequisite(TickHandle dependent, TickHandle prerequisite) noexcept;

    bool IsRegistered(TickHandle handle) const noexcept;

    // 지금 실행하는 단계다(실효 단계). 반영 전 변경은 아직 보이지 않는다. 해제된 핸들은
    // StaleHandle이다.
    Result<TickPhase> EffectivePhase(TickHandle handle) const noexcept;

    // 프레임 시작에 호출한다. 실행 수 통계를 초기화한다. 실효 단계 재계산 시점은 이 호출이 아니라
    // frameIndex가 바뀐 첫 RunPhase다(hook 안에서 불러도 같은 프레임에 다시 계산하지 않는다).
    void BeginFrame() noexcept;

    // 한 단계를 실행한다. 단계 시작 전에 보류 변경을 반영한다(새 frameIndex의 첫 RunPhase면 실효
    // 단계도 다시 계산한다 — 그 할당이 실패하면 이전 실효 단계로 실행하고 phaseRecomputeFailures를
    // 센다). 재진입(실행 중 RunPhase)은 Busy다. 순서 재계산에 필요한 할당이 실패하면
    // OutOfMemory이며 그 단계는 실행하지 않는다.
    Result<void> RunPhase(TickPhase phase, const TickPhaseFrame& frame,
                          TickExecutor& executor) noexcept;

    // 이후 Register를 거부한다(World 종료). 기존 등록은 유지되며 Unregister는 계속 가능하다.
    void Close() noexcept {
        closed_ = true;
    }

    TickStats Stats() const noexcept;

    // phase를 실효 단계로 둔 항목을 실행 순서대로 visitor(const TickEntryInfo&)에 넘긴다(진단 —
    // 순서를 다시 만들지 않는다: 순서 배열에 아직 없는 항목은 끝에 pending으로 넘긴다). 해제된
    // 항목은 넘기지 않는다. 순회 중 등록을 바꾸지 않는다.
    template <class Visitor>
    void ForEachInPhase(TickPhase phase, Visitor&& visitor) const noexcept {
        const uint32 index = static_cast<uint32>(phase);
        if (index >= TickPhaseCount) {
            return;
        }

        const Array<TickHandle>& order = order_[index];
        for (usize position = 0; position < order.Size(); ++position) {
            const Entry* entry = entries_.Find(order[position].slot);
            if (entry == nullptr || entry->removed || entry->effective != phase ||
                entry->orderPosition != position) {
                continue;
            }

            visitor(Describe(*entry, false));
        }

        // 순서 배열에 들지 않은 항목(새 등록, 재구성이 할당 실패로 멈춘 단계의 항목)은 끝에 넘긴다.
        for (auto iterator = entries_.begin(); iterator != entries_.end(); ++iterator) {
            const Entry& entry = *iterator;
            if (entry.removed || entry.effective != phase) {
                continue;
            }

            const bool ordered = entry.orderPosition < order.Size() &&
                                 order[entry.orderPosition].slot == iterator.Handle();
            if (!ordered) {
                visitor(Describe(entry, true));
            }
        }
    }

    // 실효 단계 재계산이 다음 프레임 첫 단계 전에 남아 있는지다(SetPhase·단계를 넘는 의존 변경 뒤 —
    // 그때까지 TickEntryInfo의 effective는 이전 계산 값이다).
    bool ArePhaseChangesPending() const noexcept {
        return phasesDirty_;
    }

    // phase의 순서 배열이 다음 실행 때 다시 만들어지는지다(진단 표시).
    bool IsOrderStale(TickPhase phase) const noexcept {
        const uint32 index = static_cast<uint32>(phase);
        return index < TickPhaseCount && dirty_[index];
    }

    // 등록의 대상이다(진단 — 선행 handle을 이름으로 바꿀 때). 없으면 NotFound다.
    Result<TickTarget> TargetOf(TickHandle handle) const noexcept;

private:
    // 시험이 Reaches 방문 번호를 한 바퀴 끝 근처로 옮기고 선행 목록 길이를 본다
    // (Private/TickSchedulerTestAccess.h).
    friend struct detail::TickSchedulerTestAccess;

    struct Entry {
        Entry(Allocator& allocator, const TickRegistration& value, uint64 order) noexcept;

        TickRegistration registration;
        Array<TickHandle> prerequisites;
        uint64 sequence = 0;
        // 실효 단계다(이 항목이 들어 있는 순서 배열). 등록 때는 요청 단계와 같고, 다음 프레임의 첫
        // RunPhase 전 RecomputeEffectivePhases가 고친다.
        TickPhase effective = TickPhase::PrePhysics;
        // interval 판정용 누적(이월 포함)과 마지막 실행 이후 흐른 시간이다.
        double accumulated = 0.0;
        double sinceLastRun = 0.0;
        // 단계 순서 배열(order_)에서의 위치다(재구성·끝에 붙이기가 정한다 — 없으면 NoOrder).
        uint32 orderPosition = NoOrder;
        // 이 항목을 선행으로 둔 항목 수다. 0이 아닌 항목을 해제하면 의존이 풀려 순서가 바뀌므로
        // 재구성한다.
        uint32 dependents = 0;
        bool removed = false;
    };

    static constexpr uint32 NoOrder = NumericLimits<uint32>::Max();

    static TickEntryInfo Describe(const Entry& entry, bool pending) noexcept {
        TickEntryInfo info;
        info.target = entry.registration.target;
        info.requested = entry.registration.phase;
        info.effective = entry.effective;
        info.priority = entry.registration.priority;
        info.intervalSeconds = entry.registration.intervalSeconds;
        info.enabled = entry.registration.enabled;
        info.tickWhenPaused = entry.registration.tickWhenPaused;
        info.pending = pending;
        info.prerequisites =
            Span<const TickHandle>(entry.prerequisites.Data(), entry.prerequisites.Size());
        return info;
    }

    Result<void> RebuildOrder(uint32 phase) noexcept;
    // 모든 항목의 실효 단계를 전체 그래프 위상 순서로 다시 계산하고, 바뀐 항목의 단계 배열을
    // 재구성 대상으로 표시한다. 할당 실패는 OutOfMemory(상태 불변)다.
    Result<void> RecomputeEffectivePhases() noexcept;
    // from에서 선행 간선을 따라 to에 닿는지 본다(방문 표시로 선형 시간). 할당 실패는 OutOfMemory다.
    Result<bool> Reaches(TickHandle from, TickHandle to) noexcept;
    void ApplyPendingRemovals() noexcept;
    // 순서 배열에 있던 항목(ordered)이 해제됐음을 단계 상태에 적는다.
    void NoteRemovedLocked(uint32 phase, bool ordered) noexcept;
    // 해제할 항목의 의존 계수를 정리하고 순서 배열을 다시 만들어야 하는지 돌려준다.
    bool ReleaseDependencies(const Entry& entry) noexcept;
    bool Before(uint32 left, uint32 right) const noexcept;
    void HeapPush(uint32 local) noexcept;
    uint32 HeapPop() noexcept;

    Allocator& allocator_;
    SlotArray<Entry> entries_;
    Array<TickHandle> order_[TickPhaseCount];
    bool dirty_[TickPhaseCount] = {};
    // 순서 배열을 처음부터 다시 만들지 않고 고치려고 둔 단계별 상태다(RebuildOrder 주석).
    // - maxPriority_: 순서 배열 항목의 priority 최댓값(새 항목이 이 이상이면 끝에 붙여도 순서가
    // 같다)
    // - staleInOrder_: 순서 배열에 남은 해제 항목 수(실행이 건너뛴다 — 배열의 절반을 넘으면 재구성)
    int32 maxPriority_[TickPhaseCount] = {};
    uint32 staleInOrder_[TickPhaseCount] = {};
    // 실행 중 해제를 미룬 항목이다(단계 경계에서 지운다 — 등록마다 용량을 미리 확보해 해제는
    // 할당하지 않는다).
    Array<TickHandle> pendingRemoved_;
    // 정렬·탐색 작업 공간이다. 순서가 바뀐 단계와 의존 추가 때만 쓰며 크기를 재사용한다.
    Array<TickHandle> scratch_;
    Array<uint32> localIndex_;
    Array<uint32> indegree_;
    Array<uint32> dependentOffsets_;
    Array<uint32> dependents_;
    Array<uint32> heap_;
    // 실효 단계 재계산의 항목별 계산 값이다(RecomputeEffectivePhases).
    Array<TickPhase> phaseScratch_;
    // Reaches의 방문 표시다(항목 칸마다 마지막으로 방문한 호출의 번호 — 호출마다 배열을 비우지
    // 않아 비용이 방문한 항목 수에 비례한다).
    Array<uint32> marks_;
    uint32 markEpoch_ = 0;
    Array<TickHandle> stack_;
    uint64 nextSequence_ = 1;
    uint32 pendingRemovals_ = 0;
    uint32 executedThisFrame_ = 0;
    // 실효 단계를 다시 계산해야 한다(단계를 넘는 의존·SetPhase·늦춤을 준 항목의 해제).
    bool phasesDirty_ = false;
    // 실효 단계가 요청 단계와 다른(늦춰진) 항목 수다. 0이면 선행 해제가 늦춤을 풀 일이 없다(마지막
    // 재계산 기준 — SetPhase 뒤 재계산 전에는 근사이며, 그때는 phasesDirty_가 이미 참이다).
    uint32 demoted_ = 0;
    // 마지막 RunPhase의 frameIndex다(바뀌면 새 프레임 — 실효 단계 재계산 자리).
    static constexpr uint64 NoFrame = NumericLimits<uint64>::Max();
    uint64 lastFrameIndex_ = NoFrame;
    uint32 recomputeFailures_ = 0;
    bool running_ = false;
    bool closed_ = false;
};

} // namespace rocks
