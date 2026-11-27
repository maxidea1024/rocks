#pragma once

#include <UiCore/Styles/StyleBrush.h>

namespace rocks::ui {

// 진행 막대 스타일이다(UE FProgressBarStyle). 바탕, 채움, marquee(진행률을 모를 때 움직이는 띠)다.
// UE 채움·marquee는 줄무늬 이미지(ProgressMarquee 20x12)를 가로로 깐 것이라, 이미지 경로가 생기기
// 전까지 같은 색 단색으로 그린다(Docs/Design/UiToolkit 9.1절 UW4b).
struct ProgressBarStyle {
    StyleBrush background{};
    StyleBrush fill{};
    StyleBrush marquee{};
    bool operator==(const ProgressBarStyle&) const noexcept = default;
};

} // namespace rocks::ui
