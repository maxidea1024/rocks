#pragma once

#include <UiCore/ColorRef.h>
#include <UiCore/Styles/StyleBrush.h>

namespace rocks::ui {

// 숫자 입력(스핀 상자) 스타일이다(UE FSpinBoxStyle — Docs/Design/UiToolkit 24.5절). 바탕은
// 보통·올림· 활성(끄는 중·글자 모드), 채움 막대는 보통·올림·활성이다. 활성 바탕이 None이면 올림
// 바탕을, 올림 채움이 None이면 활성 채움을 쓴다(UE BackgroundActiveBrush·HoveredFillBrush IsSet
// 규칙).
struct SpinBoxStyle {
    StyleBrush background{};
    StyleBrush hoveredBackground{};
    StyleBrush activeBackground{};
    StyleBrush activeFill{};
    StyleBrush hoveredFill{};
    StyleBrush inactiveFill{};
    // 오른쪽 화살표 이미지다(UE ArrowsImage — Starship 기본은 없음).
    StyleBrush arrowsImage{};
    ColorRef foreground{};
    // 글자 칸 여백(왼·위·오른·아래)과 채움 막대 안쪽 여백이다(UE TextPadding·InsetPadding).
    Vec4 textPadding{};
    Vec4 insetPadding{};
    bool operator==(const SpinBoxStyle&) const noexcept = default;
};

} // namespace rocks::ui
