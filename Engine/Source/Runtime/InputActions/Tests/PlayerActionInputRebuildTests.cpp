#include <Base/InitializerList.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Memory/SystemAllocator.h>
#include <Input/InputEvent.h>
#include <Input/InputState.h>
#include <InputActions/PlayerActionInput.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// 평가기 컨텍스트·다시 만들기 시험이다(Docs/Design/InputActions 4.1·4.2절 — IA2b): 우선순위·안정
// 순서, 소비(같은 컨텍스트 키 공유), 등록 횟수, 데이터 갱신, 3단 재정렬, chord 복제, ChordBlocker(+
// Down), 프로필, 놓을 때까지 무시, 키 이름 건너뛰기, 설정 충돌, 할당 실패.
using namespace rocks;
using namespace rocks::input;
namespace player_action_input_rebuild_tests_file {

namespace {

constexpr uint64 IdHigh = 0x50414952;
constexpr uint64 Jump = 1;
constexpr uint64 Fire = 2;
constexpr uint64 Ctrl = 3;
constexpr uint64 Save = 4;
constexpr uint64 Walk = 5;
constexpr uint64 MaxRebuildAttempts = 512;

Guid Id(uint64 low) noexcept {
    return Guid{IdHigh, low};
}

// 시험용 bundle 만들기다.
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
        return BindInto(bundle.data.bindings, action, key);
    }

    InputBinding& BindInto(Array<InputBinding>& list, uint64 action, const char* key) noexcept {
        InputBinding binding(allocator);
        binding.action = Id(action);
        ok = ok && binding.key.TryAssign(key).IsOk();
        ok = ok && list.TryPushBack(rocks::Move(binding)).IsOk();
        return list[list.Size() - 1];
    }

    Allocator& allocator;
    InputContextBundle bundle;
    bool ok = true;
};

InputTrigger Chord(uint64 target) noexcept {
    InputTrigger trigger;
    trigger.type = InputTriggerType::ChordAction;
    trigger.chordActionId = Id(target);
    return trigger;
}

InputTrigger Typed(InputTriggerType type) noexcept {
    InputTrigger trigger;
    trigger.type = type;
    return trigger;
}

InputKey Key(input::Key key) noexcept {
    return InputKey::FromKey(key);
}

// 다시 만든 바인딩 (액션, 키) 열이 기대와 같은지 본다.
struct Expect {
    uint64 action;
    input::Key key;
};

template <usize Count>
bool BindingsAre(const PlayerActionInput& input, const Expect (&expected)[Count]) noexcept {
    bool same = input.BindingCount() == Count;
    for (usize index = 0; same && index < Count; ++index) {
        const InputBindingView view = input.Binding(index);
        same = view.action == Id(expected[index].action) && view.key == Key(expected[index].key);
    }

    if (!same) {
        printf("  bindings:");
        for (usize index = 0; index < input.BindingCount(); ++index) {
            const InputBindingView view = input.Binding(index);
            printf(" (%u,%u)", static_cast<unsigned>(view.action.low),
                   static_cast<unsigned>(view.key.code));
        }

        printf("\n");
    }

    return same;
}

bool Apply(PlayerActionInput& input, const InputState& state) noexcept {
    return input.ApplyPendingChanges(state, 0).IsOk();
}

// 경고 코드 수를 센다.
class DiagCounter final : public RuntimeLogSink {
public:
    Result<void> Write(const LogEvent& event) noexcept override {
        unknownKeys += Contains(event.message, "(InputBindingKeyUnknown)") ? 1 : 0;
        conflicts += Contains(event.message, "(InputActionSettingsConflict)") ? 1 : 0;
        return Result<void>::Success();
    }

    uint32 unknownKeys = 0;
    uint32 conflicts = 0;

private:
    static bool Contains(StringView text, const char* needle) noexcept {
        const usize bytes = strlen(needle);
        for (usize start = 0; start + bytes <= text.ByteCount(); ++start) {
            if (memcmp(text.Data() + start, needle, bytes) == 0) {
                return true;
            }
        }

        return false;
    }
};

} // namespace

// 4.1·4.2절 1·3: 우선순위 내림차순(같으면 추가 순), 상위 컨텍스트가 소비한 키는 하위에서 빠진다.
// 같은 컨텍스트 안에서는 같은 키를 여러 바인딩이 쓴다. consumeInput 거짓·chord 바인딩은 소비하지
// 않는다.
TEST(PlayerActionInput_PriorityAndConsumption) {
    SystemAllocator allocator;
    InputState state;
    PlayerActionInput input(allocator);
    Builder low(allocator, 10);
    low.Action(Jump);
    low.Action(Fire);
    low.Bind(Jump, "SpaceBar");
    low.Bind(Fire, "LeftMouseButton");
    Builder high(allocator, 11);
    high.Action(Save);
    high.Action(Fire);
    high.Bind(Save, "SpaceBar");
    high.Bind(Fire, "SpaceBar");
    REQUIRE(low.ok && high.ok);
    REQUIRE(input.AddContext(rocks::Move(low.bundle), 0));
    REQUIRE(input.AddContext(rocks::Move(high.bundle), 5));
    CHECK(input.HasPendingRebuild());
    REQUIRE(Apply(input, state));
    CHECK(!input.HasPendingRebuild());
    // high의 두 바인딩(같은 키 공유) → low의 SpaceBar는 소비되어 빠지고 LeftMouseButton만 남는다.
    REQUIRE(input.BindingCount() == 3);
    CHECK(input.Binding(0).action == Id(Save) && input.Binding(1).action == Id(Fire));
    CHECK(input.Binding(2).action == Id(Fire) && input.Binding(2).context == Id(10));
    CHECK(input.Binding(2).key == InputKey::FromMouseButton(MouseButton::Left));
    // 액션 표는 바인딩이 가리키는 액션이다(Save·Fire — Jump는 바인딩이 없다).
    CHECK(input.ActionCount() == 2);

    // consumeInput 거짓이면 하위가 남는다. 같은 우선순위는 추가 순이다.
    PlayerActionInput passive(allocator);
    Builder first(allocator, 20);
    first.Action(Jump).settings.consumeInput = false;
    first.Bind(Jump, "SpaceBar");
    Builder second(allocator, 21);
    second.Action(Fire);
    second.Bind(Fire, "SpaceBar");
    REQUIRE(first.ok && second.ok);
    REQUIRE(passive.AddContext(rocks::Move(first.bundle), 3));
    REQUIRE(passive.AddContext(rocks::Move(second.bundle), 3));
    REQUIRE(Apply(passive, state));
    CHECK(BindingsAre(passive, {{Jump, Key::Space}, {Fire, Key::Space}}));
}

// 4.1절: 같은 id 다시 넣기(Untracked = 우선순위만, CountRegistrations = 횟수), UpdateContext, 제거,
// 비우기, 검증 실패.
TEST(PlayerActionInput_ContextRegistration) {
    SystemAllocator allocator;
    InputState state;
    PlayerActionInput input(allocator);
    auto make = [&](uint64 id, uint64 action, const char* key,
                    ContextRegistrationTracking tracking) noexcept {
        Builder builder(allocator, id);
        builder.Action(action);
        builder.Bind(action, key);
        builder.bundle.data.registrationTracking = tracking;
        return rocks::Move(builder.bundle);
    };

    REQUIRE(
        input.AddContext(make(30, Jump, "SpaceBar", ContextRegistrationTracking::Untracked), 1));
    REQUIRE(input.AddContext(make(31, Fire, "E", ContextRegistrationTracking::Untracked), 2));
    REQUIRE(Apply(input, state));
    CHECK(BindingsAre(input, {{Fire, Key::E}, {Jump, Key::Space}}));
    // Untracked 다시 넣기: 우선순위가 바뀌고 데이터(다른 키)는 무시된다.
    REQUIRE(input.AddContext(make(30, Jump, "Q", ContextRegistrationTracking::Untracked), 9));
    REQUIRE(Apply(input, state));
    CHECK(BindingsAre(input, {{Jump, Key::Space}, {Fire, Key::E}}));
    // UpdateContext: 데이터만 바꾸고 우선순위는 그대로다. 없는 id는 NotFound다.
    REQUIRE(input.UpdateContext(make(30, Jump, "Q", ContextRegistrationTracking::Untracked)));
    REQUIRE(Apply(input, state));
    CHECK(BindingsAre(input, {{Jump, Key::Q}, {Fire, Key::E}}));
    CHECK(
        input.UpdateContext(make(99, Jump, "Q", ContextRegistrationTracking::Untracked)).Error() ==
        ErrorCode::NotFound);

    // CountRegistrations: 두 번 넣으면 두 번 빼야 빠진다(첫 제거는 다시 만들기 없음).
    REQUIRE(
        input.AddContext(make(32, Save, "S", ContextRegistrationTracking::CountRegistrations), 0));
    REQUIRE(Apply(input, state));
    REQUIRE(
        input.AddContext(make(32, Save, "S", ContextRegistrationTracking::CountRegistrations), 7));
    CHECK(!input.HasPendingRebuild());
    CHECK(input.BindingCount() == 3 && input.Binding(2).action == Id(Save));
    REQUIRE(input.RemoveContext(Id(32)));
    CHECK(!input.HasPendingRebuild() && input.HasContext(Id(32)));
    REQUIRE(input.RemoveContext(Id(32)));
    CHECK(input.HasPendingRebuild() && !input.HasContext(Id(32)));
    REQUIRE(Apply(input, state));
    CHECK(input.BindingCount() == 2);
    CHECK(input.RemoveContext(Id(32)).Error() == ErrorCode::NotFound);

    // 검증 실패는 InvalidArgument이고 상태가 그대로다.
    Builder broken(allocator, 33);
    broken.Bind(Jump, "SpaceBar");
    REQUIRE(broken.ok);
    CHECK(input.AddContext(rocks::Move(broken.bundle), 0).Error() == ErrorCode::InvalidArgument);
    CHECK(!input.HasContext(Id(33)) && !input.HasPendingRebuild());

    // 비우기·즉시 다시 만들기.
    input.ClearContexts();
    CHECK(input.HasPendingRebuild() && !input.HasContext(Id(30)));
    REQUIRE(Apply(input, state));
    CHECK(input.BindingCount() == 0 && input.ActionCount() == 0);
    ContextOptions now;
    now.forceImmediately = true;
    REQUIRE(input.AddContext(make(34, Jump, "SpaceBar", ContextRegistrationTracking::Untracked), 0,
                             now));
    CHECK(!input.HasPendingRebuild() && input.BindingCount() == 1);
}

// 4.2절 2·4·5: 3단 재정렬(chord 대상 → chord → 나머지, 안정), 액션 수준 chord 복제, ChordBlocker는
// chord 바인딩 뒤의 같은 키 바인딩(다른 컨텍스트 포함)에 들어가고 Explicit이 없으면 Down(1e-8)이
// 먼저 붙는다.
TEST(PlayerActionInput_ChordOrderingAndBlockers) {
    SystemAllocator allocator;
    InputState state;
    PlayerActionInput input(allocator);
    Builder chords(allocator, 40);
    chords.Action(Fire);
    chords.Action(Jump);
    chords.Action(Ctrl);
    // Save는 액션 수준 chord(Ctrl)를 가진다.
    REQUIRE(chords.Action(Save).settings.triggers.TryPushBack(Chord(Ctrl)));
    chords.Bind(Fire, "F");
    InputBinding& chordedJump = chords.Bind(Jump, "S");
    (void)chordedJump.triggers.TryPushBack(Chord(Ctrl));
    chords.Bind(Ctrl, "LeftControl");
    chords.Bind(Save, "D");
    Builder lower(allocator, 41);
    lower.Action(Walk);
    lower.Action(Fire);
    lower.Bind(Walk, "S");
    InputBinding& pressedS = lower.Bind(Fire, "S");
    (void)pressedS.triggers.TryPushBack(Typed(InputTriggerType::Pressed));
    REQUIRE(chords.ok && lower.ok);
    REQUIRE(input.AddContext(rocks::Move(chords.bundle), 1));
    REQUIRE(input.AddContext(rocks::Move(lower.bundle), 0));
    REQUIRE(Apply(input, state));
    // chords 컨텍스트: Ctrl(대상) → Jump·Save(chord, 원래 순서) → Fire. chord 바인딩은 S를 소비하지
    // 않으므로 lower의 S 둘이 남는다.
    CHECK(BindingsAre(input, {{Ctrl, Key::LeftControl},
                              {Jump, Key::S},
                              {Save, Key::D},
                              {Fire, Key::F},
                              {Walk, Key::S},
                              {Fire, Key::S}}));
    REQUIRE(input.BindingCount() == 6);
    // 액션 표 번호: Ctrl·Jump·Save·Fire·Move 순서(바인딩에 처음 나온 순).
    usize ctrlIndex = input.ActionCount();
    usize jumpIndex = input.ActionCount();
    for (usize index = 0; index < input.ActionCount(); ++index) {
        ctrlIndex = input.ActionId(index) == Id(Ctrl) ? index : ctrlIndex;
        jumpIndex = input.ActionId(index) == Id(Jump) ? index : jumpIndex;
    }

    REQUIRE(ctrlIndex < input.ActionCount() && jumpIndex < input.ActionCount());
    // Jump: 자기 ChordAction(색인 = Ctrl).
    const InputBindingView jump = input.Binding(1);
    REQUIRE(jump.triggers.Size() == 1);
    CHECK(jump.triggers[0].type == InputTriggerType::ChordAction &&
          jump.triggers[0].chordAction == ctrlIndex);
    // Save: 액션 수준 chord가 바인딩 수준에 복제된다.
    const InputBindingView save = input.Binding(2);
    REQUIRE(save.triggers.Size() == 1);
    CHECK(save.triggers[0].type == InputTriggerType::ChordAction &&
          save.triggers[0].chordActionId == Id(Ctrl));
    // Walk(S, 트리거 없음): Down(1e-8) + ChordBlocker(Jump).
    const InputBindingView walk = input.Binding(4);
    REQUIRE(walk.triggers.Size() == 2);
    CHECK(walk.triggers[0].type == InputTriggerType::Down &&
          walk.triggers[0].actuationThreshold == ChordBlockerDownThreshold);
    CHECK(walk.triggers[1].type == InputTriggerType::ChordBlocker &&
          walk.triggers[1].chordActionId == Id(Jump) && walk.triggers[1].chordAction == jumpIndex);
    // Fire(S, Pressed — Explicit 있음): ChordBlocker만.
    const InputBindingView fire = input.Binding(5);
    REQUIRE(fire.triggers.Size() == 2);
    CHECK(fire.triggers[0].type == InputTriggerType::Pressed &&
          fire.triggers[1].type == InputTriggerType::ChordBlocker);
    // 다른 키(F·D·LeftControl)는 블로커가 없다.
    CHECK(input.Binding(0).triggers.Size() == 0 && input.Binding(3).triggers.Size() == 0);
}

// 4.2절 2: 활성 프로필의 override가 기본 bindings를 대신하고, 매핑 설정의 supportedProfiles가
// 거른다.
TEST(PlayerActionInput_KeyProfiles) {
    SystemAllocator allocator;
    InputState state;
    PlayerActionInput input(allocator);
    Builder builder(allocator, 50);
    InputActionData& jump = builder.Action(Jump);
    PlayerMappingSettings jumpSettings(allocator);
    REQUIRE(jumpSettings.name.TryAssign("Jump"));
    String classic(allocator);
    REQUIRE(classic.TryAssign("Classic"));
    REQUIRE(jumpSettings.supportedProfiles.TryPushBack(rocks::Move(classic)));
    jump.settings.mappingSettings.Emplace(rocks::Move(jumpSettings));
    builder.Action(Fire);
    builder.Bind(Jump, "SpaceBar");
    builder.Bind(Fire, "E");
    InputProfileBindings southpaw(allocator);
    REQUIRE(southpaw.profile.TryAssign("Southpaw"));
    builder.BindInto(southpaw.bindings, Fire, "Q");
    builder.BindInto(southpaw.bindings, Jump, "SpaceBar");
    REQUIRE(builder.bundle.data.profileOverrides.TryPushBack(rocks::Move(southpaw)));
    REQUIRE(builder.ok);
    REQUIRE(input.AddContext(rocks::Move(builder.bundle), 0));
    REQUIRE(Apply(input, state));
    // 프로필 없음: 기본, 거르지 않는다.
    CHECK(BindingsAre(input, {{Jump, Key::Space}, {Fire, Key::E}}));
    // Southpaw: override 목록, Jump는 Classic 전용이라 빠진다.
    REQUIRE(input.SetKeyProfile("Southpaw"));
    CHECK(input.HasPendingRebuild());
    REQUIRE(Apply(input, state));
    CHECK(BindingsAre(input, {{Fire, Key::Q}}));
    // Classic: override가 없으므로 기본, Jump는 지원 프로필이라 남는다.
    REQUIRE(input.SetKeyProfile("Classic"));
    REQUIRE(Apply(input, state));
    CHECK(BindingsAre(input, {{Jump, Key::Space}, {Fire, Key::E}}));
    // 같은 프로필 다시 설정은 다시 만들지 않는다.
    REQUIRE(input.SetKeyProfile("Classic"));
    CHECK(!input.HasPendingRebuild());
}

// 4.2절 6: 새 Boolean 바인딩의 키가 눌려 있으면 놓을 때까지 무시한다(축 액션·옵션 거짓·이미 있던
// 바인딩은 아니다). 즉시 다시 만들기는 마지막 평가의 키 상태로 본다(평가 전이면 눌린 키 없음).
TEST(PlayerActionInput_IgnorePressedKeysUntilRelease) {
    SystemAllocator allocator;
    InputState state;
    const InputEvent down[] = {InputEvent::KeyDown(Key::Space)};
    PlayerActionInput input(allocator);
    Builder first(allocator, 60);
    first.Action(Jump);
    first.Bind(Jump, "E");
    REQUIRE(first.ok);
    REQUIRE(input.AddContext(rocks::Move(first.bundle), 0));
    REQUIRE(Apply(input, state));
    CHECK(!input.Binding(0).ignored);

    state.BeginFrame(Span<const InputEvent>(down, 1));
    const InputEvent edown[] = {InputEvent::KeyDown(Key::E)};
    state.BeginFrame(Span<const InputEvent>(edown, 1));
    Builder second(allocator, 61);
    second.Action(Fire);
    second.Action(Walk, InputValueType::Axis1D);
    second.Bind(Fire, "SpaceBar");
    second.Bind(Walk, "SpaceBar");
    REQUIRE(second.ok);
    REQUIRE(input.AddContext(rocks::Move(second.bundle), 1));
    REQUIRE(Apply(input, state));
    // second(높음): Fire(Boolean·눌림) 무시, Move(축) 아님. first의 E는 이미 있던 바인딩이라
    // 그대로다.
    REQUIRE(input.BindingCount() == 3);
    CHECK(input.Binding(0).action == Id(Fire) && input.Binding(0).ignored);
    CHECK(input.Binding(1).action == Id(Walk) && !input.Binding(1).ignored);
    CHECK(input.Binding(2).action == Id(Jump) && !input.Binding(2).ignored);
    // 다시 만들어도 무시 상태는 짝지은 바인딩으로 넘어간다.
    REQUIRE(input.SetKeyProfile("Any"));
    REQUIRE(Apply(input, state));
    CHECK(input.Binding(0).ignored && !input.Binding(2).ignored);

    // 옵션 거짓이면 무시하지 않는다(요청들 AND — 하나라도 거짓이면 거짓).
    PlayerActionInput relaxed(allocator);
    Builder third(allocator, 62);
    third.Action(Fire);
    third.Bind(Fire, "SpaceBar");
    Builder fourth(allocator, 63);
    fourth.Action(Jump);
    fourth.Bind(Jump, "E");
    REQUIRE(third.ok && fourth.ok);
    ContextOptions keep;
    keep.ignorePressedKeysUntilRelease = false;
    REQUIRE(relaxed.AddContext(rocks::Move(third.bundle), 0, keep));
    REQUIRE(relaxed.AddContext(rocks::Move(fourth.bundle), 0));
    REQUIRE(Apply(relaxed, state));
    CHECK(!relaxed.Binding(0).ignored && !relaxed.Binding(1).ignored);

    // 즉시 다시 만들기: 아직 평가 전이라 눌린 키 기록이 없다(평가 뒤 기록 경로는 IA2c 시험).
    PlayerActionInput immediate(allocator);
    Builder fifth(allocator, 64);
    fifth.Action(Fire);
    fifth.Bind(Fire, "SpaceBar");
    REQUIRE(fifth.ok);
    ContextOptions now;
    now.forceImmediately = true;
    REQUIRE(immediate.AddContext(rocks::Move(fifth.bundle), 0, now));
    CHECK(immediate.BindingCount() == 1 && !immediate.Binding(0).ignored);
}

// 2.7·4.2절 2: 빈 키는 조용히, 모르는 이름은 경고와 함께 그 바인딩만 건너뛴다. 같은 id 액션의
// 설정이 다르면 정렬 순 첫 사본(높은 우선순위)을 쓴다.
TEST(PlayerActionInput_SkippedKeysAndSettingsConflict) {
    DiagCounter counter;
    SystemAllocator logAllocator;
    LogSystemConfig logConfig;
    logConfig.mode = LogMode::Manual;
    logConfig.globalMinimum = LogLevel::Debug;
    LogSystem logs(logAllocator, logConfig);
    REQUIRE(logs.Start() && logs.AddSink(counter));
    LogInstallScope installed(logs);
    SystemAllocator allocator;
    InputState state;
    PlayerActionInput input(allocator);
    Builder builder(allocator, 70);
    builder.Action(Jump);
    builder.Bind(Jump, "");
    builder.Bind(Jump, "Gamepad_LeftTrigger");
    builder.Bind(Jump, "spacebar");
    REQUIRE(builder.ok);
    REQUIRE(input.AddContext(rocks::Move(builder.bundle), 0));
    REQUIRE(Apply(input, state));
    CHECK(BindingsAre(input, {{Jump, Key::Space}}));
    (void)logs.Pump();
    CHECK(counter.unknownKeys == 1 && counter.conflicts == 0);

    // 충돌: 높은 쪽 Move는 Axis1D, 낮은 쪽은 Boolean — 높은 쪽 설정이라 눌린 키 무시가 없다.
    const InputEvent down[] = {InputEvent::KeyDown(Key::Q), InputEvent::KeyDown(Key::W)};
    state.BeginFrame(Span<const InputEvent>(down, 2));
    PlayerActionInput conflicted(allocator);
    Builder high(allocator, 71);
    high.Action(Walk, InputValueType::Axis1D);
    high.Bind(Walk, "Q");
    Builder low(allocator, 72);
    low.Action(Walk, InputValueType::Boolean);
    low.Bind(Walk, "W");
    REQUIRE(high.ok && low.ok);
    REQUIRE(conflicted.AddContext(rocks::Move(low.bundle), 0));
    REQUIRE(conflicted.AddContext(rocks::Move(high.bundle), 1));
    REQUIRE(Apply(conflicted, state));
    REQUIRE(conflicted.BindingCount() == 2 && conflicted.ActionCount() == 1);
    CHECK(!conflicted.Binding(0).ignored && !conflicted.Binding(1).ignored);
    (void)logs.Pump();
    // 충돌 경고는 액션당 한 번이다(바인딩 수와 무관).
    CHECK(counter.conflicts == 1);

    // 설정 원천은 키 소비·건너뛰기와 무관한 정렬 순 첫 사본이다: 높은 쪽 바인딩이 모르는 키로
    // 빠져도 높은 쪽(Axis1D) 설정이라 낮은 쪽 W(눌림)가 무시되지 않는다.
    PlayerActionInput sourced(allocator);
    Builder highSkipped(allocator, 73);
    highSkipped.Action(Walk, InputValueType::Axis1D);
    highSkipped.Bind(Walk, "NoSuchKey");
    Builder lowKept(allocator, 74);
    lowKept.Action(Walk, InputValueType::Boolean);
    lowKept.Bind(Walk, "W");
    REQUIRE(highSkipped.ok && lowKept.ok);
    REQUIRE(sourced.AddContext(rocks::Move(highSkipped.bundle), 1));
    REQUIRE(sourced.AddContext(rocks::Move(lowKept.bundle), 0));
    REQUIRE(Apply(sourced, state));
    REQUIRE(sourced.BindingCount() == 1);
    CHECK(!sourced.Binding(0).ignored);
}

// 4.2절 5: 같은 키 chord 바인딩 둘 — 뒤 chord 바인딩에도 앞 chord의 Blocker가 들어가고, Down은 한
// 번만이다(넣은 Down이 Explicit이다 — UE와 같다). chord 대상이면서 chord인 바인딩이 가장 앞이다.
TEST(PlayerActionInput_ChordChains) {
    SystemAllocator allocator;
    InputState state;
    PlayerActionInput input(allocator);
    Builder builder(allocator, 90);
    builder.Action(Jump);
    builder.Action(Fire);
    builder.Action(Walk);
    builder.Action(Ctrl);
    builder.Action(Save);
    builder.Bind(Walk, "S");
    (void)builder.Bind(Jump, "S").triggers.TryPushBack(Chord(Ctrl));
    (void)builder.Bind(Fire, "S").triggers.TryPushBack(Chord(Ctrl));
    // Ctrl 자신도 Save(Shift 대신)에 묶인 chord다 — 대상이면서 chord.
    (void)builder.Bind(Ctrl, "LeftControl").triggers.TryPushBack(Chord(Save));
    builder.Bind(Save, "LeftShift");
    REQUIRE(builder.ok);
    REQUIRE(input.AddContext(rocks::Move(builder.bundle), 0));
    REQUIRE(Apply(input, state));
    // 대상이면서 chord(Ctrl) → 대상(Save) → chord(Jump·Fire) → 나머지(Walk).
    CHECK(BindingsAre(input, {{Ctrl, Key::LeftControl},
                              {Save, Key::LeftShift},
                              {Jump, Key::S},
                              {Fire, Key::S},
                              {Walk, Key::S}}));
    REQUIRE(input.BindingCount() == 5);
    // Fire: ChordAction(Ctrl) + Down(1e-8) + Blocker(Jump).
    const InputBindingView fire = input.Binding(3);
    REQUIRE(fire.triggers.Size() == 3);
    CHECK(fire.triggers[1].type == InputTriggerType::Down &&
          fire.triggers[2].type == InputTriggerType::ChordBlocker &&
          fire.triggers[2].chordActionId == Id(Jump));
    // Walk: Down 하나 + Blocker(Jump) + Blocker(Fire).
    const InputBindingView walk = input.Binding(4);
    REQUIRE(walk.triggers.Size() == 3);
    CHECK(walk.triggers[0].type == InputTriggerType::Down &&
          walk.triggers[1].chordActionId == Id(Jump) && walk.triggers[2].chordActionId == Id(Fire));
}

// 4.2절 3·7: 모든 바인딩이 소비되면 액션 표에서 빠지고, 컨텍스트를 빼서 바인딩이 사라진 액션도
// 빠진다(진행 중이 아니면 — 진행 중이던 액션의 이월은 평가 시험 IA2c).
TEST(PlayerActionInput_ActionTableFollowsBindings) {
    SystemAllocator allocator;
    InputState state;
    PlayerActionInput input(allocator);
    Builder high(allocator, 100);
    high.Action(Jump);
    high.Bind(Jump, "SpaceBar");
    Builder low(allocator, 101);
    low.Action(Fire);
    low.Bind(Fire, "SpaceBar");
    REQUIRE(high.ok && low.ok);
    REQUIRE(input.AddContext(rocks::Move(high.bundle), 1));
    REQUIRE(input.AddContext(rocks::Move(low.bundle), 0));
    REQUIRE(Apply(input, state));
    CHECK(input.ActionCount() == 1 && input.ActionId(0) == Id(Jump));
    REQUIRE(input.RemoveContext(Id(100)));
    REQUIRE(Apply(input, state));
    CHECK(input.ActionCount() == 1 && input.ActionId(0) == Id(Fire));
    CHECK(input.ActionId(5) == Guid{} && input.Binding(9).action == Guid{});
}

// 4.1절: 등록의 할당 실패는 상태·bundle 그대로, 즉시 다시 만들기 실패는 성공(등록 반영 + 예약
// 남음), 프로필 설정 실패는 그대로다.
TEST(PlayerActionInput_RegistrationAllocationFailures) {
    SystemAllocator allocator;
    InputState state;
    test::NFailAllocator failing(allocator);
    PlayerActionInput input(failing);
    Builder builder(allocator, 110);
    builder.Action(Jump);
    builder.Bind(Jump, "SpaceBar");
    REQUIRE(builder.ok);
    failing.FailOnAttempt(1);
    CHECK(input.AddContext(rocks::Move(builder.bundle), 0).Error() == ErrorCode::OutOfMemory);
    failing.DisableFailure();
    CHECK(!input.HasContext(Id(110)) && !input.HasPendingRebuild());
    CHECK(builder.bundle.id == Id(110) && builder.bundle.data.bindings.Size() == 1);

    // 즉시 다시 만들기의 첫 할당을 실패시킨다(컨텍스트 배열 용량은 먼저 확보한다).
    REQUIRE(input.AddContext(rocks::Move(builder.bundle), 0));
    REQUIRE(Apply(input, state));
    Builder second(allocator, 111);
    second.Action(Fire);
    second.Bind(Fire, "E");
    REQUIRE(second.ok);
    ContextOptions now;
    now.forceImmediately = true;
    failing.FailOnAttempt(2);
    CHECK(input.AddContext(rocks::Move(second.bundle), 1, now));
    failing.DisableFailure();
    CHECK(input.HasContext(Id(111)) && input.HasPendingRebuild());
    CHECK(BindingsAre(input, {{Jump, Key::Space}}));
    REQUIRE(Apply(input, state));
    CHECK(BindingsAre(input, {{Fire, Key::E}, {Jump, Key::Space}}));

    failing.FailOnAttempt(1);
    CHECK(input.SetKeyProfile("AVeryLongProfileNameThatNeedsHeapStorage").Error() ==
          ErrorCode::OutOfMemory);
    failing.DisableFailure();
    CHECK(!input.HasPendingRebuild());
}

// 4.6절: 다시 만들기 할당 실패면 OutOfMemory이고 이전 목록과 예약이 남는다(다음에 다시 시도).
TEST(PlayerActionInput_RebuildAllocationFailureKeepsPreviousLists) {
    SystemAllocator allocator;
    InputState state;
    test::NFailAllocator failing(allocator);
    PlayerActionInput input(failing);
    Builder base(allocator, 80);
    base.Action(Jump);
    base.Bind(Jump, "SpaceBar");
    REQUIRE(base.ok);
    REQUIRE(input.AddContext(rocks::Move(base.bundle), 0));
    REQUIRE(Apply(input, state));
    REQUIRE(input.BindingCount() == 1);

    Builder more(allocator, 81);
    more.Action(Ctrl);
    REQUIRE(more.Action(Save).settings.triggers.TryPushBack(Chord(Ctrl)));
    more.Bind(Ctrl, "LeftControl");
    more.Bind(Save, "SpaceBar");
    InputModifier dead(allocator);
    REQUIRE(more.bundle.data.bindings[1].modifiers.TryPushBack(rocks::Move(dead)));
    REQUIRE(more.ok);
    REQUIRE(input.AddContext(rocks::Move(more.bundle), 1));
    uint32 failures = 0;
    bool rebuilt = false;
    for (uint64 attempt = 1; attempt < MaxRebuildAttempts && !rebuilt; ++attempt) {
        failing.FailOnAttempt(attempt);
        auto applied = input.ApplyPendingChanges(state, 0);
        failing.DisableFailure();
        if (applied) {
            rebuilt = true;
        } else {
            ++failures;
            CHECK(applied.Error() == ErrorCode::OutOfMemory);
            CHECK(input.HasPendingRebuild() && input.ActionCount() == 1);
            CHECK(BindingsAre(input, {{Jump, Key::Space}}));
        }
    }

    CHECK(rebuilt && failures > 0);
    // Ctrl → Save(SpaceBar, chord라 소비 안 함) → Jump(SpaceBar, Down + ChordBlocker).
    CHECK(BindingsAre(input, {{Ctrl, Key::LeftControl}, {Save, Key::Space}, {Jump, Key::Space}}));
    CHECK(input.Binding(2).triggers.Size() == 2);
}

} // namespace player_action_input_rebuild_tests_file
