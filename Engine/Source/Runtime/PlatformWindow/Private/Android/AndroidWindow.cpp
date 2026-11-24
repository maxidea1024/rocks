#include <Core/Diagnostics/Log.h>
#include <Core/Memory/UniquePtr.h>
#include <Launch/AndroidActivity.h>
#include <Platform/Atomics.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <PlatformWindow/Window.h>

#include <android/input.h>
#include <android/keycodes.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>

// Android 창이다(Docs/Design/AndroidPlatform.ko.md 4·5절). 앱의 그 창 하나이며, Launch 액티비티
// 층의 listener로 창·포커스·입력 큐 콜백(UI 스레드)을 받는다. 입력은 이 창이 띄운 입력 스레드의
// looper에서 읽어 고정 크기 큐에 넣고, 게임 스레드의 PumpEvents가 호스트 InputQueue로 옮긴다.
//
// 터치는 TouchInput TI1(터치 이벤트)이 오기 전까지 첫 손가락을 마우스 왼쪽 버튼으로 넘긴다(UE
// bTouchFallbackToMouse와 같은 방향의 임시 경로 — 문서 5절).
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
    default:
        return input::Key::None;
    }
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
    // 넘긴다. Back은 소비한다(UE와 같다 — 앱을 끝내지 않는다). Key::Back은 TouchInput TI1에서
    // 더한다.
    int Convert(const AInputEvent* event) noexcept {
        const int32_t type = AInputEvent_getType(event);
        if (type == AINPUT_EVENT_TYPE_KEY) {
            const int32_t code = AKeyEvent_getKeyCode(event);
            if (code == AKEYCODE_BACK) {
                return EventHandled;
            }

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

        // 임시 경로: 첫 손가락(포인터 index 0)을 마우스 왼쪽 버튼으로 넘긴다(TI1 전까지).
        const int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
        const float x = AMotionEvent_getX(event, 0);
        const float y = AMotionEvent_getY(event, 0);
        LockGuard guard(mutex_);
        switch (action) {
        case AMOTION_EVENT_ACTION_DOWN:
            PushLocked(input::InputEvent::MouseMove(x, y));
            PushLocked(input::InputEvent::MouseDown(input::MouseButton::Left));
            break;
        case AMOTION_EVENT_ACTION_MOVE:
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

        return EventHandled;
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
    AInputQueue* detachRequest_ = nullptr;
    // 창 파괴 gate다. 게임 스레드가 AcknowledgeSurfaceLost로, 소멸자가 closing으로 연다.
    ManualResetEvent surfaceReleased_{true};
    ManualResetEvent detached_{true};
    ManualResetEvent looperReady_;
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
Result<UniquePtr<GamepadBackend>> GamepadBackend::Create(Allocator&) noexcept {
    return Result<UniquePtr<GamepadBackend>>::Failure(ErrorCode::Unsupported);
}

DesktopSessionState QueryDesktopSessionState() noexcept {
    return DesktopSessionState::Unknown;
}

} // namespace rocks::platform
