#pragma once

#include <UiCore/Styles/ButtonStyle.h>
#include <UiCore/Styles/CheckBoxStyle.h>
#include <UiCore/Styles/DockTabStyle.h>
#include <UiCore/Styles/EditableTextBoxStyle.h>
#include <UiCore/Styles/MenuBarStyle.h>
#include <UiCore/Styles/MenuStyle.h>
#include <UiCore/Styles/ProgressBarStyle.h>
#include <UiCore/Styles/ScrollBarStyle.h>
#include <UiCore/Styles/SliderStyle.h>
#include <UiCore/Styles/TextStyle.h>
#include <UiCore/Styles/ToolTipStyle.h>
#include <UiCore/Styles/WindowStyle.h>

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

// 기본 슬라이더 스타일이다(UE "Slider" — StarshipCoreStyle.cpp 589–609행: 막대 RB(Input, 2, Input,
// 1) 두께 4, 비활성 막대 외곽선 Recessed, 손잡이 8x8 원 Foreground·올림 ForegroundHover).
ROCKS_UICORE_API const SliderStyle& DefaultSliderStyle() noexcept;

// 기본 진행 막대 스타일이다(UE "ProgressBar" — 561–569행: 바탕 Color(Foldout), 채움·marquee
// PrimaryHover).
ROCKS_UICORE_API const ProgressBarStyle& DefaultProgressBarStyle() noexcept;

// 기본 스크롤 막대 스타일이다(UE "Scrollbar" — 186–195행: 손잡이 RB(Hover, 4), 올림·끌기 RB(Hover2,
// 4), 트랙 없음, 두께 8).
ROCKS_UICORE_API const ScrollBarStyle& DefaultScrollBarStyle() noexcept;

// 편집 글자 상자의 반경·여백이다(UE "NormalEditableTextBox" — StarshipCoreStyle.cpp 1364–1378행:
// RB(…, InputFocusRadius 4, …), Padding (8, 3, 8, 4)).
inline constexpr float EditableTextBoxCornerRadius = 4.0f;
inline constexpr Vec4 EditableTextBoxPadding{8.0f, 3.0f, 8.0f, 4.0f};

// 기본 편집 글자 스타일이다(UE "NormalEditableText" — 1346–1356행: 선택 바탕 Highlight, caret
// White, 조합 밑줄은 전경 점선, 힌트 불투명도 0.35 — SlateEditableTextLayout.h 79행).
ROCKS_UICORE_API const EditableTextStyle& DefaultEditableTextStyle() noexcept;

// 기본 편집 글자 상자 스타일이다(UE "NormalEditableTextBox" — 14장 4.4절: 바탕 RB(Input, 4,
// InputOutline, 1)·올림 RB(Input, 4, Hover, 1)·초점 RB(Input, 4, Primary, 1)·읽기 전용 RB(Header,
// 4, InputOutline, 1), 전경 Foreground·초점 White·읽기 전용 물려받음, 오류 표지 RB(Error, 4)).
ROCKS_UICORE_API const EditableTextBoxStyle& DefaultEditableTextBoxStyle() noexcept;

// 툴팁 글꼴 크기(UE "ToolTip.Font" = Regular 9 — StarshipCoreStyle.cpp 510행)와 줄바꿈 폭(UE
// Slate.ToolTipWrapWidth 기본 1000 — SToolTip.cpp 9–12행)이다.
inline constexpr float ToolTipFontSize = 9.0f;
inline constexpr float ToolTipWrapWidth = 1000.0f;
// 글자 툴팁 여백(UE SToolTip 글자 전용 경로 Padding 11 — SToolTip.cpp 76행)과 내용 툴팁 여백(UE
// SToolTip TextMargin 기본 8 — SToolTip.h 30행)이다.
inline constexpr float TextToolTipPadding = 11.0f;
inline constexpr float ContentToolTipPadding = 8.0f;
// 밝은 툴팁 바탕이다. UE "ToolTip.BrightBackground"는 9분할 이미지(Old/ToolTip_BrightBackground,
// 여백 8/64)인데 UE 클론에 이미지가 없어 밝은 회색 둥근 상자로 근사한다(sRGB 약 #E6E6E6, 외곽선 약
// #A0A0A0, 반경 3 — 실제 에디터 화면과 대조는 사람 확인 목록, Docs/Design/UiToolkit 9.1절 UW4t).
inline constexpr LinearColor BrightToolTipFill{0.79f, 0.79f, 0.79f, 1.0f};
// 밝은 툴팁의 흐린 설명 글자색이다(UE "Documentation.SDocumentationTooltipSubdued" — (0.1, 0.1,
// 0.1), StarshipStyle.cpp 3464–3466행).
inline constexpr LinearColor BrightToolTipSubdued{0.1f, 0.1f, 0.1f, 1.0f};
// 밝은 툴팁의 글자색이다(UE FLinearColor::Black — SToolTip.cpp 66행).
inline constexpr LinearColor BrightToolTipText{0.0f, 0.0f, 0.0f, 1.0f};
// 툴팁 단축키 표지다(UE "ToolTip.KeybindBorder" RB(#808080, 4)·"ToolTip.KeybindText" Bold 10 흰색 —
// DocumentationStyleSet.cpp 65–69행, 여백 (4, 2)·제목과 9 — SDocumentationToolTip.cpp 189–193행).
// 설명 줄 간격은 Padding(0, 5, 0, 0)(278행)이다.
inline constexpr float ToolTipShortcutRadius = 4.0f;
inline constexpr Vec4 ToolTipShortcutPadding{4.0f, 2.0f, 4.0f, 2.0f};
inline constexpr float ToolTipShortcutGap = 9.0f;
inline constexpr float ToolTipShortcutFontSize = 10.0f;
inline constexpr float ToolTipLineGap = 5.0f;
inline constexpr LinearColor BrightToolTipOutline{0.35f, 0.35f, 0.35f, 1.0f};
inline constexpr float BrightToolTipRadius = 3.0f;

// 글자 툴팁 스타일이다. UE 5.8 에디터의 글자 툴팁과 문서 툴팁은 모두 밝은 바탕 위 검은 글자
// Regular 9, 여백 11이다(SToolTip.cpp 64–80행, Documentation.cpp 258–266행, StarshipStyle.cpp
// 3456–3462행). Element::SetToolTipFactory에 글자만 줄 때 쓴다.
ROCKS_UICORE_API const ToolTipStyle& DefaultToolTipStyle() noexcept;

// 내용 툴팁 스타일이다(UE "ToolTip.Background" — StarshipCoreStyle.cpp 511행: RB(Dropdown, 0,
// DropdownOutline, 1), 여백 8, 글자 Regular 9·전경색). 제목·설명·단축키처럼 여러 부품을 담는 툴팁에
// 쓴다.
ROCKS_UICORE_API const ToolTipStyle& ContentToolTipStyle() noexcept;

// 바탕·여백 없는 툴팁 스타일이다(UE UWidget::SetToolTip — SToolTip.TextMargin(0).BorderImage(
// nullptr), Widget.cpp 1514–1516행). 모양을 스스로 그리는 사용자 위젯 툴팁에 쓴다.
ROCKS_UICORE_API const ToolTipStyle& BareToolTipStyle() noexcept;

// 창 버튼 판 크기(UE IMAGE_BRUSH_SVG(..., FVector2f(42, 23)) — StarshipCoreStyle.cpp 770–800행)다.
inline constexpr Vec2 WindowButtonSize{42.0f, 23.0f};
// 창 버튼 올림 바탕이다(UE "Window.MinMaxRestoreButtonHover" FColor(255, 255, 255, 40), 닫기
// "Window.CloseButtonHover" FColor(255, 20, 20, 150) — 824–830행). 선형 색(sRGB 20 → 0.007).
inline constexpr LinearColor WindowButtonHoverFill{1.0f, 1.0f, 1.0f, 40.0f / 255.0f};
inline constexpr LinearColor WindowCloseHoverFill{1.0f, 0.007f, 0.007f, 150.0f / 255.0f};
// 창 틀이다(UE "Window" — 815–820행: 바탕 Recessed, 틀 RB(Recessed, 2, WindowBorder, 2), 외곽선
// RB(Recessed, 2, InputOutline, 1)을 2 안쪽(SWindow.cpp 620–624행), 내용 여백 3).
inline constexpr float WindowCornerRadius = 2.0f;
inline constexpr float WindowBorderThickness = 2.0f;
inline constexpr float WindowOutlineInset = 2.0f;
inline constexpr Vec4 WindowBorderPadding{3.0f, 3.0f, 3.0f, 3.0f};
// 창 내용 여백이다(UE 메인 프레임의 도킹 영역이 창 가장자리에서 떨어진 간격 — 사용자 지적
// 2026-11-26: 내용이 창 외곽선에 붙어 선이 두 겹으로 보였다).
inline constexpr Vec4 WindowContentPadding{4.0f, 2.0f, 4.0f, 4.0f};
// 제목 글자 여백(SWindowTitleBar.cpp 297행)과 작은 앱 아이콘(UE "AppIcon.Small" 24x24, 여백 (4, 4,
// 0, 0) — StarshipCoreStyle.cpp 289–290행)이다.
inline constexpr Vec4 WindowTitlePadding{5.0f, 2.0f, 5.0f, 2.0f};
inline constexpr Vec2 WindowAppIconSize{24.0f, 24.0f};
inline constexpr Vec4 WindowAppIconPadding{4.0f, 4.0f, 0.0f, 0.0f};
// 비활성 창의 제목·앱 아이콘 불투명도다(UE FWidgetStyle::SubdueAmount 0.6 — WidgetStyle.cpp 11행).
// UE Starship은 활성·비활성 제목 brush가 둘 다 없어 차이가 없다 — 어느 창이 활성인지 보이게 하는
// Rocks 개선이다.
inline constexpr float InactiveWindowTitleOpacity = 0.6f;

// 메뉴 값이다(UE StarshipCoreStyle.cpp 2505–2653행·SMenuEntryBlock.cpp 583–664행): 항목 칸 여백
// "Menu.Block.Padding" (12, 1, 5, 1), 아이콘 칸 "Menu.MenuIconSize" 14(+2)·여백 (2, 0, 6, 0), 라벨
// 여백 (2, 0, 6, 0), 단축키 앞 25 + 여백 (16, 0, 4, 0), 하위 메뉴 표시 앞 7·chevron 16, 구분선
// White25 1 px·여백 (12, 6), 묶음 제목 Bold 7·자간 250·White25·여백 (12, 6), 라디오 8x8(Input·
// 켜면 White), 항목 버튼 여백 (0, 2)·눌림 (0, 3, 0, 1).
inline constexpr Vec4 MenuRowPadding{12.0f, 1.0f, 5.0f, 1.0f};
inline constexpr float MenuIconSize = 14.0f;
inline constexpr Vec4 MenuIconPadding{2.0f, 0.0f, 6.0f, 0.0f};
inline constexpr Vec4 MenuLabelPadding{2.0f, 0.0f, 6.0f, 0.0f};
inline constexpr float MenuKeybindingGap = 25.0f;
inline constexpr Vec4 MenuKeybindingPadding{16.0f, 0.0f, 4.0f, 0.0f};
inline constexpr float MenuSubMenuGap = 7.0f;
inline constexpr float MenuSubMenuIconSize = 16.0f;
inline constexpr float MenuSeparatorThickness = 1.0f;
inline constexpr Vec4 MenuSeparatorPadding{12.0f, 6.0f, 12.0f, 6.0f};
inline constexpr float MenuHeadingFontSize = 7.0f;
inline constexpr int32 MenuHeadingLetterSpacing = 250;
inline constexpr Vec4 MenuHeadingPadding{12.0f, 6.0f, 12.0f, 6.0f};
inline constexpr float MenuRadioSize = 8.0f;
inline constexpr Vec4 MenuButtonPadding{0.0f, 2.0f, 0.0f, 2.0f};
inline constexpr Vec4 MenuButtonPressedPadding{0.0f, 3.0f, 0.0f, 1.0f};
// 메뉴 막대 버튼 여백이다("WindowMenuBar.MenuBar.Padding" (12, 4)).
inline constexpr Vec4 MenuBarButtonPadding{12.0f, 4.0f, 12.0f, 4.0f};
// 둥근 강조 값이다(UE보다 개선 — VS Code 메뉴처럼 패널 안쪽 여백 4·항목 반경 3·패널 반경 4).
inline constexpr Vec4 MenuPanelPadding{4.0f, 4.0f, 4.0f, 4.0f};
inline constexpr float MenuPanelCornerRadius = 4.0f;
inline constexpr float MenuRowCornerRadius = 3.0f;
inline constexpr float MenuBarButtonCornerRadius = 4.0f;
// 긴 메뉴의 스크롤 위치 막대 폭·반경이다(VS Code 메뉴 스크롤 막대처럼 얇게).
inline constexpr float MenuScrollThumbWidth = 4.0f;
inline constexpr float MenuScrollThumbRadius = 2.0f;

// 펼친 메뉴 기본 스타일이다(UE "Menu").
ROCKS_UICORE_API const MenuStyle& DefaultMenuStyle() noexcept;

// 메뉴 막대 기본 스타일이다(UE "WindowMenuBar").
ROCKS_UICORE_API const MenuBarStyle& DefaultMenuBarStyle() noexcept;

// 직접 그린 창 테두리 기본 스타일이다(UE "Window").
ROCKS_UICORE_API const WindowStyle& DefaultWindowStyle() noexcept;

// 키보드 초점 외곽선이다(UE 핵심 스타일 "FocusRectangle" — StarshipCoreStyle.cpp 217행: 투명 채움,
// 반경 FocusOutlineRadius, 외곽선 Primary·ControlOutlineThickness).
ROCKS_UICORE_API const StyleBrush& FocusOutlineStyle() noexcept;

// 강조색(Primary) 바탕 위의 초점 외곽선이다(외곽선 White — Primary 외곽선은 Primary 바탕에 묻힌다.
// UE에 없는 Rocks 개선, Docs/Design/UiToolkit 9.1절 UW4f1).
ROCKS_UICORE_API const StyleBrush& FocusOutlineOnAccentStyle() noexcept;

// 보통 텍스트 스타일이다(UE "NormalText" — 1252–1258행: Regular 10, 상속 전경색).
ROCKS_UICORE_API const TextStyle& NormalTextStyle() noexcept;

} // namespace rocks::ui
