#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringView.h>
#include <Input/InputState.h>
#include <Input/TouchIndexAllocator.h>
#include <TestSupport/Test.h>
#include <string.h>

using namespace rocks;
namespace input_state_tests_file {

using namespace rocks::input;

namespace {

bool Near(float actual, float expected) noexcept {
    const float difference = actual - expected;
    return difference < 1.0e-4f && difference > -1.0e-4f;
}

template <usize Count>
void Frame(InputState& state, const InputEvent (&events)[Count]) noexcept {
    state.BeginFrame(Span<const InputEvent>(events, Count));
}

void Frame(InputState& state) noexcept {
    state.BeginFrame({});
}

} // namespace

TEST(Input_KeyEdgesLastOneFrameAndRepeatDoesNotPress) {
    InputState state;
    Frame(state, {InputEvent::KeyDown(Key::W)});
    CHECK(state.IsDown(Key::W) && state.WasPressed(Key::W) && !state.WasReleased(Key::W));

    // 자동 반복 down은 눌림을 유지하되 edge를 다시 만들지 않는다.
    Frame(state, {InputEvent::KeyDown(Key::W, true)});
    CHECK(state.IsDown(Key::W) && !state.WasPressed(Key::W));

    Frame(state);
    CHECK(state.IsDown(Key::W) && !state.WasPressed(Key::W));

    Frame(state, {InputEvent::KeyUp(Key::W)});
    CHECK(!state.IsDown(Key::W) && state.WasReleased(Key::W));
    Frame(state);
    CHECK(!state.WasReleased(Key::W));
}

TEST(Input_PressAndReleaseInSameFrameReportsBothEdges) {
    InputState state;
    Frame(state,
          {InputEvent::KeyDown(Key::Space), InputEvent::KeyUp(Key::Space),
           InputEvent::MouseDown(MouseButton::Left), InputEvent::MouseUp(MouseButton::Left)});
    CHECK(state.WasPressed(Key::Space) && state.WasReleased(Key::Space) &&
          !state.IsDown(Key::Space));
    CHECK(state.WasPressed(MouseButton::Left) && state.WasReleased(MouseButton::Left));
    CHECK(!state.IsDown(MouseButton::Left));
}

TEST(Input_UnconsumedEdgesAreNotReplayedAfterIdleFrames) {
    // pause 중에도 BeginFrame은 계속 호출된다. 그 사이에 생긴 edge는 해당 프레임에서 사라진다.
    InputState state;
    Frame(state, {InputEvent::KeyDown(Key::Enter)});
    for (int frame = 0; frame < 5; ++frame) {
        Frame(state);
    }
    CHECK(!state.WasPressed(Key::Enter) && state.IsDown(Key::Enter));
}

TEST(Input_MouseDeltaWheelAndFirstPositionIgnored) {
    InputState state;
    CHECK(!state.HasMousePosition());
    Frame(state, {InputEvent::MouseMove(100.0f, 50.0f)});
    CHECK(state.HasMousePosition() && state.MouseX() == 100.0f && state.MouseY() == 50.0f);
    CHECK(state.MouseDeltaX() == 0.0f && state.MouseDeltaY() == 0.0f);

    Frame(state, {InputEvent::MouseMove(110.0f, 40.0f), InputEvent::MouseMove(115.0f, 45.0f),
                  InputEvent::MouseWheel(1.0f), InputEvent::MouseWheel(-0.5f)});
    CHECK(Near(state.MouseDeltaX(), 15.0f) && Near(state.MouseDeltaY(), -5.0f));
    CHECK(Near(state.MouseWheel(), 0.5f));

    Frame(state);
    CHECK(state.MouseDeltaX() == 0.0f && state.MouseWheel() == 0.0f && state.MouseX() == 115.0f);
}

TEST(Input_FocusLostReleasesEverythingAndResetsMouseDelta) {
    InputState state;
    Frame(state, {InputEvent::KeyDown(Key::A), InputEvent::KeyDown(Key::LeftShift),
                  InputEvent::MouseDown(MouseButton::Right), InputEvent::MouseMove(10.0f, 10.0f)});
    Frame(state, {InputEvent::FocusLost()});
    CHECK(!state.IsDown(Key::A) && state.WasReleased(Key::A));
    CHECK(!state.IsDown(Key::LeftShift) && state.WasReleased(Key::LeftShift));
    CHECK(!state.IsDown(MouseButton::Right) && state.WasReleased(MouseButton::Right));

    // 포커스를 되찾은 뒤 첫 위치는 이동량을 만들지 않는다.
    Frame(state, {InputEvent::MouseMove(500.0f, 500.0f)});
    CHECK(state.MouseDeltaX() == 0.0f && state.MouseX() == 500.0f);
}

TEST(Input_TextIsCollectedPerFrameWithLimit) {
    InputState state;
    InputEvent events[InputState::MaxTextPerFrame + 3];
    for (uint32 index = 0; index < InputState::MaxTextPerFrame + 3; ++index) {
        events[index] = InputEvent::Text(0xAC00 + index);
    }
    Frame(state, events);
    CHECK(state.Text().Size() == InputState::MaxTextPerFrame);
    CHECK(state.Text()[0] == 0xAC00);
    Frame(state);
    CHECK(state.Text().Size() == 0);
}

TEST(Input_GamepadButtonsEdgesDeadZoneAndDisconnect) {
    InputState state;
    GamepadSnapshot pad;
    pad.connected = true;
    pad.buttons = 1u << static_cast<uint32>(GamepadButton::South);
    pad.axes[static_cast<uint32>(GamepadAxis::LeftX)] = 0.1f; // 데드존 안
    pad.axes[static_cast<uint32>(GamepadAxis::RightX)] = 1.0f;
    pad.axes[static_cast<uint32>(GamepadAxis::LeftTrigger)] = 1.0f;
    Frame(state, {InputEvent::Gamepad(1, pad)});
    CHECK(state.IsGamepadConnected(1) && !state.IsGamepadConnected(0));
    CHECK(state.WasPressed(1, GamepadButton::South) && state.IsDown(1, GamepadButton::South));
    CHECK(state.GamepadAxisValue(1, GamepadAxis::LeftX) == 0.0f);
    CHECK(Near(state.GamepadAxisValue(1, GamepadAxis::RightX), 1.0f));
    CHECK(Near(state.GamepadAxisValue(1, GamepadAxis::LeftTrigger), 1.0f));

    // 데드존 경계 밖은 [0, 1]로 다시 늘린다: 반지름 (1 + d) / 2는 0.5가 된다.
    const float half = (1.0f + GamepadDeadZone{}.leftStick) * 0.5f;
    pad.axes[static_cast<uint32>(GamepadAxis::LeftX)] = half;
    Frame(state, {InputEvent::Gamepad(1, pad)});
    CHECK(Near(state.GamepadAxisValue(1, GamepadAxis::LeftX), 0.5f));
    CHECK(!state.WasPressed(1, GamepadButton::South));

    GamepadSnapshot disconnected;
    Frame(state, {InputEvent::Gamepad(1, disconnected)});
    CHECK(!state.IsGamepadConnected(1));
    CHECK(state.WasReleased(1, GamepadButton::South));
    CHECK(state.GamepadAxisValue(1, GamepadAxis::RightX) == 0.0f);

    // 범위 밖 index는 무시한다.
    Frame(state, {InputEvent::Gamepad(MaxGamepads, pad)});
    CHECK(!state.IsGamepadConnected(MaxGamepads));
}

TEST(Input_QueueHasFixedCapacityAndCountsDrops) {
    SystemAllocator allocator;
    InputQueue queue(allocator);
    CHECK(!queue.Push(InputEvent::KeyDown(Key::A)));
    CHECK(queue.TryInitialize(0).Error() == ErrorCode::InvalidArgument);
    REQUIRE(queue.TryInitialize(2));
    CHECK(queue.Push(InputEvent::KeyDown(Key::A)));
    CHECK(queue.Push(InputEvent::KeyUp(Key::A)));
    CHECK(!queue.Push(InputEvent::KeyDown(Key::B)));
    CHECK(queue.Events().Size() == 2 && queue.DroppedCount() == 2);
    queue.Clear();
    CHECK(queue.Events().Size() == 0 && queue.Push(InputEvent::KeyDown(Key::B)));
}

TEST(Input_KeyNamesCoverEveryKey) {
    CHECK(strcmp(KeyName(Key::W), "W") == 0);
    CHECK(strcmp(KeyName(Key::NumpadEnter), "NumpadEnter") == 0);
    CHECK(strcmp(KeyName(Key::Count), "Unknown") == 0);
}

TEST(Input_RepeatDownForReleasedKeyCountsAsPress) {
    // 키를 누른 채 포커스를 되찾으면 OS의 첫 down이 반복 표시로 온다. 눌림·뗌 edge가 짝을 이뤄야
    // 한다.
    InputState state;
    Frame(state, {InputEvent::KeyDown(Key::W, true)});
    CHECK(state.IsDown(Key::W) && state.WasPressed(Key::W));
    Frame(state, {InputEvent::KeyUp(Key::W)});
    CHECK(state.WasReleased(Key::W));
}

TEST(Input_DeadZoneRejectsOutOfRangeValues) {
    InputState state;
    const float zero = 0.0f;
    GamepadDeadZone bad;
    bad.leftStick = -0.1f;
    CHECK(state.SetGamepadDeadZone(bad).Error() == ErrorCode::InvalidArgument);
    bad = {};
    bad.trigger = 1.0f;
    CHECK(state.SetGamepadDeadZone(bad).Error() == ErrorCode::InvalidArgument);
    bad = {};
    bad.rightStick = zero / zero;
    CHECK(state.SetGamepadDeadZone(bad).Error() == ErrorCode::InvalidArgument);
    GamepadDeadZone none{0.0f, 0.0f, 0.0f};
    REQUIRE(state.SetGamepadDeadZone(none));
    GamepadSnapshot pad;
    pad.connected = true;
    pad.axes[static_cast<uint32>(GamepadAxis::LeftX)] = 0.05f;
    Frame(state, {InputEvent::Gamepad(0, pad)});
    CHECK(Near(state.GamepadAxisValue(0, GamepadAxis::LeftX), 0.05f));
}
} // namespace input_state_tests_file

namespace input_touch_tests_file {

using namespace rocks::input;

namespace {

template <usize Count>
void Frame(InputState& state, const InputEvent (&events)[Count], bool lost = false) noexcept {
    state.BeginFrame(Span<const InputEvent>(events, Count), lost);
}

void Frame(InputState& state, bool lost = false) noexcept {
    state.BeginFrame({}, lost);
}

} // namespace

// 터치 edge는 한 프레임만 참이고, 위치·시작 위치·이동량·압력을
// 기록한다(Docs/Design/TouchInput.ko.md 3.3절).
TEST(Input_TouchBeganMovedEndedEdges) {
    InputState state;
    Frame(state, {InputEvent::TouchBegan(2, 10.0f, 20.0f, 0.5f, true)});
    CHECK(state.IsTouchDown(2) && state.WasTouchBegan(2) && !state.WasTouchEnded(2));
    CHECK(state.TouchX(2) == 10.0f && state.TouchY(2) == 20.0f && state.TouchStartX(2) == 10.0f);
    CHECK(state.TouchForce(2) == 0.5f && state.TouchHasForce(2) && state.TouchCount() == 1);
    Frame(state,
          {InputEvent::TouchMoved(2, 15.0f, 18.0f), InputEvent::TouchMoved(2, 17.0f, 25.0f)});
    CHECK(!state.WasTouchBegan(2) && state.TouchDeltaX(2) == 7.0f && state.TouchDeltaY(2) == 5.0f);
    CHECK(state.TouchStartX(2) == 10.0f && state.TouchStartY(2) == 20.0f);
    // 압력을 모르는 이동이면 1.0이고 지원 표지가 꺼진다.
    CHECK(state.TouchForce(2) == 1.0f && !state.TouchHasForce(2));
    Frame(state, {InputEvent::TouchEnded(2, 18.0f, 25.0f)});
    CHECK(!state.IsTouchDown(2) && state.WasTouchEnded(2) && !state.WasTouchCancelled(2));
    CHECK(state.TouchX(2) == 18.0f && state.TouchCount() == 0);
    Frame(state);
    CHECK(!state.WasTouchEnded(2) && state.TouchDeltaX(2) == 0.0f);
}

// 한 프레임에 시작과 끝이 모두 오면 둘 다 참이고 끝난 상태다. 범위 밖 번호·NaN 위치는 무시한다.
TEST(Input_TouchBeganAndEndedInOneFrameAndInvalidEvents) {
    InputState state;
    Frame(state, {InputEvent::TouchBegan(0, 1.0f, 1.0f), InputEvent::TouchEnded(0, 2.0f, 1.0f)});
    CHECK(state.WasTouchBegan(0) && state.WasTouchEnded(0) && !state.IsTouchDown(0));
    volatile float zero = 0.0f;
    const float notANumber = zero / zero;
    Frame(state, {InputEvent::TouchBegan(static_cast<uint8>(MaxTouches), 1.0f, 1.0f),
                  InputEvent::TouchBegan(1, notANumber, 1.0f)});
    CHECK(state.TouchCount() == 0 && !state.WasTouchBegan(1));
    CHECK(!state.IsTouchDown(MaxTouches) && !state.WasTouchBegan(MaxTouches + 5));
}

// 시작 없이 오는 이동·끝은 무시한다(require-release).
TEST(Input_TouchWithoutBeganIsIgnored) {
    InputState state;
    Frame(state, {InputEvent::TouchMoved(3, 5.0f, 5.0f), InputEvent::TouchEnded(3, 6.0f, 6.0f)});
    CHECK(!state.IsTouchDown(3) && !state.WasTouchEnded(3) && state.TouchX(3) == 0.0f);
}

// 포커스 상실과 큐 손실은 닿아 있는 손가락을 모두 취소된 끝으로 만들고, 그 뒤의 이동·끝은 무시한다.
TEST(Input_TouchFocusLostAndQueueLossCancel) {
    InputState state;
    Frame(state, {InputEvent::TouchBegan(0, 1.0f, 1.0f), InputEvent::TouchBegan(1, 2.0f, 2.0f)});
    Frame(state, {InputEvent::FocusLost()});
    CHECK(state.WasTouchEnded(0) && state.WasTouchCancelled(0) && state.WasTouchCancelled(1));
    CHECK(state.TouchCount() == 0);

    Frame(state, {InputEvent::TouchBegan(4, 1.0f, 1.0f), InputEvent::KeyDown(Key::W)});
    Frame(state, true);
    CHECK(state.WasTouchEnded(4) && state.WasTouchCancelled(4) && !state.IsTouchDown(4));
    CHECK(!state.IsDown(Key::W) && state.WasReleased(Key::W));
    // 버려진 뗌 뒤에 OS가 보낸 이동·끝은 시작이 없으므로 무시한다.
    Frame(state, {InputEvent::TouchMoved(4, 9.0f, 9.0f), InputEvent::TouchEnded(4, 9.0f, 9.0f)});
    CHECK(!state.WasTouchEnded(4) && state.TouchX(4) == 1.0f);
    Frame(state, {InputEvent::TouchBegan(4, 3.0f, 3.0f)});
    CHECK(state.WasTouchBegan(4) && state.IsTouchDown(4));
}

// 마우스로 터치 흉내: 왼쪽 버튼이 손가락 0이 되고 그 버튼의 마우스 edge는 없다. 다른 버튼은
// 그대로다.
TEST(Input_MouseTouchEmulation) {
    InputState state;
    state.SetMouseTouchEmulation(true);
    Frame(state, {InputEvent::MouseMove(30.0f, 40.0f), InputEvent::MouseDown(MouseButton::Left)});
    CHECK(state.WasTouchBegan(0) && state.TouchX(0) == 30.0f && state.TouchY(0) == 40.0f);
    CHECK(!state.WasPressed(MouseButton::Left) && !state.IsDown(MouseButton::Left));
    Frame(state, {InputEvent::MouseMove(35.0f, 40.0f), InputEvent::MouseDown(MouseButton::Right)});
    CHECK(state.TouchDeltaX(0) == 5.0f && state.WasPressed(MouseButton::Right));
    CHECK(state.MouseX() == 35.0f);
    Frame(state, {InputEvent::MouseUp(MouseButton::Left)});
    CHECK(state.WasTouchEnded(0) && !state.WasTouchCancelled(0) &&
          !state.WasReleased(MouseButton::Left));

    // 흉내를 끄면 흉내 중이던 터치는 다음 프레임에 취소로 끝나고, 왼쪽 버튼은 다시 마우스다.
    Frame(state, {InputEvent::MouseDown(MouseButton::Left)});
    state.SetMouseTouchEmulation(false);
    Frame(state, {InputEvent::MouseDown(MouseButton::Left)});
    CHECK(state.WasTouchEnded(0) && state.WasTouchCancelled(0) &&
          state.WasPressed(MouseButton::Left));
}

// 손가락 번호 배정: 가장 작은 빈 번호, 끝난 번호는 EndFrame까지 잡아 둠, 11번째는 버림.
TEST(Input_TouchIndexAllocator) {
    TouchIndexAllocator allocator;
    CHECK(allocator.Acquire(1000) == 0 && allocator.Acquire(2000) == 1 &&
          allocator.Acquire(1000) == 0);
    CHECK(allocator.Release(1000) == 0 && allocator.Find(1000) == NoTouchIndex);
    // 끝난 0번은 프레임 경계 전에 다시 주지 않는다.
    CHECK(allocator.Acquire(3000) == 2);
    allocator.EndFrame();
    CHECK(allocator.Acquire(4000) == 0);
    for (int64 pointer = 5000; pointer < 5000 + static_cast<int64>(MaxTouches) - 3; ++pointer) {
        CHECK(allocator.Acquire(pointer) != NoTouchIndex);
    }

    CHECK(allocator.Acquire(9999) == NoTouchIndex && allocator.DroppedCount() == 1);
    CHECK(allocator.Find(9999) == NoTouchIndex && allocator.Release(9999) == NoTouchIndex);
    uint8 ended[MaxTouches];
    CHECK(allocator.ReleaseAll(ended) == MaxTouches && ended[0] == 0 &&
          ended[MaxTouches - 1] == MaxTouches - 1);
    CHECK(allocator.Find(2000) == NoTouchIndex && allocator.Acquire(1) == NoTouchIndex);
    allocator.EndFrame();
    CHECK(allocator.Acquire(1) == 0);
    CHECK(StringView(KeyName(Key::Back)) == StringView("Back"));
}

} // namespace input_touch_tests_file
