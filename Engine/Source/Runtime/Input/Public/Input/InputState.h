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
// - FocusLost는 눌린 키·마우스 버튼을 모두 놓은 것으로 처리하고 WasReleased를 세운다. 닿아 있는
//   손가락은 모두 취소된 끝(WasTouchEnded·WasTouchCancelled)이 된다.
// - 터치(Docs/Design/TouchInput.ko.md 3.3절): 손가락마다 같은 edge 규칙이다(한 프레임에 시작과 끝이
//   모두 오면 둘 다 참이고 끝난 상태다). 시작 없이 오는 TouchMoved·TouchEnded는 무시한다
//   (require-release — 손실 복구 뒤에도 다음 TouchBegan부터 다시 받는다).
//
// 스레드: 동기화하지 않는다. BeginFrame은 게임 스레드의 프레임 시작에서 호출한다.
class ROCKS_INPUT_API InputState {
public:
    InputState() noexcept = default;

    // 데드존을 바꾼다. 각 값은 [0, 1) 범위의 유한한 값이어야 하며 아니면 InvalidArgument로 거부하고
    // 기존 값을 유지한다.
    Result<void> SetGamepadDeadZone(const GamepadDeadZone& deadZone) noexcept;

    // 이전 프레임의 edge를 지우고 events를 순서대로 적용해 새 스냅샷을 만든다. 이벤트가 없어도
    // 프레임마다 호출해야 edge와 마우스 이동량이 초기화된다. 잘못된 열거 값의 이벤트는 무시한다.
    // eventsLost는 이번 프레임 이벤트를 모으는 동안 큐가 이벤트를
    // 버렸는지다(InputQueue::DroppedCount가 늘었다). 참이면 이벤트를 적용한 뒤 눌린
    // 키·버튼·손가락을 모두 놓는다(손실 복구 — 버려진 뗌 이벤트로 눌린 채 남지 않게,
    // Docs/Design/TouchInput.ko.md 3.3절).
    void BeginFrame(Span<const InputEvent> events, bool eventsLost = false) noexcept;

    // 마우스로 터치 흉내다(UE bUseMouseForTouch, 기본 꺼짐 — Docs/Design/TouchInput.ko.md 3.4절).
    // 켜면 왼쪽 버튼의 누름·이동·뗌이 손가락 0번 터치가 되고, 그 버튼의 마우스 눌림·뗌은 만들지
    // 않는다(같은 입력을 두 번 처리하지 않게). 다른 버튼·휠·커서 위치는 그대로다. 바꾸면 흉내
    // 중이던 터치를 다음 BeginFrame에서 취소로 끝낸다.
    void SetMouseTouchEmulation(bool enabled) noexcept;

    bool IsMouseTouchEmulation() const noexcept {
        return mouseTouchEmulation_;
    }

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
    // dead zone을 적용하기 전의 원시 정규화 값이다(스틱 [-1, 1], 트리거 [0, 1] — 비유한 값은 0).
    // 입력 액션 계층이 읽는다: dead zone은 DeadZone 모디파이어가 건다(UE와 같다 — Docs/Design/
    // InputActions 2.2절). 연결되지 않은 패드·범위 밖은 0이다.
    float GamepadAxisRawValue(uint32 gamepad, GamepadAxis axis) const noexcept;

    // 손가락 index(0..MaxTouches-1)의 상태다. 범위 밖 index는 닿지 않은 것으로 본다. 위치는 창
    // client 픽셀이고, 끝난 손가락은 마지막 위치를 유지한다.
    bool IsTouchDown(uint32 index) const noexcept;
    bool WasTouchBegan(uint32 index) const noexcept;
    bool WasTouchEnded(uint32 index) const noexcept;
    // 이번 프레임의 끝이 취소(OS 취소·포커스 상실·손실 복구)였다.
    bool WasTouchCancelled(uint32 index) const noexcept;
    float TouchX(uint32 index) const noexcept;
    float TouchY(uint32 index) const noexcept;
    // 시작한 위치다(끌기 판정 — UE OnTouchFirstMove 대신, TD3).
    float TouchStartX(uint32 index) const noexcept;
    float TouchStartY(uint32 index) const noexcept;
    // 이번 프레임 이동량이다.
    float TouchDeltaX(uint32 index) const noexcept;
    float TouchDeltaY(uint32 index) const noexcept;
    // 0–1 압력이다. TouchHasForce가 거짓이면 장치가 압력을 보고하지 않은 것이고 값은 1.0이다.
    float TouchForce(uint32 index) const noexcept;
    bool TouchHasForce(uint32 index) const noexcept;

    // 지금 닿아 있는 손가락 수다.
    uint32 TouchCount() const noexcept;

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
        // dead zone을 적용하기 전 값이다(GamepadAxisRawValue).
        float rawAxes[GamepadAxisCount] = {};
    };

    struct Touch {
        bool down = false;
        bool began = false;
        bool ended = false;
        bool cancelled = false;
        bool hasForce = false;
        float x = 0.0f;
        float y = 0.0f;
        float startX = 0.0f;
        float startY = 0.0f;
        float deltaX = 0.0f;
        float deltaY = 0.0f;
        float force = 1.0f;
    };

    void ApplyGamepad(uint32 index, const GamepadSnapshot& snapshot) noexcept;
    void ApplyTouch(const InputEvent& event) noexcept;
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
    Touch touches_[MaxTouches];
    bool mouseTouchEmulation_ = false;
    // 흉내 설정을 바꿨으면 다음 BeginFrame에서 흉내 중인 터치를 취소로 끝낸다.
    bool cancelEmulatedTouch_ = false;
};

} // namespace rocks::input
