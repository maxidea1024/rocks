#pragma once

#include <Core/Text/String.h>

namespace rocks::platform {

// 현재 데스크톱 세션의 텍스트 클립보드와 UTF-8을 주고받는다. 윈도우 이벤트 스레드에서 호출한다.
// 읽기에 실패하면 output을 유지한다. 백엔드가 없는 플랫폼은 Unsupported를 반환한다.
ROCKS_PLATFORMWINDOW_API Result<void> ReadClipboardText(String& output) noexcept;
ROCKS_PLATFORMWINDOW_API Result<void> WriteClipboardText(StringView text) noexcept;

} // namespace rocks::platform
