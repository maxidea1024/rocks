#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Overlay.h>
#include <UiWidget/ComboBoxStringWidget.generated.h>
#include <UiWidget/ComboBoxWidget.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

TEST(UiWidget_ComboModelsKeepSelectionWhenAnEarlierOptionIsRemoved) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x554957, 31});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto textRoot =
        objects.NewObject<ComboBoxStringWidget>(access, GeneratedType<ComboBoxStringWidget>::Id());
    auto objectRoot =
        objects.NewObject<ComboBoxWidget>(access, GeneratedType<ComboBoxWidget>::Id());
    REQUIRE(textRoot && objectRoot);
    auto text = objects.ResolveForWrite<ComboBoxStringWidget>(access, textRoot.Value().Handle());
    auto object = objects.ResolveForWrite<ComboBoxWidget>(access, objectRoot.Value().Handle());
    REQUIRE(text && object);
    auto* strings = text.Value().Get().Value();
    auto* references = object.Value().Get().Value();
    REQUIRE(strings->AddOption("First"));
    REQUIRE(strings->AddOption("Second"));
    REQUIRE(strings->SelectOption("Second"));
    CHECK(strings->SelectedIndex() == 1);
    REQUIRE(strings->RemoveOptionAt(0));
    CHECK(strings->SelectedIndex() == 0 && strings->OptionAt(0).Value() == "Second");
    strings->SetOptionFactory(
        ComboBox::MakeOption::CreateLambda([&allocator](usize, bool) noexcept {
            return SharedPtr<Element>::TryMake(allocator, allocator);
        }));
    auto host = SharedPtr<Overlay>::TryMake(allocator, allocator);
    auto native = strings->TakeElement(objects);
    REQUIRE(host && native);
    REQUIRE(host.Value()->TryAddChild(native.Value()));
    auto& combo = static_cast<ComboBox&>(*native.Value());
    REQUIRE(combo.Open());
    strings->ReleaseElements();
    CHECK(!combo.IsOpen() && !combo.Open() && host.Value()->ChildCount() == 1);

    REQUIRE(strings->ClearOptions());
    CHECK(strings->SelectedIndex() == -1 && !strings->SelectOption("Second"));

    REQUIRE(references->AddOption(objects, textRoot.Value().Handle()));
    REQUIRE(references->SelectOption(textRoot.Value().Handle()));
    CHECK(references->SelectedIndex() == 0);
    REQUIRE(references->ClearOptions(objects));
    CHECK(references->OptionCount() == 0 && references->SelectedIndex() == -1);
}

} // namespace rocks::ui
