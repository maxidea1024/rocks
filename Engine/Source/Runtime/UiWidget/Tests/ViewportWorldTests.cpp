#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Viewport.h>
#include <UiWidget/ViewportWidget.generated.h>
#include <World/Actor.generated.h>

namespace rocks::ui {

TEST(UiWidget_ViewportsOwnIndependentWorldsAndRetainCameraAcrossRebuilds) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    REQUIRE(RegisterWidgetTypes(registration));
    ObjectRegistry objects(allocator, types, {0x554957, 92});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto first =
        objects.NewObject<ViewportWidget>(access, GeneratedType<ViewportWidget>::Id(), {}, "First");
    auto second = objects.NewObject<ViewportWidget>(access, GeneratedType<ViewportWidget>::Id(), {},
                                                    "Second");
    REQUIRE(first && second);
    auto a = objects.ResolveForWrite<ViewportWidget>(access, first.Value().Handle());
    auto b = objects.ResolveForWrite<ViewportWidget>(access, second.Value().Handle());
    REQUIRE(a && b);
    auto& view = *a.Value().Get().Value();
    auto firstLease = view.AcquireViewportWorld(objects);
    auto secondLease = b.Value().Get().Value()->AcquireViewportWorld(objects);
    REQUIRE(firstLease && secondLease);
    auto left = view.GetViewportWorld(objects);
    auto right = b.Value().Get().Value()->GetViewportWorld(objects);
    REQUIRE(left && right && left.Value() != right.Value());
    auto actor = view.Spawn(objects, GeneratedType<Actor>::Id());
    REQUIRE(actor);
    uint32 count = 0;
    left.Value()->ForEachActor([&](ObjectHandle h) noexcept {
        if (h == actor.Value()) {
            ++count;
        }
    });
    right.Value()->ForEachActor([&](ObjectHandle) noexcept { ++count; });
    CHECK(count == 1);
    REQUIRE(view.SetCamera({-4, 0, 0}, Quat::Identity(), 75));
    CHECK(!view.SetCamera({}, Quat::Identity(), 180));
    auto built = view.TakeElement(objects);
    REQUIRE(built);
    auto source = static_cast<Viewport&>(*built.Value()).SceneSource();
    REQUIRE(source);
    auto* preview = static_cast<ViewportWorld*>(source.Get());
    auto camera = preview->Camera(120, 60);
    REQUIRE(camera);
    auto center = camera.Value().view.TransformPoint({});
    CHECK(center == Vec3(0, 0, 4));
    CHECK(!preview->Camera(0, 1));
    REQUIRE(preview->Advance(8));
    REQUIRE(preview->Advance(8));
    CHECK(left.Value()->State() == WorldState::Playing);
    CHECK(right.Value()->State() == WorldState::Initialized);
    view.ReleaseElements();
    auto rebuilt = view.TakeElement(objects);
    REQUIRE(rebuilt);
    CHECK(static_cast<Viewport&>(*rebuilt.Value()).SceneSource() == source);
}

} // namespace rocks::ui
