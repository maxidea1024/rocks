#pragma once

#include <UiCore/Styles/StyleBrush.h>

namespace rocks::ui {

// 체크 상자 형식이다(UE ESlateCheckBoxType — Docs/Design/UiToolkit 23.3절).
enum class CheckBoxType : uint8 {
    // 왼쪽 상자(바탕 + 상태 이미지)와 오른쪽 내용이다.
    CheckBox,
    // 상태 이미지를 버튼 바탕으로 그리고 내용을 여백 안에 둔다(상자 칸 없음).
    ToggleButton
};

// 체크 상자 스타일이다(UE FCheckBoxStyle). 상태 이미지는 해제·체크·미정 × 보통·올림·눌림이다
// (UE UncheckedImage·CheckedImage·UndeterminedImage 등 — 기본 체크 상자는 SVG 아이콘
// Check·Indeterminate, 라디오는 RadioOn·RadioOff, Docs/Design/UiToolkit 17.4·23.3절).
// - CheckBox 형식: 상자는 boxSize이고 바탕 위에 상태 이미지를 상자 전체 크기로 그린다(UE 표시
//   SImage가 SOverlay 기본 Fill로 상자만큼 늘어난다). 내용은 그 오른쪽에 padding만큼 띄운다.
// - ToggleButton 형식: 상태 이미지를 요소 전체(버튼 바탕)에 borderBackgroundColor tint로 그리고
//   내용은 padding 안에 둔다(UE SCheckBox::BuildCheckBox 574–588행).
struct CheckBoxStyle {
    CheckBoxType type = CheckBoxType::CheckBox;
    StyleBrush uncheckedImage{};
    StyleBrush uncheckedHoveredImage{};
    StyleBrush uncheckedPressedImage{};
    // 토글 형식 바탕 tint다(UE BorderBackgroundColor — 기본 White).
    ColorRef borderBackgroundColor = ColorRef::Specified({1, 1, 1, 1});
    StyleBrush background{};
    StyleBrush backgroundHovered{};
    StyleBrush backgroundPressed{};
    StyleBrush checkedImage{};
    StyleBrush checkedHoveredImage{};
    StyleBrush checkedPressedImage{};
    StyleBrush undeterminedImage{};
    StyleBrush undeterminedHoveredImage{};
    StyleBrush undeterminedPressedImage{};
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
