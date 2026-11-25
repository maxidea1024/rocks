#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application.h>
#include <UiWidget/WidgetPool.h>

#include <UiWidget/ProgressBarWidget.generated.h>
#include <UiWidget/PanelWidget.generated.h>
#include <UiWidget/OverlayWidget.generated.h>
#include <UiWidget/WidgetTree.generated.h>
#include <UiWidget/UserWidget.generated.h>

namespace rocks::ui {

TEST(UiWidget_ScreenKeepsReflectedTreeAliveUntilDetached) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x554957, 1});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    Application app(allocator);
    ObjectHandle screenHandle, panelHandle, progressHandle;
    {
        auto screen = objects.NewObject<UserWidget>(access, GeneratedType<UserWidget>::Id());
        REQUIRE(screen);
        screenHandle = screen.Value().Handle();
        auto screenBorrow = objects.ResolveForWrite<UserWidget>(access, screenHandle);
        REQUIRE(screenBorrow);
        auto* user = screenBorrow.Value().Get().Value();
        REQUIRE(user->Initialize(objects));
        auto treeBorrow = objects.ResolveForWrite<WidgetTree>(access, user->Tree());
        REQUIRE(treeBorrow);
        auto* tree = treeBorrow.Value().Get().Value();
        auto panel = tree->ConstructWidget(objects, GeneratedType<OverlayWidget>::Id(), "Overlay");
        auto bar = tree->ConstructWidget(objects, GeneratedType<ProgressBarWidget>::Id(), "Health");
        REQUIRE(panel && bar);
        panelHandle = panel.Value().Handle();
        progressHandle = bar.Value().Handle();
        REQUIRE(tree->SetRoot(objects, panelHandle));
        auto box = objects.ResolveForWrite<PanelWidget>(access, panelHandle);
        REQUIRE(box);
        REQUIRE(box.Value().Get().Value()->AddChild(objects, progressHandle));
        CHECK(!box.Value().Get().Value()->AddChild(objects, progressHandle));
        CHECK(!box.Value().Get().Value()->AddChild(objects, panelHandle));
        auto root = user->TakeElement(objects);
        REQUIRE(root);
        app.SetRoot(root.Value());
    }

    REQUIRE(objects.Collect(access));
    {
        auto bar = objects.ResolveForWrite<ProgressBarWidget>(access, progressHandle);
        REQUIRE(bar);
        REQUIRE(bar.Value().Get().Value()->SetValue(0.5f));
        auto cache = app.UpdatePaintCache(Geometry::Root({120, 40}, {}), 1);
        REQUIRE(cache);
        REQUIRE(cache.Value()->List().Boxes().Size() == 2);
        CHECK(cache.Value()->List().Boxes()[1].geometry.size.x == 60);
    }
    app.SetRoot({});
    REQUIRE(objects.Collect(access));
    CHECK(!objects.Resolve<UserWidget>(access, screenHandle));
    CHECK(!objects.Resolve<OverlayWidget>(access, panelHandle));
    CHECK(!objects.Resolve<ProgressBarWidget>(access, progressHandle));
}

TEST(UiWidget_PoolReusesObjectAndControlsElementLifetime) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x554957, 2});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    WidgetPool pool(objects, GeneratedType<UserWidget>::Id());

    auto first = pool.Acquire();
    auto second = pool.Acquire();
    REQUIRE(first && second);
    CHECK(pool.ActiveCount() == 2);
    REQUIRE(pool.Release(first.Value().widget, true));
    CHECK(pool.InactiveCount() == 1);
    REQUIRE(objects.Collect(access));
    auto cached = pool.Acquire();
    REQUIRE(cached);
    CHECK(cached.Value().widget == first.Value().widget);
    CHECK(cached.Value().element.Get() == first.Value().element.Get());

    REQUIRE(pool.Release(cached.Value().widget, false));
    auto rebuilt = pool.Acquire();
    REQUIRE(rebuilt);
    CHECK(rebuilt.Value().widget == first.Value().widget);
    CHECK(rebuilt.Value().element.Get() != first.Value().element.Get());
    REQUIRE(pool.Reset());
    REQUIRE(objects.Collect(access));
    CHECK(!objects.Resolve<UserWidget>(access, first.Value().widget));
    CHECK(!objects.Resolve<UserWidget>(access, second.Value().widget));
}

} // namespace rocks::ui
