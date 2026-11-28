#include <UiCore/Styles/CoreStyles.h>

#include <UiCore/Styles/CoreIcons.h>

// Element 스타일 형식과 UE 5.8 에디터 핵심 스타일 기본값이다(Docs/Design/UiToolkit 9.1절 UW3b,
// 14장). RB(채움, 반경, 외곽선, 두께)는 UE FSlateRoundedBoxBrush다.
namespace rocks::ui {

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
        // 표시는 SVG 16x16 Check·Indeterminate를 Primary·PrimaryHover·PrimaryPress로 tint한다
        // (1667-1691행).
        check.checkedImage = StyleBrush::Svg(icons::Check, icons::Icon16, ThemeSlot::Primary);
        check.checkedHoveredImage =
            StyleBrush::Svg(icons::Check, icons::Icon16, ThemeSlot::PrimaryHover);
        check.checkedPressedImage =
            StyleBrush::Svg(icons::Check, icons::Icon16, ThemeSlot::PrimaryPress);
        check.undeterminedImage =
            StyleBrush::Svg(icons::Indeterminate, icons::Icon16, ThemeSlot::Primary);
        check.undeterminedHoveredImage =
            StyleBrush::Svg(icons::Indeterminate, icons::Icon16, ThemeSlot::PrimaryHover);
        check.undeterminedPressedImage =
            StyleBrush::Svg(icons::Indeterminate, icons::Icon16, ThemeSlot::PrimaryPress);
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

const SliderStyle& DefaultSliderStyle() noexcept {
    constexpr float BarRadius = 2.0f;
    static const SliderStyle style = [] {
        SliderStyle slider;
        slider.bar = StyleBrush::RoundedBox(ThemeSlot::Input, BarRadius, ThemeSlot::Input,
                                            ControlOutlineThickness);
        slider.barHovered = slider.bar;
        slider.barDisabled = StyleBrush::RoundedBox(ThemeSlot::Input, BarRadius,
                                                    ThemeSlot::Recessed, ControlOutlineThickness);
        slider.thumb = StyleBrush::Pill(ThemeSlot::Foreground);
        slider.thumbHovered = StyleBrush::Pill(ThemeSlot::ForegroundHover);
        slider.thumbDisabled = slider.thumb;
        return slider;
    }();
    return style;
}

const ProgressBarStyle& DefaultProgressBarStyle() noexcept {
    static const ProgressBarStyle style{StyleBrush::Color(ThemeSlot::Foldout),
                                        StyleBrush::Color(ThemeSlot::PrimaryHover),
                                        StyleBrush::Color(ThemeSlot::PrimaryHover)};
    return style;
}

const EditableTextStyle& DefaultEditableTextStyle() noexcept {
    static const EditableTextStyle style{
        NormalTextStyle(), ColorRef::FromTheme(ThemeSlot::Highlight),
        ColorRef::FromTheme(ThemeSlot::White), ColorRef{ColorSource::Foreground}, 0.35f};
    return style;
}

namespace {

// 상태별 전경색을 한 번에 정한다(보통만 따로).
void SetForegrounds(CheckBoxStyle& style, ColorRef normal, ColorRef others) noexcept {
    style.foreground = normal;
    style.hoveredForeground = others;
    style.pressedForeground = others;
    style.checkedForeground = others;
    style.checkedHoveredForeground = others;
    style.checkedPressedForeground = others;
    style.undeterminedForeground = others;
}

} // namespace

const CheckBoxStyle& DefaultToggleButtonCheckBoxStyle() noexcept {
    static const CheckBoxStyle style = [] {
        CheckBoxStyle toggle;
        toggle.type = CheckBoxType::ToggleButton;
        toggle.checkedImage = StyleBrush::RoundedBox(ThemeSlot::Primary, ToggleButtonCornerRadius);
        toggle.checkedHoveredImage =
            StyleBrush::RoundedBox(ThemeSlot::PrimaryHover, ToggleButtonCornerRadius);
        toggle.checkedPressedImage =
            StyleBrush::RoundedBox(ThemeSlot::Dropdown, ToggleButtonCornerRadius);
        toggle.uncheckedImage = StyleBrush::None();
        toggle.uncheckedHoveredImage =
            StyleBrush::RoundedBox(ThemeSlot::Dropdown, ToggleButtonCornerRadius);
        toggle.uncheckedPressedImage = toggle.uncheckedHoveredImage;
        SetForegrounds(toggle, ColorRef::FromTheme(ThemeSlot::Foreground),
                       ColorRef::FromTheme(ThemeSlot::ForegroundHover));
        // UE 토글은 미정 전경을 정하지 않는다(UseForeground).
        toggle.undeterminedForeground = ColorRef{ColorSource::Foreground};
        toggle.padding = ToggleButtonMargins;
        return toggle;
    }();
    return style;
}

const CheckBoxStyle& DefaultToggleButtonCheckBoxAltStyle() noexcept {
    static const CheckBoxStyle style = [] {
        CheckBoxStyle toggle;
        toggle.type = CheckBoxType::ToggleButton;
        toggle.checkedImage = StyleBrush::RoundedBox(ThemeSlot::Input, ComboCornerRadius);
        toggle.checkedHoveredImage = StyleBrush::RoundedBox(ThemeSlot::Hover, ComboCornerRadius);
        toggle.checkedPressedImage = toggle.checkedImage;
        toggle.uncheckedImage = StyleBrush::None();
        toggle.uncheckedHoveredImage = toggle.checkedHoveredImage;
        toggle.uncheckedPressedImage = toggle.checkedImage;
        const ColorRef hover = ColorRef::FromTheme(ThemeSlot::ForegroundHover);
        toggle.foreground = ColorRef::FromTheme(ThemeSlot::Foreground);
        toggle.hoveredForeground = hover;
        toggle.pressedForeground = hover;
        toggle.checkedForeground = ColorRef::FromTheme(ThemeSlot::White);
        toggle.checkedHoveredForeground = ColorRef::FromTheme(ThemeSlot::White);
        // UE Alt는 체크 눌림·미정 전경을 정하지 않는다(UseForeground).
        toggle.checkedPressedForeground = ColorRef{ColorSource::Foreground};
        toggle.undeterminedForeground = ColorRef{ColorSource::Foreground};
        toggle.padding = ToggleButtonMargins;
        return toggle;
    }();
    return style;
}

const CheckBoxStyle& DefaultTransparentCheckBoxStyle() noexcept {
    static const CheckBoxStyle style = [] {
        CheckBoxStyle toggle;
        toggle.type = CheckBoxType::ToggleButton;
        SetForegrounds(toggle, ColorRef::FromTheme(ThemeSlot::Foreground),
                       ColorRef::FromTheme(ThemeSlot::ForegroundHover));
        toggle.undeterminedForeground = ColorRef{ColorSource::Foreground};
        return toggle;
    }();
    return style;
}

const CheckBoxStyle& DefaultRadioButtonStyle() noexcept {
    static const CheckBoxStyle style = [] {
        CheckBoxStyle radio;
        radio.type = CheckBoxType::CheckBox;
        radio.uncheckedImage = StyleBrush::Svg(icons::RadioOff, icons::Icon16, ThemeSlot::Dropdown);
        radio.uncheckedHoveredImage =
            StyleBrush::Svg(icons::RadioOff, icons::Icon16, ThemeSlot::Hover);
        radio.uncheckedPressedImage = radio.uncheckedImage;
        radio.checkedImage = StyleBrush::Svg(icons::RadioOn, icons::Icon16, ThemeSlot::Primary);
        radio.checkedHoveredImage =
            StyleBrush::Svg(icons::RadioOn, icons::Icon16, ThemeSlot::PrimaryHover);
        radio.checkedPressedImage = radio.checkedImage;
        radio.undeterminedImage =
            StyleBrush::Svg(icons::RadioOff, icons::Icon16, ThemeSlot::White25);
        radio.undeterminedHoveredImage =
            StyleBrush::Svg(icons::RadioOff, icons::Icon16, ThemeSlot::ForegroundHover);
        radio.undeterminedPressedImage = radio.undeterminedHoveredImage;
        const ColorRef white = ColorRef::FromTheme(ThemeSlot::White);
        SetForegrounds(radio, white, white);
        radio.boxSize = RadioButtonBoxSize;
        radio.padding = RadioButtonPadding;
        return radio;
    }();
    return style;
}

const ComboButtonStyle& DefaultComboButtonStyle() noexcept {
    static const ComboButtonStyle style = [] {
        const ColorRef foreground = ColorRef::FromTheme(ThemeSlot::Foreground);
        const ColorRef hover = ColorRef::FromTheme(ThemeSlot::ForegroundHover);
        ComboButtonStyle combo;
        ButtonStyle& button = combo.button;
        button.normal = StyleBrush::RoundedBox(ThemeSlot::Input, ComboCornerRadius,
                                               ThemeSlot::InputOutline, ComboOutlineThickness);
        button.hovered = StyleBrush::RoundedBox(ThemeSlot::Input, ComboCornerRadius,
                                                ThemeSlot::Hover, ComboOutlineThickness);
        button.pressed = button.hovered;
        button.disabled = StyleBrush::RoundedBox(ThemeSlot::Dropdown, ComboCornerRadius,
                                                 ThemeSlot::DropdownOutline, ComboOutlineThickness);
        button.normalForeground = foreground;
        button.hoveredForeground = hover;
        button.pressedForeground = hover;
        button.disabledForeground = hover;
        button.normalPadding = ComboButtonMargin;
        button.pressedPadding = PressedComboButtonMargin;
        combo.contentPadding = {};
        // 흰 이미지에 전경색을 곱해 그린다(UE ForegroundArrowImage UseForeground).
        combo.downArrow = StyleBrush::Svg(icons::WideChevronDown, icons::Icon16, ThemeSlot::White);
        combo.menuBorder =
            StyleBrush::RoundedBox(ThemeSlot::Dropdown, 0.0f, ThemeSlot::DropdownOutline, 1.0f);
        combo.menuBorderPadding = {};
        return combo;
    }();
    return style;
}

const ComboButtonStyle& DefaultSimpleComboButtonStyle() noexcept {
    static const ComboButtonStyle style = [] {
        ComboButtonStyle combo = DefaultComboButtonStyle();
        combo.button = SimpleButtonStyle();
        return combo;
    }();
    return style;
}

const ComboBoxStyle& DefaultComboBoxStyle() noexcept {
    static const ComboBoxStyle style = [] {
        ComboBoxStyle combo;
        combo.comboButton = DefaultComboButtonStyle();
        combo.contentPadding = {};
        combo.menuRowPadding = ComboMenuRowPadding;
        return combo;
    }();
    return style;
}

const ComboBoxStyle& DefaultSimpleComboBoxStyle() noexcept {
    static const ComboBoxStyle style = [] {
        ComboBoxStyle combo;
        combo.comboButton = DefaultSimpleComboButtonStyle();
        combo.contentPadding = {};
        combo.menuRowPadding = SimpleComboMenuRowPadding;
        return combo;
    }();
    return style;
}

const TableRowStyle& DefaultComboBoxRowStyle() noexcept {
    static const TableRowStyle style = [] {
        TableRowStyle row;
        const StyleBrush primary = StyleBrush::Color(ThemeSlot::Primary);
        row.evenRow = StyleBrush::None();
        row.evenRowHovered = primary;
        row.oddRow = StyleBrush::None();
        row.oddRowHovered = primary;
        row.active = StyleBrush::None();
        row.activeHovered = primary;
        // 투명 채움에 Select 외곽선이다(UE FSlateRoundedBoxBrush(Transparent, 4, Select, 1)).
        row.selectorFocused = StyleBrush::RoundedBox(ThemeSlot::Select, ComboCornerRadius,
                                                     ThemeSlot::Select, ComboOutlineThickness);
        row.selectorFocused.fill = ColorRef::Specified({0.0f, 0.0f, 0.0f, 0.0f});
        row.text = ColorRef::FromTheme(ThemeSlot::White);
        row.selectedText = ColorRef::FromTheme(ThemeSlot::White);
        return row;
    }();
    return style;
}

const EditableTextBoxStyle& DefaultEditableTextBoxStyle() noexcept {
    static const EditableTextBoxStyle style = [] {
        EditableTextBoxStyle box;
        box.normal = StyleBrush::RoundedBox(ThemeSlot::Input, EditableTextBoxCornerRadius,
                                            ThemeSlot::InputOutline, ControlOutlineThickness);
        box.hovered = StyleBrush::RoundedBox(ThemeSlot::Input, EditableTextBoxCornerRadius,
                                             ThemeSlot::Hover, ControlOutlineThickness);
        box.focused = StyleBrush::RoundedBox(ThemeSlot::Input, EditableTextBoxCornerRadius,
                                             ThemeSlot::Primary, ControlOutlineThickness);
        box.readOnly = StyleBrush::RoundedBox(ThemeSlot::Header, EditableTextBoxCornerRadius,
                                              ThemeSlot::InputOutline, ControlOutlineThickness);
        box.padding = EditableTextBoxPadding;
        box.foreground = ColorRef{ColorSource::Foreground};
        box.focusedForeground = ColorRef::FromTheme(ThemeSlot::White);
        box.readOnlyForeground = ColorRef{ColorSource::Foreground};
        box.text = DefaultEditableTextStyle();
        box.errorMarker = StyleBrush::RoundedBox(ThemeSlot::Error, EditableTextBoxCornerRadius);
        box.errorText = NormalTextStyle();
        return box;
    }();
    return style;
}

const ScrollBarStyle& DefaultScrollBarStyle() noexcept {
    static const ScrollBarStyle style{
        StyleBrush::None(), StyleBrush::RoundedBox(ThemeSlot::Hover, ButtonCornerRadius),
        StyleBrush::RoundedBox(ThemeSlot::Hover2, ButtonCornerRadius),
        StyleBrush::RoundedBox(ThemeSlot::Hover2, ButtonCornerRadius)};
    return style;
}

namespace {

// 제목·단축키·설명 툴팁 값이다. 제목은 굵은 Normal, 설명은 툴팁 글꼴, 단축키는 표지 위 흰 굵은
// 글자다.
void AddToolTipParts(ToolTipStyle& tip, ColorRef title, ColorRef description) noexcept {
    tip.title = {{Typeface::Bold, NormalFontSize}, title};
    tip.description = {{Typeface::Regular, ToolTipFontSize}, description};
    tip.shortcut = {{Typeface::Bold, ToolTipShortcutFontSize},
                    ColorRef::FromTheme(ThemeSlot::White)};
    tip.shortcutBackground = StyleBrush::RoundedBox(ThemeSlot::Hover2, ToolTipShortcutRadius);
    tip.shortcutPadding = ToolTipShortcutPadding;
    tip.shortcutGap = ToolTipShortcutGap;
    tip.lineGap = ToolTipLineGap;
}

} // namespace

const ToolTipStyle& DefaultToolTipStyle() noexcept {
    static const ToolTipStyle style = [] {
        ToolTipStyle tip;
        tip.background.mode = BrushMode::RoundedBox;
        tip.background.fill = ColorRef::Specified(BrightToolTipFill);
        tip.background.outlineColor = ColorRef::Specified(BrightToolTipOutline);
        tip.background.outlineWidth = ControlOutlineThickness;
        tip.background.cornerRadii = {BrightToolTipRadius, BrightToolTipRadius, BrightToolTipRadius,
                                      BrightToolTipRadius};
        tip.padding = {TextToolTipPadding, TextToolTipPadding, TextToolTipPadding,
                       TextToolTipPadding};
        tip.text = {{Typeface::Regular, ToolTipFontSize}, ColorRef::Specified(BrightToolTipText)};
        tip.wrapWidth = ToolTipWrapWidth;
        AddToolTipParts(tip, ColorRef::Specified(BrightToolTipText),
                        ColorRef::Specified(BrightToolTipSubdued));
        return tip;
    }();
    return style;
}

const ToolTipStyle& ContentToolTipStyle() noexcept {
    static const ToolTipStyle style = [] {
        ToolTipStyle tip;
        tip.background = StyleBrush::RoundedBox(
            ThemeSlot::Dropdown, 0.0f, ThemeSlot::DropdownOutline, ControlOutlineThickness);
        tip.padding = {ContentToolTipPadding, ContentToolTipPadding, ContentToolTipPadding,
                       ContentToolTipPadding};
        tip.text = {{Typeface::Regular, ToolTipFontSize}, {ColorSource::Foreground}};
        tip.wrapWidth = ToolTipWrapWidth;
        AddToolTipParts(tip, ColorRef::FromTheme(ThemeSlot::ForegroundHover),
                        ColorRef::FromTheme(ThemeSlot::Foreground));
        return tip;
    }();
    return style;
}

const MenuStyle& DefaultMenuStyle() noexcept {
    static const MenuStyle style = [] {
        MenuStyle menu;
        menu.background = StyleBrush::RoundedBox(ThemeSlot::Dropdown, MenuPanelCornerRadius);
        menu.outline = StyleBrush::RoundedBox(ThemeSlot::Dropdown, MenuPanelCornerRadius,
                                              ThemeSlot::DropdownOutline, ControlOutlineThickness);
        menu.outline.fill = ColorRef::Specified({0, 0, 0, 0});
        menu.panelPadding = MenuPanelPadding;
        // 항목 버튼은 바탕 없이 올림·눌림만 Primary 둥근 상자다(UE "Menu.Button" — 각진 것을 개선).
        menu.row = NoBorderButtonStyle();
        menu.row.hovered = StyleBrush::RoundedBox(ThemeSlot::Primary, MenuRowCornerRadius);
        menu.row.pressed = menu.row.hovered;
        menu.row.normalPadding = MenuButtonPadding;
        menu.row.pressedPadding = MenuButtonPressedPadding;
        menu.rowPadding = MenuRowPadding;
        menu.iconSize = MenuIconSize;
        menu.iconPadding = MenuIconPadding;
        menu.labelPadding = MenuLabelPadding;
        menu.keybindingGap = MenuKeybindingGap;
        menu.keybindingPadding = MenuKeybindingPadding;
        menu.subMenuGap = MenuSubMenuGap;
        menu.subMenuIconSize = MenuSubMenuIconSize;
        menu.separator = StyleBrush::Color(ThemeSlot::White25);
        menu.separatorThickness = MenuSeparatorThickness;
        menu.separatorPadding = MenuSeparatorPadding;
        menu.label = NormalTextStyle();
        menu.keybinding = {{Typeface::Regular, SmallFontSize}, {ColorSource::Foreground}};
        menu.heading = {{Typeface::Bold, MenuHeadingFontSize, MenuHeadingLetterSpacing},
                        ColorRef::FromTheme(ThemeSlot::White25)};
        menu.headingPadding = MenuHeadingPadding;
        menu.checkIcon = icons::MenuCheck;
        menu.subMenuIcon = icons::ChevronRight;
        menu.radioOff = StyleBrush::Pill(ThemeSlot::Input);
        menu.radioOn = StyleBrush::Pill(ThemeSlot::White);
        menu.radioSize = MenuRadioSize;
        menu.scrollThumb = StyleBrush::RoundedBox(ThemeSlot::Hover2, MenuScrollThumbRadius);
        menu.scrollThumbWidth = MenuScrollThumbWidth;
        return menu;
    }();
    return style;
}

const MenuBarStyle& DefaultMenuBarStyle() noexcept {
    static const MenuBarStyle style = [] {
        MenuBarStyle bar;
        // UE "WindowMenuBar.Button" = Menu.Button에 올림 Hover, 전경 Foreground·올림
        // ForegroundHover.
        bar.button = NoBorderButtonStyle();
        bar.button.hovered = StyleBrush::RoundedBox(ThemeSlot::Hover, MenuBarButtonCornerRadius);
        bar.button.normalForeground = ColorRef::FromTheme(ThemeSlot::Foreground);
        bar.button.hoveredForeground = ColorRef::FromTheme(ThemeSlot::ForegroundHover);
        bar.button.pressedForeground = ColorRef::FromTheme(ThemeSlot::ForegroundHover);
        bar.button.normalPadding = MenuBarButtonPadding;
        bar.button.pressedPadding = MenuBarButtonPadding;
        bar.openBrush = StyleBrush::RoundedBox(ThemeSlot::Primary, MenuBarButtonCornerRadius);
        bar.label = NormalTextStyle();
        return bar;
    }();
    return style;
}

const WindowStyle& DefaultWindowStyle() noexcept {
    static const WindowStyle style = [] {
        WindowStyle window;
        // 창 버튼은 바탕 없음, 올림만 반투명 바탕이다. 아이콘 색은 전경 Foreground·올림
        // ForegroundHover다(UE 버튼 이미지 tint).
        window.controlButton.hovered = StyleBrush::Color(ThemeSlot::White);
        window.controlButton.hovered.fill = ColorRef::Specified(WindowButtonHoverFill);
        window.controlButton.pressed = window.controlButton.hovered;
        window.controlButton.normalForeground = ColorRef::FromTheme(ThemeSlot::Foreground);
        window.controlButton.hoveredForeground = ColorRef::FromTheme(ThemeSlot::ForegroundHover);
        window.controlButton.pressedForeground = ColorRef::FromTheme(ThemeSlot::Foreground);
        window.controlButton.disabledForeground = ColorRef::FromTheme(ThemeSlot::Foreground);
        window.closeButton = window.controlButton;
        window.closeButton.hovered.fill = ColorRef::Specified(WindowCloseHoverFill);
        window.closeButton.pressed = window.closeButton.hovered;
        window.minimizeIcon = icons::WindowMinimize;
        window.maximizeIcon = icons::WindowMaximize;
        window.restoreIcon = icons::WindowRestore;
        window.closeIcon = icons::WindowClose;
        window.exitFullscreenIcon = icons::WindowExitFullscreen;
        window.buttonSize = WindowButtonSize;
        window.title = NormalTextStyle();
        window.titlePadding = WindowTitlePadding;
        window.appIcon = icons::RocksLogo;
        window.appIconSize = WindowAppIconSize;
        window.appIconPadding = WindowAppIconPadding;
        window.background = StyleBrush::Color(ThemeSlot::Recessed);
        window.border = StyleBrush::RoundedBox(ThemeSlot::Recessed, WindowCornerRadius,
                                               ThemeSlot::WindowBorder, WindowBorderThickness);
        window.outline = StyleBrush::RoundedBox(ThemeSlot::Recessed, WindowCornerRadius,
                                                ThemeSlot::InputOutline, ControlOutlineThickness);
        window.outlineInset = WindowOutlineInset;
        window.borderPadding = WindowBorderPadding;
        window.contentPadding = WindowContentPadding;
        return window;
    }();
    return style;
}

const ToolTipStyle& BareToolTipStyle() noexcept {
    static const ToolTipStyle style = [] {
        ToolTipStyle tip = ContentToolTipStyle();
        tip.background = StyleBrush::None();
        tip.padding = {};
        return tip;
    }();
    return style;
}

const StyleBrush& FocusOutlineStyle() noexcept {
    static const StyleBrush style = [] {
        StyleBrush brush = StyleBrush::RoundedBox(ThemeSlot::Primary, FocusOutlineRadius,
                                                  ThemeSlot::Primary, ControlOutlineThickness);
        brush.fill = ColorRef::Specified({0, 0, 0, 0});
        return brush;
    }();
    return style;
}

const StyleBrush& FocusOutlineOnAccentStyle() noexcept {
    static const StyleBrush style = [] {
        StyleBrush brush = FocusOutlineStyle();
        brush.outlineColor = ColorRef::FromTheme(ThemeSlot::White);
        return brush;
    }();
    return style;
}

const TextStyle& NormalTextStyle() noexcept {
    static const TextStyle style{{Typeface::Regular, NormalFontSize}, {ColorSource::Foreground}};
    return style;
}

} // namespace rocks::ui
