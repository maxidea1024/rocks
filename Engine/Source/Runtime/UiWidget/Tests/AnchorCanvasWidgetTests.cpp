#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/HitGrid.h>
#include <UiWidget/ButtonWidget.generated.h>
#include <UiWidget/AnchorCanvasWidget.generated.h>
#include <UiWidget/AnchorCanvasWidgetSlot.generated.h>
#include <UiWidget/SpacerWidget.generated.h>
#include <UiWidget/UserWidget.generated.h>
#include <UiWidget/WidgetAsset.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

TEST(UiWidget_CanvasSlotSurvivesTemplateInstantiation) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x554957, 27});
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
    auto root = tree->ConstructWidget(objects, GeneratedType<AnchorCanvasWidget>::Id(), "Hud");
    auto gap = tree->ConstructWidget(objects, GeneratedType<SpacerWidget>::Id(), "Reticle");
    REQUIRE(root && gap);
    REQUIRE(tree->SetRoot(objects, root.Value().Handle()));
    auto rootBorrow = objects.ResolveForWrite<AnchorCanvasWidget>(access, root.Value().Handle());
    REQUIRE(rootBorrow);
    auto* panel = rootBorrow.Value().Get().Value();
    auto invalid = panel->AddAt(objects, gap.Value().Handle(), {.anchorMin = {1, 1}});
    CHECK(!invalid && panel->ChildCount() == 0);
    CanvasSlot layout{.anchorMin = {0.5f, 0.5f},
                      .anchorMax = {0.5f, 0.5f},
                      .offset = {-5, -5, 10, 10},
                      .alignment = {0, 0}};
    auto slot = panel->AddAt(objects, gap.Value().Handle(), layout);
    REQUIRE(slot);
    REQUIRE(objects.Resolve<AnchorCanvasWidgetSlot>(access, slot.Value()));

    auto compiled = asset->Compile(objects, allocator);
    REQUIRE(compiled);
    auto instance = compiled.Value().Instantiate(objects);
    REQUIRE(instance);
    auto screen = objects.ResolveForWrite<UserWidget>(access, instance.Value().Handle());
    REQUIRE(screen);
    auto host = screen.Value().Get().Value()->TakeElement(objects);
    REQUIRE(host);
    REQUIRE(host.Value()->Prepass(1));
    REQUIRE(host.Value()->Arrange(Geometry::Root({100, 80}, {})));
    auto* canvas = host.Value()->ChildAt(0);
    REQUIRE(canvas && canvas->ChildCount() == 1);
    auto* reticle = canvas->ChildAt(0);
    REQUIRE(reticle);
    CHECK(reticle->GetGeometry().size == Vec2(10, 10));
    CHECK(reticle->GetGeometry().layout.Apply({0, 0}) == Vec2(45, 35));
    // 원본 화면에는 같은 곳에 놓인 버튼 두 개를 추가한다. 순서를 바꿔도 자식이나 슬롯은 교체되지
    // 않는다.
    auto lower = tree->ConstructWidget(objects, GeneratedType<ButtonWidget>::Id(), "Lower");
    auto upper = tree->ConstructWidget(objects, GeneratedType<ButtonWidget>::Id(), "Upper");
    REQUIRE(lower && upper);
    CanvasSlot bounds{.offset = {0, 0, 30, 20}, .alignment = {0, 0}};
    auto lowerSlot = panel->AddAt(objects, lower.Value().Handle(), bounds);
    auto upperSlot = panel->AddAt(objects, upper.Value().Handle(), bounds);
    REQUIRE(lowerSlot && upperSlot);
    auto mutableSlot = objects.ResolveForWrite<AnchorCanvasWidgetSlot>(access, lowerSlot.Value());
    REQUIRE(mutableSlot);
    auto* moved = mutableSlot.Value().Get().Value();
    auto native = panel->TakeElement(objects);
    REQUIRE(native && native.Value()->Prepass(1));
    REQUIRE(native.Value()->Arrange(Geometry::Root({100, 80}, {})));
    auto low = native.Value()->ChildRefAt(1);
    auto high = native.Value()->ChildRefAt(2);
    HitGrid hits(allocator);
    REQUIRE(hits.Update(native.Value()));
    CHECK(hits.Find({5, 5}, *native.Value()).Get() == high.Get());
    REQUIRE(moved->SetZOrder(objects, 9));
    REQUIRE(native.Value()->Arrange(Geometry::Root({100, 80}, {})));
    REQUIRE(hits.Update(native.Value()));
    CHECK(hits.Find({5, 5}, *native.Value()).Get() == low.Get());
    CHECK(moved->Handle() == lowerSlot.Value());
    REQUIRE(!moved->SetMinimum(objects, {2, 2}));
    REQUIRE(moved->SetAnchors(objects, {}, {1, 1}));
    REQUIRE(moved->SetOffsets(objects, {10, 8, 20, 16}));
    REQUIRE(native.Value()->Arrange(Geometry::Root({100, 80}, {})));
    CHECK(low->GetGeometry().size == Vec2(70, 56));
    CHECK(low->GetGeometry().layout.Apply({}) == Vec2(10, 8));
    REQUIRE(moved->SetAnchors(objects, {}, {}));
    REQUIRE(moved->SetAutoSize(objects, true));
    REQUIRE(native.Value()->Prepass(1));
    REQUIRE(native.Value()->Arrange(Geometry::Root({100, 80}, {})));
    CHECK(low->GetGeometry().size == low->GetDesiredSize());
}

} // namespace rocks::ui
