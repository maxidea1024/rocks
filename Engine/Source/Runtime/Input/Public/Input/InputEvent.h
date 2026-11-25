#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/SharedPtr.h>
#include <Core/Text/String.h>
#include <Input/InputTypes.h>

namespace rocks::input {

enum class InputEventType : uint8 {
    // key가 눌림. repeat는 OS 자동 반복 표시다. 이미 눌린 키의 down은 edge를 만들지
    // 않는다(InputState).
    KeyDown,
    KeyUp,
    // 입력된 문자 하나(UTF-32 code point). 키 바인딩이 아니라 텍스트 입력에 쓴다.
    Text,
    // 마우스 커서 위치(창 client 영역 픽셀, 좌상단 원점). 절대 위치다.
    MouseMove,
    MouseDown,
    MouseUp,
    // 휠 회전. wheel은 한 칸 = 1.0이며 위(앞)로 굴리면 +다.
    MouseWheel,
    // OS가 마우스 capture를 강제로 해제했다.
    PointerCaptureLost,
    // 게임패드 한 개의 전체 상태(backend가 폴링한 값). 연결 해제도 이 이벤트로 알린다.
    GamepadState,
    // 손가락(또는 펜)이 닿았다·움직였다(위치·압력)·떨어졌다(Docs/Design/TouchInput.ko.md 3.1절).
    // touchIndex는 backend가 배정한 손가락 번호(0..MaxTouches-1 — TouchIndexAllocator)이고, x·y는
    // 창 client 영역 픽셀(좌상단 원점)이다. force는 0–1 압력이며 hasForce가 거짓이면 1.0이다.
    // TouchEnded의 cancelled는 OS 취소(제스처 가로챔·포커스·손실 복구)다.
    TouchBegan,
    TouchMoved,
    TouchEnded,
    // 창이 입력 포커스를 잃음. 눌려 있던 키·버튼을 모두 놓은 것으로 처리한다.
    FocusLost,
    // 조합 중 문자열과 UTF-8 커서 위치다. 빈 문자열은 기존 조합 표시를 취소한다.
    TextComposition,
    // IME가 확정한 한 덩어리다. 여러 코드 포인트도 편집 이력 하나를 만든다.
    TextCompositionCommit,
    MouseLeave
};

// 게임패드 한 개의 폴링 결과다. 축은 데드존을 적용하기 전의 정규화 값이다.
struct GamepadSnapshot {
    bool connected = false;
    // GamepadButton 순서의 비트 집합이다.
    uint32 buttons = 0;
    float axes[GamepadAxisCount] = {};
};

// 입력 이벤트 하나다. type에 해당하는 필드만 의미가 있다.
struct InputEvent {
    InputEventType type = InputEventType::FocusLost;
    Key key = Key::None;
    bool repeat = false;
    // 마우스 버튼·휠 이벤트에 창 client 좌표가 함께 실렸는지다.
    bool hasPointerPosition = false;
    MouseButton mouseButton = MouseButton::Left;
    uint8 gamepadIndex = 0;
    uint32 codePoint = 0;
    uint32 userId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float wheel = 0.0f;
    GamepadSnapshot gamepad;
    SharedPtr<const String> composition;
    usize compositionCaret = 0;
    uint8 touchIndex = 0;
    float force = 1.0f;
    bool hasForce = false;
    bool cancelled = false;

    static InputEvent KeyDown(Key value, bool isRepeat = false) noexcept {
        InputEvent event;
        event.type = InputEventType::KeyDown;
        event.key = value;
        event.repeat = isRepeat;
        return event;
    }

    static InputEvent KeyUp(Key value) noexcept {
        InputEvent event;
        event.type = InputEventType::KeyUp;
        event.key = value;
        return event;
    }

    static InputEvent Text(uint32 value) noexcept {
        InputEvent event;
        event.type = InputEventType::Text;
        event.codePoint = value;
        return event;
    }

    static InputEvent Composition(SharedPtr<const String> text = {}, usize caret = 0,
                                  bool commit = false) noexcept {
        InputEvent event;
        event.type = commit ? InputEventType::TextCompositionCommit : InputEventType::TextComposition;
        event.composition = Move(text);
        event.compositionCaret = caret;
        return event;
    }

    static InputEvent MouseMove(float positionX, float positionY) noexcept {
        InputEvent event;
        event.type = InputEventType::MouseMove;
        event.x = positionX;
        event.y = positionY;
        return event;
    }

    static InputEvent MouseDown(MouseButton button) noexcept {
        InputEvent event;
        event.type = InputEventType::MouseDown;
        event.mouseButton = button;
        return event;
    }

    static InputEvent MouseDown(MouseButton button, float positionX, float positionY) noexcept {
        InputEvent event = MouseDown(button);
        event.x = positionX;
        event.y = positionY;
        event.hasPointerPosition = true;
        return event;
    }

    static InputEvent MouseUp(MouseButton button) noexcept {
        InputEvent event;
        event.type = InputEventType::MouseUp;
        event.mouseButton = button;
        return event;
    }

    static InputEvent MouseUp(MouseButton button, float positionX, float positionY) noexcept {
        InputEvent event = MouseUp(button);
        event.x = positionX;
        event.y = positionY;
        event.hasPointerPosition = true;
        return event;
    }

    static InputEvent MouseWheel(float delta) noexcept {
        InputEvent event;
        event.type = InputEventType::MouseWheel;
        event.wheel = delta;
        return event;
    }

    static InputEvent MouseWheel(float delta, float positionX, float positionY) noexcept {
        InputEvent event = MouseWheel(delta);
        event.x = positionX;
        event.y = positionY;
        event.hasPointerPosition = true;
        return event;
    }

    static InputEvent Gamepad(uint8 index, const GamepadSnapshot& snapshot) noexcept {
        InputEvent event;
        event.type = InputEventType::GamepadState;
        event.gamepadIndex = index;
        event.gamepad = snapshot;
        return event;
    }

    static InputEvent TouchBegan(uint8 index, float positionX, float positionY,
                                 float pressure = 1.0f, bool pressureKnown = false) noexcept {
        InputEvent event;
        event.type = InputEventType::TouchBegan;
        event.touchIndex = index;
        event.x = positionX;
        event.y = positionY;
        event.force = pressure;
        event.hasForce = pressureKnown;
        return event;
    }

    static InputEvent TouchMoved(uint8 index, float positionX, float positionY,
                                 float pressure = 1.0f, bool pressureKnown = false) noexcept {
        InputEvent event = TouchBegan(index, positionX, positionY, pressure, pressureKnown);
        event.type = InputEventType::TouchMoved;
        return event;
    }

    static InputEvent TouchEnded(uint8 index, float positionX, float positionY,
                                 bool isCancelled = false) noexcept {
        InputEvent event;
        event.type = InputEventType::TouchEnded;
        event.touchIndex = index;
        event.x = positionX;
        event.y = positionY;
        event.cancelled = isCancelled;
        return event;
    }

    static InputEvent FocusLost() noexcept {
        InputEvent event;
        event.type = InputEventType::FocusLost;
        return event;
    }

    static InputEvent CaptureLost() noexcept {
        InputEvent event;
        event.type = InputEventType::PointerCaptureLost;
        return event;
    }
};

// 한 프레임 동안 모은 입력 이벤트 큐다. 입력 backend가 Push하고 InputState::BeginFrame이 소비한 뒤
// Clear한다. 용량은 생성 시 정하며 프레임 중 할당하지 않는다. 가득 차면 이벤트를 버리고
// DroppedCount를 늘린다(입력 폭주가 메모리를 늘리지 않게 한다).
//
// 사용 예:
//   InputQueue queue(allocator);
//   if (!queue.TryInitialize(256)) { ... }       // OutOfMemory
//   queue.Push(InputEvent::KeyDown(Key::W));
//   state.BeginFrame(queue.Events());
//   queue.Clear();
//
// 스레드: 동기화하지 않는다. 게임 스레드(창 메시지 펌프 스레드)에서만 쓴다.
class ROCKS_INPUT_API InputQueue {
public:
    explicit InputQueue(Allocator& allocator) noexcept : events_(allocator) {
    }

    // 최대 이벤트 수를 정하고 저장소를 미리 할당한다. 0은 InvalidArgument다. 다시 호출하면 기존
    // 이벤트를 버리고 새 용량으로 바꾼다.
    Result<void> TryInitialize(usize capacity) noexcept;

    // 이벤트를 추가한다. 가득 찼으면 버리고 false를 반환한다.
    bool Push(const InputEvent& event) noexcept;

    // 보관한 이벤트와 직전 전달 이후의 유실 건수를 대상 큐에 넘긴다.
    void DrainTo(InputQueue& target) noexcept;

    Span<const InputEvent> Events() const noexcept {
        return events_.View();
    }

    // 이벤트를 비운다. DroppedCount는 유지한다.
    void Clear() noexcept {
        events_.Clear();
    }

    // 생성 이후 용량 초과로 버린 이벤트 수다.
    uint64 DroppedCount() const noexcept {
        return dropped_;
    }

private:
    Array<InputEvent> events_;
    usize capacity_ = 0;
    uint64 dropped_ = 0;
    uint64 forwardedDropped_ = 0;
};

} // namespace rocks::input
