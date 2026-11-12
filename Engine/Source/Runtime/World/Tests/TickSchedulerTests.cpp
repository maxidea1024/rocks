#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <World/TickScheduler.h>

using namespace rocks;

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
    auto other = scheduler.Register(Make(4, TickPhase::LateUpdate));
    REQUIRE(a && b && c && other);
    // a는 c 뒤에 실행되어야 한다. c는 priority가 가장 낮지만 a 앞으로 당겨진다.
    REQUIRE(scheduler.AddPrerequisite(a.Value(), c.Value()));
    CHECK(scheduler.AddPrerequisite(a.Value(), c.Value()).IsOk()); // 중복은 성공
    REQUIRE(scheduler.AddPrerequisite(c.Value(), b.Value()));
    // 사이클(b → a)과 자기 의존, 다른 단계 의존은 거부한다.
    CHECK(scheduler.AddPrerequisite(b.Value(), a.Value()).Error() == ErrorCode::InvalidArgument);
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
          ErrorCode::InvalidArgument);
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
