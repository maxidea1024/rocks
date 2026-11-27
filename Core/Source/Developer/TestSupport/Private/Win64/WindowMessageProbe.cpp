#include <TestSupport/WindowMessageProbe.h>

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

namespace rocks::test {

bool SupportsWindowMessages() noexcept {
    return true;
}

bool PostWindowDoubleClick(void* nativeWindow, int32 x, int32 y) noexcept {
    return nativeWindow != nullptr &&
           PostMessageW(static_cast<HWND>(nativeWindow), WM_LBUTTONDBLCLK, MK_LBUTTON,
                        MAKELPARAM(x, y)) != FALSE;
}

bool SendWindowDpiChanged(void* nativeWindow, uint32 dpi, int32 width, int32 height) noexcept {
    const auto window = static_cast<HWND>(nativeWindow);
    RECT current{};
    if (window == nullptr || !GetWindowRect(window, &current)) {
        return false;
    }

    RECT suggested{current.left, current.top, current.left + width, current.top + height};
    (void)SendMessageW(window, WM_DPICHANGED, MAKEWPARAM(dpi, dpi),
                       reinterpret_cast<LPARAM>(&suggested));
    return true;
}

bool HasThreadWakeMessage() noexcept {
    MSG message;
    return PeekMessageW(&message, nullptr, WM_NULL, WM_NULL, PM_NOREMOVE) != FALSE;
}

} // namespace rocks::test
