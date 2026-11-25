#pragma once

#include <Input/InputEvent.h>
#include <Platform/Win64/WindowsHeadersBegin.h>
#include <Windows.h>
#include <Platform/Win64/WindowsHeadersEnd.h>

namespace rocks::platform {

bool IsInlineCompositionWindow(HWND window) noexcept;
bool RouteInlineComposition(HWND window, LPARAM flags, Allocator& allocator,
                            input::InputQueue& queue) noexcept;

} // namespace rocks::platform
