#pragma once

#include <Base/Types.h>
#include <Core/Memory/Allocator.h>
#include <InputActions/InputActionBindings.h>
#include <Meta/Annotations.h>
#include <World/Component.h>

namespace rocks {

// 입력 바인딩 층이다(UE UInputComponent + UEnhancedInputComponent —
// Docs/Design/InputActions 5.5절). PlayerController의 입력 스택에 들어가면 그 플레이어의 액션
// 이벤트를 받는다. priority가 높은 층이 먼저 받고, blockInput이면 그 아래층은 받지 않는다.
// 바인딩(Bindings)은 5.1절 API 그대로이고 저장하지 않는다.
//
// 사용 예(Actor 입력 켜기):
//   auto component = controller.EnableInput(door);
//   if (component) {
//       (void)world.With<InputComponent>(component.Value(), [&](InputComponent& input) noexcept {
//           input.SetPriority(5);
//           return input.Bindings().BindAction(useId, input::ActionEvent::Started, Move(onUse));
//       });
//   }
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_GAMEPLAY_API InputComponent : public Component {
    ROCKS_GENERATED_BODY()

public:
    explicit InputComponent(Allocator& allocator) noexcept
        : Component(allocator), bindings_(allocator) {
    }

    // 스택 정렬 순위다(클수록 먼저 받는다 — UE Priority). 기본 0이다.
    int32 Priority() const noexcept {
        return priority_;
    }

    // 다음 틱의 스택 정렬부터 반영한다.
    void SetPriority(int32 priority) noexcept {
        priority_ = priority;
    }

    // 이 층 아래는 입력을 받지 않는지다(UE bBlockInput). 기본 거짓이다.
    bool BlocksInput() const noexcept {
        return blockInput_;
    }

    void SetBlockInput(bool block) noexcept {
        blockInput_ = block;
    }

    // 이 층의 액션 바인딩이다(콜백·함수·값 — 5.1절).
    input::InputActionBindings& Bindings() noexcept {
        return bindings_;
    }

    const input::InputActionBindings& Bindings() const noexcept {
        return bindings_;
    }

private:
    ROCKS_PROPERTY(ReadOnly, Name = "priority") int32 priority_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "blockInput") bool blockInput_ = false;
    ROCKS_NATIVE() input::InputActionBindings bindings_;
};

} // namespace rocks
