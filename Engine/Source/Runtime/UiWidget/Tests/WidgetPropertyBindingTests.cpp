#include <Core/Memory/SystemAllocator.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <TestSupport/Test.h>
#include <UiWidget/CheckBoxWidget.generated.h>
#include <UiWidget/UserWidget.generated.h>
#include <UiWidget/Widget.generated.h>
#include <UiWidget/WidgetAsset.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

TEST(UiWidget_AssetViewModelInitializesBeforePublicationAndTracksNativeSetters) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    REQUIRE(RegisterWidgetTypes(registration));
    ObjectRegistry objects(allocator, types, {0x554957, 46});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);

    auto modelRef = objects.NewObject<CheckBoxWidget>(access, GeneratedType<CheckBoxWidget>::Id());
    auto assetRef = objects.NewObject<WidgetAsset>(access, GeneratedType<WidgetAsset>::Id());
    REQUIRE(modelRef && assetRef);
    const auto modelHandle = modelRef.Value().Handle();
    auto model = objects.ResolveForWrite<CheckBoxWidget>(access, modelHandle);
    auto asset = objects.ResolveForWrite<WidgetAsset>(access, assetRef.Value().Handle());
    REQUIRE(model && asset);
    auto* blueprint = asset.Value().Get().Value();
    REQUIRE(blueprint->Initialize(objects));
    blueprint->SetViewModelType(GeneratedType<CheckBoxWidget>::Id());
    auto tree = objects.ResolveForWrite<WidgetTree>(access, blueprint->Tree());
    REQUIRE(tree);
    auto* design = tree.Value().Get().Value();
    auto child = design->ConstructWidget(objects, GeneratedType<CheckBoxWidget>::Id(), "display");
    REQUIRE(child);
    REQUIRE(design->SetRoot(objects, child.Value().Handle()));
    auto widget = objects.Resolve<Widget>(access, child.Value().Handle());
    REQUIRE(widget);
    const auto id = widget.Value().Get().Value()->StableId();
    PropertyPathBuffer checked, enabled;
    REQUIRE(ParsePropertyPath(objects, access, modelHandle, "checked", checked));
    REQUIRE(ParsePropertyPath(objects, access, child.Value().Handle(), "enabled", enabled));
    REQUIRE(blueprint->AddPropertyBinding(
        {checked.steps[0].fieldId, id, enabled.steps[0].fieldId, false, true}));
    REQUIRE(blueprint->AddPropertyBinding(
        {checked.steps[0].fieldId, id, checked.steps[0].fieldId, true}));
    CHECK(!blueprint->AddPropertyBinding(
        {checked.steps[0].fieldId, id, enabled.steps[0].fieldId, true}));
    ObjectGraphSaveOptions options;
    options.externals = GraphExternalPolicy::Include;
    auto archive = SaveObjectGraph(allocator, objects, access, assetRef.Value().Handle(), options);
    REQUIRE(archive);
    ObjectRegistry game(allocator, types, {0x554957, 48});
    const auto runtimeAccess = game.OwnerAccess();
    ObjectAccessScope runtimeScope(runtimeAccess);
    auto restored = LoadObjectGraph(allocator, game, types, runtimeAccess, archive.Value().View());
    REQUIRE(restored);
    auto runtimeAsset = game.ResolveForWrite<WidgetAsset>(runtimeAccess, restored.Value().Handle());
    REQUIRE(runtimeAsset);
    auto compiled = runtimeAsset.Value().Get().Value()->Compile(game, allocator);
    REQUIRE(compiled);
    CHECK(!compiled.Value().Instantiate(game));
    auto runtimeModel =
        game.NewObject<CheckBoxWidget>(runtimeAccess, GeneratedType<CheckBoxWidget>::Id());
    REQUIRE(runtimeModel);
    auto modelObject =
        game.ResolveForWrite<CheckBoxWidget>(runtimeAccess, runtimeModel.Value().Handle());
    REQUIRE(modelObject);
    auto* actualModel = modelObject.Value().Get().Value();
    WidgetModelSource origin{actualModel->Handle(), {}};
    auto instance = compiled.Value().Instantiate(game, {}, "Screen", nullptr, origin);
    REQUIRE(instance);
    auto second = compiled.Value().Instantiate(game, {}, "SecondScreen", nullptr, origin);
    REQUIRE(second);
    auto other = game.ResolveForWrite<UserWidget>(runtimeAccess, second.Value().Handle());
    REQUIRE(other);
    auto otherRoot = other.Value().Get().Value()->TakeElement(game);
    REQUIRE(otherRoot);
    auto* otherTarget = otherRoot.Value()->ChildAt(0);
    REQUIRE(otherTarget);
    auto owner = game.ResolveForWrite<UserWidget>(runtimeAccess, instance.Value().Handle());
    REQUIRE(owner);
    auto* screen = owner.Value().Get().Value();
    CHECK(screen->ViewModel() == actualModel->Handle());
    auto live = screen->TakeElement(game);
    REQUIRE(live);
    Element* target = live.Value()->ChildAt(0);
    REQUIRE(target);
    CHECK(!target->IsEnabled());
    CHECK(!otherTarget->IsEnabled());
    actualModel->SetChecked(true);
    CHECK(target->IsEnabled());
    CHECK(otherTarget->IsEnabled());
    auto childTree = game.Resolve<WidgetTree>(runtimeAccess, screen->Tree());
    REQUIRE(childTree);
    auto targetWidget = game.ResolveForWrite<CheckBoxWidget>(
        runtimeAccess, childTree.Value().Get().Value()->RootWidget());
    REQUIRE(targetWidget);
    CHECK(!targetWidget.Value().Get().Value()->IsChecked());
    targetWidget.Value().Get().Value()->SetEnabled(false);
    CHECK(!actualModel->IsChecked());
    CHECK(!otherTarget->IsEnabled());
    actualModel->SetChecked(true);
    CHECK(target->IsEnabled());
    // TakeElement를 부른 쪽이 명시 ReleaseElements로 OnDestruct 짝을 맞춘다(앱에 붙지 않은
    // Element는 미룬 통지를 전할 곳이 없다 — Docs/Design/UiToolkit 21.4절).
    screen->ReleaseElements();
    other.Value().Get().Value()->ReleaseElements();
}

} // namespace rocks::ui
