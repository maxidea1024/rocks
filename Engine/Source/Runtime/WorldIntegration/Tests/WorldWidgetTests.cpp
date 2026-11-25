#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiWidget/ButtonWidget.generated.h>
#include <UiWidget/UserWidget.generated.h>
#include <UiWidget/WidgetTree.generated.h>
#include <World/Actor.generated.h>
#include <WorldIntegration/WorldWidgetComponent.generated.h>
#include <WorldIntegration/WorldWidgetInteraction.h>

namespace rocks::ui {

TEST(WorldIntegration_WidgetRayUsesPivotPlayerCaptureAndProjectedPlayerLayer) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWorldIntegrationTypes(modules));
    WorldDesc desc;
    desc.name = "WorldWidgetTests";
    desc.context = {0x574944, 1};
    World world(allocator, types, desc);
    REQUIRE(world.Initialize());
    {
        auto& objects = world.Objects();
        const auto access = world.Access();
        ObjectAccessScope scope(access);
        auto actor = world.SpawnActor<Actor>();
        REQUIRE(actor);
        auto added = world.AddComponent<WorldWidgetComponent>(actor.Value());
        REQUIRE(added);
        auto component = objects.ResolveForWrite<WorldWidgetComponent>(access, added.Value());
        REQUIRE(component);
        auto& widget = *component.Value().Get().Value();
        REQUIRE(widget.SetDrawSize({40, 20}));
        REQUIRE(widget.SetPivot({0.25f, 0.5f}));
        REQUIRE(widget.SetRelativeLocation({3, 0, 0}));
        REQUIRE(widget.SetRelativeScale({1, 2, 1}));
        auto hit = widget.RayToLocal({7, 4, 1}, {-2, 0, 0});
        REQUIRE(hit);
        CHECK(hit.Value() == Vec2(8, 9));
        CHECK(!widget.RayToLocal({7, 4, 1}, {0, 1, 0}));
        CHECK(!widget.RayToLocal({7, 4, 1}, {2, 0, 0}));
        auto owner = objects.NewObject<UserWidget>(access, GeneratedType<UserWidget>::Id());
        REQUIRE(owner);
        auto user = objects.ResolveForWrite<UserWidget>(access, owner.Value().Handle());
        REQUIRE(user);
        REQUIRE(user.Value().Get().Value()->Initialize(objects));
        auto tree = objects.ResolveForWrite<WidgetTree>(access, user.Value().Get().Value()->Tree());
        REQUIRE(tree);
        auto child = tree.Value().Get().Value()->ConstructWidget(
            objects, GeneratedType<ButtonWidget>::Id(), "Button");
        REQUIRE(child);
        REQUIRE(tree.Value().Get().Value()->SetRoot(objects, child.Value().Handle()));
        auto button = objects.ResolveForWrite<ButtonWidget>(access, child.Value().Handle());
        REQUIRE(button);
        uint32 clicked = 0;
        button.Value().Get().Value()->SetOnClicked(
            Delegate<void()>::CreateLambda([&]() noexcept { ++clicked; }));
        REQUIRE(widget.SetWidget(owner.Value().Handle()));
        const ObjectHandle hits[] = {added.Value()};
        WorldWidgetInteraction pointer(2, 7), other(3, 7);
        auto down = pointer.Route(world, hits, {6, 0, 0}, {-1, 0, 0}, PointerEventType::Down);
        REQUIRE(down && down.Value());
        auto stray = other.Route(world, hits, {6, 0, 0}, {-1, 0, 0}, PointerEventType::Up);
        REQUIRE(stray);
        CHECK(clicked == 0);
        auto released = pointer.Route(world, {}, {6, 0, 0}, {-1, 0, 0}, PointerEventType::Up);
        REQUIRE(released && released.Value());
        CHECK(clicked == 1);
        widget.SetScreenSpace(true, 2);
        REQUIRE(widget.SetRelativeLocation({0, 0, 0.5}));
        auto layer = SharedPtr<AnchorCanvas>::TryMake(allocator, allocator);
        REQUIRE(layer);
        auto wrong = widget.ProjectToPlayer(3, Mat4d::Identity(), {100, 80}, layer.Value());
        REQUIRE(wrong);
        CHECK(!wrong.Value());
        auto placed = widget.ProjectToPlayer(2, Mat4d::Identity(), {100, 80}, layer.Value());
        REQUIRE(placed && placed.Value());
        REQUIRE(layer.Value()->Arrange(Geometry::Root({100, 80}, {})));
        CHECK(widget.WindowRoot()->GetGeometry().render.tx == 40);
        CHECK(widget.WindowRoot()->GetGeometry().render.ty == 30);
        REQUIRE(widget.SetRelativeLocation({0, 0, -1}));
        auto culled = widget.ProjectToPlayer(2, Mat4d::Identity(), {100, 80}, layer.Value());
        REQUIRE(culled);
        CHECK(!culled.Value() && layer.Value()->ChildCount() == 0);
    }

    REQUIRE(world.Shutdown());
}
} // namespace rocks::ui
