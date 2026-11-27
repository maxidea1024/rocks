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

bool HasThreadWakeMessage() noexcept {
    MSG message;
    return PeekMessageW(&message, nullptr, WM_NULL, WM_NULL, PM_NOREMOVE) != FALSE;
}

} // namespace rocks::test
