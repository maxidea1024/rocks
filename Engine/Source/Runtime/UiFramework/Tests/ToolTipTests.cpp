#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/Styles/CoreStyles.h>
#include <UiFramework/Layout/AnchorCanvas.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/Button.h>
#include <UiFramework/Application/InputAdapter.h>
#include <UiFramework/Popups/ToolTip.h>

namespace rocks::ui {

namespace {

// 글자마다 폭 10, 공백 뒤 줄바꿈 허용인 레이블(줄바꿈 높이 시험용)이다.
Result<void> ShapeWords(TextLabel& label, StringView text, float lineHeight) noexcept {
    constexpr usize MaxClusters = 64;
    text::TextCluster clusters[MaxClusters];
    const usize count = text.ByteCount();
    if (count > MaxClusters) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize i = 0; i < count; ++i) {
        clusters[i] = {.byteEnd = i + 1, .advance = 10, .breakAfter = text.Data()[i] == ' '};
    }

    return label.SetShapedText(text, {clusters, count}, {}, lineHeight, false);
}

} // namespace

TEST(UiFramework_ToolTipTextUsesBrightStyle) {
    SystemAllocator allocator;
    auto label = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    auto tip = SharedPtr<ToolTip>::TryMake(allocator, allocator);
    REQUIRE(label && tip);
    REQUIRE(ShapeWords(*label.Value(), "tip", 12));
    REQUIRE(tip.Value()->Construct({.text = label.Value()}));
    // 글자 툴팁은 밝은 바탕·검은 글자·여백 11이다.
    CHECK(tip.Value()->GetStyle() == DefaultToolTipStyle());
    CHECK(label.Value()->GetColor() == ColorRef::Specified({0, 0, 0, 1}));
    Application app(allocator);
    app.SetRoot(tip.Value());
    auto cache = app.UpdatePaintCache(Geometry::Root({120, 40}, {}), 1);
    REQUIRE(cache);
    CHECK(label.Value()->GetGeometry().layout.tx == TextToolTipPadding);
    CHECK(label.Value()->GetGeometry().layout.ty == TextToolTipPadding);
    bool bright = false;
    for (const PaintBox& box : cache.Value()->List().Boxes()) {
        bright = bright || (box.roundedBox && box.color == BrightToolTipFill &&
                            box.outlineColor == BrightToolTipOutline);
    }

    CHECK(bright);
}

TEST(UiFramework_ToolTipContentUsesDarkStyle) {
    SystemAllocator allocator;
    auto content = SharedPtr<Frame>::TryMake(allocator, allocator);
    auto tip = SharedPtr<ToolTip>::TryMake(allocator, allocator);
    REQUIRE(content && tip);
    content.Value()->SetDesiredSize({30, 10});
    REQUIRE(tip.Value()->Construct({.content = content.Value()}));
    CHECK(tip.Value()->GetStyle() == ContentToolTipStyle());
    // 원하는 크기는 내용 + 여백 8씩이다.
    Application app(allocator);
    app.SetRoot(tip.Value());
    auto cache = app.UpdatePaintCache(Geometry::Root({120, 40}, {}), 1);
    REQUIRE(cache);
    CHECK(tip.Value()->GetDesiredSize() == Vec2(30 + 16, 10 + 16));
    bool dark = false;
    for (const PaintBox& box : cache.Value()->List().Boxes()) {
        dark = dark || (box.roundedBox && box.color == app.Theme().Get(ThemeSlot::Dropdown) &&
                        box.outlineColor == app.Theme().Get(ThemeSlot::DropdownOutline));
    }

    CHECK(dark);
}

TEST(UiFramework_ToolTipRejectsInvalidArguments) {
    SystemAllocator allocator;
    auto label = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    auto content = SharedPtr<Frame>::TryMake(allocator, allocator);
    auto tip = SharedPtr<ToolTip>::TryMake(allocator, allocator);
    REQUIRE(label && content && tip);
    // 글자와 내용을 함께 주면 안 된다.
    CHECK(tip.Value()->Construct({.text = label.Value(), .content = content.Value()}).Error() ==
          ErrorCode::InvalidArgument);
    REQUIRE(tip.Value()->Construct({.text = label.Value()}));
    ToolTipStyle bad = DefaultToolTipStyle();
    bad.padding.x = -1;
    CHECK(tip.Value()->SetStyle(bad).Error() == ErrorCode::InvalidArgument);
    bad = DefaultToolTipStyle();
    bad.wrapWidth = platform::PlatformMath::Infinity<float>();
    CHECK(tip.Value()->SetStyle(bad).Error() == ErrorCode::InvalidArgument);
    CHECK(tip.Value()->GetStyle() == DefaultToolTipStyle());
}

} // namespace rocks::ui

namespace rocks::ui {

namespace {

double keyboardTipClock = 0;
double ReadKeyboardTipClock() noexcept {
    return keyboardTipClock;
}

} // namespace

TEST(UiFramework_ToolTipOpensForKeyboardFocus) {
    SystemAllocator allocator;
    Application app(allocator);
    auto root = SharedPtr<AnchorCanvas>::TryMake(allocator, allocator);
    auto first = SharedPtr<Button>::TryMake(allocator, allocator);
    auto second = SharedPtr<Button>::TryMake(allocator, allocator);
    auto content = SharedPtr<Frame>::TryMake(allocator, allocator);
    REQUIRE(root && first && second && content);
    content.Value()->SetDesiredSize({30, 10});
    REQUIRE(first.Value()->SetToolTipFactory(Element::ToolTipFactory::CreateLambda(
        [element = content.Value()]() noexcept -> Result<SharedPtr<Element>> {
            return Result<SharedPtr<Element>>::Success(element);
        })));
    REQUIRE(
        root.Value()->TryAddSlot(first.Value(), {.offset = {0, 0, 20, 10}, .alignment = {0, 0}}));
    REQUIRE(
        root.Value()->TryAddSlot(second.Value(), {.offset = {40, 0, 20, 10}, .alignment = {0, 0}}));
    app.SetRoot(root.Value());
    keyboardTipClock = 0;
    app.SetToolTipClock(&ReadKeyboardTipClock);
    REQUIRE(app.SetToolTipTiming(0.5, 0));
    const auto frame = [&app]() noexcept {
        return static_cast<bool>(app.UpdatePaintCache(Geometry::Root({100, 60}, {}), 1));
    };
    REQUIRE(frame());
    // Tab으로 초점을 옮기면 지연 뒤에 툴팁이 소유자 아래 왼쪽에 열린다(UE는 hover만).
    REQUIRE(app.MoveFocus(0, true, false, FocusCause::Navigation).Value());
    REQUIRE(frame());
    CHECK(!app.IsToolTipOpen(0));
    keyboardTipClock = 0.6;
    REQUIRE(frame());
    REQUIRE(frame());
    CHECK(app.IsToolTipOpen(0));
    auto bounds = content.Value()->GetGeometry().WindowBounds();
    REQUIRE(bounds);
    // 소유자(0..10) 아래 2 px 간격이다.
    CHECK(bounds.Value().min.x == 0 && bounds.Value().min.y == 12);
    // 툴팁 소유자가 아닌 곳의 포인터 이동으로는 닫지 않는다.
    REQUIRE(app.RoutePointer({PointerEventType::Move, {80, 50}}));
    CHECK(app.IsToolTipOpen(0));
    // 초점이 떠나면 닫는다.
    REQUIRE(app.MoveFocus(0, true, false, FocusCause::Navigation).Value());
    CHECK(!app.IsToolTipOpen(0));
    // 눌러서 옮긴 초점은 키보드 툴팁을 열지 않는다.
    REQUIRE(app.SetFocus(0, first.Value(), FocusCause::Pointer));
    keyboardTipClock = 2;
    REQUIRE(frame());
    CHECK(!app.IsToolTipOpen(0));
    app.SetToolTipClock(nullptr);
}

namespace {

// 툴팁 시험 무대: 소유자 버튼 하나와 툴팁 내용, 주입한 시계.
double benchClock = 0;
double ReadBenchClock() noexcept {
    return benchClock;
}

struct TipBench {
    SystemAllocator allocator;
    Application app{allocator};
    SharedPtr<AnchorCanvas> root;
    SharedPtr<Button> owner;
    SharedPtr<Button> other;
    SharedPtr<Frame> content;
    Vec2 window{100, 60};

    bool Build(Vec4 ownerOffset) noexcept {
        auto madeRoot = SharedPtr<AnchorCanvas>::TryMake(allocator, allocator);
        auto madeOwner = SharedPtr<Button>::TryMake(allocator, allocator);
        auto madeOther = SharedPtr<Button>::TryMake(allocator, allocator);
        auto madeContent = SharedPtr<Frame>::TryMake(allocator, allocator);
        if (!madeRoot || !madeOwner || !madeOther || !madeContent) {
            return false;
        }

        root = Move(madeRoot.Value());
        owner = Move(madeOwner.Value());
        other = Move(madeOther.Value());
        content = Move(madeContent.Value());
        content->SetDesiredSize({30, 10});
        if (!owner->SetToolTipFactory(Element::ToolTipFactory::CreateLambda(
                [element = content]() noexcept -> Result<SharedPtr<Element>> {
                    return Result<SharedPtr<Element>>::Success(element);
                })) ||
            !root->TryAddSlot(owner, {.offset = ownerOffset, .alignment = {0, 0}}) ||
            !root->TryAddSlot(other, {.offset = {70, 0, 20, 10}, .alignment = {0, 0}})) {
            return false;
        }

        app.SetRoot(root);
        benchClock = 0;
        app.SetToolTipClock(&ReadBenchClock);
        return app.SetToolTipTiming(0.5, 0) && Step();
    }

    bool Step() noexcept {
        return static_cast<bool>(app.UpdatePaintCache(Geometry::Root(window, {}), 1));
    }

    // 시계를 지연 뒤로 옮겨 툴팁을 열고 배치까지 마친다.
    bool Elapse() noexcept {
        benchClock += 1;
        return Step() && Step();
    }

    Box2<float> ContentBounds() const noexcept {
        auto bounds = content->GetGeometry().WindowBounds();
        return bounds ? bounds.Value() : Box2<float>{};
    }

    ~TipBench() noexcept {
        app.SetToolTipClock(nullptr);
    }
};

} // namespace

TEST(UiFramework_ToolTipKeyboardClosesOnKeysButNotModifiers) {
    TipBench bench;
    REQUIRE(bench.Build({0, 0, 20, 10}));
    REQUIRE(bench.app.SetFocus(0, bench.owner, FocusCause::Navigation));
    REQUIRE(bench.Elapse());
    REQUIRE(bench.app.IsToolTipOpen(0));
    // 보조 키는 닫지 않고, 다른 키는 닫는다.
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::KeyDown(input::Key::LeftShift)));
    CHECK(bench.app.IsToolTipOpen(0));
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::KeyUp(input::Key::LeftShift)));
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::KeyDown(input::Key::A)));
    CHECK(!bench.app.IsToolTipOpen(0));
    // Ctrl+F1(ShowFocusToolTip)은 지연 없이 다시 연다.
    CHECK(bench.app.ShowFocusToolTip(0));
    REQUIRE(bench.Step());
    REQUIRE(bench.Step());
    CHECK(bench.app.IsToolTipOpen(0));
    // 게임패드 버튼 누름도 닫는다.
    (void)bench.app.RouteGamepadButtons(0, 1u << static_cast<uint32>(input::GamepadButton::South));
    CHECK(!bench.app.IsToolTipOpen(0));
    // 툴팁이 없는 요소에는 다시 열기가 없다.
    REQUIRE(bench.app.SetFocus(0, bench.other, FocusCause::Navigation));
    CHECK(!bench.app.ShowFocusToolTip(0));
}

TEST(UiFramework_ToolTipKeyboardNotOpenedForDirectFocus) {
    TipBench bench;
    REQUIRE(bench.Build({0, 0, 20, 10}));
    REQUIRE(bench.app.SetFocus(0, bench.owner, FocusCause::Direct));
    REQUIRE(bench.Elapse());
    CHECK(!bench.app.IsToolTipOpen(0));
    // 같은 대상에 원인만 Navigation으로 바뀌어도 예약한다.
    REQUIRE(bench.app.SetFocus(0, bench.owner, FocusCause::Navigation));
    REQUIRE(bench.Elapse());
    CHECK(bench.app.IsToolTipOpen(0));
}

TEST(UiFramework_ToolTipFlipsAboveNearBottomEdge) {
    TipBench bench;
    REQUIRE(bench.Build({0, 48, 20, 10}));
    // 키보드 툴팁: 소유자(48..58) 아래 공간이 모자라 위로(48 - 2 - 10 = 36) 뒤집는다.
    REQUIRE(bench.app.SetFocus(0, bench.owner, FocusCause::Navigation));
    REQUIRE(bench.Elapse());
    REQUIRE(bench.app.IsToolTipOpen(0));
    CHECK(bench.ContentBounds().min.y == 36);
    bench.app.CloseToolTip(0);
    REQUIRE(bench.app.SetFocus(0, {}));
    // hover 툴팁: 커서(5, 55) 아래(71)가 모자라 커서 위(55 - 10 = 45)로 뒤집는다.
    REQUIRE(bench.app.RoutePointer({PointerEventType::Move, {5, 55}}));
    REQUIRE(bench.Elapse());
    REQUIRE(bench.app.IsToolTipOpen(0));
    CHECK(bench.ContentBounds().min.y == 45 && bench.ContentBounds().min.x == 17);
}

TEST(UiFramework_ToolTipWrapsToWindowWidthInSameFrame) {
    SystemAllocator allocator;
    auto label = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    auto tip = SharedPtr<ToolTip>::TryMake(allocator, allocator);
    REQUIRE(label && tip);
    // "aaaa bbbb cccc"(글자 폭 10)는 한 줄 140이다. 폭 72(여백 11씩 → 안쪽 50)이면 단어마다 한
    // 줄, 세 줄이다.
    REQUIRE(ShapeWords(*label.Value(), "aaaa bbbb cccc", 12));
    REQUIRE(tip.Value()->Construct({.text = label.Value()}));
    const Vec2 measured = tip.Value()->MeasureForWidth(72);
    CHECK(label.Value()->Layout().Lines().Size() == 3);
    CHECK(measured.y == label.Value()->GetDesiredSize().y + 2 * TextToolTipPadding ||
          measured.y == label.Value()->ComputeDesiredSize(1).y + 2 * TextToolTipPadding);
    CHECK(measured.y > 2 * label.Value()->LineHeight() + 2 * TextToolTipPadding);
}

TEST(UiFramework_ToolTipPartsApiAndForeground) {
    SystemAllocator allocator;
    auto title = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    auto keys = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    auto description = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    auto tip = SharedPtr<ToolTip>::TryMake(allocator, allocator);
    auto bright = SharedPtr<ToolTip>::TryMake(allocator, allocator);
    auto content = SharedPtr<Frame>::TryMake(allocator, allocator);
    REQUIRE(title && keys && description && tip && bright && content);
    REQUIRE(ShapeWords(*title.Value(), "Save", 12));
    REQUIRE(ShapeWords(*keys.Value(), "Ctrl+S", 12));
    REQUIRE(ShapeWords(*description.Value(), "Saves the level", 12));
    // 제목·단축키·설명으로 짠 툴팁은 스타일 색을 레이블에 적용한다.
    REQUIRE(tip.Value()->Construct(
        {.title = title.Value(), .shortcut = keys.Value(), .description = description.Value()}));
    const ToolTipStyle& style = ContentToolTipStyle();
    CHECK(title.Value()->GetColor() == style.title.color);
    CHECK(keys.Value()->GetColor() == style.shortcut.color);
    CHECK(description.Value()->GetColor() == style.description.color);
    Application app(allocator);
    app.SetRoot(tip.Value());
    REQUIRE(app.UpdatePaintCache(Geometry::Root({300, 80}, {}), 1));
    // 설명은 제목 줄 아래 lineGap만큼 떨어진다.
    auto titleBounds = title.Value()->GetGeometry().WindowBounds();
    auto descriptionBounds = description.Value()->GetGeometry().WindowBounds();
    REQUIRE(titleBounds && descriptionBounds);
    CHECK(descriptionBounds.Value().min.y >= titleBounds.Value().max.y + style.lineGap);
    // 셋 중 둘 이상의 방식을 섞으면 안 된다.
    auto mixed = SharedPtr<ToolTip>::TryMake(allocator, allocator);
    REQUIRE(mixed);
    CHECK(mixed.Value()->Construct({.text = keys.Value(), .title = title.Value()}).Error() ==
          ErrorCode::InvalidArgument);
    // 밝은 스타일 내용 툴팁의 자식은 검은 전경색을 물려받는다(흰 글자가 밝은 바탕에 묻히지 않게).
    REQUIRE(
        bright.Value()->Construct({.content = content.Value(), .style = &DefaultToolTipStyle()}));
    CHECK(bright.Value()->GetForegroundColor() == DefaultToolTipStyle().text.color);
}

TEST(UiFramework_ToolTipKeyboardFindsAncestorOwner) {
    SystemAllocator allocator;
    Application app(allocator);
    auto root = SharedPtr<AnchorCanvas>::TryMake(allocator, allocator);
    auto box = SharedPtr<Frame>::TryMake(allocator, allocator);
    auto field = SharedPtr<Button>::TryMake(allocator, allocator);
    auto content = SharedPtr<Frame>::TryMake(allocator, allocator);
    REQUIRE(root && box && field && content);
    content.Value()->SetDesiredSize({30, 10});
    // 툴팁은 상자에, 초점은 그 안의 칸에 있다(hover처럼 조상에서 소유자를 찾는다).
    REQUIRE(box.Value()->SetToolTipFactory(Element::ToolTipFactory::CreateLambda(
        [element = content.Value()]() noexcept -> Result<SharedPtr<Element>> {
            return Result<SharedPtr<Element>>::Success(element);
        })));
    REQUIRE(box.Value()->TryAddChild(field.Value()));
    REQUIRE(root.Value()->TryAddSlot(box.Value(), {.offset = {0, 0, 40, 20}, .alignment = {0, 0}}));
    app.SetRoot(root.Value());
    REQUIRE(app.UpdatePaintCache(Geometry::Root({100, 60}, {}), 1));
    InputAdapter adapter(app);
    // Ctrl+F1은 InputAdapter 경로로 지연 없이 연다.
    REQUIRE(app.SetFocus(0, field.Value(), FocusCause::Pointer));
    REQUIRE(adapter.Route(input::InputEvent::KeyDown(input::Key::LeftControl)));
    REQUIRE(adapter.Route(input::InputEvent::KeyDown(input::Key::F1)));
    REQUIRE(adapter.Route(input::InputEvent::KeyUp(input::Key::LeftControl)));
    REQUIRE(app.UpdatePaintCache(Geometry::Root({100, 60}, {}), 1));
    REQUIRE(app.UpdatePaintCache(Geometry::Root({100, 60}, {}), 1));
    CHECK(app.IsToolTipOpen(0));
}

TEST(UiFramework_ToolTipHeaderWrapsInNarrowWindow) {
    SystemAllocator allocator;
    auto title = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    auto keys = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    auto tip = SharedPtr<ToolTip>::TryMake(allocator, allocator);
    REQUIRE(title && keys && tip);
    REQUIRE(ShapeWords(*title.Value(), "Save Current Level", 12));
    REQUIRE(ShapeWords(*keys.Value(), "Ctrl+S", 12));
    REQUIRE(tip.Value()->Construct({.title = title.Value(), .shortcut = keys.Value()}));
    Application app(allocator);
    app.SetRoot(tip.Value());
    REQUIRE(app.UpdatePaintCache(Geometry::Root({400, 80}, {}), 1));
    const Vec2 wide = tip.Value()->GetDesiredSize();
    // 좁은 폭에서는 제목이 줄을 나눠 높아지고, 단축키 표지는 한 줄 그대로다.
    const Vec2 narrow = tip.Value()->MeasureForWidth(wide.x - 60);
    CHECK(narrow.y > wide.y);
    CHECK(keys.Value()->Layout().Lines().Size() == 1);
}

TEST(UiFramework_ToolTipHeaderArrangesInsideNarrowWindow) {
    SystemAllocator allocator;
    auto title = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    auto keys = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    auto tip = SharedPtr<ToolTip>::TryMake(allocator, allocator);
    REQUIRE(title && keys && tip);
    REQUIRE(ShapeWords(*title.Value(), "Save Current Level", 12));
    REQUIRE(ShapeWords(*keys.Value(), "Ctrl+S", 12));
    REQUIRE(tip.Value()->Construct({.title = title.Value(), .shortcut = keys.Value()}));
    Application app(allocator);
    app.SetRoot(tip.Value());
    REQUIRE(app.UpdatePaintCache(Geometry::Root({400, 80}, {}), 1));
    const float narrow = tip.Value()->GetDesiredSize().x - 60;
    // 좁은 칸에 실제로 놓아도 단축키 표지는 툴팁 안에 남고 제목은 줄을 나눈다.
    REQUIRE(app.UpdatePaintCache(Geometry::Root({narrow, 80}, {}), 1));
    REQUIRE(app.UpdatePaintCache(Geometry::Root({narrow, 80}, {}), 1));
    auto tipBounds = tip.Value()->GetGeometry().WindowBounds();
    auto keyBounds = keys.Value()->GetGeometry().WindowBounds();
    REQUIRE(tipBounds && keyBounds);
    CHECK(keyBounds.Value().max.x <= tipBounds.Value().max.x);
    CHECK(title.Value()->Layout().Lines().Size() > 1);
}

} // namespace rocks::ui
