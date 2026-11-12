#include <Core/Time/GameClock.h>
#include <TestSupport/Test.h>

using namespace rocks;

namespace {

bool Near(double actual, double expected, double tolerance = 1.0e-9) noexcept {
    const double difference = actual - expected;
    return difference <= tolerance && difference >= -tolerance;
}

// frames번 delta를 진행하고 고정 스텝·게임 시간 합을 돌려준다.
struct Totals {
    uint64 fixedSteps = 0;
    double gameSeconds = 0.0;
    double droppedSeconds = 0.0;
};

Totals Run(GameClock& clock, double delta, uint32 frames) noexcept {
    Totals totals;
    for (uint32 frame = 0; frame < frames; ++frame) {
        auto plan = clock.Advance(delta);
        if (!plan) {
            return {};
        }
        totals.fixedSteps += plan.Value().fixedSteps;
        totals.gameSeconds += plan.Value().deltaSeconds;
        totals.droppedSeconds += plan.Value().droppedSeconds;
    }

    return totals;
}

} // namespace

TEST(GameClock_FixedStepCountMatchesElapsedTimeAtCommonFrameRates) {
    // 30·60·144 FPS로 정확히 2초를 진행하면 60Hz 고정 스텝은 120번이다.
    const double rates[] = {30.0, 60.0, 144.0};
    for (double rate : rates) {
        GameClock clock;
        const uint32 frames = static_cast<uint32>(rate * 2.0);
        const Totals totals = Run(clock, 1.0 / rate, frames);
        CHECK(totals.fixedSteps == 120);
        CHECK(Near(totals.gameSeconds, 2.0, 1.0e-6));
        CHECK(totals.droppedSeconds == 0.0);
        CHECK(clock.FixedStepCount() == 120);
        CHECK(clock.FrameIndex() == frames);
    }
}

TEST(GameClock_InterpolationAlphaIsRemainderOverStep) {
    GameClock clock;
    REQUIRE(clock.SetPolicy({0.1, 1.0, 8}));
    auto plan = clock.Advance(0.25);
    REQUIRE(plan);
    CHECK(plan.Value().fixedSteps == 2);
    CHECK(Near(plan.Value().interpolationAlpha, 0.5));
    CHECK(plan.Value().firstFixedStepIndex == 0);
    auto next = clock.Advance(0.05);
    REQUIRE(next);
    CHECK(next.Value().fixedSteps == 1);
    CHECK(next.Value().firstFixedStepIndex == 2);
    CHECK(Near(next.Value().interpolationAlpha, 0.0, 1.0e-6));
}

TEST(GameClock_TimeScaleAppliesOnceToVariableAndFixedTime) {
    GameClock clock;
    REQUIRE(clock.SetPolicy({0.1, 1.0, 16}));
    REQUIRE(clock.SetTimeScale(2.0));
    auto plan = clock.Advance(0.25);
    REQUIRE(plan);
    // 게임 delta 0.5 → 고정 스텝 5번. scale을 delta와 accumulator에 두 번 곱하면 10번이 된다.
    CHECK(Near(plan.Value().deltaSeconds, 0.5));
    CHECK(Near(plan.Value().unscaledDeltaSeconds, 0.25));
    CHECK(plan.Value().fixedSteps == 5);

    REQUIRE(clock.SetTimeScale(0.1));
    const Totals slow = Run(clock, 0.1, 10);
    CHECK(Near(slow.gameSeconds, 0.1));
    CHECK(slow.fixedSteps == 1);

    // 0은 게임 시간을 멈추지만 pause 상태는 아니다.
    REQUIRE(clock.SetTimeScale(0.0));
    auto stopped = clock.Advance(0.5);
    REQUIRE(stopped);
    CHECK(stopped.Value().deltaSeconds == 0.0);
    CHECK(stopped.Value().fixedSteps == 0);
    CHECK(!clock.IsPaused());
}

TEST(GameClock_PauseStopsGameTimeAndResumeKeepsScaleWithoutCatchUp) {
    GameClock clock;
    REQUIRE(clock.SetPolicy({0.1, 1.0, 16}));
    REQUIRE(clock.SetTimeScale(0.5));
    clock.SetPaused(true);
    const Totals paused = Run(clock, 0.2, 10);
    CHECK(paused.fixedSteps == 0);
    CHECK(paused.gameSeconds == 0.0);
    CHECK(Near(clock.UnscaledTimeSeconds(), 2.0));

    // pause 중 흐른 시간을 resume 뒤에 한꺼번에 따라잡지 않는다. 직전 scale 0.5가 다시 적용된다.
    clock.SetPaused(false);
    auto resumed = clock.Advance(0.2);
    REQUIRE(resumed);
    CHECK(Near(resumed.Value().deltaSeconds, 0.1));
    CHECK(resumed.Value().fixedSteps == 1);
    CHECK(clock.TimeScale() == 0.5);
}

TEST(GameClock_SingleStepRequestIsConsumedByNextAdvance) {
    GameClock clock;
    REQUIRE(clock.SetPolicy({0.1, 1.0, 8}));
    CHECK(clock.RequestSingleStep().Error() == ErrorCode::InvalidArgument);
    clock.SetPaused(true);
    REQUIRE(clock.RequestSingleStep());
    auto step = clock.Advance(0.016);
    REQUIRE(step);
    CHECK(step.Value().singleStep);
    CHECK(step.Value().fixedSteps == 1);
    CHECK(Near(step.Value().deltaSeconds, 0.1));
    // 실제·unscaled 시간은 측정값 그대로 흐른다(UI·timeout 왜곡 없음).
    CHECK(Near(step.Value().unscaledDeltaSeconds, 0.016));
    CHECK(step.Value().frameIndex == 1);
    CHECK(clock.FixedStepCount() == 1);
    CHECK(Near(clock.GameTimeSeconds(), 0.1));
    // 요청은 한 번만 소비된다.
    auto after = clock.Advance(0.3);
    REQUIRE(after);
    CHECK(!after.Value().singleStep && after.Value().fixedSteps == 0);
    CHECK(after.Value().frameIndex == 2);
}

TEST(GameClock_LongFrameIsClampedAndExcessSubstepsAreReported) {
    GameClock clock;
    REQUIRE(clock.SetPolicy({0.01, 0.25, 4}));
    auto plan = clock.Advance(3.0);
    REQUIRE(plan);
    CHECK(Near(plan.Value().clampedSeconds, 2.75));
    CHECK(Near(plan.Value().unscaledDeltaSeconds, 0.25));
    CHECK(plan.Value().fixedSteps == 4);
    // 0.25초 중 4스텝(0.04초)만 실행하고 남은 0.21초는 버린다(한 스텝 미만 잔여는 없다).
    CHECK(Near(plan.Value().droppedSeconds, 0.21, 1.0e-9));
    CHECK(plan.Value().interpolationAlpha >= 0.0 && plan.Value().interpolationAlpha < 1.0);
    auto next = clock.Advance(0.0);
    REQUIRE(next);
    CHECK(next.Value().fixedSteps == 0);
}

TEST(GameClock_RejectsInvalidInputWithoutChangingState) {
    GameClock clock;
    const double zero = 0.0;
    const double nan = zero / zero;
    const double infinity = 1.0 / zero;
    CHECK(clock.Advance(-0.1).Error() == ErrorCode::InvalidArgument);
    CHECK(clock.Advance(nan).Error() == ErrorCode::InvalidArgument);
    CHECK(clock.Advance(infinity).Error() == ErrorCode::InvalidArgument);
    CHECK(clock.FrameIndex() == 0);
    CHECK(clock.SetTimeScale(-1.0).Error() == ErrorCode::InvalidArgument);
    CHECK(clock.SetTimeScale(nan).Error() == ErrorCode::InvalidArgument);
    CHECK(clock.TimeScale() == 1.0);
    CHECK(clock.SetPolicy({0.0, 1.0, 1}).Error() == ErrorCode::InvalidArgument);
    CHECK(clock.SetPolicy({0.1, -1.0, 1}).Error() == ErrorCode::InvalidArgument);
    CHECK(clock.SetPolicy({0.1, 1.0, 0}).Error() == ErrorCode::InvalidArgument);
    CHECK(clock.SetPolicy({nan, 1.0, 1}).Error() == ErrorCode::InvalidArgument);
    // 감산이 진척하지 않는 극소 스텝, 지나친 스텝 수, 과도한 배율은 거부한다.
    CHECK(clock.SetPolicy({1.0e-20, 1.0, 1}).Error() == ErrorCode::InvalidArgument);
    CHECK(clock.SetPolicy({2.0, 1.0, 1}).Error() == ErrorCode::InvalidArgument);
    CHECK(clock.SetPolicy({0.1, 1.0, TimeStepPolicy::MaxSubsteps + 1}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(clock.SetTimeScale(GameClock::MaxTimeScale * 2.0).Error() == ErrorCode::InvalidArgument);
    CHECK(clock.Policy().fixedStepSeconds == 1.0 / 60.0);
}

TEST(GameClock_ExtremeButValidSettingsKeepAlphaInRange) {
    // 최소 스텝 + 최대 배율 + 긴 프레임: 대부분 버려지지만 보간 비율과 누적은 [0, 1) 안에 머문다.
    GameClock clock;
    REQUIRE(clock.SetPolicy({TimeStepPolicy::MinFixedStepSeconds, 10.0, 3}));
    REQUIRE(clock.SetTimeScale(GameClock::MaxTimeScale));
    for (int frame = 0; frame < 20; ++frame) {
        auto plan = clock.Advance(0.37 + frame * 0.013);
        REQUIRE(plan);
        CHECK(plan.Value().fixedSteps == 3);
        CHECK(plan.Value().interpolationAlpha >= 0.0 && plan.Value().interpolationAlpha < 1.0);
        CHECK(plan.Value().droppedSeconds >= 0.0);
    }
    CHECK(clock.FixedStepCount() == 60);
}

TEST(GameClock_SingleStepUsesExplicitDebugDeltaAndUnpauseDropsRequest) {
    GameClock clock;
    REQUIRE(clock.SetPolicy({0.1, 1.0, 8}));
    clock.SetPaused(true);
    REQUIRE(clock.RequestSingleStep(0.25));
    auto step = clock.Advance(0.0);
    REQUIRE(step);
    CHECK(step.Value().fixedSteps == 1 && Near(step.Value().deltaSeconds, 0.25));
    CHECK(clock.RequestSingleStep(-0.5).Error() == ErrorCode::InvalidArgument);
    CHECK(clock.RequestSingleStep(-1.0).Error() == ErrorCode::InvalidArgument);
    // pause를 풀면 대기 중인 요청은 버린다.
    REQUIRE(clock.RequestSingleStep());
    clock.SetPaused(false);
    clock.SetPaused(true);
    auto paused = clock.Advance(0.1);
    REQUIRE(paused);
    CHECK(!paused.Value().singleStep && paused.Value().fixedSteps == 0);
    // scale 0은 pause가 아니므로 단일 스텝을 요청할 수 없다.
    clock.SetPaused(false);
    REQUIRE(clock.SetTimeScale(0.0));
    CHECK(clock.RequestSingleStep().Error() == ErrorCode::InvalidArgument);
}

TEST(GameClock_FixedTimeSurvivesStepLengthChange) {
    GameClock clock;
    REQUIRE(clock.SetPolicy({0.1, 1.0, 16}));
    for (int frame = 0; frame < 10; ++frame) {
        REQUIRE(clock.Advance(0.1));
    }
    CHECK(Near(clock.FixedTimeSeconds(), 1.0, 1.0e-6));
    REQUIRE(clock.SetPolicy({0.2, 1.0, 16}));
    CHECK(Near(clock.FixedTimeSeconds(), 1.0, 1.0e-6));
    REQUIRE(clock.Advance(0.4));
    CHECK(Near(clock.FixedTimeSeconds(), 1.4, 1.0e-6));
    CHECK(clock.SetPolicy({0.1, TimeStepPolicy::MaxFrameDeltaSeconds * 2.0, 1}).Error() ==
          ErrorCode::InvalidArgument);
}

TEST(GameClock_ChangingFixedStepResetsAccumulator) {
    GameClock clock;
    REQUIRE(clock.SetPolicy({0.1, 1.0, 8}));
    auto partial = clock.Advance(0.05);
    REQUIRE(partial);
    CHECK(Near(partial.Value().interpolationAlpha, 0.5));
    REQUIRE(clock.SetPolicy({0.2, 1.0, 8}));
    auto plan = clock.Advance(0.1);
    REQUIRE(plan);
    CHECK(plan.Value().fixedSteps == 0);
    CHECK(Near(plan.Value().interpolationAlpha, 0.5));
}
