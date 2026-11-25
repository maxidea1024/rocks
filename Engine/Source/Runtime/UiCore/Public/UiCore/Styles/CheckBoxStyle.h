#pragma once

#include <UiCore/Styles/StyleBrush.h>

namespace rocks::ui {

// 체크 상자 스타일이다(UE FCheckBoxStyle의 CheckBox 형식). 체크·미정 표시 색은 UE SVG 아이콘
// ("Starship/CoreWidgets/CheckBox/check"·"indeterminate")의 tint다 — SVG가 생기기 전까지 같은 색의
// 선으로 상자 전체 크기에 그린다(UE 표시 SImage가 SOverlay 기본 Fill로 상자만큼 늘어난다,
// Docs/Design/UiToolkit 9.1절 UW4). 상자는 boxSize이고 내용은 그 오른쪽에 padding만큼 띄운다.
struct CheckBoxStyle {
    StyleBrush background{};
    StyleBrush backgroundHovered{};
    StyleBrush backgroundPressed{};
    ColorRef checkedColor{ColorSource::Foreground};
    ColorRef checkedHoveredColor{ColorSource::Foreground};
    ColorRef checkedPressedColor{ColorSource::Foreground};
    ColorRef undeterminedColor{ColorSource::Foreground};
    ColorRef undeterminedHoveredColor{ColorSource::Foreground};
    ColorRef undeterminedPressedColor{ColorSource::Foreground};
    Vec2 boxSize{18.0f, 18.0f};
    // 상자와 내용 사이 여백이다(UE FCheckBoxStyle 기본 (2, 0, 0, 0) — SlateTypes.cpp 24행).
    Vec4 padding{2.0f, 0.0f, 0.0f, 0.0f};
    ColorRef foreground{ColorSource::Foreground};
    ColorRef hoveredForeground{ColorSource::Foreground};
    ColorRef pressedForeground{ColorSource::Foreground};
    ColorRef checkedForeground{ColorSource::Foreground};
    ColorRef checkedHoveredForeground{ColorSource::Foreground};
    ColorRef checkedPressedForeground{ColorSource::Foreground};
    ColorRef undeterminedForeground{ColorSource::Foreground};
    bool operator==(const CheckBoxStyle&) const noexcept = default;
};

} // namespace rocks::ui
