#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <Input/InputState.h>
#include <InputActions/ActionEvalContext.h>
#include <InputActions/ActionEvent.h>
#include <InputActions/ActionEventRecord.h>
#include <InputActions/ActionStateView.h>
#include <InputActions/ChordDependency.h>
#include <InputActions/ContextOptions.h>
#include <InputActions/InputActionData.h>
#include <InputActions/InputActionSettings.h>
#include <InputActions/InputBindingView.h>
#include <InputActions/InputContextBundle.h>
#include <InputActions/InputKey.h>
#include <InputActions/InputModifier.h>
#include <InputActions/InputModifierState.h>
#include <InputActions/InputTrigger.h>
#include <InputActions/InputTriggerState.h>
#include <InputActions/InputValue.h>
#include <InputActions/TriggerCombiner.h>
#include <InputActions/TriggerContext.h>
#include <InputActions/TriggerTypes.h>

namespace rocks::input {

class InputUserSettings;

// 로컬 플레이어 하나의 컨텍스트 목록과 액션 상태다(UE UEnhancedPlayerInput + 입력 subsystem의
// 컨텍스트 목록 — Docs/Design/InputActions 4절). 컨텍스트는 InputContextBundle 사본으로 넣고 id로
// 가리킨다(2.7절). 추가·제거는 다시 만들기를 예약하고 ApplyPendingChanges(Evaluate가 처음에
// 부른다)가 한 번 한다(4.1·4.2절).
//
// 사용 예:
//   PlayerActionInput input(allocator);
//   if (auto added = input.AddContext(Move(bundle), 0); !added) { return added; }
//   (void)input.Evaluate(inputState, context);   // 평가마다
//   for (const ActionEventRecord& event : input.Events()) { ... }
//   input.FinishDispatch();                        // 전달을 마친 뒤
//
// 할당(4.6절): 컨텍스트 추가·다시 만들기·주입 호출 때 한다. 평가(Evaluate·FinishDispatch·Flush)는
// 이벤트 목록이 예약을 넘을 때(전달 중 즉시 다시 만들기로 액션이 는 경우)만 할당한다. 스레드: 게임
// 스레드 전용이다.
class ROCKS_INPUTACTIONS_API PlayerActionInput {
public:
    explicit PlayerActionInput(Allocator& allocator) noexcept;
    PlayerActionInput(const PlayerActionInput&) = delete;
    PlayerActionInput& operator=(const PlayerActionInput&) = delete;

    // ---- 컨텍스트(4.1절) ----

    // bundle을 priority(클수록 먼저)로 넣는다(bundle은 옮겨 온다 — 그 allocator가 컨텍스트보다
    // 오래 살아야 한다). 검증(ValidateInputContextBundle) 실패는 InvalidArgument, 할당 실패는
    // OutOfMemory이고 둘 다 상태·bundle이 그대로다. 같은 id가 이미 있으면 데이터는 건드리지
    // 않고(UE와 같다) Untracked면 우선순위를 바꾸고 다시 만들기를 예약하며, CountRegistrations면
    // 등록 횟수만 늘린다. forceImmediately면 그 자리에서 다시 만든다. 그 다시 만들기가 실패해도
    // 등록은 반영되었으므로 성공이고 예약이 남는다(다음 ApplyPendingChanges가 다시 시도한다).
    Result<void> AddContext(InputContextBundle&& bundle, int32 priority,
                            const ContextOptions& options = {}) noexcept;

    // 있는 id의 데이터 사본을 bundle로 바꾸고 다시 만들기를 예약한다(우선순위·등록 횟수 그대로 —
    // 2.7절). 없는 id는 NotFound, 검증 실패는 InvalidArgument다(상태·bundle 그대로).
    Result<void> UpdateContext(InputContextBundle&& bundle, bool forceImmediately = false) noexcept;

    // 컨텍스트를 뺀다(CountRegistrations면 횟수가 0이 될 때). 없는 id는 NotFound다.
    Result<void> RemoveContext(const Guid& contextId, const ContextOptions& options = {}) noexcept;

    // 모든 컨텍스트를 빼고 다시 만들기를 예약한다(UE ClearAllMappings).
    void ClearContexts() noexcept;

    bool HasContext(const Guid& contextId) const noexcept;

    // 사용자 설정(리바인딩 — 5.2절)을 붙인다(null이면 뗀다). 붙어 있으면 활성 프로필은 설정의 현재
    // 프로필이고, 리바인딩 가능한 바인딩은 사용자 키로 바뀌며, 설정의 Revision이 바뀌면 다음
    // ApplyPendingChanges가 다시 만든다. notifyUserSettings 옵션의 컨텍스트는 설정에 등록·해제한다.
    // settings는 붙어 있는 동안 살아 있어야 한다. 다시 만들기를 예약한다.
    void SetUserSettings(InputUserSettings* settings) noexcept;

    InputUserSettings* UserSettings() const noexcept {
        return settings_;
    }

    // 활성 키 프로필 id다(빈 값 = 없음 — 사용자 설정이 없을 때 쓴다). 바꾸면 다시 만들기를
    // 예약한다. 할당 실패는 OutOfMemory(그대로).
    Result<void> SetKeyProfile(StringView profileId) noexcept;

    bool HasPendingRebuild() const noexcept {
        return rebuildPending_;
    }

    // 예약된 다시 만들기를 한다(4.2절). state·gamepad는 "지금 눌려 있음" 판정에 쓴다. 할당 실패면
    // OutOfMemory이고 이전 목록과 예약이 남는다.
    Result<void> ApplyPendingChanges(const InputState& state, uint32 gamepad) noexcept;

    // ---- 평가(4.3절) ----

    // 평가 한 번이다(UE UEnhancedPlayerInput::EvaluateInputDelegates의 액션 부분): 앞 평가의 전달
    // 뒤 정리 → 예약된 다시 만들기 → 바인딩 루프 → 주입 → 액션 마무리. 다시 만들기가 실패하면 이전
    // 목록으로 평가를 마치고 OutOfMemory를 돌려준다. 이벤트 목록이 용량을 넘어 커질 때의 할당
    // 실패도 OutOfMemory(그 기록은 빠진다)다.
    Result<void> Evaluate(const InputState& state, const ActionEvalContext& context) noexcept;

    // 전달을 마친 뒤의 정리다(4.3절 6): 끝난 액션의 시간을 0으로, 사라진 액션·끝난 주입 전용 액션을
    // 표에서 지운다. 안 부르면 다음 Evaluate가 처음에 한다.
    void FinishDispatch() noexcept;

    // 액션의 마지막 평가 상태(값·트리거 상태·이벤트·경과/Triggered 시간)다. 표(바인딩이 가리키는
    // 액션·사라진 액션·주입 전용 액션)에 없는 액션은 found 거짓이고 나머지는 기본값이다. 시간은
    // FinishDispatch 전까지 이번 평가의 누적값이다.
    ActionStateView ActionState(const Guid& actionId) const noexcept;

    // 마지막 평가의 이벤트다(다음 Evaluate까지 유효하다 — 전달 중 즉시 다시 만들기·주입도 무효로
    // 만들지 않는다).
    Span<const ActionEventRecord> Events() const noexcept {
        return events_.View();
    }

    // 다시 만들 때 모은 chord 짝이다(전달 억제 — 4.4절). 다음 다시 만들기까지 유효하다.
    Span<const ChordDependency> ChordDependencies() const noexcept {
        return chordDependencies_.View();
    }

    // 마지막 평가에서 눌려 있던 디지털 바인딩을 놓을 때까지 무시한다(UE FlushPressedKeys — 4.3절).
    void Flush() noexcept;

    // ---- 주입(4.3절 3) ----

    // 다음 Evaluate에 action의 값을 한 번 넣는다(UE InjectInputForAction). 표에 그 id가 있으면 표의
    // 설정을, 없으면 action의 설정을 쓴다(주입 전용 항목). 검증 실패는 InvalidArgument, 할당 실패는
    // OutOfMemory이고 둘 다 상태가 그대로다.
    Result<void> InjectValue(const InputActionData& action, const InputValue& value,
                             Span<const InputTrigger> triggers = {},
                             Span<const InputModifier> modifiers = {}) noexcept;

    // 평가마다 다시 넣는 주입이다(UE StartContinuousInjectionForAction). 같은 액션의 이전 연속
    // 주입을 대신한다. 오류는 InjectValue와 같다.
    Result<void> StartContinuousInjection(const InputActionData& action, const InputValue& value,
                                          Span<const InputTrigger> triggers = {},
                                          Span<const InputModifier> modifiers = {}) noexcept;

    // 연속 주입의 값을 바꾼다. 연속 주입이 없으면 NotFound다.
    Result<void> UpdateContinuousInjection(const Guid& actionId, const InputValue& value) noexcept;

    // 연속 주입을 멈춘다(다음 평가에서 놓기 — 값 0). 없으면 NotFound다.
    Result<void> StopContinuousInjection(const Guid& actionId) noexcept;

    // ---- 다시 만든 결과(진단·시험) ----

    usize BindingCount() const noexcept {
        return bindings_.Size();
    }

    // index ≥ BindingCount()면 빈 보기다.
    InputBindingView Binding(usize index) const noexcept;

    usize ActionCount() const noexcept {
        return actions_.Size();
    }

    // index ≥ ActionCount()면 0 id다.
    Guid ActionId(usize index) const noexcept;

private:
    struct ContextEntry {
        explicit ContextEntry(Allocator& allocator) noexcept : bundle(allocator) {
        }

        InputContextBundle bundle;
        int32 priority = 0;
        uint32 registrations = 1;
        // 같은 우선순위의 안정 순서(추가 순 — 7.3절)다.
        uint64 order = 0;
    };

    // 액션 표 한 칸이다(UE FInputActionInstance). 설정은 사본이다.
    struct ActionEntry {
        explicit ActionEntry(Allocator& allocator) noexcept
            : settings(allocator), triggerStates(allocator), modifierStates(allocator) {
        }

        Guid id;
        InputActionSettings settings;
        Array<InputTriggerState> triggerStates;
        Array<InputModifierState> modifierStates;
        InputValue value;
        TriggerState state = TriggerState::None;
        ActionEvent events = ActionEvent::None;
        double elapsedSeconds = 0.0;
        double triggeredSeconds = 0.0;
        // 바인딩에서 사라졌다(다음 평가에 이벤트 없는 액션으로 마무리한 뒤 지운다 — 4.2절 7).
        bool unmapped = false;
        // 사라진 액션을 평가 한 번으로 마무리했다(정리에서 지운다 — 그 전 정리는 남긴다).
        bool unmappedFinished = false;
        // 바인딩 없이 주입으로만 들어온 액션이다(4.3절 3).
        bool injectedOnly = false;
        // 직전·이번 평가에 주입되었다(놓기 판정 — UE LastInjectedActions).
        bool injectedLast = false;
        bool injectedThis = false;
        // 이번 평가에 바인딩·주입 이벤트가 있었다(UE ActionsWithEventsThisTick)와 가장 진전된
        // 결합이다.
        bool hadEvent = false;
        TriggerCombiner combiner;
    };

    // 주입 하나의 트리거·모디파이어와 상태다((액션, 종류 열)마다 — 4.3절 3).
    struct InjectionEntry {
        explicit InjectionEntry(Allocator& allocator) noexcept
            : triggers(allocator), modifiers(allocator), triggerStates(allocator),
              modifierStates(allocator), pending(allocator) {
        }

        Guid action;
        Array<InputTrigger> triggers;
        Array<InputModifier> modifiers;
        Array<InputTriggerState> triggerStates;
        Array<InputModifierState> modifierStates;
        // 다음 평가에 넣을 값이다.
        Array<InputValue> pending;
        bool continuous = false;
        InputValue continuousValue;
    };

    // 다시 만든 바인딩 하나다(UE FEnhancedActionKeyMapping의 사본).
    struct BindingEntry {
        explicit BindingEntry(Allocator& allocator) noexcept
            : triggers(allocator), modifiers(allocator), triggerStates(allocator),
              modifierStates(allocator) {
        }

        uint32 action = 0;
        Guid context;
        InputKey key;
        Array<InputTrigger> triggers;
        Array<InputModifier> modifiers;
        Array<InputTriggerState> triggerStates;
        Array<InputModifierState> modifierStates;
        bool ignored = false;
        // alwaysTick 트리거가 바인딩·액션에 있다(None 키 이벤트여도 평가 — 4.3절 2).
        bool alwaysTick = false;
    };

    // 정렬 순 컨텍스트 번호(우선순위 내림차순, 같으면 추가 순)를 order에 쓴다.
    Result<void> SortedContexts(Array<uint32>& order) const noexcept;
    usize FindContext(const Guid& contextId) const noexcept;
    Result<void> Rebuild(const InputState* state, uint32 gamepad) noexcept;
    Result<void> RequestRebuild(const ContextOptions& options) noexcept;
    bool KeyDownForRebuild(const InputState* state, uint32 gamepad, InputKey key) const noexcept;
    bool LastDown(InputKey key) const noexcept;
    // ChordAction·ChordBlocker의 대상 색인을 table의 앞쪽 mapped칸에서 푼다(없으면 NoChordAction).
    static void ResolveChords(Span<InputTrigger> triggers, const Array<ActionEntry>& table,
                              usize mapped) noexcept;
    usize FindAction(const Guid& actionId) const noexcept;
    // 바인딩·주입 한 번을 액션에 반영한다(UE ProcessActionMappingEvent).
    void ProcessMapping(ActionEntry& action, usize actionIndex, const InputValue& raw,
                        Span<const InputTrigger> triggers, Span<InputTriggerState> triggerStates,
                        Span<const InputModifier> modifiers,
                        Span<InputModifierState> modifierStates,
                        const ModifierContext& modifierContext,
                        const TriggerContext& triggerContext) noexcept;
    Result<void> Inject(const InputActionData& action, const InputValue& value,
                        Span<const InputTrigger> triggers, Span<const InputModifier> modifiers,
                        bool continuous) noexcept;
    void EvaluateInjections(const ModifierContext& modifierContext,
                            const TriggerContext& triggerContext) noexcept;
    void RecordDownKeys(const InputState& state, uint32 gamepad) noexcept;

    Allocator* allocator_;
    Array<ContextEntry> contexts_;
    Array<ActionEntry> actions_;
    Array<BindingEntry> bindings_;
    // 마지막 평가에서 눌려 있던 키의 비트다(InputKeySlot — 즉시 다시 만들기의 "지금 눌려 있음",
    // 4.1절). 묶이지 않은 키도 담는다.
    static constexpr uint32 KeyBitsPerWord = 64;
    static constexpr uint32 KeyBitWords = (InputKeySlotCount + KeyBitsPerWord - 1) / KeyBitsPerWord;
    uint64 lastDownKeys_[KeyBitWords] = {};
    // 액션 표 앞쪽(바인딩이 가리키는 액션) 칸마다 이번 평가의 바인딩 수준 결합 상태다(chord 대상 —
    // TriggerContext::chordStates). 다시 만들 때 할당한다.
    Array<TriggerState> chordStates_;
    // 액션 표 앞쪽 칸 수다. 표는 바인딩이 가리키는 액션 → 사라진 액션 → 주입 전용 액션 순이다.
    usize mappedActions_ = 0;
    Array<InjectionEntry> injections_;
    Array<ActionEventRecord> events_;
    Array<ChordDependency> chordDependencies_;
    // Evaluate 뒤 FinishDispatch를 아직 안 했다.
    bool dispatchPending_ = false;
    InputUserSettings* settings_ = nullptr;
    // 마지막 다시 만들기가 본 설정 Revision이다.
    uint64 settingsRevision_ = 0;
    String profile_;
    uint64 nextOrder_ = 0;
    bool rebuildPending_ = false;
    bool pendingIgnorePressed_ = true;
};

} // namespace rocks::input
