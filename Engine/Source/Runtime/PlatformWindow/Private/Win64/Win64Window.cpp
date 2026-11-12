#include <Platform/Clock.h>
#include <PlatformWindow/Window.h>

#include <Windows.h>
#include <Xinput.h>

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

// UTF-8 → UTF-16(널 종료). 넘치면 자른다. 변환할 수 없는 입력은 빈 문자열이다.
void ToWide(StringView text, wchar_t (&buffer)[MaxTitleCharacters]) noexcept {
    buffer[0] = L'\0';
    if (text.ByteCount() == 0) {
        return;
    }
    const int bytes =
        text.ByteCount() > 0x7fffffff ? 0x7fffffff : static_cast<int>(text.ByteCount());
    int written = MultiByteToWideChar(CP_UTF8, 0, text.Data(), bytes, buffer,
                                      static_cast<int>(MaxTitleCharacters - 1));
    if (written <= 0) {
        written = 0;
    }
    buffer[written] = L'\0';
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
        instance_ = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        windowClass.lpfnWndProc = &Win64Window::Procedure;
        windowClass.hInstance = instance_;
        windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        windowClass.lpszClassName = WindowClassName;
        // 같은 프로세스의 두 번째 창은 이미 등록된 클래스를 쓴다.
        if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }

        const DWORD style = desc.resizable
                                ? WS_OVERLAPPEDWINDOW
                                : (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
        RECT rectangle = {0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height)};
        AdjustWindowRectEx(&rectangle, style, FALSE, 0);
        wchar_t title[MaxTitleCharacters];
        ToWide(desc.title, title);
        window_ =
            CreateWindowExW(0, WindowClassName, title, style, CW_USEDEFAULT, CW_USEDEFAULT,
                            rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
                            nullptr, nullptr, instance_, this);
        if (window_ == nullptr) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }

        width_ = desc.width;
        height_ = desc.height;
        if (desc.visible) {
            ShowWindow(window_, SW_SHOW);
            UpdateWindow(window_);
        }
        return Result<void>::Success();
    }

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
        ToWide(text, title);
        return SetWindowTextW(window_, title) ? Result<void>::Success()
                                              : Result<void>::Failure(ErrorCode::DependencyFailed);
    }

private:
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
        if (down) {
            if (capturedButtons_++ == 0) {
                SetCapture(window_);
            }
            Push(input::InputEvent::MouseDown(button));
        } else {
            if (capturedButtons_ > 0 && --capturedButtons_ == 0) {
                ReleaseCapture();
            }
            Push(input::InputEvent::MouseUp(button));
        }
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
        case WM_KILLFOCUS:
            focused_ = false;
            capturedButtons_ = 0;
            Push(input::InputEvent::FocusLost());
            return 0;
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
        case WM_MOUSEWHEEL:
            Push(input::InputEvent::MouseWheel(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) /
                                               static_cast<float>(WHEEL_DELTA)));
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
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
    uint32 width_ = 0;
    uint32 height_ = 0;
    uint32 highSurrogate_ = 0;
    uint32 capturedButtons_ = 0;
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

class XInputGamepads final : public GamepadBackend {
public:
    ~XInputGamepads() override {
        if (library_ != nullptr) {
            FreeLibrary(library_);
        }
    }

    // XInput DLL을 연다. Windows 8 이상은 xinput1_4, 그 이전 호환으로 xinput9_1_0을 쓴다.
    Result<void> Load() noexcept {
        library_ = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (library_ == nullptr) {
            library_ = LoadLibraryExW(L"xinput9_1_0.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
        if (library_ == nullptr) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }
        getState_ = reinterpret_cast<XInputGetStateFunction>(
            reinterpret_cast<void*>(GetProcAddress(library_, "XInputGetState")));
        return getState_ != nullptr ? Result<void>::Success()
                                    : Result<void>::Failure(ErrorCode::DependencyFailed);
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
    };

    HMODULE library_ = nullptr;
    XInputGetStateFunction getState_ = nullptr;
    Slot slots_[input::MaxGamepads];
};

} // namespace

Result<UniquePtr<Window>> Window::Create(Allocator& allocator, const WindowDesc& desc) noexcept {
    if (desc.width == 0 || desc.height == 0) {
        return Result<UniquePtr<Window>>::Failure(ErrorCode::InvalidArgument);
    }

    auto created = UniquePtr<Win64Window>::TryMake(allocator);
    if (!created) {
        return Result<UniquePtr<Window>>::Failure(created.Error());
    }

    auto opened = created.Value()->Create(desc);
    if (!opened) {
        return Result<UniquePtr<Window>>::Failure(opened.Error());
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

} // namespace rocks::platform
