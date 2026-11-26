#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/Button.h>
#include <UiFramework/Scrolling/ScrollView.h>
#include <UiFramework/Layout/SizeBox.h>
#include <UiFramework/Layout/VerticalStack.h>

namespace rocks::ui {

TEST(UiFramework_TouchTapStaysWithButtonAndPanTransfersCaptureOnce) {
    SystemAllocator allocator;
    auto scroll = SharedPtr<ScrollView>::TryMake(allocator, allocator);
    auto button = SharedPtr<Button>::TryMake(allocator, allocator);
    REQUIRE(scroll && button);
    button.Value()->SetDesiredSize({120, 240});
    REQUIRE(scroll.Value()->TryAddChild(button.Value()));
    uint32 clicks = 0;
    button.Value()->SetOnClicked(
        Delegate<void()>::CreateLambda([&clicks]() noexcept { ++clicks; }));
    Application app(allocator);
    app.SetRoot(scroll.Value());
    REQUIRE(app.UpdatePaintCache(Geometry::Root({100, 60}, {}), 1));
    auto touch = [&app](PointerEventType type, float y) noexcept {
        return app.RoutePointer({type, {12, y}, 3, 1, PointerSource::Touch});
    };
    REQUIRE(touch(PointerEventType::Down, 35));
    CHECK(button.Value()->IsPressed());
    REQUIRE(touch(PointerEventType::Move, 32));
    CHECK(button.Value()->IsPressed() && !scroll.Value()->IsScrolling());
    REQUIRE(touch(PointerEventType::Up, 32));
    CHECK(clicks == 1 && scroll.Value()->GetScrollOffset() == 0);
    REQUIRE(touch(PointerEventType::Down, 35));
    REQUIRE(touch(PointerEventType::Move, 20));
    CHECK(!button.Value()->IsPressed() && scroll.Value()->IsScrolling());
    CHECK(scroll.Value()->GetScrollOffset() == 15);
    REQUIRE(touch(PointerEventType::Move, -5));
    CHECK(scroll.Value()->GetScrollOffset() == 40);
    REQUIRE(touch(PointerEventType::Up, -5));
    CHECK(clicks == 1 && !scroll.Value()->IsScrolling());
}

TEST(UiFramework_NestedTouchScrollChoosesScrollableAxis) {
    SystemAllocator allocator;
    auto outer = SharedPtr<ScrollView>::TryMake(allocator, allocator);
    auto stack = SharedPtr<VerticalStack>::TryMake(allocator, allocator);
    auto bounds = SharedPtr<SizeBox>::TryMake(allocator, allocator);
    auto inner = SharedPtr<ScrollView>::TryMake(allocator, allocator, StackDirection::Horizontal);
    auto button = SharedPtr<Button>::TryMake(allocator, allocator);
    auto tail = SharedPtr<Frame>::TryMake(allocator, allocator);
    REQUIRE(outer && stack && bounds && inner && button && tail);
    button.Value()->SetDesiredSize({240, 40});
    tail.Value()->SetDesiredSize({100, 100});
    REQUIRE(bounds.Value()->SetHeightOverride(40));
    REQUIRE(inner.Value()->TryAddChild(button.Value()));
    REQUIRE(bounds.Value()->TryAddChild(inner.Value()));
    REQUIRE(stack.Value()->TryAddChild(bounds.Value()));
    REQUIRE(stack.Value()->TryAddChild(tail.Value()));
    REQUIRE(outer.Value()->TryAddChild(stack.Value()));
    Application app(allocator);
    app.SetRoot(outer.Value());
    REQUIRE(app.UpdatePaintCache(Geometry::Root({100, 50}, {}), 1));
    REQUIRE(app.RoutePointer({PointerEventType::Down, {50, 22}, 0, 0, PointerSource::Touch}));
    REQUIRE(app.RoutePointer({PointerEventType::Move, {28, 21}, 0, 0, PointerSource::Touch}));
    CHECK(inner.Value()->GetScrollOffset() == 22 && outer.Value()->GetScrollOffset() == 0);
    REQUIRE(app.RoutePointer({PointerEventType::Up, {28, 21}, 0, 0, PointerSource::Touch}));
    REQUIRE(app.RoutePointer({PointerEventType::Down, {55, 30}, 1, 0, PointerSource::Touch}));
    REQUIRE(app.RoutePointer({PointerEventType::Move, {54, 15}, 1, 0, PointerSource::Touch}));
    CHECK(inner.Value()->GetScrollOffset() == 22 && outer.Value()->GetScrollOffset() == 15);
    REQUIRE(app.RoutePointer({PointerEventType::Cancel, {54, 15}, 1, 0, PointerSource::Touch}));
    CHECK(!outer.Value()->IsScrolling() && !button.Value()->IsPressed());
}
} // namespace rocks::ui
