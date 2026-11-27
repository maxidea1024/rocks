#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiWidget/HorizontalBoxWidget.generated.h>
#include <UiWidget/HorizontalBoxWidgetSlot.generated.h>
#include <UiWidget/BorderWidget.generated.h>
#include <UiWidget/BorderWidgetSlot.generated.h>
#include <UiWidget/SpacerWidget.generated.h>
#include <UiWidget/UserWidget.generated.h>
#include <UiWidget/WidgetAsset.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

TEST(UiWidget_StackAndAlignedSlotsSurviveTemplateInstantiation) {
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

    auto root = tree->ConstructWidget(objects, GeneratedType<HorizontalBoxWidget>::Id(), "Bar");
    auto border = tree->ConstructWidget(objects, GeneratedType<BorderWidget>::Id(), "Right");
    auto item = tree->ConstructWidget(objects, GeneratedType<SpacerWidget>::Id(), "Icon");
    REQUIRE(root && border && item);
    REQUIRE(tree->SetRoot(objects, root.Value().Handle()));
    auto panel = objects.ResolveForWrite<HorizontalBoxWidget>(access, root.Value().Handle());
    auto frame = objects.ResolveForWrite<BorderWidget>(access, border.Value().Handle());
    auto spacer = objects.ResolveForWrite<SpacerWidget>(access, item.Value().Handle());
    REQUIRE(panel && frame && spacer);
    REQUIRE(spacer.Value().Get().Value()->SetSize({12, 8}));
    auto* stack = panel.Value().Get().Value();
    auto* content = frame.Value().Get().Value();
    REQUIRE(!stack->AddAt(objects, border.Value().Handle(), {.weight = -1}));
    auto stackSlot = stack->AddAt(objects, border.Value().Handle(),
                                  {.rule = StackSizeRule::Fill, .before = 5, .after = 3});
    REQUIRE(stackSlot && objects.Resolve<HorizontalBoxWidgetSlot>(access, stackSlot.Value()));
    SlotPlacement placement{.padding = {1, 2, 3, 4},
                            .horizontal = StackAlignment::End,
                            .vertical = StackAlignment::Center};
    auto contentSlot = content->AddAlignedChild(objects, item.Value().Handle(), placement);
    REQUIRE(contentSlot && objects.Resolve<BorderWidgetSlot>(access, contentSlot.Value()));
    auto compiled = asset->Compile(objects, allocator);
    REQUIRE(compiled);
    auto instance = compiled.Value().Instantiate(objects);
    REQUIRE(instance);
    auto screen = objects.ResolveForWrite<UserWidget>(access, instance.Value().Handle());
    REQUIRE(screen);
    auto host = screen.Value().Get().Value()->TakeElement(objects);
    REQUIRE(host && host.Value()->Prepass(1));
    REQUIRE(host.Value()->Arrange(Geometry::Root({100, 60}, {})));
    auto* bar = host.Value()->ChildAt(0);
    REQUIRE(bar);
    auto* right = bar->ChildAt(0);
    REQUIRE(right);
    CHECK(right->GetGeometry().size == Vec2(92, 60));
    // slot 여백·정렬은 wrapper 없이 자식 Element가 직접 든다(20.10절).
    auto* icon = right->ChildAt(0);
    REQUIRE(icon && icon->GetSlotPlacement());
    CHECK(*icon->GetSlotPlacement() == placement);
    CHECK(icon->GetGeometry().size == Vec2(12, 8));
    CHECK(icon->GetGeometry().layout.Apply({0, 0}) == Vec2(82, 25));
    auto live = content->TakeElement(objects);
    REQUIRE(live && live.Value()->ChildCount() == 1);
    REQUIRE(content->RemoveChild(objects, item.Value().Handle()));
    CHECK(live.Value()->ChildCount() == 0);
    auto ordinary = content->AddChild(objects, item.Value().Handle());
    REQUIRE(ordinary);
    auto sameIcon = spacer.Value().Get().Value()->CachedElement();
    CHECK(live.Value()->ChildAt(0) == sameIcon.Get());
    auto mutableSlot = objects.ResolveForWrite<BorderWidgetSlot>(access, ordinary.Value());
    REQUIRE(mutableSlot);
    REQUIRE(mutableSlot.Value().Get().Value()->SetLayout(objects, placement));
    REQUIRE(live.Value()->ChildAt(0) == sameIcon.Get());
    REQUIRE(sameIcon->GetSlotPlacement() && *sameIcon->GetSlotPlacement() == placement);
    REQUIRE(mutableSlot.Value().Get().Value()->SetLayout(objects, {}));
    CHECK(live.Value()->ChildAt(0) == sameIcon.Get());
    // 기본값이면 slot 정보를 지운다.
    CHECK(sameIcon->GetSlotPlacement() == nullptr);
    auto nativeStack = stack->TakeElement(objects);
    auto mutableStackSlot =
        objects.ResolveForWrite<HorizontalBoxWidgetSlot>(access, stackSlot.Value());
    REQUIRE(nativeStack && mutableStackSlot);
    REQUIRE(mutableStackSlot.Value().Get().Value()->SetLayout(
        objects, {.rule = StackSizeRule::Fill, .before = 20}));
    REQUIRE(nativeStack.Value()->Prepass(1));
    REQUIRE(nativeStack.Value()->Arrange(Geometry::Root({100, 60}, {})));
    CHECK(live.Value()->GetGeometry().size == Vec2(80, 60));
    REQUIRE(mutableStackSlot.Value().Get().Value()->SetLayout(
        objects,
        {.rule = StackSizeRule::Fill, .before = 20, .mainAlignment = StackAlignment::End}));
    REQUIRE(nativeStack.Value()->Prepass(1));
    REQUIRE(nativeStack.Value()->Arrange(Geometry::Root({100, 60}, {})));
    CHECK(live.Value()->GetGeometry().size.x == live.Value()->GetDesiredSize().x);
    CHECK(live.Value()->GetGeometry().layout.Apply({}).x == 100 - live.Value()->GetDesiredSize().x);
}
} // namespace rocks::ui
