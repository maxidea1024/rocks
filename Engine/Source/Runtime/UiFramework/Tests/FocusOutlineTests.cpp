#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/AnchorCanvas.h>
#include <UiFramework/Application.h>
#include <UiFramework/Button.h>

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
    REQUIRE(bench.app.MoveFocus(0, true).Value());
    CHECK(bench.app.GetFocus(0) == bench.first);
    CHECK(bench.app.GetFocusCause(0) == FocusCause::Navigation);
    CHECK(bench.first->IsFocusOutlineVisible());
    CHECK(bench.Outlines() == 1);
    REQUIRE(bench.app.MoveFocus(0, true).Value());
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
    REQUIRE(bench.app.Navigate(0, FocusDirection::Left).Value());
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

} // namespace rocks::ui
