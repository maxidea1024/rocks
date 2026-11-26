#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/Button.h>

// 버튼 스타일 적용 시험이다(Docs/Design/UiToolkit 9.1절 UW3c — UE SButton). 상태별 brush·여백·
// 전경색, 색 곱, 전경색 상속과 캐시 재기록을 단언한다.

namespace rocks::ui {

namespace {

// 물려받은 전경색으로 상자 하나를 그린다(레이블 대역).
class ForegroundProbe final : public Element {
public:
    explicit ForegroundProbe(Allocator& allocator) noexcept : Element(allocator) {
    }

    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override {
        ++paints;
        return list.TryAddBox({GetGeometry(), list.ForegroundColor(), layer});
    }

    mutable uint32 paints = 0;
};

LinearColor Theme(ThemeSlot slot) noexcept {
    return DefaultThemeColors().Get(slot);
}

} // namespace

TEST(UiFramework_ButtonPaintsStyleStates) {
    SystemAllocator allocator;
    Button button(allocator);
    REQUIRE(button.Arrange(Geometry::Root({80, 24}, {})));
    PaintList list(allocator);
    const auto paint = [&]() noexcept {
        list.Clear();
        return button.OnPaint(list, 0);
    };
    // 보통: RB(Secondary, 4, Input, 1).
    REQUIRE(paint());
    REQUIRE(list.Boxes().Size() == 1);
    CHECK(list.Boxes()[0].roundedBox && list.Boxes()[0].color == Theme(ThemeSlot::Secondary));
    CHECK(list.Boxes()[0].outlineColor == Theme(ThemeSlot::Input));
    CHECK(list.Boxes()[0].outlineWidth == 1.0f);
    CHECK(list.Boxes()[0].cornerRadius == Vec4(4, 4, 4, 4));
    // 눌림: RB(Header) — 키보드로 눌러도 같다.
    REQUIRE(button.OnKeyEvent(0, input::InputEvent::KeyDown(input::Key::Enter)));
    REQUIRE(paint());
    CHECK(list.Boxes()[0].color == Theme(ThemeSlot::Header));
    REQUIRE(button.OnKeyEvent(0, input::InputEvent::KeyUp(input::Key::Enter)));
    // 비활성: RB(Dropdown, 4, Recessed, 1).
    button.SetEnabled(false);
    REQUIRE(paint());
    CHECK(list.Boxes()[0].color == Theme(ThemeSlot::Dropdown));
    CHECK(list.Boxes()[0].outlineColor == Theme(ThemeSlot::Recessed));
    button.SetEnabled(true);
    // ButtonColorAndOpacity는 brush 색에 곱한다.
    button.SetColor({0.5f, 1, 1, 1});
    REQUIRE(paint());
    CHECK(list.Boxes()[0].color == Theme(ThemeSlot::Secondary) * LinearColor(0.5f, 1, 1, 1));
    // 단순 버튼은 보통 상태를 그리지 않는다.
    REQUIRE(button.SetStyle(SimpleButtonStyle()));
    REQUIRE(paint());
    CHECK(list.Boxes().Size() == 0);
}

TEST(UiFramework_ButtonPaddingFollowsPressedState) {
    SystemAllocator allocator;
    auto button = SharedPtr<Button>::TryMake(allocator, allocator);
    auto content = SharedPtr<ForegroundProbe>::TryMake(allocator, allocator);
    REQUIRE(button && content);
    REQUIRE(button.Value()->TryAddChild(content.Value()));
    REQUIRE(button.Value()->SetContentPadding({1, 1, 1, 1}));
    const Geometry root = Geometry::Root({80, 24}, {});
    REQUIRE(button.Value()->Arrange(root));
    // 보통: ButtonPadding (12, 1.5, 12, 1.5) + 내용 여백 1.
    CHECK(content.Value()->GetGeometry().size == Vec2(80 - 26, 24 - 5));
    CHECK(content.Value()->GetGeometry().WindowBounds().Value().min == Vec2(13, 2.5f));
    // 눌림: PressedButtonPadding (12, 2.5, 12, 0.5) — 내용이 1만큼 내려간다.
    REQUIRE(button.Value()->OnKeyEvent(0, input::InputEvent::KeyDown(input::Key::Space)));
    REQUIRE(button.Value()->Arrange(root));
    CHECK(content.Value()->GetGeometry().size == Vec2(80 - 26, 24 - 5));
    CHECK(content.Value()->GetGeometry().WindowBounds().Value().min == Vec2(13, 3.5f));
    // 잘못된 여백·스타일은 거부하고 값을 바꾸지 않는다.
    CHECK(!button.Value()->SetContentPadding({-1, 0, 0, 0}));
    ButtonStyle bad = DefaultButtonStyle();
    bad.pressedPadding = {0, -1, 0, 0};
    CHECK(!button.Value()->SetStyle(bad));
    CHECK(button.Value()->GetStyle() == DefaultButtonStyle());
}

// Construct가 기본 스타일(생성자와 같은 값)을 받아도 스타일 여백을 넣는다. Frame::Construct가
// 여백을 0으로 덮은 뒤 SetStyle이 같은 값이라 건너뛰어, 기본 스타일 버튼이 여백 없이 그려지던
// 회귀다.
TEST(UiFramework_ButtonConstructKeepsDefaultStylePadding) {
    SystemAllocator allocator;
    auto button = SharedPtr<Button>::TryMake(allocator, allocator);
    auto content = SharedPtr<ForegroundProbe>::TryMake(allocator, allocator);
    REQUIRE(button && content);
    REQUIRE(button.Value()->Construct({.content = content.Value(), .style = DefaultButtonStyle()}));
    REQUIRE(button.Value()->Arrange(Geometry::Root({80, 24}, {})));
    CHECK(content.Value()->GetGeometry().WindowBounds().Value().min == Vec2(12, 1.5f));
    CHECK(content.Value()->GetGeometry().size == Vec2(80 - 24, 24 - 3));
}

TEST(UiFramework_ButtonForegroundFollowsStateAndOverride) {
    SystemAllocator allocator;
    Button button(allocator);
    // 보통·올림·눌림은 ForegroundHover, 비활성은 Foreground다(UE "Button").
    CHECK(button.GetForegroundColor() == ColorRef::FromTheme(ThemeSlot::ForegroundHover));
    button.SetEnabled(false);
    CHECK(button.GetForegroundColor() == ColorRef::FromTheme(ThemeSlot::Foreground));
    button.SetEnabled(true);
    // 단순 버튼은 보통 Foreground, 올림 ForegroundHover다.
    REQUIRE(button.SetStyle(SimpleButtonStyle()));
    CHECK(button.GetForegroundColor() == ColorRef::FromTheme(ThemeSlot::Foreground));
    button.OnMouseEnter(0);
    CHECK(button.GetForegroundColor() == ColorRef::FromTheme(ThemeSlot::ForegroundHover));
    // 직접 정한 전경색은 스타일보다 앞선다(UE ForegroundColor가 UseStyle이 아닐 때).
    REQUIRE(button.SetForegroundColor(ColorRef::FromTheme(ThemeSlot::AccentRed)));
    CHECK(button.GetForegroundColor() == ColorRef::FromTheme(ThemeSlot::AccentRed));
    CHECK(!button.SetForegroundColor(ColorRef{static_cast<ColorSource>(99)}));
}

TEST(UiFramework_ButtonContentInheritsForegroundThroughCache) {
    SystemAllocator allocator;
    auto root = SharedPtr<Frame>::TryMake(allocator, allocator);
    auto button = SharedPtr<Button>::TryMake(allocator, allocator);
    auto label = SharedPtr<ForegroundProbe>::TryMake(allocator, allocator);
    REQUIRE(root && button && label);
    REQUIRE(button.Value()->SetStyle(SimpleButtonStyle()));
    REQUIRE(button.Value()->TryAddChild(label.Value()));
    REQUIRE(root.Value()->TryAddChild(button.Value()));
    Application application(allocator);
    application.SetRoot(root.Value());
    const Geometry viewport = Geometry::Root({80, 24}, {});
    auto frame = application.UpdatePaintCache(viewport, 1);
    REQUIRE(frame);
    const CacheRoot& cache = *frame.Value();
    const auto labelColor = [&]() noexcept {
        const Span<const PaintBox> boxes = cache.List().Boxes();
        return boxes[boxes.Size() - 1].color;
    };
    // 단순 버튼 보통 상태: 레이블은 Foreground다.
    CHECK(labelColor() == Theme(ThemeSlot::Foreground));
    const uint32 painted = label.Value()->paints;
    // 올림: 버튼과, 상속 색이 바뀐 레이블만 다시 기록한다.
    button.Value()->OnMouseEnter(0);
    REQUIRE(application.UpdatePaintCache(viewport, 1));
    CHECK(labelColor() == Theme(ThemeSlot::ForegroundHover));
    CHECK(label.Value()->paints == painted + 1);
    // 바뀐 것이 없으면 다시 기록하지 않는다.
    REQUIRE(application.UpdatePaintCache(viewport, 1));
    CHECK(cache.LastUpdate().paintCalls == 0);
    // 테마를 바꾸면 캐시 전체를 새 색으로 다시 만든다.
    ThemeColors theme;
    REQUIRE(theme.Load(allocator, R"({"colors":{"ForegroundHover":"#FF0000"}})"));
    application.SetTheme(theme);
    REQUIRE(application.UpdatePaintCache(viewport, 1));
    CHECK(cache.LastUpdate().rebuilt);
    CHECK(labelColor() == theme.Get(ThemeSlot::ForegroundHover));
    CHECK(labelColor() != Theme(ThemeSlot::ForegroundHover));
}

} // namespace rocks::ui
