#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/SlotArray.h>
#include <Object/ObjectHandle.h>

namespace rocks {

// 한 프레임의 Tick 실행 단계다. 단계 순서가 실행 순서다. Physics 단계만 고정 스텝(프레임당 0회
// 이상)으로 실행하고 나머지는 프레임당 한 번 가변 delta로 실행한다.
enum class TickPhase : uint8 {
    PrePhysics,
    Physics,
    PostPhysics,
    PreRender,
    LateUpdate,
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
    TickPhase phase = TickPhase::PrePhysics;
    // 같은 단계 안에서 선행 의존이 없는 등록끼리의 순서다. 작은 값이 먼저 실행된다. 같은 값은
    // 등록 순서를 따른다.
    int32 priority = 0;
    // 0이면 매 실행 기회마다, 양수면 누적 시간이 interval 이상일 때 실행한다(초). 실행 시 delta는
    // 마지막 실행 이후 누적 시간이다. Physics 단계에서는 고정 스텝 횟수로 누적한다.
    double intervalSeconds = 0.0;
    // World pause 중에도 unscaled delta로 실행한다. 기본은 pause 중 정지다.
    bool tickWhenPaused = false;
    // 등록 직후 활성 상태다. SetEnabled로 바꾼다.
    bool enabled = true;
};

// 실행기에 넘기는 한 번의 실행 정보다.
struct TickContext {
    TickPhase phase = TickPhase::PrePhysics;
    // 이 등록에 넘기는 delta(초). interval이 있으면 누적 시간이다.
    double deltaSeconds = 0.0;
    // pause·scale을 적용하지 않은 이번 프레임 delta다.
    double unscaledDeltaSeconds = 0.0;
    uint64 frameIndex = 0;
    // Physics 단계에서 이번 실행의 고정 스텝 번호다. 다른 단계에서는 0이다.
    uint64 fixedStepIndex = 0;
    bool paused = false;
};

// Tick 실행기다. World가 구현해 대상 객체를 Borrow하고 hook을 호출한다. 실행 중 스케줄러의
// 등록·해제 ·의존 변경은 허용하며 단계 경계에서 반영된다.
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

struct TickStats {
    uint32 registered = 0;
    uint32 enabled = 0;
    // 마지막 RunPhase들에서 실제로 실행한 수다(BeginFrame에서 0으로 초기화).
    uint32 executedThisFrame = 0;
    // 아직 반영하지 않은 등록 변경 수다.
    uint32 pendingChanges = 0;
};

// 등록 기반 Tick 스케줄러다. 매 프레임 모든 객체를 순회하지 않고 등록된 항목만 단계별 밀집 배열로
// 실행한다.
//
// 사용 예:
//   TickScheduler ticks(allocator);
//   auto handle = ticks.Register({.target = {actor, kind, hook}, .phase = TickPhase::PrePhysics});
//   ticks.AddPrerequisite(handle.Value(), cameraTick);   // camera 먼저
//   ticks.RunPhase(TickPhase::PrePhysics, frame, executor);
//
// 계약:
// - 등록·해제·활성 변경·의존 변경은 언제든 호출할 수 있다. RunPhase 실행 중의 변경은 그 단계가 끝난
//   뒤(단계 경계) 반영되며, 실행 중 해제·비활성화된 항목은 즉시 이후 실행에서 빠진다.
// - 선행 의존은 같은 단계의 등록 사이에만 둘 수 있다(다른 단계 간 순서는 단계 순서로 보장된다).
//   사이클을 만드는 의존 추가는 InvalidArgument로 거부하며 상태를 바꾸지 않는다. 해제된 등록에 걸린
//   의존은 자동으로 사라진다.
// - 순서 계산(위상 정렬 → priority → 등록 순서)은 등록 집합이 바뀐 단계에서만 다시 한다. 단계 실행
//   중에는 할당하지 않는다.
// - 스레드: 동기화하지 않는다. World의 게임 스레드에서만 쓴다. 병렬 실행은 후속 범위다.
class TickScheduler {
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

    // dependent가 prerequisite보다 나중에 실행되게 한다. 다른 단계, 자기 자신, 사이클은
    // InvalidArgument, 이미 있는 의존은 성공(중복 추가 없음)이다.
    Result<void> AddPrerequisite(TickHandle dependent, TickHandle prerequisite) noexcept;
    Result<void> RemovePrerequisite(TickHandle dependent, TickHandle prerequisite) noexcept;

    bool IsRegistered(TickHandle handle) const noexcept;

    // 프레임 시작에 호출한다. 실행 수 통계를 초기화한다.
    void BeginFrame() noexcept;

    // 한 단계를 실행한다. 단계 시작 전에 보류 변경을 반영한다. 재진입(실행 중 RunPhase)은 Busy다.
    // 순서 재계산에 필요한 할당이 실패하면 OutOfMemory이며 그 단계는 실행하지 않는다.
    Result<void> RunPhase(TickPhase phase, const TickPhaseFrame& frame,
                          TickExecutor& executor) noexcept;

    // 이후 Register를 거부한다(World 종료). 기존 등록은 유지되며 Unregister는 계속 가능하다.
    void Close() noexcept {
        closed_ = true;
    }

    TickStats Stats() const noexcept;

private:
    struct Entry {
        Entry(Allocator& allocator, const TickRegistration& value, uint64 order) noexcept;

        TickRegistration registration;
        Array<TickHandle> prerequisites;
        uint64 sequence = 0;
        double accumulated = 0.0;
        bool removed = false;
    };

    Result<void> RebuildOrder(uint32 phase) noexcept;
    // from에서 선행 간선을 따라 to에 닿는지 본다. 작업 스택 할당 실패는 OutOfMemory다.
    Result<bool> Reaches(TickHandle from, TickHandle to) noexcept;
    void ApplyPendingRemovals() noexcept;

    Allocator& allocator_;
    SlotArray<Entry> entries_;
    Array<TickHandle> order_[TickPhaseCount];
    bool dirty_[TickPhaseCount] = {};
    // 정렬·탐색 작업 공간이다. 프레임 사이에 재사용한다(순서가 바뀐 단계에서만 쓴다).
    Array<TickHandle> scratch_;
    Array<uint8> marks_;
    Array<TickHandle> stack_;
    Array<TickHandle> pendingRemovals_;
    uint64 nextSequence_ = 1;
    uint32 executedThisFrame_ = 0;
    bool running_ = false;
    bool closed_ = false;
};

} // namespace rocks
