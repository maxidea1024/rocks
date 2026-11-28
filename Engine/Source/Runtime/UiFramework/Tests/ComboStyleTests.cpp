#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/PaintList.h>
#include <UiCore/StyleSet.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/ComboBox.h>
#include <UiFramework/Layout/AnchorCanvas.h>
#include <UiFramework/Layout/Overlay.h>
#include <UiFramework/Layout/VerticalStack.h>

// 콤보 스타일 시험이다(Docs/Design/UiToolkit 23.2절 — UE "ComboButton"·"ComboBox"·"ComboBox.Row"):
// 스타일 키와 값, 고정 크기 없는 원하는 크기, 펼침 목록 바탕·행 모양·강조, 처음 펼칠 때 선택 행
// 보이기, Frame StyleBrush 바탕, ScrollView의 미룬 스크롤 요청.

namespace rocks::ui {

namespace {

// 값을 정한 크기의 표시 요소다(글자 대신).
Result<SharedPtr<Element>> MakeSized(Allocator& allocator, Vec2 size) noexcept {
    auto frame = SharedPtr<Frame>::TryMake(allocator, allocator);
    if (!frame) {
        return Result<SharedPtr<Element>>::Failure(frame.Error());
    }

    frame.Value()->SetDesiredSize(size);
    return Result<SharedPtr<Element>>::Success(frame.Value());
}

constexpr Vec2 OptionSize{50, 14};

} // namespace

TEST(UiFramework_ComboStylesAreRegisteredWithUeValues) {
    SystemAllocator allocator;
    StyleSet styles(allocator);
    REQUIRE(AddCoreStyles(styles));
    const ComboBoxStyle* combo = styles.Find<ComboBoxStyle>("ComboBox");
    const ComboBoxStyle* simple = styles.Find<ComboBoxStyle>("SimpleComboBox");
    const ComboButtonStyle* button = styles.Find<ComboButtonStyle>("ComboButton");
    const ComboButtonStyle* simpleButton = styles.Find<ComboButtonStyle>("SimpleComboButton");
    const TableRowStyle* row = styles.Find<TableRowStyle>("ComboBox.Row");
    REQUIRE(combo && simple && button && simpleButton && row);
    CHECK(*combo == DefaultComboBoxStyle() && *row == DefaultComboBoxRowStyle());
    // UE 값: 버튼 여백 (8, 1, 8, 1)·눌림 (8, 2, 8, 0), 행 여백 (8, 3)·단순 (8, 2), 화살표
    // 16·여백 2.
    CHECK(button->button.normalPadding == ComboButtonMargin);
    CHECK(button->button.pressedPadding == PressedComboButtonMargin);
    CHECK(combo->menuRowPadding == ComboMenuRowPadding);
    CHECK(simple->menuRowPadding == SimpleComboMenuRowPadding);
    CHECK(button->downArrow.imageSize == icons::Icon16);
    CHECK(button->downArrowPadding == Vec4(2, 2, 2, 2));
    CHECK(simpleButton->button == SimpleButtonStyle());
    // 행: 올림 Primary, 글자 White, 보통 바탕 없음.
    CHECK(row->evenRowHovered == StyleBrush::Color(ThemeSlot::Primary));
    CHECK(row->evenRow.mode == BrushMode::None);
    CHECK(row->text == ColorRef::FromTheme(ThemeSlot::White));
}

TEST(UiFramework_ComboBoxSizesFromContentPaddingAndArrow) {
    SystemAllocator allocator;
    ComboBox combo(allocator);
    REQUIRE(combo.Construct(
        {.count = 1,
         .selected = 0,
         .makeOption = ComboBox::MakeOption::CreateLambda(
             [&allocator](usize, bool) noexcept { return MakeSized(allocator, OptionSize); })}));
    // 고정 크기가 없다: 내용 50 + 버튼 여백 8 + 8 + 화살표 칸 20, 높이는 화살표 칸(16 + 2 + 2) +
    // 버튼 여백 1 + 1(내용 14보다 높다).
    REQUIRE(combo.Prepass(1));
    const Vec4 padding = combo.GetStyle().normalPadding;
    CHECK(combo.GetDesiredSize().x == OptionSize.x + padding.x + padding.z + 20);
    CHECK(combo.GetDesiredSize().y == 20 + padding.y + padding.w);
    // 스타일을 바꾸면 버튼 모양·화살표 칸이 따라간다(단순 콤보 — 버튼 여백이 다르다).
    REQUIRE(combo.SetComboStyle(DefaultSimpleComboBoxStyle()));
    CHECK(combo.GetStyle() == SimpleButtonStyle());
    REQUIRE(combo.Prepass(1));
    const Vec4 simple = combo.GetStyle().normalPadding;
    CHECK(combo.GetDesiredSize().x == OptionSize.x + simple.x + simple.z + 20);
    // 잘못된 여백은 거부한다.
    ComboBoxStyle bad = DefaultComboBoxStyle();
    bad.menuRowPadding.x = -1;
    CHECK(combo.SetComboStyle(bad).Error() == ErrorCode::InvalidArgument);
}

TEST(UiFramework_ComboBoxMenuUsesBorderAndRowStyles) {
    SystemAllocator allocator;
    auto root = SharedPtr<Overlay>::TryMake(allocator, allocator);
    auto canvas = SharedPtr<AnchorCanvas>::TryMake(allocator, allocator);
    auto combo = SharedPtr<ComboBox>::TryMake(allocator, allocator);
    REQUIRE(root && canvas && combo);
    constexpr usize Count = 40;
    constexpr int32 Selected = 30;
    REQUIRE(combo.Value()->Construct(
        {.count = Count,
         .selected = Selected,
         .makeOption = ComboBox::MakeOption::CreateLambda(
             [&allocator](usize, bool) noexcept { return MakeSized(allocator, OptionSize); }),
         .maxListHeight = 100}));
    REQUIRE(root.Value()->TryAddChild(canvas.Value()));
    REQUIRE(
        canvas.Value()->TryAddSlot(combo.Value(), {.offset = {10, 10, 120, 24}, .alignment = {}}));
    Application app(allocator);
    app.SetRoot(root.Value());
    const Geometry area = Geometry::Root({400, 400}, {});
    REQUIRE(app.UpdatePaintCache(area, 1));
    REQUIRE(combo.Value()->Open());
    REQUIRE(app.UpdatePaintCache(area, 1));
    // 목록: SizeBox → 바탕 Frame(StyleBrush menuBorder) → ScrollView → 행 묶음.
    Element* menu = combo.Value()->MenuContent();
    REQUIRE(menu != nullptr && menu->ChildCount() == 1);
    auto* border = ElementCast<Frame>(menu->ChildAt(0));
    REQUIRE(border != nullptr && border->GetStyleBrush());
    CHECK(*border->GetStyleBrush() == DefaultComboBoxStyle().comboButton.menuBorder);
    auto* scroll = ElementCast<ScrollView>(border->ChildAt(0));
    REQUIRE(scroll != nullptr);
    Element* stack = scroll->ChildAt(0);
    REQUIRE(stack != nullptr && stack->ChildCount() == Count);
    // 행 버튼: 보통 바탕 없음·올림 Primary·글자 White·행 여백. 행 높이는 내용 + 여백(고정 24 없음).
    auto* row = ElementCast<Button>(stack->ChildAt(0));
    REQUIRE(row != nullptr);
    CHECK(row->GetStyle().hovered == DefaultComboBoxRowStyle().evenRowHovered);
    CHECK(row->GetStyle().normalPadding == ComboMenuRowPadding);
    CHECK(row->GetStyle().normalForeground == ColorRef::FromTheme(ThemeSlot::White));
    CHECK(row->GetGeometry().size.y ==
          OptionSize.y + ComboMenuRowPadding.y + ComboMenuRowPadding.w);
    CHECK(!row->SupportsKeyboardFocus());
    // 처음 펼칠 때 선택 행이 목록 안에 보인다(미룬 스크롤 — 같은 배치).
    const auto inside = [&](const Element& element) noexcept {
        const auto view = scroll->GetGeometry().WindowBounds().Value();
        const auto box = element.GetGeometry().WindowBounds().Value();
        return box.min.y >= view.min.y - 0.01f && box.max.y <= view.max.y + 0.01f;
    };
    CHECK(scroll->GetScrollOffset() > 0);
    CHECK(inside(*stack->ChildAt(Selected)));
    // 강조 행(키보드)은 올림 모양 — Primary로 그린다.
    CHECK(combo.Value()->HighlightedIndex() == Selected);
    auto* highlighted = ElementCast<Button>(stack->ChildAt(Selected));
    REQUIRE(highlighted != nullptr && highlighted->IsHighlighted());
    combo.Value()->Close();
    CHECK(combo.Value()->HighlightedIndex() == -1);
    app.Shutdown();
}

TEST(UiFramework_FrameStyleBrushFollowsTheTheme) {
    SystemAllocator allocator;
    Frame frame(allocator);
    REQUIRE(frame.SetStyleBrush(StyleBrush::Color(ThemeSlot::Primary)));
    REQUIRE(frame.Arrange(Geometry::Root({10, 10}, {})));
    PaintList list(allocator);
    REQUIRE(frame.OnPaint(list, 0));
    REQUIRE(list.Boxes().Size() == 1);
    CHECK(list.Boxes()[0].color == list.Theme().Get(ThemeSlot::Primary));
    // 풀린 brush를 주면 StyleBrush 바탕은 지워진다.
    REQUIRE(frame.SetBrush(Brush::RoundedBox({1, 0, 0, 1}, 0)));
    CHECK(!frame.GetStyleBrush());
}

TEST(UiFramework_ScrollViewRevealsARequestedChildInTheSameArrange) {
    SystemAllocator allocator;
    auto scroll = SharedPtr<ScrollView>::TryMake(allocator, allocator);
    auto stack = SharedPtr<VerticalStack>::TryMake(allocator, allocator);
    REQUIRE(scroll && stack);
    REQUIRE(scroll.Value()->TryAddChild(stack.Value()));
    SharedPtr<Element> target;
    for (usize i = 0; i < 20; ++i) {
        auto item = MakeSized(allocator, {40, 10});
        REQUIRE(item && stack.Value()->TryAddChild(item.Value()));
        target = i == 15 ? item.Value() : target;
    }

    // 아직 배치되지 않았어도 요청해 두면 첫 배치 안에서 맞춘다.
    scroll.Value()->RequestScrollIntoView(target, ScrollIntoViewAlignment::Nearest);
    REQUIRE(scroll.Value()->Prepass(1));
    REQUIRE(scroll.Value()->ArrangeIfNeeded(Geometry::Root({40, 50}, {})));
    // 15번 행(150–160)이 50 높이 창 끝에 맞는다: 오프셋 110.
    CHECK(scroll.Value()->GetScrollOffset() == 110);
    const auto box = target->GetGeometry().WindowBounds().Value();
    CHECK(box.min.y == 40 && box.max.y == 50);
    // 요청은 한 번만 쓴다 — 사용자 스크롤은 그대로 남는다.
    REQUIRE(scroll.Value()->SetScrollOffset(0));
    REQUIRE(scroll.Value()->ArrangeIfNeeded(Geometry::Root({40, 50}, {})));
    CHECK(scroll.Value()->GetScrollOffset() == 0);
}

} // namespace rocks::ui
