#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <TestSupport/Test.h>
#include <UiWidget/OverlayWidget.generated.h>
#include <UiWidget/ProgressBarWidget.generated.h>
#include <UiWidget/UserWidget.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

TEST(UiWidget_TreeTraversalHonorsSlotOrderNestedBoundariesAndLiveElements) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    REQUIRE(RegisterWidgetTypes(registration));
    // 이름 찾기(FindWidget)는 객체 이름을 읽으므로 이름 풀을 붙인다.
    NamePool names(allocator);
    REQUIRE(names.Initialize());
    RegistryOptions options;
    options.names = &names;
    ObjectRegistry objects(allocator, types, {0x554957, 47}, options);
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto owner = objects.NewObject<WidgetTree>(access, GeneratedType<WidgetTree>::Id());
    REQUIRE(owner);
    auto borrowed = objects.ResolveForWrite<WidgetTree>(access, owner.Value().Handle());
    REQUIRE(borrowed);
    auto* tree = borrowed.Value().Get().Value();
    auto panel = tree->ConstructWidget(objects, GeneratedType<OverlayWidget>::Id(), "root");
    auto screen = tree->ConstructWidget(objects, GeneratedType<UserWidget>::Id(), "screen");
    auto sibling =
        tree->ConstructWidget(objects, GeneratedType<ProgressBarWidget>::Id(), "sibling");
    REQUIRE(panel && screen && sibling);
    auto overlay = objects.ResolveForWrite<OverlayWidget>(access, panel.Value().Handle());
    auto nested = objects.ResolveForWrite<UserWidget>(access, screen.Value().Handle());
    REQUIRE(overlay && nested);
    REQUIRE(tree->SetRoot(objects, panel.Value().Handle()));
    auto* children = overlay.Value().Get().Value();
    REQUIRE(children->AddChild(objects, screen.Value().Handle()));
    REQUIRE(children->AddChild(objects, sibling.Value().Handle()));
    auto* user = nested.Value().Get().Value();
    REQUIRE(user->Initialize(objects));
    auto inner = objects.ResolveForWrite<WidgetTree>(access, user->Tree());
    REQUIRE(inner);
    auto* sub = inner.Value().Get().Value();
    auto child = sub->ConstructWidget(objects, GeneratedType<ProgressBarWidget>::Id(), "inner");
    REQUIRE(child);
    REQUIRE(sub->SetRoot(objects, child.Value().Handle()));
    auto outerOnly = tree->GetAllWidgets(objects);
    auto all = tree->GetAllWidgets(objects, true);
    REQUIRE(outerOnly && all);
    REQUIRE(outerOnly.Value().Size() == 3 && all.Value().Size() == 4);
    CHECK(all.Value()[0] == panel.Value().Handle());
    CHECK(all.Value()[1] == screen.Value().Handle());
    CHECK(all.Value()[2] == child.Value().Handle());
    CHECK(all.Value()[3] == sibling.Value().Handle());
    auto byName = tree->FindWidget(objects, "SIBLING");
    REQUIRE(byName);
    CHECK(byName.Value() == sibling.Value().Handle());
    CHECK(!tree->FindWidget(objects, "inner"));
    auto native = children->TakeElement(objects);
    REQUIRE(native);
    auto byElement = tree->FindWidget(objects, *native.Value());
    REQUIRE(byElement);
    CHECK(byElement.Value() == panel.Value().Handle());
    CHECK(!tree->RemoveWidget(objects, child.Value().Handle()));
    REQUIRE(tree->RemoveWidget(objects, sibling.Value().Handle()));
    CHECK(!tree->FindWidget(objects, "sibling"));
    REQUIRE(tree->RemoveWidget(objects, panel.Value().Handle()));
    CHECK(tree->RootWidget().IsNull());
    // TakeElement를 부른 쪽이 명시 ReleaseElements로 OnDestruct 짝을 맞춘다(앱에 붙지 않은
    // Element는 미룬 통지를 전할 곳이 없다 — Docs/Design/UiToolkit 21.4절).
    children->ReleaseElements();
}

} // namespace rocks::ui
