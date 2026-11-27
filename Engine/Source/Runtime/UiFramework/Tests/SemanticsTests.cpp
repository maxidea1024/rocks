#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Layout/AnchorCanvas.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/Button.h>
#include <UiFramework/Layout/Frame.h>

namespace rocks::ui {

namespace {

double tipClock = 0;
double ReadTipClock() noexcept {
    return tipClock;
}
} // namespace

TEST(UiFramework_ToolTipDelayFadeAndAccessibleSummary) {
    SystemAllocator allocator;
    Application app(allocator);
    auto owner = SharedPtr<Button>::TryMake(allocator, allocator);
    auto label = SharedPtr<Frame>::TryMake(allocator, allocator);
    REQUIRE(owner && label);
    label.Value()->SetDesiredSize({20, 12});
    REQUIRE(label.Value()->SetAccessibility("hint", "hint", AccessibleBehavior::Custom,
                                            AccessibleBehavior::Custom, true));
    REQUIRE(owner.Value()->SetToolTipFactory(
        Element::ToolTipFactory::CreateLambda([element = label.Value()]() noexcept {
            return Result<SharedPtr<Element>>::Success(element);
        }),
        "Open inventory"));
    REQUIRE(owner.Value()->SetAccessibility("Inventory", {}, AccessibleBehavior::Custom,
                                            AccessibleBehavior::ToolTip, true));
    app.SetRoot(owner.Value());
    tipClock = 0;
    app.SetClock(ClockFunction::CreateStatic(&ReadTipClock));
    const Geometry area = Geometry::Root({120, 60}, {});
    REQUIRE(app.UpdatePaintCache(area, 1));
    REQUIRE(app.RoutePointer({PointerEventType::Move, {8, 4}}));
    tipClock = 0.14;
    app.BeginFrame();
    REQUIRE(app.UpdatePaintCache(area, 1));
    CHECK(!app.IsToolTipOpen(0));
    tipClock = 0.16;
    app.BeginFrame();
    REQUIRE(app.UpdatePaintCache(area, 1));
    REQUIRE(app.IsToolTipOpen(0));
    REQUIRE(label.Value()->Parent());
    CHECK(label.Value()->GetGeometry().layout.Apply({}) == Vec2(20, 20));
    CHECK(label.Value()->Parent()->GetRenderOpacity() == 0);
    tipClock = 0.22;
    app.BeginFrame();
    REQUIRE(app.UpdatePaintCache(area, 1));
    CHECK(label.Value()->Parent()->GetRenderOpacity() > 0.5f);
    auto tree = app.AccessibleTree();
    REQUIRE(tree);
    bool found = false;
    for (const auto& node : tree.Value()) {
        if (node.native.Lock().Get() == owner.Value().Get()) {
            found = true;
            CHECK(node.name.View() == "Inventory");
            CHECK(node.summary.View() == "Open inventory");
        }
    }

    CHECK(found);
    REQUIRE(app.RoutePointer({PointerEventType::Down, {8, 4}}));
    CHECK(!app.IsToolTipOpen(0));
    CHECK(!label.Value()->Parent());
    CHECK(!owner.Value()->Parent());
}

TEST(UiFramework_NavigationStopsWrapsAndResolvesExplicitWithinDomain) {
    SystemAllocator allocator;
    Application app(allocator);
    auto root = SharedPtr<AnchorCanvas>::TryMake(allocator, allocator);
    auto first = SharedPtr<Button>::TryMake(allocator, allocator);
    auto second = SharedPtr<Button>::TryMake(allocator, allocator);
    auto other = SharedPtr<Button>::TryMake(allocator, allocator);
    REQUIRE(root && first && second && other);
    const Guid domain{1, 1}, otherDomain{1, 2}, id{2, 2};
    first.Value()->SetNavigationIdentity(domain, {2, 1});
    other.Value()->SetNavigationIdentity(otherDomain, id);
    second.Value()->SetNavigationIdentity(domain, id);
    REQUIRE(
        root.Value()->TryAddSlot(first.Value(), {.offset = {0, 0, 20, 10}, .alignment = {0, 0}}));
    REQUIRE(
        root.Value()->TryAddSlot(other.Value(), {.offset = {23, 0, 20, 10}, .alignment = {0, 0}}));
    REQUIRE(
        root.Value()->TryAddSlot(second.Value(), {.offset = {50, 0, 20, 10}, .alignment = {0, 0}}));
    app.SetRoot(root.Value());
    REQUIRE(app.UpdatePaintCache(Geometry::Root({100, 40}, {}), 1));
    REQUIRE(app.SetFocus(0, first.Value()));
    REQUIRE(
        first.Value()->SetNavigationRule(FocusDirection::Right, {NavigationRule::Explicit, id}));
    REQUIRE(app.Navigate(0, FocusDirection::Right).Value());
    CHECK(app.GetFocus(0) == second.Value());
    REQUIRE(root.Value()->SetNavigationRule(FocusDirection::Next, {NavigationRule::Stop, {}}));
    CHECK(!app.MoveFocus(0, true, true).Value());
    REQUIRE(root.Value()->SetNavigationRule(FocusDirection::Next, {NavigationRule::Wrap, {}}));
    REQUIRE(app.MoveFocus(0, true, false).Value());
    CHECK(app.GetFocus(0) == first.Value());
    REQUIRE(first.Value()->SetNavigationRule(FocusDirection::Down, {NavigationRule::Custom, {}}));
    REQUIRE(first.Value()->SetNavigationHandler(
        NavigationHandler::CreateLambda([id](uint32, FocusDirection) noexcept { return id; })));
    REQUIRE(app.Navigate(0, FocusDirection::Down).Value());
    CHECK(app.GetFocus(0) == second.Value());
}
} // namespace rocks::ui
