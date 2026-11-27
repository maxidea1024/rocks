#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Layout/Overlay.h>
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
    REQUIRE(strings->SelectOption(objects, "Second"));
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
    auto& combo = StaticElementCast<ComboBox>(*native.Value());
    REQUIRE(combo.Open());
    strings->ReleaseElements();
    CHECK(!combo.IsOpen() && !combo.Open() && host.Value()->ChildCount() == 1);

    REQUIRE(strings->ClearOptions());
    CHECK(strings->SelectedIndex() == -1 && !strings->SelectOption(objects, "Second"));

    REQUIRE(references->AddOption(objects, textRoot.Value().Handle()));
    REQUIRE(references->SelectOption(objects, textRoot.Value().Handle()));
    CHECK(references->SelectedIndex() == 0);
    REQUIRE(references->ClearOptions(objects));
    CHECK(references->OptionCount() == 0 && references->SelectedIndex() == -1);
}

TEST(UiWidget_ComboSelectionEventsFollowUeSelectInfo) {
    // UE UComboBoxString(Docs/Design/UiToolkit 21.5a절): 프로그램 선택도 사건을 보낸다(까닭 Direct
    // — 위젯이 없으면 HandleSelectionChanged, 있으면 SComboBox 목록 선택). 같은 항목은 다시 보내지
    // 않는다. Element 없이 해제하면 사건이 없고, Element가 있을 때 선택 항목을 지우면 (-1,
    // Direct)다. 목록 행 클릭은 Pointer다.
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x554957, 32});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto made =
        objects.NewObject<ComboBoxStringWidget>(access, GeneratedType<ComboBoxStringWidget>::Id());
    REQUIRE(made);
    auto borrowed = objects.ResolveForWrite<ComboBoxStringWidget>(access, made.Value().Handle());
    REQUIRE(borrowed);
    auto& combo = *borrowed.Value().Get().Value();
    REQUIRE(combo.AddOption("First"));
    REQUIRE(combo.AddOption("Second"));
    REQUIRE(combo.AddOption("Third"));
    struct Seen {
        int32 index;
        WidgetSelectionCause cause;
    };
    Array<Seen> seen(allocator);
    REQUIRE(combo.OnSelectionChanged().TryAddLambda(
        [&seen](int32 index, WidgetSelectionCause cause) noexcept {
            (void)seen.TryPushBack({index, cause});
        }));
    // Element 없음: 선택은 (1, Direct) 한 번, 같은 선택은 없음, 해제는 사건 없음.
    REQUIRE(combo.SetSelectedIndex(objects, 1));
    REQUIRE(combo.SetSelectedIndex(objects, 1));
    REQUIRE(seen.Size() == 1);
    CHECK(seen[0].index == 1 && seen[0].cause == WidgetSelectionCause::Direct);
    REQUIRE(combo.ClearSelection(objects));
    CHECK(seen.Size() == 1 && combo.SelectedIndex() == -1);
    // Element 있음: 프로그램 선택은 Direct, 행 클릭은 Pointer, 선택 항목 삭제는 (-1, Direct).
    combo.SetOptionFactory(ComboBox::MakeOption::CreateLambda([&allocator](usize, bool) noexcept {
        return SharedPtr<Element>::TryMake(allocator, allocator);
    }));
    auto native = combo.TakeElement(objects);
    REQUIRE(native);
    auto& element = StaticElementCast<ComboBox>(*native.Value());
    REQUIRE(combo.SetSelectedIndex(objects, 2));
    REQUIRE(seen.Size() == 2);
    CHECK(seen[1].index == 2 && seen[1].cause == WidgetSelectionCause::Direct);
    // 행 클릭이 부르는 선택 경로다(목록은 앱의 팝업 층이 있어야 열린다 — 여기서는 경로만 본다).
    REQUIRE(element.Select(0, SelectionCause::Pointer));
    REQUIRE(seen.Size() == 3);
    CHECK(seen[2].index == 0 && seen[2].cause == WidgetSelectionCause::Pointer);
    CHECK(combo.SelectedIndex() == 0);
    REQUIRE(combo.RemoveOptionAt(0));
    REQUIRE(seen.Size() == 4);
    CHECK(seen[3].index == -1 && seen[3].cause == WidgetSelectionCause::Direct);
    CHECK(combo.SelectedIndex() == -1);
    combo.ReleaseElements();
}

} // namespace rocks::ui
