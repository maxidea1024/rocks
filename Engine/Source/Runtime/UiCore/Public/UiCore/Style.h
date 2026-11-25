#pragma once

#include <Core/Math/Vector4.h>
#include <Core/Text/StringView.h>
#include <UiCore/Brush.h>
#include <UiCore/ColorRef.h>

// Element 스타일 형식이다(Docs/Design/UiToolkit 4.1절, 9.1절 UW3b — UE FSlateWidgetStyle 파생과
// FSlateBrush). 색은 ColorRef(테마 slot·전경 상속)로 들고, 그릴 때 테마로 푼다. 기본값은 UE 5.8
// 에디터 핵심 스타일(14장)이다.
//
// 사용 예:
//   const ButtonStyle& style = DefaultButtonStyle();
//   const Brush brush = style.normal.Resolve(list.Theme(), {});
namespace rocks::ui {

// 테마 색으로 푸는 brush다(UE FSlateBrush — tint가 FSlateColor). 둥근 상자면 반경·외곽선을 쓴다.
struct ROCKS_UICORE_API StyleBrush {
    BrushMode mode = BrushMode::None;
    ColorRef fill = ColorRef::Specified({1, 1, 1, 1});
    ColorRef outlineColor = ColorRef::Specified({0, 0, 0, 0});
    float outlineWidth = 0.0f;
    // 반경 넷(왼쪽 위부터 시계 방향)과 방식이다. HalfHeight면 반경을 쓰지 않는다.
    Vec4 cornerRadii{};
    BrushRounding rounding = BrushRounding::Fixed;
    // 이미지 brush의 resource·논리 크기·9분할 여백이다.
    SlotHandle resource{};
    Vec2 imageSize{};
    Vec4 margin{};
    // 채움 색 알파에 곱한다(UE Panel.CopyWithNewOpacity(0.8)처럼 테마 색의 반투명 변형).
    float opacity = 1.0f;

    // 그리지 않는 brush다(UE FSlateNoResource).
    static StyleBrush None() noexcept;
    // 테마 색 둥근 상자다(UE FSlateRoundedBoxBrush(fill, radius, outline, width)).
    static StyleBrush RoundedBox(ThemeSlot fill, float radius, ThemeSlot outline,
                                 float outlineWidth) noexcept;
    // 외곽선 없는 테마 색 둥근 상자다.
    static StyleBrush RoundedBox(ThemeSlot fill, float radius) noexcept;
    // 테마 색 단색 상자다(UE FSlateColorBrush).
    static StyleBrush Color(ThemeSlot fill) noexcept;

    // 테마로 색을 풀어 그릴 Brush를 만든다.
    Brush Resolve(const ThemeColors& theme, const StyleColorContext& context) const noexcept;

    bool operator==(const StyleBrush&) const noexcept = default;
};

// 글꼴 typeface다(UE 핵심 스타일이 쓰는 FSlateFontInfo::TypefaceFontName 값 — "Regular",
// "Bold", "Italic", "BoldItalic", "Light"). 문자열 대신 열거라 스타일이 이름 수명을 갖지 않는다.
enum class Typeface : uint8 {
    Regular,
    Bold,
    Italic,
    BoldItalic,
    Light
};

// typeface의 조합 글꼴 이름이다(UE TypefaceFontName 값 그대로 — "Regular"·"Bold"·"Italic"·
// "BoldItalic"·"Light").
ROCKS_UICORE_API StringView TypefaceName(Typeface typeface) noexcept;

// 글꼴 지정이다(UE FSlateFontInfo의 typeface·크기). size는 96 DPI 포인트다.
struct FontStyle {
    Typeface typeface = Typeface::Regular;
    float size = 10.0f;
    bool operator==(const FontStyle&) const noexcept = default;
};

// 텍스트 스타일이다(UE FTextBlockStyle의 글꼴·색).
struct TextStyle {
    FontStyle font{};
    ColorRef color{ColorSource::Foreground};
    bool operator==(const TextStyle&) const noexcept = default;
};

// 버튼 스타일이다(UE FButtonStyle). 여백은 (왼쪽, 위, 오른쪽, 아래)다. 기본값은 UE FButtonStyle
// 생성자와 같다(brush 없음, 여백 0, 전경색 상속).
struct ButtonStyle {
    StyleBrush normal{};
    StyleBrush hovered{};
    StyleBrush pressed{};
    StyleBrush disabled{};
    Vec4 normalPadding{};
    Vec4 pressedPadding{};
    ColorRef normalForeground{ColorSource::Foreground};
    ColorRef hoveredForeground{ColorSource::Foreground};
    ColorRef pressedForeground{ColorSource::Foreground};
    ColorRef disabledForeground{ColorSource::Foreground};
    bool operator==(const ButtonStyle&) const noexcept = default;
};

// 체크 상자 스타일이다(UE FCheckBoxStyle의 CheckBox 형식). 체크·미정 표시 색은 UE SVG 아이콘
// ("Starship/CoreWidgets/CheckBox/check"·"indeterminate")의 tint다 — SVG가 생기기 전까지 같은 색의
// 선으로 상자 전체 크기에 그린다(UE 표시 SImage가 SOverlay 기본 Fill로 상자만큼 늘어난다,
// Docs/Design/UiToolkit 9.1절 UW4). 상자는 boxSize이고 내용은 그 오른쪽에 padding만큼 띄운다.
struct CheckBoxStyle {
    StyleBrush background{};
    StyleBrush backgroundHovered{};
    StyleBrush backgroundPressed{};
    ColorRef checkedColor{ColorSource::Foreground};
    ColorRef checkedHoveredColor{ColorSource::Foreground};
    ColorRef checkedPressedColor{ColorSource::Foreground};
    ColorRef undeterminedColor{ColorSource::Foreground};
    ColorRef undeterminedHoveredColor{ColorSource::Foreground};
    ColorRef undeterminedPressedColor{ColorSource::Foreground};
    Vec2 boxSize{18.0f, 18.0f};
    // 상자와 내용 사이 여백이다(UE FCheckBoxStyle 기본 (2, 0, 0, 0) — SlateTypes.cpp 24행).
    Vec4 padding{2.0f, 0.0f, 0.0f, 0.0f};
    ColorRef foreground{ColorSource::Foreground};
    ColorRef hoveredForeground{ColorSource::Foreground};
    ColorRef pressedForeground{ColorSource::Foreground};
    ColorRef checkedForeground{ColorSource::Foreground};
    ColorRef checkedHoveredForeground{ColorSource::Foreground};
    ColorRef checkedPressedForeground{ColorSource::Foreground};
    ColorRef undeterminedForeground{ColorSource::Foreground};
    bool operator==(const CheckBoxStyle&) const noexcept = default;
};

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
