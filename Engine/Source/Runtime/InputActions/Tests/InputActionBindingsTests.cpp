#include <Base/InitializerList.h>
#include <Core/Memory/SystemAllocator.h>
#include <Input/InputEvent.h>
#include <Input/InputState.h>
#include <InputActions/InputActionBindings.h>
#include <InputActions/PlayerActionInput.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// 액션 바인딩 전달 시험이다(Docs/Design/InputActions 5.1절 — IA3): 실행 순서(Started 앞·등록 역순),
// 콜백이 받는 이벤트, 소비, chord 억제, 콜백 중 추가·제거, 값 바인딩, 검증·할당 실패.
using namespace rocks;
using namespace rocks::input;
namespace input_action_bindings_tests_file {

namespace {

constexpr uint64 IdHigh = 0x49414244;
constexpr uint64 Jump = 1;
constexpr uint64 Fire = 2;
constexpr uint64 Ctrl = 3;
constexpr uint64 Save = 4;
constexpr usize MaxCalls = 32;
constexpr double Step = 0.1;

Guid Id(uint64 low) noexcept {
    return Guid{IdHigh, low};
}

// 콜백 기록: (표지, 받은 이벤트).
struct CallLog {
    char tags[MaxCalls] = {};
    ActionEvent events[MaxCalls] = {};
    usize count = 0;

    void Add(char tag, ActionEvent event) noexcept {
        if (count < MaxCalls) {
            tags[count] = tag;
            events[count] = event;
            ++count;
        }
    }

    bool Is(const char* expected) noexcept {
        const bool same = strlen(expected) == count && memcmp(tags, expected, count) == 0;
        if (!same) {
            printf("  calls '%.*s', expected '%s'\n", static_cast<int>(count), tags, expected);
        }

        count = 0;
        return same;
    }
};

struct Rig {
    explicit Rig(Allocator& allocator) noexcept
        : allocator(allocator), input(allocator), bindings(allocator), bundle(allocator) {
        bundle.id = Id(100);
    }

    void Action(uint64 id) noexcept {
        InputActionData action(allocator);
        action.id = Id(id);
        ok = ok && bundle.actions.TryPushBack(rocks::Move(action)).IsOk();
    }

    InputBinding& Bind(uint64 action, const char* key) noexcept {
        InputBinding binding(allocator);
        binding.action = Id(action);
        ok = ok && binding.key.TryAssign(key).IsOk();
        ok = ok && bundle.data.bindings.TryPushBack(rocks::Move(binding)).IsOk();
        return bundle.data.bindings[bundle.data.bindings.Size() - 1];
    }

    bool Start() noexcept {
        return ok && input.AddContext(rocks::Move(bundle), 0).IsOk() && Frame();
    }

    template <class Lambda>
    ActionBindingHandle On(uint64 action, ActionEvent events, Lambda&& lambda,
                           bool consumes = false) noexcept {
        auto callback = InputActionBindings::Callback::TryCreate(allocator, rocks::Move(lambda));
        if (!callback) {
            ok = false;
            return {};
        }

        auto bound =
            bindings.BindAction(Id(action), events, rocks::Move(callback.Value()), consumes);
        ok = ok && bound.IsOk();
        return bound ? bound.Value() : ActionBindingHandle{};
    }

    ActionBindingHandle Record(uint64 action, ActionEvent events, char tag,
                               bool consumes = false) noexcept {
        CallLog* target = &log;
        return On(
            action, events,
            [target, tag](const ActionEventData& data) noexcept { target->Add(tag, data.events); },
            consumes);
    }

    template <usize Count>
    bool Frame(const InputEvent (&events)[Count]) noexcept {
        state.BeginFrame(Span<const InputEvent>(events, Count));
        return Run();
    }

    bool Frame() noexcept {
        state.BeginFrame({});
        return Run();
    }

    bool Run() noexcept {
        const bool evaluated =
            input.Evaluate(state, ActionEvalContext{Step, 1.0, false, 0.0f, false, 0}).IsOk();
        bindings.Dispatch(input);
        input.FinishDispatch();
        return evaluated;
    }

    Allocator& allocator;
    InputState state;
    PlayerActionInput input;
    InputActionBindings bindings;
    InputContextBundle bundle;
    CallLog log;
    bool ok = true;
};

const ActionEvent StartedTriggered = ActionEvent::Started | ActionEvent::Triggered;

} // namespace

// 5.1절: 실행 목록은 등록 순이고 Started는 앞에 끼워 넣는다(Started끼리 등록 역순). 콜백은 (이벤트
// & 마스크)를 받는다 — 한 평가의 Started·Triggered는 Started 바인딩이 Started, Triggered 바인딩이
// Triggered다. 마스크가 겹치지 않으면 부르지 않는다.
TEST(InputActionBindings_DispatchOrder) {
    SystemAllocator allocator;
    Rig rig(allocator);
    rig.Action(Jump);
    rig.Action(Fire);
    rig.Bind(Jump, "SpaceBar");
    rig.Bind(Fire, "E");
    REQUIRE(rig.Start());
    rig.Record(Jump, ActionEvent::Triggered, '1');
    rig.Record(Jump, ActionEvent::Started, '2');
    rig.Record(Fire, ActionEvent::Triggered, '3');
    rig.Record(Jump, ActionEvent::Started, '4');
    rig.Record(Jump, ActionEvent::Completed, '5');
    rig.Record(Jump, ActionEvent::Started | ActionEvent::Completed, '6');
    REQUIRE(rig.ok);
    REQUIRE(rig.Frame({InputEvent::KeyDown(Key::Space), InputEvent::KeyDown(Key::E)}));
    // Started: 6 → 4 → 2(등록 역순으로 앞에), 그 뒤 1 → 3.
    REQUIRE(rig.log.count == 5);
    CHECK(rig.log.events[0] == ActionEvent::Started && rig.log.events[3] == ActionEvent::Triggered);
    CHECK(rig.log.Is("64213"));
    REQUIRE(rig.Frame());
    CHECK(rig.log.Is("13"));
    REQUIRE(rig.Frame({InputEvent::KeyUp(Key::Space)}));
    CHECK(rig.log.Is("356"));
}

// 5.1절: consumes — 실행 목록에서 그 뒤의 같은 액션 바인딩을 막는다(다른 액션은 아니다). Started
// 바인딩의 소비가 뒤의 Triggered를 막는다.
TEST(InputActionBindings_Consumption) {
    SystemAllocator allocator;
    Rig rig(allocator);
    rig.Action(Jump);
    rig.Action(Fire);
    rig.Bind(Jump, "SpaceBar");
    rig.Bind(Fire, "E");
    REQUIRE(rig.Start());
    rig.Record(Jump, ActionEvent::Triggered, 'a', true);
    rig.Record(Jump, ActionEvent::Triggered, 'b');
    rig.Record(Fire, ActionEvent::Triggered, 'c');
    rig.Record(Fire, ActionEvent::Started, 'd', true);
    REQUIRE(rig.ok);
    REQUIRE(rig.Frame({InputEvent::KeyDown(Key::Space), InputEvent::KeyDown(Key::E)}));
    // d(Started, 소비) → a(소비) → b 막힘 → c 막힘(d가 Fire를 소비).
    CHECK(rig.log.Is("da"));
    REQUIRE(rig.Frame());
    CHECK(rig.log.Is("ac"));
}

// 4.4절: chord 트리거를 가진 액션(Save = Ctrl+S)이 Triggered로 전달되면 그 대상(Ctrl)의 바인딩은
// 막는다. Ctrl만 누르면 Ctrl이 전달된다.
TEST(InputActionBindings_ChordSuppression) {
    SystemAllocator allocator;
    Rig rig(allocator);
    rig.Action(Ctrl);
    rig.Action(Save);
    rig.Bind(Ctrl, "LeftControl");
    InputTrigger chord;
    chord.type = InputTriggerType::ChordAction;
    chord.chordActionId = Id(Ctrl);
    (void)rig.Bind(Save, "S").triggers.TryPushBack(chord);
    REQUIRE(rig.Start());
    rig.Record(Ctrl, ActionEvent::Triggered, 'C');
    rig.Record(Save, ActionEvent::Triggered, 'S');
    REQUIRE(rig.ok);
    REQUIRE(rig.Frame({InputEvent::KeyDown(Key::LeftControl)}));
    CHECK(rig.log.Is("C"));
    REQUIRE(rig.Frame({InputEvent::KeyDown(Key::S)}));
    CHECK(rig.log.Is("S"));
    // S를 놓는 평가도 Save는 Triggered다(Explicit 없는 chord 바인딩 — UE FTriggerStateTracker와
    // 같다).
    REQUIRE(rig.Frame({InputEvent::KeyUp(Key::S)}));
    CHECK(rig.log.Is("S"));
    REQUIRE(rig.Frame());
    CHECK(rig.log.Is("C"));
}

// 5.1절: 콜백 안에서 더한 바인딩은 다음 전달부터, 지운 바인딩은 이번 실행 목록에 있으면 이번에
// 실행하고 다음부터 빠진다. 전달 중 다시 부른 Dispatch는 아무것도 하지 않는다.
TEST(InputActionBindings_ChangesDuringDispatch) {
    SystemAllocator allocator;
    Rig rig(allocator);
    rig.Action(Jump);
    rig.Bind(Jump, "SpaceBar");
    REQUIRE(rig.Start());
    ActionBindingHandle later{};
    bool added = false;
    Rig* self = &rig;
    rig.On(Jump, ActionEvent::Triggered, [self, &later, &added](const ActionEventData&) noexcept {
        self->log.Add('x', ActionEvent::Triggered);
        if (!added) {
            added = true;
            self->Record(Jump, ActionEvent::Triggered, 'n');
            self->bindings.RemoveBinding(later);
            self->bindings.Dispatch(self->input);
        }
    });
    later = rig.Record(Jump, ActionEvent::Triggered, 'l');
    REQUIRE(rig.ok);
    REQUIRE(rig.Frame({InputEvent::KeyDown(Key::Space)}));
    // x → l(지웠지만 이번 목록에 있다), n은 다음부터.
    CHECK(rig.log.Is("xl"));
    CHECK(rig.bindings.BindingCount() == 2);
    REQUIRE(rig.Frame());
    CHECK(rig.log.Is("xn"));
    rig.bindings.ClearBindings();
    CHECK(rig.bindings.BindingCount() == 0);
    REQUIRE(rig.Frame());
    CHECK(rig.log.Is(""));
}

// 5.1절: 값 바인딩은 전달마다 그 액션의 현재 값이고, 표에 없는 액션·없는 handle은 0이다.
TEST(InputActionBindings_ValueBindings) {
    SystemAllocator allocator;
    Rig rig(allocator);
    rig.Action(Jump);
    rig.Bind(Jump, "SpaceBar");
    REQUIRE(rig.Start());
    auto jump = rig.bindings.BindActionValue(Id(Jump));
    auto missing = rig.bindings.BindActionValue(Id(99));
    REQUIRE(jump && missing);
    REQUIRE(rig.Frame({InputEvent::KeyDown(Key::Space)}));
    CHECK(rig.bindings.ActionValue(jump.Value()).value.x == 1.0);
    CHECK(rig.bindings.ActionValue(missing.Value()).value.x == 0.0);
    REQUIRE(rig.Frame({InputEvent::KeyUp(Key::Space)}));
    CHECK(rig.bindings.ActionValue(jump.Value()).value.x == 0.0);
    rig.bindings.RemoveValueBinding(jump.Value());
    CHECK(rig.bindings.ActionValue(jump.Value()).value.x == 0.0);
    CHECK(rig.bindings.ActionValue(ActionValueHandle{}).value.x == 0.0);
}

// BindAction: 빈 콜백·None 마스크는 InvalidArgument, 할당 실패는 OutOfMemory(바인딩 수 그대로).
TEST(InputActionBindings_BindValidationAndAllocation) {
    SystemAllocator allocator;
    InputActionBindings bindings(allocator);
    CHECK(bindings.BindAction(Id(Jump), ActionEvent::Triggered, InputActionBindings::Callback{})
              .Error() == ErrorCode::InvalidArgument);
    auto callback =
        InputActionBindings::Callback::TryCreate(allocator, [](const ActionEventData&) noexcept {});
    REQUIRE(callback);
    CHECK(bindings.BindAction(Id(Jump), ActionEvent::None, rocks::Move(callback.Value())).Error() ==
          ErrorCode::InvalidArgument);
    bool succeeded = false;
    uint32 failures = 0;
    for (uint64 attempt = 1; attempt < MaxCalls && !succeeded; ++attempt) {
        test::NFailAllocator failing(allocator);
        InputActionBindings fresh(failing);
        auto function = InputActionBindings::Callback::TryCreate(
            allocator, [](const ActionEventData&) noexcept {});
        REQUIRE(function);
        failing.FailOnAttempt(attempt);
        auto bound = fresh.BindAction(Id(Jump), StartedTriggered, rocks::Move(function.Value()));
        failing.DisableFailure();
        if (bound) {
            succeeded = fresh.BindingCount() == 1;
        } else {
            ++failures;
            CHECK(bound.Error() == ErrorCode::OutOfMemory && fresh.BindingCount() == 0);
        }
    }

    CHECK(succeeded && failures > 0);
}

// 5.1절: 마스크 하나가 Started·Triggered 둘 다 겹치면 한 번 부르고 두 비트를 받는다. 콜백은
// 값·시간도 받는다.
TEST(InputActionBindings_MaskAndEventData) {
    SystemAllocator allocator;
    Rig rig(allocator);
    rig.Action(Jump);
    rig.Bind(Jump, "SpaceBar");
    REQUIRE(rig.Start());
    ActionEventData last;
    usize calls = 0;
    rig.On(Jump, StartedTriggered, [&last, &calls](const ActionEventData& data) noexcept {
        last = data;
        ++calls;
    });
    REQUIRE(rig.ok);
    REQUIRE(rig.Frame({InputEvent::KeyDown(Key::Space)}));
    CHECK(calls == 1 && last.events == StartedTriggered && last.action == Id(Jump));
    CHECK(last.value.value.x == 1.0 && last.elapsedSeconds > 0.0 && last.triggeredSeconds > 0.0);
    REQUIRE(rig.Frame());
    CHECK(calls == 2 && last.events == ActionEvent::Triggered);
}

// 5.1절: 전달은 할당하지 않는다(앞 전달 중에 더한 바인딩의 Started 앞 삽입 포함). 전달 중
// ClearBindings는 이번 목록을 마치고 끝난 뒤 지운다.
TEST(InputActionBindings_DispatchAllocatesNothingAndClearDuringDispatch) {
    SystemAllocator allocator;
    test::NFailAllocator counting(allocator);
    Rig rig(counting);
    rig.Action(Jump);
    rig.Bind(Jump, "SpaceBar");
    REQUIRE(rig.Start());
    Rig* self = &rig;
    bool added = false;
    rig.On(Jump, ActionEvent::Started, [self, &added](const ActionEventData&) noexcept {
        self->log.Add('s', ActionEvent::Started);
        if (!added) {
            added = true;
            self->Record(Jump, ActionEvent::Started, 'n');
        }
    });
    REQUIRE(rig.ok);
    REQUIRE(rig.Frame({InputEvent::KeyDown(Key::Space)}));
    CHECK(rig.log.Is("s"));
    REQUIRE(rig.Frame({InputEvent::KeyUp(Key::Space)}));
    REQUIRE(rig.Frame());
    // 다음 누름: 평가 뒤 전달만 할당을 세어 본다.
    rig.state.BeginFrame({});
    const InputEvent down[] = {InputEvent::KeyDown(Key::Space)};
    rig.state.BeginFrame(Span<const InputEvent>(down, 1));
    REQUIRE(rig.input.Evaluate(rig.state, ActionEvalContext{Step, 1.0, false, 0.0f, false, 0}));
    counting.FailOnAttempt(MaxCalls);
    rig.bindings.Dispatch(rig.input);
    CHECK(counting.Attempts() == 0);
    counting.DisableFailure();
    rig.input.FinishDispatch();
    CHECK(rig.log.Is("ns"));

    // 전달 중 ClearBindings.
    rig.On(Jump, ActionEvent::Triggered, [self](const ActionEventData&) noexcept {
        self->log.Add('c', ActionEvent::Triggered);
        self->bindings.ClearBindings();
    });
    rig.Record(Jump, ActionEvent::Triggered, 'z');
    REQUIRE(rig.ok);
    REQUIRE(rig.Frame());
    CHECK(rig.log.Is("cz") && rig.bindings.BindingCount() == 0);
}

// 4.4절·5.1절: 콜백 안의 즉시 다시 만들기(같은 컨텍스트 갱신)가 chord 짝 목록을 바꿔도 같은 전달의
// 뒤 바인딩 억제 판정은 새 목록으로 맞다.
TEST(InputActionBindings_ChordSuppressionAfterRebuildInCallback) {
    SystemAllocator allocator;
    Rig rig(allocator);
    auto fill = [&](InputContextBundle& bundle) noexcept {
        bundle.id = Id(100);
        for (const uint64 id : {Ctrl, Save}) {
            InputActionData action(allocator);
            action.id = Id(id);
            (void)bundle.actions.TryPushBack(rocks::Move(action));
        }

        InputBinding ctrl(allocator);
        ctrl.action = Id(Ctrl);
        (void)ctrl.key.TryAssign("LeftControl");
        (void)bundle.data.bindings.TryPushBack(rocks::Move(ctrl));
        InputBinding save(allocator);
        save.action = Id(Save);
        (void)save.key.TryAssign("S");
        InputTrigger chord;
        chord.type = InputTriggerType::ChordAction;
        chord.chordActionId = Id(Ctrl);
        (void)save.triggers.TryPushBack(chord);
        (void)bundle.data.bindings.TryPushBack(rocks::Move(save));
    };
    fill(rig.bundle);
    REQUIRE(rig.Start());
    Rig* self = &rig;
    rig.On(Save, ActionEvent::Started, [self, &fill, &allocator](const ActionEventData&) noexcept {
        self->log.Add('r', ActionEvent::Started);
        InputContextBundle again(allocator);
        fill(again);
        (void)self->input.UpdateContext(rocks::Move(again), true);
    });
    rig.Record(Ctrl, ActionEvent::Triggered, 'C');
    rig.Record(Save, ActionEvent::Triggered, 'S');
    REQUIRE(rig.ok);
    REQUIRE(rig.Frame({InputEvent::KeyDown(Key::LeftControl)}));
    CHECK(rig.log.Is("C"));
    REQUIRE(rig.Frame({InputEvent::KeyDown(Key::S)}));
    // r(Started, 다시 만들기) → C는 억제 → S.
    CHECK(rig.log.Is("rS"));
}

} // namespace input_action_bindings_tests_file
