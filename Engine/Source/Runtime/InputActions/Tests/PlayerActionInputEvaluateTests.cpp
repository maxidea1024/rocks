#include <Base/InitializerList.h>
#include <Core/Memory/SystemAllocator.h>
#include <Input/InputEvent.h>
#include <Input/InputState.h>
#include <InputActions/PlayerActionInput.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <math.h>
#include <stdio.h>

// 평가기 평가 한 번 시험이다(Docs/Design/InputActions 4.3–4.6절 — IA2c): 키 이벤트·None 건너뛰기,
// alwaysTick, 누적 두 방식, 가장 진전된 바인딩, 액션 마무리(min 제한·무트리거 재판정·pause), 전이와
// 시간, 전달 뒤 정리, 주입(한 번·연속·놓기), flush, 놓을 때까지 무시, 사라진 액션, chord, 상태
// 보존, 터치.
using namespace rocks;
using namespace rocks::input;
namespace player_action_input_evaluate_tests_file {

namespace {

constexpr uint64 IdHigh = 0x50414945;
constexpr uint64 Jump = 1;
constexpr uint64 Fire = 2;
constexpr uint64 Ctrl = 3;
constexpr uint64 Save = 4;
constexpr uint64 Walk = 5;
constexpr uint64 Dash = 6;
constexpr double Step = 0.1;
constexpr double Tolerance = 1.0e-9;

Guid Id(uint64 low) noexcept {
    return Guid{IdHigh, low};
}

struct Builder {
    explicit Builder(Allocator& allocator, uint64 context) noexcept
        : allocator(allocator), bundle(allocator) {
        bundle.id = Id(context);
    }

    InputActionData& Action(uint64 id, InputValueType type = InputValueType::Boolean) noexcept {
        InputActionData action(allocator);
        action.id = Id(id);
        action.settings.valueType = type;
        ok = ok && bundle.actions.TryPushBack(rocks::Move(action)).IsOk();
        return bundle.actions[bundle.actions.Size() - 1];
    }

    InputBinding& Bind(uint64 action, const char* key) noexcept {
        InputBinding binding(allocator);
        binding.action = Id(action);
        ok = ok && binding.key.TryAssign(key).IsOk();
        ok = ok && bundle.data.bindings.TryPushBack(rocks::Move(binding)).IsOk();
        return bundle.data.bindings[bundle.data.bindings.Size() - 1];
    }

    Allocator& allocator;
    InputContextBundle bundle;
    bool ok = true;
};

InputTrigger Typed(InputTriggerType type) noexcept {
    InputTrigger trigger;
    trigger.type = type;
    return trigger;
}

InputTrigger HoldFor(float seconds) noexcept {
    InputTrigger trigger = Typed(InputTriggerType::Hold);
    trigger.holdTime = seconds;
    return trigger;
}

InputTrigger Chord(uint64 target) noexcept {
    InputTrigger trigger = Typed(InputTriggerType::ChordAction);
    trigger.chordActionId = Id(target);
    return trigger;
}

constexpr ActionEvent None = ActionEvent::None;
constexpr ActionEvent Started = ActionEvent::Started;
constexpr ActionEvent Triggered = ActionEvent::Triggered;
constexpr ActionEvent Ongoing = ActionEvent::Ongoing;
constexpr ActionEvent Canceled = ActionEvent::Canceled;
constexpr ActionEvent Completed = ActionEvent::Completed;
const ActionEvent StartedTriggered = ActionEvent::Started | ActionEvent::Triggered;

// 한 평가를 돌린다(입력 이벤트를 먼저 넣는다).
struct Player {
    explicit Player(Allocator& allocator) noexcept : input(allocator) {
    }

    template <usize Count>
    bool Frame(const InputEvent (&events)[Count]) noexcept {
        state.BeginFrame(Span<const InputEvent>(events, Count));
        return input.Evaluate(state, context).IsOk();
    }

    bool Frame() noexcept {
        state.BeginFrame({});
        return input.Evaluate(state, context).IsOk();
    }

    ActionEvent Events(uint64 action) const noexcept {
        return input.ActionState(Id(action)).events;
    }

    bool Is(uint64 action, ActionEvent expected) const noexcept {
        const ActionEvent got = Events(action);
        if (got != expected) {
            printf("  action %u: events %u, expected %u\n", static_cast<unsigned>(action),
                   static_cast<unsigned>(got), static_cast<unsigned>(expected));
        }

        return got == expected;
    }

    InputState state;
    PlayerActionInput input;
    ActionEvalContext context{Step, 1.0, false, 0.0f, false, 0};
};

} // namespace

// 4.3절 2·4·6: 누름 → Started|Triggered, 유지 → Triggered, 놓음 → Completed, 그 뒤 이벤트 없음.
// 시간은 상태가 None이 아닌 동안·Triggered 동안 dt씩, 끝나면 전달 뒤 정리에서 0이다.
TEST(PlayerActionEvaluate_PressHoldRelease) {
    SystemAllocator allocator;
    Player player(allocator);
    Builder builder(allocator, 10);
    builder.Action(Jump);
    builder.Bind(Jump, "SpaceBar");
    REQUIRE(builder.ok);
    REQUIRE(player.input.AddContext(rocks::Move(builder.bundle), 0));
    REQUIRE(player.Frame());
    CHECK(player.Is(Jump, None) && player.input.Events().IsEmpty());
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::Space)}));
    CHECK(player.Is(Jump, StartedTriggered));
    ActionStateView view = player.input.ActionState(Id(Jump));
    CHECK(view.found && view.state == TriggerState::Triggered && view.value.value.x == 1.0);
    CHECK(fabs(view.elapsedSeconds - Step) < Tolerance &&
          fabs(view.triggeredSeconds - Step) < Tolerance);
    REQUIRE(player.input.Events().Size() == 1);
    CHECK(player.input.Events()[0].action == Id(Jump) &&
          player.input.Events()[0].events == StartedTriggered);
    REQUIRE(player.Frame());
    CHECK(player.Is(Jump, Triggered));
    view = player.input.ActionState(Id(Jump));
    CHECK(fabs(view.elapsedSeconds - 2 * Step) < Tolerance);
    REQUIRE(player.Frame({InputEvent::KeyUp(Key::Space)}));
    CHECK(player.Is(Jump, Completed));
    // 정리 전에는 시간이 남고(전달하는 쪽이 읽는다), 정리 뒤 0이다.
    CHECK(player.input.ActionState(Id(Jump)).elapsedSeconds > 0.0);
    player.input.FinishDispatch();
    CHECK(player.input.ActionState(Id(Jump)).elapsedSeconds == 0.0 &&
          player.input.ActionState(Id(Jump)).triggeredSeconds == 0.0);
    REQUIRE(player.Frame());
    CHECK(player.Is(Jump, None) && player.input.Events().IsEmpty());
    // 한 평가에 눌렀다 뗀 키는 값 1로 한 번 Triggered다.
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::Space), InputEvent::KeyUp(Key::Space)}));
    CHECK(player.Is(Jump, StartedTriggered));
    REQUIRE(player.Frame());
    CHECK(player.Is(Jump, Completed));
}

// 4.3절 2: None 키 이벤트는 평가하지 않는다(트리거 시간도 안 흐른다). alwaysTick(RepeatedTap)은
// None이어도 평가한다 — 탭 사이 Ongoing이 이어진다. Tap은 이어지지 않는다(이벤트 없는 액션 →
// Canceled).
TEST(PlayerActionEvaluate_NoneKeyEventsAndAlwaysTick) {
    SystemAllocator allocator;
    Player player(allocator);
    Builder builder(allocator, 20);
    builder.Action(Jump);
    builder.Action(Fire);
    (void)builder.Bind(Jump, "E").triggers.TryPushBack(Typed(InputTriggerType::RepeatedTap));
    (void)builder.Bind(Fire, "Q").triggers.TryPushBack(Typed(InputTriggerType::Tap));
    REQUIRE(builder.ok);
    REQUIRE(player.input.AddContext(rocks::Move(builder.bundle), 0));
    // 다시 만들기를 먼저 끝낸다(같은 평가에 누른 키의 새 Boolean 바인딩은 놓을 때까지 무시 — 4.2절
    // 6).
    REQUIRE(player.Frame());
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::E), InputEvent::KeyDown(Key::Q)}));
    CHECK(player.Is(Jump, Started) && player.Is(Fire, Started));
    REQUIRE(player.Frame({InputEvent::KeyUp(Key::E), InputEvent::KeyUp(Key::Q)}));
    // 놓는 평가(Actuated): RepeatedTap은 탭 사이 Ongoing, Tap은 Triggered.
    CHECK(player.Is(Jump, Ongoing) && player.Is(Fire, Triggered));
    REQUIRE(player.Frame());
    // 키 이벤트 None: RepeatedTap은 alwaysTick이라 Ongoing이 이어지고, Tap은 평가하지 않아 끝난다.
    CHECK(player.Is(Jump, Ongoing) && player.Is(Fire, Completed));
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::E)}));
    REQUIRE(player.Frame({InputEvent::KeyUp(Key::E)}));
    CHECK(player.Is(Jump, Triggered));
}

// 4.3절 2-3: 누적 두 방식. TakeHighestAbsoluteValue는 같으면 나중 값, Cumulative는 합(0이어도
// 바인딩 결합이 Triggered면 Triggered — UE와 같다).
TEST(PlayerActionEvaluate_Accumulation) {
    SystemAllocator allocator;
    for (const ActionAccumulation mode :
         {ActionAccumulation::TakeHighestAbsoluteValue, ActionAccumulation::Cumulative}) {
        Player player(allocator);
        Builder builder(allocator, 30);
        builder.Action(Walk, InputValueType::Axis1D).settings.accumulation = mode;
        builder.Bind(Walk, "D");
        InputModifier negate(allocator);
        negate.kind = InputModifierKind::Negate;
        (void)builder.Bind(Walk, "A").modifiers.TryPushBack(rocks::Move(negate));
        REQUIRE(builder.ok);
        REQUIRE(player.input.AddContext(rocks::Move(builder.bundle), 0));
        // 다시 만들기를 먼저 끝낸다(같은 평가에 누른 키의 새 Boolean 바인딩은 놓을 때까지 무시
        // — 4.2절 6).
        REQUIRE(player.Frame());
        REQUIRE(player.Frame({InputEvent::KeyDown(Key::D)}));
        CHECK(player.input.ActionState(Id(Walk)).value.value.x == 1.0);
        REQUIRE(player.Frame({InputEvent::KeyDown(Key::A)}));
        const ActionStateView both = player.input.ActionState(Id(Walk));
        if (mode == ActionAccumulation::TakeHighestAbsoluteValue) {
            CHECK(both.value.value.x == -1.0);
        } else {
            CHECK(both.value.value.x == 0.0);
        }

        CHECK(both.state == TriggerState::Triggered);
    }
}

// 4.3절 2-4·4: 가장 진전된 바인딩을 남기고, 액션 트리거는 바인딩 결합에 이어서 평가하며 바인딩
// 트리거가 있으면 min(액션, 바인딩)으로 제한한다.
TEST(PlayerActionEvaluate_CombineBindingsAndActionTriggers) {
    SystemAllocator allocator;
    Player player(allocator);
    Builder builder(allocator, 40);
    builder.Action(Jump);
    (void)builder.Bind(Jump, "E").triggers.TryPushBack(HoldFor(0.25f));
    builder.Bind(Jump, "Q");
    // Fire: 바인딩 Pressed + 액션 Down → 둘째 평가는 min(Triggered, None) = None.
    REQUIRE(builder.Action(Fire).settings.triggers.TryPushBack(Typed(InputTriggerType::Down)));
    (void)builder.Bind(Fire, "F").triggers.TryPushBack(Typed(InputTriggerType::Pressed));
    REQUIRE(builder.ok);
    REQUIRE(player.input.AddContext(rocks::Move(builder.bundle), 0));
    // 다시 만들기를 먼저 끝낸다(같은 평가에 누른 키의 새 Boolean 바인딩은 놓을 때까지 무시 — 4.2절
    // 6).
    REQUIRE(player.Frame());
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::E), InputEvent::KeyDown(Key::F)}));
    CHECK(player.Is(Jump, Started) && player.Is(Fire, StartedTriggered));
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::Q)}));
    // Q(트리거 없음 → Triggered)가 E(Hold 진행 중 Ongoing)보다 진전되었다.
    CHECK(player.Is(Jump, Triggered) && player.Is(Fire, Completed));
    REQUIRE(player.Frame({InputEvent::KeyUp(Key::Q)}));
    // Q를 놓아도 E의 Hold가 0.3 ≥ 0.25로 Triggered다.
    CHECK(player.Is(Jump, Triggered));
}

// 4.3절 4-2·4-5: 트리거 없는 액션은 액션 모디파이어가 값을 바꾸면 nonzero로 다시 정한다(Scalar 0 →
// 이벤트 없음). 일시정지 중에는 triggerWhenPaused가 아니면 None이다.
TEST(PlayerActionEvaluate_ActionModifiersAndPause) {
    SystemAllocator allocator;
    Player player(allocator);
    Builder builder(allocator, 50);
    InputModifier zero(allocator);
    zero.kind = InputModifierKind::Scalar;
    zero.scalar = Vec3d();
    REQUIRE(builder.Action(Walk, InputValueType::Axis1D)
                .settings.modifiers.TryPushBack(rocks::Move(zero)));
    builder.Bind(Walk, "W");
    builder.Action(Dash, InputValueType::Axis1D);
    builder.Bind(Dash, "W");
    builder.Action(Jump);
    builder.Bind(Jump, "SpaceBar");
    builder.Action(Fire).settings.triggerWhenPaused = true;
    builder.Bind(Fire, "E");
    REQUIRE(builder.ok);
    REQUIRE(player.input.AddContext(rocks::Move(builder.bundle), 0));
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::W)}));
    // 대조: 같은 키의 모디파이어 없는 Dash는 Triggered다(W 바인딩은 평가되었다).
    CHECK(player.Is(Dash, StartedTriggered));
    CHECK(player.Is(Walk, None) && player.input.ActionState(Id(Walk)).value.value.x == 0.0);
    player.context.paused = true;
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::Space), InputEvent::KeyDown(Key::E)}));
    CHECK(player.Is(Jump, None) && player.Is(Fire, StartedTriggered));
    player.context.paused = false;
    REQUIRE(player.Frame());
    CHECK(player.Is(Jump, StartedTriggered) && player.Is(Fire, Triggered));
}

// 4.3절 3: 한 번 주입(다음 평가에 놓기 → Completed, 주입 전용 항목은 정리에서 지움), 연속
// 주입(평가마다, 값 바꾸기, 멈추면 놓기), 주입 트리거 상태는 (액션, 종류 열)마다 이어진다. 검증
// 실패는 그대로다.
TEST(PlayerActionEvaluate_Injection) {
    SystemAllocator allocator;
    Player player(allocator);
    InputActionData dash(allocator);
    dash.id = Id(Dash);
    REQUIRE(player.input.InjectValue(dash, InputValue::FromBool(true)));
    REQUIRE(player.Frame());
    CHECK(player.Is(Dash, StartedTriggered));
    REQUIRE(player.Frame());
    CHECK(player.Is(Dash, Completed));
    player.input.FinishDispatch();
    CHECK(!player.input.ActionState(Id(Dash)).found);

    // 연속 주입: Hold(0.25) 트리거 상태가 평가 사이에 이어진다.
    InputActionData walk(allocator);
    walk.id = Id(Walk);
    walk.settings.valueType = InputValueType::Axis1D;
    const InputTrigger hold[] = {HoldFor(0.25f)};
    REQUIRE(player.input.StartContinuousInjection(
        walk, InputValue(InputValueType::Axis1D, Vec3d(0.5, 0, 0)), hold));
    REQUIRE(player.Frame());
    CHECK(player.Is(Walk, Started));
    REQUIRE(player.Frame());
    CHECK(player.Is(Walk, Ongoing));
    REQUIRE(player.Frame());
    CHECK(player.Is(Walk, Triggered));
    CHECK(player.input.ActionState(Id(Walk)).value.value.x == 0.5);
    REQUIRE(player.input.UpdateContinuousInjection(
        Id(Walk), InputValue(InputValueType::Axis1D, Vec3d(-1, 0, 0))));
    REQUIRE(player.Frame());
    CHECK(player.input.ActionState(Id(Walk)).value.value.x == -1.0);
    REQUIRE(player.input.StopContinuousInjection(Id(Walk)));
    REQUIRE(player.Frame());
    CHECK(player.Is(Walk, Completed));
    CHECK(player.input.StopContinuousInjection(Id(Walk)).Error() == ErrorCode::NotFound);
    CHECK(player.input.UpdateContinuousInjection(Id(Walk), InputValue()).Error() ==
          ErrorCode::NotFound);

    // 한 평가에 두 번 주입하면 각각 처리한다(TakeHighest: 큰 쪽).
    REQUIRE(player.input.InjectValue(walk, InputValue(InputValueType::Axis1D, Vec3d(0.2, 0, 0))));
    REQUIRE(player.input.InjectValue(walk, InputValue(InputValueType::Axis1D, Vec3d(-0.7, 0, 0))));
    REQUIRE(player.Frame());
    CHECK(player.input.ActionState(Id(Walk)).value.value.x == -0.7);

    // 검증 실패: 0 id, 비유한 트리거.
    InputActionData broken(allocator);
    CHECK(player.input.InjectValue(broken, InputValue()).Error() == ErrorCode::InvalidArgument);
    InputTrigger notFinite = HoldFor(0.25f);
    notFinite.holdTime = static_cast<float>(::nan(""));
    const InputTrigger bad[] = {notFinite};
    CHECK(player.input.InjectValue(dash, InputValue(), bad).Error() == ErrorCode::InvalidArgument);

    // 할당 실패는 그대로다.
    test::NFailAllocator failing(allocator);
    PlayerActionInput fresh(failing);
    failing.FailOnAttempt(1);
    CHECK(fresh.InjectValue(dash, InputValue::FromBool(true)).Error() == ErrorCode::OutOfMemory);
    failing.DisableFailure();
    CHECK(!fresh.ActionState(Id(Dash)).found);
}

// 4.3절 Flush: 마지막 평가에서 눌려 있던 디지털 바인딩은 놓을 때까지 무시한다 — 진행 중 액션은 다음
// 평가에 끝나고, 놓은 뒤 다시 누르면 시작한다. 축 바인딩은 무시하지 않는다.
TEST(PlayerActionEvaluate_Flush) {
    SystemAllocator allocator;
    Player player(allocator);
    Builder builder(allocator, 60);
    builder.Action(Jump);
    builder.Bind(Jump, "SpaceBar");
    builder.Action(Walk, InputValueType::Axis1D);
    builder.Bind(Walk, "MouseX");
    REQUIRE(builder.ok);
    REQUIRE(player.input.AddContext(rocks::Move(builder.bundle), 0));
    REQUIRE(player.Frame({InputEvent::MouseMove(10.0f, 10.0f)}));
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::Space), InputEvent::MouseMove(20.0f, 10.0f)}));
    CHECK(player.Is(Jump, StartedTriggered) && player.Is(Walk, StartedTriggered));
    player.input.Flush();
    REQUIRE(player.Frame({InputEvent::MouseMove(30.0f, 10.0f)}));
    CHECK(player.Is(Jump, Completed) && player.Is(Walk, Triggered));
    REQUIRE(player.Frame());
    CHECK(player.Is(Jump, None));
    REQUIRE(player.Frame({InputEvent::KeyUp(Key::Space)}));
    CHECK(player.Is(Jump, None));
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::Space)}));
    CHECK(player.Is(Jump, StartedTriggered));
}

// 4.2절 6·4.1절: 다시 만들 때 눌려 있던 키의 새 Boolean 바인딩은 놓을 때까지 무시하고(놓으면 그
// 평가부터 평가), 즉시 다시 만들기는 마지막 평가의 키 상태로 판정한다.
TEST(PlayerActionEvaluate_IgnoreUntilRelease) {
    SystemAllocator allocator;
    Player player(allocator);
    Builder first(allocator, 70);
    first.Action(Fire);
    first.Bind(Fire, "E");
    REQUIRE(first.ok);
    REQUIRE(player.input.AddContext(rocks::Move(first.bundle), 0));
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::Space)}));
    // 즉시 다시 만들기: 마지막 평가에서 SpaceBar가 눌려 있었다.
    Builder second(allocator, 71);
    second.Action(Jump);
    second.Bind(Jump, "SpaceBar");
    REQUIRE(second.ok);
    ContextOptions now;
    now.forceImmediately = true;
    REQUIRE(player.input.AddContext(rocks::Move(second.bundle), 1, now));
    REQUIRE(player.input.BindingCount() == 2);
    CHECK(player.input.Binding(0).action == Id(Jump) && player.input.Binding(0).ignored);
    REQUIRE(player.Frame());
    CHECK(player.Is(Jump, None));
    REQUIRE(player.Frame({InputEvent::KeyUp(Key::Space)}));
    CHECK(player.Is(Jump, None) && !player.input.Binding(0).ignored);
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::Space)}));
    CHECK(player.Is(Jump, StartedTriggered));
}

// 4.2절 7: 진행 중이던 액션의 바인딩이 사라지면 다음 평가에 끝나고(Completed·Canceled) 정리에서
// 지운다. 평가 전에 다시 만들기가 두 번 와도 남는다.
TEST(PlayerActionEvaluate_UnmappedActionsFinish) {
    SystemAllocator allocator;
    Player player(allocator);
    Builder builder(allocator, 80);
    builder.Action(Jump);
    builder.Bind(Jump, "SpaceBar");
    builder.Action(Fire);
    (void)builder.Bind(Fire, "E").triggers.TryPushBack(HoldFor(1.0f));
    REQUIRE(builder.ok);
    REQUIRE(player.input.AddContext(rocks::Move(builder.bundle), 0));
    // 다시 만들기를 먼저 끝낸다(같은 평가에 누른 키의 새 Boolean 바인딩은 놓을 때까지 무시 — 4.2절
    // 6).
    REQUIRE(player.Frame());
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::Space), InputEvent::KeyDown(Key::E)}));
    CHECK(player.Is(Jump, StartedTriggered) && player.Is(Fire, Started));
    ContextOptions now;
    now.forceImmediately = true;
    REQUIRE(player.input.RemoveContext(Id(80), now));
    Builder other(allocator, 81);
    other.Action(Walk);
    other.Bind(Walk, "W");
    REQUIRE(other.ok);
    REQUIRE(player.input.AddContext(rocks::Move(other.bundle), 0, now));
    CHECK(player.input.ActionState(Id(Jump)).found &&
          player.input.ActionState(Id(Jump)).value.value.x == 0.0);
    REQUIRE(player.Frame());
    CHECK(player.Is(Jump, Completed) && player.Is(Fire, Canceled));
    player.input.FinishDispatch();
    CHECK(!player.input.ActionState(Id(Jump)).found && !player.input.ActionState(Id(Fire)).found);
}

// 4.3절 2·4.4절: chord — Ctrl을 누른 채 S면 Save만 Triggered(S의 Walk는 ChordBlocker로 막힘),
// S만이면 Walk. chord 짝을 모은다.
TEST(PlayerActionEvaluate_Chords) {
    SystemAllocator allocator;
    Player player(allocator);
    Builder builder(allocator, 90);
    builder.Action(Walk);
    builder.Action(Ctrl);
    builder.Action(Save);
    builder.Bind(Walk, "S");
    builder.Bind(Ctrl, "LeftControl");
    (void)builder.Bind(Save, "S").triggers.TryPushBack(Chord(Ctrl));
    REQUIRE(builder.ok);
    REQUIRE(player.input.AddContext(rocks::Move(builder.bundle), 0));
    // 다시 만들기를 먼저 끝낸다(같은 평가에 누른 키의 새 Boolean 바인딩은 놓을 때까지 무시 — 4.2절
    // 6).
    REQUIRE(player.Frame());
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::S)}));
    CHECK(player.Is(Walk, StartedTriggered) && player.Is(Save, None));
    REQUIRE(player.Frame({InputEvent::KeyUp(Key::S)}));
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::LeftControl)}));
    CHECK(player.Is(Ctrl, StartedTriggered));
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::S)}));
    CHECK(player.Is(Save, StartedTriggered) && player.Is(Walk, None));
    REQUIRE(player.input.ChordDependencies().Size() == 1);
    CHECK(player.input.ChordDependencies()[0].source == Id(Save) &&
          player.input.ChordDependencies()[0].target == Id(Ctrl));
}

// 4.2절 6: 다시 만들어도 짝지은 바인딩의 트리거 상태(Hold 누름 시간)가 이어진다.
TEST(PlayerActionEvaluate_StateSurvivesRebuild) {
    SystemAllocator allocator;
    Player player(allocator);
    Builder builder(allocator, 100);
    builder.Action(Jump);
    (void)builder.Bind(Jump, "E").triggers.TryPushBack(HoldFor(0.45f));
    REQUIRE(builder.ok);
    REQUIRE(player.input.AddContext(rocks::Move(builder.bundle), 0));
    // 다시 만들기를 먼저 끝낸다(같은 평가에 누른 키의 새 Boolean 바인딩은 놓을 때까지 무시 — 4.2절
    // 6).
    REQUIRE(player.Frame());
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::E)}));
    REQUIRE(player.Frame());
    REQUIRE(player.Frame());
    CHECK(player.Is(Jump, Ongoing));
    // 다시 만들기(프로필 변경) — 누름 0.3초가 이어져 두 평가 뒤(0.5 ≥ 0.45) Triggered다.
    REQUIRE(player.input.SetKeyProfile("Other"));
    REQUIRE(player.Frame());
    CHECK(player.Is(Jump, Ongoing));
    REQUIRE(player.Frame());
    CHECK(player.Is(Jump, Triggered));
}

// 4.3절 2: 터치 키 — Boolean 액션은 닿음 1·0(x = 0 열이어도), 축 액션은 닿아 있지 않으면 0.
TEST(PlayerActionEvaluate_TouchKeys) {
    SystemAllocator allocator;
    Player player(allocator);
    Builder builder(allocator, 110);
    builder.Action(Jump);
    builder.Bind(Jump, "Touch1");
    builder.Action(Walk, InputValueType::Axis2D);
    builder.Bind(Walk, "Touch2");
    REQUIRE(builder.ok);
    REQUIRE(player.input.AddContext(rocks::Move(builder.bundle), 0));
    // 다시 만들기를 먼저 끝낸다(같은 평가에 누른 키의 새 Boolean 바인딩은 놓을 때까지 무시 — 4.2절
    // 6).
    REQUIRE(player.Frame());
    REQUIRE(player.Frame(
        {InputEvent::TouchBegan(0, 0.0f, 50.0f), InputEvent::TouchBegan(1, 30.0f, 40.0f)}));
    CHECK(player.Is(Jump, StartedTriggered) && player.Is(Walk, StartedTriggered));
    CHECK(player.input.ActionState(Id(Walk)).value.value.x == 30.0);
    REQUIRE(player.Frame(
        {InputEvent::TouchEnded(0, 0.0f, 50.0f), InputEvent::TouchEnded(1, 31.0f, 41.0f)}));
    CHECK(player.Is(Jump, Completed) && player.Is(Walk, Completed));
    CHECK(player.input.ActionState(Id(Walk)).value.value.x == 0.0);
}

// 4.3절 1: 예약된 다시 만들기가 실패하면 이전 목록으로 평가하고 OutOfMemory를 돌려준다.
TEST(PlayerActionEvaluate_RebuildFailureStillEvaluates) {
    SystemAllocator allocator;
    test::NFailAllocator failing(allocator);
    PlayerActionInput input(failing);
    InputState state;
    Builder builder(allocator, 120);
    builder.Action(Jump);
    builder.Bind(Jump, "SpaceBar");
    REQUIRE(builder.ok);
    REQUIRE(input.AddContext(rocks::Move(builder.bundle), 0));
    const ActionEvalContext context{Step, 1.0, false, 0.0f, false, 0};
    REQUIRE(input.Evaluate(state, context));
    REQUIRE(input.SetKeyProfile("Other"));
    const InputEvent down[] = {InputEvent::KeyDown(Key::Space)};
    state.BeginFrame(Span<const InputEvent>(down, 1));
    failing.FailOnAttempt(1);
    CHECK(input.Evaluate(state, context).Error() == ErrorCode::OutOfMemory);
    failing.DisableFailure();
    CHECK(input.ActionState(Id(Jump)).events == StartedTriggered && input.HasPendingRebuild());
}

// 4.3절 2: 한 평가 탭 — 다음 평가가 놓기로 평가된다(UE KeyDownPrevious). Pressed의 직전 값이 풀려
// 다음 누름이 다시 발사되고, Tap·Released는 탭에 Triggered다.
TEST(PlayerActionEvaluate_SubFrameTapTriggers) {
    SystemAllocator allocator;
    Player player(allocator);
    Builder builder(allocator, 130);
    builder.Action(Jump);
    builder.Action(Fire);
    builder.Action(Save);
    (void)builder.Bind(Jump, "E").triggers.TryPushBack(Typed(InputTriggerType::Pressed));
    (void)builder.Bind(Fire, "Q").triggers.TryPushBack(Typed(InputTriggerType::Tap));
    (void)builder.Bind(Save, "R").triggers.TryPushBack(Typed(InputTriggerType::Released));
    REQUIRE(builder.ok);
    REQUIRE(player.input.AddContext(rocks::Move(builder.bundle), 0));
    REQUIRE(player.Frame());
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::E), InputEvent::KeyUp(Key::E),
                          InputEvent::KeyDown(Key::Q), InputEvent::KeyUp(Key::Q),
                          InputEvent::KeyDown(Key::R), InputEvent::KeyUp(Key::R)}));
    CHECK(player.Is(Jump, StartedTriggered) && player.Is(Fire, Started) &&
          player.Is(Save, Started));
    REQUIRE(player.Frame());
    CHECK(player.Is(Jump, Completed) && player.Is(Fire, Triggered) && player.Is(Save, Triggered));
    REQUIRE(player.Frame());
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::E)}));
    CHECK(player.Is(Jump, StartedTriggered));
}

// 4.3절 2-3: Axis2D 성분마다 TakeHighest(같으면 나중 값).
TEST(PlayerActionEvaluate_Axis2DAccumulation) {
    SystemAllocator allocator;
    Player player(allocator);
    Builder builder(allocator, 140);
    builder.Action(Walk, InputValueType::Axis2D);
    builder.Bind(Walk, "D");
    InputModifier swizzle(allocator);
    swizzle.kind = InputModifierKind::Swizzle;
    (void)builder.Bind(Walk, "W").modifiers.TryPushBack(rocks::Move(swizzle));
    InputModifier negate(allocator);
    negate.kind = InputModifierKind::Negate;
    (void)builder.Bind(Walk, "A").modifiers.TryPushBack(rocks::Move(negate));
    REQUIRE(builder.ok);
    REQUIRE(player.input.AddContext(rocks::Move(builder.bundle), 0));
    REQUIRE(player.Frame());
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::D), InputEvent::KeyDown(Key::W)}));
    CHECK(player.input.ActionState(Id(Walk)).value.value == Vec3d(1, 1, 0));
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::A)}));
    // x: 1(D) 뒤 −1(A, 절댓값 같음 → 나중 값), y: 1(W).
    CHECK(player.input.ActionState(Id(Walk)).value.value == Vec3d(-1, 1, 0));
}

// 4.3절 4-6: Hold를 일찍 놓으면 Started → Canceled. 전달 뒤 정리를 부르지 않아도 다음 평가가 처음에
// 한다.
TEST(PlayerActionEvaluate_CanceledAndImplicitFinish) {
    SystemAllocator allocator;
    Player player(allocator);
    Builder builder(allocator, 150);
    builder.Action(Jump);
    (void)builder.Bind(Jump, "E").triggers.TryPushBack(HoldFor(1.0f));
    REQUIRE(builder.ok);
    REQUIRE(player.input.AddContext(rocks::Move(builder.bundle), 0));
    REQUIRE(player.Frame());
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::E)}));
    CHECK(player.Is(Jump, Started));
    REQUIRE(player.Frame({InputEvent::KeyUp(Key::E)}));
    CHECK(player.Is(Jump, Canceled) && player.input.ActionState(Id(Jump)).elapsedSeconds > 0.0);
    REQUIRE(player.Frame());
    CHECK(player.Is(Jump, None) && player.input.ActionState(Id(Jump)).elapsedSeconds == 0.0);
}

// 4.3절 6·4.6절: 주입이 걸린 사라진 액션은 정리에서 지우지 않고 주입 전용이 된다. 전달 중 즉시 다시
// 만들기는 Events()를 무효로 만들지 않는다.
TEST(PlayerActionEvaluate_InjectionKeepsUnmappedAndEventsStable) {
    SystemAllocator allocator;
    Player player(allocator);
    Builder builder(allocator, 160);
    builder.Action(Jump);
    builder.Bind(Jump, "SpaceBar");
    REQUIRE(builder.ok);
    REQUIRE(player.input.AddContext(rocks::Move(builder.bundle), 0));
    REQUIRE(player.Frame());
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::Space)}));
    REQUIRE(player.input.Events().Size() == 1);
    // 전달 중: 새 액션을 가진 컨텍스트를 즉시 다시 만들기로 넣어도 Events()는 그대로다.
    const ActionEventRecord* before = player.input.Events().Data();
    Builder menu(allocator, 161);
    for (uint64 id = 20; id < 40; ++id) {
        menu.Action(id);
        char key[] = "F1";
        key[1] = static_cast<char>('1' + (id % 9));
        menu.Bind(id, key);
    }

    REQUIRE(menu.ok);
    ContextOptions now;
    now.forceImmediately = true;
    REQUIRE(player.input.AddContext(rocks::Move(menu.bundle), 1, now));
    CHECK(player.input.Events().Data() == before && player.input.Events().Size() == 1);
    REQUIRE(player.input.RemoveContext(Id(161), now));
    // Jump를 빼고(사라진 액션) 연속 주입을 건다 → 정리에서 주입 전용으로 바뀌어 남는다.
    REQUIRE(player.input.RemoveContext(Id(160), now));
    InputActionData jump(allocator);
    jump.id = Id(Jump);
    REQUIRE(player.input.StartContinuousInjection(jump, InputValue::FromBool(true)));
    player.input.FinishDispatch();
    REQUIRE(player.Frame());
    CHECK(player.Is(Jump, Triggered));
    REQUIRE(player.Frame());
    CHECK(player.Is(Jump, Triggered) && player.input.ActionState(Id(Jump)).found);
    REQUIRE(player.input.StopContinuousInjection(Id(Jump)));
    REQUIRE(player.Frame());
    CHECK(player.Is(Jump, Completed));
    player.input.FinishDispatch();
    CHECK(!player.input.ActionState(Id(Jump)).found);
}

// 4.2절 5: Explicit 트리거가 있는 바인딩의 ChordBlocker(Down 없이) — Ctrl을 누른 채면 Pressed가
// 막힌다.
TEST(PlayerActionEvaluate_BlockerWithExplicitTrigger) {
    SystemAllocator allocator;
    Player player(allocator);
    Builder builder(allocator, 170);
    builder.Action(Ctrl);
    builder.Action(Save);
    builder.Action(Walk);
    builder.Bind(Ctrl, "LeftControl");
    (void)builder.Bind(Save, "S").triggers.TryPushBack(Chord(Ctrl));
    (void)builder.Bind(Walk, "S").triggers.TryPushBack(Typed(InputTriggerType::Pressed));
    REQUIRE(builder.ok);
    REQUIRE(player.input.AddContext(rocks::Move(builder.bundle), 0));
    REQUIRE(player.Frame());
    REQUIRE(player.input.BindingCount() == 3 && player.input.Binding(2).triggers.Size() == 2);
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::LeftControl)}));
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::S)}));
    CHECK(player.Is(Save, StartedTriggered) && player.Is(Walk, None));
    REQUIRE(player.Frame({InputEvent::KeyUp(Key::S), InputEvent::KeyUp(Key::LeftControl)}));
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::S)}));
    CHECK(player.Is(Walk, StartedTriggered) && player.Is(Save, None));
}

// 4.3절 Flush: 놓을 때 Released가 늦게 나지 않는다(상태 초기화 — UE 키 상태 0과 같은 결과).
TEST(PlayerActionEvaluate_FlushResetsTriggerState) {
    SystemAllocator allocator;
    Player player(allocator);
    Builder builder(allocator, 180);
    builder.Action(Save);
    (void)builder.Bind(Save, "R").triggers.TryPushBack(Typed(InputTriggerType::Released));
    REQUIRE(builder.ok);
    REQUIRE(player.input.AddContext(rocks::Move(builder.bundle), 0));
    REQUIRE(player.Frame());
    REQUIRE(player.Frame({InputEvent::KeyDown(Key::R)}));
    CHECK(player.Is(Save, Started));
    player.input.Flush();
    REQUIRE(player.Frame());
    CHECK(player.Is(Save, Canceled));
    REQUIRE(player.Frame({InputEvent::KeyUp(Key::R)}));
    CHECK(player.Is(Save, None));
}

// 4.3절 3: 주입 할당 실패는 어느 할당에서 실패해도 상태가 그대로다(시도마다 전부 훑는다).
TEST(PlayerActionEvaluate_InjectionAllocationFailures) {
    SystemAllocator allocator;
    InputActionData walk(allocator);
    walk.id = Id(Walk);
    walk.settings.valueType = InputValueType::Axis1D;
    const InputTrigger hold[] = {HoldFor(0.25f)};
    InputModifier dead(allocator);
    const InputModifier modifiers[] = {rocks::Move(dead)};
    bool succeeded = false;
    uint32 failures = 0;
    for (uint64 attempt = 1; attempt < 64 && !succeeded; ++attempt) {
        test::NFailAllocator failing(allocator);
        PlayerActionInput input(failing);
        failing.FailOnAttempt(attempt);
        auto injected = input.InjectValue(walk, InputValue(InputValueType::Axis1D, Vec3d(1, 0, 0)),
                                          hold, modifiers);
        failing.DisableFailure();
        if (injected) {
            succeeded = input.ActionState(Id(Walk)).found;
        } else {
            ++failures;
            CHECK(injected.Error() == ErrorCode::OutOfMemory && !input.ActionState(Id(Walk)).found);
        }
    }

    CHECK(succeeded && failures > 1);
}

} // namespace player_action_input_evaluate_tests_file
