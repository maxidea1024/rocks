#pragma once

#include <UiCore/Styles/StyleBrush.h>

namespace rocks::ui {

// 버튼 스타일이다(UE FButtonStyle). 여백은 (왼쪽, 위, 오른쪽, 아래)다. 기본값은 UE FButtonStyle
// 생성자와 같다(brush 없음, 여백 0, 전경색 상속).
struct ButtonStyle {
    StyleBrush normal{};
    StyleBrush hovered{};
    StyleBrush pressed{};
    StyleBrush disabled{};
    Vec4 normalPadding{};
    Vec4 pressedPadding{};
    ColorRef normalForeground{ColorSource::Foreground};
    ColorRef hoveredForeground{ColorSource::Foreground};
    ColorRef pressedForeground{ColorSource::Foreground};
    ColorRef disabledForeground{ColorSource::Foreground};
    bool operator==(const ButtonStyle&) const noexcept = default;
};

} // namespace rocks::ui
