#include <Platform/Clock.h>
#include <Platform/DynamicLibrary.h>
#include <Platform/Win64/WideString.h>
#include <Input/TouchIndexAllocator.h>
#include <PlatformWindow/Window.h>

#include "../Common/GamepadMotorMapping.h"
#include "../Common/PointerTouchMapping.h"

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>
#include <WtsApi32.h>
#include <Xinput.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

namespace rocks::platform {

namespace {

using input::Key;

constexpr wchar_t WindowClassName[] = L"RocksWindow";
constexpr usize MaxTitleCharacters = 256;

// 스캔코드 set 1(make code) → Key. 확장 키(E0 접두)는 ExtendedKey가 처리한다.
Key BaseKey(uint32 scanCode) noexcept {
    switch (scanCode) {
    case 0x01:
        return Key::Escape;
    case 0x02:
        return Key::Digit1;
    case 0x03:
        return Key::Digit2;
    case 0x04:
        return Key::Digit3;
    case 0x05:
        return Key::Digit4;
    case 0x06:
        return Key::Digit5;
    case 0x07:
        return Key::Digit6;
    case 0x08:
        return Key::Digit7;
    case 0x09:
        return Key::Digit8;
    case 0x0A:
        return Key::Digit9;
    case 0x0B:
        return Key::Digit0;
    case 0x0C:
        return Key::Minus;
    case 0x0D:
        return Key::Equals;
    case 0x0E:
        return Key::Backspace;
    case 0x0F:
        return Key::Tab;
    case 0x10:
        return Key::Q;
    case 0x11:
        return Key::W;
    case 0x12:
        return Key::E;
    case 0x13:
        return Key::R;
    case 0x14:
        return Key::T;
    case 0x15:
        return Key::Y;
    case 0x16:
        return Key::U;
    case 0x17:
        return Key::I;
    case 0x18:
        return Key::O;
    case 0x19:
        return Key::P;
    case 0x1A:
        return Key::LeftBracket;
    case 0x1B:
        return Key::RightBracket;
    case 0x1C:
        return Key::Enter;
    case 0x1D:
        return Key::LeftControl;
    case 0x1E:
        return Key::A;
    case 0x1F:
        return Key::S;
    case 0x20:
        return Key::D;
    case 0x21:
        return Key::F;
    case 0x22:
        return Key::G;
    case 0x23:
        return Key::H;
    case 0x24:
        return Key::J;
    case 0x25:
        return Key::K;
    case 0x26:
        return Key::L;
    case 0x27:
        return Key::Semicolon;
    case 0x28:
        return Key::Apostrophe;
    case 0x29:
        return Key::Grave;
    case 0x2A:
        return Key::LeftShift;
    case 0x2B:
        return Key::Backslash;
    case 0x2C:
        return Key::Z;
    case 0x2D:
        return Key::X;
    case 0x2E:
        return Key::C;
    case 0x2F:
        return Key::V;
    case 0x30:
        return Key::B;
    case 0x31:
        return Key::N;
    case 0x32:
        return Key::M;
    case 0x33:
        return Key::Comma;
    case 0x34:
        return Key::Period;
    case 0x35:
        return Key::Slash;
    case 0x36:
        return Key::RightShift;
    case 0x37:
        return Key::NumpadMultiply;
    case 0x38:
        return Key::LeftAlt;
    case 0x39:
        return Key::Space;
    case 0x3A:
        return Key::CapsLock;
    case 0x3B:
        return Key::F1;
    case 0x3C:
        return Key::F2;
    case 0x3D:
        return Key::F3;
    case 0x3E:
        return Key::F4;
    case 0x3F:
        return Key::F5;
    case 0x40:
        return Key::F6;
    case 0x41:
        return Key::F7;
    case 0x42:
        return Key::F8;
    case 0x43:
        return Key::F9;
    case 0x44:
        return Key::F10;
    case 0x47:
        return Key::Numpad7;
    case 0x48:
        return Key::Numpad8;
    case 0x49:
        return Key::Numpad9;
    case 0x4A:
        return Key::NumpadSubtract;
    case 0x4B:
        return Key::Numpad4;
    case 0x4C:
        return Key::Numpad5;
    case 0x4D:
        return Key::Numpad6;
    case 0x4E:
        return Key::NumpadAdd;
    case 0x4F:
        return Key::Numpad1;
    case 0x50:
        return Key::Numpad2;
    case 0x51:
        return Key::Numpad3;
    case 0x52:
        return Key::Numpad0;
    case 0x53:
        return Key::NumpadDecimal;
    case 0x57:
        return Key::F11;
    case 0x58:
        return Key::F12;
    default:
        return Key::None;
    }
}

Key ExtendedKey(uint32 scanCode) noexcept {
    switch (scanCode) {
    case 0x1C:
        return Key::NumpadEnter;
    case 0x1D:
        return Key::RightControl;
    case 0x35:
        return Key::NumpadDivide;
    case 0x38:
        return Key::RightAlt;
    case 0x47:
        return Key::Home;
    case 0x48:
        return Key::Up;
    case 0x49:
        return Key::PageUp;
    case 0x4B:
        return Key::Left;
    case 0x4D:
        return Key::Right;
    case 0x4F:
        return Key::End;
    case 0x50:
        return Key::Down;
    case 0x51:
        return Key::PageDown;
    case 0x52:
        return Key::Insert;
    case 0x53:
        return Key::Delete;
    case 0x5B:
        return Key::LeftSuper;
    case 0x5C:
        return Key::RightSuper;
    default:
        return Key::None;
    }
}

// lParam의 스캔코드(16–23비트)와 확장 플래그(24비트)로 물리 키를 정한다.
Key KeyFromMessage(LPARAM lParam) noexcept {
    const uint32 scanCode = static_cast<uint32>((lParam >> 16) & 0xFF);
    const bool extended = ((lParam >> 24) & 1) != 0;
    return extended ? ExtendedKey(scanCode) : BaseKey(scanCode);
}

// 창 제목 UTF-8 → UTF-16(널 종료). 넘치면 글자 경계에서 자른다. 잘못된 UTF-8이면 거짓이다.
bool ToWide(StringView text, wchar_t (&buffer)[MaxTitleCharacters]) noexcept {
    return static_cast<bool>(windows::ConvertUtf8ToWide(
        text.Data(), text.ByteCount(), buffer, MaxTitleCharacters, windows::TextFit::Truncate));
}

class Win64Window final : public Window {
public:
    Win64Window() noexcept = default;

    ~Win64Window() override {
        if (window_ != nullptr) {
            SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
            DestroyWindow(window_);
        }
    }

    Result<void> Create(const WindowDesc& desc) noexcept {
        // 제목은 OS 호출 전에 바꿔 둔다. 잘못된 UTF-8은 입력 오류다(Window::Create가 먼저 거른다).
        wchar_t title[MaxTitleCharacters];
        if (!ToWide(desc.title, title)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        instance_ = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        windowClass.lpfnWndProc = &Win64Window::Procedure;
        windowClass.hInstance = instance_;
        windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        windowClass.lpszClassName = WindowClassName;
        // 같은 프로세스의 두 번째 창은 이미 등록된 클래스를 쓴다.
        if (RegisterClassExW(&windowClass) == 0) {
            const DWORD error = GetLastError();
            if (error != ERROR_CLASS_ALREADY_EXISTS) {
                return Fail("RegisterClassExW", error);
            }
        }

        const DWORD style = desc.resizable
                                ? WS_OVERLAPPEDWINDOW
                                : (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
        RECT rectangle = {0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height)};
        AdjustWindowRectEx(&rectangle, style, FALSE, 0);
        window_ =
            CreateWindowExW(0, WindowClassName, title, style, CW_USEDEFAULT, CW_USEDEFAULT,
                            rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
                            nullptr, nullptr, instance_, this);
        if (window_ == nullptr) {
            return Fail("CreateWindowExW", GetLastError());
        }

        width_ = desc.width;
        height_ = desc.height;
        if (desc.visible) {
            ShowWindow(window_, SW_SHOW);
            UpdateWindow(window_);
        }
        return Result<void>::Success();
    }

    // 생성 실패의 OS 호출과 GetLastError다(WindowCreateReport).
    const char* failedCall = nullptr;
    DWORD failedError = 0;

    void PumpEvents(input::InputQueue& queue) noexcept override {
        queue_ = &queue;
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                closeRequested_ = true;
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        queue_ = nullptr;
        // 프레임 경계: 이번 펌프에서 끝난 손가락 번호를 돌려받는다(TouchIndexAllocator).
        touches_.EndFrame();
    }

    bool IsCloseRequested() const noexcept override {
        return closeRequested_;
    }

    void ClearCloseRequest() noexcept override {
        closeRequested_ = false;
    }

    uint32 Width() const noexcept override {
        return width_;
    }

    uint32 Height() const noexcept override {
        return height_;
    }

    bool ConsumeResize(uint32& width, uint32& height) noexcept override {
        if (!resized_) {
            return false;
        }
        resized_ = false;
        width = width_;
        height = height_;
        return true;
    }

    bool HasFocus() const noexcept override {
        return focused_;
    }

    bool IsMinimized() const noexcept override {
        return minimized_ || width_ == 0 || height_ == 0;
    }

    NativeWindowHandle NativeHandle() const noexcept override {
        return {window_, instance_};
    }

    Result<void> SetTitle(StringView text) noexcept override {
        wchar_t title[MaxTitleCharacters];
        if (!ToWide(text, title)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        return SetWindowTextW(window_, title) ? Result<void>::Success()
                                              : Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    void ShowAndActivate() noexcept override {
        // 숨긴 채 만든 창을 보이고 앞으로 가져온다. 같은 프로세스의 창(스플래시)이 전경이면 OS가
        // 허용하고, 사용자가 다른 앱으로 옮겼으면 SetForegroundWindow가 실패해 작업 표시줄 단추만
        // 깜빡인다(강제하지 않는다 — Docs/Design/SplashScreen 9.4절).
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
        (void)SetForegroundWindow(window_);
    }

private:
    // 실패한 OS 호출과 오류를 기억하고 DependencyFailed를 돌려준다(Window::Create가 report로
    // 옮긴다).
    Result<void> Fail(const char* call, DWORD error) noexcept {
        failedCall = call;
        failedError = error;
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    void Push(const input::InputEvent& event) noexcept {
        // 펌프 밖(예: 창 생성 중)에 온 메시지의 입력은 버린다.
        if (queue_ != nullptr) {
            queue_->Push(event);
        }
    }

    void PushText(wchar_t unit) noexcept {
        // UTF-16 대리 쌍을 하나의 code point로 합친다.
        const uint32 value = static_cast<uint32>(unit);
        if (value >= 0xD800 && value <= 0xDBFF) {
            highSurrogate_ = value;
            return;
        }
        uint32 codePoint = value;
        if (value >= 0xDC00 && value <= 0xDFFF) {
            if (highSurrogate_ == 0) {
                return;
            }
            codePoint = 0x10000 + ((highSurrogate_ - 0xD800) << 10) + (value - 0xDC00);
        }
        highSurrogate_ = 0;
        // 제어 문자(Backspace·Enter 등)는 키 이벤트로 처리하므로 문자 입력에서 뺀다.
        if (codePoint >= 0x20 && codePoint != 0x7F) {
            Push(input::InputEvent::Text(codePoint));
        }
    }

    void MouseButton(input::MouseButton button, bool down) noexcept {
        // 버튼을 누른 채 창 밖으로 나가도 놓기 이벤트를 받도록 캡처한다.
        const uint32 bit = 1u << static_cast<uint32>(button);
        if (down) {
            if (capturedButtons_++ == 0) {
                SetCapture(window_);
            }
            heldButtons_ |= bit;
            Push(input::InputEvent::MouseDown(button));
        } else {
            heldButtons_ &= ~bit;
            if (capturedButtons_ > 0 && --capturedButtons_ == 0) {
                ReleaseCapture();
            }
            Push(input::InputEvent::MouseUp(button));
        }
    }

    // 버튼을 누른 채 다른 창·시스템이 capture를 가져갔다(WM_CAPTURECHANGED). 이 창에는 뗌이 오지
    // 않으므로 눌린 버튼마다 MouseUp을 만들어 눌린 채 남지 않게 한다. 마지막 버튼을 떼며 부른
    // ReleaseCapture가 보내는 WM_CAPTURECHANGED는 눌린 버튼이 없어 아무것도 하지 않는다.
    void LoseCapture() noexcept {
        for (uint32 index = 0; index < input::MouseButtonCount; ++index) {
            if ((heldButtons_ & (1u << index)) != 0) {
                Push(input::InputEvent::MouseUp(static_cast<input::MouseButton>(index)));
            }
        }

        heldButtons_ = 0;
        capturedButtons_ = 0;
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) noexcept {
        switch (message) {
        case WM_CLOSE:
            closeRequested_ = true;
            return 0;
        case WM_SIZE:
            minimized_ = wParam == SIZE_MINIMIZED;
            width_ = LOWORD(lParam);
            height_ = HIWORD(lParam);
            resized_ = true;
            return 0;
        case WM_SETFOCUS:
            focused_ = true;
            return 0;
        case WM_KILLFOCUS: {
            focused_ = false;
            capturedButtons_ = 0;
            heldButtons_ = 0;
            // 닿아 있던 손가락은 FocusLost가 InputState에서 취소로 끝낸다. 배정기도 같이 비운다.
            uint8 ended[input::MaxTouches];
            (void)touches_.ReleaseAll(ended);
            Push(input::InputEvent::FocusLost());
            return 0;
        }
        case WM_POINTERDOWN:
        case WM_POINTERUPDATE:
        case WM_POINTERUP:
        case WM_POINTERCAPTURECHANGED:
            // 터치·펜은 처리하고 DefWindowProc에 넘기지 않는다. 그러면 OS가 마우스 메시지를 따로
            // 만들지 않는다(UE 기본 — Docs/Design/TouchInput.ko.md 4.1절). 마우스 포인터는 아래로
            // 넘겨 WM_MOUSE*가 그대로 온다.
            if (HandlePointer(message, wParam)) {
                return 0;
            }

            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const Key key = KeyFromMessage(lParam);
            const bool repeat = ((lParam >> 30) & 1) != 0;
            if (key != Key::None) {
                Push(input::InputEvent::KeyDown(key, repeat));
            }
            // Alt+F4 같은 시스템 키 동작은 OS 기본 처리로 넘긴다.
            break;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            const Key key = KeyFromMessage(lParam);
            if (key != Key::None) {
                Push(input::InputEvent::KeyUp(key));
            }
            break;
        }
        case WM_CHAR:
            PushText(static_cast<wchar_t>(wParam));
            return 0;
        case WM_MOUSEMOVE:
            Push(input::InputEvent::MouseMove(
                static_cast<float>(static_cast<int16>(LOWORD(lParam))),
                static_cast<float>(static_cast<int16>(HIWORD(lParam)))));
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            MouseButton(input::MouseButton::Left, message == WM_LBUTTONDOWN);
            return 0;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            MouseButton(input::MouseButton::Right, message == WM_RBUTTONDOWN);
            return 0;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            MouseButton(input::MouseButton::Middle, message == WM_MBUTTONDOWN);
            return 0;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            MouseButton(HIWORD(wParam) == XBUTTON1 ? input::MouseButton::X1
                                                   : input::MouseButton::X2,
                        message == WM_XBUTTONDOWN);
            return TRUE;
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lParam) != window_) {
                LoseCapture();
            }

            return 0;
        case WM_MOUSEWHEEL:
            Push(input::InputEvent::MouseWheel(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) /
                                               static_cast<float>(WHEEL_DELTA)));
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    // WM_POINTER 터치·펜 메시지를 터치 이벤트로 바꾼다. 터치·펜이 아니면 false다(마우스는
    // WM_MOUSE*).
    bool HandlePointer(UINT message, WPARAM wParam) noexcept {
        const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
        POINTER_INPUT_TYPE type = PT_POINTER;
        if (!GetPointerType(pointerId, &type) ||
            !pointer::IsTouchPointerType(static_cast<uint32>(type))) {
            return false;
        }

        POINTER_INFO info = {};
        float force = 1.0f;
        bool hasForce = false;
        if (type == PT_TOUCH) {
            POINTER_TOUCH_INFO touch = {};
            if (!GetPointerTouchInfo(pointerId, &touch)) {
                return false;
            }

            info = touch.pointerInfo;
            if ((touch.touchMask & TOUCH_MASK_PRESSURE) != 0) {
                force = pointer::NormalizePointerPressure(touch.pressure);
                hasForce = true;
            }
        } else {
            POINTER_PEN_INFO pen = {};
            if (!GetPointerPenInfo(pointerId, &pen)) {
                return false;
            }

            info = pen.pointerInfo;
            if ((pen.penMask & PEN_MASK_PRESSURE) != 0) {
                force = pointer::NormalizePointerPressure(pen.pressure);
                hasForce = true;
            }
        }

        POINT location = info.ptPixelLocation;
        (void)ScreenToClient(window_, &location);
        const auto x = static_cast<float>(location.x);
        const auto y = static_cast<float>(location.y);
        switch (pointer::TouchActionOf(message, static_cast<uint32>(info.pointerFlags))) {
        case pointer::PointerTouchAction::Began: {
            const uint8 index = touches_.Acquire(pointerId);
            if (index != input::NoTouchIndex) {
                Push(input::InputEvent::TouchBegan(index, x, y, force, hasForce));
            }

            break;
        }
        case pointer::PointerTouchAction::Moved: {
            const uint8 index = touches_.Find(pointerId);
            if (index != input::NoTouchIndex) {
                Push(input::InputEvent::TouchMoved(index, x, y, force, hasForce));
            }

            break;
        }
        case pointer::PointerTouchAction::Ended:
        case pointer::PointerTouchAction::Cancelled: {
            const bool cancelled =
                pointer::TouchActionOf(message, static_cast<uint32>(info.pointerFlags)) ==
                pointer::PointerTouchAction::Cancelled;
            const uint8 index = touches_.Release(pointerId);
            if (index != input::NoTouchIndex) {
                Push(input::InputEvent::TouchEnded(index, x, y, cancelled));
            }

            break;
        }
        case pointer::PointerTouchAction::None:
            break;
        }

        return true;
    }

    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wParam,
                                      LPARAM lParam) noexcept {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            auto* self = static_cast<Win64Window*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        auto* self = reinterpret_cast<Win64Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self == nullptr) {
            return DefWindowProcW(window, message, wParam, lParam);
        }
        return self->Handle(message, wParam, lParam);
    }

    HWND window_ = nullptr;
    HINSTANCE instance_ = nullptr;
    input::InputQueue* queue_ = nullptr;
    // 터치·펜 포인터의 손가락 번호 배정이다(창 스레드만 만진다).
    input::TouchIndexAllocator touches_;
    uint32 width_ = 0;
    uint32 height_ = 0;
    uint32 highSurrogate_ = 0;
    uint32 capturedButtons_ = 0;
    // 지금 눌린 마우스 버튼(MouseButton 비트)이다. capture를 잃으면 이 버튼들의 뗌을 만든다.
    uint32 heldButtons_ = 0;
    bool closeRequested_ = false;
    bool resized_ = false;
    bool focused_ = false;
    bool minimized_ = false;
};

// XInput 버튼 비트 → GamepadButton 순서 비트.
uint32 MapButtons(WORD buttons) noexcept {
    struct Mapping {
        WORD xinput;
        input::GamepadButton button;
    };
    static constexpr Mapping mappings[] = {
        {XINPUT_GAMEPAD_A, input::GamepadButton::South},
        {XINPUT_GAMEPAD_B, input::GamepadButton::East},
        {XINPUT_GAMEPAD_X, input::GamepadButton::West},
        {XINPUT_GAMEPAD_Y, input::GamepadButton::North},
        {XINPUT_GAMEPAD_LEFT_SHOULDER, input::GamepadButton::LeftShoulder},
        {XINPUT_GAMEPAD_RIGHT_SHOULDER, input::GamepadButton::RightShoulder},
        {XINPUT_GAMEPAD_BACK, input::GamepadButton::Back},
        {XINPUT_GAMEPAD_START, input::GamepadButton::Start},
        {XINPUT_GAMEPAD_LEFT_THUMB, input::GamepadButton::LeftThumb},
        {XINPUT_GAMEPAD_RIGHT_THUMB, input::GamepadButton::RightThumb},
        {XINPUT_GAMEPAD_DPAD_UP, input::GamepadButton::DPadUp},
        {XINPUT_GAMEPAD_DPAD_DOWN, input::GamepadButton::DPadDown},
        {XINPUT_GAMEPAD_DPAD_LEFT, input::GamepadButton::DPadLeft},
        {XINPUT_GAMEPAD_DPAD_RIGHT, input::GamepadButton::DPadRight},
    };
    uint32 result = 0;
    for (const Mapping& mapping : mappings) {
        if ((buttons & mapping.xinput) != 0) {
            result |= 1u << static_cast<uint32>(mapping.button);
        }
    }
    return result;
}

float StickAxis(SHORT value) noexcept {
    // [-32768, 32767] → [-1, 1]. 음수 쪽이 한 칸 더 넓으므로 각 방향의 최대값으로 나눈다.
    return value < 0 ? static_cast<float>(value) / 32768.0f : static_cast<float>(value) / 32767.0f;
}

using XInputGetStateFunction = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputSetStateFunction = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);

class XInputGamepads final : public GamepadBackend {
public:
    ~XInputGamepads() override {
        // 진동 중인 패드를 멈춘다(엔진 종료 — Docs/Design/InputActions 6.3절).
        for (DWORD index = 0; index < input::MaxGamepads; ++index) {
            const Slot& slot = slots_[index];
            if (setState_ != nullptr && slot.connected &&
                !(slot.motors == platform::gamepad::XInputMotorPair{})) {
                XINPUT_VIBRATION stop = {};
                (void)setState_(index, &stop);
            }
        }

        // 함수 표를 먼저 놓아야 DLL을 내릴 수 있다.
        getState_ = nullptr;
        setState_ = nullptr;
        lease_.Reset();
        (void)library_.Close();
    }

    // System32의 XInput DLL을 연다(Windows 8 이상 xinput1_4, 이전 호환 xinput9_1_0).
    Result<void> Load() noexcept {
        if (!library_.OpenSystem("xinput1_4.dll") && !library_.OpenSystem("xinput9_1_0.dll")) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }

        void* getState = nullptr;
        if (!library_.Find("XInputGetState", getState, lease_)) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }

        // 진동은 선택이다(없으면 SetMotors가 Unsupported — 입력은 그대로 쓴다).
        void* setState = nullptr;
        if (library_.Find("XInputSetState", setState, lease_)) {
            setState_ = reinterpret_cast<XInputSetStateFunction>(setState);
        }

        getState_ = reinterpret_cast<XInputGetStateFunction>(getState);
        return Result<void>::Success();
    }

    Result<void> SetMotors(uint32 index, const input::MotorValues& values) noexcept override {
        if (index >= input::MaxGamepads || !platform::gamepad::MotorValuesAreFinite(values)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (setState_ == nullptr) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        Slot& slot = slots_[index];
        if (!slot.connected) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        // 바뀐 값만 쓴다(UE XInputInterface.cpp:373-385).
        const platform::gamepad::XInputMotorPair speeds =
            platform::gamepad::XInputMotorSpeeds(values);
        if (speeds == slot.motors) {
            return Result<void>::Success();
        }

        XINPUT_VIBRATION vibration = {};
        vibration.wLeftMotorSpeed = speeds.left;
        vibration.wRightMotorSpeed = speeds.right;
        if (setState_(static_cast<DWORD>(index), &vibration) != ERROR_SUCCESS) {
            // 직전 값은 그대로다(다음 호출이 다시 쓴다).
            return Result<void>::Failure(ErrorCode::IoError);
        }

        slot.motors = speeds;
        return Result<void>::Success();
    }

    void Poll(input::InputQueue& queue) noexcept override {
        const uint64 now = MonotonicNanoseconds();
        for (DWORD index = 0; index < input::MaxGamepads; ++index) {
            Slot& slot = slots_[index];
            // 연결되지 않은 슬롯 조회는 느리므로 1초에 한 번만 다시 확인한다.
            if (!slot.connected && slot.lastProbe != 0 && now - slot.lastProbe < 1000000000ull) {
                continue;
            }
            XINPUT_STATE state = {};
            const DWORD result = getState_(index, &state);
            if (result != ERROR_SUCCESS) {
                slot.lastProbe = now;
                if (slot.connected) {
                    slot.connected = false;
                    queue.Push(input::InputEvent::Gamepad(static_cast<uint8>(index), {}));
                }
                continue;
            }
            const bool changed = !slot.connected || state.dwPacketNumber != slot.packet;
            // 새로 연결된 패드는 모터가 멈춘 상태다(직전 값을 0으로 — 6.4절).
            if (!slot.connected) {
                slot.motors = {};
            }

            slot.connected = true;
            slot.packet = state.dwPacketNumber;
            if (!changed) {
                continue;
            }
            input::GamepadSnapshot snapshot;
            snapshot.connected = true;
            snapshot.buttons = MapButtons(state.Gamepad.wButtons);
            snapshot.axes[static_cast<uint32>(input::GamepadAxis::LeftX)] =
                StickAxis(state.Gamepad.sThumbLX);
            snapshot.axes[static_cast<uint32>(input::GamepadAxis::LeftY)] =
                StickAxis(state.Gamepad.sThumbLY);
            snapshot.axes[static_cast<uint32>(input::GamepadAxis::RightX)] =
                StickAxis(state.Gamepad.sThumbRX);
            snapshot.axes[static_cast<uint32>(input::GamepadAxis::RightY)] =
                StickAxis(state.Gamepad.sThumbRY);
            snapshot.axes[static_cast<uint32>(input::GamepadAxis::LeftTrigger)] =
                static_cast<float>(state.Gamepad.bLeftTrigger) / 255.0f;
            snapshot.axes[static_cast<uint32>(input::GamepadAxis::RightTrigger)] =
                static_cast<float>(state.Gamepad.bRightTrigger) / 255.0f;
            queue.Push(input::InputEvent::Gamepad(static_cast<uint8>(index), snapshot));
        }
    }

private:
    struct Slot {
        bool connected = false;
        DWORD packet = 0;
        uint64 lastProbe = 0;
        // 마지막으로 장치에 쓴 모터 속도다.
        platform::gamepad::XInputMotorPair motors;
    };

    // lease_가 살아 있는 동안 getState_가 유효하다. 소멸자가 lease_를 먼저 놓고 library_를 닫는다.
    DynamicLibrary library_;
    DynamicLibraryLease lease_;
    XInputGetStateFunction getState_ = nullptr;
    XInputSetStateFunction setState_ = nullptr;
    Slot slots_[input::MaxGamepads];
};

} // namespace

Result<UniquePtr<Window>> Window::Create(Allocator& allocator, const WindowDesc& desc,
                                         WindowCreateReport* report) noexcept {
    auto fail = [report](ErrorCode error, const char* call, DWORD osError) noexcept {
        if (report != nullptr) {
            *report = WindowCreateReport{};
            report->code = error;
            report->osError = static_cast<int64>(osError);
            usize length = 0;
            while (call != nullptr && call[length] != '\0' && length + 1 < sizeof(report->call)) {
                report->call[length] = call[length];
                ++length;
            }

            report->truncated = call != nullptr && call[length] != '\0';
        }

        return Result<UniquePtr<Window>>::Failure(error);
    };

    if (desc.width == 0 || desc.height == 0 || !desc.title.IsValidUtf8()) {
        return fail(ErrorCode::InvalidArgument, nullptr, 0);
    }

    auto created = UniquePtr<Win64Window>::TryMake(allocator);
    if (!created) {
        return fail(created.Error(), nullptr, 0);
    }

    auto opened = created.Value()->Create(desc);
    if (!opened) {
        return fail(opened.Error(), created.Value()->failedCall, created.Value()->failedError);
    }

    if (report != nullptr) {
        *report = WindowCreateReport{};
    }

    return Result<UniquePtr<Window>>::Success(UniquePtr<Window>(Move(created.Value())));
}

Result<UniquePtr<GamepadBackend>> GamepadBackend::Create(Allocator& allocator) noexcept {
    auto created = UniquePtr<XInputGamepads>::TryMake(allocator);
    if (!created) {
        return Result<UniquePtr<GamepadBackend>>::Failure(created.Error());
    }

    auto loaded = created.Value()->Load();
    if (!loaded) {
        return Result<UniquePtr<GamepadBackend>>::Failure(loaded.Error());
    }

    return Result<UniquePtr<GamepadBackend>>::Success(
        UniquePtr<GamepadBackend>(Move(created.Value())));
}

DesktopSessionState QueryDesktopSessionState() noexcept {
    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
                                     WTSConnectState, &buffer, &bytes) ||
        buffer == nullptr) {
        return DesktopSessionState::Unknown;
    }

    DesktopSessionState state = DesktopSessionState::Unknown;
    if (bytes >= sizeof(WTS_CONNECTSTATE_CLASS)) {
        const auto connect = *reinterpret_cast<const WTS_CONNECTSTATE_CLASS*>(buffer);
        if (connect == WTSActive) {
            state = DesktopSessionState::Connected;
        } else if (connect == WTSDisconnected) {
            state = DesktopSessionState::Disconnected;
        }
    }

    WTSFreeMemory(buffer);
    return state;
}

} // namespace rocks::platform
