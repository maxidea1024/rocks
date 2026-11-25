#include "Win64InlineComposition.h"
#include <Platform/Clock.h>
#include <Platform/DynamicLibrary.h>
#include <Platform/Win64/WideString.h>
#include <PlatformWindow/Window.h>

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>
#include <imm.h>
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
    explicit Win64Window(Allocator& allocator) noexcept : allocator_(allocator), deferred_(allocator) {
    }

    ~Win64Window() override {
        if (window_ != nullptr) {
            SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
            DestroyWindow(window_);
        }
    }

    Result<void> Create(const WindowDesc& desc) noexcept {
        // 다른 창이 메시지를 펌프하거나 동기 Win32 호출이 재진입했을 때 입력을 보관한다.
        auto initialized = deferred_.TryInitialize(256);
        if (!initialized) {
            return initialized;
        }
        // 제목은 OS 호출 전에 바꿔 둔다. 잘못된 UTF-8은 입력 오류다(Window::Create가 먼저 거른다).
        wchar_t title[MaxTitleCharacters];
        if (!ToWide(desc.title, title)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        instance_ = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC | CS_DBLCLKS;
        windowClass.lpfnWndProc = &Win64Window::Procedure;
        windowClass.hInstance = instance_;
        windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        cursor_ = windowClass.hCursor;
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
        const int x = desc.useDesktopPosition ? desc.desktopPosition.x +
                                                    (desc.positionClientOrigin ? rectangle.left : 0)
                                              : CW_USEDEFAULT;
        const int y = desc.useDesktopPosition
                          ? desc.desktopPosition.y + (desc.positionClientOrigin ? rectangle.top : 0)
                          : CW_USEDEFAULT;
        window_ = CreateWindowExW(
            0, WindowClassName, title, style, x, y, rectangle.right - rectangle.left,
            rectangle.bottom - rectangle.top, nullptr, nullptr, instance_, this);
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
        deferred_.DrainTo(queue);
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

    bool ConsumeRepaint() noexcept override {
        const bool requested = repaint_;
        repaint_ = false;
        return requested;
    }

    void WaitForEvents(uint32 milliseconds) noexcept override {
        (void)MsgWaitForMultipleObjectsEx(0, nullptr, milliseconds, QS_ALLINPUT,
                                          MWMO_INPUTAVAILABLE);
    }

    void SetRegionHitTest(RegionHitTest&& callback) noexcept override {
        regionHitTest_ = Move(callback);
    }

    void SetMouseCursor(input::MouseCursor shape) noexcept override {
        if (shape == cursorShape_) {
            return;
        }
        LPCWSTR resource = reinterpret_cast<LPCWSTR>(IDC_ARROW);
        if (shape == input::MouseCursor::ResizeHorizontal) {
            resource = reinterpret_cast<LPCWSTR>(IDC_SIZEWE);
        } else if (shape == input::MouseCursor::ResizeVertical) {
            resource = reinterpret_cast<LPCWSTR>(IDC_SIZENS);
        }
        switch(shape) {
        case input::MouseCursor::Text: resource=reinterpret_cast<LPCWSTR>(IDC_IBEAM);break;
        case input::MouseCursor::Hand: resource=reinterpret_cast<LPCWSTR>(IDC_HAND);break;
        case input::MouseCursor::Crosshair: resource=reinterpret_cast<LPCWSTR>(IDC_CROSS);break;
        case input::MouseCursor::Move: resource=reinterpret_cast<LPCWSTR>(IDC_SIZEALL);break;
        case input::MouseCursor::ResizeNorthWestSouthEast: resource=reinterpret_cast<LPCWSTR>(IDC_SIZENWSE);break;
        case input::MouseCursor::ResizeNorthEastSouthWest: resource=reinterpret_cast<LPCWSTR>(IDC_SIZENESW);break;
        case input::MouseCursor::Unavailable: resource=reinterpret_cast<LPCWSTR>(IDC_NO);break;
        case input::MouseCursor::Wait: resource=reinterpret_cast<LPCWSTR>(IDC_WAIT);break;
        default: break;
        }
        HCURSOR cursor = LoadCursorW(nullptr, resource);
        if (!cursor) {
            return;
        }
        cursor_ = cursor;
        cursorShape_ = shape;
        // 입력 큐를 처리한 뒤에도 현재 창의 클라이언트 영역에서만 즉시 반영한다.
        const HWND captured = GetCapture();
        RECT client{};
        POINT point{};
        if (captured == window_ ||
            (!captured && GetCursorPos(&point) && WindowFromPoint(point) == window_ &&
             ScreenToClient(window_, &point) && GetClientRect(window_, &client) &&
             PtInRect(&client, point))) {
            SetCursor(cursor_);
        }
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

    Result<IntVec2> ClientToDesktop(IntVec2 point) const noexcept override {
        POINT p{point.x, point.y};
        if (!::ClientToScreen(window_, &p)) {
            return Result<IntVec2>::Failure(ErrorCode::DependencyFailed);
        }
        return Result<IntVec2>::Success({static_cast<int32>(p.x), static_cast<int32>(p.y)});
    }

    Result<IntVec2> DesktopToClient(IntVec2 point) const noexcept override {
        POINT p{point.x, point.y};
        if (!::ScreenToClient(window_, &p)) {
            return Result<IntVec2>::Failure(ErrorCode::DependencyFailed);
        }
        return Result<IntVec2>::Success({static_cast<int32>(p.x), static_cast<int32>(p.y)});
    }

    bool IsAtDesktopPoint(IntVec2 point) const noexcept override {
        const HWND hit = WindowFromPoint({point.x, point.y});
        return hit && GetAncestor(hit, GA_ROOT) == window_;
    }

    Result<void> SetTitle(StringView text) noexcept override {
        wchar_t title[MaxTitleCharacters];
        if (!ToWide(text, title)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        return SetWindowTextW(window_, title) ? Result<void>::Success()
                                              : Result<void>::Failure(ErrorCode::DependencyFailed);
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
        if (queue_ != nullptr) {
            queue_->Push(event);
        } else {
            deferred_.Push(event);
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

    void MouseButton(input::MouseButton button, bool down, LPARAM position) noexcept {
        const float x = static_cast<float>(static_cast<int16>(LOWORD(position)));
        const float y = static_cast<float>(static_cast<int16>(HIWORD(position)));
        // 버튼을 누른 채 창 밖으로 나가도 놓기 이벤트를 받도록 캡처한다.
        const uint32 bit = 1u << static_cast<uint32>(button);
        if (down) {
            if (!capturedButtons_) {
                SetCapture(window_);
            }
            capturedButtons_ |= bit;
            Push(input::InputEvent::MouseDown(button, x, y));
        } else {
            capturedButtons_ &= ~bit;
            if (!capturedButtons_ && GetCapture() == window_) {
                ReleaseCapture();
            }
            Push(input::InputEvent::MouseUp(button, x, y));
        }
    }

    input::WindowRegion ClientRegion(LPARAM coordinates) noexcept {
        POINT point{static_cast<short>(LOWORD(coordinates)),
                    static_cast<short>(HIWORD(coordinates))};
        if (regionHitTest_ && ScreenToClient(window_, &point)) {
            return regionHitTest_.Execute({point.x, point.y});
        }
        return input::WindowRegion::Client;
    }

    void ToggleBorderlessFullscreen() noexcept {
        if (fullscreenStyle_) {
            SetWindowLongPtrW(window_, GWL_STYLE, fullscreenStyle_);
            SetWindowPlacement(window_, &windowedPlacement_);
            SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            fullscreenStyle_ = 0;
            return;
        }
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        windowedPlacement_.length = sizeof(windowedPlacement_);
        if (!GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor) ||
            !GetWindowPlacement(window_, &windowedPlacement_)) {
            return;
        }
        fullscreenStyle_ = GetWindowLongPtrW(window_, GWL_STYLE);
        SetWindowLongPtrW(window_, GWL_STYLE,
                          (fullscreenStyle_ & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW)) |
                              WS_POPUP);
        const RECT bounds = monitor.rcMonitor;
        SetWindowPos(window_, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
                     bounds.bottom - bounds.top, SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) noexcept {
        switch (message) {
        case WM_NCHITTEST: {
            const auto frame = DefWindowProcW(window_, message, wParam, lParam);
            if (frame == HTCLIENT && ClientRegion(lParam) != input::WindowRegion::Client) {
                return HTCAPTION;
            }
            return frame;
        }
        case WM_NCLBUTTONDBLCLK:
            if (wParam == HTCAPTION &&
                ClientRegion(lParam) == input::WindowRegion::FullscreenCaption) {
                ToggleBorderlessFullscreen();
                return 0;
            }
            break;
        case WM_SETCURSOR:
            if (reinterpret_cast<HWND>(wParam) == window_ && LOWORD(lParam) == HTCLIENT &&
                cursor_) {
                SetCursor(cursor_);
                return TRUE;
            }
            break;
        case WM_CLOSE:
            closeRequested_ = true;
            return 0;
        case WM_PAINT:
            repaint_ = true;
            break;
        case WM_SIZE:
            repaint_ = true;
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
            if (GetCapture() == window_) {
                ReleaseCapture();
            }
            Push(input::InputEvent::FocusLost());
            return 0;
        case WM_CAPTURECHANGED:
            // 마지막 버튼을 놓아 정상 해제할 때는 이미 0이므로 별도 취소를 보내지 않는다.
            if (capturedButtons_ != 0) {
                capturedButtons_ = 0;
                Push(input::InputEvent::CaptureLost());
            }
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
        case WM_IME_SETCONTEXT:
            if (IsInlineCompositionWindow(window_)) {
                lParam &= ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW);
            }
            break;
        case WM_IME_COMPOSITION:
            if (IsInlineCompositionWindow(window_) &&
                RouteInlineComposition(window_, lParam, allocator_, queue_ ? *queue_ : deferred_)) {
                return 0;
            }
            break;
        case WM_IME_ENDCOMPOSITION:
            if (IsInlineCompositionWindow(window_)) {
                Push(input::InputEvent::Composition());
            }
            break;
        case WM_CHAR:
            PushText(static_cast<wchar_t>(wParam));
            return 0;
        case WM_MOUSELEAVE: {
            trackingMouse_ = false;
            input::InputEvent leave;
            leave.type = input::InputEventType::MouseLeave;
            Push(leave);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (!trackingMouse_) {
                TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window_, 0};
                trackingMouse_ = TrackMouseEvent(&tracking) != FALSE;
            }
            Push(input::InputEvent::MouseMove(
                static_cast<float>(static_cast<int16>(LOWORD(lParam))),
                static_cast<float>(static_cast<int16>(HIWORD(lParam)))));
            return 0;
        case WM_LBUTTONDBLCLK:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            MouseButton(input::MouseButton::Left, message != WM_LBUTTONUP, lParam);
            return 0;
        case WM_RBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            MouseButton(input::MouseButton::Right, message != WM_RBUTTONUP, lParam);
            return 0;
        case WM_MBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            MouseButton(input::MouseButton::Middle, message != WM_MBUTTONUP, lParam);
            return 0;
        case WM_XBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            MouseButton(HIWORD(wParam) == XBUTTON1 ? input::MouseButton::X1
                                                   : input::MouseButton::X2,
                        message != WM_XBUTTONUP, lParam);
            return TRUE;
        case WM_MOUSEWHEEL: {
            const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) /
                                static_cast<float>(WHEEL_DELTA);
            POINT point{static_cast<int16>(LOWORD(lParam)), static_cast<int16>(HIWORD(lParam))};
            if (ScreenToClient(window_, &point)) {
                Push(input::InputEvent::MouseWheel(delta, static_cast<float>(point.x),
                                                   static_cast<float>(point.y)));
            } else {
                Push(input::InputEvent::MouseWheel(delta));
            }
            return 0;
        }
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

    Allocator& allocator_;
    HWND window_ = nullptr;
    HCURSOR cursor_ = nullptr;
    RegionHitTest regionHitTest_;
    LONG_PTR fullscreenStyle_ = 0;
    WINDOWPLACEMENT windowedPlacement_{};
    bool trackingMouse_ = false;
    input::MouseCursor cursorShape_ = input::MouseCursor::Arrow;
    HINSTANCE instance_ = nullptr;
    input::InputQueue* queue_ = nullptr;
    input::InputQueue deferred_;
    uint32 width_ = 0;
    uint32 height_ = 0;
    uint32 highSurrogate_ = 0;
    uint32 capturedButtons_ = 0;
    bool closeRequested_ = false;
    bool resized_ = false;
    bool repaint_ = true;
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
        // 함수 표를 먼저 놓아야 DLL을 내릴 수 있다.
        getState_ = nullptr;
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

        getState_ = reinterpret_cast<XInputGetStateFunction>(getState);
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

    // lease_가 살아 있는 동안 getState_가 유효하다. 소멸자가 lease_를 먼저 놓고 library_를 닫는다.
    DynamicLibrary library_;
    DynamicLibraryLease lease_;
    XInputGetStateFunction getState_ = nullptr;
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

    auto created = UniquePtr<Win64Window>::TryMake(allocator, allocator);
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
