#include <UiCore/Style.h>

// Element 스타일 형식과 UE 5.8 에디터 핵심 스타일 기본값이다(Docs/Design/UiToolkit 9.1절 UW3b,
// 14장). RB(채움, 반경, 외곽선, 두께)는 UE FSlateRoundedBoxBrush다.
namespace rocks::ui {

StringView TypefaceName(Typeface typeface) noexcept {
    switch (typeface) {
    case Typeface::Bold:
        return "Bold";
    case Typeface::Italic:
        return "Italic";
    case Typeface::BoldItalic:
        return "BoldItalic";
    case Typeface::Light:
        return "Light";
    case Typeface::Regular:
    default:
        return "Regular";
    }
}

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
    brush.tint.a *= opacity;
    brush.outline = {cornerRadii, outlineColor.Resolve(theme, context), outlineWidth, rounding};
    return brush;
}

const ButtonStyle& DefaultButtonStyle() noexcept {
    // StarshipCoreStyle.cpp 1460–1470행: RB(Secondary·Hover·Header, 4, Input, 1), 비활성
    // RB(Dropdown, 4, Recessed, 1), 전경 ForegroundHover(비활성 Foreground), ButtonPadding.
    static const ButtonStyle style{
        StyleBrush::RoundedBox(ThemeSlot::Secondary, ButtonCornerRadius, ThemeSlot::Input,
                               ControlOutlineThickness),
        StyleBrush::RoundedBox(ThemeSlot::Hover, ButtonCornerRadius, ThemeSlot::Input,
                               ControlOutlineThickness),
        StyleBrush::RoundedBox(ThemeSlot::Header, ButtonCornerRadius, ThemeSlot::Input,
                               ControlOutlineThickness),
        StyleBrush::RoundedBox(ThemeSlot::Dropdown, ButtonCornerRadius, ThemeSlot::Recessed,
                               ControlOutlineThickness),
        ButtonPadding,
        PressedButtonPadding,
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
                                                ThemeSlot::Input, ControlOutlineThickness);
        primary.hovered = StyleBrush::RoundedBox(ThemeSlot::PrimaryHover, ButtonCornerRadius,
                                                 ThemeSlot::Input, ControlOutlineThickness);
        primary.pressed = StyleBrush::RoundedBox(ThemeSlot::PrimaryPress, ButtonCornerRadius,
                                                 ThemeSlot::Input, ControlOutlineThickness);
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
                                   SimpleButtonPadding,
                                   PressedSimpleButtonPadding,
                                   ColorRef::FromTheme(ThemeSlot::Foreground),
                                   ColorRef::FromTheme(ThemeSlot::ForegroundHover),
                                   ColorRef::FromTheme(ThemeSlot::ForegroundHover),
                                   ColorRef::FromTheme(ThemeSlot::Foreground)};
    return style;
}

const CheckBoxStyle& DefaultCheckBoxStyle() noexcept {
    static const CheckBoxStyle style = [] {
        const ColorRef hover = ColorRef::FromTheme(ThemeSlot::ForegroundHover);
        CheckBoxStyle check;
        check.background = StyleBrush::RoundedBox(ThemeSlot::Input, CheckBoxCornerRadius,
                                                  ThemeSlot::Secondary, CheckBoxOutlineThickness);
        check.backgroundHovered = StyleBrush::RoundedBox(
            ThemeSlot::Input, CheckBoxCornerRadius, ThemeSlot::Hover, CheckBoxOutlineThickness);
        check.backgroundPressed = check.backgroundHovered;
        check.checkedColor = ColorRef::FromTheme(ThemeSlot::Primary);
        check.checkedHoveredColor = ColorRef::FromTheme(ThemeSlot::PrimaryHover);
        check.checkedPressedColor = ColorRef::FromTheme(ThemeSlot::PrimaryPress);
        check.undeterminedColor = check.checkedColor;
        check.undeterminedHoveredColor = check.checkedHoveredColor;
        check.undeterminedPressedColor = check.checkedPressedColor;
        check.foreground = hover;
        check.hoveredForeground = hover;
        check.pressedForeground = hover;
        check.checkedForeground = hover;
        check.checkedHoveredForeground = hover;
        check.checkedPressedForeground = hover;
        check.undeterminedForeground = hover;
        return check;
    }();
    return style;
}

const DockTabStyle& DefaultDockTabStyle() noexcept {
    // 1876–1899행. HoveredBrush는 Panel을 80% 불투명도로(주석 "Panel + 20% Black") 쓴다.
    constexpr float HoveredTabOpacity = 0.8f;
    // UE SetOverlapWidth(-2.0f) — 음수 겹침은 탭 사이 간격이다.
    constexpr float TabGap = 2.0f;
    static const DockTabStyle style = [] {
        DockTabStyle tab;
        tab.foreground = StyleBrush::RoundedBox(ThemeSlot::Panel, DockTabCornerRadius);
        tab.foreground.cornerRadii = {DockTabCornerRadius, DockTabCornerRadius, 0, 0};
        tab.hovered = tab.foreground;
        tab.hovered.opacity = HoveredTabOpacity;
        tab.contentArea = StyleBrush::Color(ThemeSlot::Panel);
        tab.tabWell = StyleBrush::Color(ThemeSlot::Background);
        tab.tabPadding = {4.0f, 3.0f, 10.0f, 4.0f};
        tab.verticalPaddingScale = DockTabVerticalPaddingScale;
        tab.tabGap = TabGap;
        tab.maxTabSize = PanelTabMaxSize;
        tab.normalForeground = ColorRef::FromTheme(ThemeSlot::Foreground);
        tab.hoveredForeground = ColorRef::FromTheme(ThemeSlot::ForegroundHover);
        tab.foregroundForeground = ColorRef::FromTheme(ThemeSlot::Foreground);
        tab.activeForeground = ColorRef::FromTheme(ThemeSlot::ForegroundHover);
        return tab;
    }();
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
