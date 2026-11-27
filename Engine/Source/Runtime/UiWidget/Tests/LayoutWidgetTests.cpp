#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiWidget/BorderWidget.generated.h>
#include <UiWidget/CheckBoxWidget.generated.h>
#include <UiWidget/SpacerWidget.generated.h>
#include <UiWidget/VerticalBoxWidget.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

TEST(UiWidget_LayoutControlsReapplyPropertiesAfterRebuild) {
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
    auto stackRef = tree->ConstructWidget(objects, GeneratedType<VerticalBoxWidget>::Id(), "Stats");
    auto borderRef =
        tree->ConstructWidget(objects, GeneratedType<BorderWidget>::Id(), "Background");
    auto spacerRef = tree->ConstructWidget(objects, GeneratedType<SpacerWidget>::Id(), "Gap");
    auto checkRef =
        tree->ConstructWidget(objects, GeneratedType<CheckBoxWidget>::Id(), "ShowStats");
    REQUIRE(stackRef && borderRef && spacerRef && checkRef);
    auto stack = objects.ResolveForWrite<VerticalBoxWidget>(access, stackRef.Value().Handle());
    auto border = objects.ResolveForWrite<BorderWidget>(access, borderRef.Value().Handle());
    auto spacer = objects.ResolveForWrite<SpacerWidget>(access, spacerRef.Value().Handle());
    auto check = objects.ResolveForWrite<CheckBoxWidget>(access, checkRef.Value().Handle());
    REQUIRE(stack && border && spacer && check);
    auto* root = stack.Value().Get().Value();
    REQUIRE(tree->SetRoot(objects, stackRef.Value().Handle()));
    REQUIRE(root->AddChild(objects, checkRef.Value().Handle()));
    REQUIRE(check.Value().Get().Value()->AddChild(objects, borderRef.Value().Handle()));
    REQUIRE(border.Value().Get().Value()->AddChild(objects, spacerRef.Value().Handle()));
    REQUIRE(!check.Value().Get().Value()->AddChild(objects, spacerRef.Value().Handle()));
    auto* gap = spacer.Value().Get().Value();
    REQUIRE(gap->SetSize({20, 8}));
    for (int pass = 0; pass < 2; ++pass) {
        auto native = root->TakeElement(objects);
        REQUIRE(native);
        REQUIRE(native.Value()->Prepass(1));
        REQUIRE(gap->CachedElement());
        CHECK(gap->CachedElement()->GetDesiredSize() == Vec2(20, 8));
        REQUIRE(!gap->SetSize({-1, 10}));
        root->ReleaseElements();
    }
}
} // namespace rocks::ui
