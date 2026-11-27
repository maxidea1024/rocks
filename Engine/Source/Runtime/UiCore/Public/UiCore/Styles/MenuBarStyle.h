#pragma once

#include <UiCore/Styles/ButtonStyle.h>
#include <UiCore/Styles/TextStyle.h>

namespace rocks::ui {

// 메뉴 막대 스타일이다(UE "WindowMenuBar.*" — StarshipCoreStyle.cpp 2640–2653행). 막대 버튼은 바탕
// 없이 올림만 Hover이고, 메뉴가 열린 버튼은 openBrush(Primary)로 그린다. 강조는 둥근 상자로 버튼
// 글자 높이(여백 포함)만큼 세로 가운데에 그린다 — 누름 영역은 막대 높이 전체다(UE는 막대 높이
// 전체를 각지게 칠한다).
struct MenuBarStyle {
    ButtonStyle button{};
    StyleBrush openBrush{};
    TextStyle label{};
    bool operator==(const MenuBarStyle&) const noexcept = default;
};

} // namespace rocks::ui
