## 14. 에디터 스타일 값(UE 5.8.4 Starship)

UW3(9.1절)에서 옮길 값의 원본 표다. UE 5.8.4 클론(4절 표의 파일)에서 2026-11-25에 읽었고, 모든 값에 UE 파일:줄을 단다. 이 표는 출처 기록이고, Rocks 기본 테마·스타일 값은 이 표와 대조하는 시험(UV8)으로 묶는다. 표의 영어 머리말과 UE 식별자는 출처 그대로 둔다. NOT FOUND는 원본에서 찾지 못한 항목이다.

Source: `C:\github\UnrealEngine` (5.8.4, read only). Path abbreviations:

| Abbrev | Path |
|---|---|
| SC | Engine/Source/Runtime/SlateCore/Private/Styling/StyleColors.cpp |
| SCH | Engine/Source/Runtime/SlateCore/Public/Styling/StyleColors.h |
| CS | Engine/Source/Runtime/SlateCore/Private/Styling/CoreStyle.cpp |
| SCS | Engine/Source/Runtime/SlateCore/Private/Styling/StarshipCoreStyle.cpp |
| ES | Engine/Source/Editor/EditorStyle/Private/StarshipStyle.cpp |
| RBB | Engine/Source/Runtime/SlateCore/Public/Brushes/SlateRoundedBoxBrush.h |
| SB | Engine/Source/Runtime/SlateCore/Public/Styling/SlateBrush.h |
| ST | Engine/Source/Runtime/SlateCore/Private/Styling/SlateTypes.cpp |
| DET | Engine/Source/Runtime/SlateCore/Private/Rendering/DrawElementTypes.cpp |
| EB | Engine/Source/Runtime/SlateCore/Private/Rendering/ElementBatcher.cpp |
| SSC | Engine/Shaders/Private/SlateShaderCommon.ush |
| SEPS | Engine/Shaders/Private/SlateElementPixelShader.usf |
| SVS | Engine/Shaders/Private/SlateVertexShader.usf |
| RP | Engine/Source/Runtime/SlateRHIRenderer/Private/SlateRHIRenderingPolicy.cpp |

Notation: `RB(fill, r, outline, w)` = `FSlateRoundedBoxBrush`; `RB(fill, r)` = no outline; `RB(fill)` / `RB(fill, outline, w)` without radius = **HalfHeightRadius (pill/circle)**, see §5. `Color(x)` = `FSlateColorBrush` (flat rect). `BOX("img", m, tint)` = 9-slice image, `BORDER(...)` = border-only image, `SVG("img", size, tint)` = vector image. `None` = `FSlateNoResource`. Color names are `FStyleColors::X` = `EStyleColor::X` (§1).

---

## 1. Theme colors (EStyleColor dark defaults)

`COLOR(hex)` = `FLinearColor::FromSRGBColor(FColor::FromHex(hex))` — SCH:25, i.e. hex values are sRGB. Defaults set in `USlateThemeManager::InitializeDefaults()` SC:78-136.

| EStyleColor | sRGB hex (as written) | Line |
|---|---|---|
| Black | #000000FF | SC:82 |
| Title | #151515FF | SC:83 |
| Background | #151515FF | SC:84 |
| WindowBorder | 0F0F0FFF | SC:85 |
| Foldout | 0F0F0FFF | SC:86 |
| Input | 0F0F0FFF | SC:87 |
| InputOutline | 383838FF | SC:88 |
| Recessed | #1A1A1AFF | SC:89 |
| Panel | #242424FF | SC:90 |
| Header | #2F2F2FFF | SC:91 |
| Dropdown | #383838FF | SC:92 |
| DropdownOutline | #4C4C4CFF | SC:93 |
| Hover | #575757FF | SC:94 |
| Hover2 | #808080FF | SC:95 |
| White | #FFFFFFFF | SC:97 |
| White25 | #FFFFFF40 | SC:98 |
| Highlight | #0070E0FF | SC:99 |
| Primary | #0070E0FF | SC:101 |
| PrimaryHover | #0E86FFFF | SC:102 |
| PrimaryPress | #0050A0FF | SC:103 |
| Secondary | #383838FF | SC:104 |
| Foreground | #C0C0C0FF | SC:106 |
| ForegroundHover | #FFFFFFFF | SC:107 |
| ForegroundInverted | = Input (0F0F0FFF) | SC:108 |
| ForegroundHeader | #C8C8C8FF | SC:109 |
| Select | = Primary (#0070E0FF) | SC:111 |
| SelectInactive | #40576F (alpha FF implied) | SC:112 |
| SelectParent | #2C323AFF | SC:113 |
| SelectHover | = Panel (#242424FF) | SC:114 |
| Notifications | 464B50FF | SC:116 |
| AccentBlue | #26BBFFFF | SC:120 |
| AccentPurple | #A139BFFF | SC:121 |
| AccentPink | #FF729CFF | SC:122 |
| AccentRed | #FF4040FF | SC:123 |
| AccentOrange | #FE9B07FF | SC:124 |
| AccentYellow | #FFDC1AFF | SC:125 |
| AccentGreen | #8BC24AFF | SC:126 |
| AccentBrown | #804D39FF | SC:127 |
| AccentBlack | #242424FF | SC:128 |
| AccentGray | #808080FF | SC:129 |
| AccentWhite | #FFFFFFFF | SC:130 |
| AccentFolder | #B68F55FF | SC:131 |
| Warning | #FFB800FF | SC:133 |
| Error | #EF3535FF | SC:134 |
| Success | #1FE44BFF | SC:135 |
| User1..User16 | no default (user slots) | SCH:85-100 |

Non-theme "dynamic" colors in core style (SCS:60-64, user-customizable):

| Name | Value as written | Approx sRGB (computed, not in source) |
|---|---|---|
| SelectorColor | linear (0.701, 0.225, 0.003) | ~#DB830A (orange) |
| SelectionColor | COLOR("18A0FBFF") | #18A0FB |
| SelectionColor_Inactive | linear (0.25, 0.25, 0.25) | ~#898989 |
| SelectionColor_Pressed | linear (0.701, 0.225, 0.003) | ~#DB830A |
| HighlightColor (ref) | linear (0.068, 0.068, 0.068) | ~#494949 |

`DefaultForeground` = Foreground, `InvertedForeground` = ForegroundInverted, `HighlightColor` (Slate color used) = `FStyleColors::Highlight` (SCS:171-177).

## 2. Fonts

Composite default font (LegacySlateFontInfoCache.cpp:111-122): Regular=Roboto-Regular, Italic=Roboto-Italic, Bold=Roboto-Bold, BoldItalic=Roboto-BoldItalic, BoldCondensed=Roboto-BoldCondensed, BoldCondensedItalic, Black, BlackItalic, Medium=Roboto-Medium, Light=Roboto-Light, VeryLight=Roboto-Light (Auto hinting), Mono=DroidSansMono.ttf; fallback DroidSansFallback (:127). Sizes are points at `RenderDPI = 96` (SlateFontInfo.h:15) → px = pt x 96/72.

| FStyleFonts / style name | Typeface | Size | Line |
|---|---|---|---|
| Normal ("NormalFont") | Regular | 10 | SCS:34, :1260 |
| NormalItalic | Italic | 10 | SCS:35 |
| NormalBold ("NormalFontBold") | Bold | 10 | SCS:36 |
| NormalBoldItalic | BoldItalic | 10 | SCS:37 |
| Small ("SmallFont") | Regular | 8 | SCS:38 |
| SmallBold ("SmallFontBold") | Bold | 8 | SCS:39 |
| Large ("Font.Large") | Regular | 14 | SCS:40 |
| LargeBold ("Font.Large.Bold") | Bold | 14 | SCS:41 |
| HeadingMedium | BoldCondensed | 33 | SCS:42 |
| HeadingSmall | BoldCondensed | 21 | SCS:43 |
| HeadingExtraSmall | BoldCondensed | 15 | SCS:44 |
| ToolTip.Font | Regular | 9 | SCS:510 |
| HintText | Italic | 8, color UseSubduedForeground | SCS:1404-1407 |
| MonospacedText | Mono | 10 | SCS:1282 |
| Log.Normal / MessageLog / SyntaxHighlight | Mono | 9 | SCS:901, :870, :887 |
| Menu.Heading | Bold 7, LetterSpacing 250, color White25 | 7 | SCS:2532-2538 |
| Menu.Keybinding | Small (8) | 8 | SCS:2529 |
| PrimaryButtonText | NormalBold (10) | 10 | SCS:1511 |
| NotificationList.FontBold / FontLight | Bold / Light | 12 | SCS:700-701 |
| PropertyWindow.NormalFont / BoldFont | Small / SmallBold | 8 | ES:3657-3658 |
| DetailsView.CategoryFontStyle | SmallBold | 8 | ES:3805 |
| DetailsView.NameTextBlockStyle | Regular, color White | 11 | ES:3683-3689 |

`NormalText` (SCS:1252-1258): font Normal, color UseForeground, shadow 0, SelectedBackgroundColor = Highlight, HighlightColor = Black, HighlightShape = Color(AccentGreen).

## 3. Constants

| Constant | Value | Line |
|---|---|---|
| InputFocusRadius | 4.0 | CS:61 |
| InputFocusThickness | 1.0 | CS:62 |
| DefaultMargins | (8, 4) | CS:52 |
| ButtonMargins | (12, 1.5, 12, 1.5) | CS:54 |
| PressedButtonMargins | (12, 2.5, 12, 0.5) (press shifts content down 1) | CS:56 |
| ToggleButtonMargins | (16, 2) | CS:57 |
| ComboButtonMargin | (8, 1, 8, 1) | CS:58 |
| PressedComboButtonMargin | (8, 2, 8, 0) | CS:59 |
| Icon sizes | Icon8x8 ... Icon128x128 | CS:28-49 |
| Checkbox corner radius / outline | 3.0 / 1.0 (local consts) | SCS:1663-1664 |
| Dock tab maximum size (Panel / Major) | 160×25 / 210×50; all tabs in a well share one width (W − Overlap)/N + Overlap | SDockingTabWell.cpp:9-10, 304-342 |
| Button corner radius | 4.0 (literal, no named const) | SCS:1446-1464 |
| FocusRectangle | RB(Transparent, 4, Primary, 1) | SCS:217 |
| Window corner radius | 2 (SetCornerRadius(2)); NotificationWindow 8 | SCS:819, :832 |
| Default brush outline settings | radii 0, HalfHeightRadius, width 0 | SB:134-140 |
| PropertyRowHeight (details) | 26.0 | PropertyEditor/Private/UserInterface/PropertyEditor/PropertyEditorConstants.h:28 |
| FMargin default for FEditableTextBoxStyle | (4, 2) | ST:292 |
| FCheckBoxStyle default Padding | (2, 0, 0, 0) | ST:24 |
| FScrollBarStyle default Thickness | 16 (Starship overrides to 8) | ST:390 |
| FSpinBoxStyle default TextPadding / InsetPadding | (1, 2) / 0 | ST:564-565 |

## 4. Widget styles

Rounded boxes: radius in Slate units; outline drawn **inside** the rect (§5). FButtonStyle has no focused state — keyboard focus is drawn with the separate `FocusRectangle` brush. No widget style has a disabled image except Button/ComboButton `Disabled`; other widgets use the shader disabled effect (alpha x 0.45, SEPS:625-646, `USE_LEGACY_DISABLED_EFFECT 0` at SEPS:32).

### 4.1 Buttons (SCS:1442-1555)

| Style | Normal | Hovered | Pressed | Disabled | Foreground N/H/P/D | Padding N / P |
|---|---|---|---|---|---|---|
| `Button` / `SecondaryButton` (1460-1470, 1516) | RB(Secondary,4,Input,1) | RB(Hover,4,Input,1) | RB(Header,4,Input,1) | RB(Dropdown,4,Recessed,1) | FgHover/FgHover/FgHover/Foreground | ButtonMargins / PressedButtonMargins |
| `PrimaryButton` (1445-1455) | RB(Primary,4,Input,1) | RB(PrimaryHover,4,Input,1) | RB(PrimaryPress,4,Input,1) | RB(Dropdown,4,Recessed,1) | same as Button | same; label+icon variant (4,1.5,4,1.5)/(4,2.5,4,0.5) (1457-1458) |
| `EditorUtilityButton` (1472-1475) | as Button, image size 32x32 | | | | | |
| `SimpleButton` (=IconOnly, 1493-1495, 1508) | None | RB(Dropdown,4) | RB(Dropdown,4) | None | Foreground/FgHover/FgHover/Foreground | (2,1,2,1) / (2,2,2,0) |
| `SimpleButtonLabelAndIcon` (1489-1491) | as SimpleButton | | | | | (4,1,4,1) / (4,2,4,0) |
| `HoverOnlyButton` (1540-1546) | None all | | | | | (2,0)/(2,0); `ThinHoverOnlyButton` 0/0 (1548-1553) |
| `InvisibleButton` (1526-1538) | None all | | | | White all | 0 |
| `NoBorder` (233-242) | None all | | | | FgHover all | 0 |

Text: `ButtonText` = NormalText + ellipsis overflow (1499-1501).

### 4.2 CheckBox / Toggle / Radio / Segmented (SCS:1661-1842)

| Style | Unchecked N/H/P | Checked N/H/P | Undetermined | Background N/H/P | Foreground | Padding |
|---|---|---|---|---|---|---|
| `Checkbox` (1667-1691) type CheckBox | None/None/None | SVG "Starship/CoreWidgets/CheckBox/check" 16x16 tinted Primary / PrimaryHover / PrimaryPress | SVG ".../indeterminate" 16x16 Primary/PrimaryHover/PrimaryPress | RB(Input,3,Secondary,1) 18x18 / RB(Input,3,Hover,1) / RB(Input,3,Hover,1) | ForegroundHover all states | default (2,0,0,0) |
| `TransparentCheckBox` (1694-1712) ToggleButton | None all | None all | None | — | Fg / FgHover (hover, pressed, checked*) | default |
| `ToggleButtonCheckbox` (1715-1731) | None / RB(Dropdown,4) / RB(Dropdown,4) | RB(Primary,4) / RB(PrimaryHover,4) / RB(Dropdown,4) | — | — | Fg; FgHover for hover/pressed/checked | ToggleButtonMargins (16,2) |
| `ToggleButtonCheckboxAlt` (1734-1750) | None / RB(Hover,4) / RB(Input,4) | RB(Input,4) / RB(Hover,4) / RB(Input,4) | — | — | Fg/FgHover; checked White | (16,2) |
| `SegmentedControl` (1752-1776) control | None / RB(Hover,4) / RB(Header,4) | RB(Dropdown,4) / RB(Hover,4) / RB(Header,4) | — | control bg RB(Input,4), UniformPadding 1 | Fg; FgHover others | (8,3) |
| `RadioButton` (1820-1841) CheckBox type | SVG "radio-off" 16x16 Dropdown / Hover / Dropdown | SVG "radio-on" 16x16 Primary / PrimaryHover / Primary | radio-off White25 / FgHover / FgHover | — | linear White all | (4) |
| `Menu.CheckBox` (2564-2589) | None | SVG check White | indeterminate White | RB(Panel,3,Hover,1) 18x18 all | White | default |
| `Menu.RadioButton` (2595-2607) | RB(Input) 8x8 pill / RB(Input) / RB(Hover2) | RB(White) 8x8 all | Input / White / White | — | — | default |
| `FilterBar.BasicFilterButton` (957-965) | RB(Header,4,Input,1) / RB(Hover,4,Input,1) / same | RB(Primary,4,Input,1) / RB(PrimaryHover,4,Input,1) / same | — | — | from ToggleButtonCheckbox | (16,4) |

### 4.3 ComboButton / ComboBox (SCS:1557-1659)

| Item | Value | Line |
|---|---|---|
| ComboButton button Normal / Hovered / Pressed / Disabled | RB(Input,4,InputOutline,1) / RB(Input,4,Hover,1) / RB(Input,4,Hover,1) / RB(Dropdown,4,DropdownOutline,1) | 1560-1564 |
| Foreground N/H/P/D | Foreground / FgHover / FgHover / FgHover | 1565-1568 |
| Padding N / P | ComboButtonMargin (8,1,8,1) / (8,2,8,0) | 1569-1570 |
| Down arrow | SVG "Starship/CoreWidgets/ComboBox/wide-chevron-down" 16x16 | 1581 |
| Menu border | RB(Dropdown, 0, DropdownOutline, 1) (square corners), MenuBorderPadding 0, ContentPadding 0 | 1580-1583 |
| ComboBox | ContentPadding 0, MenuRowPadding (8,3) | 1591-1595 |
| SimpleComboButton | button SimpleButtonLabelAndIcon, same arrow & menu border, ShadowOffset 0 | 1604-1611; SimpleComboBox row pad (8,2) 1616-1620 |
| `ComboBox.Row` (table row inside dropdown) | even/odd: None, hovered Color(Primary); active None, active/inactive hovered & highlighted Color(Primary); SelectorFocused RB(Transparent,4,Select,1); text White, selected text White | 1622-1646 |

### 4.4 EditableTextBox / SearchBox / InlineEditableTextBlock (SCS:1318-1440)

| Item | Normal | Hovered | Focused | ReadOnly | Other | Line |
|---|---|---|---|---|---|---|
| `NormalEditableTextBox` | RB(Input,4,InputOutline,1) | RB(Input,4,Hover,1) | RB(Input,4,Primary,1) | RB(Header,4,InputOutline,1) | font Normal; padding (8,3,8,4); fg Foreground; focused fg White; readonly fg UseForeground; BackgroundColor White; scrollbar = Scrollbar | 1364-1378 |
| `NormalEditableText` | selection bg Color(Highlight); caret Color(White); composing BORDER "Old/HyperlinkDotted" | | | | | 1346-1356 |
| `SearchBox` text box (no radius = pill) | RB(Input, Secondary, 1) | RB(Input, Hover, 1) | RB(Input, Primary, 1) | RB(Header, InputOutline, 1) | inherits NormalEditableTextBox padding | 1383-1389 |
| `SearchBox` images | up/down: SVG chevron-up/chevron-down 16 Foreground; glass SVG "search"; clear SVG "close"; ImagePadding (4,0,0,0); ImageSizeOverride 14; left-aligned glass & clear | | | | | 1391-1401 |
| `FilterBar.SearchBox` | SearchBox with padding (8,3,30,4) | | | | | 928-935 |
| `InlineEditableTextBlockStyle` | text NormalText; edit box NormalEditableTextBox | | | | Small variant: font Small both; Hint variant: HintText | 1409-1439 |
| `DarkEditableTextBox` (legacy images) | BOX "Common/TextBox_Dark" 4/16 | "TextBox_Hovered_Dark" | same | "TextBox_ReadOnly" | | 1318-1326 |

### 4.5 SpinBox / NumericEntry (SCS:611-645)

| Brush | `SpinBox` = `NumericEntrySpinBox` | `NumericEntrySpinBox_Dark` | `Menu.SpinBox` (2495-2504) |
|---|---|---|---|
| Background | RB(Input,4,InputOutline,1) | same | RB(Input,4,Secondary,1) |
| HoveredBackground | RB(Input,4,Hover,1) | same | RB(Input,4,Hover,1) |
| ActiveBackground | RB(Input,4,Primary,1) | (not set) | (not set) |
| ActiveFill | RB(Primary,4,Transparent,1) | RB(Hover,4,...) | RB(Hover,4,...) |
| HoveredFill | RB(Hover,4,Transparent,1) | (not set) | (not set) |
| InactiveFill | RB(Dropdown,4,Transparent,1) | RB(Secondary,4,...) | RB(Secondary,4,...) |
| Arrows | None | None | None |
| Foreground | ForegroundHover | ForegroundHover | ForegroundHover |
| TextPadding / InsetPadding | (7,2.5,7,2.5) / 1 | 0 / default | (10,3.5,10,4) / default |

Decorators: `NumericEntrySpinBox.Decorator` BOX "Common/TextBoxLabelBorder" 5/16 (642); `NarrowDecorator` SVG ".../NumericEntryBox/NarrowDecorator" 2x16 (644).

### 4.6 Slider (SCS:589-609)

| State | Bar | Thumb |
|---|---|---|
| Normal | RB(Input,2,Input,1) | RB(Foreground) 8x8 → circle (HalfHeight) |
| Hovered | RB(Input,2,Input,1) | RB(ForegroundHover) 8x8 |
| Disabled | RB(Input,2,Recessed,1) | RB(Foreground) 8x8 |
| BarThickness | 4.0 | |

`ColorPicker.Slider` thumbs: image "Common/ColorPicker_SliderHandle" 8x32 (2066-2070).

### 4.7 ScrollBar / ScrollBox (SCS:186-195, 736-753)

| Item | Value |
|---|---|
| Thumb normal / hovered / dragged | RB(Hover,4) / RB(Hover2,4) / RB(Hover2,4) |
| Track backgrounds, top/bottom slot images | None (FScrollBarStyle default, ST:381-389) |
| Thickness | 8.0 (SCS:191) |
| ScrollBox shadows | BOX "Common/ScrollBoxShadowTop/Bottom" 16x8, "…Left/Right" 8x16; BarThickness 8; `ScrollBoxNoShadow` = None shadows |
| ScrollBorder | BOX "Common/ScrollBorderShadowTop/Bottom" 16x8 (757-765) |

### 4.8 ProgressBar (SCS:561-569)

Background Color(Foldout); Fill and Marquee = image "Starship/CoreWidgets/ProgressBar/ProgressMarquee" 20x12 tinted PrimaryHover, tile horizontal; EnableFillAnimation true.

### 4.9 ExpandableArea (SCS:579-587)

Collapsed SVG "Starship/Common/caret-right" 16 DefaultForeground; expanded SVG "caret-down" 16; `ExpandableArea.TitleFont` SmallBold; `ExpandableArea.Border` RB(Panel,4).

### 4.10 TableView rows & header (SCS:2161-2294)

`TableView.Row` (2174-2199):

| Brush | Value |
|---|---|
| Even / Odd row | Color(Recessed) / Color(Recessed) |
| Even / Odd hovered | Color(SelectHover)=Panel |
| SelectorFocused | BORDER "Common/Selector" 4/16 tinted SelectorColor |
| Active / ActiveHovered (selected, focused list) | Color(Select)=Primary |
| Inactive / InactiveHovered (selected, unfocused) | Color(SelectInactive) |
| Active/Inactive Highlighted (parent of selection) | Color(SelectParent) |
| Text / SelectedText | Foreground / ForegroundHover |
| Drop indicators | BOX "Common/DropZoneIndicator_Above/Onto/Below" tinted SelectionColor |

Variants: `SimpleTableView.Row` even/odd None, selected text ForegroundInverted (2204-2229); `TableView.AlternatingRow` even Recessed / odd Background (2243-2246); `TableView.DarkRow` image "Common/Selection" black 10% (2238-2241); `ShowParentsTableView.Row` parent RB(Header,2,Transparent,1), hovered RB(SelectHover,2) (2231-2236). Tree arrows: SVG "caret-right"/"caret-down" 16, Foreground / ForegroundHover (2248-2251). ListView/TreeView = default FTableViewStyle (2166-2172).

`TableView.Header` (2254-2293):

| Item | Value |
|---|---|
| Column normal / hovered | Color(Header) / Color(Dropdown) |
| Sort icons | SVG "Starship/CoreWidgets/TableView/sort-up-arrow", "sort-down-arrow", "sort-up-arrows", "sort-down-arrows" 12x12 |
| Menu dropdown | SVG "Starship/Common/ellipsis-vertical-narrow" 6x24; borders None |
| Column splitter | handle Color(Recessed), size 1.0 |
| Background | Color(Recessed); foreground Foreground |
| Horizontal separator | Color(Recessed), thickness 2.0 |
| Header row height | NOT FOUND in style |

### 4.11 Menu / MenuBar (SCS:2492-2655)

| Item | Value | Line |
|---|---|---|
| Menu.Background | Color(Dropdown) | 2507 |
| Menu.Outline | RB(Transparent, 0, DropdownOutline, 1) | 2508 |
| Menu.WidgetBorder | RB(Input, 5) | 2494 |
| Menu.Block.Padding / IndentedPadding | (12,1,5,1) / +18 left | 2519-2521 |
| Menu.Separator / padding | Color(White25) / (12,6,12,6) | 2523-2524 |
| Menu.Heading padding | (12,6,12,6) | 2539 |
| Menu.MenuIconSize | 14 | 2517 |
| Menu.SubMenuIndicator | SVG "chevron-right" 16 Foreground | 2511 |
| Menu.Label | NormalText | 2526 |
| Menu.Button N / H / P | None / Color(Primary) / Color(Primary); hovered fg FgHover; padding (0,2) / (0,3,0,1) | 2622-2631 |
| Menu.Check | checked SVG "Starship/Common/check" 16; undetermined same at white 1% / 20% | 2547-2558 |
| Menu.ToggleButton | BOX "Common/RoundedSelection_16x" 4/16, SelectionColor (hover) / SelectionColor_Pressed (pressed/checked) | 2610-2620 |
| WindowMenuBar.Button | from Menu.Button: hovered Color(Hover); fg Foreground / FgHover | 2639-2651 |
| WindowMenuBar.Button.SubMenuOpen | Color(Primary) | 2652 |
| WindowMenuBar.MenuBar.Padding | (12, 4) | 2653 |
| WindowMenuBar.Background | None; label font Normal | 2648-2649 |

### 4.12 ToolTip (SCS:508-515; Slate/Private/Widgets/SToolTip.cpp, Public/Widgets/SToolTip.h)

Background RB(Dropdown, 0, DropdownOutline, 1) (square); font Regular 9; default TextMargin 8 (SToolTip.h:30); `ToolTip.BrightBackground` BOX "Old/ToolTip_BrightBackground" 8/64 used for rich tooltips with padding 11 (SToolTip.cpp:75-76).

### 4.13 Docking tabs (SCS:1844-2038)

| Item | `Docking.Tab` (minor/panel) 1876-1899 | `Docking.MajorTab` 1902-1923 |
|---|---|---|
| Normal | None | None |
| Hovered | BOX "/Starship/Docking/DockTab_Foreground" 4/20 tinted Panel @ 0.8 opacity | same |
| Foreground (active) | same image tinted Panel | same |
| ColorOverlayTab / Icon | Color(Foreground) / None | Color(Foreground) / RB(Foreground,4) |
| ContentArea / TabWell | Color(Panel) / Color(Background) | Color(Background) / Color(Background) |
| TabPadding | (4,3,10,4) | (4,4,10,4) |
| OverlapWidth | -2 (gap) | -2 |
| Fg normal/active/foreground/hovered | Foreground / FgHover / Foreground / FgHover | Foreground / White / White / White |
| Flash color | SelectionColor | SelectionColor |
| Close button | SVG "Starship/Common/close-small" 16, Foreground / hovered FgHover | same |
| IconBorderPadding | — | 4 |

Others: `Docking.Border` RB(Panel,4) (1853); `Docking.Tab.InactiveTabSeparator` Color(Hover) (1927); `Docking.Tab.ActiveTabIndicatorColor` Primary (1929); sidebar border RB(Transparent,5,Hover,1) (2000-2004); StatusBar height 32, bg Color(Panel) (1973-1977). `DockColor_*` FColor(45/54/62) declared 1872-1874 but unused in the shown code.

### 4.14 Hyperlink (SCS:527-539)

Underline button: normal BORDER "Old/HyperlinkDotted" margin (0,0,0,3/16); hovered BORDER "Old/HyperlinkUnderline"; pressed None. Text NormalText (UseForeground). Padding 0.

### 4.15 Separator / Splitter / Border (SCS)

| Name | Value | Line |
|---|---|---|
| Separator | Color(Recessed) | 725 |
| Splitter | handle Color(Background), highlight Color(Secondary) | 651-654 |
| SplitterPanel | handle Color(Panel), highlight FColor(96,96,96) = #606060 | 655-658 |
| Border | Color(Panel) | 519 |
| FloatingBorder | RB(Panel @ alpha 0.5, 8) | 521-524 |
| ToolPanel.GroupBorder | Color(Panel) | 452 |
| Window | bg Color(Recessed); border RB(Recessed,2,WindowBorder,2); outline RB(Recessed,2,InputOutline,1); corner 2; border padding 3 | 815-820 |

### 4.16 ToolBar (SCS:2314-2490)

| Item | `ToolBar` (2316-2362) | `SlimToolBar` (2367-2460) |
|---|---|---|
| Background / padding | Color(Panel) / (0,4) | Color(Panel) / (4,6) |
| Button N/H/P | None / BOX "Common/RoundedSelection_16x" 4/16 SelectionColor / same SelectionColor_Pressed | SimpleButton: None / RB(Dropdown,4) / RB(Dropdown,4) |
| Button padding N / P | (2) / (2,3,2,1) | (8,4) / (8,5,8,3) |
| Button fg | UseForeground | Foreground; FgHover hovered/pressed |
| Toggle | RoundedSelection_16x Selection/Pressed colors | ToggleButtonCheckbox with padding (8,4) |
| Icon size | 40x40 | 20x20 |
| Separator | Color(Background), pad (1,0) | Color(Recessed), pad (4,-5) |
| Block / button / combo / checkbox padding | block (2,2,4,4), indented (18,2,4,4); button (2,0); combo (4,0); check (4,0) | button (4,0); combo 0; check (8,0); label pad (4,0,0,0) |
| Wrap (overflow) | SVG "Common/menu" 16 | combo button Color(Dropdown)/Hover/Header, pad (6,4) |

Settings region brushes (2452-2457): Left RB(Dropdown,(4,0,0,4),Header,1), Right RB(Dropdown,(0,4,4,0),Header,1), hover variants RB(Hover,...), LeftToggle RB(Primary,(4,0,0,4)).
Editor viewport toolbar (ES:2619-2759): bg RB(Dropdown,5,Dropdown,1), hover RB(Hover,5,...), pressed RB(Header,5,...); Start/Middle/End radii (5,0,0,5)/(0,0,0,0)/(0,5,5,0); fg ForegroundHeader / FgHover / FgHover / disabled Hover2; padding (6,4); toggle checked fg AccentBlue; icon 16; separator Hover thickness 1 pad (4,0); arrow SVG "Starship/EditorViewport/small-chevron-down" 6x6.

### 4.17 ColorPicker (SCS:2040-2159; editor adds only ES:8838-8840 "ColorPicker.ColorThemes" SVG)

| Name | Value | Line |
|---|---|---|
| RoundedSolidBackground | RB(White, 4) | 2044 |
| RoundedAlphaBackground | RB with image "Starship/Common/Checker" 16x16 tiled, radius 4, no outline | 2045 |
| RoundedInputBorder / Active / Hovered | RB(Transparent,4,InputOutline,1) / (…,Primary,1) / (…,Hover,1) | 2046-2048 |
| MultipleValuesBackground | RB(Input, 4) | 2049 |
| RecessedBackground | RB(Recessed, 8) | 2050 |
| Font / SmallFont | Normal / Small | 2052-2053 |
| AlphaBackground | image "Starship/Common/Checker" 16 tiled | 2055 |
| Selector / wheel / spectrum | "Common/Circle" 8x8; "Common/ColorWheel" 192; "Common/ColorSpectrum" 256 | 2062, 2139, 2096 |
| HexMode combo button | RB(Panel,4,Panel,1) all states, content pad 0 | 2072-2081 |
| ColorSlider.SpinBox | SpinBox with all fills RB(Input,4,Transparent,1) | 2086-2091 |
| ThemesComboButton | RB(Dropdown,4,Input,1) all states | 2110-2118 |
| AddButton | RB(Transparent,4,DropdownOutline,1); hover/press outline Hover2; pad 0 | 2120-2128 |
| DeleteButton | normal outline Transparent; hover DropdownOutline | 2130-2134 |
| ColorPreviewButton | RB(Transparent,4,Transparent,1); hover/press fill black 40% | 2150-2157 |
| CursorDecorator window | corner radius 4 | 2106-2108 |
| Icons (names only) | "Icons/eyedropper_16px", "Starship/Common/EyeDropper_20", "Common/ColorWheel_20x", "Common/ColorSpectrum_20x", "Starship/Common/ColorThemes_20/_16", "ColorThemesOff_20" | 2056-2104 |

### 4.18 DetailsView / PropertyEditor (editor; ES:3541-3877 + PropertyEditor module)

| Item | Value | Line |
|---|---|---|
| Category (classic) top / hovered | Color(Header) / Color(Hover) | ES:3723, 3775 |
| CollapsedCategory / hovered | Color(Header) / Color(Hover) | ES:3721-3722 |
| Category (card style) | RB(Header,(4,4,4,4)) collapsed; RB(Header,(4,4,0,0)) expanded; left/right side variants | ES:3727-3732; DetailsViewStyle.cpp:259-288 |
| CategoryBottom | Color(Recessed) | ES:3776 |
| CategoryMiddle (inner rows) | Color(White) brush, real color from `GetRowBackgroundColor` | ES:3779 |
| Row background color | base Panel (hovered: Header) + sRGB offset per indent: offsets {0,4,6,10,16}, index ping-pongs 0→3 with indent | PropertyEditorConstants.cpp:25-57 |
| Category text | SmallBold, color ForegroundHeader; "Update" variant Small + ToUpper | ES:3805-3815 |
| Category combo button | RB(Header,0) N / RB(Input,0) H / Header P,D; pad (2,0,2,0); arrow SVG "ellipsis-vertical-narrow" 6x15, pad (2,5,3,5) | ES:3760-3773 |
| Highlight | RB(Transparent, 0, AccentBlue, 1) | ES:3780 |
| Splitter / GridLine | Color(Recessed) / Color(Recessed) | ES:3786-3791 |
| PropertyWindow.PropertyRow | TableView.Row with even/odd Color(Panel) | ES:3644-3648 |
| DetailsView.TreeView.TableRow | all None; text DefaultForeground / Inverted | ES:3848-3863 |
| Row height | 26.0 (MinDesiredHeight) | PropertyEditorConstants.h:28; SDetailSingleItemRow.cpp:835; SDetailCategoryTableRow.cpp:179 |
| Name / value column padding | Left (20,0,10,0); Right (12,0,2,0) | SDetailSingleItemRow.cpp:39-40 |
| Value widget min/max width | 125 / 125 | DetailWidgetRow.cpp:6-7 |
| Row padding | outer category (0, TopCategoryPadding, 0, 1), else (0,0,0,1); Card TopCategoryPadding 8, table padding (8,0,20,8) with scrollbar / (8,0,8,8) without; Default 0 | DetailsViewStyle.cpp:233-238, 292-299 |
| ChannelToggleButton | unchecked RB(Input,4,DropdownOutline,1)/H RB(Input,4,Hover,1)/P RB(Hover,4,DropdownOutline,1); checked RB(Primary,4,DropdownOutline,1)/H RB(Primary,4,Hover,1); pad (16,4); checked fg White | ES:3794-3803 |
| SectionButton | = FilterBar.BasicFilterButton | ES:3792 |
| NameAreaButton | Button with pad (6,3) | ES:3867-3870 |
| PropertyWindow.EditConstColor / FilteredColor | FColor(152,152,152,80) / FColor(0,255,0,80) | ES:3671-3672 |
| AssetThumbnailBorder / Hovered | RB(Transparent,4,InputOutline,1) / (…,Hover2,1) | ES:3556-3557 |
| Pulldown arrows | SVG chevron-down / chevron-up 16, Foreground / FgHover | ES:3712-3715 |
| Icons (names) | "Starship/Common/ResetToDefault", "lock", "lock-unlocked", "layout-spreadsheet", "settings", "Starship/DetailsView/DetailsOverride*" | ES:3653-3757 |
| Category expander arrow, header height beyond 26 | NOT FOUND as separate style | — |

---

## 5. Rounded box rendering

**Brush → element** (DET:296-319):
- `CornerRadii` from `OutlineSettings.CornerRadii` (order X=TopLeft, Y=TopRight, Z=BottomRight, W=BottomLeft, SB:198).
- If `RoundingType == HalfHeightRadius`, all four radii = `LocalSize.Y / 2` (DET:300-305). Constructors without a radius argument (RBB:19, RBB:116) leave the default outline settings → HalfHeightRadius (SB:134-140, 158-163). With a radius (RBB:75-88, RBB:165-177) → FixedRadius.
- Outline color = `OutlineSettings.Color.GetSpecifiedColor()`, or with its alpha replaced by the tint alpha when `bUseBrushTransparency` (DET:307-315). Outline width stored as `OutlineWeight`.

**Per-element data to GPU** (EB:772-786 vertex path, EB:1125-1134 batch params):
- Pixel-shader **uniforms** (per batch): `ShaderParams.PixelParams = (0, OutlineWeight, LocalSize.X, LocalSize.Y)`, `PixelParams2 = CornerRadii` (SEPS:29-30 `float4 ShaderParams; float4 ShaderParams2;`).
- **Vertex** data: fill = `Color` (tint, packed FColor), outline = `SecondaryColor` (packed `OutlineColor`), `MaterialTexCoords = LocalPosition / LocalSize` (0..1 across the box; RenderingCommon.h:288-296).
- Because ShaderParams is part of the batch key (`IsBatchableWith`, SlateRenderBatch.h:30-41), every rounded box with a different size/radius/outline width becomes its own batch/draw.
- Geometry = the box quad only; `GSlateFeathering = 0` (EB:36), so no extra AA skirt.
- Colors: `PackVertexColor` → `ToFColor(bSRGBVertexColor)` (ElementBatcher.h:282-287); VS converts both colors `sRGBToLinear` since `SOURCE_IN_LINEAR_SPACE 1` (SSC:33, SVS:27-34). Fill also multiplied by texture sample (`GetColor` = texture x vertex color, SEPS:299-325; no brush texture → `GWhiteTexture`, RP:1264).

**Pixel shader math** (`GetRoundedBoxElementColor` SEPS:470-473 → `GetRoundedBoxElementColorInternal(size=ShaderParams.zw, UV, thickness=ShaderParams.y, radii=ShaderParams2, fill, border)` SSC:84-119):

```
pos    = size * UV; center = size / 2                       // SSC:86-87
q      = step(UV, 0.5)                                      // SSC:92, 1 on left/top half
top    = lerp(r.y, r.x, q.x); bottom = lerp(r.z, r.w, q.x)  // SSC:94-95
radius = lerp(bottom, top, q.y)                              // SSC:96
dist(p, c, R, inset):                                        // SSC:69-82
  p = abs(p - c) - (c - (R + inset))
  return (p.x <= 0 || p.y <= 0) ? max(p.x - R, p.y - R)      // straight-edge zone
                                : length(p) - R              // corner arc
dext = dist(pos, center, radius, 0)                          // outer edge, SSC:99
din  = dist(pos, center, max(radius - thickness, 0), thickness) // inner edge inset by thickness, SSC:100
bi = smoothstep(+1.0, -1.0, dext)    // border coverage, AA half-width 1.0 local unit (SSC:103-104)
fi = smoothstep(+0.5, -0.5, din)     // fill coverage,   AA half-width 0.5 (SSC:106-107)
out   = lerp(border, fill, thickness > radius); out.a = 0    // SSC:113-114
out   = lerp(out, border, bi)                                // SSC:117
out   = lerp(out, fill, fi)                                  // SSC:118
```

Notes:
- SDF is the standard rounded-rect distance with the "use edge distance outside the corner quadrant" fix for radius ~ thickness (SSC:76-81 comments).
- Outline is **inside** the box: fill region is the box inset by `thickness` with radius `radius - thickness`; border occupies the ring between. Fill is drawn over the border (`lerp(out, fill, fi)`), not composited by coverage.
- AA widths are in **local Slate units** (size is LocalSize, not pixels), so at DPI scale != 1 the transition is not exactly 1 px. Since the quad ends at dext = 0, the outer ramp is cut: at the boundary bi = 0.5, at a pixel center 0.5 inside bi ≈ 0.84.
- Output is **straight (non-premultiplied)** alpha: rgb is lerped together with a; outside pixels carry border/fill rgb with a = 0 (avoids dark fringes). Blend state for normal Slate batches = `SrcAlpha, InvSrcAlpha` (alpha: `One, InvSrcAlpha`); `One, InvSrcAlpha` only with `ESlateBatchDrawFlag::PreMultipliedAlpha`; opaque with `NoBlending` (RP:1254-1261).
- After shading: disabled effect (alpha x 0.45, SEPS:625-646), `ApplyContrast`, `GammaCorrect` (SEPS:650-653).
