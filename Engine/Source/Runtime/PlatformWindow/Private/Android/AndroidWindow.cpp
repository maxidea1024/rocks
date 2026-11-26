#include <Base/PlacementNew.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Memory/UniquePtr.h>
#include <Launch/AndroidActivity.h>
#include <Platform/Atomics.h>
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

// Android 창이다(Docs/Design/AndroidPlatform.ko.md 4·5절). 앱의 그 창 하나이며, Launch 액티비티
// 층의 listener로 창·포커스·입력 큐 콜백(UI 스레드)을 받는다. 입력은 이 창이 띄운 입력 스레드의
// looper에서 읽어 고정 크기 큐에 넣고, 게임 스레드의 PumpEvents가 호스트 InputQueue로 옮긴다.
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

// 입력 스레드가 모으고 GamepadBackend::Poll(게임 스레드)이 읽는 패드 상태다(Docs/Design/
// AndroidPlatform.ko.md 5절). 패드는 입력 장치 ID 순서로 가장 작은 빈 슬롯에 앉는다. 연결 해제
// 감지는 InputManager 리스너(Java)가 필요해 후속이다 — 그때까지 앉은 패드는 연결된 채로 남는다.
class AndroidGamepadStore {
public:
    // 장치의 슬롯이다. 처음 보는 장치면 빈 슬롯을 준다. 자리가 없으면 MaxGamepads다.
    uint32 SlotLocked(int32_t deviceId) noexcept {
        for (uint32 slot = 0; slot < input::MaxGamepads; ++slot) {
            if (pads_[slot].used && pads_[slot].deviceId == deviceId) {
                return slot;
            }
        }

        for (uint32 slot = 0; slot < input::MaxGamepads; ++slot) {
            if (!pads_[slot].used) {
                pads_[slot].used = true;
                pads_[slot].deviceId = deviceId;
                pads_[slot].state.connected = true;
                return slot;
            }
        }

        return input::MaxGamepads;
    }

    void SetButton(int32_t deviceId, input::GamepadButton button, bool down) noexcept {
        LockGuard guard(mutex_);
        const uint32 slot = SlotLocked(deviceId);
        if (slot == input::MaxGamepads) {
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
    void SetAxes(int32_t deviceId, const AInputEvent* event) noexcept {
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
        const uint32 slot = SlotLocked(deviceId);
        if (slot == input::MaxGamepads) {
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

    // 바뀐 패드를 큐에 넣는다. 폴링 사이에 눌렀다 뗀 버튼은 "눌림 → 현재" 두 상태로 넣어 edge를
    // 잃지 않는다(입력 스레드는 이벤트로 받지만 backend 계약은 폴링이다).
    void Poll(input::InputQueue& queue,
              input::GamepadSnapshot (&last)[input::MaxGamepads]) noexcept {
        LockGuard guard(mutex_);
        for (uint32 slot = 0; slot < input::MaxGamepads; ++slot) {
            Pad& pad = pads_[slot];
            if (!pad.used) {
                continue;
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
    }

private:
    // HAT 축이 방향 버튼으로 눌렸다고 보는 경계다.
    static constexpr float HatThreshold = 0.5f;

    struct Pad {
        bool used = false;
        int32_t deviceId = 0;
        uint32 latched = 0;
        input::GamepadSnapshot state;
    };

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

    Mutex mutex_;
    Pad pads_[input::MaxGamepads];
};

// 프로세스에 하나다(창과 GamepadBackend가 같이 쓴다). 파괴하지 않는다.
AndroidGamepadStore& GamepadStore() noexcept {
    alignas(AndroidGamepadStore) static unsigned char storage[sizeof(AndroidGamepadStore)];
    static AndroidGamepadStore* store = ::new (storage) AndroidGamepadStore();
    return *store;
}

// 게임 스레드의 패드 backend다(Poll이 저장소를 읽는다).
class AndroidGamepads final : public GamepadBackend {
public:
    void Poll(input::InputQueue& queue) noexcept override {
        GamepadStore().Poll(queue, last_);
    }

private:
    input::GamepadSnapshot last_[input::MaxGamepads];
};

bool IsGamepadSource(int32_t source) noexcept {
    return (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD ||
           (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK;
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

    void PumpEvents(input::InputQueue& queue) noexcept override {
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
        LockGuard guard(mutex_);
        if (window_ == window) {
            UpdateSize(window);
        }
    }

    void OnFocusChanged(bool focused) noexcept override {
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
            return;
        }

        pending_[pendingCount_++] = event;
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

        self.looperReady_.Signal();
        while (AtomicLoad(&self.stopInput_) == 0) {
            int events = 0;
            void* data = nullptr;
            const int ident = ALooper_pollOnce(-1, nullptr, &events, &data);
            self.ServiceQueueRequests();
            if (ident == LooperIdInput) {
                self.DrainInputQueue();
            }
        }

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
                    GamepadStore().SetButton(AInputEvent_getDeviceId(event), button,
                                             keyAction == AKEY_EVENT_ACTION_DOWN);
                }

                return EventHandled;
            }
        }

        if (type == AINPUT_EVENT_TYPE_MOTION &&
            (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
            if ((AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK) ==
                AMOTION_EVENT_ACTION_MOVE) {
                GamepadStore().SetAxes(AInputEvent_getDeviceId(event), event);
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
    // 손가락 번호 배정이다(입력 스레드가 배정하고 게임 스레드 PumpEvents가 프레임 경계를 알린다).
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
    uint32 closing_ = 0;
    uint32 stopInput_ = 0;
    // 입력 스레드만 만진다(looper_는 Start에서 준비된 뒤 읽기만 한다).
    Thread inputThread_;
    ALooper* looper_ = nullptr;
    AInputQueue* queue_ = nullptr;
};

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

    // 직접 그린 창 테두리는 데스크톱 Windows만 지원한다.
    if (desc.frame != WindowFrame::System) {
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

DesktopSessionState QueryDesktopSessionState() noexcept {
    return DesktopSessionState::Unknown;
}

} // namespace rocks::platform
