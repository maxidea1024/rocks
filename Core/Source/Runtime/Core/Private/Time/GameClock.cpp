#include <Core/Math/MathUtility.h>
#include <Core/Time/GameClock.h>

namespace rocks {

namespace {

// 1/60처럼 이진수로 정확히 표현되지 않는 스텝은 같은 길이의 delta를 더해도 누적이 스텝보다 아주
// 조금 작을 수 있다. 상대 오차 1e-9 이내는 한 스텝으로 인정해 60프레임 × 1/60초가 59스텝이 되지
// 않게 한다. 인정한 만큼의 음수 잔여는 0으로 정리한다.
constexpr double StepTolerance = 1.0e-9;

bool IsPositiveFinite(double value) noexcept {
    return math::IsFinite(value) && value > 0.0;
}

} // namespace

Result<void> GameClock::SetPolicy(const TimeStepPolicy& policy) noexcept {
    if (!math::IsFinite(policy.fixedStepSeconds) ||
        policy.fixedStepSeconds < TimeStepPolicy::MinFixedStepSeconds ||
        policy.fixedStepSeconds > 1.0 || !IsPositiveFinite(policy.maxFrameDeltaSeconds) ||
        policy.maxFrameDeltaSeconds > TimeStepPolicy::MaxFrameDeltaSeconds ||
        policy.maxSubsteps == 0 || policy.maxSubsteps > TimeStepPolicy::MaxSubsteps) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (policy.fixedStepSeconds != policy_.fixedStepSeconds) {
        accumulator_ = 0.0;
        fixedTimeBase_ = FixedTimeSeconds();
        fixedStepsBase_ = fixedSteps_;
    }

    policy_ = policy;
    return Result<void>::Success();
}

Result<StepPlan> GameClock::Advance(double realDeltaSeconds) noexcept {
    if (!math::IsFinite(realDeltaSeconds) || realDeltaSeconds < 0.0) {
        return Result<StepPlan>::Failure(ErrorCode::InvalidArgument);
    }

    StepPlan plan;
    plan.realDeltaSeconds = realDeltaSeconds;
    plan.unscaledDeltaSeconds = realDeltaSeconds < policy_.maxFrameDeltaSeconds
                                    ? realDeltaSeconds
                                    : policy_.maxFrameDeltaSeconds;
    plan.clampedSeconds = realDeltaSeconds - plan.unscaledDeltaSeconds;
    plan.deltaSeconds = paused_ ? 0.0 : plan.unscaledDeltaSeconds * timeScale_;
    plan.fixedStepSeconds = policy_.fixedStepSeconds;
    plan.frameIndex = frameIndex_ + 1;
    plan.firstFixedStepIndex = fixedSteps_;
    if (paused_ && singleStepPending_) {
        // 디버그 단일 스텝: 고정 스텝 한 번, 가변 delta는 요청값, 고정 누적·보간 비율은 그대로다.
        singleStepPending_ = false;
        plan.singleStep = true;
        plan.deltaSeconds = singleStepDelta_;
        plan.fixedSteps = 1;
        plan.interpolationAlpha = accumulator_ / policy_.fixedStepSeconds;
        gameTime_ += plan.deltaSeconds;
        unscaledTime_ += plan.unscaledDeltaSeconds;
        fixedSteps_ += 1;
        frameIndex_ = plan.frameIndex;
        return Result<StepPlan>::Success(plan);
    }

    // 고정 accumulator는 가변 Update와 같은 게임 delta를 받는다(scale·pause의 이중 적용 방지).
    double accumulator = accumulator_ + plan.deltaSeconds;
    const double step = policy_.fixedStepSeconds;
    const double threshold = step * (1.0 - StepTolerance);
    uint32 steps = 0;
    while (accumulator >= threshold && steps < policy_.maxSubsteps) {
        accumulator -= step;
        ++steps;
    }

    if (accumulator < 0.0) {
        accumulator = 0.0;
    }

    // 상한에 걸려 남은 누적 중 한 스텝 이상은 버린다. 한 스텝 미만은 다음 프레임 보간에 쓴다.
    // floor 반올림으로 나머지가 [0, step) 밖으로 나가지 않게 고정한다.
    if (accumulator >= threshold) {
        double kept = accumulator - step * math::Floor(accumulator / step);
        if (!(kept >= 0.0) || kept >= threshold) {
            kept = 0.0;
        }
        plan.droppedSeconds = accumulator - kept;
        accumulator = kept;
    }

    plan.fixedSteps = steps;
    plan.interpolationAlpha = accumulator / step;
    if (!(plan.interpolationAlpha >= 0.0) || plan.interpolationAlpha >= 1.0) {
        plan.interpolationAlpha = 0.0;
        accumulator = 0.0;
    }

    accumulator_ = accumulator;
    gameTime_ += plan.deltaSeconds;
    unscaledTime_ += plan.unscaledDeltaSeconds;
    fixedSteps_ += steps;
    frameIndex_ = plan.frameIndex;
    return Result<StepPlan>::Success(plan);
}

Result<void> GameClock::RequestSingleStep() noexcept {
    return RequestSingleStep(policy_.fixedStepSeconds);
}

Result<void> GameClock::RequestSingleStep(double debugDeltaSeconds) noexcept {
    if (!paused_ || !math::IsFinite(debugDeltaSeconds) || debugDeltaSeconds < 0.0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    singleStepDelta_ = debugDeltaSeconds;
    singleStepPending_ = true;
    return Result<void>::Success();
}

Result<void> GameClock::SetTimeScale(double scale) noexcept {
    if (!math::IsFinite(scale) || scale < 0.0 || scale > GameClock::MaxTimeScale) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    timeScale_ = scale;
    return Result<void>::Success();
}

} // namespace rocks
