#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Layout/AnchorCanvas.h>
#include <UiFramework/Controls/Button.h>
#include <UiFramework/Application/InputAdapter.h>

namespace rocks::ui {

TEST(UiFramework_ArrowNavigationUsesScreenPositionAndAlignedRow) {
    SystemAllocator allocator;
    auto root = SharedPtr<AnchorCanvas>::TryMake(allocator, allocator);
    auto left = SharedPtr<Button>::TryMake(allocator, allocator);
    auto below = SharedPtr<Button>::TryMake(allocator, allocator);
    auto right = SharedPtr<Button>::TryMake(allocator, allocator);
    REQUIRE(root && left && below && right);
    REQUIRE(
        root.Value()->TryAddSlot(left.Value(), {.offset = {10, 10, 40, 22}, .alignment = {0, 0}}));
    REQUIRE(
        root.Value()->TryAddSlot(below.Value(), {.offset = {32, 50, 40, 22}, .alignment = {0, 0}}));
    REQUIRE(root.Value()->TryAddSlot(right.Value(),
                                     {.offset = {140, 10, 40, 22}, .alignment = {0, 0}}));
    Application app(allocator);
    InputAdapter adapter(app);
    app.SetRoot(root.Value());
    REQUIRE(app.UpdatePaintCache(Geometry::Root({200, 80}, {}), 1));
    REQUIRE(app.SetFocus(0, left.Value()));
    REQUIRE(adapter.Route(input::InputEvent::KeyDown(input::Key::Right)).Value());
    CHECK(app.GetFocus(0).Get() == right.Value().Get());
    REQUIRE(adapter.Route(input::InputEvent::KeyDown(input::Key::Left)).Value());
    REQUIRE(adapter.Route(input::InputEvent::KeyDown(input::Key::Down)).Value());
    CHECK(app.GetFocus(0).Get() == below.Value().Get());
    CHECK(!adapter.Route(input::InputEvent::KeyDown(input::Key::Down)).Value());
}
} // namespace rocks::ui
