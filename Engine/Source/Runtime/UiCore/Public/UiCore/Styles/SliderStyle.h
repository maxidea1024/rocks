#pragma once

#include <UiCore/Styles/StyleBrush.h>

namespace rocks::ui {

// 슬라이더 스타일이다(UE FSliderStyle). 막대와 손잡이를 상태(보통·올림·비활성)마다 둔다. 막대는
// barThickness 높이로 가운데에, 손잡이는 thumbSize로 값 위치에 그린다. UE 기본 슬라이더는 채움
// 막대가 없다(값은 손잡이 위치로만 보인다).
struct SliderStyle {
    StyleBrush bar{};
    StyleBrush barHovered{};
    StyleBrush barDisabled{};
    StyleBrush thumb{};
    StyleBrush thumbHovered{};
    StyleBrush thumbDisabled{};
    float barThickness = 4.0f;
    Vec2 thumbSize{8.0f, 8.0f};
    bool operator==(const SliderStyle&) const noexcept = default;
};

} // namespace rocks::ui
