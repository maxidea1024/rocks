#pragma once

#include <Base/Types.h>

namespace rocks::ui {

// 누른 뒤 이만큼(px) 움직이면 끌기로 본다(UE FSlateApplication DragTriggerDistance 5).
inline constexpr float PointerDragThreshold = 5.0f;
// 휠 한 칸의 스크롤 거리(px)다(UE SScrollBox 휠 배율 기본값의 거리).
inline constexpr float WheelScrollStep = 48.0f;
// 목록의 휠 한 칸 스크롤 행 수다.
inline constexpr double WheelScrollRows = 3.0;

} // namespace rocks::ui
