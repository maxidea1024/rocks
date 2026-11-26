#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Controls/Button.h>
#include <UiFramework/Lists/ListView.h>
#include <UiFramework/Lists/TileView.h>
#include <UiFramework/Lists/TreeView.h>

namespace rocks::ui {

namespace {

class RowSource final : public TableRowSource {
public:
    uint32 RootCount() const noexcept override {
        return roots;
    }

    uint64 RootKey(uint32 index) const noexcept override {
        return index + 1;
    }

    uint32 ChildCount(uint64 key) const noexcept override {
        return key == 1 ? 2u : 0u;
    }

    uint64 ChildKey(uint64, uint32 index) const noexcept override {
        return cycle ? 1u : index + 100u;
    }

    Result<SharedPtr<Element>> CreateRow(Allocator& allocator) noexcept override {
        auto row = SharedPtr<Button>::TryMake(allocator, allocator);
        if (!row) {
            return Result<SharedPtr<Element>>::Failure(row.Error());
        }

        ++created;
        return Result<SharedPtr<Element>>::Success(Move(row.Value()));
    }

    Result<void> BindRow(Element&, TableItem) noexcept override {
        ++bound;
        return Result<void>::Success();
    }

    void SetRowSelected(Element&, TableItem item, bool selected) noexcept override {
        if (selected) {
            lastSelection = item.key;
        }
    }

    uint64 lastSelection = 0;
    uint32 roots = 1000000;
    uint32 created = 0;
    uint32 bound = 0;
    bool cycle = false;
};

} // namespace

TEST(UiFramework_ListRecyclesVisibleRowsAndReleasesFocus) {
    SystemAllocator allocator;
    RowSource source;
    Application app(allocator);
    auto view = SharedPtr<ListView>::TryMake(allocator, allocator, app, source);
    REQUIRE(view);
    auto& list = *view.Value();
    REQUIRE(list.SetRowHeight(20));
    app.SetRoot(view.Value());
    auto viewport = Geometry::Root({100, 60}, {});
    REQUIRE(app.UpdatePaintCache(viewport, 1));
    CHECK(list.ChildCount() == 3 && source.created == 3);
    REQUIRE(app.SetFocus(0, list.ChildRefAt(0)));
    const auto middle = list.ChildRefAt(1);
    REQUIRE(list.SetScrollOffset(20));
    REQUIRE(app.UpdatePaintCache(viewport, 1));
    CHECK(!app.GetFocus(0));
    CHECK(list.ChildAt(0) == middle.Get());
    CHECK(list.ItemAtRow(0).key == 2 && source.created == 3 && source.bound == 4);
    REQUIRE(list.SetScrollOffset(list.MaxScrollOffset()));
    REQUIRE(app.UpdatePaintCache(viewport, 1));
    CHECK(list.ItemAtRow(2).key == source.roots && source.created == 3);
    REQUIRE(app.UpdatePaintCache(viewport, 1));
    CHECK(source.bound == 7);
    app.SetRoot({});
}

TEST(UiFramework_ListNavigationRealizesOnlyRequestedRowsAndKeepsSelectionByKey) {
    SystemAllocator allocator;
    RowSource source;
    Application app(allocator);
    auto made = SharedPtr<ListView>::TryMake(allocator, allocator, app, source);
    REQUIRE(made);
    auto& list = *made.Value();
    app.SetRoot(made.Value());
    REQUIRE(list.SetRowHeight(10));
    const auto area = Geometry::Root({100, 30}, {});
    REQUIRE(app.UpdatePaintCache(area, 1));
    REQUIRE(list.NavigateToIndex(500, 0));
    REQUIRE(app.UpdatePaintCache(area, 1));
    CHECK(list.ItemAtRow(2).key == 501);
    CHECK(app.GetFocus(0).Get() == list.ChildAt(2));
    // 목록 안 이동은 늘 Navigation이다(외곽선이 보인다 — UE NavigateToWidget).
    CHECK(app.GetFocusCause(0) == FocusCause::Navigation);
    CHECK(list.SelectedKey() && *list.SelectedKey() == 501 && source.lastSelection == 501);
    CHECK(source.created == 3);
    list.SetSelectOnNavigation(false);
    REQUIRE(list.NavigateToIndex(2, 0));
    REQUIRE(app.UpdatePaintCache(area, 1));
    CHECK(app.GetFocus(0).Get() == list.ChildAt(0));
    CHECK(list.SelectedKey() && *list.SelectedKey() == 501);
    list.SetPointerScrolling(false);
    CHECK(!list.OnPointerEvent({.type = PointerEventType::Wheel, .position = {}, .wheel = -1})
               .handled);
    source.roots = 100;
    list.Refresh();
    REQUIRE(app.UpdatePaintCache(area, 1));
    CHECK(!list.SelectedKey());
    app.SetRoot({});
}

TEST(UiFramework_MultiSelectionSurvivesRecyclingAndReportsAlignedScroll) {
    SystemAllocator allocator;
    RowSource source;
    source.roots = 100;
    Application app(allocator);
    auto list = SharedPtr<ListView>::TryMake(allocator, allocator, app, source);
    REQUIRE(list);
    auto& view = *list.Value();
    app.SetRoot(list.Value());
    view.SetSelectionMode(ListSelectionMode::Multi);
    REQUIRE(view.SetRowHeight(10));
    const auto area = Geometry::Root({80, 30}, {});
    REQUIRE(app.UpdatePaintCache(area, 1));
    uint32 changes = 0;
    uint64 clicked = 0;
    view.SetOnItemClicked(
        Delegate<void(uint64)>::CreateLambda([&clicked](uint64 key) noexcept { clicked = key; }));
    view.SetOnSelectionChanged(
        Delegate<void()>::CreateLambda([&changes]() noexcept { ++changes; }));
    REQUIRE(app.RoutePointer({PointerEventType::Down, {2, 2}, 0}));
    REQUIRE(app.RoutePointer({PointerEventType::Up, {2, 2}, 0}));
    REQUIRE(app.RouteKey(0, input::InputEvent::KeyDown(input::Key::LeftShift)));
    REQUIRE(app.RoutePointer({PointerEventType::Down, {2, 22}, 0}));
    REQUIRE(app.RoutePointer({PointerEventType::Up, {2, 22}, 0}));
    REQUIRE(app.RouteKey(0, input::InputEvent::KeyUp(input::Key::LeftShift)));
    CHECK(view.SelectedKeys().Size() == 3 && view.IsSelected(2) && changes == 2 && clicked == 3);
    REQUIRE(view.SetItemSelection(2, false));
    view.SetScrollIntoViewAlignment(ScrollIntoViewAlignment::Center);
    uint64 reported = 0;
    view.SetOnItemScrolledIntoView(
        Delegate<void(uint64)>::CreateLambda([&reported](uint64 key) noexcept { reported = key; }));
    REQUIRE(view.ScrollIndexIntoView(50));
    REQUIRE(app.UpdatePaintCache(area, 1));
    CHECK(reported == 51 && view.ItemAtRow(1).key == 51 && source.created == 3);
    CHECK(!view.IsItemVisible(1) && view.IsSelected(1) && view.IsSelected(3));
    REQUIRE(view.ScrollIndexIntoView(0));
    view.CancelScrollIntoView();
    REQUIRE(app.UpdatePaintCache(area, 1));
    CHECK(view.IsItemVisible(51) && reported == 51);
    source.roots = 1;
    view.Refresh();
    REQUIRE(app.UpdatePaintCache(area, 1));
    CHECK(view.SelectedKeys().Size() == 1 && view.IsSelected(1));
    app.SetRoot({});
}

TEST(UiFramework_TouchListSelectsOnTapAndLeavesSelectionAloneWhenPanning) {
    SystemAllocator allocator;
    RowSource source;
    source.roots = 80;
    Application app(allocator);
    auto widget = SharedPtr<ListView>::TryMake(allocator, allocator, app, source);
    REQUIRE(widget);
    auto& list = *widget.Value();
    REQUIRE(list.SetRowHeight(30));
    app.SetRoot(widget.Value());
    REQUIRE(app.UpdatePaintCache(Geometry::Root({90, 60}, {}), 1));
    REQUIRE(app.RoutePointer({PointerEventType::Down, {10, 45}, 1, 0, PointerSource::Touch}));
    CHECK(!list.SelectedKey());
    REQUIRE(app.RoutePointer({PointerEventType::Move, {10, 20}, 1, 0, PointerSource::Touch}));
    REQUIRE(app.RoutePointer({PointerEventType::Up, {10, 15}, 1, 0, PointerSource::Touch}));
    CHECK(!list.SelectedKey() && list.ScrollOffset() == 30);
    REQUIRE(app.UpdatePaintCache(Geometry::Root({90, 60}, {}), 1));
    REQUIRE(app.RoutePointer({PointerEventType::Down, {10, 45}, 2, 0, PointerSource::Touch}));
    REQUIRE(app.RoutePointer({PointerEventType::Up, {10, 45}, 2, 0, PointerSource::Touch}));
    REQUIRE(list.SelectedKey());
    CHECK(*list.SelectedKey() == 3);
    app.SetRoot({});
}

TEST(UiFramework_TilesReflowAndTreeFlattensOnlyExpandedNodes) {
    SystemAllocator allocator;
    RowSource source;
    Application app(allocator);
    auto tiles = SharedPtr<TileView>::TryMake(allocator, allocator, app, source);
    REQUIRE(tiles);
    REQUIRE(tiles.Value()->SetTileWidth(50));
    REQUIRE(tiles.Value()->SetRowHeight(20));
    app.SetRoot(tiles.Value());
    REQUIRE(app.UpdatePaintCache(Geometry::Root({150, 40}, {}), 1));
    CHECK(tiles.Value()->ChildCount() == 6);
    REQUIRE(app.UpdatePaintCache(Geometry::Root({100, 40}, {}), 1));
    CHECK(tiles.Value()->ChildCount() == 4);
    source.roots = 2;
    auto tree = SharedPtr<TreeView>::TryMake(allocator, allocator, app, source);
    REQUIRE(tree);
    app.SetRoot(tree.Value());
    auto bounds = Geometry::Root({80, 120}, {});
    REQUIRE(app.UpdatePaintCache(bounds, 1));
    REQUIRE(tree.Value()->ChildCount() == 2);
    REQUIRE(tree.Value()->SetExpanded(1, true));
    REQUIRE(app.UpdatePaintCache(bounds, 1));
    REQUIRE(tree.Value()->ChildCount() == 4);
    CHECK(tree.Value()->ItemAtRow(1).key == 100 && tree.Value()->ItemAtRow(2).depth == 1);
    tree.Value()->CollapseAll();
    REQUIRE(app.UpdatePaintCache(bounds, 1));
    CHECK(tree.Value()->ChildCount() == 2);
    REQUIRE(tree.Value()->ExpandAll());
    REQUIRE(app.UpdatePaintCache(bounds, 1));
    CHECK(tree.Value()->ChildCount() == 4 && tree.Value()->IsExpanded(1));
    source.cycle = true;
    tree.Value()->Refresh();
    CHECK(!app.UpdatePaintCache(bounds, 1));
    CHECK(tree.Value()->ChildCount() == 4);
    REQUIRE(tree.Value()->SetExpanded(1, false));
    REQUIRE(app.UpdatePaintCache(bounds, 1));
    CHECK(tree.Value()->ChildCount() == 2);
    app.SetRoot({});
}
} // namespace rocks::ui
