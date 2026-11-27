#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/PaintList.h>
#include <UiCore/StyleSet.h>

using namespace rocks;
using namespace rocks::ui;

// 스타일 기반 시험이다(Docs/Design/UiToolkit 9.1절 UW3b). 기본 스타일 값을 14장(UE 5.8
// StarshipCoreStyle.cpp·CoreStyle.cpp)과 대조하고, 테마로 푼 brush와 이름 표를 단언한다.

namespace {

void CheckRoundedBox(const StyleBrush& brush, ThemeSlot fill, ThemeSlot outline) noexcept {
    CHECK(brush.mode == BrushMode::RoundedBox);
    CHECK(brush.fill == ColorRef::FromTheme(fill));
    CHECK(brush.outlineColor == ColorRef::FromTheme(outline));
    CHECK(brush.outlineWidth == 1.0f);
    CHECK(brush.cornerRadii == Vec4(4, 4, 4, 4));
    CHECK(brush.rounding == BrushRounding::Fixed);
}

} // namespace

TEST(UiCore_DefaultButtonStyleMatchesUe) {
    // StarshipCoreStyle.cpp 1460–1470행, CoreStyle.cpp 54–56행.
    const ButtonStyle& style = DefaultButtonStyle();
    CheckRoundedBox(style.normal, ThemeSlot::Secondary, ThemeSlot::Input);
    CheckRoundedBox(style.hovered, ThemeSlot::Hover, ThemeSlot::Input);
    CheckRoundedBox(style.pressed, ThemeSlot::Header, ThemeSlot::Input);
    CheckRoundedBox(style.disabled, ThemeSlot::Dropdown, ThemeSlot::Recessed);
    CHECK(style.normalPadding == Vec4(12, 1.5f, 12, 1.5f));
    CHECK(style.pressedPadding == Vec4(12, 2.5f, 12, 0.5f));
    CHECK(style.normalForeground == ColorRef::FromTheme(ThemeSlot::ForegroundHover));
    CHECK(style.hoveredForeground == ColorRef::FromTheme(ThemeSlot::ForegroundHover));
    CHECK(style.pressedForeground == ColorRef::FromTheme(ThemeSlot::ForegroundHover));
    CHECK(style.disabledForeground == ColorRef::FromTheme(ThemeSlot::Foreground));
}

TEST(UiCore_PrimaryAndSimpleButtonStylesMatchUe) {
    // 1445–1455행: 주 동작 버튼은 채움만 Primary 계열이고 비활성·여백·전경은 Button과 같다.
    const ButtonStyle& primary = PrimaryButtonStyle();
    CheckRoundedBox(primary.normal, ThemeSlot::Primary, ThemeSlot::Input);
    CheckRoundedBox(primary.hovered, ThemeSlot::PrimaryHover, ThemeSlot::Input);
    CheckRoundedBox(primary.pressed, ThemeSlot::PrimaryPress, ThemeSlot::Input);
    CHECK(primary.disabled == DefaultButtonStyle().disabled);
    CHECK(primary.normalPadding == DefaultButtonStyle().normalPadding);
    // 1475–1495행: 단순 버튼은 보통·비활성을 그리지 않고 올림·눌림만 외곽선 없는 Dropdown이다.
    const ButtonStyle& simple = SimpleButtonStyle();
    CHECK(simple.normal.mode == BrushMode::None && simple.disabled.mode == BrushMode::None);
    CHECK(simple.hovered.mode == BrushMode::RoundedBox);
    CHECK(simple.hovered.fill == ColorRef::FromTheme(ThemeSlot::Dropdown));
    CHECK(simple.hovered.outlineWidth == 0.0f && simple.hovered.cornerRadii == Vec4(4, 4, 4, 4));
    CHECK(simple.pressed == simple.hovered);
    CHECK(simple.normalPadding == Vec4(2, 1, 2, 1));
    CHECK(simple.pressedPadding == Vec4(2, 2, 2, 0));
    CHECK(simple.normalForeground == ColorRef::FromTheme(ThemeSlot::Foreground));
    CHECK(simple.hoveredForeground == ColorRef::FromTheme(ThemeSlot::ForegroundHover));
}

TEST(UiCore_StyleBrushResolvesThemeColors) {
    const ThemeColors& theme = DefaultThemeColors();
    const Brush brush = DefaultButtonStyle().normal.Resolve(theme, {});
    CHECK(brush.mode == BrushMode::RoundedBox);
    CHECK(brush.tint == theme.Get(ThemeSlot::Secondary));
    CHECK(brush.outline.color == theme.Get(ThemeSlot::Input));
    CHECK(brush.outline.width == 1.0f);
    CHECK(brush.outline.cornerRadii == Vec4(4, 4, 4, 4));
    CHECK(brush.outline.rounding == BrushRounding::Fixed);
    CHECK(brush.IsValid());
    // 전경 상속 색은 문맥의 전경색을 따른다(UE FSlateColor::UseForeground).
    StyleBrush inherited = StyleBrush::Color(ThemeSlot::Panel);
    inherited.fill = ColorRef{ColorSource::Foreground};
    const StyleColorContext context{{0.25f, 0.5f, 0.75f, 1.0f}, {1, 1, 1, 1}};
    CHECK(inherited.Resolve(theme, context).tint == context.foreground);
    CHECK(StyleBrush::Color(ThemeSlot::Panel).Resolve(theme, {}).mode == BrushMode::Box);
}

TEST(UiCore_TextStylesMatchUe) {
    // StarshipCoreStyle.cpp 34–40행·1252–1258행: 보통 Regular 10, 작게 8, 크게 14, 주 버튼 Bold 10.
    SystemAllocator allocator;
    StyleSet styles(allocator);
    REQUIRE(AddCoreStyles(styles));
    CHECK(*styles.Find<TextStyle>("NormalText") == NormalTextStyle());
    CHECK((NormalTextStyle().font == FontStyle{Typeface::Regular, 10}));
    CHECK(NormalTextStyle().color == ColorRef{ColorSource::Foreground});
    const TextStyle* small = styles.Find<TextStyle>("SmallText");
    const TextStyle* large = styles.Find<TextStyle>("Text.Large");
    const TextStyle* primary = styles.Find<TextStyle>("PrimaryButtonText");
    REQUIRE(small && large && primary);
    CHECK((small->font == FontStyle{Typeface::Regular, 8}));
    CHECK((large->font == FontStyle{Typeface::Regular, 14}));
    CHECK((primary->font == FontStyle{Typeface::Bold, 10}));
}

TEST(UiCore_StyleSetFindsReplacesAndFallsBack) {
    SystemAllocator allocator;
    StyleSet styles(allocator);
    REQUIRE(AddCoreStyles(styles));
    REQUIRE(styles.Find<ButtonStyle>("Button") && styles.Find<ButtonStyle>("PrimaryButton") &&
            styles.Find<ButtonStyle>("SimpleButton"));
    CHECK(*styles.Find<ButtonStyle>("PrimaryButton") == PrimaryButtonStyle());
    // 이름은 대소문자를 가리고, 없는 이름은 nullptr다(부품의 내장 기본값은 부품이 쓴다).
    CHECK(styles.Find<ButtonStyle>("button") == nullptr);
    CHECK(!styles.Find<ButtonStyle>("Missing"));
    CHECK(!styles.Find<TextStyle>("Missing"));
    // 같은 이름은 값을 바꾸고, 빈 이름은 거부한다.
    ButtonStyle custom = DefaultButtonStyle();
    custom.normalPadding = {1, 2, 3, 4};
    REQUIRE(styles.Set<ButtonStyle>("Button", custom));
    CHECK(styles.Find<ButtonStyle>("Button")->normalPadding == Vec4(1, 2, 3, 4));
    CHECK(!styles.Set<ButtonStyle>({}, custom));
    CHECK(!styles.Set<TextStyle>({}, NormalTextStyle()));
}

TEST(UiCore_PaintListThemeDefaultsToUeDark) {
    SystemAllocator allocator;
    PaintList list(allocator);
    CHECK(&list.Theme() == &DefaultThemeColors());
    CHECK(list.Theme() == ThemeColors{});
    ThemeColors custom;
    list.SetTheme(&custom);
    CHECK(&list.Theme() == &custom);
    // Clear는 그리기 기록만 지운다.
    list.Clear();
    CHECK(&list.Theme() == &custom);
    list.SetTheme(nullptr);
    CHECK(&list.Theme() == &DefaultThemeColors());
}
