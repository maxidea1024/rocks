#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Layout/AnchorCanvas.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/Button.h>
#include <UiFramework/Popups/MenuAnchor.h>
#include <UiFramework/Layout/Overlay.h>
#include <UiCore/Styles/CoreStyles.h>

namespace rocks::ui {

namespace {

// 초점 외곽선 상자 수다: 채움이 투명하고 외곽선이 Primary 1 px인 둥근 상자(FocusOutlineStyle).
usize CountFocusOutlines(const CacheRoot& cache, const ThemeColors& theme, Vec2 size) noexcept {
    usize count = 0;
    for (const PaintBox& box : cache.List().Boxes()) {
        if (box.roundedBox && box.color.a == 0 && box.outlineWidth == 1 &&
            box.outlineColor == theme.Get(ThemeSlot::Primary) && box.geometry.size == size) {
            ++count;
        }
    }

    return count;
}

// 외곽선 질의에 정해진 답을 하는 초점 요소다.
class ForcedOutline final : public Frame {
    ROCKS_UI_ELEMENT(ForcedOutline, Frame)

public:
    ForcedOutline(Allocator& allocator, Optional<bool> answer) noexcept
        : Frame(allocator), answer_(answer) {
        SetDesiredSize({20, 10});
    }

    bool SupportsKeyboardFocus() const noexcept override {
        return true;
    }

    Optional<bool> OnQueryShowFocus(FocusCause) const noexcept override {
        return answer_;
    }

private:
    Optional<bool> answer_;
};

// 누르면 지정한 요소를 부모에서 떼어 내는 요소다(탭 머리를 눌러 페이지가 바뀌는 경우).
class DetachOnPress final : public Frame {
    ROCKS_UI_ELEMENT(DetachOnPress, Frame)

public:
    explicit DetachOnPress(Allocator& allocator) noexcept : Frame(allocator) {
        SetDesiredSize({20, 10});
    }

    SharedPtr<Element> victim;

    PointerReply OnPointerEvent(const PointerEvent& event) noexcept override {
        if (event.type == PointerEventType::Down && victim && victim->Parent()) {
            (void)victim->Parent()->RemoveChild(*victim);
        }

        return {.handled = true};
    }
};

struct FocusBench {
    SystemAllocator allocator;
    Application app{allocator};
    SharedPtr<AnchorCanvas> root;
    SharedPtr<Button> first;
    SharedPtr<Button> second;

    bool Build() noexcept {
        auto madeRoot = SharedPtr<AnchorCanvas>::TryMake(allocator, allocator);
        auto madeFirst = SharedPtr<Button>::TryMake(allocator, allocator);
        auto madeSecond = SharedPtr<Button>::TryMake(allocator, allocator);
        if (!madeRoot || !madeFirst || !madeSecond) {
            return false;
        }

        root = Move(madeRoot.Value());
        first = Move(madeFirst.Value());
        second = Move(madeSecond.Value());
        if (!root->TryAddSlot(first, {.offset = {0, 0, 20, 10}, .alignment = {0, 0}}) ||
            !root->TryAddSlot(second, {.offset = {30, 0, 20, 10}, .alignment = {0, 0}})) {
            return false;
        }

        app.SetRoot(root);
        return static_cast<bool>(app.UpdatePaintCache(Geometry::Root({100, 40}, {}), 1));
    }

    usize Outlines() noexcept {
        auto cache = app.UpdatePaintCache(Geometry::Root({100, 40}, {}), 1);
        return cache ? CountFocusOutlines(*cache.Value(), app.Theme(), {20, 10}) : usize{999};
    }
};

} // namespace

TEST(UiFramework_FocusOutlineShowsOnlyForNavigation) {
    FocusBench bench;
    REQUIRE(bench.Build());
    // Tab(이동)으로 옮긴 초점은 외곽선을 켠다. 그리기 cache도 그 요소만 다시 기록해 반영한다.
    REQUIRE(bench.app.MoveFocus(0, true, false, FocusCause::Navigation).Value());
    CHECK(bench.app.GetFocus(0) == bench.first);
    CHECK(bench.app.GetFocusCause(0) == FocusCause::Navigation);
    CHECK(bench.first->IsFocusOutlineVisible());
    CHECK(bench.Outlines() == 1);
    REQUIRE(bench.app.MoveFocus(0, true, false, FocusCause::Navigation).Value());
    CHECK(!bench.first->IsFocusOutlineVisible() && bench.second->IsFocusOutlineVisible());
    CHECK(bench.Outlines() == 1);
    // 포인터 누름이 옮긴 초점은 외곽선이 없다(마우스 사용자에게 외곽선을 보이지 않는다).
    REQUIRE(bench.app.RoutePointer({PointerEventType::Down, {5, 5}}));
    REQUIRE(bench.app.RoutePointer({PointerEventType::Up, {5, 5}}));
    CHECK(bench.app.GetFocus(0) == bench.first);
    CHECK(bench.app.GetFocusCause(0) == FocusCause::Pointer);
    CHECK(!bench.first->IsFocusOutlineVisible() && !bench.second->IsFocusOutlineVisible());
    CHECK(bench.Outlines() == 0);
    // 코드가 직접 정한 초점도 외곽선이 없다.
    REQUIRE(bench.app.SetFocus(0, bench.second));
    CHECK(bench.app.GetFocusCause(0) == FocusCause::Direct &&
          !bench.second->IsFocusOutlineVisible());
    // 방향 이동도 Navigation이다.
    REQUIRE(bench.app.Navigate(0, FocusDirection::Left, {}, FocusCause::Navigation).Value());
    CHECK(bench.app.GetFocus(0) == bench.first && bench.first->IsFocusOutlineVisible());
}

TEST(UiFramework_FocusOutlineFollowsCauseOnSameTarget) {
    FocusBench bench;
    REQUIRE(bench.Build());
    REQUIRE(bench.app.SetFocus(0, bench.first, FocusCause::Navigation));
    CHECK(bench.first->IsFocusOutlineVisible());
    // 키보드로 옮긴 초점을 마우스로 누르면 같은 대상이어도 외곽선이 꺼진다.
    REQUIRE(bench.app.RoutePointer({PointerEventType::Down, {5, 5}}));
    REQUIRE(bench.app.RoutePointer({PointerEventType::Up, {5, 5}}));
    CHECK(bench.app.GetFocus(0) == bench.first && !bench.first->IsFocusOutlineVisible());
    REQUIRE(bench.app.SetFocus(0, bench.first, FocusCause::Navigation));
    CHECK(bench.first->IsFocusOutlineVisible());
}

TEST(UiFramework_FocusOutlineClearsWithFocus) {
    FocusBench bench;
    REQUIRE(bench.Build());
    // 초점 해제.
    REQUIRE(bench.app.SetFocus(0, bench.first, FocusCause::Navigation));
    REQUIRE(bench.app.SetFocus(0, {}));
    CHECK(!bench.first->IsFocusOutlineVisible());
    CHECK(bench.Outlines() == 0);
    // 창 입력 끊김(ResetInput).
    REQUIRE(bench.app.SetFocus(0, bench.first, FocusCause::Navigation));
    bench.app.ResetInput();
    CHECK(!bench.first->IsFocusOutlineVisible());
    // 요소를 떼어 내면 다음 갱신이 초점과 외곽선을 모두 지운다.
    REQUIRE(bench.app.SetFocus(0, bench.second, FocusCause::Navigation));
    const SharedPtr<Button> detached = bench.second;
    CHECK(bench.root->RemoveChild(*detached));
    CHECK(bench.Outlines() == 0);
    CHECK(!bench.app.GetFocus(0) && !detached->IsFocusOutlineVisible());
}

TEST(UiFramework_FocusOutlineCountsUsers) {
    FocusBench bench;
    REQUIRE(bench.Build());
    // 두 사용자가 같은 요소에 외곽선을 켜면 둘 다 끌 때까지 남는다.
    REQUIRE(bench.app.SetFocus(0, bench.first, FocusCause::Navigation));
    REQUIRE(bench.app.SetFocus(1, bench.first, FocusCause::Navigation));
    REQUIRE(bench.app.SetFocus(0, bench.second, FocusCause::Pointer));
    CHECK(bench.first->IsFocusOutlineVisible());
    REQUIRE(bench.app.SetFocus(1, {}));
    CHECK(!bench.first->IsFocusOutlineVisible() && !bench.second->IsFocusOutlineVisible());
}

TEST(UiFramework_FocusOutlineNotShownForCodeMovedFocus) {
    FocusBench bench;
    REQUIRE(bench.Build());
    // 코드가 옮기는 초점(메뉴·화면의 첫 초점 — MoveFocusWithin 기본)은 Direct라 외곽선이 없다.
    REQUIRE(bench.app.MoveFocusWithin(0, bench.root, true, false).Value());
    CHECK(bench.app.GetFocusCause(0) == FocusCause::Direct);
    CHECK(!bench.first->IsFocusOutlineVisible());
    REQUIRE(bench.app.Navigate(0, FocusDirection::Right).Value());
    CHECK(!bench.second->IsFocusOutlineVisible());
}

TEST(UiFramework_FocusOutlineEmptyClickKeepsFocusWithoutOutline) {
    FocusBench bench;
    REQUIRE(bench.Build());
    REQUIRE(bench.app.SetFocus(0, bench.first, FocusCause::Navigation));
    // 초점 요소가 없는 곳을 누르면 초점은 그대로 두고 외곽선만 끈다.
    REQUIRE(bench.app.RoutePointer({PointerEventType::Down, {80, 30}}));
    REQUIRE(bench.app.RoutePointer({PointerEventType::Up, {80, 30}}));
    CHECK(bench.app.GetFocus(0) == bench.first);
    CHECK(bench.app.GetFocusCause(0) == FocusCause::Pointer);
    CHECK(!bench.first->IsFocusOutlineVisible() && bench.Outlines() == 0);
}

TEST(UiFramework_FocusOutlineClearsWhenTargetDisabledOrHidden) {
    FocusBench bench;
    REQUIRE(bench.Build());
    REQUIRE(bench.app.SetFocus(0, bench.first, FocusCause::Navigation));
    bench.first->SetEnabled(false);
    CHECK(bench.Outlines() == 0);
    CHECK(!bench.app.GetFocus(0) && !bench.first->IsFocusOutlineVisible());
    bench.first->SetEnabled(true);
    REQUIRE(bench.app.SetFocus(0, bench.second, FocusCause::Navigation));
    bench.second->SetVisibility(Visibility::Hidden);
    CHECK(bench.Outlines() == 0);
    CHECK(!bench.app.GetFocus(0) && !bench.second->IsFocusOutlineVisible());
}

TEST(UiFramework_FocusOutlineSkipsHitTestInvisibleInTabOrder) {
    FocusBench bench;
    REQUIRE(bench.Build());
    // 누를 수 없는 요소는 Tab 순서에서 빠진다(툴팁 층 등).
    bench.first->SetVisibility(Visibility::HitTestInvisible);
    REQUIRE(bench.app.MoveFocus(0, true, false, FocusCause::Navigation).Value());
    CHECK(bench.app.GetFocus(0) == bench.second);
    CHECK(!bench.app.MoveFocus(0, true, false, FocusCause::Navigation).Value());
}

TEST(UiFramework_FocusOutlineQueryOverridesCause) {
    SystemAllocator allocator;
    Application app(allocator);
    auto root = SharedPtr<AnchorCanvas>::TryMake(allocator, allocator);
    auto never = SharedPtr<ForcedOutline>::TryMake(allocator, allocator, Optional<bool>(false));
    auto always = SharedPtr<ForcedOutline>::TryMake(allocator, allocator, Optional<bool>(true));
    REQUIRE(root && never && always);
    REQUIRE(
        root.Value()->TryAddSlot(never.Value(), {.offset = {0, 0, 20, 10}, .alignment = {0, 0}}));
    REQUIRE(
        root.Value()->TryAddSlot(always.Value(), {.offset = {30, 0, 20, 10}, .alignment = {0, 0}}));
    app.SetRoot(root.Value());
    REQUIRE(app.UpdatePaintCache(Geometry::Root({100, 40}, {}), 1));
    // false를 답하면 키보드 이동에도 없고, true를 답하면 코드가 옮긴 초점에도 보인다.
    REQUIRE(app.SetFocus(0, never.Value(), FocusCause::Navigation));
    CHECK(!never.Value()->IsFocusOutlineVisible());
    REQUIRE(app.SetFocus(0, always.Value(), FocusCause::Direct));
    CHECK(always.Value()->IsFocusOutlineVisible());
}

TEST(UiFramework_FocusOutlineUsesWhiteOnAccentButton) {
    SystemAllocator allocator;
    auto plain = SharedPtr<Button>::TryMake(allocator, allocator);
    auto accent = SharedPtr<Button>::TryMake(allocator, allocator);
    REQUIRE(plain && accent);
    REQUIRE(accent.Value()->SetStyle(PrimaryButtonStyle()));
    // Primary 바탕에서는 Primary 외곽선이 묻혀 흰 외곽선을 쓴다.
    CHECK(plain.Value()->FocusOutlineBrush() == &FocusOutlineStyle());
    CHECK(accent.Value()->FocusOutlineBrush() == &FocusOutlineOnAccentStyle());
    CHECK(FocusOutlineOnAccentStyle().outlineColor == ColorRef::FromTheme(ThemeSlot::White));
}

TEST(UiFramework_FocusOutlineFollowsMenuOpenCause) {
    SystemAllocator allocator;
    ElementBuildScope scope(allocator);
    auto root = SharedPtr<Overlay>::TryMake(allocator, allocator);
    auto canvas = SharedPtr<AnchorCanvas>::TryMake(allocator, allocator);
    auto item = SharedPtr<Button>::TryMake(allocator, allocator);
    auto anchor = SharedPtr<MenuAnchor>::TryMake(allocator, allocator);
    REQUIRE(root && canvas && item && anchor);
    item.Value()->SetDesiredSize({40, 20});
    anchor.Value()->SetMenuFactory(MenuAnchor::MenuFactory::CreateLambda(
        [menu = item.Value()]() noexcept { return Result<SharedPtr<Element>>::Success(menu); }));
    REQUIRE(root.Value()->TryAddChild(canvas.Value()));
    REQUIRE(
        canvas.Value()->TryAddSlot(anchor.Value(), {.offset = {10, 10, 10, 10}, .alignment = {}}));
    Application app(allocator);
    app.SetRoot(root.Value());
    const auto area = Geometry::Root({100, 80}, {});
    REQUIRE(app.UpdatePaintCache(area, 1));
    // 키보드로 옮긴 초점에서 연 메뉴는 첫 항목에 외곽선을 잇고, 닫으면 그 원인으로 복원한다.
    REQUIRE(app.SetFocus(0, anchor.Value(), FocusCause::Navigation));
    REQUIRE(anchor.Value()->OpenFocused(app, 0));
    REQUIRE(app.UpdatePaintCache(area, 1));
    CHECK(app.GetFocus(0) == item.Value() && item.Value()->IsFocusOutlineVisible());
    REQUIRE(app.RouteKey(0, input::InputEvent::KeyDown(input::Key::Escape)));
    CHECK(app.GetFocus(0) == anchor.Value() && anchor.Value()->IsFocusOutlineVisible());
    // 마우스로 잡은 초점에서 연 메뉴는 외곽선이 없다.
    REQUIRE(app.SetFocus(0, anchor.Value(), FocusCause::Pointer));
    REQUIRE(anchor.Value()->OpenFocused(app, 0));
    REQUIRE(app.UpdatePaintCache(area, 1));
    CHECK(app.GetFocus(0) == item.Value() && !item.Value()->IsFocusOutlineVisible());
    anchor.Value()->Close();
}

TEST(UiFramework_FocusOutlinePressDetachingFocusDoesNotFail) {
    FocusBench bench;
    REQUIRE(bench.Build());
    auto tab = SharedPtr<DetachOnPress>::TryMake(bench.allocator, bench.allocator);
    REQUIRE(tab);
    tab.Value()->victim = bench.first;
    REQUIRE(bench.root->TryAddSlot(tab.Value(), {.offset = {0, 20, 20, 10}, .alignment = {0, 0}}));
    REQUIRE(bench.Outlines() == 0);
    REQUIRE(bench.app.SetFocus(0, bench.first, FocusCause::Navigation));
    // 누름 처리 중 외곽선이 켜진 초점 요소가 떨어져 나가도 포인터 처리는 성공한다(회귀: 갤러리 탭
    // 전환에서 InvalidArgument로 앱이 닫혔다).
    auto down = bench.app.RoutePointer({PointerEventType::Down, {5, 25}});
    REQUIRE(down);
    CHECK(down.Value());
    REQUIRE(bench.app.RoutePointer({PointerEventType::Up, {5, 25}}));
    CHECK(bench.Outlines() == 0);
    CHECK(!bench.app.GetFocus(0));
}

TEST(UiFramework_FocusOutlineEmptyClickNeverFailsForUnfocusableTarget) {
    FocusBench bench;
    REQUIRE(bench.Build());
    REQUIRE(bench.app.SetFocus(0, bench.first, FocusCause::Navigation));
    // 외곽선이 켜진 초점 요소가 초점을 못 받게 돼도 빈 곳 클릭은 실패하지 않고 외곽선만 끈다.
    bench.first->SetFocusable(false);
    auto down = bench.app.RoutePointer({PointerEventType::Down, {80, 30}});
    REQUIRE(down);
    REQUIRE(bench.app.RoutePointer({PointerEventType::Up, {80, 30}}));
    CHECK(!bench.first->IsFocusOutlineVisible());
    CHECK(bench.app.GetFocusCause(0) == FocusCause::Pointer);
}

TEST(UiFramework_FocusOutlineMenuRestoreUsesLastInput) {
    SystemAllocator allocator;
    ElementBuildScope scope(allocator);
    auto root = SharedPtr<Overlay>::TryMake(allocator, allocator);
    auto canvas = SharedPtr<AnchorCanvas>::TryMake(allocator, allocator);
    auto item = SharedPtr<Button>::TryMake(allocator, allocator);
    auto anchor = SharedPtr<MenuAnchor>::TryMake(allocator, allocator);
    REQUIRE(root && canvas && item && anchor);
    item.Value()->SetDesiredSize({40, 20});
    anchor.Value()->SetMenuFactory(MenuAnchor::MenuFactory::CreateLambda(
        [menu = item.Value()]() noexcept { return Result<SharedPtr<Element>>::Success(menu); }));
    REQUIRE(root.Value()->TryAddChild(canvas.Value()));
    REQUIRE(
        canvas.Value()->TryAddSlot(anchor.Value(), {.offset = {10, 10, 10, 10}, .alignment = {}}));
    Application app(allocator);
    app.SetRoot(root.Value());
    const auto area = Geometry::Root({100, 80}, {});
    REQUIRE(app.UpdatePaintCache(area, 1));
    // 마우스로 잡은 초점이라도 Enter(Navigation)로 열면 첫 항목에 외곽선이 있다.
    REQUIRE(app.SetFocus(0, anchor.Value(), FocusCause::Pointer));
    REQUIRE(anchor.Value()->OpenFocused(app, 0, FocusCause::Navigation));
    REQUIRE(app.UpdatePaintCache(area, 1));
    CHECK(item.Value()->IsFocusOutlineVisible());
    // 메뉴 안을 마우스로 누른 뒤 닫으면 복원 초점에 외곽선이 없다(마지막 입력이 마우스).
    auto itemBounds = item.Value()->GetGeometry().WindowBounds();
    REQUIRE(itemBounds);
    const Vec2 inside = (itemBounds.Value().min + itemBounds.Value().max) * 0.5f;
    REQUIRE(app.RoutePointer({PointerEventType::Down, inside}));
    REQUIRE(app.RoutePointer({PointerEventType::Up, inside}));
    anchor.Value()->Close();
    CHECK(app.GetFocus(0) == anchor.Value() && !anchor.Value()->IsFocusOutlineVisible());
}

} // namespace rocks::ui
