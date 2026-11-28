#include <Base/PlacementNew.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Hash/TypeHash.h>
#include <Core/Memory/UniquePtr.h>
#include <Launch/AndroidActivity.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <Input/TouchIndexAllocator.h>
#include <PlatformWindow/Window.h>

#include <android/input.h>
#include <android/keycodes.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <jni.h>

#include "../Common/GamepadMotorMapping.h"
#include "../Common/GamepadSlotTable.h"

// Android 창이다(Docs/Design/AndroidPlatform.ko.md 4·5절). 앱의 그 창 하나이며, Launch 액티비티
// 층의 listener로 창·포커스·입력 큐 콜백(UI 스레드)을 받는다. 입력은 이 창이 띄운 입력 스레드의
// looper에서 읽어 고정 크기 큐에 넣고, 게임 스레드의 TakeEvents가 호스트 InputQueue로 옮긴다. 넣을
// 때마다 펌프 수준 대기(WaitForMessages)를 깨운다(Docs/Design/UiToolkit 19.2절).
//
// 터치는 손가락마다 TouchBegan·TouchMoved·TouchEnded로 넘기고, 손가락 번호는 TouchIndexAllocator가
// OS pointerId에 배정한다(Docs/Design/TouchInput.ko.md 3·4.2절). 마우스(AINPUT_SOURCE_MOUSE)는
// 마우스 이벤트다.
namespace rocks::platform {

namespace {

inline constexpr LogCategory LogAndroidWindow{"Platform.Window"};

// 입력 스레드가 게임 스레드 펌프 전까지 모아 두는 이벤트 수다. 넘치면 새 이벤트를 버리고 다음 펌프
// 끝에 FocusLost를 넣는다(macOS와 같은 복구).
constexpr usize PendingEventCapacity = 1024;

// 창 파괴 gate의 제한 시간이다(UE 옛 창 동작의 EMDoneTrigger->Wait(4000)과 같다 — AD3). UI 스레드가
// 막혀 있으므로 무제한으로 기다리지 않는다.
constexpr uint32 SurfaceReleaseTimeoutMilliseconds = 4000;

// looper ident다.
constexpr int LooperIdInput = 1;

// 빈 프레임 색이다(RGBA8 little-endian의 불투명 검정 — A가 가장 높은 byte).
constexpr uint32 OpaqueBlackRgba = 0xFF000000u;

constexpr char InputThreadName[] = "InputThread";

// 압력 지원 여부를 기억하는 입력 장치 수다(넘치면 가장 오래된 칸부터 덮는다).
constexpr uint32 MaxPressureDevices = 8;

// 패드 진동 JNI 호출 한 번이 만드는 지역 참조 수의 상한이다(PushLocalFrame).
constexpr jint VibrationLocalReferences = 32;

// 패드 하나에서 쓰는 진동자 수다(UE는 첫 두 진동자를 왼쪽·오른쪽으로 쓴다 — 6.4절).
constexpr uint32 MaxPadVibrators = 2;

// MonotonicNanoseconds를 진동·장치 확인 시각(ms)으로 바꾼다.
constexpr uint64 NanosecondsPerMillisecond = 1000000;

// 패드 연결 해제를 확인하는 간격이다(XInput 빈 슬롯 확인과 같다 — AndroidPlatform 5절, AD12).
constexpr uint64 GamepadDeviceCheckIntervalMilliseconds = 1000;

// 장치 확인·descriptor 읽기 JNI 호출 한 번이 만드는 지역 참조 수의 상한이다(PushLocalFrame).
constexpr jint DeviceQueryLocalReferences = 8;

// 앉지 못한 패드 장치 ID(표가 꽉 참·이미 없는 장치)를 기억하는 수다. 그 장치의 이벤트마다 JNI를
// 다시 하지 않는다. 넘치면 가장 오래된 칸부터 덮는다.
constexpr uint32 MaxRejectedGamepadDevices = 8;

// 입력 이벤트를 처리했다고(1) 또는 시스템에 넘긴다고(0) AInputQueue_finishEvent에 알리는 값이다.
constexpr int EventHandled = 1;
constexpr int EventNotHandled = 0;

input::Key KeyFromKeyCode(int32_t code) noexcept {
    if (code >= AKEYCODE_A && code <= AKEYCODE_Z) {
        return static_cast<input::Key>(static_cast<uint32>(input::Key::A) +
                                       static_cast<uint32>(code - AKEYCODE_A));
    }

    if (code >= AKEYCODE_0 && code <= AKEYCODE_9) {
        return static_cast<input::Key>(static_cast<uint32>(input::Key::Digit0) +
                                       static_cast<uint32>(code - AKEYCODE_0));
    }

    switch (code) {
    case AKEYCODE_SPACE:
        return input::Key::Space;
    case AKEYCODE_ENTER:
        return input::Key::Enter;
    case AKEYCODE_DEL:
        return input::Key::Backspace;
    case AKEYCODE_FORWARD_DEL:
        return input::Key::Delete;
    case AKEYCODE_TAB:
        return input::Key::Tab;
    case AKEYCODE_ESCAPE:
        return input::Key::Escape;
    case AKEYCODE_DPAD_UP:
        return input::Key::Up;
    case AKEYCODE_DPAD_DOWN:
        return input::Key::Down;
    case AKEYCODE_DPAD_LEFT:
        return input::Key::Left;
    case AKEYCODE_DPAD_RIGHT:
        return input::Key::Right;
    case AKEYCODE_SHIFT_LEFT:
        return input::Key::LeftShift;
    case AKEYCODE_SHIFT_RIGHT:
        return input::Key::RightShift;
    case AKEYCODE_CTRL_LEFT:
        return input::Key::LeftControl;
    case AKEYCODE_CTRL_RIGHT:
        return input::Key::RightControl;
    case AKEYCODE_ALT_LEFT:
        return input::Key::LeftAlt;
    case AKEYCODE_ALT_RIGHT:
        return input::Key::RightAlt;
    case AKEYCODE_BACK:
        // 뒤로 가기는 소비하고 키 이벤트로만 넘긴다(앱을 끝내지 않는다 — UE와 같다).
        return input::Key::Back;
    default:
        return input::Key::None;
    }
}

// 패드 버튼 비트다.
uint32 ButtonBit(input::GamepadButton button) noexcept {
    return 1u << static_cast<uint32>(button);
}

// 게임패드 키코드 → 버튼(UE AndroidGameControllerMapper 표준 배치). 패드 버튼이 아니면 false다.
bool GamepadButtonFromKeyCode(int32_t code, input::GamepadButton& button) noexcept {
    switch (code) {
    case AKEYCODE_BUTTON_A:
        button = input::GamepadButton::South;
        return true;
    case AKEYCODE_BUTTON_B:
        button = input::GamepadButton::East;
        return true;
    case AKEYCODE_BUTTON_X:
        button = input::GamepadButton::West;
        return true;
    case AKEYCODE_BUTTON_Y:
        button = input::GamepadButton::North;
        return true;
    case AKEYCODE_BUTTON_L1:
        button = input::GamepadButton::LeftShoulder;
        return true;
    case AKEYCODE_BUTTON_R1:
        button = input::GamepadButton::RightShoulder;
        return true;
    case AKEYCODE_BUTTON_SELECT:
        button = input::GamepadButton::Back;
        return true;
    case AKEYCODE_BUTTON_START:
        button = input::GamepadButton::Start;
        return true;
    case AKEYCODE_BUTTON_THUMBL:
        button = input::GamepadButton::LeftThumb;
        return true;
    case AKEYCODE_BUTTON_THUMBR:
        button = input::GamepadButton::RightThumb;
        return true;
    case AKEYCODE_DPAD_UP:
        button = input::GamepadButton::DPadUp;
        return true;
    case AKEYCODE_DPAD_DOWN:
        button = input::GamepadButton::DPadDown;
        return true;
    case AKEYCODE_DPAD_LEFT:
        button = input::GamepadButton::DPadLeft;
        return true;
    case AKEYCODE_DPAD_RIGHT:
        button = input::GamepadButton::DPadRight;
        return true;
    default:
        return false;
    }
}

// 입력 스레드가 처리할 패드 진동 쓰기 하나다(Docs/Design/InputActions 6.4절).
struct VibrationRequest {
    bool pending = false;
    int32_t deviceId = 0;
    gamepad::AndroidVibratorPair intensities;
};

// 입력 스레드가 모으고 GamepadBackend::Poll(게임 스레드)이 읽는 패드 상태다(Docs/Design/
// AndroidPlatform.ko.md 5절). 슬롯 배정은 GamepadSlotTable이다(같은 descriptor는 다시 연결돼도 같은
// 슬롯). 연결 해제는 게임 스레드 Poll이 1초마다 확인을 요청하면 입력 스레드가 JNI 장치 목록으로
// 판정한다(AD12).
//
// 진동(InputActions 6.4절): 게임 스레드는 JavaVM에 붙어 있지 않으므로 SetMotors가 슬롯의 요청 칸에
// 쓰고 입력 스레드 looper를 깨운다. JavaVM에 붙은 입력 스레드가 요청을 가져가 JNI로 쓴다.
class AndroidGamepadStore {
public:
    // 연결된 그 장치의 슬롯이다(없으면 NoGamepadSlot — 입력 스레드가 descriptor를 읽어 Seat한다).
    uint32 FindSlot(int32_t deviceId) const noexcept {
        LockGuard guard(mutex_);
        return table_.Find(deviceId);
    }

    // 장치를 앉힌다(GamepadSlotTable 규칙). 새로 앉은 슬롯(이어받기 포함)은 빈 상태·새 세대로
    // 시작한다. 자리가 없으면 NoGamepadSlot이다.
    uint32 Seat(int32_t deviceId, uint64 descriptorHash) noexcept {
        LockGuard guard(mutex_);
        const bool known = table_.Find(deviceId) != gamepad::NoGamepadSlot;
        const uint32 slot = table_.Seat(deviceId, descriptorHash);
        if (slot != gamepad::NoGamepadSlot && !known) {
            ResetPadLocked(slot);
            pads_[slot].state.connected = true;
        }

        return slot;
    }

    void SetButton(uint32 slot, input::GamepadButton button, bool down) noexcept {
        LockGuard guard(mutex_);
        if (!table_.IsConnected(slot)) {
            return;
        }

        Pad& pad = pads_[slot];
        if (down) {
            pad.state.buttons |= ButtonBit(button);
            pad.latched |= ButtonBit(button);
        } else {
            pad.state.buttons &= ~ButtonBit(button);
        }
    }

    // 축과 HAT(방향 버튼)을 갱신한다. 스틱 Y는 화면 아래가 +라 뒤집는다(위가 +).
    void SetAxes(uint32 slot, const AInputEvent* event) noexcept {
        auto axis = [event](int32_t code) noexcept {
            return AMotionEvent_getAxisValue(event, code, 0);
        };
        const float leftX = axis(AMOTION_EVENT_AXIS_X);
        const float leftY = -axis(AMOTION_EVENT_AXIS_Y);
        const float rightX = axis(AMOTION_EVENT_AXIS_Z);
        const float rightY = -axis(AMOTION_EVENT_AXIS_RZ);
        const float leftTrigger =
            Max(axis(AMOTION_EVENT_AXIS_LTRIGGER), axis(AMOTION_EVENT_AXIS_BRAKE));
        const float rightTrigger =
            Max(axis(AMOTION_EVENT_AXIS_RTRIGGER), axis(AMOTION_EVENT_AXIS_GAS));
        const float hatX = axis(AMOTION_EVENT_AXIS_HAT_X);
        const float hatY = axis(AMOTION_EVENT_AXIS_HAT_Y);
        LockGuard guard(mutex_);
        if (!table_.IsConnected(slot)) {
            return;
        }

        Pad& pad = pads_[slot];
        float* axes = pad.state.axes;
        axes[static_cast<uint32>(input::GamepadAxis::LeftX)] = leftX;
        axes[static_cast<uint32>(input::GamepadAxis::LeftY)] = leftY;
        axes[static_cast<uint32>(input::GamepadAxis::RightX)] = rightX;
        axes[static_cast<uint32>(input::GamepadAxis::RightY)] = rightY;
        axes[static_cast<uint32>(input::GamepadAxis::LeftTrigger)] = leftTrigger;
        axes[static_cast<uint32>(input::GamepadAxis::RightTrigger)] = rightTrigger;
        const uint32 hatMask =
            ButtonBit(input::GamepadButton::DPadUp) | ButtonBit(input::GamepadButton::DPadDown) |
            ButtonBit(input::GamepadButton::DPadLeft) | ButtonBit(input::GamepadButton::DPadRight);
        uint32 hat = 0;
        hat |= hatX < -HatThreshold ? ButtonBit(input::GamepadButton::DPadLeft) : 0u;
        hat |= hatX > HatThreshold ? ButtonBit(input::GamepadButton::DPadRight) : 0u;
        hat |= hatY < -HatThreshold ? ButtonBit(input::GamepadButton::DPadUp) : 0u;
        hat |= hatY > HatThreshold ? ButtonBit(input::GamepadButton::DPadDown) : 0u;
        pad.latched |= hat & ~pad.state.buttons;
        pad.state.buttons = (pad.state.buttons & ~hatMask) | hat;
    }

    // 진동·장치 확인을 맡을 입력 스레드 looper다(JavaVM에 붙은 입력 스레드가 시작할 때 걸고 끝날 때
    // 뗀다 — nullptr이면 진동·해제 감지 없음).
    void SetInputLooper(ALooper* looper) noexcept {
        LockGuard guard(mutex_);
        inputLooper_ = looper;
    }

    // 장치 확인을 요청하고 입력 스레드를 깨운다(게임 스레드 Poll — 연결된 패드가 없거나 입력
    // 스레드가 없으면 하지 않는다).
    void RequestDeviceCheck() noexcept {
        LockGuard guard(mutex_);
        if (inputLooper_ == nullptr || !table_.AnyConnected()) {
            return;
        }

        deviceCheckPending_ = true;
        ALooper_wake(inputLooper_);
    }

    // 장치 확인 요청을 가져간다(입력 스레드).
    bool TakeDeviceCheck() noexcept {
        LockGuard guard(mutex_);
        const bool pending = deviceCheckPending_;
        deviceCheckPending_ = false;
        return pending;
    }

    // 목록에 없는 연결된 패드를 끊는다(상태를 비워 다음 Poll이 connected = false를 낸다). 끊은 슬롯
    // 비트다.
    uint32 DisconnectMissing(Span<const int32> connectedIds) noexcept {
        LockGuard guard(mutex_);
        const uint32 lost = table_.DisconnectMissing(connectedIds);
        for (uint32 slot = 0; slot < input::MaxGamepads; ++slot) {
            if ((lost & (1u << slot)) != 0) {
                ResetPadLocked(slot);
            }
        }

        return lost;
    }

    // 앱 창 포커스다(UI 스레드 OnFocusChanged가 쓴다 — 포커스가 없으면 진동을 끈다).
    void SetFocused(bool focused) noexcept {
        LockGuard guard(mutex_);
        focused_ = focused;
    }

    bool Focused() const noexcept {
        LockGuard guard(mutex_);
        return focused_;
    }

    // 그 슬롯에 진동을 쓸 수 있는지다: 연결된 패드가 없으면 NotFound, 진동을 쓸 입력 스레드가
    // 없으면(창 없음·입력 스레드의 JavaVM 붙이기 실패) Unsupported, 되면 None이다.
    ErrorCode VibrationAvailability(uint32 slot) const noexcept {
        LockGuard guard(mutex_);
        if (!table_.IsConnected(slot)) {
            return ErrorCode::NotFound;
        }

        return inputLooper_ != nullptr ? ErrorCode::None : ErrorCode::Unsupported;
    }

    // 그 슬롯의 진동 요청을 두고(처리 전 요청은 덮는다) 입력 스레드를 깨운다. 연결된 패드나 입력
    // 스레드가 없으면 아무것도 하지 않는다.
    void RequestVibration(uint32 slot, gamepad::AndroidVibratorPair intensities) noexcept {
        LockGuard guard(mutex_);
        if (!table_.IsConnected(slot) || inputLooper_ == nullptr) {
            return;
        }

        VibrationRequest& request = vibrationRequests_[slot];
        request.pending = true;
        request.deviceId = table_.DeviceId(slot);
        request.intensities = intensities;
        ALooper_wake(inputLooper_);
    }

    // 처리할 요청을 out에 옮기고 비운다(입력 스레드 — JNI는 잠금 밖에서 한다). 옮긴 수다.
    uint32 TakeVibrationRequests(VibrationRequest (&out)[input::MaxGamepads]) noexcept {
        LockGuard guard(mutex_);
        uint32 count = 0;
        for (uint32 slot = 0; slot < input::MaxGamepads; ++slot) {
            if (vibrationRequests_[slot].pending) {
                out[count++] = vibrationRequests_[slot];
                vibrationRequests_[slot] = VibrationRequest{};
            }
        }

        return count;
    }

    // 바뀐 패드를 큐에 넣는다(끊김 포함 — 끊긴 슬롯은 빈 상태라 한 번 connected = false를 낸다).
    // 세대가 바뀌었는데(끊긴 뒤 다시 앉음·이어받기가 이번 Poll 사이에 겹침) 지난번에 연결로 냈으면
    // 끊김을 먼저 한 번 낸다. 폴링 사이에 눌렀다 뗀 버튼은 "눌림 → 현재" 두 상태로 넣어 edge를 잃지
    // 않는다(입력 스레드는 이벤트로 받지만 backend 계약은 폴링이다). 세대가 바뀐 슬롯 비트다.
    uint32 Poll(input::InputQueue& queue, input::GamepadSnapshot (&last)[input::MaxGamepads],
                uint32 (&seenGenerations)[input::MaxGamepads]) noexcept {
        LockGuard guard(mutex_);
        uint32 changed = 0;
        for (uint32 slot = 0; slot < input::MaxGamepads; ++slot) {
            Pad& pad = pads_[slot];
            if (pad.generation != seenGenerations[slot]) {
                seenGenerations[slot] = pad.generation;
                changed |= 1u << slot;
                if (last[slot].connected && pad.state.connected) {
                    (void)queue.Push(input::InputEvent::Gamepad(static_cast<uint8>(slot),
                                                                input::GamepadSnapshot{}));
                    last[slot] = input::GamepadSnapshot{};
                }
            }

            const uint32 missed = pad.latched & ~pad.state.buttons;
            pad.latched = 0;
            if (missed != 0) {
                input::GamepadSnapshot pressed = pad.state;
                pressed.buttons |= missed;
                (void)queue.Push(input::InputEvent::Gamepad(static_cast<uint8>(slot), pressed));
                last[slot] = pressed;
            }

            if (!Same(pad.state, last[slot])) {
                (void)queue.Push(input::InputEvent::Gamepad(static_cast<uint8>(slot), pad.state));
                last[slot] = pad.state;
            }
        }

        return changed;
    }

private:
    // HAT 축이 방향 버튼으로 눌렸다고 보는 경계다.
    static constexpr float HatThreshold = 0.5f;

    struct Pad {
        uint32 latched = 0;
        input::GamepadSnapshot state;
        // 앉음·끊김마다 늘어난다(Poll이 한 Poll 사이의 끊김·다시 앉음을 알아챈다).
        uint32 generation = 0;
    };

    // 슬롯을 빈 상태·새 세대로 되돌리고 진동 요청을 버린다(잠금 아래).
    void ResetPadLocked(uint32 slot) noexcept {
        const uint32 generation = pads_[slot].generation + 1;
        pads_[slot] = Pad{};
        pads_[slot].generation = generation;
        vibrationRequests_[slot] = VibrationRequest{};
    }

    static float Max(float left, float right) noexcept {
        return left > right ? left : right;
    }

    static bool Same(const input::GamepadSnapshot& left,
                     const input::GamepadSnapshot& right) noexcept {
        if (left.connected != right.connected || left.buttons != right.buttons) {
            return false;
        }

        for (uint32 axis = 0; axis < input::GamepadAxisCount; ++axis) {
            if (left.axes[axis] != right.axes[axis]) {
                return false;
            }
        }

        return true;
    }

    mutable Mutex mutex_;
    gamepad::GamepadSlotTable table_;
    Pad pads_[input::MaxGamepads];
    VibrationRequest vibrationRequests_[input::MaxGamepads];
    ALooper* inputLooper_ = nullptr;
    bool deviceCheckPending_ = false;
    bool focused_ = false;
};

// 프로세스에 하나다(창과 GamepadBackend가 같이 쓴다). 파괴하지 않는다.
AndroidGamepadStore& GamepadStore() noexcept {
    alignas(AndroidGamepadStore) static unsigned char storage[sizeof(AndroidGamepadStore)];
    static AndroidGamepadStore* store = ::new (storage) AndroidGamepadStore();
    return *store;
}

// 게임 스레드의 패드 backend다(Poll이 저장소를 읽는다). 진동은 저장소 요청 칸으로 입력 스레드에
// 넘긴다(InputActions 6.4절).
class AndroidGamepads final : public GamepadBackend {
public:
    AndroidGamepads() noexcept = default;
    AndroidGamepads(const AndroidGamepads&) = delete;
    AndroidGamepads& operator=(const AndroidGamepads&) = delete;

    // 0이 아닌 값을 쓴 슬롯을 멈춘다.
    ~AndroidGamepads() override {
        for (uint32 slot = 0; slot < input::MaxGamepads; ++slot) {
            if (!(vibration_[slot].written == gamepad::AndroidVibratorPair{})) {
                GamepadStore().RequestVibration(slot, gamepad::AndroidVibratorPair{});
            }
        }
    }

    // 1초마다 장치 확인을 요청하고(AD12) 바뀐 패드를 낸다. 끊긴 슬롯·세대가 바뀐 슬롯의 진동 상태는
    // 지운다(새 장치에는 처음부터 쓴다).
    void Poll(input::InputQueue& queue) noexcept override {
        AndroidGamepadStore& store = GamepadStore();
        const uint64 now = MonotonicNanoseconds() / NanosecondsPerMillisecond;
        if (!deviceCheckRequested_ ||
            now - lastDeviceCheckMilliseconds_ >= GamepadDeviceCheckIntervalMilliseconds) {
            store.RequestDeviceCheck();
            deviceCheckRequested_ = true;
            lastDeviceCheckMilliseconds_ = now;
        }

        const uint32 changed = store.Poll(queue, last_, seenGenerations_);
        for (uint32 slot = 0; slot < input::MaxGamepads; ++slot) {
            if ((changed & (1u << slot)) != 0 || !last_[slot].connected) {
                vibration_[slot] = gamepad::AndroidVibrationState{};
            }
        }
    }

    // 바뀐 세기(또는 1초가 지난 같은 세기)만 입력 스레드에 넘긴다. 쓰기 결과는 비동기라 넘기면
    // 성공이다.
    Result<void> SetMotors(uint32 index, const input::MotorValues& values) noexcept override {
        if (index >= input::MaxGamepads || !gamepad::MotorValuesAreFinite(values)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        AndroidGamepadStore& store = GamepadStore();
        if (const ErrorCode available = store.VibrationAvailability(index);
            available != ErrorCode::None) {
            return Result<void>::Failure(available);
        }

        const uint64 now = MonotonicNanoseconds() / NanosecondsPerMillisecond;
        gamepad::AndroidVibratorPair intensities;
        if (gamepad::NextAndroidVibration(vibration_[index], values, store.Focused(), now,
                                          intensities)) {
            store.RequestVibration(index, intensities);
        }

        return Result<void>::Success();
    }

private:
    input::GamepadSnapshot last_[input::MaxGamepads];
    gamepad::AndroidVibrationState vibration_[input::MaxGamepads];
    uint32 seenGenerations_[input::MaxGamepads] = {};
    uint64 lastDeviceCheckMilliseconds_ = 0;
    bool deviceCheckRequested_ = false;
};

bool IsGamepadSource(int32_t source) noexcept {
    return (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD ||
           (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK;
}

} // namespace

namespace {

// 펌프 수준 대기의 신호다. 입력 스레드·UI 스레드가 상태를 바꾸면 올리고, 게임 스레드의
// WaitForMessages가 지난 대기 뒤 신호가 없을 때만 기다린다(창 mutex → 이 잠금 순서).
Mutex messageGate;
ConditionVariable messageSignal;
uint64 messageSignals = 0;
uint64 consumedSignals = 0;

void SignalMessages() noexcept {
    {
        LockGuard guard(messageGate);
        ++messageSignals;
    }

    messageSignal.NotifyAll();
}

} // namespace

class AndroidWindow final : public Window, public launch::android::ActivityListener {
public:
    AndroidWindow() noexcept = default;

    ~AndroidWindow() override {
        // UI 스레드가 창 파괴 gate에서 기다리고 있으면 먼저 풀고(교착 방지), listener를 뗀다.
        AtomicStore(&closing_, uint32{1});
        surfaceReleased_.Signal();
        launch::android::SetActivityListener(nullptr);
        AtomicStore(&stopInput_, uint32{1});
        if (ALooper* looper = looper_; looper != nullptr) {
            ALooper_wake(looper);
        }

        if (inputThread_.IsJoinable()) {
            (void)inputThread_.Join();
        }

        if (window_ != nullptr) {
            ANativeWindow_release(window_);
            window_ = nullptr;
        }
    }

    // 입력 스레드를 띄우고(looper 준비를 기다린다) 액티비티 listener를 건다(지금 창·입력 큐를
    // 곧바로 받는다).
    Result<void> Start() noexcept {
        ThreadOptions options;
        options.name = InputThreadName;
        if (inputThread_.Start(&RunInputThread, this, options) != ThreadStatus::Success) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }

        (void)looperReady_.Wait();
        if (looper_ == nullptr) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }

        launch::android::SetActivityListener(this);
        return Result<void>::Success();
    }

    // --- Window(게임 스레드) ---

    void TakeEvents(input::InputQueue& queue) noexcept override {
        LockGuard guard(mutex_);
        for (usize index = 0; index < pendingCount_; ++index) {
            (void)queue.Push(pending_[index]);
        }

        pendingCount_ = 0;
        if (overflowed_) {
            (void)queue.Push(input::InputEvent::FocusLost());
            overflowed_ = false;
        }

        // 프레임 경계: 끝난 손가락 번호를 돌려받는다(이번에 옮긴 끝 이벤트의 번호는 다음 시작부터
        // 다시 쓴다 — TouchIndexAllocator).
        touches_.EndFrame();
    }

    bool IsCloseRequested() const noexcept override {
        return false;
    }

    void ClearCloseRequest() noexcept override {
    }

    uint32 Width() const noexcept override {
        LockGuard guard(mutex_);
        return width_;
    }

    uint32 Height() const noexcept override {
        LockGuard guard(mutex_);
        return height_;
    }

    bool ConsumeResize(uint32& width, uint32& height) noexcept override {
        LockGuard guard(mutex_);
        if (!resized_) {
            return false;
        }

        resized_ = false;
        width = width_;
        height = height_;
        return true;
    }

    bool HasFocus() const noexcept override {
        LockGuard guard(mutex_);
        return focused_;
    }

    // 창(surface)이 없는 동안은 최소화와 같다(present를 건너뛴다).
    bool IsMinimized() const noexcept override {
        LockGuard guard(mutex_);
        return window_ == nullptr;
    }

    NativeWindowHandle NativeHandle() const noexcept override {
        LockGuard guard(mutex_);
        NativeWindowHandle handle;
        handle.window = window_;
        handle.instance = launch::android::CurrentActivity();
        return handle;
    }

    // 전체 화면 앱 창이라 제목이 없다(최근 앱 목록 이름은 manifest label이다).
    Result<void> SetTitle(StringView title) noexcept override {
        return title.IsValidUtf8() ? Result<void>::Failure(ErrorCode::Unsupported)
                                   : Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    bool ConsumeSurfaceLost() noexcept override {
        LockGuard guard(mutex_);
        const bool lost = surfaceLostPending_;
        surfaceLostPending_ = false;
        return lost;
    }

    void AcknowledgeSurfaceLost() noexcept override {
        surfaceReleased_.Signal();
    }

    // CPU로 검은 프레임 한 장을 낸다(렌더러 없는 호스트 — Window.h 계약).
    void PresentBlankFrame() noexcept override {
        LockGuard guard(mutex_);
        if (window_ == nullptr) {
            return;
        }

        (void)ANativeWindow_setBuffersGeometry(window_, 0, 0,
                                               AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
        ANativeWindow_Buffer buffer{};
        if (ANativeWindow_lock(window_, &buffer, nullptr) != 0) {
            return;
        }

        auto* row = static_cast<uint32*>(buffer.bits);
        for (int32_t y = 0; y < buffer.height; ++y) {
            for (int32_t x = 0; x < buffer.width; ++x) {
                row[x] = OpaqueBlackRgba;
            }

            row += buffer.stride;
        }

        (void)ANativeWindow_unlockAndPost(window_);
    }

    // --- ActivityListener(UI 스레드) ---

    void OnWindowCreated(ANativeWindow* window) noexcept override {
        // 게임 lane의 펌프 수준 대기를 깨운다(창 상태가 바뀌었다).
        SignalMessages();
        ANativeWindow_acquire(window);
        LockGuard guard(mutex_);
        if (window_ != nullptr) {
            ANativeWindow_release(window_);
        }

        window_ = window;
        UpdateSize(window);
        // 새 surface다. 크기가 같아도 swapchain(또는 빈 프레임)을 다시 만들어야 하므로 알린다.
        resized_ = true;
    }

    // 창 파괴 gate(AD3): 게임 스레드가 surface를 놓았다고 알릴 때까지(최대 4초) 기다린 뒤 창을
    // 놓는다.
    void OnWindowDestroyed(ANativeWindow* window) noexcept override {
        // 게임 lane의 펌프 수준 대기를 깨운다(창 상태가 바뀌었다).
        SignalMessages();
        {
            LockGuard guard(mutex_);
            surfaceReleased_.Reset();
            surfaceLostPending_ = true;
        }

        if (AtomicLoad(&closing_) == 0 &&
            surfaceReleased_.Wait(SurfaceReleaseTimeoutMilliseconds) != WaitResult::Signaled) {
            Log::Error(LogAndroidWindow,
                       "The renderer did not release the window surface within {Timeout} ms",
                       SurfaceReleaseTimeoutMilliseconds);
        }

        LockGuard guard(mutex_);
        if (window_ == window) {
            ANativeWindow_release(window_);
            window_ = nullptr;
        }
    }

    void OnWindowResized(ANativeWindow* window) noexcept override {
        // 게임 lane의 펌프 수준 대기를 깨운다(창 상태가 바뀌었다).
        SignalMessages();
        LockGuard guard(mutex_);
        if (window_ == window) {
            UpdateSize(window);
        }
    }

    void OnFocusChanged(bool focused) noexcept override {
        // 게임 lane의 펌프 수준 대기를 깨운다(창 상태가 바뀌었다).
        SignalMessages();
        GamepadStore().SetFocused(focused);
        LockGuard guard(mutex_);
        focused_ = focused;
        if (!focused) {
            // 닿아 있던 손가락은 FocusLost가 InputState에서 취소로 끝낸다. 배정기도 같이 비운다.
            uint8 ended[input::MaxTouches];
            (void)touches_.ReleaseAll(ended);
            PushLocked(input::InputEvent::FocusLost());
        }
    }

    // 입력 큐 붙이기·떼기는 입력 스레드가 한다(looper를 도는 스레드에서 큐를 만지게). 떼기는 끝날
    // 때까지 기다린다(콜백이 돌아오면 큐가 사라진다).
    void OnInputQueueCreated(AInputQueue* queue) noexcept override {
        {
            LockGuard guard(mutex_);
            attachRequest_ = queue;
        }

        ALooper_wake(looper_);
    }

    void OnInputQueueDestroyed(AInputQueue* queue) noexcept override {
        {
            LockGuard guard(mutex_);
            detachRequest_ = queue;
            detached_.Reset();
        }

        ALooper_wake(looper_);
        (void)detached_.Wait();
    }

private:
    void UpdateSize(ANativeWindow* window) noexcept {
        const int32_t width = ANativeWindow_getWidth(window);
        const int32_t height = ANativeWindow_getHeight(window);
        const uint32 newWidth = width > 0 ? static_cast<uint32>(width) : 0u;
        const uint32 newHeight = height > 0 ? static_cast<uint32>(height) : 0u;
        if (newWidth != width_ || newHeight != height_) {
            width_ = newWidth;
            height_ = newHeight;
            resized_ = true;
        }
    }

    void PushLocked(const input::InputEvent& event) noexcept {
        if (pendingCount_ == PendingEventCapacity) {
            overflowed_ = true;
        } else {
            pending_[pendingCount_++] = event;
        }

        SignalMessages();
    }

    // 입력 스레드다. looper를 만들고, 붙이기·떼기 요청과 입력 큐 이벤트를 처리한다.
    static void RunInputThread(void* raw) noexcept {
        auto& self = *static_cast<AndroidWindow*>(raw);
        // 입력 큐는 콜백 없이 ident로 등록하므로 non-callback fd를 허용하는 looper여야 한다(UE
        // LaunchAndroid.cpp:970과 같다). 없으면 AInputQueue_attachLooper의 addFd가 실패해 입력이
        // 오지 않는다.
        self.looper_ = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
        // 압력 지원 판정(InputDevice JNI)을 위해 이 스레드를 JavaVM에 붙인다. 실패하면 압력을
        // 모르는 것으로 둔다.
        ANativeActivity* activity = launch::android::CurrentActivity();
        if (activity != nullptr &&
            activity->vm->AttachCurrentThread(&self.env_, nullptr) != JNI_OK) {
            self.env_ = nullptr;
        }

        // 진동·패드 해제 확인은 JNI가 있어야 한다(InputActions 6.4절, AD12).
        if (self.env_ != nullptr) {
            GamepadStore().SetInputLooper(self.looper_);
        }

        self.looperReady_.Signal();
        while (AtomicLoad(&self.stopInput_) == 0) {
            int events = 0;
            void* data = nullptr;
            const int ident = ALooper_pollOnce(-1, nullptr, &events, &data);
            self.ServiceQueueRequests();
            self.ServiceDeviceCheck();
            self.ServiceVibrationRequests();
            if (ident == LooperIdInput) {
                self.DrainInputQueue();
            }
        }

        // 남은 요청(backend 소멸의 0 포함)을 쓰고 뗀다.
        GamepadStore().SetInputLooper(nullptr);
        self.ServiceVibrationRequests();

        // 종료 때 붙어 있던 큐를 뗀다.
        if (self.queue_ != nullptr) {
            AInputQueue_detachLooper(self.queue_);
            self.queue_ = nullptr;
        }

        if (self.env_ != nullptr) {
            (void)activity->vm->DetachCurrentThread();
            self.env_ = nullptr;
        }

        self.detached_.Signal();
    }

    void ServiceQueueRequests() noexcept {
        AInputQueue* attach = nullptr;
        AInputQueue* detach = nullptr;
        {
            LockGuard guard(mutex_);
            attach = attachRequest_;
            detach = detachRequest_;
            attachRequest_ = nullptr;
            detachRequest_ = nullptr;
        }

        if (detach != nullptr) {
            if (queue_ == detach) {
                AInputQueue_detachLooper(queue_);
                queue_ = nullptr;
            }

            detached_.Signal();
        }

        if (attach != nullptr && attach != queue_) {
            if (queue_ != nullptr) {
                AInputQueue_detachLooper(queue_);
            }

            queue_ = attach;
            AInputQueue_attachLooper(queue_, looper_, LooperIdInput, nullptr, nullptr);
        }
    }

    void DrainInputQueue() noexcept {
        if (queue_ == nullptr) {
            return;
        }

        AInputEvent* event = nullptr;
        while (AInputQueue_getEvent(queue_, &event) >= 0) {
            if (AInputQueue_preDispatchEvent(queue_, event) != 0) {
                continue;
            }

            AInputQueue_finishEvent(queue_, event, Convert(event));
        }
    }

    // 입력 이벤트 하나를 큐에 넣고, 처리했으면 EventHandled다. 볼륨 키 등 모르는 키는 시스템에
    // 넘긴다. Back은 Key::Back으로 넘기고 소비한다(UE와 같다 — 앱을 끝내지 않는다).
    int Convert(const AInputEvent* event) noexcept {
        const int32_t type = AInputEvent_getType(event);
        const int32_t source = AInputEvent_getSource(event);
        // 게임패드 버튼·축은 패드 저장소로 간다(GamepadBackend::Poll이 읽는다).
        if (type == AINPUT_EVENT_TYPE_KEY && IsGamepadSource(source)) {
            input::GamepadButton button = input::GamepadButton::South;
            if (GamepadButtonFromKeyCode(AKeyEvent_getKeyCode(event), button)) {
                const int32_t keyAction = AKeyEvent_getAction(event);
                if (keyAction == AKEY_EVENT_ACTION_DOWN || keyAction == AKEY_EVENT_ACTION_UP) {
                    if (const uint32 slot = GamepadSlotFor(AInputEvent_getDeviceId(event));
                        slot != gamepad::NoGamepadSlot) {
                        GamepadStore().SetButton(slot, button, keyAction == AKEY_EVENT_ACTION_DOWN);
                    }
                }

                return EventHandled;
            }
        }

        if (type == AINPUT_EVENT_TYPE_MOTION &&
            (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
            if ((AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK) ==
                AMOTION_EVENT_ACTION_MOVE) {
                if (const uint32 slot = GamepadSlotFor(AInputEvent_getDeviceId(event));
                    slot != gamepad::NoGamepadSlot) {
                    GamepadStore().SetAxes(slot, event);
                }
            }

            return EventHandled;
        }

        if (type == AINPUT_EVENT_TYPE_KEY) {
            const int32_t code = AKeyEvent_getKeyCode(event);
            const input::Key key = KeyFromKeyCode(code);
            if (key == input::Key::None) {
                return EventNotHandled;
            }

            const int32_t action = AKeyEvent_getAction(event);
            LockGuard guard(mutex_);
            if (action == AKEY_EVENT_ACTION_DOWN) {
                PushLocked(input::InputEvent::KeyDown(key, AKeyEvent_getRepeatCount(event) > 0));
            } else if (action == AKEY_EVENT_ACTION_UP) {
                PushLocked(input::InputEvent::KeyUp(key));
            }

            return EventHandled;
        }

        if (type != AINPUT_EVENT_TYPE_MOTION) {
            return EventNotHandled;
        }

        // 압력 지원 여부는 잠금 밖에서 정한다(처음 보는 장치면 JNI를 부른다).
        const bool hasForce = DeviceReportsPressure(AInputEvent_getDeviceId(event));
        const int32_t action = AMotionEvent_getAction(event);
        const int32_t kind = action & AMOTION_EVENT_ACTION_MASK;
        const auto pointer =
            static_cast<size_t>((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                                AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        LockGuard guard(mutex_);
        if ((AInputEvent_getSource(event) & AINPUT_SOURCE_MOUSE) == AINPUT_SOURCE_MOUSE) {
            ConvertMouse(event, kind);
            return EventHandled;
        }

        switch (kind) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN: {
            const uint8 index = touches_.Acquire(AMotionEvent_getPointerId(event, pointer));
            if (index != input::NoTouchIndex) {
                PushLocked(input::InputEvent::TouchBegan(index, AMotionEvent_getX(event, pointer),
                                                         AMotionEvent_getY(event, pointer),
                                                         Pressure(event, pointer), hasForce));
            }

            break;
        }
        case AMOTION_EVENT_ACTION_MOVE: {
            // 이 이벤트의 모든 포인터 이동이다(history 샘플은 최신 값만 쓴다 — UE와 같다).
            const size_t count = AMotionEvent_getPointerCount(event);
            for (size_t item = 0; item < count; ++item) {
                const uint8 index = touches_.Find(AMotionEvent_getPointerId(event, item));
                if (index != input::NoTouchIndex) {
                    PushLocked(input::InputEvent::TouchMoved(index, AMotionEvent_getX(event, item),
                                                             AMotionEvent_getY(event, item),
                                                             Pressure(event, item), hasForce));
                }
            }

            break;
        }
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP: {
            const uint8 index = touches_.Release(AMotionEvent_getPointerId(event, pointer));
            if (index != input::NoTouchIndex) {
                PushLocked(input::InputEvent::TouchEnded(index, AMotionEvent_getX(event, pointer),
                                                         AMotionEvent_getY(event, pointer)));
            }

            break;
        }
        case AMOTION_EVENT_ACTION_CANCEL: {
            // OS가 제스처를 가로챘다. 닿아 있던 손가락을 모두 취소로 끝낸다.
            uint8 ended[input::MaxTouches];
            const uint32 count = touches_.ReleaseAll(ended);
            const float x = AMotionEvent_getX(event, 0);
            const float y = AMotionEvent_getY(event, 0);
            for (uint32 item = 0; item < count; ++item) {
                PushLocked(input::InputEvent::TouchEnded(ended[item], x, y, true));
            }

            break;
        }
        default:
            // ACTION_OUTSIDE는 시작이 없던 포인터라 무시한다(TouchInput TD15).
            break;
        }

        return EventHandled;
    }

    // 마우스(데스크톱 마우스를 붙인 기기·에뮬레이터 마우스 모드)는 왼쪽 버튼 마우스 이벤트다.
    void ConvertMouse(const AInputEvent* event, int32_t kind) noexcept {
        const float x = AMotionEvent_getX(event, 0);
        const float y = AMotionEvent_getY(event, 0);
        switch (kind) {
        case AMOTION_EVENT_ACTION_DOWN:
            PushLocked(input::InputEvent::MouseMove(x, y));
            PushLocked(input::InputEvent::MouseDown(input::MouseButton::Left));
            break;
        case AMOTION_EVENT_ACTION_MOVE:
        case AMOTION_EVENT_ACTION_HOVER_MOVE:
            PushLocked(input::InputEvent::MouseMove(x, y));
            break;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_CANCEL:
            PushLocked(input::InputEvent::MouseMove(x, y));
            PushLocked(input::InputEvent::MouseUp(input::MouseButton::Left));
            break;
        default:
            break;
        }
    }

    // 입력 장치가 압력을 보고하는지다(TouchInput 3.1절 hasForce). 처음 보는 장치는
    // InputDevice.getDevice(id).getMotionRange(AXIS_PRESSURE)를 JNI로 묻고, 범위가 있고 폭이 0보다
    // 크면 지원으로 본다. 입력 스레드만 부른다(결과 칸·env_는 입력 스레드 전용).
    bool DeviceReportsPressure(int32_t deviceId) noexcept {
        for (uint32 slot = 0; slot < pressureDeviceCount_; ++slot) {
            if (pressureDevices_[slot].deviceId == deviceId) {
                return pressureDevices_[slot].reportsPressure;
            }
        }

        const bool reports = QueryDevicePressure(deviceId);
        PressureDevice& entry = pressureDeviceCount_ < MaxPressureDevices
                                    ? pressureDevices_[pressureDeviceCount_++]
                                    : pressureDevices_[pressureDeviceNext_++ % MaxPressureDevices];
        entry.deviceId = deviceId;
        entry.reportsPressure = reports;
        return reports;
    }

    bool QueryDevicePressure(int32_t deviceId) noexcept {
        JNIEnv* env = env_;
        constexpr jint LocalReferences = 8;
        if (env == nullptr || env->PushLocalFrame(LocalReferences) != JNI_OK) {
            return false;
        }

        bool reports = false;
        jclass deviceClass = env->FindClass("android/view/InputDevice");
        jmethodID getDevice =
            deviceClass != nullptr
                ? env->GetStaticMethodID(deviceClass, "getDevice", "(I)Landroid/view/InputDevice;")
                : nullptr;
        jmethodID getMotionRange =
            deviceClass != nullptr ? env->GetMethodID(deviceClass, "getMotionRange",
                                                      "(I)Landroid/view/InputDevice$MotionRange;")
                                   : nullptr;
        jobject device = getDevice != nullptr
                             ? env->CallStaticObjectMethod(deviceClass, getDevice, deviceId)
                             : nullptr;
        jobject range = device != nullptr && getMotionRange != nullptr
                            ? env->CallObjectMethod(device, getMotionRange,
                                                    static_cast<jint>(AMOTION_EVENT_AXIS_PRESSURE))
                            : nullptr;
        if (range != nullptr && !env->ExceptionCheck()) {
            jmethodID getRange = env->GetMethodID(env->GetObjectClass(range), "getRange", "()F");
            reports = getRange != nullptr && env->CallFloatMethod(range, getRange) > 0.0f;
        }

        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            reports = false;
        }

        (void)env->PopLocalFrame(nullptr);
        return reports;
    }

    // 패드 이벤트의 슬롯이다. 처음 보는 장치는 descriptor를 읽어 앉힌다(AD12). 장치가 이미 없으면
    // (해제 뒤 늦게 온 이벤트) 앉히지 않고 NoGamepadSlot이다. JNI가 없으면 descriptor 없이 앉힌다.
    uint32 GamepadSlotFor(int32_t deviceId) noexcept {
        AndroidGamepadStore& store = GamepadStore();
        if (const uint32 slot = store.FindSlot(deviceId); slot != gamepad::NoGamepadSlot) {
            return slot;
        }

        for (uint32 index = 0; index < rejectedDeviceCount_; ++index) {
            if (rejectedDevices_[index] == deviceId) {
                return gamepad::NoGamepadSlot;
            }
        }

        uint64 descriptorHash = gamepad::UnknownDescriptorHash;
        const uint32 slot = ReadDescriptorHash(deviceId, descriptorHash)
                                ? store.Seat(deviceId, descriptorHash)
                                : gamepad::NoGamepadSlot;
        if (slot == gamepad::NoGamepadSlot) {
            RememberRejectedDevice(deviceId);
            return slot;
        }

        Log::Info(LogAndroidWindow, "Gamepad {Slot} connected (input device {Device})", slot,
                  deviceId);
        return slot;
    }

    // 앉지 못한 장치를 기억한다(슬롯이 비면 — 끊김 — 잊는다).
    void RememberRejectedDevice(int32_t deviceId) noexcept {
        if (rejectedDeviceCount_ < MaxRejectedGamepadDevices) {
            rejectedDevices_[rejectedDeviceCount_++] = deviceId;
            return;
        }

        rejectedDevices_[rejectedDeviceNext_] = deviceId;
        rejectedDeviceNext_ = (rejectedDeviceNext_ + 1) % MaxRejectedGamepadDevices;
    }

    // InputDevice.getDescriptor()의 해시를 읽는다. 장치가 없으면(getDevice가 null) false다. JNI가
    // 없거나 읽지 못하면(예외) 해시는 모름이고 true다.
    bool ReadDescriptorHash(int32_t deviceId, uint64& hash) noexcept {
        hash = gamepad::UnknownDescriptorHash;
        JNIEnv* env = env_;
        if (env == nullptr || env->PushLocalFrame(DeviceQueryLocalReferences) != JNI_OK) {
            return true;
        }

        bool exists = true;
        jclass deviceClass = env->FindClass("android/view/InputDevice");
        jmethodID getDevice =
            deviceClass != nullptr && !env->ExceptionCheck()
                ? env->GetStaticMethodID(deviceClass, "getDevice", "(I)Landroid/view/InputDevice;")
                : nullptr;
        jobject device = getDevice != nullptr && !env->ExceptionCheck()
                             ? env->CallStaticObjectMethod(deviceClass, getDevice, deviceId)
                             : nullptr;
        if (!env->ExceptionCheck() && getDevice != nullptr && device == nullptr) {
            exists = false;
        } else if (device != nullptr && !env->ExceptionCheck()) {
            jmethodID getDescriptor =
                env->GetMethodID(deviceClass, "getDescriptor", "()Ljava/lang/String;");
            auto descriptor =
                getDescriptor != nullptr && !env->ExceptionCheck()
                    ? static_cast<jstring>(env->CallObjectMethod(device, getDescriptor))
                    : nullptr;
            if (descriptor != nullptr && !env->ExceptionCheck()) {
                const char* bytes = env->GetStringUTFChars(descriptor, nullptr);
                if (bytes != nullptr) {
                    const auto length = static_cast<usize>(env->GetStringUTFLength(descriptor));
                    const uint64 value = HashStringBytes(StringView(bytes, length));
                    // 0은 "모름"이라 실제 해시가 0이면 1로 둔다.
                    hash = value == gamepad::UnknownDescriptorHash ? value + 1 : value;
                    env->ReleaseStringUTFChars(descriptor, bytes);
                }
            }
        }

        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            hash = gamepad::UnknownDescriptorHash;
            exists = true;
        }

        (void)env->PopLocalFrame(nullptr);
        return exists;
    }

    // 게임 스레드가 요청했으면 지금 장치 ID 목록(InputDevice.getDeviceIds)에 없는 패드를 끊는다
    // (AD12). 목록을 읽지 못하면 아무것도 끊지 않는다.
    void ServiceDeviceCheck() noexcept {
        if (!GamepadStore().TakeDeviceCheck()) {
            return;
        }

        JNIEnv* env = env_;
        if (env == nullptr || env->PushLocalFrame(DeviceQueryLocalReferences) != JNI_OK) {
            return;
        }

        jclass deviceClass = env->FindClass("android/view/InputDevice");
        jmethodID getDeviceIds = deviceClass != nullptr && !env->ExceptionCheck()
                                     ? env->GetStaticMethodID(deviceClass, "getDeviceIds", "()[I")
                                     : nullptr;
        auto ids =
            getDeviceIds != nullptr && !env->ExceptionCheck()
                ? static_cast<jintArray>(env->CallStaticObjectMethod(deviceClass, getDeviceIds))
                : nullptr;
        if (ids != nullptr && !env->ExceptionCheck()) {
            const jsize count = env->GetArrayLength(ids);
            jint* values = env->GetIntArrayElements(ids, nullptr);
            if (values != nullptr) {
                const uint32 lost = GamepadStore().DisconnectMissing(
                    Span<const int32>(values, static_cast<usize>(count)));
                env->ReleaseIntArrayElements(ids, values, JNI_ABORT);
                if (lost != 0) {
                    // 자리가 났으니 앉지 못했던 장치를 다시 받아 본다.
                    rejectedDeviceCount_ = 0;
                    rejectedDeviceNext_ = 0;
                }

                for (uint32 slot = 0; slot < input::MaxGamepads; ++slot) {
                    if ((lost & (1u << slot)) != 0) {
                        Log::Info(LogAndroidWindow, "Gamepad {Slot} disconnected", slot);
                    }
                }
            }
        }

        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }

        (void)env->PopLocalFrame(nullptr);
    }

    // 저장소의 진동 요청을 JNI로 쓴다(입력 스레드).
    void ServiceVibrationRequests() noexcept {
        VibrationRequest requests[input::MaxGamepads];
        const uint32 count = GamepadStore().TakeVibrationRequests(requests);
        for (uint32 index = 0; index < count; ++index) {
            WriteVibrators(requests[index].deviceId, requests[index].intensities);
        }
    }

    // 패드의 진동자에 쓴다(UE GameActivity AndroidThunkJava_SetInputDeviceVibrators — 6.4절).
    // getVibratorManager(API 31)가 있으면 진동자가 하나면 max, 둘 이상이면 첫째 왼쪽·둘째 오른쪽,
    // 없으면 getVibrator 하나에 max다. 장치·진동자가 없거나 Java 예외면 버린다(예외는 지우고 한 번
    // 경고한다). 예외가 남은 채로 다음 JNI를 부르지 않는다(CheckJNI abort) — 던질 수 있는 호출마다
    // 확인하고 멈춘다.
    void WriteVibrators(int32_t deviceId, gamepad::AndroidVibratorPair intensities) noexcept {
        JNIEnv* env = env_;
        if (env == nullptr || env->PushLocalFrame(VibrationLocalReferences) != JNI_OK) {
            return;
        }

        WriteDeviceVibrators(env, deviceId, intensities);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            if (!vibrationWarned_) {
                vibrationWarned_ = true;
                Log::Warning(LogAndroidWindow,
                             "Gamepad vibration failed on the Java side; the request was dropped");
            }
        }

        (void)env->PopLocalFrame(nullptr);
    }

    // WriteVibrators의 본문이다. 예외가 나면 남긴 채 돌아온다(호출자가 지운다).
    static void WriteDeviceVibrators(JNIEnv* env, int32_t deviceId,
                                     gamepad::AndroidVibratorPair intensities) noexcept {
        const uint8 larger =
            intensities.left > intensities.right ? intensities.left : intensities.right;
        jclass deviceClass = env->FindClass("android/view/InputDevice");
        if (deviceClass == nullptr || env->ExceptionCheck()) {
            return;
        }

        jmethodID getDevice =
            env->GetStaticMethodID(deviceClass, "getDevice", "(I)Landroid/view/InputDevice;");
        if (getDevice == nullptr || env->ExceptionCheck()) {
            return;
        }

        jobject device = env->CallStaticObjectMethod(deviceClass, getDevice, deviceId);
        if (device == nullptr || env->ExceptionCheck()) {
            return;
        }

        jmethodID getManager =
            env->GetMethodID(deviceClass, "getVibratorManager", "()Landroid/os/VibratorManager;");
        if (getManager != nullptr) {
            jobject manager = env->CallObjectMethod(device, getManager);
            if (manager != nullptr && !env->ExceptionCheck()) {
                WriteManagedVibrators(env, manager, intensities, larger);
            }

            return;
        }

        // API 31 미만(NoSuchMethodError) — 진동자 하나다.
        env->ExceptionClear();
        jmethodID getVibrator =
            env->GetMethodID(deviceClass, "getVibrator", "()Landroid/os/Vibrator;");
        if (getVibrator == nullptr || env->ExceptionCheck()) {
            return;
        }

        jobject vibrator = env->CallObjectMethod(device, getVibrator);
        if (vibrator != nullptr && !env->ExceptionCheck()) {
            ApplyVibrator(env, vibrator, larger);
        }
    }

    // VibratorManager의 진동자에 쓴다(하나면 larger, 둘 이상이면 첫째 왼쪽·둘째 오른쪽 — 셋째부터는
    // 쓰지 않는다).
    static void WriteManagedVibrators(JNIEnv* env, jobject manager,
                                      gamepad::AndroidVibratorPair intensities,
                                      uint8 larger) noexcept {
        jclass managerClass = env->GetObjectClass(manager);
        jmethodID getIds = env->GetMethodID(managerClass, "getVibratorIds", "()[I");
        if (getIds == nullptr || env->ExceptionCheck()) {
            return;
        }

        jmethodID getVibrator =
            env->GetMethodID(managerClass, "getVibrator", "(I)Landroid/os/Vibrator;");
        if (getVibrator == nullptr || env->ExceptionCheck()) {
            return;
        }

        auto ids = static_cast<jintArray>(env->CallObjectMethod(manager, getIds));
        if (ids == nullptr || env->ExceptionCheck()) {
            return;
        }

        const jsize count = env->GetArrayLength(ids);
        if (count <= 0) {
            return;
        }

        jint vibratorIds[MaxPadVibrators] = {};
        const jsize used = count < static_cast<jsize>(MaxPadVibrators)
                               ? count
                               : static_cast<jsize>(MaxPadVibrators);
        env->GetIntArrayRegion(ids, 0, used, vibratorIds);
        if (env->ExceptionCheck()) {
            return;
        }

        const uint8 strengths[MaxPadVibrators] = {used == 1 ? larger : intensities.left,
                                                  intensities.right};
        for (jsize index = 0; index < used; ++index) {
            jobject vibrator = env->CallObjectMethod(manager, getVibrator, vibratorIds[index]);
            if (env->ExceptionCheck()) {
                return;
            }

            if (vibrator != nullptr) {
                ApplyVibrator(env, vibrator, strengths[index]);
                if (env->ExceptionCheck()) {
                    return;
                }
            }
        }
    }

    // 세기가 1 이상이면 1초 one-shot, 0이면 cancel이다(UE VibrateRunnable). vibrator는 null이
    // 아니고 예외가 없는 상태에서 부른다.
    static void ApplyVibrator(JNIEnv* env, jobject vibrator, uint8 intensity) noexcept {
        jclass vibratorClass = env->GetObjectClass(vibrator);
        if (intensity == 0) {
            jmethodID cancel = env->GetMethodID(vibratorClass, "cancel", "()V");
            if (cancel != nullptr && !env->ExceptionCheck()) {
                env->CallVoidMethod(vibrator, cancel);
            }

            return;
        }

        jclass effectClass = env->FindClass("android/os/VibrationEffect");
        if (effectClass == nullptr || env->ExceptionCheck()) {
            return;
        }

        jmethodID createOneShot = env->GetStaticMethodID(effectClass, "createOneShot",
                                                         "(JI)Landroid/os/VibrationEffect;");
        if (createOneShot == nullptr || env->ExceptionCheck()) {
            return;
        }

        jmethodID vibrate =
            env->GetMethodID(vibratorClass, "vibrate", "(Landroid/os/VibrationEffect;)V");
        if (vibrate == nullptr || env->ExceptionCheck()) {
            return;
        }

        jobject effect = env->CallStaticObjectMethod(
            effectClass, createOneShot,
            static_cast<jlong>(gamepad::AndroidVibrationPulseMilliseconds),
            static_cast<jint>(intensity));
        if (effect != nullptr && !env->ExceptionCheck()) {
            env->CallVoidMethod(vibrator, vibrate, effect);
        }
    }

    // 0–1 압력이다(AMotionEvent_getPressure — 장치 단위가 1을 넘을 수 있어 자른다).
    static float Pressure(const AInputEvent* event, size_t pointer) noexcept {
        const float pressure = AMotionEvent_getPressure(event, pointer);
        return pressure < 0.0f ? 0.0f : (pressure > 1.0f ? 1.0f : pressure);
    }

    // 창 상태·이벤트 큐·요청을 지킨다(UI 스레드·입력 스레드가 쓰고 게임 스레드가 읽는다).
    mutable Mutex mutex_;
    input::InputEvent pending_[PendingEventCapacity];
    usize pendingCount_ = 0;
    bool overflowed_ = false;
    bool resized_ = false;
    bool focused_ = false;
    bool surfaceLostPending_ = false;
    uint32 width_ = 0;
    uint32 height_ = 0;
    ANativeWindow* window_ = nullptr;
    AInputQueue* attachRequest_ = nullptr;
    // 손가락 번호 배정이다(입력 스레드가 배정하고 게임 스레드 TakeEvents가 프레임 경계를 알린다).
    input::TouchIndexAllocator touches_;
    AInputQueue* detachRequest_ = nullptr;
    // 창 파괴 gate다. 게임 스레드가 AcknowledgeSurfaceLost로, 소멸자가 closing으로 연다.
    ManualResetEvent surfaceReleased_{true};
    ManualResetEvent detached_{true};
    ManualResetEvent looperReady_;
    // 입력 장치별 압력 지원 여부와 JNI env다(입력 스레드 전용).
    struct PressureDevice {
        int32_t deviceId = 0;
        bool reportsPressure = false;
    };

    PressureDevice pressureDevices_[MaxPressureDevices];
    uint32 pressureDeviceCount_ = 0;
    uint32 pressureDeviceNext_ = 0;
    JNIEnv* env_ = nullptr;
    // 진동 Java 예외 경고를 한 번 냈다(입력 스레드 전용).
    bool vibrationWarned_ = false;
    // 앉지 못한 패드 장치 ID다(입력 스레드 전용 — GamepadSlotFor).
    int32_t rejectedDevices_[MaxRejectedGamepadDevices] = {};
    uint32 rejectedDeviceCount_ = 0;
    uint32 rejectedDeviceNext_ = 0;
    uint32 closing_ = 0;
    uint32 stopInput_ = 0;
    // 입력 스레드만 만진다(looper_는 Start에서 준비된 뒤 읽기만 한다).
    Thread inputThread_;
    ALooper* looper_ = nullptr;
    AInputQueue* queue_ = nullptr;
};

bool Window::SupportsFrame(WindowFrame frame) noexcept {
    // 직접 그린 창 테두리는 데스크톱 Windows만 지원한다.
    return frame == WindowFrame::System;
}

Result<UniquePtr<Window>> Window::Create(Allocator& allocator, const WindowDesc& desc,
                                         WindowCreateReport* report) noexcept {
    auto fail = [report](ErrorCode error) noexcept {
        if (report != nullptr) {
            *report = WindowCreateReport{};
            report->code = error;
        }

        return Result<UniquePtr<Window>>::Failure(error);
    };

    if (desc.width == 0 || desc.height == 0 || !desc.title.IsValidUtf8()) {
        return fail(ErrorCode::InvalidArgument);
    }

    if (!SupportsFrame(desc.frame)) {
        return fail(ErrorCode::Unsupported);
    }

    // APK가 아닌 실행 파일(adb 시험)에는 액티비티가 없다.
    if (launch::android::CurrentActivity() == nullptr) {
        return fail(ErrorCode::Unsupported);
    }

    auto created = UniquePtr<AndroidWindow>::TryMake(allocator);
    if (!created) {
        return fail(created.Error());
    }

    if (auto started = created.Value()->Start(); !started) {
        return fail(started.Error());
    }

    if (report != nullptr) {
        *report = WindowCreateReport{};
    }

    return Result<UniquePtr<Window>>::Success(UniquePtr<Window>(Move(created.Value())));
}

// 게임패드는 AP5(Android 패드)에서 붙인다.
// 게임패드는 창의 입력 스레드가 모은 패드 저장소를 읽는다(창이 있어야 입력이 온다 — APK).
Result<UniquePtr<GamepadBackend>> GamepadBackend::Create(Allocator& allocator) noexcept {
    if (launch::android::CurrentActivity() == nullptr) {
        return Result<UniquePtr<GamepadBackend>>::Failure(ErrorCode::Unsupported);
    }

    auto created = UniquePtr<AndroidGamepads>::TryMake(allocator);
    if (!created) {
        return Result<UniquePtr<GamepadBackend>>::Failure(created.Error());
    }

    return Result<UniquePtr<GamepadBackend>>::Success(
        UniquePtr<GamepadBackend>(Move(created.Value())));
}

// Android에는 다른 스레드가 보낸 창 메시지가 없다(입력은 입력 스레드가 넘기고 TakeEvents가 옮긴다).
// 이 플랫폼의 DPI는 창 backend가 따로 정한다(Android density — 그 플랫폼 단계).
bool EnableHighDpiAwareness() noexcept {
    return false;
}

void PumpSentMessages() noexcept {
}

// OS 메시지는 입력 스레드·UI 스레드가 받아 창 큐에 넘기므로 게임 lane이 펌프할 것이 없다.
void PumpMessages() noexcept {
}

void WaitForMessages(uint32 milliseconds) noexcept {
    LockGuard guard(messageGate);
    if (messageSignals == consumedSignals) {
        (void)messageSignal.Wait(messageGate, milliseconds);
    }

    consumedSignals = messageSignals;
}

bool HasPendingWindowEvents() noexcept {
    LockGuard guard(messageGate);
    return messageSignals != consumedSignals;
}

void WakeMessageWait() noexcept {
    SignalMessages();
}

DesktopSessionState QueryDesktopSessionState() noexcept {
    return DesktopSessionState::Unknown;
}

} // namespace rocks::platform
