#pragma once

#include <UiCore/Styles/ButtonStyle.h>

namespace rocks::ui {

// 콤보 버튼 스타일이다(UE FComboButtonStyle — Docs/Design/UiToolkit 23.2절). 버튼 모양, 오른쪽 아래
// 화살표 이미지와 그 둘레 여백, 펼침 목록 바탕과 그 안쪽 여백을 담는다. 기본값은 UE
// FComboButtonStyle 생성자와 같다(SlateTypes.cpp 171–179행: 내용 여백 5, 화살표 여백 2, 목록 여백
// 0).
struct ComboButtonStyle {
    ButtonStyle button{};
    // 버튼 안 내용 여백이다(버튼 스타일 여백에 더한다 — UE ContentPadding).
    Vec4 contentPadding{5.0f, 5.0f, 5.0f, 5.0f};
    // 아래 화살표다(UE DownArrowImage — 흰 이미지에 전경색을 곱해 그린다).
    StyleBrush downArrow{};
    Vec4 downArrowPadding{2.0f, 2.0f, 2.0f, 2.0f};
    // 펼침 목록 바탕과 안쪽 여백이다(UE MenuBorderBrush·MenuBorderPadding).
    StyleBrush menuBorder{};
    Vec4 menuBorderPadding{};
    bool operator==(const ComboButtonStyle&) const noexcept = default;
};

} // namespace rocks::ui
