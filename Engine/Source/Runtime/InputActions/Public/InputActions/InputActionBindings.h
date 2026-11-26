#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Function.h>
#include <Core/Memory/Allocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>
#include <InputActions/ActionBindingHandle.h>
#include <InputActions/ActionEvent.h>
#include <InputActions/ActionEventData.h>
#include <InputActions/ActionValueHandle.h>
#include <InputActions/InputValue.h>
#include <InputActions/PlayerActionInput.h>

namespace rocks::input {

// 액션 이벤트를 콜백에 전달한다(UE UEnhancedInputComponent의 바인딩 부분 — Docs/Design/InputActions
// 5.1절). PlayerActionInput의 평가 뒤 Dispatch가 등록 순으로 실행 목록을 만들고(Started는 앞에 끼워
// 넣는다) 소비·chord 억제를 적용해 실행한다.
//
// 콜백 수명: Function은 캡처를 그대로 쥔다(UE delegate처럼 객체 파괴를 알지 못한다). 객체를
// 가리키려면 handle을 캡처해 World::With로 접근하거나, 그 객체가 없어지기 전에(OnUnPossess·EndPlay)
// RemoveBinding한다. 콜백을 만든 allocator는 바인딩보다 오래 살아야 한다.
//
// 사용 예:
//   auto callback = Function<void(const ActionEventData&)>::TryCreate(
//       allocator, [world, pawn](const ActionEventData& event) noexcept {
//           (void)world->With<MyPawn>(pawn, [](MyPawn& value) noexcept { value.Jump(); });
//       });
//   if (callback) {
//       (void)bindings.BindAction(jumpId, ActionEvent::Triggered, Move(callback.Value()));
//   }
//   ...
//   (void)input.Evaluate(state, context);
//   bindings.Dispatch(input);
//   input.FinishDispatch();
//
// 할당: 바인딩 추가 때만 한다(전달은 할당하지 않는다). 스레드: 게임 스레드 전용이다.
class ROCKS_INPUTACTIONS_API InputActionBindings {
public:
    using Callback = Function<void(const ActionEventData&)>;

    explicit InputActionBindings(Allocator& allocator) noexcept;
    InputActionBindings(const InputActionBindings&) = delete;
    InputActionBindings& operator=(const InputActionBindings&) = delete;

    // events(마스크)와 겹치는 이벤트가 난 평가마다 callback을 한 번 부른다(받는 이벤트는 겹친
    // 비트). consumes면 실행 목록에서 이 뒤의 같은 액션 바인딩을 막는다. 빈 callback·None 마스크는
    // InvalidArgument(UE는 None 바인딩을 이벤트 없는 평가마다 부르지만 Rocks는 받지 않는다
    // — 5.1절), 할당 실패는 OutOfMemory(그대로)다. 전달 중에 더하면 다음 전달부터 실행한다.
    Result<ActionBindingHandle> BindAction(const Guid& actionId, ActionEvent events,
                                           Callback&& callback, bool consumes = false) noexcept;

    // 전달마다 그 액션의 현재 값을 담는 값 바인딩이다. 할당 실패는 OutOfMemory다.
    Result<ActionValueHandle> BindActionValue(const Guid& actionId) noexcept;

    // 값 바인딩의 값이다(없는 handle은 0 값).
    InputValue ActionValue(ActionValueHandle handle) const noexcept;

    // 바인딩을 지운다(없는 handle은 무시). 전달 중이면 이번 실행 목록에 있는 것은 실행하고 전달이
    // 끝난 뒤 지운다.
    void RemoveBinding(ActionBindingHandle handle) noexcept;

    // 값 바인딩을 지운다(없는 handle은 무시 — 전달 중에도 바로 지운다, 값 바인딩은 실행 목록이
    // 없다).
    void RemoveValueBinding(ActionValueHandle handle) noexcept;

    // 이벤트·값 바인딩을 모두 지운다(전달 중이면 이벤트 바인딩은 RemoveBinding과 같이 끝난 뒤).
    void ClearBindings() noexcept;

    // 이벤트 바인딩 수다(지움 표시된 것은 빼고 센다 — 값 바인딩은 세지 않는다).
    usize BindingCount() const noexcept;

    // input의 마지막 평가 이벤트를 전달한다(5.1절 전달 규칙). 콜백 안에서 바인딩을 더하거나 지워도
    // 되고(다음 전달부터 반영), input의 컨텍스트를 바꿔도(즉시 다시 만들기 포함) 된다. 콜백 안에서
    // input.Evaluate를 부르면 안 된다(이번 기록이 바뀐다 — 그 뒤 바인딩은 건너뛴다). 전달 중 다시
    // 부르면 아무것도 하지 않는다.
    void Dispatch(const PlayerActionInput& input) noexcept;

private:
    struct EventBinding {
        uint32 id = 0;
        Guid action;
        ActionEvent events = ActionEvent::None;
        Callback callback;
        bool consumes = false;
        bool removed = false;
    };

    struct ValueBinding {
        uint32 id = 0;
        Guid action;
        InputValue value;
    };

    void PurgeRemoved() noexcept;

    Allocator* allocator_;
    // 바인딩마다 따로 할당한다(콜백 실행 중 추가가 실행 중인 콜백을 옮기지 않는다).
    Array<UniquePtr<EventBinding>> bindings_;
    Array<ValueBinding> values_;
    // 전달 작업 배열이다(바인딩을 더할 때 용량을 잡는다 — 전달 무할당).
    Array<usize> runList_;
    Array<Guid> triggeredActions_;
    Array<Guid> consumedActions_;
    uint32 nextId_ = 1;
    bool dispatching_ = false;
};

} // namespace rocks::input
