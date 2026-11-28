#pragma once

#include <Base/Types.h>

namespace rocks::ui {

// 스크롤 막대를 내용 옆 칸에 둘지 내용 위에 둘지다(Docs/Design/UiToolkit 23.6절).
enum class ScrollBarPlacement : uint8 {
    // 막대 칸(두께 + 막대 여백)을 따로 두어 내용이 줄어든다(UE SScrollBox 기본).
    Reserve,
    // 내용 위에 겹친다.
    Overlay
};

} // namespace rocks::ui
