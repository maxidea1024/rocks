#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiWidget/GridPanelWidget.generated.h>
#include <UiWidget/ScrollBoxWidget.generated.h>
#include <UiWidget/ScrollBoxWidgetSlot.generated.h>
#include <UiWidget/SpacerWidget.generated.h>
#include <UiWidget/UserWidget.generated.h>
#include <UiWidget/WidgetAsset.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

TEST(UiWidget_GridFillAndScrollSizeSurviveRebuild) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x554957, 1305});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);

    auto assetRef = objects.NewObject<WidgetAsset>(access, GeneratedType<WidgetAsset>::Id());
    REQUIRE(assetRef);
    auto assetBorrow = objects.ResolveForWrite<WidgetAsset>(access, assetRef.Value().Handle());
    REQUIRE(assetBorrow);
    auto* asset = assetBorrow.Value().Get().Value();
    REQUIRE(asset->Initialize(objects));
    auto treeBorrow = objects.ResolveForWrite<WidgetTree>(access, asset->Tree());
    REQUIRE(treeBorrow);
    auto* tree = treeBorrow.Value().Get().Value();

    auto gridId = tree->ConstructWidget(objects, GeneratedType<GridPanelWidget>::Id(), "Grid");
    auto scrollId = tree->ConstructWidget(objects, GeneratedType<ScrollBoxWidget>::Id(), "Scroll");
    REQUIRE(gridId && scrollId);
    REQUIRE(tree->SetRoot(objects, gridId.Value().Handle()));
    auto gridBorrow = objects.ResolveForWrite<GridPanelWidget>(access, gridId.Value().Handle());
    auto scrollBorrow = objects.ResolveForWrite<ScrollBoxWidget>(access, scrollId.Value().Handle());
    REQUIRE(gridBorrow && scrollBorrow);
    auto* grid = gridBorrow.Value().Get().Value();
    auto* scroll = scrollBorrow.Value().Get().Value();
    REQUIRE(grid->AddAt(objects, scrollId.Value().Handle(), {}));
    REQUIRE(grid->SetColumnFill(0, 1));
    REQUIRE(grid->SetRowFill(0, 1));
    ObjectHandle second;
    const StringView labels[] = {"Auto", "Fill"};
    for (const auto label : labels) {
        auto item = tree->ConstructWidget(objects, GeneratedType<SpacerWidget>::Id(), label);
        REQUIRE(item);
        auto spacer = objects.ResolveForWrite<SpacerWidget>(access, item.Value().Handle());
        REQUIRE(spacer);
        REQUIRE(spacer.Value().Get().Value()->SetSize({8, 10}));
        auto slot = scroll->AddChild(objects, item.Value().Handle());
        REQUIRE(slot);
        second = slot.Value();
    }

    auto scrollSlot = objects.ResolveForWrite<ScrollBoxWidgetSlot>(access, second);
    REQUIRE(scrollSlot);
    auto* weighted = scrollSlot.Value().Get().Value();
    REQUIRE(weighted->SetSize(objects, StackSizeRule::Fill, 2));
    REQUIRE(!weighted->SetSize(objects, StackSizeRule::Fill, -2));

    auto compiled = asset->Compile(objects, allocator);
    REQUIRE(compiled);
    auto instance = compiled.Value().Instantiate(objects);
    REQUIRE(instance);
    auto screen = objects.ResolveForWrite<UserWidget>(access, instance.Value().Handle());
    REQUIRE(screen);
    auto host = screen.Value().Get().Value()->TakeElement(objects);
    REQUIRE(host && host.Value()->Prepass(1));
    REQUIRE(host.Value()->Arrange(Geometry::Root({80, 60}, {})));
    auto* copiedScroll = host.Value()->ChildAt(0)->ChildAt(0);
    CHECK(copiedScroll->GetGeometry().size == Vec2(80, 60));
    CHECK(copiedScroll->ChildAt(0)->ChildAt(1)->GetGeometry().size == Vec2(80, 50));

    auto native = grid->TakeElement(objects);
    REQUIRE(native && native.Value()->Prepass(1));
    REQUIRE(native.Value()->Arrange(Geometry::Root({80, 60}, {})));
    auto* stack = scroll->CachedElement()->ChildAt(0);
    auto secondElement = stack->ChildRefAt(1);
    REQUIRE(weighted->SetLayout(objects, {.padding = {2, 1, 3, 4}}));
    REQUIRE(native.Value()->Prepass(1));
    REQUIRE(native.Value()->Arrange(Geometry::Root({80, 60}, {})));
    CHECK(secondElement->GetGeometry().size == Vec2(75, 45));
    REQUIRE(weighted->SetSize(objects, StackSizeRule::Auto));
    REQUIRE(native.Value()->Prepass(1));
    REQUIRE(native.Value()->Arrange(Geometry::Root({80, 60}, {})));
    CHECK(secondElement->GetGeometry().size == Vec2(75, 10));
    grid->ClearFill();
    REQUIRE(native.Value()->Arrange(Geometry::Root({80, 60}, {})));
    CHECK(grid->ColumnFill(0) == 0 && grid->RowFill(0) == 0);
    CHECK(scroll->CachedElement()->GetGeometry().size == Vec2(13, 25));
    // TakeElement를 부른 쪽이 명시 ReleaseElements로 OnDestruct 짝을 맞춘다(앱에 붙지 않은
    // Element는 미룬 통지를 전할 곳이 없다 — Docs/Design/UiToolkit 21.4절).
    screen.Value().Get().Value()->ReleaseElements();
}

} // namespace rocks::ui
