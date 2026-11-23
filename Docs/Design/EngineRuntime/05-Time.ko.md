# 엔진 런타임 — 5. 시간

[목차](README.ko.md)

## 5. 시간

Core에 `Core/Time/GameClock.h`를 둔다(계획 8.3절의 StepPlanner).

- `TimeStepPolicy{fixedStepSeconds, maxFrameDeltaSeconds, maxSubsteps}`.
- `GameClock::Advance(realDeltaSeconds) → StepPlan{unscaledDelta, delta, fixedSteps, fixedStep, alpha, droppedSeconds}`.
- 실제 시간 delta는 `maxFrameDeltaSeconds`로 자른다. 게임 delta = 잘린 실제 delta × scale이며 pause면 0이다. 고정 accumulator에는 게임 delta만 더하므로 scale·pause가 같은 값을 두 번 적용하지 않는다.
- 한 프레임의 고정 스텝 수는 `maxSubsteps`를 넘지 않으며, 넘는 누적은 버리고 `droppedSeconds`로 보고한다.
- pause 중 `RequestSingleStep(debugDelta)`은 단일 스텝을 요청하고, 다음 `Advance`가 이를 소비해 고정 스텝 정확히 한 번과 디버그 delta(생략 시 fixedStep)를 계획한다(`StepPlan::singleStep`). 실제·unscaled 시간은 측정값 그대로 흐르고, World는 그 프레임을 pause가 아닌 것처럼 실행한다.
- scale은 [0, 1000] 범위의 유한한 값이다. 0은 게임 시간을 멈추지만 pause 상태가 아니다(단일 스텝 요청 거부, `TickWhenPaused` 미실행). 음수·NaN은 `InvalidArgument`다. scale과 pause는 별도 상태라 pause 해제 시 이전 scale이 그대로 적용된다.
- 고정 스텝은 [10µs, 1s], 프레임당 고정 스텝 상한은 [1, 1024], 프레임 delta 상한은 (0, 60s]다. 고정 시간은 스텝 길이가 바뀐 시점을 기준점으로 누적한다. 극단값에서도 보간 비율과 잔여 누적을 [0, 1)·[0, step)으로 고정한다.
- Engine은 실제 시간과 프레임 번호를, World는 자기 `GameClock`을 가진다. World별 pause·scale이 독립이다.
