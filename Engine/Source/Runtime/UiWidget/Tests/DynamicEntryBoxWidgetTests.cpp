#include "WarningCapture.h"
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiWidget/DynamicEntryBoxWidget.generated.h>
#include <UiWidget/SafeZoneWidget.generated.h>
#include <UiWidget/SafeZoneWidgetSlot.generated.h>
#include <UiWidget/SpacerWidget.generated.h>
#include <UiWidget/UserWidget.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

TEST(UiWidget_DynamicEntriesReuseObjectsInsideSafeZone) {
    // 엔트리 제거(PanelWidget::RemoveChild)는 뗀 Element를 명시 해제가 끝날 때까지 붙잡는다 —
    // 먼저 부서지면 수명 경고가 난다(Docs/Design/UiToolkit 21.4절).
    test::WarningCapture warnings;
    REQUIRE(warnings.started);
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x554957, 30});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto root = objects.NewObject<WidgetTree>(access, GeneratedType<WidgetTree>::Id());
    REQUIRE(root);
    auto borrow = objects.ResolveForWrite<WidgetTree>(access, root.Value().Handle());
    REQUIRE(borrow);
    auto* tree = borrow.Value().Get().Value();
    auto safe = tree->ConstructWidget(objects, GeneratedType<SafeZoneWidget>::Id(), "Safe");
    auto entries =
        tree->ConstructWidget(objects, GeneratedType<DynamicEntryBoxWidget>::Id(), "Items");
    REQUIRE(safe && entries);
    auto zone = objects.ResolveForWrite<SafeZoneWidget>(access, safe.Value().Handle());
    auto box = objects.ResolveForWrite<DynamicEntryBoxWidget>(access, entries.Value().Handle());
    REQUIRE(zone && box);
    auto* panel = box.Value().Get().Value();
    auto* parent = zone.Value().Get().Value();
    REQUIRE(tree->SetRoot(objects, safe.Value().Handle()));
    auto safetySlot = parent->AddChild(objects, entries.Value().Handle());
    REQUIRE(safetySlot);
    REQUIRE(parent->SetSafeInsets({8, 12, 4, 2}));
    parent->SetSidesToPad(true, false, true, true);
    auto native = parent->TakeElement(objects);
    REQUIRE(native);
    REQUIRE(native.Value()->Arrange(Geometry::Root({100, 60}, {})));
    CHECK(panel->CachedElement()->GetGeometry().size == Vec2(88, 58));
    auto slot = objects.ResolveForWrite<SafeZoneWidgetSlot>(access, safetySlot.Value());
    REQUIRE(slot);
    REQUIRE(parent->SetSafeInsets({8, 12, 4, 2}, {3, 2, 1, 4}));
    REQUIRE(slot.Value().Get().Value()->SetSafety(objects, false, {2, 2, 3, 0.5f}));
    REQUIRE(native.Value()->Arrange(Geometry::Root({100, 60}, {})));
    CHECK(panel->CachedElement()->GetGeometry().size == Vec2(91, 58));
    CHECK(panel->CachedElement()->GetGeometry().layout.Apply({}) == Vec2(6, 0));

    auto type = ClassRef<UserWidget>::Of<UserWidget>();
    auto first = panel->CreateEntry(objects, type);
    REQUIRE(first);
    REQUIRE(panel->RemoveChild(objects, first.Value()));
    CHECK(panel->ChildCount() == 0);
    auto next = panel->CreateEntry(objects, type);
    REQUIRE(next);
    CHECK(next.Value() == first.Value());
    CHECK(panel->ChildCount() == 1);
    auto active = panel->EntryAt(objects, 0);
    REQUIRE(active);
    CHECK(active.Value() == first.Value());
    REQUIRE(panel->ResetEntries(objects));
    CHECK(panel->ChildCount() == 0);
    auto reused = panel->CreateEntry(objects, type);
    REQUIRE(reused);
    CHECK(reused.Value() == first.Value());
    // TakeElement를 부른 쪽이 명시 ReleaseElements로 OnDestruct 짝을 맞춘다(앱에 붙지 않은
    // Element는 미룬 통지를 전할 곳이 없다 — Docs/Design/UiToolkit 21.4절).
    parent->ReleaseElements();
    CHECK(warnings.UiWarnings() == 0);
}

TEST(UiWidget_DynamicEntryLayoutsArrangeTheSameEntriesInSixModes) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x554957, 1201});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto root = objects.NewObject<WidgetTree>(access, GeneratedType<WidgetTree>::Id());
    REQUIRE(root);
    auto tree = objects.ResolveForWrite<WidgetTree>(access, root.Value().Handle());
    REQUIRE(tree);
    auto* widgets = tree.Value().Get().Value();
    auto id =
        widgets->ConstructWidget(objects, GeneratedType<DynamicEntryBoxWidget>::Id(), "Inventory");
    REQUIRE(id);
    auto box = objects.ResolveForWrite<DynamicEntryBoxWidget>(access, id.Value().Handle());
    REQUIRE(box);
    auto* panel = box.Value().Get().Value();
    const StringView names[] = {"A", "B"};
    for (StringView name : names) {
        auto entry = widgets->ConstructWidget(objects, GeneratedType<SpacerWidget>::Id(), name);
        REQUIRE(entry);
        auto spacer = objects.ResolveForWrite<SpacerWidget>(access, entry.Value().Handle());
        REQUIRE(spacer);
        REQUIRE(spacer.Value().Get().Value()->SetSize({8, 8}));
        REQUIRE(panel->AddChild(objects, entry.Value().Handle()));
    }

    const DynamicBoxType modes[] = {DynamicBoxType::Vertical, DynamicBoxType::Horizontal,
                                    DynamicBoxType::Wrap,     DynamicBoxType::VerticalWrap,
                                    DynamicBoxType::Overlay,  DynamicBoxType::Radial};
    for (const auto type : modes) {
        REQUIRE(panel->SetBoxType(type));
        REQUIRE(panel->SetEntrySpacing({2, 3}));
        auto native = panel->TakeElement(objects);
        REQUIRE(native);
        REQUIRE(native.Value()->Prepass(1));
        REQUIRE(native.Value()->Arrange(Geometry::Root({12, 12}, {})));
        const Vec2 offset = native.Value()->ChildAt(1)->GetGeometry().layout.Apply({});
        if (type == DynamicBoxType::Vertical || type == DynamicBoxType::Wrap) {
            CHECK(offset == Vec2(0, 11));
        } else if (type == DynamicBoxType::Horizontal || type == DynamicBoxType::VerticalWrap) {
            CHECK(offset == Vec2(10, 0));
        } else if (type == DynamicBoxType::Overlay) {
            CHECK(offset == Vec2(2, 3));
            const Vec2 steps[] = {{1, 2}, {3, 4}};
            REQUIRE(panel->SetSpacingPattern(steps));
            REQUIRE(native.Value()->Arrange(Geometry::Root({12, 12}, {})));
            CHECK(native.Value()->ChildAt(1)->GetGeometry().layout.Apply({}) == Vec2(1, 2));
        } else {
            CHECK(offset.x < native.Value()->ChildAt(0)->GetGeometry().layout.tx);
            RadialBoxSettings settings;
            settings.distributeEvenly = false;
            settings.clockwise = true;
            settings.angleBetweenItems = 90;
            settings.margin = 1;
            REQUIRE(panel->SetRadialSettings(settings));
            REQUIRE(native.Value()->Arrange(Geometry::Root({20, 20}, {})));
            const Vec2 second = native.Value()->ChildAt(1)->GetGeometry().layout.Apply({});
            CHECK(math::Abs(second.x - 6) < 0.001f && math::Abs(second.y - 11) < 0.001f);
        }

        panel->ReleaseElements();
    }
}

} // namespace rocks::ui
