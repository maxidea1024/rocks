#include <Base/InitializerList.h>
#include <InputActions/ActionEvent.h>
#include <InputActions/InputTrigger.h>
#include <InputActions/TriggerCombiner.h>
#include <TestSupport/Test.h>
#include <stdio.h>

// 입력 액션 트리거 시험이다(Docs/Design/InputActions 3.2–3.4절 — IA1): 종류마다 시간열, 시간 확장,
// chord, 결합 규칙, 전이 이벤트표.
using namespace rocks;
using namespace rocks::input;
namespace input_trigger_tests_file {

namespace {

constexpr double TriggerStep = 0.1;

const char* StateName(TriggerState state) noexcept {
    switch (state) {
    case TriggerState::None:
        return "None";
    case TriggerState::Ongoing:
        return "Ongoing";
    case TriggerState::Triggered:
        return "Triggered";
    }

    return "?";
}

// values를 차례로 넣고 결합 상태열이 expected와 같은지 본다(결합기는 평가마다 새것 — 평가기와
// 같다).
template <usize Count>
bool Sequence(const InputTrigger& trigger, const double (&values)[Count],
              const TriggerState (&expected)[Count], double timeDilation = 1.0,
              double deltaSeconds = TriggerStep) noexcept {
    InputTriggerState state;
    bool same = true;
    for (usize index = 0; index < Count; ++index) {
        TriggerCombiner combiner;
        const InputValue value(InputValueType::Axis1D, Vec3d(values[index], 0.0, 0.0));
        const TriggerState got = combiner.Evaluate(Span<const InputTrigger>(&trigger, 1),
                                                   Span<InputTriggerState>(&state, 1), value,
                                                   TriggerContext{deltaSeconds, timeDilation});
        if (got != expected[index]) {
            printf("  step %u: %s, expected %s\n", static_cast<unsigned>(index), StateName(got),
                   StateName(expected[index]));
            same = false;
        }
    }

    return same;
}

constexpr TriggerState N = TriggerState::None;
constexpr TriggerState O = TriggerState::Ongoing;
constexpr TriggerState T = TriggerState::Triggered;

// chord 대상 상태 표(액션 번호 색인)를 쓰는 문맥이다.
template <usize Count>
TriggerContext ChordContext(const TriggerState (&states)[Count]) noexcept {
    return TriggerContext{TriggerStep, 1.0, Span<const TriggerState>(states, Count)};
}

} // namespace

// Down·Pressed·Released와 작동 임계(|v|² ≥ thr²).
TEST(InputTrigger_InstantKinds) {
    InputTrigger down;
    CHECK(Sequence(down, {0.6, 0.4, -0.5, 0.0}, {T, N, T, N}));
    down.actuationThreshold = 0.1f;
    CHECK(Sequence(down, {0.15, 0.05}, {T, N}));

    InputTrigger pressed;
    pressed.type = InputTriggerType::Pressed;
    CHECK(Sequence(pressed, {1.0, 1.0, 0.0, 1.0}, {T, N, N, T}));

    InputTrigger released;
    released.type = InputTriggerType::Released;
    CHECK(Sequence(released, {1.0, 1.0, 0.0, 0.0}, {O, O, T, N}));
}

// Hold(반복·한 번)·HoldAndRelease·Tap — dt 0.1.
TEST(InputTrigger_TimedKinds) {
    InputTrigger hold;
    hold.type = InputTriggerType::Hold;
    hold.holdTime = 0.25f;
    CHECK(Sequence(hold, {1, 1, 1, 1, 0}, {O, O, T, T, N}));
    hold.oneShot = true;
    CHECK(Sequence(hold, {1, 1, 1, 1, 0, 1, 1, 1}, {O, O, T, N, N, O, O, T}));

    // HoldAndRelease: 놓는 평가에서 이번 dt를 더해 0.3 ≥ 0.25면 Triggered.
    InputTrigger release = InputTrigger::HoldAndReleaseDefault();
    release.holdTime = 0.25f;
    CHECK(Sequence(release, {1, 1, 0}, {O, O, T}));
    CHECK(Sequence(release, {1, 0}, {O, N}));
    CHECK(release.holdTime == 0.25f && InputTrigger::HoldAndReleaseDefault().holdTime == 0.5f);

    // Tap: 0.2 안에 놓으면 Triggered, 오래 누르면 None으로 멈춘다.
    InputTrigger tap;
    tap.type = InputTriggerType::Tap;
    CHECK(Sequence(tap, {1, 0}, {O, T}));
    // 경계(누적 0.2 = 임계 0.2)는 누적 반올림에 좌우되므로 임계를 0.15로 둬 피한다.
    tap.tapReleaseTime = 0.15f;
    CHECK(Sequence(tap, {1, 0}, {O, T}));
    CHECK(Sequence(tap, {1, 1, 1, 0}, {O, N, N, N}));
}

// RepeatedTap(2번, 간격 0.5 — 평가 dt 시계)·Pulse.
TEST(InputTrigger_RepeatedTapAndPulse) {
    InputTrigger repeated;
    repeated.type = InputTriggerType::RepeatedTap;
    CHECK(AlwaysTicks(InputTriggerType::RepeatedTap) && !AlwaysTicks(InputTriggerType::Tap));
    // 탭(누름 → 놓음) → 탭 사이 Ongoing → 둘째 탭에 Triggered.
    CHECK(Sequence(repeated, {1, 0, 0, 1, 0}, {O, O, O, O, T}));
    // 간격을 넘기면 None으로 끝난다. 간격을 0.45로 둬 기한(0.65)이 평가 시각과 겹치지 않게 한다.
    repeated.repeatDelay = 0.45f;
    CHECK(Sequence(repeated, {1, 0, 0, 1, 0}, {O, O, O, O, T}));
    CHECK(Sequence(repeated, {1, 0, 0, 0, 0, 0, 0, 0}, {O, O, O, O, O, O, N, N}));
    // 시계는 dilation을 받지 않는다(UE 벽시계와 같다): dilation 0이어도 기한이 끝난다.
    InputTrigger dilated = repeated;
    dilated.affectedByTimeDilation = true;
    CHECK(Sequence(dilated, {1, 0, 0, 0, 0, 0, 0, 0}, {O, O, O, O, O, O, N, N}, 0.0));
    // 첫 탭은 세지 않는다(마감 없음은 늘 과거 — UE RepeatTime 0): dt 0이어도 탭 한 번은
    // Ongoing이다.
    CHECK(Sequence(repeated, {1, 0}, {O, O}, 1.0, 0.0));
    CHECK(Sequence(repeated, {1, 0, 1, 0}, {O, O, O, T}, 1.0, 0.0));
    // 세 번 탭: 셋째에 Triggered.
    InputTrigger triple;
    triple.type = InputTriggerType::RepeatedTap;
    triple.tapCount = 3;
    CHECK(Sequence(triple, {1, 0, 1, 0, 1, 0}, {O, O, O, O, O, T}));
    // 너무 오래 누르면 탭이 아니다(임계 0.15 — 경계를 피한다).
    InputTrigger longPress;
    longPress.type = InputTriggerType::RepeatedTap;
    longPress.tapReleaseTime = 0.15f;
    CHECK(Sequence(longPress, {1, 1, 1, 0}, {O, N, N, N}));
    // Triggered 직후 간격 안의 탭 하나가 다시 Triggered다(UE 동작 그대로 — 마감이 남는다).
    InputTrigger again;
    again.type = InputTriggerType::RepeatedTap;
    CHECK(Sequence(again, {1, 0, 1, 0, 1, 0}, {O, O, O, T, O, T}));

    InputTrigger pulse;
    pulse.type = InputTriggerType::Pulse;
    pulse.pulseInterval = 0.25f;
    pulse.triggerLimit = 2;
    // 시작 발사(0.1 > 0), 0.3 > 0.25, 한도 2 뒤 None.
    CHECK(Sequence(pulse, {1, 1, 1, 1, 1, 1}, {T, O, T, N, N, N}));
    pulse.triggerOnStart = false;
    pulse.triggerLimit = 0;
    CHECK(Sequence(pulse, {1, 1, 1, 1, 1, 1, 0}, {O, O, T, O, O, T, N}));
}

// 시간 트리거의 dilation: affectedByTimeDilation이면 dt × dilation.
TEST(InputTrigger_TimeDilation) {
    InputTrigger hold;
    hold.type = InputTriggerType::Hold;
    hold.holdTime = 0.25f;
    // dilation 0.5: 꺼져 있으면 영향 없음, 켜면 0.05씩 — 다섯째에 0.25.
    CHECK(Sequence(hold, {1, 1, 1}, {O, O, T}, 0.5));
    hold.affectedByTimeDilation = true;
    CHECK(Sequence(hold, {1, 1, 1, 1, 1}, {O, O, O, O, T}, 0.5));
}

// 결합 규칙(UE FTriggerStateTracker): 트리거 없음, Explicit 하나라도, Implicit 모두, Blocker.
TEST(InputTrigger_CombineRules) {
    const InputValue on(InputValueType::Axis1D, Vec3d(1, 0, 0));
    const TriggerContext context{TriggerStep, 1.0};
    // 트리거가 없으면 정해 둔 no-trigger 상태다.
    TriggerCombiner empty;
    CHECK(empty.State() == N && !empty.EvaluatedTriggers());
    empty.SetStateForNoTriggers(T);
    CHECK(empty.State() == T);

    // Explicit 둘: 하나라도 Triggered면 Triggered, 아니면 진행 중인 것이 있으면 Ongoing.
    InputTrigger pressed;
    pressed.type = InputTriggerType::Pressed;
    InputTrigger hold;
    hold.type = InputTriggerType::Hold;
    const InputTrigger pair[] = {pressed, hold};
    InputTriggerState pairState[2];
    TriggerCombiner first;
    CHECK(first.Evaluate(pair, pairState, on, context) == T);
    TriggerCombiner second;
    CHECK(second.Evaluate(pair, pairState, on, context) == O);

    // Implicit(ChordAction)은 모두 Triggered여야 한다 — 대상이 Triggered가 아니면 막는다.
    InputTrigger chord;
    chord.type = InputTriggerType::ChordAction;
    InputTrigger down;
    const InputTrigger chorded[] = {chord, down};
    InputTriggerState chordedState[2];
    const TriggerState targetTriggered[] = {T};
    const TriggerState targetOngoing[] = {O};
    const TriggerState targetNone[] = {N};
    TriggerCombiner withChord;
    CHECK(withChord.Evaluate(chorded, chordedState, on, ChordContext(targetTriggered)) == T);
    TriggerCombiner withoutChord;
    CHECK(withoutChord.Evaluate(chorded, chordedState, on, ChordContext(targetOngoing)) == N);

    // Blocker: 대상이 Triggered면 막는다. Blocker만 있으면 active를 세우지 않는다.
    InputTrigger blocker;
    blocker.type = InputTriggerType::ChordBlocker;
    const InputTrigger blocked[] = {down, blocker};
    InputTriggerState blockedState[2];
    TriggerCombiner blockedNow;
    CHECK(blockedNow.Evaluate(blocked, blockedState, on, ChordContext(targetTriggered)) == N);
    TriggerCombiner open;
    CHECK(open.Evaluate(blocked, blockedState, on, ChordContext(targetNone)) == T);
    InputTriggerState aloneState;
    TriggerCombiner alone;
    CHECK(alone.Evaluate(Span<const InputTrigger>(&blocker, 1),
                         Span<InputTriggerState>(&aloneState, 1), on,
                         ChordContext(targetNone)) == T);
    // (Explicit 없음·Implicit 없음 → 모든 조건 참이라 Triggered — UE와 같다.)

    // 결합기를 이어서 평가할 수 있다(바인딩 → 액션 트리거).
    TriggerCombiner chained;
    InputTriggerState chainedA;
    InputTriggerState chainedB;
    chained.Evaluate(Span<const InputTrigger>(&hold, 1), Span<InputTriggerState>(&chainedA, 1), on,
                     context);
    CHECK(chained.State() == O);
    chained.Evaluate(Span<const InputTrigger>(&down, 1), Span<InputTriggerState>(&chainedB, 1), on,
                     context);
    CHECK(chained.State() == T);

    // 진전 비교: (상태, 평가 여부).
    CHECK(withoutChord < first && !(first < withoutChord));
    TriggerCombiner noneEvaluated;
    CHECK(noneEvaluated < withoutChord);

    // KindOf·IsTriggerBlocking.
    CHECK(KindOf(InputTriggerType::ChordAction) == TriggerKind::Implicit);
    CHECK(KindOf(InputTriggerType::ChordBlocker) == TriggerKind::Blocker);
    CHECK(KindOf(InputTriggerType::Tap) == TriggerKind::Explicit);
    CHECK(IsTriggerBlocking(InputTriggerType::ChordAction, O));
    CHECK(!IsTriggerBlocking(InputTriggerType::ChordBlocker, O));
    CHECK(!IsTriggerBlocking(InputTriggerType::Down, N));
}

// chord 대상이 여럿: 트리거마다 자기 대상(chordAction 번호)의 상태를 읽는다(UE
// FindActionInstanceData(ChordAction)). 대상이 표 밖이면 None이다.
TEST(InputTrigger_ChordTargetsPerTrigger) {
    const InputValue on(InputValueType::Axis1D, Vec3d(1, 0, 0));
    InputTrigger ctrl;
    ctrl.type = InputTriggerType::ChordAction;
    ctrl.chordAction = 0;
    InputTrigger shift = ctrl;
    shift.chordAction = 1;
    InputTrigger down;
    const InputTrigger both[] = {ctrl, shift, down};
    InputTriggerState bothState[3];
    const TriggerState allTriggered[] = {T, T};
    const TriggerState secondOngoing[] = {T, O};
    const TriggerState firstNone[] = {N, T};
    TriggerCombiner all;
    CHECK(all.Evaluate(both, bothState, on, ChordContext(allTriggered)) == T);
    TriggerCombiner partial;
    CHECK(partial.Evaluate(both, bothState, on, ChordContext(secondOngoing)) == N);
    TriggerCombiner first;
    CHECK(first.Evaluate(both, bothState, on, ChordContext(firstNone)) == N);

    // 대상 번호가 표 밖이면 None이라 막는다.
    InputTrigger missing = ctrl;
    missing.chordAction = 5;
    const InputTrigger outside[] = {missing, down};
    InputTriggerState outsideState[2];
    TriggerCombiner outsideCombiner;
    CHECK(outsideCombiner.Evaluate(outside, outsideState, on, ChordContext(allTriggered)) == N);
    TriggerCombiner empty;
    CHECK(empty.Evaluate(outside, outsideState, on, TriggerContext{TriggerStep, 1.0}) == N);

    // Blocker 여럿: 대상 중 하나라도 Triggered면 막는다.
    InputTrigger blockCtrl;
    blockCtrl.type = InputTriggerType::ChordBlocker;
    blockCtrl.chordAction = 0;
    InputTrigger blockShift = blockCtrl;
    blockShift.chordAction = 1;
    const InputTrigger blockers[] = {down, blockCtrl, blockShift};
    InputTriggerState blockerState[3];
    const TriggerState noneTriggered[] = {N, O};
    const TriggerState secondTriggered[] = {N, T};
    TriggerCombiner unblocked;
    CHECK(unblocked.Evaluate(blockers, blockerState, on, ChordContext(noneTriggered)) == T);
    TriggerCombiner held;
    CHECK(held.Evaluate(blockers, blockerState, on, ChordContext(secondTriggered)) == N);
}

// 전이표 9칸(UE EnhancedPlayerInput.cpp:120-176)과 비트 값.
TEST(InputTrigger_TransitionEvents) {
    CHECK(static_cast<uint8>(ActionEvent::Triggered) == 1 &&
          static_cast<uint8>(ActionEvent::Started) == 2);
    CHECK(static_cast<uint8>(ActionEvent::Ongoing) == 4 &&
          static_cast<uint8>(ActionEvent::Canceled) == 8 &&
          static_cast<uint8>(ActionEvent::Completed) == 16);
    CHECK(TransitionEvent(N, N) == ActionEvent::None);
    CHECK(TransitionEvent(N, O) == ActionEvent::Started);
    CHECK(TransitionEvent(N, T) == (ActionEvent::Started | ActionEvent::Triggered));
    CHECK(TransitionEvent(O, N) == ActionEvent::Canceled);
    CHECK(TransitionEvent(O, O) == ActionEvent::Ongoing);
    CHECK(TransitionEvent(O, T) == ActionEvent::Triggered);
    CHECK(TransitionEvent(T, T) == ActionEvent::Triggered);
    CHECK(TransitionEvent(T, O) == ActionEvent::Ongoing);
    CHECK(TransitionEvent(T, N) == ActionEvent::Completed);
    CHECK(HasAnyEvent(TransitionEvent(N, T), ActionEvent::Started));
    CHECK(!HasAnyEvent(TransitionEvent(O, O), ActionEvent::Triggered));
}

} // namespace input_trigger_tests_file
