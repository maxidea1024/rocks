#include <UiCore/Style.h>

// Element 스타일 형식과 UE 5.8 에디터 핵심 스타일 기본값이다(Docs/Design/UiToolkit 9.1절 UW3b,
// 14장). RB(채움, 반경, 외곽선, 두께)는 UE FSlateRoundedBoxBrush다.
namespace rocks::ui {

StyleBrush StyleBrush::None() noexcept {
    return {};
}

StyleBrush StyleBrush::RoundedBox(ThemeSlot fill, float radius, ThemeSlot outline,
                                  float outlineWidth) noexcept {
    StyleBrush brush;
    brush.mode = BrushMode::RoundedBox;
    brush.fill = ColorRef::FromTheme(fill);
    brush.outlineColor = ColorRef::FromTheme(outline);
    brush.outlineWidth = outlineWidth;
    brush.cornerRadii = {radius, radius, radius, radius};
    return brush;
}

StyleBrush StyleBrush::RoundedBox(ThemeSlot fill, float radius) noexcept {
    StyleBrush brush = RoundedBox(fill, radius, ThemeSlot::Black, 0.0f);
    brush.outlineColor = ColorRef::Specified({0, 0, 0, 0});
    return brush;
}

StyleBrush StyleBrush::Color(ThemeSlot fill) noexcept {
    StyleBrush brush;
    brush.mode = BrushMode::Box;
    brush.fill = ColorRef::FromTheme(fill);
    return brush;
}

Brush StyleBrush::Resolve(const ThemeColors& theme,
                          const StyleColorContext& context) const noexcept {
    Brush brush;
    brush.mode = mode;
    brush.tint = fill.Resolve(theme, context);
    brush.resource = resource;
    brush.imageSize = imageSize;
    brush.margin = margin;
    brush.outline = {cornerRadii, outlineColor.Resolve(theme, context), outlineWidth, rounding};
    return brush;
}

const ButtonStyle& DefaultButtonStyle() noexcept {
    // StarshipCoreStyle.cpp 1460–1470행: RB(Secondary·Hover·Header, 4, Input, 1), 비활성
    // RB(Dropdown, 4, Recessed, 1), 전경 ForegroundHover(비활성 Foreground), ButtonMargins.
    static const ButtonStyle style{StyleBrush::RoundedBox(ThemeSlot::Secondary, ButtonCornerRadius,
                                                          ThemeSlot::Input, InputFocusThickness),
                                   StyleBrush::RoundedBox(ThemeSlot::Hover, ButtonCornerRadius,
                                                          ThemeSlot::Input, InputFocusThickness),
                                   StyleBrush::RoundedBox(ThemeSlot::Header, ButtonCornerRadius,
                                                          ThemeSlot::Input, InputFocusThickness),
                                   StyleBrush::RoundedBox(ThemeSlot::Dropdown, ButtonCornerRadius,
                                                          ThemeSlot::Recessed, InputFocusThickness),
                                   ButtonMargins,
                                   PressedButtonMargins,
                                   ColorRef::FromTheme(ThemeSlot::ForegroundHover),
                                   ColorRef::FromTheme(ThemeSlot::ForegroundHover),
                                   ColorRef::FromTheme(ThemeSlot::ForegroundHover),
                                   ColorRef::FromTheme(ThemeSlot::Foreground)};
    return style;
}

const ButtonStyle& PrimaryButtonStyle() noexcept {
    // 1445–1455행: RB(Primary·PrimaryHover·PrimaryPress, 4, Input, 1), 나머지는 Button과 같다.
    static const ButtonStyle style = [] {
        ButtonStyle primary = DefaultButtonStyle();
        primary.normal = StyleBrush::RoundedBox(ThemeSlot::Primary, ButtonCornerRadius,
                                                ThemeSlot::Input, InputFocusThickness);
        primary.hovered = StyleBrush::RoundedBox(ThemeSlot::PrimaryHover, ButtonCornerRadius,
                                                 ThemeSlot::Input, InputFocusThickness);
        primary.pressed = StyleBrush::RoundedBox(ThemeSlot::PrimaryPress, ButtonCornerRadius,
                                                 ThemeSlot::Input, InputFocusThickness);
        return primary;
    }();
    return style;
}

const ButtonStyle& SimpleButtonStyle() noexcept {
    // 1475–1495행: 보통·비활성 없음, 올림·눌림 RB(Dropdown, 4), 전경 Foreground·ForegroundHover,
    // 여백 (2,1,2,1)·(2,2,2,0).
    static const ButtonStyle style{StyleBrush::None(),
                                   StyleBrush::RoundedBox(ThemeSlot::Dropdown, ButtonCornerRadius),
                                   StyleBrush::RoundedBox(ThemeSlot::Dropdown, ButtonCornerRadius),
                                   StyleBrush::None(),
                                   SimpleButtonMargins,
                                   PressedSimpleButtonMargins,
                                   ColorRef::FromTheme(ThemeSlot::Foreground),
                                   ColorRef::FromTheme(ThemeSlot::ForegroundHover),
                                   ColorRef::FromTheme(ThemeSlot::ForegroundHover),
                                   ColorRef::FromTheme(ThemeSlot::Foreground)};
    return style;
}

const ButtonStyle& NoBorderButtonStyle() noexcept {
    static const ButtonStyle style = [] {
        ButtonStyle noBorder;
        noBorder.normalForeground = ColorRef::FromTheme(ThemeSlot::ForegroundHover);
        noBorder.hoveredForeground = ColorRef::FromTheme(ThemeSlot::ForegroundHover);
        noBorder.pressedForeground = ColorRef::FromTheme(ThemeSlot::ForegroundHover);
        return noBorder;
    }();
    return style;
}

const TextStyle& NormalTextStyle() noexcept {
    static const TextStyle style{{Typeface::Regular, NormalFontSize}, {ColorSource::Foreground}};
    return style;
}

} // namespace rocks::ui
