#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiWidget/CheckBoxWidget.generated.h>
#include <UiWidget/EditableTextWidget.generated.h>
#include <UiWidget/WidgetFieldBindings.h>
#include <UiWidget/WidgetTree.generated.h>
#include <UiWidget/Widget.generated.h>

namespace rocks::ui {

TEST(UiWidget_FieldBindingUpdatesLiveElementAfterNotificationAndDetaches) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    REQUIRE(RegisterWidgetTypes(registration));
    ObjectRegistry objects(allocator, types, {0x554957, 43});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto tree = objects.NewObject<WidgetTree>(access, GeneratedType<WidgetTree>::Id());
    REQUIRE(tree);
    auto treeBorrow = objects.ResolveForWrite<WidgetTree>(access, tree.Value().Handle());
    REQUIRE(treeBorrow);
    auto* root = treeBorrow.Value().Get().Value();
    auto source = root->ConstructWidget(objects, GeneratedType<EditableTextWidget>::Id(), "model");
    auto target = root->ConstructWidget(objects, GeneratedType<CheckBoxWidget>::Id(), "display");
    REQUIRE(source);
    REQUIRE(target);

    const ObjectHandle model = source.Value().Handle();
    const ObjectHandle widget = target.Value().Handle();
    auto borrowed = objects.ResolveForWrite<CheckBoxWidget>(access, widget);
    REQUIRE(borrowed);
    auto* check = borrowed.Value().Get().Value();
    auto live = check->TakeElement(objects);
    REQUIRE(live);

    WidgetFieldBindings bindings(objects);
    auto subscription = bindings.Bind(model, "readOnly", widget, WidgetSetter(&Widget::SetEnabled));
    REQUIRE(subscription);
    CHECK(!check->IsEnabled());
    CHECK(!live.Value()->IsEnabled());
    PropertyPathBuffer path;
    REQUIRE(ParsePropertyPath(objects, access, model, "readOnly", path));
    FieldValue value(allocator);
    value.kind = ValueKind::Bool;
    value.boolean = true;
    REQUIRE(bindings.Write(model, path.View(), value));
    CHECK(live.Value()->IsEnabled());
    auto once =
        bindings.ApplyOnce(model, "readOnly", widget, WidgetSetter(&CheckBoxWidget::SetChecked));
    REQUIRE(once);
    CHECK(check->IsChecked());
    bindings.Unbind(subscription.Value());
    value.boolean = false;
    REQUIRE(bindings.Write(model, path.View(), value));
    CHECK(live.Value()->IsEnabled());
    CHECK(check->IsChecked());
    auto mismatch =
        bindings.ApplyOnce(model, "text", widget, WidgetSetter(&CheckBoxWidget::SetChecked));
    REQUIRE(!mismatch);
    CHECK(mismatch.Error() == ErrorCode::TypeMismatch);

    auto callback = WidgetMethod(objects, widget, &CheckBoxWidget::SetChecked);
    callback.Execute(false);
    CHECK(!check->IsChecked());
}

} // namespace rocks::ui
