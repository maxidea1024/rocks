#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Scrolling/ScrollBar.h>
#include <UiWidget/CheckBoxWidget.generated.h>
#include <UiWidget/ScrollBarWidget.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

TEST(UiWidget_ScrollBarPersistsInputAndIgnoresReleasedControls) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    REQUIRE(RegisterWidgetTypes(registration));
    ObjectRegistry objects(allocator, types, {0x554957, 43});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto treeRef = objects.NewObject<WidgetTree>(access, GeneratedType<WidgetTree>::Id());
    REQUIRE(treeRef);
    auto tree = objects.ResolveForWrite<WidgetTree>(access, treeRef.Value().Handle());
    REQUIRE(tree);
    auto item = tree.Value().Get().Value()->ConstructWidget(
        objects, GeneratedType<ScrollBarWidget>::Id(), "bar");
    REQUIRE(item);
    auto resolved = objects.ResolveForWrite<ScrollBarWidget>(access, item.Value().Handle());
    REQUIRE(resolved);
    auto& model = *resolved.Value().Get().Value();
    REQUIRE(model.SetState(0.25f, 0.25f));
    model.SetHorizontal(true);
    auto first = model.TakeElement(objects);
    REQUIRE(first);
    REQUIRE(first.Value()->Arrange(Geometry::Root({100, 12}, {})));
    REQUIRE(first.Value()->OnPointerEvent({PointerEventType::Down, {30, 5}}).capture);
    REQUIRE(first.Value()->OnPointerEvent({PointerEventType::Move, {100, 5}}).handled);
    CHECK(model.Offset() == 0.75f);

    model.ReleaseElements();
    auto rebuilt = model.TakeElement(objects);
    REQUIRE(rebuilt);
    CHECK(static_cast<ScrollBar&>(*rebuilt.Value()).Offset() == 0.75f);
    REQUIRE(first.Value()->OnPointerEvent({PointerEventType::Move, {15, 5}}).handled);
    CHECK(model.Offset() == 0.75f);
}

TEST(UiWidget_CheckBoxHandlesMixedStateAndPreservesBooleanAPI) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    REQUIRE(RegisterWidgetTypes(registration));
    ObjectRegistry objects(allocator, types, {0x554957, 44});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto treeRef = objects.NewObject<WidgetTree>(access, GeneratedType<WidgetTree>::Id());
    REQUIRE(treeRef);
    auto tree = objects.ResolveForWrite<WidgetTree>(access, treeRef.Value().Handle());
    REQUIRE(tree);
    auto item = tree.Value().Get().Value()->ConstructWidget(
        objects, GeneratedType<CheckBoxWidget>::Id(), "mixed");
    REQUIRE(item);
    auto resolved = objects.ResolveForWrite<CheckBoxWidget>(access, item.Value().Handle());
    REQUIRE(resolved);
    auto& model = *resolved.Value().Get().Value();
    REQUIRE(model.SetState(CheckState::Indeterminate));
    CHECK(!model.IsChecked());
    auto native = model.TakeElement(objects);
    REQUIRE(native);
    CHECK(static_cast<CheckBox&>(*native.Value()).GetState() == CheckState::Indeterminate);
    CHECK(native.Value()->OnKeyEvent(0, input::InputEvent::KeyDown(input::Key::Space)));
    CHECK(native.Value()->OnKeyEvent(0, input::InputEvent::KeyUp(input::Key::Space)));
    CHECK(model.GetState() == CheckState::Checked && model.IsChecked());
    model.SetChecked(false);
    CHECK(model.GetState() == CheckState::Unchecked);
}

} // namespace rocks::ui
