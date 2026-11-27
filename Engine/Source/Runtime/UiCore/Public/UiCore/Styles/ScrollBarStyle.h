#pragma once

#include <UiCore/Styles/StyleBrush.h>

namespace rocks::ui {

// 스크롤 막대 스타일이다(UE FScrollBarStyle). 손잡이를 상태(보통·올림·끌기)마다 두고, 트랙은 UE
// 기본처럼 그리지 않는다(None). thickness는 막대 두께다.
struct ScrollBarStyle {
    StyleBrush track{};
    StyleBrush thumb{};
    StyleBrush thumbHovered{};
    StyleBrush thumbDragged{};
    float thickness = 8.0f;
    bool operator==(const ScrollBarStyle&) const noexcept = default;
};

} // namespace rocks::ui
