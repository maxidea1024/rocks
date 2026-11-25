#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiWidget/SpacerWidget.generated.h>
#include <UiWidget/UserWidget.generated.h>
#include <UiWidget/Widget.generated.h>
#include <UiWidget/WidgetAnimation.generated.h>
#include <UiWidget/WidgetAsset.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

TEST(UiWidget_AssetAnimationInstantiatesByIdAndRejectsMissingTarget) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    REQUIRE(RegisterWidgetTypes(registration));
    ObjectRegistry objects(allocator, types, {0x554957, 49});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto asset = objects.NewObject<WidgetAsset>(access, GeneratedType<WidgetAsset>::Id());
    REQUIRE(asset);
    auto assetBorrow = objects.ResolveForWrite<WidgetAsset>(access, asset.Value().Handle());
    REQUIRE(assetBorrow);
    auto* blueprint = assetBorrow.Value().Get().Value();
    REQUIRE(blueprint->Initialize(objects));
    auto tree = objects.ResolveForWrite<WidgetTree>(access, blueprint->Tree());
    REQUIRE(tree);
    auto* design = tree.Value().Get().Value();
    auto widget = design->ConstructWidget(objects, GeneratedType<SpacerWidget>::Id(), "box");
    REQUIRE(widget);
    REQUIRE(design->SetRoot(objects, widget.Value().Handle()));
    auto created = blueprint->CreateAnimation(objects, "Fade");
    REQUIRE(created);
    auto animation = objects.ResolveForWrite<WidgetAnimation>(access, created.Value().Handle());
    REQUIRE(animation);
    auto* clip = animation.Value().Get().Value();
    auto target = objects.Resolve<SpacerWidget>(access, widget.Value().Handle());
    REQUIRE(target);
    TangentCurve curve(allocator);
    REQUIRE(curve.TryAddKey(0, 0));
    REQUIRE(curve.TryAddKey(1, 1));
    REQUIRE(clip->SetTrack(target.Value().Get().Value()->StableId(),
                           WidgetAnimationChannel::Opacity, curve));
    auto templateData = blueprint->Compile(objects, allocator);
    REQUIRE(templateData);
    auto instance = templateData.Value().Instantiate(objects);
    REQUIRE(instance);
    auto borrowedScreen = objects.ResolveForWrite<UserWidget>(access, instance.Value().Handle());
    REQUIRE(borrowedScreen);
    auto* screen = borrowedScreen.Value().Get().Value();
    auto found = screen->FindAnimation(objects, "fade");
    REQUIRE(found);
    CHECK(found.Value() != clip->Handle());
    auto byId = screen->FindAnimation(objects, clip->StableId());
    REQUIRE(byId);
    CHECK(byId.Value() == found.Value());
    Application app(allocator);
    auto root = screen->TakeElement(objects);
    REQUIRE(root);
    app.SetRoot(root.Value());
    auto player = screen->PlayAnimation(objects, app, clip->StableId());
    REQUIRE(player);
    REQUIRE(player.Value()->Seek(0.5f));
    auto liveTree = objects.Resolve<WidgetTree>(access, screen->Tree());
    REQUIRE(liveTree);
    auto box = objects.Resolve<Widget>(access, liveTree.Value().Get().Value()->RootWidget());
    REQUIRE(box);
    CHECK(box.Value().Get().Value()->GetRenderOpacity() == 0.5f);
    REQUIRE(clip->SetTrack({1, 99}, WidgetAnimationChannel::Opacity, curve));
    auto invalid = blueprint->Compile(objects, allocator);
    REQUIRE(!invalid);
    CHECK(invalid.Error() == ErrorCode::NotFound);
    screen->StopAllAnimations();
}

} // namespace rocks::ui
