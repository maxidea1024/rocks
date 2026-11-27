#include "Win64InlineComposition.h"
#include <Core/Assert.h>
#include <Platform/Clock.h>
#include <Platform/DynamicLibrary.h>
#include <Platform/Win64/WideString.h>
#include <Input/TouchIndexAllocator.h>
#include <PlatformWindow/Window.h>

#include "../Common/GamepadMotorMapping.h"
#include "../Common/PointerTouchMapping.h"

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <imm.h>
#include <WtsApi32.h>
#include <Xinput.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

namespace rocks::platform {

namespace {

using input::Key;

constexpr wchar_t WindowClassName[] = L"RocksWindow";
// 직접 그린 창의 DWM 그림자를 살리는 위쪽 프레임 확장(화소)이다.
constexpr int CustomFrameShadowMargin = 1;
// 최대화 중 자동 숨김 작업 표시줄 쪽에 남기는 틈(화소)이다.
constexpr LONG AutoHideTaskbarGap = 1;
// WM_SYSCOMMAND wParam의 아래 4 bit는 OS가 쓰므로 명령 비교 전에 지운다(SC_* 문서).
constexpr WPARAM SysCommandMask = 0xFFF0;
// SC_KEYMENU의 lParam이 이 글자면 Alt+Space(시스템 메뉴)다.
constexpr LPARAM SystemMenuKey = ' ';
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

class Win64Window;

// 이 스레드가 만든 창 목록이다(펌프 수준 대기가 창 대기 큐를 본다 — Docs/Design/UiToolkit 19.2절).
// 창은 만든 스레드에서만 쓰므로 스레드마다 따로 둔다.
thread_local Win64Window* threadWindows = nullptr;
// 마지막으로 메시지를 펌프한 스레드 ID다(WakeMessageWait가 빈 메시지를 게시한다). 0이면 없다.
volatile LONG pumpThread = 0;
// 창 대기 큐 용량이다(한 펌프 동안 다른 창 메시지를 처리하는 사이 쌓이는 입력).
constexpr usize WindowPendingEvents = 256;
// OS 두 번 누르기 메시지의 누름 횟수다.
constexpr uint8 DoubleClickCount = 2;

class Win64Window final : public Window {
public:
    explicit Win64Window(Allocator& allocator) noexcept
        : allocator_(allocator), pending_(allocator) {
    }

    ~Win64Window() override {
        Unlink();
        if (window_ != nullptr) {
            SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
            DestroyWindow(window_);
        }
    }

    // 이 스레드의 OS 메시지를 모두 디스패치한다. 창 프로시저가 입력을 그 창의 대기 큐에 넣는다.
    static void PumpThreadMessages() noexcept {
        (void)InterlockedExchange(&pumpThread, static_cast<LONG>(GetCurrentThreadId()));
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            // 스레드 닫기 요청은 이 스레드의 모든 창에 알린다.
            if (message.message == WM_QUIT) {
                for (Win64Window* window = threadWindows; window; window = window->next_) {
                    window->closeRequested_ = true;
                }

                continue;
            }

            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    // 이 스레드의 창 중 대기 큐에 이벤트가 남은 창이 있는지다.
    static bool AnyPendingEvents() noexcept {
        for (Win64Window* window = threadWindows; window; window = window->next_) {
            if (!window->pending_.Events().IsEmpty()) {
                return true;
            }
        }

        return false;
    }

    Result<void> Create(const WindowDesc& desc) noexcept {
        // 메시지 펌프가 디스패치한 이 창의 입력을 호스트가 TakeEvents로 가져갈 때까지 보관한다.
        auto initialized = pending_.TryInitialize(WindowPendingEvents);
        if (!initialized) {
            return initialized;
        }

        // 창 목록에 먼저 넣는다(생성 중 메시지도 이 창 대기 큐로 간다). 소멸자가 뺀다.
        Link();

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

        // 직접 그린 창도 OS 스타일(제목·시스템 메뉴·크기 조절 틀)은 그대로 둔다 — 화면 가장자리
        // 붙이기·애니메이션·시스템 메뉴가 이 스타일에 달려 있다. 비클라이언트 영역은
        // WM_NCCALCSIZE가 없앤다(UE FWindowsWindow도 같다).
        customFrame_ = desc.frame == WindowFrame::Custom;
        resizable_ = desc.resizable;
        const DWORD style = desc.resizable
                                ? WS_OVERLAPPEDWINDOW
                                : (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
        RECT rectangle = {0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height)};
        if (!customFrame_) {
            AdjustWindowRectEx(&rectangle, style, FALSE, 0);
        }

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
        if (customFrame_) {
            // 프레임을 다시 계산하게 해 OS 테두리를 없애고, DWM 그림자는 위 1 px 확장으로 살린다.
            const MARGINS shadow{0, 0, CustomFrameShadowMargin, 0};
            (void)DwmExtendFrameIntoClientArea(window_, &shadow);
            SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                             SWP_FRAMECHANGED);
        }

        if (desc.visible) {
            ShowWindow(window_, SW_SHOW);
            UpdateWindow(window_);
        }

        return Result<void>::Success();
    }

    // 생성 실패의 OS 호출과 GetLastError다(WindowCreateReport).
    const char* failedCall = nullptr;
    DWORD failedError = 0;

    void TakeEvents(input::InputQueue& queue) noexcept override {
        AssertOwnerThread();
        pending_.DrainTo(queue);
        // 프레임 경계: 이번에 넘긴 이벤트에서 끝난 손가락 번호를 돌려받는다(TouchIndexAllocator).
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

    bool ConsumeRepaint() noexcept override {
        const bool requested = repaint_;
        repaint_ = false;
        return requested;
    }

    void SetRegionHitTest(RegionHitTest&& callback) noexcept override {
        regionHitTest_ = Move(callback);
    }

    void SetLiveResizeHandler(LiveResizeHandler&& handler) noexcept override {
        liveResize_ = Move(handler);
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

        switch (shape) {
        case input::MouseCursor::Text:
            resource = reinterpret_cast<LPCWSTR>(IDC_IBEAM);
            break;
        case input::MouseCursor::Hand:
            resource = reinterpret_cast<LPCWSTR>(IDC_HAND);
            break;
        case input::MouseCursor::Crosshair:
            resource = reinterpret_cast<LPCWSTR>(IDC_CROSS);
            break;
        case input::MouseCursor::Move:
            resource = reinterpret_cast<LPCWSTR>(IDC_SIZEALL);
            break;
        case input::MouseCursor::ResizeNorthWestSouthEast:
            resource = reinterpret_cast<LPCWSTR>(IDC_SIZENWSE);
            break;
        case input::MouseCursor::ResizeNorthEastSouthWest:
            resource = reinterpret_cast<LPCWSTR>(IDC_SIZENESW);
            break;
        case input::MouseCursor::Unavailable:
            resource = reinterpret_cast<LPCWSTR>(IDC_NO);
            break;
        case input::MouseCursor::Wait:
            resource = reinterpret_cast<LPCWSTR>(IDC_WAIT);
            break;
        default:
            break;
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

    Result<IntRect> WorkAreaAt(IntVec2 clientPoint) const noexcept override {
        POINT p{clientPoint.x, clientPoint.y};
        if (!::ClientToScreen(window_, &p)) {
            return Result<IntRect>::Failure(ErrorCode::DependencyFailed);
        }

        MONITORINFO info{};
        info.cbSize = sizeof(info);
        const HMONITOR monitor = ::MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
        if (monitor == nullptr || !::GetMonitorInfoW(monitor, &info)) {
            return Result<IntRect>::Failure(ErrorCode::DependencyFailed);
        }

        // 전체 화면 창은 작업 표시줄을 덮으므로 모니터 전체가 팝업 영역이다.
        RECT area = fullscreenStyle_ != 0 ? info.rcMonitor : info.rcWork;
        POINT corners[2] = {{area.left, area.top}, {area.right, area.bottom}};
        // 창이 화면 원점에 있으면 정상 결과도 0이므로 오류 코드로 구분한다.
        ::SetLastError(0);
        if (::MapWindowPoints(nullptr, window_, corners, 2) == 0 && ::GetLastError() != 0) {
            return Result<IntRect>::Failure(ErrorCode::DependencyFailed);
        }

        return Result<IntRect>::Success(
            IntRect{{static_cast<int32>(corners[0].x), static_cast<int32>(corners[0].y)},
                    {static_cast<int32>(corners[1].x), static_cast<int32>(corners[1].y)}});
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

    void Minimize() noexcept override {
        ShowWindow(window_, SW_MINIMIZE);
    }

    void ToggleMaximize() noexcept override {
        // 크기 고정 창은 최대화하지 않는다(UE HasMaximizeBox).
        if (resizable_) {
            ShowWindow(window_, IsZoomed(window_) ? SW_RESTORE : SW_MAXIMIZE);
        }
    }

    bool IsResizable() const noexcept override {
        return resizable_;
    }

    Result<void> SetWindowMode(WindowMode mode) noexcept override {
        AssertOwnerThread();
        // 독점 전체 화면은 렌더러 스왑 체인 몫이라 Fullscreen도 테두리 없는 전체 화면이다(19.2절).
        const bool fullscreen = mode != WindowMode::Windowed;
        if (fullscreen != (fullscreenStyle_ != 0)) {
            ToggleBorderlessFullscreen();
        }

        return Result<void>::Success();
    }

    WindowMode GetWindowMode() const noexcept override {
        return fullscreenStyle_ != 0 ? WindowMode::WindowedFullscreen : WindowMode::Windowed;
    }

    // 최대화된 client 사각형에서 자동 숨김 작업 표시줄이 붙은 가장자리를 1 px 남긴다. 창이 화면을
    // 꽉 채우면 OS가 그 작업 표시줄을 불러내지 못한다.
    void LeaveAutoHideTaskbarEdge(RECT& client) const noexcept {
        APPBARDATA query{};
        query.cbSize = sizeof(query);
        if ((SHAppBarMessage(ABM_GETSTATE, &query) & ABS_AUTOHIDE) == 0) {
            return;
        }

        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (!GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) {
            return;
        }

        const UINT edges[] = {ABE_LEFT, ABE_TOP, ABE_RIGHT, ABE_BOTTOM};
        for (const UINT edge : edges) {
            APPBARDATA bar{};
            bar.cbSize = sizeof(bar);
            bar.uEdge = edge;
            bar.rc = monitor.rcMonitor;
            if (!SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &bar)) {
                continue;
            }

            switch (edge) {
            case ABE_LEFT:
                client.left += AutoHideTaskbarGap;
                break;
            case ABE_TOP:
                client.top += AutoHideTaskbarGap;
                break;
            case ABE_RIGHT:
                client.right -= AutoHideTaskbarGap;
                break;
            default:
                client.bottom -= AutoHideTaskbarGap;
                break;
            }
        }
    }

    void RequestClose() noexcept override {
        PostMessageW(window_, WM_CLOSE, 0, 0);
    }

    bool IsMaximized() const noexcept override {
        return IsZoomed(window_) != FALSE;
    }

    bool IsActive() const noexcept override {
        return active_;
    }

    WindowFrame Frame() const noexcept override {
        return customFrame_ ? WindowFrame::Custom : WindowFrame::System;
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
        pending_.Push(event);
    }

    // 창 객체는 만든 스레드(게임 lane)에서만 쓴다(Docs/Design/UiToolkit 19.2절 스레드 계약).
    void AssertOwnerThread() const noexcept {
#if !ROCKS_BUILD_RELEASE
        ROCKS_ASSERT(ownerThread_ == CurrentThreadId());
#endif
    }

    void Link() noexcept {
        ownerThread_ = CurrentThreadId();
        next_ = threadWindows;
        threadWindows = this;
    }

    void Unlink() noexcept {
        for (Win64Window** link = &threadWindows; *link; link = &(*link)->next_) {
            if (*link == this) {
                *link = next_;
                break;
            }
        }

        next_ = nullptr;
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

    void MouseButton(input::MouseButton button, bool down, LPARAM position,
                     uint8 clicks = 1) noexcept {
        const float x = static_cast<float>(static_cast<int16>(LOWORD(position)));
        const float y = static_cast<float>(static_cast<int16>(HIWORD(position)));
        // 버튼을 누른 채 창 밖으로 나가도 놓기 이벤트를 받도록 캡처한다.
        const uint32 bit = 1u << static_cast<uint32>(button);
        if (down) {
            if (!capturedButtons_) {
                SetCapture(window_);
            }

            capturedButtons_ |= bit;
            Push(input::InputEvent::MouseDown(button, x, y, clicks));
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
            // 표지를 먼저 지운다 — 아래 SWP_FRAMECHANGED의 WM_NCCALCSIZE가 직접 그린 창 규칙(OS
            // 테두리 없음)을 따라야 한다.
            const LONG_PTR windowedStyle = fullscreenStyle_;
            fullscreenStyle_ = 0;
            SetWindowLongPtrW(window_, GWL_STYLE, windowedStyle);
            SetWindowPlacement(window_, &windowedPlacement_);
            SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
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

    WindowZone HitZone(IntVec2 clientPoint) const noexcept override {
        const input::WindowRegion region =
            regionHitTest_ ? regionHitTest_.Execute(clientPoint) : input::WindowRegion::Client;
        // OS 테두리 창은 UI 영역만 본다: client·팝업 말고는 끌기 영역이다.
        if (!customFrame_) {
            return region == input::WindowRegion::Client || region == input::WindowRegion::Popup
                       ? WindowZone::Client
                       : WindowZone::Caption;
        }

        // 팝업 위 지점은 크기 조절 가장자리보다 앞선다.
        if (region == input::WindowRegion::Popup) {
            return WindowZone::Client;
        }

        RECT client{};
        if (resizable_ && !IsZoomed(window_) && !fullscreenStyle_ &&
            GetClientRect(window_, &client)) {
            const UINT dpi = GetDpiForWindow(window_);
            const int border = MulDiv(CustomFrameResizeBorder, dpi, USER_DEFAULT_SCREEN_DPI);
            const int corner =
                border + MulDiv(CustomFrameCornerExtension, dpi, USER_DEFAULT_SCREEN_DPI);
            const int x = clientPoint.x;
            const int y = clientPoint.y;
            const bool left = x < client.left + border;
            const bool right = x >= client.right - border;
            const bool top = y < client.top + border;
            const bool bottom = y >= client.bottom - border;
            // 한 축이 가장자리 안이면 다른 축은 모서리 폭(두께 + 확장)까지 모서리로 본다.
            const bool nearLeft = x < client.left + corner;
            const bool nearRight = x >= client.right - corner;
            const bool nearTop = y < client.top + corner;
            const bool nearBottom = y >= client.bottom - corner;
            if ((top && nearLeft) || (left && nearTop)) {
                return WindowZone::TopLeftBorder;
            }

            if ((top && nearRight) || (right && nearTop)) {
                return WindowZone::TopRightBorder;
            }

            if ((bottom && nearLeft) || (left && nearBottom)) {
                return WindowZone::BottomLeftBorder;
            }

            if ((bottom && nearRight) || (right && nearBottom)) {
                return WindowZone::BottomRightBorder;
            }

            if (left) {
                return WindowZone::LeftBorder;
            }

            if (right) {
                return WindowZone::RightBorder;
            }

            if (top) {
                return WindowZone::TopBorder;
            }

            if (bottom) {
                return WindowZone::BottomBorder;
            }
        }

        switch (region) {
        case input::WindowRegion::Caption:
            return WindowZone::Caption;
        case input::WindowRegion::MinimizeButton:
            return WindowZone::MinimizeButton;
        case input::WindowRegion::MaximizeButton:
            return WindowZone::MaximizeButton;
        case input::WindowRegion::CloseButton:
            return WindowZone::CloseButton;
        case input::WindowRegion::SystemMenu:
            return WindowZone::SystemMenu;
        default:
            return WindowZone::Client;
        }
    }

    // 직접 그린 창의 비클라이언트 판정이다. 최대화 버튼만 HTMAXBUTTON(Win11 스냅 레이아웃은 그
    // hover에만 뜬다 — 크기 고정 창은 최대화가 없어 HTCLIENT)이고, 최소화·닫기는 HTCLIENT로 두어
    // UI 버튼이 평소 입력으로 처리한다(Docs/Design/UiToolkit 9.1절 UW7b).
    LRESULT CustomHitTest(LPARAM coordinates) const noexcept {
        POINT point{static_cast<short>(LOWORD(coordinates)),
                    static_cast<short>(HIWORD(coordinates))};
        if (!ScreenToClient(window_, &point)) {
            return HTNOWHERE;
        }

        switch (HitZone({point.x, point.y})) {
        case WindowZone::Caption:
            return HTCAPTION;
        case WindowZone::TopLeftBorder:
            return HTTOPLEFT;
        case WindowZone::TopBorder:
            return HTTOP;
        case WindowZone::TopRightBorder:
            return HTTOPRIGHT;
        case WindowZone::LeftBorder:
            return HTLEFT;
        case WindowZone::RightBorder:
            return HTRIGHT;
        case WindowZone::BottomLeftBorder:
            return HTBOTTOMLEFT;
        case WindowZone::BottomBorder:
            return HTBOTTOM;
        case WindowZone::BottomRightBorder:
            return HTBOTTOMRIGHT;
        case WindowZone::MaximizeButton:
            return resizable_ ? HTMAXBUTTON : HTCLIENT;
        case WindowZone::SystemMenu:
            return HTSYSMENU;
        default:
            return HTCLIENT;
        }
    }

    // 커서가 아직 이 창 위에 있는지다(클라이언트↔비클라이언트로 옮길 때 오는 leave를 거른다).
    bool CursorStillInside() const noexcept {
        POINT cursor{};
        return GetCursorPos(&cursor) && WindowFromPoint(cursor) == window_;
    }

    void PushMouseLeave() noexcept {
        input::InputEvent leave;
        leave.type = input::InputEventType::MouseLeave;
        Push(leave);
    }

    void PushNonClientPressed() noexcept {
        input::InputEvent pressed;
        pressed.type = input::InputEventType::NonClientPressed;
        Push(pressed);
    }

    // 최대화 버튼 위 비클라이언트 마우스 메시지를 클라이언트 입력으로 바꾼다(UI 버튼의 올림·눌림).
    // 눌린 뒤에는 캡처가 클라이언트 메시지로 이어진다.
    LPARAM ScreenToClientParam(LPARAM coordinates) const noexcept {
        POINT point{static_cast<short>(LOWORD(coordinates)),
                    static_cast<short>(HIWORD(coordinates))};
        (void)ScreenToClient(window_, &point);
        return MAKELPARAM(static_cast<WORD>(static_cast<short>(point.x)),
                          static_cast<WORD>(static_cast<short>(point.y)));
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) noexcept {
        switch (message) {
        case WM_NCCALCSIZE:
            // 직접 그린 창은 창 전체가 client다. 최대화 중에는 OS가 화면 밖으로 내미는 테두리
            // 두께만큼 안으로 들인다(UE WindowsApplication.cpp 2490–2520행).
            if (customFrame_ && wParam && !fullscreenStyle_) {
                if (IsZoomed(window_)) {
                    WINDOWINFO info{};
                    info.cbSize = sizeof(info);
                    if (GetWindowInfo(window_, &info)) {
                        auto* rects = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                        const LONG inset = static_cast<LONG>(info.cxWindowBorders);
                        rects->rgrc[0].left += inset;
                        rects->rgrc[0].top += inset;
                        rects->rgrc[0].right -= inset;
                        rects->rgrc[0].bottom -= inset;
                        LeaveAutoHideTaskbarEdge(rects->rgrc[0]);
                        rects->rgrc[1] = rects->rgrc[0];
                        return WVR_VALIDRECTS;
                    }
                }

                return 0;
            }
            break;
        case WM_NCACTIVATE:
            active_ = wParam != FALSE;
            repaint_ = true;
            // 직접 그린 창은 OS가 옛 비클라이언트 틀을 다시 그리지 않게 한다.
            if (customFrame_) {
                return DefWindowProcW(window_, message, wParam, -1);
            }
            break;
        case WM_NCMOUSEMOVE:
            // 직접 그린 창은 비클라이언트(제목·가장자리) 이동도 UI에 보낸다 — 최대화 버튼에서 제목
            // 빈 곳으로 옮기면 올림이 풀려야 한다(UE WindowsApplication.cpp 3527행). 기본 처리는
            // 최대화 버튼만 막는다(OS 버튼 추적 대신 UI 버튼).
            if (customFrame_) {
                if (!trackingNonClient_) {
                    TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE | TME_NONCLIENT,
                                             window_, 0};
                    trackingNonClient_ = TrackMouseEvent(&tracking) != FALSE;
                }

                const LPARAM local = ScreenToClientParam(lParam);
                Push(input::InputEvent::MouseMove(
                    static_cast<float>(static_cast<int16>(LOWORD(local))),
                    static_cast<float>(static_cast<int16>(HIWORD(local)))));
                if (wParam == HTMAXBUTTON) {
                    return 0;
                }
            }
            break;
        case WM_NCMOUSELEAVE:
            if (customFrame_) {
                trackingNonClient_ = false;
                // 클라이언트로 옮겨 간 것이면 버린다(뒤따르는 이동이 올림을 다시 정한다).
                if (!CursorStillInside()) {
                    PushMouseLeave();
                }

                return 0;
            }
            break;
        case WM_NCLBUTTONDOWN:
            if (customFrame_ && wParam == HTMAXBUTTON) {
                MouseButton(input::MouseButton::Left, true, ScreenToClientParam(lParam));
                return 0;
            }

            // 제목·가장자리 누름은 열린 메뉴·툴팁을 닫는다(기본 처리로 창 옮기기·크기 조절은
            // 이어진다).
            PushNonClientPressed();
            // 전체 화면 창은 제목을 끌어도 움직이지 않는다(모니터를 덮은 채로 — VS Code·Edge F11).
            // 전체 화면에서 UI 제목 줄은 Client로 답하고 두 번 누르기를 위젯이 받는다(19.2절).
            if (fullscreenStyle_ && wParam == HTCAPTION) {
                return 0;
            }
            break;
        case WM_NCRBUTTONDOWN:
            PushNonClientPressed();
            break;
        case WM_NCLBUTTONUP:
            if (customFrame_ && wParam == HTMAXBUTTON) {
                MouseButton(input::MouseButton::Left, false, ScreenToClientParam(lParam));
                return 0;
            }
            break;
        case WM_NCHITTEST: {
            if (customFrame_ && !fullscreenStyle_) {
                return CustomHitTest(lParam);
            }

            const auto frame = DefWindowProcW(window_, message, wParam, lParam);
            const input::WindowRegion region =
                frame == HTCLIENT ? ClientRegion(lParam) : input::WindowRegion::Client;
            if (region == input::WindowRegion::Caption) {
                return HTCAPTION;
            }

            return frame;
        }
        case WM_NCLBUTTONDBLCLK:
            // 최대화 버튼의 빠른 두 번 누름은 두 번째 누름으로 보낸다(UI 버튼이 처리).
            if (customFrame_ && wParam == HTMAXBUTTON) {
                MouseButton(input::MouseButton::Left, true, ScreenToClientParam(lParam));
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
        case WM_ENTERSIZEMOVE:
            sizeMoveLoop_ = true;
            PushNonClientPressed();
            return 0;
        case WM_EXITSIZEMOVE:
            sizeMoveLoop_ = false;
            return 0;
        case WM_SIZE:
            repaint_ = true;
            minimized_ = wParam == SIZE_MINIMIZED;
            width_ = LOWORD(lParam);
            height_ = HIWORD(lParam);
            resized_ = true;
            // modal 루프 안에서는 호스트 루프가 멈춰 있으므로 여기서 바로 그리게 한다(UW7a).
            if (sizeMoveLoop_ && !minimized_ && !liveResizing_ && liveResize_.IsBound()) {
                liveResizing_ = true;
                liveResize_.Execute();
                liveResizing_ = false;
            }

            return 0;
        case WM_SETFOCUS:
            focused_ = true;
            return 0;
        case WM_KILLFOCUS: {
            focused_ = false;
            capturedButtons_ = 0;
            if (GetCapture() == window_) {
                ReleaseCapture();
            }

            // 닿아 있던 손가락은 FocusLost가 InputState에서 취소로 끝낸다. 배정기도 같이 비운다.
            uint8 ended[input::MaxTouches];
            (void)touches_.ReleaseAll(ended);
            Push(input::InputEvent::FocusLost());
            return 0;
        }
        case WM_CAPTURECHANGED:
            // 다른 창·시스템이 버튼을 누른 채인 capture를 가져갔다. 뗌이 오지 않으므로 취소를
            // 보낸다 (UE OnMouseCaptureLost — 클릭으로 처리하지 않는다). 마지막 버튼을 놓아 스스로
            // 해제할 때는 이미 0이라 보내지 않는다.
            if (capturedButtons_ != 0) {
                capturedButtons_ = 0;
                Push(input::InputEvent::CaptureLost());
            }

            return 0;
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
        case WM_SYSCOMMAND:
            // F10·Alt·Alt+글자는 OS 메뉴 모드(SC_KEYMENU)로 가지 않는다 — UI 메뉴 막대 가속키가
            // 받고, 메뉴 없는 창의 경고음도 없다(UE도 처리한 WM_SYSKEYDOWN에 0). Alt+Space(lParam '
            // ')는 시스템 메뉴를 그대로 연다.
            if ((wParam & SysCommandMask) == SC_KEYMENU && lParam != SystemMenuKey) {
                return 0;
            }
            break;
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
                RouteInlineComposition(window_, lParam, allocator_, pending_)) {
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
            // 직접 그린 창에서 비클라이언트(제목·최대화 버튼)로 옮겨 간 것이면 버린다 —
            // 비클라이언트 이동이 이어서 온다.
            if (!customFrame_ || !CursorStillInside()) {
                PushMouseLeave();
            }

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
            MouseButton(input::MouseButton::Left, message != WM_LBUTTONUP, lParam,
                        message == WM_LBUTTONDBLCLK ? DoubleClickCount : 1);
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

    Allocator& allocator_;
    HWND window_ = nullptr;
    HCURSOR cursor_ = nullptr;
    RegionHitTest regionHitTest_;
    LONG_PTR fullscreenStyle_ = 0;
    WINDOWPLACEMENT windowedPlacement_{};
    bool trackingMouse_ = false;
    // 최대화 버튼 위 비클라이언트 이동을 추적 중인지다(WM_NCMOUSELEAVE).
    bool trackingNonClient_ = false;
    bool customFrame_ = false;
    bool resizable_ = true;
    bool active_ = false;
    input::MouseCursor cursorShape_ = input::MouseCursor::Arrow;
    HINSTANCE instance_ = nullptr;
    input::InputQueue pending_;
    // 이 스레드의 창 목록 다음 창과 만든 스레드다.
    Win64Window* next_ = nullptr;
    uint64 ownerThread_ = 0;
    // 터치·펜 포인터의 손가락 번호 배정이다(창 스레드만 만진다).
    input::TouchIndexAllocator touches_;
    uint32 width_ = 0;
    uint32 height_ = 0;
    uint32 highSurrogate_ = 0;
    uint32 capturedButtons_ = 0;
    bool closeRequested_ = false;
    bool resized_ = false;
    // WM_ENTERSIZEMOVE와 WM_EXITSIZEMOVE 사이(크기·이동 modal 루프)인지, 실시간 그리기 콜백
    // 중인지다.
    bool sizeMoveLoop_ = false;
    bool liveResizing_ = false;
    LiveResizeHandler liveResize_{};
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

bool Window::SupportsFrame(WindowFrame frame) noexcept {
    return frame == WindowFrame::System || frame == WindowFrame::Custom;
}

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

void PumpMessages() noexcept {
    Win64Window::PumpThreadMessages();
}

void WaitForMessages(uint32 milliseconds) noexcept {
    // 다른 창 메시지를 처리하는 사이 쌓인 대기 이벤트가 있으면 기다리지 않는다(19.1절 결함 — OS
    // 큐는 비어 있어도 그 이벤트가 아직 호스트로 가지 않았다).
    if (Win64Window::AnyPendingEvents()) {
        return;
    }

    (void)MsgWaitForMultipleObjectsEx(0, nullptr, milliseconds, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}

bool HasPendingWindowEvents() noexcept {
    return Win64Window::AnyPendingEvents();
}

void WakeMessageWait() noexcept {
    const LONG thread = InterlockedCompareExchange(&pumpThread, 0, 0);
    if (thread != 0) {
        // 빈 메시지 하나로 대기를 푼다(다음 PumpMessages가 버린다).
        (void)PostThreadMessageW(static_cast<DWORD>(thread), WM_NULL, 0, 0);
    }
}

void PumpSentMessages() noexcept {
    // PM_QS_SENDMESSAGE로 보낸 메시지만 디스패치한다(PeekMessage가 대기 중인 보낸 메시지를 먼저
    // 처리한다). PM_NOREMOVE라 게시 메시지는 큐에 남는다.
    MSG message;
    (void)PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
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
