#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/Button.h>

using namespace rocks;
using namespace rocks::ui;

TEST(UiFramework_ButtonCaptureDeliversReleaseOutside) {
    SystemAllocator allocator;
    auto buttonResult = SharedPtr<Button>::TryMake(allocator, allocator);
    REQUIRE(buttonResult);
    auto button = buttonResult.Value();
    button->SetGeometry({{40, 40}, {}, {}});
    int clicks = 0;
    button->SetOnClicked(Delegate<void()>::CreateLambda([&clicks]() noexcept { ++clicks; }));

    Application app(allocator);
    app.SetRoot(button);
    auto rightClick = app.RoutePointer(
        {PointerEventType::Down, {10, 10}, 1, 0, PointerSource::Mouse, input::MouseButton::Right});
    REQUIRE(rightClick);
    CHECK(!rightClick.Value() && !button->IsPressed());

    REQUIRE(app.RoutePointer({PointerEventType::Down, {10, 10}, 1}));
    CHECK(button->IsPressed());
    REQUIRE(app.RoutePointer({PointerEventType::Up, {100, 100}, 1}));
    CHECK(!button->IsPressed() && clicks == 0);

    REQUIRE(app.RoutePointer({PointerEventType::Down, {10, 10}, 2}));
    REQUIRE(app.RoutePointer({PointerEventType::Up, {10, 10}, 2}));
    CHECK(!button->IsPressed() && clicks == 1);

    REQUIRE(app.RoutePointer({PointerEventType::Down, {10, 10}, 3}));
    app.SetRoot({});
    CHECK(!button->IsPressed());
}

TEST(UiFramework_CaptureSeparatesUsersWithTheSamePointerId) {
    SystemAllocator allocator;
    auto rootResult = SharedPtr<Element>::TryMake(allocator, allocator);
    auto firstResult = SharedPtr<Button>::TryMake(allocator, allocator);
    auto secondResult = SharedPtr<Button>::TryMake(allocator, allocator);
    REQUIRE(rootResult && firstResult && secondResult);
    auto root = rootResult.Value();
    auto first = firstResult.Value();
    auto second = secondResult.Value();
    root->SetGeometry(Geometry::Root({100, 50}, {}));
    first->SetGeometry(Geometry::Root({40, 40}, {}));
    second->SetGeometry(Geometry::Root({40, 40}, {1, 0, 0, 1, 50, 0}));
    REQUIRE(root->TryAddChild(first));
    REQUIRE(root->TryAddChild(second));

    Application app(allocator);
    app.SetRoot(root);
    REQUIRE(app.RoutePointer({PointerEventType::Down, {10, 10}, 0, 1}));
    REQUIRE(app.RoutePointer({PointerEventType::Down, {60, 10}, 0, 2}));
    CHECK(first->IsPressed() && second->IsPressed());
    REQUIRE(app.RoutePointer({PointerEventType::Up, {10, 10}, 0, 1}));
    CHECK(!first->IsPressed() && second->IsPressed());
    REQUIRE(app.RoutePointer({PointerEventType::Up, {60, 10}, 0, 2}));
    CHECK(!second->IsPressed());
}
