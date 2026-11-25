#pragma once

#include <UiCore/Styles/ButtonStyle.h>
#include <UiCore/Styles/CheckBoxStyle.h>
#include <UiCore/Styles/DockTabStyle.h>
#include <UiCore/Styles/TextStyle.h>

namespace rocks::ui {

// UE 5.8 에디터 핵심 스타일(14장)의 상수와 기본 스타일이다. 위젯은 스타일을 따로 받지 않으면 이
// 값을 쓴다.

// UE 5.8 에디터 핵심 스타일 상수다(14장 3절). 대응: FocusOutlineRadius = UE InputFocusRadius,
// ControlOutlineThickness = InputFocusThickness(CoreStyle.cpp 61–62행),
// ButtonPadding·PressedButtonPadding = ButtonMargins·PressedButtonMargins(54·56행),
// SimpleButtonPadding·PressedSimpleButtonPadding = SimpleButtonIconOnly 여백(StarshipCoreStyle.cpp
// 1493–1495행).
inline constexpr float FocusOutlineRadius = 4.0f;
inline constexpr float ControlOutlineThickness = 1.0f;
inline constexpr float ButtonCornerRadius = 4.0f;
inline constexpr float CheckBoxCornerRadius = 3.0f;
// UE SDockTabDefs::TabVerticalPaddingScaleOverride(SDockTab.cpp 30행)다.
inline constexpr float DockTabVerticalPaddingScale = 0.85f;
// UE "DockTab_Foreground" 이미지의 9분할 여백 4/20에 맞춘 탭 위쪽 반경이다(이미지는 UE 클론에 없어
// 모양은 미확인).
inline constexpr float DockTabCornerRadius = 4.0f;
// Panel 탭의 최대 크기다(UE FDockingConstants::MaxMinorTabSize — SDockingTabWell.cpp 9행).
inline constexpr Vec2 PanelTabMaxSize{160.0f, 25.0f};
inline constexpr float CheckBoxOutlineThickness = 1.0f;
inline constexpr Vec4 ButtonPadding{12.0f, 1.5f, 12.0f, 1.5f};
inline constexpr Vec4 PressedButtonPadding{12.0f, 2.5f, 12.0f, 0.5f};
inline constexpr Vec4 SimpleButtonPadding{2.0f, 1.0f, 2.0f, 1.0f};
inline constexpr Vec4 PressedSimpleButtonPadding{2.0f, 2.0f, 2.0f, 0.0f};

// 글꼴 크기(96 DPI 포인트)다(14장 2절 — StarshipCoreStyle.cpp 34–40행).
inline constexpr float NormalFontSize = 10.0f;
inline constexpr float SmallFontSize = 8.0f;
inline constexpr float LargeFontSize = 14.0f;

// 기본 버튼 스타일이다(UE 핵심 스타일 "Button" — StarshipCoreStyle.cpp 1460–1470행, 1506행).
ROCKS_UICORE_API const ButtonStyle& DefaultButtonStyle() noexcept;

// 주 동작 버튼 스타일이다(UE "PrimaryButton" — 1445–1455행).
ROCKS_UICORE_API const ButtonStyle& PrimaryButtonStyle() noexcept;

// 아이콘·단순 버튼 스타일이다(UE "SimpleButton" = SimpleButtonIconOnly — 1493–1495행, 1508행).
ROCKS_UICORE_API const ButtonStyle& SimpleButtonStyle() noexcept;

// 기본 체크 상자 스타일이다(UE "Checkbox" — StarshipCoreStyle.cpp 1661–1691행: 상자 RB(Input, 3,
// Secondary, 1) 18×18, 올림·눌림 외곽선 Hover, 표시 Primary·PrimaryHover·PrimaryPress, 전경
// ForegroundHover).
ROCKS_UICORE_API const CheckBoxStyle& DefaultCheckBoxStyle() noexcept;

// 패널 탭 스타일이다(UE "Docking.Tab" — StarshipCoreStyle.cpp 1876–1899행: 선택 탭 Panel, 올림
// Panel 80%, 내용 Panel, 탭 줄 Background, 여백 (4, 3, 10, 4), 간격 2, 전경 Foreground·올림
// ForegroundHover).
ROCKS_UICORE_API const DockTabStyle& DefaultDockTabStyle() noexcept;

// 보이지 않는 버튼 스타일이다(UE "NoBorder" — 232–240행, 252행: brush 없음, 전경 ForegroundHover,
// 여백 0). 펼침 영역 머리처럼 내용만 보이는 클릭 영역에 쓴다.
ROCKS_UICORE_API const ButtonStyle& NoBorderButtonStyle() noexcept;

// 보통 텍스트 스타일이다(UE "NormalText" — 1252–1258행: Regular 10, 상속 전경색).
ROCKS_UICORE_API const TextStyle& NormalTextStyle() noexcept;

} // namespace rocks::ui
