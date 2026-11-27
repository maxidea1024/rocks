#pragma once

#include <UiCore/Styles/StyleBrush.h>

namespace rocks::ui {

// 도킹 탭 스타일이다(UE FDockTabStyle). 탭 모양은 UE 이미지 "DockTab_Foreground"(위가 둥근 탭)를
// 위쪽 반경 4의 둥근 상자로 그린다(Docs/Design/UiToolkit 9.1절 UW4). 여백은 세로에
// verticalPaddingScale을 곱해 쓴다(UE SDockTabDefs::TabVerticalPaddingScaleOverride).
struct DockTabStyle {
    StyleBrush normal{};
    StyleBrush hovered{};
    StyleBrush foreground{};
    StyleBrush contentArea{};
    StyleBrush tabWell{};
    Vec4 tabPadding{};
    float verticalPaddingScale = 1.0f;
    // 탭 사이 간격이다(UE OverlapWidth가 음수일 때의 크기 — UE는 -2로 2만큼 띄운다).
    float tabGap = 0.0f;
    // 탭 하나의 최대 크기다. 한 탭 줄의 탭은 모두 같은 너비다(UE
    // SDockingTabWell::ComputeChildSize).
    Vec2 maxTabSize{};
    ColorRef normalForeground{ColorSource::Foreground};
    ColorRef hoveredForeground{ColorSource::Foreground};
    // 앞에 보이는 탭과 키보드 초점이 있는 탭의 전경색이다(UE
    // ForegroundForeground·ActiveForeground).
    ColorRef foregroundForeground{ColorSource::Foreground};
    ColorRef activeForeground{ColorSource::Foreground};
    bool operator==(const DockTabStyle&) const noexcept = default;
};

} // namespace rocks::ui
