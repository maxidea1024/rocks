#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <Input/InputState.h>
#include <InputActions/ActionEvent.h>
#include <InputActions/ContextOptions.h>
#include <InputActions/InputActionSettings.h>
#include <InputActions/InputBindingView.h>
#include <InputActions/InputContextBundle.h>
#include <InputActions/InputKey.h>
#include <InputActions/InputModifier.h>
#include <InputActions/InputModifierState.h>
#include <InputActions/InputTrigger.h>
#include <InputActions/InputTriggerState.h>
#include <InputActions/InputValue.h>
#include <InputActions/TriggerTypes.h>

namespace rocks::input {

// 로컬 플레이어 하나의 컨텍스트 목록과 액션 상태다(UE UEnhancedPlayerInput + 입력 subsystem의
// 컨텍스트 목록 — Docs/Design/InputActions 4절). 컨텍스트는 InputContextBundle 사본으로 넣고 id로
// 가리킨다(2.7절). 추가·제거는 다시 만들기를 예약하고 ApplyPendingChanges(Evaluate가 처음에
// 부른다)가 한 번 한다(4.1·4.2절).
//
// 사용 예:
//   PlayerActionInput input(allocator);
//   if (auto added = input.AddContext(Move(bundle), 0); !added) { return added; }
//   if (auto applied = input.ApplyPendingChanges(inputState, gamepad); !applied) { ... }
//
// 할당: 다시 만들 때만 한다(4.6절). 스레드: 게임 스레드 전용이다.
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

    // 활성 키 프로필 id다(빈 값 = 없음). 바꾸면 다시 만들기를 예약한다. 할당 실패는
    // OutOfMemory(그대로).
    Result<void> SetKeyProfile(StringView profileId) noexcept;

    bool HasPendingRebuild() const noexcept {
        return rebuildPending_;
    }

    // 예약된 다시 만들기를 한다(4.2절). state·gamepad는 "지금 눌려 있음" 판정에 쓴다. 할당 실패면
    // OutOfMemory이고 이전 목록과 예약이 남는다.
    Result<void> ApplyPendingChanges(const InputState& state, uint32 gamepad) noexcept;

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
        // 바인딩에서 사라졌다(다음 평가에 값 0으로 한 번 평가한 뒤 지운다 — 4.2절 7).
        bool unmapped = false;
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

    Allocator* allocator_;
    Array<ContextEntry> contexts_;
    Array<ActionEntry> actions_;
    Array<BindingEntry> bindings_;
    // 마지막 평가에서 눌려 있던 키의 비트다(InputKeySlot — 즉시 다시 만들기의 "지금 눌려 있음",
    // 4.1절). 묶이지 않은 키도 담는다.
    static constexpr uint32 KeyBitWords = (InputKeySlotCount + 63) / 64;
    uint64 lastDownKeys_[KeyBitWords] = {};
    // 액션 표 앞쪽(바인딩이 가리키는 액션) 칸마다 이번 평가의 바인딩 수준 결합 상태다(chord 대상 —
    // TriggerContext::chordStates). 다시 만들 때 할당한다.
    Array<TriggerState> chordStates_;
    // 액션 표 앞쪽 칸 수다. 표는 바인딩이 가리키는 액션 → 사라진 액션 → 주입 전용 액션 순이다.
    usize mappedActions_ = 0;
    String profile_;
    uint64 nextOrder_ = 0;
    bool rebuildPending_ = false;
    bool pendingIgnorePressed_ = true;
};

} // namespace rocks::input
