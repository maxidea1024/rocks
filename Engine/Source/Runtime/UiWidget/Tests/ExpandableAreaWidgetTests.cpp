#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiWidget/ExpandableAreaWidget.generated.h>
#include <UiWidget/SpacerWidget.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

TEST(UiWidget_ExpandableAreaKeepsRolesAndOverridesAcrossRebuild) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x554957, 25});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto treeRef = objects.NewObject<WidgetTree>(access, GeneratedType<WidgetTree>::Id());
    REQUIRE(treeRef);
    auto treeBorrow = objects.ResolveForWrite<WidgetTree>(access, treeRef.Value().Handle());
    REQUIRE(treeBorrow);
    auto* tree = treeBorrow.Value().Get().Value();

    auto root =
        tree->ConstructWidget(objects, GeneratedType<ExpandableAreaWidget>::Id(), "Details");
    auto header = tree->ConstructWidget(objects, GeneratedType<SpacerWidget>::Id(), "Summary");
    auto body = tree->ConstructWidget(objects, GeneratedType<SpacerWidget>::Id(), "Properties");
    REQUIRE(root && header && body);
    REQUIRE(tree->SetRoot(objects, root.Value().Handle()));
    auto areaRef = objects.ResolveForWrite<ExpandableAreaWidget>(access, root.Value().Handle());
    REQUIRE(areaRef);
    auto* area = areaRef.Value().Get().Value();
    REQUIRE(area->AddBody(objects, body.Value().Handle()));
    REQUIRE(!area->AddBody(objects, header.Value().Handle()));
    REQUIRE(area->AddChild(objects, header.Value().Handle()));
    uint32 toggles = 0;
    REQUIRE(area->OnExpansionChanged().TryAddLambda([&toggles](bool) noexcept { ++toggles; }));
    auto native = area->TakeElement(objects);
    REQUIRE(native && native.Value()->ChildCount() == 2);
    CHECK(native.Value()->ChildAt(1)->GetVisibility() == Visibility::Collapsed);
    area->SetExpanded(true);
    area->SetExpanded(true);
    // 프로그램 변경은 사건을 보내지 않는다(UE UExpandableArea::SetIsExpanded — UiToolkit 21.5a절).
    CHECK(toggles == 0 && native.Value()->ChildAt(1)->GetVisibility() != Visibility::Collapsed);
    // 사용자가 머리를 누르면 한 번 보낸다(UE SExpandableArea OnAreaExpansionChanged).
    Element& head = *native.Value()->ChildAt(0);
    CHECK(head.SendKey(0, input::InputEvent::KeyDown(input::Key::Enter)).IsHandled());
    CHECK(head.SendKey(0, input::InputEvent::KeyUp(input::Key::Enter)).IsHandled());
    CHECK(toggles == 1 && !area->IsExpanded());
    CHECK(native.Value()->ChildAt(1)->GetVisibility() == Visibility::Collapsed);
    area->SetExpanded(true);
    REQUIRE(area->RemoveChild(objects, header.Value().Handle()));
    CHECK(native.Value()->ChildAt(0)->ChildCount() == 0);
    CHECK(native.Value()->ChildAt(1)->ChildCount() == 1);
    REQUIRE(area->AddHeader(objects, header.Value().Handle()));
    area->SetForceVolatile(true);
    area->SetClipToBounds(true);
    REQUIRE(area->SetVisibility(Visibility::HitTestInvisible));
    area->ReleaseElements();
    auto rebuilt = area->TakeElement(objects);
    REQUIRE(rebuilt && rebuilt.Value()->ChildAt(0)->ChildCount() == 1);
    CHECK(rebuilt.Value()->IsVolatile() && rebuilt.Value()->ClipsToBounds());
    CHECK(rebuilt.Value()->GetVisibility() == Visibility::HitTestInvisible);
    rebuilt.Value()->SetVolatile(true);
    area->SetForceVolatile(false);
    CHECK(rebuilt.Value()->IsVolatile());
}
} // namespace rocks::ui
