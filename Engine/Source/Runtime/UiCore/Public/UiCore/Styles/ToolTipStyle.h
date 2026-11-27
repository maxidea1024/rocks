#pragma once

#include <Core/Math/Vector4.h>
#include <UiCore/Styles/StyleBrush.h>
#include <UiCore/Styles/TextStyle.h>

namespace rocks::ui {

// 툴팁 스타일이다(UE SToolTip의 BorderImage·TextMargin·Font·WrapTextAt). padding은 왼쪽·위·
// 오른쪽·아래 여백이고, wrapWidth는 글자를 줄바꿈하는 최대 폭(논리 단위)이다.
//
// 제목·단축키·설명으로 짠 툴팁(ToolTip::Args의 title·shortcut·description)은 아래 값을 쓴다: 제목
// 글자, 단축키 표지(바탕·여백·글자, 제목 오른쪽 shortcutGap 띄움), 설명 글자(제목 아래 lineGap
// 띄움). 글꼴은 호출자가 이 스타일의 font로 shaping하고, 색은 ToolTip이 레이블에 적용한다.
struct ToolTipStyle {
    StyleBrush background{};
    Vec4 padding{};
    TextStyle text{};
    float wrapWidth = 0.0f;
    TextStyle title{};
    TextStyle description{};
    TextStyle shortcut{};
    StyleBrush shortcutBackground{};
    Vec4 shortcutPadding{};
    float shortcutGap = 0.0f;
    float lineGap = 0.0f;
    bool operator==(const ToolTipStyle&) const noexcept = default;
};

} // namespace rocks::ui
