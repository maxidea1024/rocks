#pragma once

#include <Core/Math/Vector4.h>
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

// UE 5.8 에디터 핵심 스타일 상수다(14장 3절 — CoreStyle.cpp 54–62행, 단순 버튼 여백은
// StarshipCoreStyle.cpp 1493–1495행).
inline constexpr float InputFocusRadius = 4.0f;
inline constexpr float InputFocusThickness = 1.0f;
inline constexpr float ButtonCornerRadius = 4.0f;
inline constexpr Vec4 ButtonMargins{12.0f, 1.5f, 12.0f, 1.5f};
inline constexpr Vec4 PressedButtonMargins{12.0f, 2.5f, 12.0f, 0.5f};
inline constexpr Vec4 SimpleButtonMargins{2.0f, 1.0f, 2.0f, 1.0f};
inline constexpr Vec4 PressedSimpleButtonMargins{2.0f, 2.0f, 2.0f, 0.0f};

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

// 보이지 않는 버튼 스타일이다(UE "NoBorder" — 232–240행, 252행: brush 없음, 전경 ForegroundHover,
// 여백 0). 펼침 영역 머리처럼 내용만 보이는 클릭 영역에 쓴다.
ROCKS_UICORE_API const ButtonStyle& NoBorderButtonStyle() noexcept;

// 보통 텍스트 스타일이다(UE "NormalText" — 1252–1258행: Regular 10, 상속 전경색).
ROCKS_UICORE_API const TextStyle& NormalTextStyle() noexcept;

} // namespace rocks::ui
