#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Layout/Frame.h>
#include <UiFramework/Scrolling/ScrollView.h>

namespace rocks::ui {

TEST(UiFramework_HorizontalScrollUsesContentCoordinatesAndTracksCapturedDrag) {
    SystemAllocator allocator;
    auto stack = SharedPtr<StackPanel>::TryMake(allocator, allocator, StackDirection::Horizontal);
    auto scroll = SharedPtr<ScrollView>::TryMake(allocator, allocator, StackDirection::Horizontal);
    REQUIRE(stack && scroll);
    for (int i = 0; i < 4; ++i) {
        auto item = SharedPtr<Frame>::TryMake(allocator, allocator);
        REQUIRE(item);
        item.Value()->SetDesiredSize({40, 20});
        REQUIRE(stack.Value()->TryAddChild(item.Value()));
    }

    auto& view = *scroll.Value();
    REQUIRE(view.TryAddChild(stack.Value()));
    view.SetRightDragScrolling(true);
    view.ScrollToEnd();
    Application app(allocator);
    app.SetRoot(scroll.Value());
    auto geometry = Geometry::Root({60, 20}, {});
    REQUIRE(app.UpdatePaintCache(geometry, 1));
    CHECK(view.GetScrollOffset() == 100);
    const auto& first = *stack.Value()->ChildAt(0);
    REQUIRE(view.ScrollIntoView(first, ScrollIntoViewAlignment::Start));
    CHECK(view.GetScrollOffset() == 0);
    REQUIRE(app.UpdatePaintCache(geometry, 1));
    int changed = 0;
    view.SetOnUserScrolled(
        Delegate<void(float)>::CreateLambda([&changed](float) noexcept { ++changed; }));
    PointerEvent event{PointerEventType::Down, {45, 6}};
    event.button = input::MouseButton::Right;
    REQUIRE(app.RoutePointer(event));
    CHECK(view.IsScrolling() && app.HasMouseCapture(view, 0, 0));
    event.type = PointerEventType::Move;
    event.position = {15, 6};
    REQUIRE(app.RoutePointer(event));
    CHECK(changed == 1 && view.GetScrollOffset() == 30);
    event.type = PointerEventType::Up;
    REQUIRE(app.RoutePointer(event));
    CHECK(!view.IsScrolling() && !app.HasMouseCapture(view, 0, 0));
    CHECK(view.GetViewFraction() == 0.375f && view.GetViewOffsetFraction() == 0.1875f);
}
} // namespace rocks::ui
