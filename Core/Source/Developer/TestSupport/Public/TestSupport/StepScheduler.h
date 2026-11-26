#pragma once

#include <Base/Types.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <Platform/Sync/WaiterSignal.h>
#include <Platform/Thread.h>

namespace rocks::test {

class StepScheduler;

namespace detail {

// 이 스레드가 배우로 도는 스케줄러와 배우 번호다(배우 스레드가 아니면 nullptr). thread_local은 공개
// 헤더에 두지 않으므로(공유 라이브러리 경계) Private/StepScheduler.cpp가 갖는다.
StepScheduler*& CurrentStepScheduler() noexcept;
uint32& CurrentStepActor() noexcept;

} // namespace detail

// interleaving 시험 스케줄러다(Docs/Design/Synchronization 14.3절, D24 — CHESS·loom과 같은 생각).
// 배우(actor) 스레드 몇 개를 돌리되 한 번에 하나만 다음 단계 지점(StepPolicy::Step)까지 가게 한다.
// - Explore: 작은 시나리오의 모든 순서를 깊이 우선으로 전수한다(매 결정 지점에서 진행할 배우를
// 고른다).
// - RunSchedule: 시험이 적은 대본(결정마다 고를 배우 번호) 하나로 돈다.
// 단계 지점 사이 코드는 원자 연산이 하나 이하가 되게 둔다(순차 일관 모델 기준의 전수다. 약한 메모리
// 순서의 재배치는 잡지 못한다). 배우 코드는 단계 없이 상대를 기다리며 돌면 안 된다(진행을 상대에게
// 넘길 지점이 없다). 대상 컨테이너는 ScheduledSteps 정책으로 instantiation한다.
//
// 사용 예:
//   BoundedSpscQueue<uint32, test::ScheduledSteps> queue;
//   test::StepScheduler scheduler;
//   const uint32 schedules = scheduler.Explore(2, Actor, &context,
//       [&]() noexcept { Reset(context); }, [&]() noexcept { CHECK(Valid(context)); });
class StepScheduler {
public:
    static constexpr uint32 MaxActors = 4;
    static constexpr uint32 MaxDecisions = 512;

    using ActorFunction = void (*)(void* context, uint32 actor) noexcept;

    // 모든 순서를 돈다. 매 순서 전에 setup(), 뒤에 check()를 부른다. 돈 순서 수를 돌려준다. 순서가
    // maxSchedules를 넘으면 Fatal이다(시나리오를 줄인다).
    template <class Setup, class Check>
    uint32 Explore(uint32 actorCount, ActorFunction actor, void* context, Setup&& setup,
                   Check&& check, uint32 maxSchedules = DefaultMaxSchedules) noexcept {
        Begin(actorCount, actor, context);
        prefixLength_ = 0;
        uint32 schedules = 0;
        while (true) {
            setup();
            RunOnce(nullptr, 0);
            check();
            if (++schedules > maxSchedules) {
                platform::Fatal("StepScheduler exceeded the schedule limit");
            }

            // 가장 깊은 결정 중 아직 고르지 않은 배우가 남은 곳으로 되돌아간다.
            uint32 depth = decisions_;
            while (depth > 0 && choices_[depth - 1] + 1 >= counts_[depth - 1]) {
                --depth;
            }

            if (depth == 0) {
                return schedules;
            }

            ++choices_[depth - 1];
            prefixLength_ = depth;
        }
    }

    // 대본 하나로 돈다. 결정 d에서 schedule[d]번 배우가 진행할 수 있으면 그 배우를, 아니면(끝났거나
    // 대본이 짧으면) 번호가 가장 작은 진행 가능 배우를 고른다.
    void RunSchedule(uint32 actorCount, ActorFunction actor, void* context, const uint32* schedule,
                     uint32 scheduleLength) noexcept {
        Begin(actorCount, actor, context);
        prefixLength_ = 0;
        RunOnce(schedule, scheduleLength);
    }

    // 단계 지점이다(ScheduledSteps가 부른다). 스케줄러 배우 스레드가 아니면 아무 일도 하지 않는다.
    static void Step(const char*) noexcept {
        if (StepScheduler* scheduler = detail::CurrentStepScheduler(); scheduler != nullptr) {
            scheduler->Yield(detail::CurrentStepActor(), false);
        }
    }

    // 진행할 수 없는 배우의 단계 지점이다(예: 비어서 다시 시도). 다른 배우가 한 걸음 나아갈 때까지
    // 이 배우를 고르지 않는다(같은 상태를 무한히 되풀이하는 순서를 탐색하지 않는다). 모두 막혔으면
    // 막힌 배우도 고른다(정말 진행이 없으면 결정 한도에서 Fatal이다).
    static void Blocked(const char*) noexcept {
        if (StepScheduler* scheduler = detail::CurrentStepScheduler(); scheduler != nullptr) {
            scheduler->Yield(detail::CurrentStepActor(), true);
        }
    }

private:
    static constexpr uint32 DefaultMaxSchedules = 200000;

    enum class ActorState : uint32 {
        Running,
        AtStep,
        Blocked,
        Finished
    };

    struct Actor {
        platform::WaiterSignal go;
        platform::Atomic<ActorState> state;
        platform::Thread thread;
        StepScheduler* scheduler = nullptr;
        uint32 index = 0;
    };

    void Begin(uint32 actorCount, ActorFunction actor, void* context) noexcept {
        if (actorCount == 0 || actorCount > MaxActors) {
            platform::Fatal("StepScheduler actor count is out of range");
        }

        actorCount_ = actorCount;
        actorFunction_ = actor;
        context_ = context;
    }

    static void ActorMain(void* raw) noexcept {
        Actor& actor = *static_cast<Actor*>(raw);
        detail::CurrentStepScheduler() = actor.scheduler;
        detail::CurrentStepActor() = actor.index;
        // 시작도 단계 지점이다: 처음 진행할 배우를 스케줄러가 고른다.
        actor.scheduler->Yield(actor.index, false);
        actor.scheduler->actorFunction_(actor.scheduler->context_, actor.index);
        detail::CurrentStepScheduler() = nullptr;
        actor.state.Store<platform::MemoryOrder::Release>(ActorState::Finished);
        actor.scheduler->running_.FetchSub<platform::MemoryOrder::AcquireRelease>(1);
        platform::AtomicThreadFence<platform::MemoryOrder::SequentiallyConsistent>();
        actor.scheduler->ready_.Notify();
    }

    void Yield(uint32 index, bool blocked) noexcept {
        Actor& actor = actors_[index];
        actor.go.Reset();
        actor.state.Store<platform::MemoryOrder::Release>(blocked ? ActorState::Blocked
                                                                  : ActorState::AtStep);
        running_.FetchSub<platform::MemoryOrder::AcquireRelease>(1);
        platform::AtomicThreadFence<platform::MemoryOrder::SequentiallyConsistent>();
        ready_.Notify();
        actor.go.Wait();
    }

    // 진행 중인 배우가 없을 때까지 기다린다(모두 단계 지점에 섰거나 끝났다). "신호 지움 → 수
    // 읽기"는 배우의 "수 내림 → 깨움"과 store-buffering 짝이라 양쪽에 seq_cst fence를 둔다.
    void WaitQuiescent() noexcept {
        while (true) {
            ready_.Reset();
            platform::AtomicThreadFence<platform::MemoryOrder::SequentiallyConsistent>();
            if (running_.Load<platform::MemoryOrder::Acquire>() == 0) {
                return;
            }

            ready_.Wait();
        }
    }

    void RunOnce(const uint32* schedule, uint32 scheduleLength) noexcept {
        decisions_ = 0;
        running_.Store<platform::MemoryOrder::Relaxed>(actorCount_);
        for (uint32 index = 0; index < actorCount_; ++index) {
            Actor& actor = actors_[index];
            actor.scheduler = this;
            actor.index = index;
            actor.state.Store<platform::MemoryOrder::Relaxed>(ActorState::Running);
            if (actor.thread.Start(ActorMain, &actor) != platform::ThreadStatus::Success) {
                platform::Fatal("StepScheduler could not start an actor thread");
            }
        }

        while (true) {
            WaitQuiescent();
            uint32 runnable[MaxActors] = {};
            uint32 runnableCount = 0;
            uint32 blocked[MaxActors] = {};
            uint32 blockedCount = 0;
            for (uint32 index = 0; index < actorCount_; ++index) {
                const ActorState state =
                    actors_[index].state.Load<platform::MemoryOrder::Acquire>();
                if (state == ActorState::AtStep) {
                    runnable[runnableCount++] = index;
                } else if (state == ActorState::Blocked) {
                    blocked[blockedCount++] = index;
                }
            }

            const bool onlyBlocked = runnableCount == 0;
            if (onlyBlocked) {
                for (uint32 index = 0; index < blockedCount; ++index) {
                    runnable[runnableCount++] = blocked[index];
                }
            }

            if (runnableCount == 0) {
                break;
            }

            if (decisions_ == MaxDecisions) {
                platform::Fatal("StepScheduler exceeded the decision limit");
            }

            uint32 pick = 0;
            if (schedule != nullptr) {
                const uint32 wanted =
                    decisions_ < scheduleLength ? schedule[decisions_] : MaxActors;
                for (uint32 candidate = 0; candidate < runnableCount; ++candidate) {
                    if (runnable[candidate] == wanted) {
                        pick = candidate;
                    }
                }
            } else if (decisions_ < prefixLength_) {
                pick = choices_[decisions_];
            } else {
                choices_[decisions_] = 0;
                counts_[decisions_] = runnableCount;
            }

            ++decisions_;
            // 막히지 않은 배우가 한 걸음 나아가면 상태가 바뀌었을 수 있으므로 막힌 배우를 다시
            // 후보로 둔다.
            if (!onlyBlocked) {
                for (uint32 index = 0; index < blockedCount; ++index) {
                    actors_[blocked[index]].state.Store<platform::MemoryOrder::Release>(
                        ActorState::AtStep);
                }
            }

            Actor& chosen = actors_[runnable[pick]];
            chosen.state.Store<platform::MemoryOrder::Release>(ActorState::Running);
            running_.FetchAdd<platform::MemoryOrder::AcquireRelease>(1);
            chosen.go.Notify();
        }

        for (uint32 index = 0; index < actorCount_; ++index) {
            if (actors_[index].thread.Join() != platform::ThreadStatus::Success) {
                platform::Fatal("StepScheduler could not join an actor thread");
            }
        }
    }

    Actor actors_[MaxActors];
    platform::WaiterSignal ready_;
    platform::Atomic<uint32> running_;
    ActorFunction actorFunction_ = nullptr;
    void* context_ = nullptr;
    uint32 actorCount_ = 0;
    uint32 decisions_ = 0;
    uint32 prefixLength_ = 0;
    uint32 choices_[MaxDecisions] = {};
    uint32 counts_[MaxDecisions] = {};
};

// StepScheduler로 도는 단계 정책이다. 작은 태그(IndexedFreeList ABA 재현)도 허용한다.
struct ScheduledSteps {
    static constexpr bool AllowSmallTag = true;

    static void Step(const char* name) noexcept {
        StepScheduler::Step(name);
    }
};

} // namespace rocks::test
