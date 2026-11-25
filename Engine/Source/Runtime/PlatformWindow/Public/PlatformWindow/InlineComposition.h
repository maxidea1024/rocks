#pragma once
#include <Base/Types.h>

namespace rocks::platform {

// 현재 입력 창에서 조합 문자열을 UI에 전달한다. 비활성화하면 네이티브 IME 표시로 되돌린다.
ROCKS_PLATFORMWINDOW_API void SetInlineComposition(bool enabled) noexcept;
// 후보 목록을 현재 창의 클라이언트 좌표(물리 픽셀)로 옮긴다.
ROCKS_PLATFORMWINDOW_API void MoveCompositionWindow(int32 x, int32 y) noexcept;

} // namespace rocks::platform
