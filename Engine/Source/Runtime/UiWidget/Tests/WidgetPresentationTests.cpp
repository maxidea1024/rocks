#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiWidget/OverlayWidget.generated.h>
#include <UiWidget/ProgressBarWidget.generated.h>
#include <UiWidget/UserWidget.generated.h>
#include <UiWidget/WidgetTree.generated.h>
#include <UiWidget/Widget.generated.h>

namespace rocks::ui {
TEST(UiWidget_ViewportPlacementAndTransformsSurviveReattachment) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    REQUIRE(RegisterWidgetTypes(registration));
    ObjectRegistry objects(allocator, types, {0x554957, 83});
    auto access=objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto owner=objects.NewObject<UserWidget>(access,GeneratedType<UserWidget>::Id());
    REQUIRE(owner);
    auto borrow=objects.ResolveForWrite<UserWidget>(access,owner.Value().Handle());
    REQUIRE(borrow);
    auto& screen=*borrow.Value().Get().Value();
    REQUIRE(screen.Initialize(objects));
    auto tree=objects.ResolveForWrite<WidgetTree>(access,screen.Tree());
    REQUIRE(tree);
    auto& content=*tree.Value().Get().Value();
    auto panel=content.ConstructWidget(objects,GeneratedType<OverlayWidget>::Id(),"Panel");
    auto leaf=content.ConstructWidget(objects,GeneratedType<ProgressBarWidget>::Id(),"Progress");
    REQUIRE(panel && leaf && content.SetRoot(objects,panel.Value().Handle()));
    auto overlay=objects.ResolveForWrite<OverlayWidget>(access,panel.Value().Handle());
    auto widget=objects.ResolveForWrite<Widget>(access,leaf.Value().Handle());
    REQUIRE(overlay && widget);
    auto& parent=*overlay.Value().Get().Value();
    auto& progress=*widget.Value().Get().Value();
    REQUIRE(parent.AddChild(objects,leaf.Value().Handle()));
    REQUIRE(parent.GetChildAt(objects,0));
    CHECK(parent.GetChildAt(objects,0).Value()==leaf.Value().Handle());
    CHECK(parent.GetChildIndex(objects,leaf.Value().Handle()).Value()==0);
    CHECK(progress.GetParent(objects).Value()==panel.Value().Handle());
    REQUIRE(progress.SetRenderTranslation({4,5}));
    REQUIRE(progress.SetRenderOpacity(0.5f));
    auto layer=SharedPtr<AnchorCanvas>::TryMake(allocator,allocator);
    REQUIRE(layer);
    REQUIRE(screen.SetDesiredSizeInViewport({40,20}));
    REQUIRE(screen.SetPositionInViewport({10,12}));
    const auto viewport=Geometry::Root({100,100},{});
    for (int cycle=0;cycle<2;++cycle) {
        REQUIRE(screen.AddToViewport(objects,layer.Value(),2));
        CHECK(screen.IsInViewport() && layer.Value()->ChildCount()==1);
        CHECK(!screen.AddToViewport(objects,layer.Value()));
        REQUIRE(layer.Value()->Prepass(1));
        REQUIRE(layer.Value()->Arrange(viewport));
        auto bounds=progress.GetCachedGeometry();
        REQUIRE(bounds);
        CHECK(bounds.Value().render.tx==14 && bounds.Value().render.ty==17);
        CHECK(bounds.Value().size.x==40 && bounds.Value().size.y==20);
        REQUIRE(screen.RemoveFromParent(objects));
        CHECK(!screen.IsInViewport() && layer.Value()->ChildCount()==0);
    }
    REQUIRE(progress.RemoveFromParent(objects));
    CHECK(!parent.HasAnyChildren());
    CHECK(progress.GetParent(objects).Value().IsNull());
}
} // namespace rocks::ui
