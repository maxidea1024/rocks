#pragma once

#include <Core/Containers/Span.h>
#include <Input/InputEvent.h>

namespace rocks::input {

// 게임패드 데드존이다. 스틱은 원형(radial) 데드존이며 경계 밖을 [0, 1]로 다시 늘린다. 기본값은
// XInput 권장값(왼쪽 7849/32767, 오른쪽 8689/32767, 트리거 30/255)과 같다.
struct GamepadDeadZone {
    float leftStick = 0.2395f;
    float rightStick = 0.2652f;
    float trigger = 0.1176f;
};

// 한 프레임 동안 변하지 않는 입력 스냅샷이다. 게임 코드는 프레임 중 몇 번을 읽어도 같은 값을 본다.
//
// 사용 예:
//   if (input.WasPressed(Key::Space)) { Jump(); }          // 이번 프레임에 눌렸을 때 한 번
//   if (input.IsDown(Key::W)) { MoveForward(delta); }      // 누르고 있는 동안 계속
//   Vec2 look(input.MouseDeltaX(), input.MouseDeltaY());
//   float moveX = input.GamepadAxisValue(0, GamepadAxis::LeftX);
//
// edge 규칙:
// - WasPressed/WasReleased는 해당 프레임에만 참이다. 소비하지 않은 edge는 다음 프레임으로 넘어가지
//   않으므로 pause 뒤 resume에서 과거 입력이 다시 발생하지 않는다.
// - 한 프레임 안에서 눌렀다 뗐으면 WasPressed와 WasReleased가 모두 참이고 IsDown은 거짓이다.
// - 이미 눌린 키의 KeyDown(OS 자동 반복 포함)은 edge를 만들지 않는다. 눌려 있지 않던 키의 KeyDown은
//   repeat 표시와 관계없이 눌림 edge다(포커스 복귀 직후의 반복 down).
// - FocusLost는 눌린 키·마우스 버튼을 모두 놓은 것으로 처리하고 WasReleased를 세운다.
//
// 스레드: 동기화하지 않는다. BeginFrame은 게임 스레드의 프레임 시작에서 호출한다.
class InputState {
public:
    InputState() noexcept = default;

    // 데드존을 바꾼다. 각 값은 [0, 1) 범위의 유한한 값이어야 하며 아니면 InvalidArgument로 거부하고
    // 기존 값을 유지한다.
    Result<void> SetGamepadDeadZone(const GamepadDeadZone& deadZone) noexcept;

    // 이전 프레임의 edge를 지우고 events를 순서대로 적용해 새 스냅샷을 만든다. 이벤트가 없어도
    // 프레임마다 호출해야 edge와 마우스 이동량이 초기화된다. 잘못된 열거 값의 이벤트는 무시한다.
    void BeginFrame(Span<const InputEvent> events) noexcept;

    bool IsDown(Key key) const noexcept;
    bool WasPressed(Key key) const noexcept;
    bool WasReleased(Key key) const noexcept;

    bool IsDown(MouseButton button) const noexcept;
    bool WasPressed(MouseButton button) const noexcept;
    bool WasReleased(MouseButton button) const noexcept;

    // 마지막으로 알려진 커서 위치(창 client 픽셀)다. 아직 이동 이벤트가 없으면 HasMousePosition이
    // 거짓이고 위치는 0이다.
    bool HasMousePosition() const noexcept {
        return hasMousePosition_;
    }

    float MouseX() const noexcept {
        return mouseX_;
    }

    float MouseY() const noexcept {
        return mouseY_;
    }

    // 이번 프레임의 커서 이동량이다. 첫 위치 이벤트와 FocusLost 직후 첫 위치는 이동량에 넣지
    // 않는다.
    float MouseDeltaX() const noexcept {
        return mouseDeltaX_;
    }

    float MouseDeltaY() const noexcept {
        return mouseDeltaY_;
    }

    // 이번 프레임의 휠 회전 합(칸 단위, 위로 +)이다.
    float MouseWheel() const noexcept {
        return wheel_;
    }

    // 이번 프레임에 입력된 문자 수와 각 문자다(최대 MaxTextPerFrame개, 넘치면 버린다).
    static constexpr uint32 MaxTextPerFrame = 32;

    Span<const uint32> Text() const noexcept {
        return Span<const uint32>(text_, textCount_);
    }

    // 게임패드 index(0..MaxGamepads-1)의 상태다. 범위 밖 index는 연결되지 않은 것으로 본다.
    bool IsGamepadConnected(uint32 index) const noexcept;
    bool IsDown(uint32 gamepad, GamepadButton button) const noexcept;
    bool WasPressed(uint32 gamepad, GamepadButton button) const noexcept;
    bool WasReleased(uint32 gamepad, GamepadButton button) const noexcept;

    // 데드존을 적용한 축 값이다. 스틱 [-1, 1], 트리거 [0, 1]. 연결되지 않았으면 0이다.
    float GamepadAxisValue(uint32 gamepad, GamepadAxis axis) const noexcept;

private:
    static constexpr uint32 KeyWords = (KeyCount + 63) / 64;

    struct Bits {
        uint64 words[KeyWords] = {};

        bool Get(uint32 index) const noexcept {
            return (words[index / 64] >> (index % 64)) & 1u;
        }

        void Set(uint32 index, bool value) noexcept {
            const uint64 mask = uint64(1) << (index % 64);
            words[index / 64] = value ? (words[index / 64] | mask) : (words[index / 64] & ~mask);
        }
    };

    struct Gamepad {
        bool connected = false;
        uint32 buttons = 0;
        uint32 pressed = 0;
        uint32 released = 0;
        float axes[GamepadAxisCount] = {};
    };

    void ApplyGamepad(uint32 index, const GamepadSnapshot& snapshot) noexcept;
    void ReleaseAll() noexcept;

    Bits keysDown_;
    Bits keysPressed_;
    Bits keysReleased_;
    uint32 mouseDown_ = 0;
    uint32 mousePressed_ = 0;
    uint32 mouseReleased_ = 0;
    bool hasMousePosition_ = false;
    bool mouseDeltaValid_ = false;
    float mouseX_ = 0.0f;
    float mouseY_ = 0.0f;
    float mouseDeltaX_ = 0.0f;
    float mouseDeltaY_ = 0.0f;
    float wheel_ = 0.0f;
    uint32 text_[MaxTextPerFrame] = {};
    uint32 textCount_ = 0;
    Gamepad gamepads_[MaxGamepads];
    GamepadDeadZone deadZone_;
};

} // namespace rocks::input
