#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiWidget/ButtonWidget.generated.h>
#include <UiWidget/ListViewWidget.generated.h>
#include <UiFramework/Button.h>

namespace rocks::ui {

TEST(UiWidget_ObjectListKeepsSelectedObjectWhenInsertingBeforeIt) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x554957, 33});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto model = objects.NewObject<ListViewWidget>(access, GeneratedType<ListViewWidget>::Id());
    auto first = objects.NewObject<ButtonWidget>(access, GeneratedType<ButtonWidget>::Id());
    auto second = objects.NewObject<ButtonWidget>(access, GeneratedType<ButtonWidget>::Id());
    REQUIRE(model && first && second);
    auto borrow = objects.ResolveForWrite<ListViewWidget>(access, model.Value().Handle());
    REQUIRE(borrow);
    auto* list = borrow.Value().Get().Value();
    Application app(allocator);
    ObjectHandle bound{};
    REQUIRE(list->UseObjectItems(
        objects, app, ListViewWidget::CreateEntry::CreateLambda([](Allocator& storage) noexcept {
            return SharedPtr<Element>::TryMake(storage, storage);
        }),
        ListViewWidget::BindEntry::CreateLambda([&bound](Element&, ObjectHandle item) noexcept {
            bound = item;
            return Result<void>::Success();
        })));
    REQUIRE(list->AddItem(objects, second.Value().Handle()));
    auto view = list->TakeElement(objects);
    REQUIRE(view);
    app.SetRoot(view.Value());
    const auto area = Geometry::Root({100, 24}, {});
    REQUIRE(app.UpdatePaintCache(area, 1));
    CHECK(bound == second.Value().Handle());
    REQUIRE(list->SetSelectedItem(second.Value().Handle()));
    auto selected = list->SelectedKey();
    REQUIRE(selected);
    REQUIRE(list->InsertItem(objects, 0, first.Value().Handle()));
    REQUIRE(app.UpdatePaintCache(area, 1));
    REQUIRE(list->SelectedKey());
    CHECK(list->FindItem(second.Value().Handle()).Value() == 1 &&
          *list->SelectedKey() == *selected);
    REQUIRE(list->ScrollObjectIntoView(second.Value().Handle()));
    REQUIRE(app.UpdatePaintCache(area, 1));
    CHECK(bound == second.Value().Handle() && list->IsItemVisible(*selected));
    REQUIRE(list->RemoveItem(objects, second.Value().Handle()));
    REQUIRE(app.UpdatePaintCache(area, 1));
    CHECK(!list->SelectedKey() && list->Items().Size() == 1);
    app.SetRoot({});
}

} // namespace rocks::ui
