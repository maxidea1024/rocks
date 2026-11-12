#include <Core/Memory/SystemAllocator.h>
#include <Input/InputState.h>
#include <TestSupport/Test.h>
#include <string.h>

using namespace rocks;
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
