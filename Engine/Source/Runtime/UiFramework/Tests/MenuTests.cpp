#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Layout/AnchorCanvas.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/ComboBox.h>
#include <UiFramework/Layout/Overlay.h>

namespace rocks::ui {

TEST(UiFramework_ComboPopupUsesOuterOverlayAndSelectsByPointer) {
    SystemAllocator allocator;
    ElementBuildScope scope(allocator);
    auto root = SharedPtr<Overlay>::TryMake(allocator, allocator);
    auto nested = SharedPtr<Overlay>::TryMake(allocator, allocator);
    auto canvas = SharedPtr<AnchorCanvas>::TryMake(allocator, allocator);
    int32 notified = -1;
    auto combo = Build<ComboBox>({
        .count = 3,
        .makeOption = ComboBox::MakeOption::CreateLambda([](usize, bool) noexcept {
            auto frame = Build<Frame>({});
            return frame ? Result<SharedPtr<Element>>::Success(frame.Value().AsSharedPtr())
                         : Result<SharedPtr<Element>>::Failure(frame.Error());
        }),
        .onSelectionChanged = Delegate<void(int32, SelectionCause)>::CreateLambda(
            [&notified](int32 index, SelectionCause) noexcept { notified = index; }),
    });
    REQUIRE(root && nested && canvas && combo);
    REQUIRE(root.Value()->TryAddChild(nested.Value()));
    REQUIRE(nested.Value()->TryAddChild(canvas.Value()));
    auto anchor = combo.Value().AsSharedPtr();
    REQUIRE(canvas.Value()->TryAddSlot(anchor, {.offset = {10, 80, 60, 20}, .alignment = {}}));

    Application app(allocator);
    app.SetRoot(root.Value());
    const Geometry bounds = Geometry::Root({120, 110}, {});
    REQUIRE(app.UpdatePaintCache(bounds, 1));
    REQUIRE(anchor->Open());
    REQUIRE(app.UpdatePaintCache(bounds, 1));
    REQUIRE(root.Value()->ChildCount() == 2);
    CHECK(nested.Value()->ChildCount() == 1);
    auto menu = root.Value()->ChildAt(1)->ChildAt(0);
    REQUIRE(menu != nullptr);
    CHECK(menu->GetGeometry().Contains({20, 10}));

    REQUIRE(app.RoutePointer({PointerEventType::Down, {20, 38}, 0}));
    REQUIRE(app.RoutePointer({PointerEventType::Up, {20, 38}, 0}));
    CHECK(notified == 1);
    CHECK(anchor->SelectedIndex() == 1 && !anchor->IsOpen());
    REQUIRE(anchor->Open());
    REQUIRE(app.UpdatePaintCache(bounds, 1));
    REQUIRE(app.RoutePointer({PointerEventType::Down, {110, 105}, 0}));
    CHECK(!anchor->IsOpen() && root.Value()->ChildCount() == 1);
    CHECK(anchor->SelectedIndex() == 1);

    REQUIRE(anchor->Open());
    REQUIRE(root.Value()->RemoveChild(*nested.Value()));
    CHECK(!anchor->IsOpen() && root.Value()->ChildCount() == 0);
}

TEST(UiFramework_MenuPlacementAndKeyboardFocusReturn) {
    SystemAllocator allocator;
    ElementBuildScope scope(allocator);
    auto root = SharedPtr<Overlay>::TryMake(allocator, allocator);
    auto canvas = SharedPtr<AnchorCanvas>::TryMake(allocator, allocator);
    auto button = SharedPtr<Button>::TryMake(allocator, allocator);
    auto anchor = SharedPtr<MenuAnchor>::TryMake(allocator, allocator);
    REQUIRE(root && canvas && button && anchor);
    button.Value()->SetDesiredSize({40, 20});
    anchor.Value()->SetMenuFactory(MenuAnchor::MenuFactory::CreateLambda(
        [item = button.Value()]() noexcept { return Result<SharedPtr<Element>>::Success(item); }));
    anchor.Value()->SetPlacement(MenuPlacement::Right);
    REQUIRE(root.Value()->TryAddChild(canvas.Value()));
    REQUIRE(
        canvas.Value()->TryAddSlot(anchor.Value(), {.offset = {80, 30, 10, 10}, .alignment = {}}));
    Application app(allocator);
    app.SetRoot(root.Value());
    const auto area = Geometry::Root({100, 80}, {});
    REQUIRE(app.UpdatePaintCache(area, 1));
    REQUIRE(app.SetFocus(1, anchor.Value()));
    REQUIRE(anchor.Value()->OpenFocused(app, 1));
    REQUIRE(app.UpdatePaintCache(area, 1));
    CHECK(app.GetFocus(1).Get() == button.Value().Get());
    CHECK(anchor.Value()->MenuPosition().Value() == Vec2(40, 30));
    REQUIRE(app.RouteKey(1, input::InputEvent::KeyDown(input::Key::Escape)));
    CHECK(!anchor.Value()->IsOpen() && app.GetFocus(1).Get() == anchor.Value().Get());
    anchor.Value()->SetFitInWindow(false);
    REQUIRE(anchor.Value()->Open());
    REQUIRE(app.UpdatePaintCache(area, 1));
    CHECK(anchor.Value()->MenuPosition().Value() == Vec2(90, 30));
}

} // namespace rocks::ui
