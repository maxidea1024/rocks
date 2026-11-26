#pragma once

#include <Core/Math/Vector4.h>
#include <UiCore/Styles/ButtonStyle.h>
#include <UiCore/Styles/TextStyle.h>
#include <UiCore/VectorImageKey.h>

namespace rocks::ui {

// 펼친 메뉴 스타일이다(UE 핵심 스타일 "Menu.*" — Docs/Design/UiToolkit 9.1절 UW7c). 항목 한 줄은
// [아이콘·체크 칸][라벨][단축키][하위 메뉴 표시]이고 rowPadding 안에 놓인다(UE SMenuEntryBlock
// 배치). 항목 강조와 패널은 둥근 상자이고 항목은 패널 안쪽 panelPadding만큼 들여 놓는다(UE는 각진
// 강조가 패널 끝까지 닿는다 — 강조 경계가 또렷한 VS Code식으로 개선).
struct MenuStyle {
    StyleBrush background{};
    StyleBrush outline{};
    Vec4 panelPadding{};
    ButtonStyle row{};
    Vec4 rowPadding{};
    float iconSize = 0.0f;
    Vec4 iconPadding{};
    Vec4 labelPadding{};
    float keybindingGap = 0.0f;
    Vec4 keybindingPadding{};
    float subMenuGap = 0.0f;
    float subMenuIconSize = 0.0f;
    StyleBrush separator{};
    float separatorThickness = 0.0f;
    Vec4 separatorPadding{};
    TextStyle label{};
    TextStyle keybinding{};
    TextStyle heading{};
    Vec4 headingPadding{};
    VectorImageKey checkIcon{};
    VectorImageKey subMenuIcon{};
    StyleBrush radioOff{};
    StyleBrush radioOn{};
    float radioSize = 0.0f;
    bool operator==(const MenuStyle&) const noexcept = default;
};

} // namespace rocks::ui
