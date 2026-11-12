#pragma once

#include <Core/Result.h>

// 게임 시계와 프레임 스텝 계획이다(엔진 구현 계획 8.3절의 StepPlanner). 한 프레임의 실제 경과
// 시간을 받아 가변 Update에 쓸 게임 delta와 고정 Update를 몇 번 실행할지 계산한다. OS 시계를 읽지
// 않는 순수 값 계산이므로 테스트에서 임의 delta로 재현할 수 있다. 실제 경과 시간은 호출자가
// platform::MonotonicNanoseconds로 측정해 넘긴다.
//
// 사용 예:
//   GameClock clock;
//   if (!clock.SetPolicy({.fixedStepSeconds = 1.0 / 60.0, .maxFrameDeltaSeconds = 0.25,
//                         .maxSubsteps = 8})) { ... }
//   auto plan = clock.Advance(measuredSeconds);
//   if (!plan) { ... }                        // 음수·NaN 입력
//   for (uint32 step = 0; step < plan.Value().fixedSteps; ++step) {
//       FixedUpdate(plan.Value().fixedStepSeconds);
//   }
//   Update(plan.Value().deltaSeconds);         // pause면 0
//   Render(plan.Value().interpolationAlpha);   // 이전·현재 고정 상태 보간 비율
//
// 시간 도메인:
// - 실제 시간(real): 측정값 그대로. 통계용이다.
// - unscaled: maxFrameDeltaSeconds로 자른 실제 시간. pause·scale과 무관하게 흐르며
//   UI·timeout에 쓴다.
// - 게임 시간(game): unscaled × timeScale, pause면 0. 가변 Update와 고정 accumulator가
//   같은 값을 쓰므로 scale·pause를 두 번 적용하지 않는다.
// - 고정 시간: 정수 스텝 번호 × fixedStepSeconds. 누적 덧셈 오차가 쌓이지 않는다.
//
// 스레드: 동기화하지 않는 값 타입이다. 한 스레드(보통 게임 스레드)에서만 변경한다.
namespace rocks {

// 스텝 계획 정책이다. 기본값은 60Hz 고정 스텝, 프레임당 실제 delta 최대 0.25초, 고정 스텝 최대
// 8회다.
struct TimeStepPolicy {
    // 고정 Update 한 번의 길이(초). [MinFixedStepSeconds, 1] 범위여야 한다.
    double fixedStepSeconds = 1.0 / 60.0;
    // 한 프레임에서 인정하는 실제 delta 상한(초). 디버거 정지·로딩 같은 긴 정지를 한 번에 따라잡지
    // 않게 한다. 유한한 양수여야 한다.
    double maxFrameDeltaSeconds = 0.25;
    // 한 프레임의 고정 스텝 실행 상한. [1, MaxSubsteps] 범위여야 한다. 넘는 누적 시간은 버리고
    // 보고한다.
    uint32 maxSubsteps = 8;

    // 고정 스텝 하한(10µs)이다. 이보다 작으면 누적 감산이 부동소수점 반올림으로 진척하지 않을 수
    // 있다.
    static constexpr double MinFixedStepSeconds = 1.0e-5;
    // 프레임당 고정 스텝 상한의 최댓값이다. 한 프레임이 무한정 길어지는 설정을 막는다.
    static constexpr uint32 MaxSubsteps = 1024;
};

// Advance/StepSingle 한 번의 결과다. 모든 시간은 초 단위다.
struct StepPlan {
    // 호출자가 측정해 넘긴 실제 경과 시간이다.
    double realDeltaSeconds = 0.0;
    // maxFrameDeltaSeconds로 자른 실제 시간이다. pause·scale을 적용하지 않는다.
    double unscaledDeltaSeconds = 0.0;
    // 가변 Update에 넘길 게임 delta다. pause면 0이다.
    double deltaSeconds = 0.0;
    // 이번 프레임에 실행할 고정 스텝 수다(maxSubsteps 이하).
    uint32 fixedSteps = 0;
    // 고정 스텝 한 번의 길이다(정책 값).
    double fixedStepSeconds = 0.0;
    // 고정 스텝 실행 후 남은 누적 / fixedStepSeconds. [0, 1) 범위의 렌더 보간 비율이다.
    double interpolationAlpha = 0.0;
    // 프레임 상한으로 잘라낸 실제 시간이다(realDelta - unscaledDelta).
    double clampedSeconds = 0.0;
    // maxSubsteps를 넘어 버린 게임 시간이다. 0이 아니면 시뮬레이션이 실제보다 느리게 진행했다.
    double droppedSeconds = 0.0;
    // 이 계획의 프레임 번호다. 첫 Advance가 1이다.
    uint64 frameIndex = 0;
    // 이 계획의 첫 고정 스텝 번호다. fixedSteps가 0이면 다음에 실행할 번호다. 첫 스텝이 0이다.
    uint64 firstFixedStepIndex = 0;
};

// 한 시간 도메인(엔진 전체 또는 World 하나)의 게임 시계다. pause와 timeScale은 서로 독립 상태라서
// pause를 풀면 직전 scale이 그대로 적용된다. 상태 변경은 다음 Advance부터 반영된다(프레임 경계).
class GameClock {
public:
    GameClock() noexcept = default;

    // 정책을 바꾼다. 검증에 실패하면 InvalidArgument이며 기존 정책을 유지한다. 고정 스텝 길이가
    // 바뀌면 남은 누적을 0으로 비워 새 길이로 다시 시작한다(이전 길이 기준의 보간 비율은 의미가
    // 없다).
    Result<void> SetPolicy(const TimeStepPolicy& policy) noexcept;

    const TimeStepPolicy& Policy() const noexcept {
        return policy_;
    }

    // 실제 경과 시간을 받아 이번 프레임의 계획을 만들고 시계를 진행한다. 음수·NaN·무한대는
    // InvalidArgument이며 상태를 바꾸지 않는다. 0은 허용한다(같은 시각에 두 번 호출).
    Result<StepPlan> Advance(double realDeltaSeconds) noexcept;

    // pause 상태에서 고정 스텝 정확히 한 번을 실행하는 디버그 단일 스텝을 계획한다. 가변 Update에
    // 넘길 delta는 debugDeltaSeconds(생략하면 fixedStepSeconds)이며 고정 누적은 바꾸지 않는다.
    // pause가 아니거나 debugDeltaSeconds가 음수·비유한 값이면 InvalidArgument다.
    Result<StepPlan> StepSingle(double debugDeltaSeconds = -1.0) noexcept;

    // 시간 배율이다. [0, MaxTimeScale] 범위의 유한한 값만 허용하며 그 외는 InvalidArgument다. 0은
    // 게임 시간을 멈추지만 pause와는 별개 상태다: IsPaused는 false로 남고, StepSingle은 거부되며,
    // World의 "pause 중 실행" Tick도 돌지 않는다. 디버그 정지에는 SetPaused를 쓴다. 역재생은 seek
    // 경로를 쓴다.
    Result<void> SetTimeScale(double scale) noexcept;

    // 시간 배율 상한이다. 큰 배율은 고정 스텝 상한에 걸려 대부분 버려지므로 의미가 없다.
    static constexpr double MaxTimeScale = 1000.0;

    double TimeScale() const noexcept {
        return timeScale_;
    }

    void SetPaused(bool paused) noexcept {
        paused_ = paused;
    }

    bool IsPaused() const noexcept {
        return paused_;
    }

    // 지금까지 진행한 게임 시간(가변 delta의 합)이다.
    double GameTimeSeconds() const noexcept {
        return gameTime_;
    }

    // 지금까지 진행한 unscaled 시간이다.
    double UnscaledTimeSeconds() const noexcept {
        return unscaledTime_;
    }

    // 실행한 고정 스텝 수다. 고정 시간은 FixedStepCount() × fixedStepSeconds다.
    uint64 FixedStepCount() const noexcept {
        return fixedSteps_;
    }

    // 마지막으로 계획한 프레임 번호다. Advance 전에는 0이다.
    uint64 FrameIndex() const noexcept {
        return frameIndex_;
    }

private:
    TimeStepPolicy policy_;
    double timeScale_ = 1.0;
    bool paused_ = false;
    double accumulator_ = 0.0;
    double gameTime_ = 0.0;
    double unscaledTime_ = 0.0;
    uint64 fixedSteps_ = 0;
    uint64 frameIndex_ = 0;
};

} // namespace rocks
