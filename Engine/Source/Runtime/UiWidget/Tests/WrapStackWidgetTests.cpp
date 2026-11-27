#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiWidget/SpacerWidget.generated.h>
#include <UiWidget/UserWidget.generated.h>
#include <UiWidget/WidgetAsset.generated.h>
#include <UiWidget/WidgetTree.generated.h>
#include <UiWidget/WrapStackWidget.generated.h>
#include <UiWidget/WrapStackWidgetSlot.generated.h>

namespace rocks::ui {

TEST(UiWidget_WrapSlotsReflowAfterTemplateInstantiation) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x554957, 37});
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

    auto root = tree->ConstructWidget(objects, GeneratedType<WrapStackWidget>::Id(), "Bar");
    REQUIRE(root && tree->SetRoot(objects, root.Value().Handle()));
    auto rootBorrow = objects.ResolveForWrite<WrapStackWidget>(access, root.Value().Handle());
    REQUIRE(rootBorrow);
    auto* wrap = rootBorrow.Value().Get().Value();
    REQUIRE(wrap->SetSpacing({2, 3}));

    auto addSpacer = [&](StringView name, const WrapSlot& layout) {
        auto item = tree->ConstructWidget(objects, GeneratedType<SpacerWidget>::Id(), name);
        REQUIRE(item);
        auto spacer = objects.ResolveForWrite<SpacerWidget>(access, item.Value().Handle());
        REQUIRE(spacer && spacer.Value().Get().Value()->SetSize({10, 5}));
        auto slot = wrap->AddAt(objects, item.Value().Handle(), layout);
        REQUIRE(slot && objects.Resolve<WrapStackWidgetSlot>(access, slot.Value()));
    };
    addSpacer("First", {});
    addSpacer("Second",
              {.fillEmptySpace = true,
               .fillLineWhenSizeLessThan = 100,
               .placement = {.padding = {2, 1, 3, 2}, .horizontal = StackAlignment::End}});
    addSpacer("Third", {.forceNewLine = true});

    auto compiled = asset->Compile(objects, allocator);
    REQUIRE(compiled);
    auto instance = compiled.Value().Instantiate(objects);
    REQUIRE(instance);
    auto screen = objects.ResolveForWrite<UserWidget>(access, instance.Value().Handle());
    REQUIRE(screen);
    auto host = screen.Value().Get().Value()->TakeElement(objects);
    REQUIRE(host && host.Value()->Prepass(1));
    REQUIRE(host.Value()->Arrange(Geometry::Root({99, 60}, {})));

    auto* bar = host.Value()->ChildAt(0);
    REQUIRE(bar && bar->ChildCount() == 3);
    const auto* middle = bar->ChildAt(1);
    const auto* last = bar->ChildAt(2);
    CHECK(middle->GetGeometry().size == Vec2(10, 5));
    CHECK(middle->GetGeometry().layout.Apply({0, 0}) == Vec2(86, 9));
    CHECK(last->GetGeometry().layout.Apply({0, 0}) == Vec2(0, 19));

    // 원본과 화면 복제본은 독립된 슬롯을 갖는다. 원본에서 이미 붙인 슬롯을 직접 갱신한다.
    auto source = wrap->TakeElement(objects);
    REQUIRE(source && source.Value()->Prepass(1));
    REQUIRE(source.Value()->Arrange(Geometry::Root({99, 60}, {})));
    auto slotBorrow = objects.ResolveForWrite<WrapStackWidgetSlot>(access, wrap->SlotAt(1));
    REQUIRE(slotBorrow);
    auto* slot = slotBorrow.Value().Get().Value();
    const auto handle = slot->Handle();
    const auto previous = source.Value()->ChildRefAt(1);
    REQUIRE(!slot->UpdateLayout(objects, {.fillLineWhenSizeLessThan = -1}));
    CHECK(slot->WrapLayout().fillLineWhenSizeLessThan == 100);
    REQUIRE(slot->UpdateLayout(objects, {}));
    REQUIRE(source.Value()->Prepass(1));
    REQUIRE(source.Value()->Arrange(Geometry::Root({99, 60}, {})));
    CHECK(slot->Handle() == handle);
    CHECK(source.Value()->ChildAt(1) == previous.Get());
    CHECK(previous->GetGeometry().layout.Apply({0, 0}) == Vec2(12, 0));

    // 임계값과 같으면 첫 두 항목이 같은 줄을 쓰고 마지막 항목만 새 줄에 남는다.
    REQUIRE(host.Value()->Arrange(Geometry::Root({100, 60}, {})));
    CHECK(middle->GetGeometry().layout.Apply({0, 0}) == Vec2(87, 1));
    CHECK(last->GetGeometry().layout.Apply({0, 0}) == Vec2(0, 11));
}

} // namespace rocks::ui
