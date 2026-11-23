#include <Base/InitializerList.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <World/Private/TickSchedulerTestAccess.h>
#include <World/TickScheduler.h>

using namespace rocks;
namespace tick_scheduler_tests_file {

namespace {

// 실행 순서를 기록하는 실행기다. target.kind를 식별 번호로 쓴다.
class RecordingExecutor final : public TickExecutor {
public:
    explicit RecordingExecutor(TickScheduler& scheduler) noexcept : scheduler_(scheduler) {
    }

    void ExecuteTick(const TickTarget& target, const TickContext& context) noexcept override {
        if (count < 64) {
            order[count] = target.kind;
            deltas[count] = context.deltaSeconds;
            ++count;
        }
        if (target.kind == unregisterOnRun && !victim.IsNull()) {
            CHECK(scheduler_.Unregister(victim));
            victim = {};
        }
        if (target.kind == registerOnRun) {
            TickRegistration added;
            added.target.kind = 99;
            added.phase = context.phase;
            auto handle = scheduler_.Register(added);
            CHECK(handle.IsOk());
            registerOnRun = 255;
        }
    }

    void Reset() noexcept {
        count = 0;
    }

    uint8 order[64] = {};
    double deltas[64] = {};
    uint32 count = 0;
    uint8 unregisterOnRun = 255;
    uint8 registerOnRun = 255;
    TickHandle victim;

private:
    TickScheduler& scheduler_;
};

TickRegistration Make(uint8 id, TickPhase phase = TickPhase::PrePhysics,
                      int32 priority = 0) noexcept {
    TickRegistration registration;
    registration.target.kind = id;
    registration.phase = phase;
    registration.priority = priority;
    return registration;
}

TickPhaseFrame Frame(double delta = 0.1, bool paused = false) noexcept {
    TickPhaseFrame frame;
    frame.deltaSeconds = paused ? 0.0 : delta;
    frame.unscaledDeltaSeconds = delta;
    frame.paused = paused;
    return frame;
}

} // namespace

TEST(Tick_PriorityThenRegistrationOrderWithinPhase) {
    SystemAllocator allocator;
    TickScheduler scheduler(allocator);
    RecordingExecutor executor(scheduler);
    REQUIRE(scheduler.Register(Make(1, TickPhase::PrePhysics, 5)));
    REQUIRE(scheduler.Register(Make(2, TickPhase::PrePhysics, -1)));
    REQUIRE(scheduler.Register(Make(3, TickPhase::PrePhysics, 5)));
    REQUIRE(scheduler.Register(Make(4, TickPhase::PostPhysics, -100)));
    REQUIRE(scheduler.RunPhase(TickPhase::PrePhysics, Frame(), executor));
    REQUIRE(executor.count == 3);
    CHECK(executor.order[0] == 2 && executor.order[1] == 1 && executor.order[2] == 3);
    executor.Reset();
    REQUIRE(scheduler.RunPhase(TickPhase::PostPhysics, Frame(), executor));
    CHECK(executor.count == 1 && executor.order[0] == 4);
}

TEST(Tick_PrerequisitesOverridePriorityAndCyclesAreRejected) {
    SystemAllocator allocator;
    TickScheduler scheduler(allocator);
    RecordingExecutor executor(scheduler);
    auto a = scheduler.Register(Make(1, TickPhase::PrePhysics, 0));
    auto b = scheduler.Register(Make(2, TickPhase::PrePhysics, 10));
    auto c = scheduler.Register(Make(3, TickPhase::PrePhysics, 20));
    auto other = scheduler.Register(Make(4, TickPhase::Physics));
    REQUIRE(a && b && c && other);
    // a는 c 뒤에 실행되어야 한다. c는 priority가 가장 낮지만 a 앞으로 당겨진다.
    REQUIRE(scheduler.AddPrerequisite(a.Value(), c.Value()));
    CHECK(scheduler.AddPrerequisite(a.Value(), c.Value()).IsOk()); // 중복은 성공
    REQUIRE(scheduler.AddPrerequisite(c.Value(), b.Value()));
    // 사이클(b → a)과 자기 의존, 고정 스텝(Physics) 등록과의 의존은 거부한다.
    CHECK(scheduler.AddPrerequisite(b.Value(), a.Value()).Error() == ErrorCode::CycleDetected);
    CHECK(scheduler.AddPrerequisite(a.Value(), a.Value()).Error() == ErrorCode::InvalidArgument);
    CHECK(scheduler.AddPrerequisite(a.Value(), other.Value()).Error() ==
          ErrorCode::InvalidArgument);
    REQUIRE(scheduler.RunPhase(TickPhase::PrePhysics, Frame(), executor));
    REQUIRE(executor.count == 3);
    CHECK(executor.order[0] == 2 && executor.order[1] == 3 && executor.order[2] == 1);

    // 선행 등록을 해제하면 의존도 사라진다.
    REQUIRE(scheduler.Unregister(c.Value()));
    executor.Reset();
    REQUIRE(scheduler.RunPhase(TickPhase::PrePhysics, Frame(), executor));
    REQUIRE(executor.count == 2);
    CHECK(executor.order[0] == 1 && executor.order[1] == 2);
    CHECK(scheduler.RemovePrerequisite(a.Value(), b.Value()).Error() == ErrorCode::NotFound);
}

TEST(Tick_IntervalCarriesRemainderAndPassesElapsedDelta) {
    SystemAllocator allocator;
    TickScheduler scheduler(allocator);
    RecordingExecutor executor(scheduler);
    TickRegistration slow = Make(1);
    slow.intervalSeconds = 0.25;
    REQUIRE(scheduler.Register(slow));
    for (int frame = 0; frame < 5; ++frame) {
        REQUIRE(scheduler.RunPhase(TickPhase::PrePhysics, Frame(0.1), executor));
    }
    // 0.3에서 실행(흐른 시간 0.3 전달, 0.05 이월), 0.5에서 다시 실행(흐른 시간 0.2).
    REQUIRE(executor.count == 2);
    CHECK(executor.deltas[0] > 0.29 && executor.deltas[0] < 0.31);
    CHECK(executor.deltas[1] > 0.19 && executor.deltas[1] < 0.21);
    TickRegistration invalid = Make(2);
    invalid.intervalSeconds = -1.0;
    CHECK(scheduler.Register(invalid).Error() == ErrorCode::InvalidArgument);
}

TEST(Tick_IntervalAtSixtyHertzKeepsAverageFrequency) {
    // 1/60초 프레임으로 0.1초 간격이면 정확히 6프레임마다 실행해야 한다(합산 오차로 7프레임 금지).
    SystemAllocator allocator;
    TickScheduler scheduler(allocator);
    RecordingExecutor executor(scheduler);
    TickRegistration slow = Make(1);
    slow.intervalSeconds = 0.1;
    REQUIRE(scheduler.Register(slow));
    for (int frame = 0; frame < 60; ++frame) {
        REQUIRE(scheduler.RunPhase(TickPhase::PrePhysics, Frame(1.0 / 60.0), executor));
    }
    CHECK(executor.count == 10);
}

TEST(Tick_IndependentEntriesKeepPriorityOrderAroundDependencies) {
    SystemAllocator allocator;
    TickScheduler scheduler(allocator);
    RecordingExecutor executor(scheduler);
    auto x = scheduler.Register(Make(1, TickPhase::PrePhysics, 0));
    auto p1 = scheduler.Register(Make(2, TickPhase::PrePhysics, 1));
    auto p2 = scheduler.Register(Make(3, TickPhase::PrePhysics, 5));
    auto q = scheduler.Register(Make(4, TickPhase::PrePhysics, 3));
    REQUIRE(x && p1 && p2 && q);
    REQUIRE(scheduler.AddPrerequisite(x.Value(), p2.Value()));
    REQUIRE(scheduler.AddPrerequisite(x.Value(), p1.Value()));
    REQUIRE(scheduler.RunPhase(TickPhase::PrePhysics, Frame(), executor));
    // 독립인 P1(1)·Q(3)·P2(5)는 priority 순이고, X는 두 선행이 끝난 뒤 실행된다.
    REQUIRE(executor.count == 4);
    CHECK(executor.order[0] == 2 && executor.order[1] == 4 && executor.order[2] == 3 &&
          executor.order[3] == 1);
}

TEST(Tick_CycleCheckIsLinearOnDiamondDependencies) {
    // 층마다 두 항목이 아래 층 두 항목 모두에 의존하는 40층 그래프. 방문 표시가 없으면 2^40 경로를
    // 탐색한다.
    SystemAllocator allocator;
    TickScheduler scheduler(allocator);
    RecordingExecutor executor(scheduler);
    constexpr uint32 Layers = 40;
    TickHandle handles[Layers][2];
    for (uint32 layer = 0; layer < Layers; ++layer) {
        for (uint32 side = 0; side < 2; ++side) {
            auto registered = scheduler.Register(Make(static_cast<uint8>(layer)));
            REQUIRE(registered);
            handles[layer][side] = registered.Value();
            if (layer > 0) {
                REQUIRE(scheduler.AddPrerequisite(handles[layer][side], handles[layer - 1][0]));
                REQUIRE(scheduler.AddPrerequisite(handles[layer][side], handles[layer - 1][1]));
            }
        }
    }
    auto unrelated = scheduler.Register(Make(200));
    REQUIRE(unrelated);
    REQUIRE(scheduler.AddPrerequisite(unrelated.Value(), handles[Layers - 1][0]));
    CHECK(scheduler.AddPrerequisite(handles[0][0], unrelated.Value()).Error() ==
          ErrorCode::CycleDetected);
    REQUIRE(scheduler.RunPhase(TickPhase::PrePhysics, Frame(), executor));
    CHECK(scheduler.Stats().executedThisFrame == Layers * 2 + 1);
}

TEST(Tick_PausePolicyAndEnableToggle) {
    SystemAllocator allocator;
    TickScheduler scheduler(allocator);
    RecordingExecutor executor(scheduler);
    auto normal = scheduler.Register(Make(1));
    TickRegistration ui = Make(2);
    ui.tickWhenPaused = true;
    auto always = scheduler.Register(ui);
    REQUIRE(normal && always);
    REQUIRE(scheduler.RunPhase(TickPhase::PrePhysics, Frame(0.1, true), executor));
    REQUIRE(executor.count == 1);
    CHECK(executor.order[0] == 2 && executor.deltas[0] == 0.1);

    executor.Reset();
    REQUIRE(scheduler.SetEnabled(always.Value(), false));
    REQUIRE(scheduler.RunPhase(TickPhase::PrePhysics, Frame(0.1), executor));
    CHECK(executor.count == 1 && executor.order[0] == 1);
    CHECK(scheduler.Stats().registered == 2 && scheduler.Stats().enabled == 1);
}

TEST(Tick_ChangesDuringPhaseApplyAtBoundary) {
    SystemAllocator allocator;
    TickScheduler scheduler(allocator);
    RecordingExecutor executor(scheduler);
    REQUIRE(scheduler.Register(Make(1, TickPhase::PrePhysics, 0)));
    auto victim = scheduler.Register(Make(2, TickPhase::PrePhysics, 1));
    REQUIRE(victim);
    // 1번 실행 중에 2번을 해제하고 새 등록(99)을 추가한다.
    executor.unregisterOnRun = 1;
    executor.victim = victim.Value();
    executor.registerOnRun = 1;
    REQUIRE(scheduler.RunPhase(TickPhase::PrePhysics, Frame(), executor));
    // 해제된 2번은 같은 단계에서 즉시 빠지고, 새 등록은 다음 경계부터 실행된다.
    REQUIRE(executor.count == 1);
    CHECK(executor.order[0] == 1);
    CHECK(!scheduler.IsRegistered(victim.Value()));
    CHECK(scheduler.Unregister(victim.Value()).Error() == ErrorCode::StaleHandle);
    executor.Reset();
    REQUIRE(scheduler.RunPhase(TickPhase::PrePhysics, Frame(), executor));
    REQUIRE(executor.count == 2);
    CHECK(executor.order[0] == 1 && executor.order[1] == 99);
}

TEST(Tick_ClosedSchedulerRejectsRegistrationButAllowsUnregister) {
    SystemAllocator allocator;
    TickScheduler scheduler(allocator);
    auto handle = scheduler.Register(Make(1));
    REQUIRE(handle);
    scheduler.Close();
    CHECK(scheduler.Register(Make(2)).Error() == ErrorCode::ShuttingDown);
    CHECK(scheduler.Unregister(handle.Value()).IsOk());
    CHECK(scheduler.Stats().registered == 0);
}

TEST(Tick_RegistrationAtOrAboveMaxPriorityAppendsWithoutRebuild) {
    SystemAllocator allocator;
    TickScheduler scheduler(allocator);
    RecordingExecutor executor(scheduler);
    REQUIRE(scheduler.Register(Make(1, TickPhase::PrePhysics, 0)));
    REQUIRE(scheduler.Register(Make(2, TickPhase::PrePhysics, 3)));
    REQUIRE(scheduler.RunPhase(TickPhase::PrePhysics, Frame(), executor));
    REQUIRE(scheduler.Stats().pendingChanges == 0);

    // 최댓값과 같거나 큰 priority는 재구성해도 맨 끝이므로 끝에 붙고 재구성을 예약하지 않는다.
    REQUIRE(scheduler.Register(Make(3, TickPhase::PrePhysics, 3)));
    CHECK(scheduler.Stats().pendingChanges == 0);
    REQUIRE(scheduler.Register(Make(4, TickPhase::PrePhysics, 7)));
    CHECK(scheduler.Stats().pendingChanges == 0);
    executor.Reset();
    REQUIRE(scheduler.RunPhase(TickPhase::PrePhysics, Frame(), executor));
    REQUIRE(executor.count == 4);
    CHECK(executor.order[0] == 1 && executor.order[1] == 2 && executor.order[2] == 3 &&
          executor.order[3] == 4);

    // 최댓값보다 작으면 중간에 들어가야 하므로 재구성한다.
    REQUIRE(scheduler.Register(Make(5, TickPhase::PrePhysics, 1)));
    CHECK(scheduler.Stats().pendingChanges == 1);
    executor.Reset();
    REQUIRE(scheduler.RunPhase(TickPhase::PrePhysics, Frame(), executor));
    REQUIRE(executor.count == 5);
    CHECK(executor.order[0] == 1 && executor.order[1] == 5 && executor.order[2] == 2 &&
          executor.order[3] == 3 && executor.order[4] == 4);
    CHECK(scheduler.Stats().pendingChanges == 0);

    // 재구성을 기다리는 단계에서는 끝에 붙이지 않는다(재구성이 함께 정한다).
    REQUIRE(scheduler.Register(Make(6, TickPhase::PrePhysics, -1)));
    REQUIRE(scheduler.Register(Make(7, TickPhase::PrePhysics, 9)));
    CHECK(scheduler.Stats().pendingChanges == 1);
    executor.Reset();
    REQUIRE(scheduler.RunPhase(TickPhase::PrePhysics, Frame(), executor));
    REQUIRE(executor.count == 7);
    CHECK(executor.order[0] == 6 && executor.order[1] == 1 && executor.order[6] == 7);
}

TEST(Tick_PrerequisiteAlreadyHonouredSkipsRebuild) {
    SystemAllocator allocator;
    TickScheduler scheduler(allocator);
    RecordingExecutor executor(scheduler);
    auto first = scheduler.Register(Make(1));
    auto second = scheduler.Register(Make(2));
    auto third = scheduler.Register(Make(3));
    REQUIRE(first && second && third);
    REQUIRE(scheduler.RunPhase(TickPhase::PrePhysics, Frame(), executor));
    REQUIRE(scheduler.Stats().pendingChanges == 0);

    // 지금 순서에서 이미 선행이 앞이면 재구성하지 않는다.
    REQUIRE(scheduler.AddPrerequisite(third.Value(), first.Value()));
    CHECK(scheduler.Stats().pendingChanges == 0);
    executor.Reset();
    REQUIRE(scheduler.RunPhase(TickPhase::PrePhysics, Frame(), executor));
    REQUIRE(executor.count == 3);
    CHECK(executor.order[0] == 1 && executor.order[1] == 2 && executor.order[2] == 3);

    // 선행이 뒤에 있으면 재구성한다.
    REQUIRE(scheduler.AddPrerequisite(first.Value(), second.Value()));
    CHECK(scheduler.Stats().pendingChanges == 1);
    executor.Reset();
    REQUIRE(scheduler.RunPhase(TickPhase::PrePhysics, Frame(), executor));
    REQUIRE(executor.count == 3);
    CHECK(executor.order[0] == 2 && executor.order[1] == 1 && executor.order[2] == 3);
}

TEST(Tick_DeferredRemovalOfPrerequisiteRebuildsOrder) {
    SystemAllocator allocator;
    TickScheduler scheduler(allocator);
    RecordingExecutor executor(scheduler);
    // 3번(priority -5)은 1번(priority 10)에 묶여 2번(priority 0)과 1번 뒤에 실행된다.
    auto pinned = scheduler.Register(Make(1, TickPhase::PrePhysics, 10));
    auto middle = scheduler.Register(Make(2, TickPhase::PrePhysics, 0));
    auto bound = scheduler.Register(Make(3, TickPhase::PrePhysics, -5));
    REQUIRE(pinned && middle && bound);
    REQUIRE(scheduler.AddPrerequisite(bound.Value(), pinned.Value()));
    REQUIRE(scheduler.RunPhase(TickPhase::PrePhysics, Frame(), executor));
    REQUIRE(executor.count == 3);
    CHECK(executor.order[0] == 2 && executor.order[1] == 1 && executor.order[2] == 3);

    // 실행 중에 선행(1번)을 해제한다. 단계 경계의 미룬 해제가 의존을 풀었으니 순서를 다시 만들어야
    // 3번이 priority대로 맨 앞에 온다(남은 순서 배열을 그대로 쓰면 2번, 3번 순서로 남는다).
    executor.Reset();
    executor.unregisterOnRun = 2;
    executor.victim = pinned.Value();
    REQUIRE(scheduler.RunPhase(TickPhase::PrePhysics, Frame(), executor));
    REQUIRE(executor.count == 2);
    CHECK(executor.order[0] == 2 && executor.order[1] == 3);
    CHECK(!scheduler.IsRegistered(pinned.Value()));
    CHECK(scheduler.Stats().pendingChanges == 1);

    executor.Reset();
    REQUIRE(scheduler.RunPhase(TickPhase::PrePhysics, Frame(), executor));
    REQUIRE(executor.count == 2);
    CHECK(executor.order[0] == 3 && executor.order[1] == 2);
}

TEST(Tick_CycleCheckSurvivesMarkEpochWrap) {
    SystemAllocator allocator;
    TickScheduler scheduler(allocator);
    auto first = scheduler.Register(Make(1));
    auto second = scheduler.Register(Make(2));
    auto third = scheduler.Register(Make(3));
    REQUIRE(first && second && third);
    // 이 추가의 탐색이 1번 칸에 방문 번호 1을 남긴다.
    REQUIRE(detail::TickSchedulerTestAccess::MarkEpoch(scheduler) == 0);
    REQUIRE(scheduler.AddPrerequisite(second.Value(), first.Value()));
    REQUIRE(detail::TickSchedulerTestAccess::MarkEpoch(scheduler) == 1);

    // 번호가 한 바퀴 돌면 방문 표시를 비우고 1부터 다시 쓴다. 비우지 않으면 1번 칸의 옛 표시가 새
    // 번호와 같아 2번 -> 1번 경로를 놓치고 사이클을 받아들인다.
    detail::TickSchedulerTestAccess::SetMarkEpoch(scheduler, NumericLimits<uint32>::Max());
    CHECK(scheduler.AddPrerequisite(first.Value(), second.Value()).Error() ==
          ErrorCode::CycleDetected);
    CHECK(detail::TickSchedulerTestAccess::MarkEpoch(scheduler) == 1);
    CHECK(scheduler.AddPrerequisite(third.Value(), second.Value()));
    CHECK(scheduler.AddPrerequisite(first.Value(), third.Value()).Error() ==
          ErrorCode::CycleDetected);
}
namespace {

// 가변 단계 넷(PrePhysics·PostPhysics·LateUpdate·PreRender)을 한 프레임으로 실행하고 단계마다 실행
// 순서를 "단계번호:식별자" 꼴로 남긴다(Physics 등록이 없는 시험용).
struct FrameRecord {
    uint8 phase[64] = {};
    uint8 id[64] = {};
    uint32 count = 0;

    bool Is(std::initializer_list<uint32> expected) const noexcept {
        if (expected.size() != count) {
            return false;
        }
        uint32 at = 0;
        for (uint32 packed : expected) {
            if (phase[at] != packed / 100 || id[at] != packed % 100) {
                return false;
            }
            ++at;
        }
        return true;
    }
};

FrameRecord RunVariableFrame(TickScheduler& scheduler, RecordingExecutor& executor) noexcept {
    FrameRecord record;
    scheduler.BeginFrame();
    const TickPhase phases[] = {TickPhase::PrePhysics, TickPhase::PostPhysics,
                                TickPhase::LateUpdate, TickPhase::PreRender};
    for (TickPhase phase : phases) {
        executor.Reset();
        auto ran = scheduler.RunPhase(phase, Frame(), executor);
        CHECK(ran.IsOk());
        for (uint32 at = 0; at < executor.count && record.count < 64; ++at) {
            record.phase[record.count] = static_cast<uint8>(phase);
            record.id[record.count] = executor.order[at];
            ++record.count;
        }
    }
    return record;
}

// 단계 번호(TickPhase 열거 값) × 100 + 식별자다.
constexpr uint32 Pre(uint32 id) noexcept {
    return 0 * 100 + id;
}

constexpr uint32 Post(uint32 id) noexcept {
    return 2 * 100 + id;
}

constexpr uint32 Late(uint32 id) noexcept {
    return 3 * 100 + id;
}

constexpr uint32 Render(uint32 id) noexcept {
    return 4 * 100 + id;
}

} // namespace

// 15.3절 ERD3: 선행이 더 늦은 단계면 의존하는 쪽이 그 단계로 늦춰져 선행 뒤에 실행된다.
TEST(Tick_LaterPhasePrerequisiteDemotesDependent) {
    SystemAllocator allocator;
    TickScheduler scheduler(allocator);
    RecordingExecutor executor(scheduler);
    auto a = scheduler.Register(Make(1, TickPhase::PrePhysics));
    auto b = scheduler.Register(Make(2, TickPhase::PostPhysics, 10));
    REQUIRE(a && b);
    REQUIRE(scheduler.AddPrerequisite(a.Value(), b.Value()));
    // 반영 전에는 요청 단계 그대로다.
    CHECK(scheduler.EffectivePhase(a.Value()).Value() == TickPhase::PrePhysics);

    // 프레임의 첫 단계 전에 반영된다. a는 priority가 더 낮아도 b 뒤다.
    CHECK(RunVariableFrame(scheduler, executor).Is({Post(2), Post(1)}));
    CHECK(scheduler.EffectivePhase(a.Value()).Value() == TickPhase::PostPhysics);

    // 선행을 빼면 다음 프레임에 원래 단계로 돌아간다.
    REQUIRE(scheduler.RemovePrerequisite(a.Value(), b.Value()));
    CHECK(RunVariableFrame(scheduler, executor).Is({Pre(1), Post(2)}));
    CHECK(scheduler.EffectivePhase(a.Value()).Value() == TickPhase::PrePhysics);
}

// 프레임 중간에 늦춤이 생겨도 그 프레임에 두 번 실행하거나 빠뜨리지 않는다(다음 프레임에 옮긴다).
TEST(Tick_DemotionAddedMidFrameAppliesNextFrame) {
    SystemAllocator allocator;
    TickScheduler scheduler(allocator);
    RecordingExecutor executor(scheduler);
    auto a = scheduler.Register(Make(1, TickPhase::PrePhysics));
    auto b = scheduler.Register(Make(2, TickPhase::PostPhysics));
    REQUIRE(a && b);

    scheduler.BeginFrame();
    REQUIRE(scheduler.RunPhase(TickPhase::PrePhysics, Frame(), executor));
    CHECK(executor.count == 1 && executor.order[0] == 1);
    REQUIRE(scheduler.AddPrerequisite(a.Value(), b.Value()));
    executor.Reset();
    REQUIRE(scheduler.RunPhase(TickPhase::PostPhysics, Frame(), executor));
    CHECK(executor.count == 1 && executor.order[0] == 2);

    CHECK(RunVariableFrame(scheduler, executor).Is({Post(2), Post(1)}));
}

// 늦춤은 전이적이고, 단계를 넘는 간선도 순환 검사에 들어간다.
TEST(Tick_DemotionIsTransitiveAndCrossPhaseCyclesAreRejected) {
    SystemAllocator allocator;
    TickScheduler scheduler(allocator);
    RecordingExecutor executor(scheduler);
    auto a = scheduler.Register(Make(1, TickPhase::PrePhysics));
    auto b = scheduler.Register(Make(2, TickPhase::PostPhysics));
    auto c = scheduler.Register(Make(3, TickPhase::LateUpdate));
    REQUIRE(a && b && c);
    REQUIRE(scheduler.AddPrerequisite(a.Value(), b.Value()));
    REQUIRE(scheduler.AddPrerequisite(b.Value(), c.Value()));
    CHECK(scheduler.AddPrerequisite(c.Value(), a.Value()).Error() == ErrorCode::CycleDetected);

    CHECK(RunVariableFrame(scheduler, executor).Is({Late(3), Late(2), Late(1)}));

    // 중간 선행을 해제하면 그 늦춤도 풀린다(다음 프레임).
    REQUIRE(scheduler.Unregister(b.Value()));
    CHECK(RunVariableFrame(scheduler, executor).Is({Pre(1), Late(3)}));
}

// 선행이 더 이른 단계인 간선은 지우지 않는다. 선행이 나중에 늦은 단계로 옮겨지면 다시 쓰인다.
TEST(Tick_EarlierPhaseEdgeIsKeptForLaterPhaseChanges) {
    SystemAllocator allocator;
    TickScheduler scheduler(allocator);
    RecordingExecutor executor(scheduler);
    auto a = scheduler.Register(Make(1, TickPhase::PostPhysics));
    auto b = scheduler.Register(Make(2, TickPhase::PrePhysics));
    REQUIRE(a && b);
    REQUIRE(scheduler.AddPrerequisite(a.Value(), b.Value()));
    CHECK(RunVariableFrame(scheduler, executor).Is({Pre(2), Post(1)}));

    REQUIRE(scheduler.SetPhase(b.Value(), TickPhase::PreRender));
    CHECK(RunVariableFrame(scheduler, executor).Is({Render(2), Render(1)}));
    CHECK(scheduler.EffectivePhase(a.Value()).Value() == TickPhase::PreRender);
}

// Late hook(LateUpdate)도 PreRender 선행이 있으면 PreRender로 늦춰진다(15.3절 hook별 실효 단계).
TEST(Tick_LateUpdateEntryCanBeDemotedToPreRender) {
    SystemAllocator allocator;
    TickScheduler scheduler(allocator);
    RecordingExecutor executor(scheduler);
    auto late = scheduler.Register(Make(1, TickPhase::LateUpdate));
    auto render = scheduler.Register(Make(2, TickPhase::PreRender));
    REQUIRE(late && render);
    REQUIRE(scheduler.AddPrerequisite(late.Value(), render.Value()));
    CHECK(RunVariableFrame(scheduler, executor).Is({Render(2), Render(1)}));
}

// 고정 스텝(Physics) 등록과 가변 등록은 서로 의존할 수 없고, 단계 변경으로 종류를 바꿀 수도 없다.
TEST(Tick_FixedStepEntriesDoNotMixWithVariablePhases) {
    SystemAllocator allocator;
    TickScheduler scheduler(allocator);
    auto fixed = scheduler.Register(Make(1, TickPhase::Physics));
    auto fixedOther = scheduler.Register(Make(2, TickPhase::Physics));
    auto variable = scheduler.Register(Make(3, TickPhase::PostPhysics));
    REQUIRE(fixed && fixedOther && variable);
    CHECK(scheduler.AddPrerequisite(variable.Value(), fixed.Value()).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(scheduler.AddPrerequisite(fixed.Value(), variable.Value()).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(scheduler.AddPrerequisite(fixed.Value(), fixedOther.Value()));
    CHECK(scheduler.SetPhase(variable.Value(), TickPhase::Physics).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(scheduler.SetPhase(fixed.Value(), TickPhase::PrePhysics).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(scheduler.SetPhase(variable.Value(), TickPhase::Count).Error() ==
          ErrorCode::InvalidArgument);
}

// priority·interval은 실행 중에도 바꿀 수 있고 다음 단계 경계에서 반영된다(15.3절 ERD6).
TEST(Tick_PriorityAndIntervalChangesApplyAtNextBoundary) {
    SystemAllocator allocator;
    TickScheduler scheduler(allocator);
    RecordingExecutor executor(scheduler);
    auto a = scheduler.Register(Make(1, TickPhase::PrePhysics, 0));
    auto b = scheduler.Register(Make(2, TickPhase::PrePhysics, 1));
    REQUIRE(a && b);
    CHECK(RunVariableFrame(scheduler, executor).Is({Pre(1), Pre(2)}));
    REQUIRE(scheduler.SetPriority(a.Value(), 5));
    CHECK(RunVariableFrame(scheduler, executor).Is({Pre(2), Pre(1)}));

    // interval 변경은 누적을 0으로 되돌린다: 0.25초 간격, 0.1초 프레임이면 세 번째 프레임에 실행.
    REQUIRE(scheduler.SetInterval(b.Value(), 0.25));
    CHECK(RunVariableFrame(scheduler, executor).Is({Pre(1)}));
    CHECK(RunVariableFrame(scheduler, executor).Is({Pre(1)}));
    CHECK(RunVariableFrame(scheduler, executor).Is({Pre(2), Pre(1)}));
    CHECK(scheduler.SetInterval(b.Value(), -1.0).Error() == ErrorCode::InvalidArgument);

    REQUIRE(scheduler.Unregister(a.Value()));
    CHECK(scheduler.SetPriority(a.Value(), 0).Error() == ErrorCode::StaleHandle);
    CHECK(scheduler.SetPhase(a.Value(), TickPhase::PostPhysics).Error() ==
          ErrorCode::StaleHandle);
    CHECK(scheduler.EffectivePhase(a.Value()).Error() == ErrorCode::StaleHandle);
}

} // namespace tick_scheduler_tests_file
