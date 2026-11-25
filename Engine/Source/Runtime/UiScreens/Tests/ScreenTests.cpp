#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiScreens/ScreenInputRouter.h>
#include <UiScreens/Screen.generated.h>
#include <UiWidget/WidgetAsset.h>
#include <UiWidget/WidgetAsset.generated.h>

#include <UiWidget/WidgetTree.generated.h>
#include <UiWidget/UserWidget.generated.h>
#include <UiWidget/SliderWidget.generated.h>

namespace rocks::ui {

TEST(UiScreens_StackQueuePoolAndBackArePlayerScoped) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterScreenTypes(modules));
    ObjectRegistry objects(allocator, types, {0x534352, 1});
    ObjectAccessScope scope(objects.OwnerAccess());
    Application app(allocator);
    auto stack = SharedPtr<ScreenStack>::TryMake(allocator, objects, app, 7u);
    REQUIRE(stack);
    app.SetRoot(stack.Value());
    ScreenInputRouter router(allocator, app, *stack.Value());
    auto first = stack.Value()->AddScreen(ClassRef<Screen>::Of<Screen>());
    auto second = stack.Value()->AddScreen(ClassRef<Screen>::Of<Screen>());
    REQUIRE(first && second);
    CHECK(stack.Value()->ActiveScreen() == second.Value());
    REQUIRE(router.Route(input::InputEvent::KeyDown(input::Key::Escape)).Value());
    CHECK(stack.Value()->ActiveScreen() == first.Value());
    auto reused = stack.Value()->AddScreen(ClassRef<Screen>::Of<Screen>());
    REQUIRE(reused);
    CHECK(reused.Value() == second.Value());
    input::GamepadSnapshot pad{};
    pad.connected = true;
    pad.buttons = 1u << static_cast<uint32>(input::GamepadButton::East);
    REQUIRE(router.Route(input::InputEvent::Gamepad(0, pad)).Value());
    REQUIRE(router.Route(input::InputEvent::Gamepad(0, pad)).Value());
    CHECK(router.Device() == ScreenInputDevice::Gamepad);
    CHECK(stack.Value()->ActiveScreen() == first.Value());
    REQUIRE(stack.Value()->RemoveScreen(first.Value()));
    CHECK(!router.Route(input::InputEvent::KeyDown(input::Key::Escape)).Value());
    auto queue = SharedPtr<ScreenQueue>::TryMake(allocator, objects, app, 7u);
    REQUIRE(queue);
    app.SetRoot(queue.Value());
    auto a = queue.Value()->AddScreen(ClassRef<Screen>::Of<Screen>());
    auto b = queue.Value()->AddScreen(ClassRef<Screen>::Of<Screen>());
    REQUIRE(a && b);
    CHECK(queue.Value()->ActiveScreen() == a.Value());
    REQUIRE(queue.Value()->Back().Value());
    CHECK(queue.Value()->ActiveScreen() == b.Value());
    app.SetRoot({});
}

TEST(UiScreens_AssetBindsIndependentInstancesAndRestoresFocus) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterScreenTypes(modules));
    ObjectRegistry source(allocator, types, {0x534352, 2});
    ObjectRegistry game(allocator, types, {0x534352, 3});
    Guid targetId{};
    WidgetTemplate compiled(allocator);
    {
        const auto access = source.OwnerAccess();
        ObjectAccessScope scope(access);
        auto asset = source.NewObject<WidgetAsset>(access, GeneratedType<WidgetAsset>::Id());
        REQUIRE(asset);
        auto editor = source.ResolveForWrite<WidgetAsset>(access, asset.Value().Handle());
        REQUIRE(editor);
        auto* design = editor.Value().Get().Value();
        design->SetController(ClassRef<UserWidget>::Of<Screen>());
        REQUIRE(design->Initialize(source));
        auto tree = source.ResolveForWrite<WidgetTree>(access, design->Tree());
        REQUIRE(tree);
        auto* hierarchy = tree.Value().Get().Value();
        auto bar = hierarchy->ConstructWidget(source, GeneratedType<SliderWidget>::Id(), "Volume");
        REQUIRE(bar);
        REQUIRE(hierarchy->SetRoot(source, bar.Value().Handle()));
        auto value = source.ResolveForWrite<SliderWidget>(access, bar.Value().Handle());
        REQUIRE(value);
        REQUIRE(value.Value().Get().Value()->SetValue(0.5f));
        targetId = value.Value().Get().Value()->StableId();
        Guid focusField{};
        for (const auto& field : GeneratedType<Screen>::Schema().fields) {
            if (StringView(field.toolName.Data(), field.toolName.Size()) == "focusTarget") {
                focusField = field.id;
            }
        }

        REQUIRE(focusField != Guid{});
        REQUIRE(design->AddBinding({focusField, targetId, false}));
        auto built = design->Compile(source, allocator);
        REQUIRE(built);
        compiled = Move(built.Value());
    }

    {
        const auto access = game.OwnerAccess();
        ObjectAccessScope scope(access);
        auto first = compiled.Instantiate(game);
        auto second = compiled.Instantiate(game);
        REQUIRE(first && second);
        auto a = game.ResolveForWrite<Screen>(access, first.Value().Handle());
        auto b = game.ResolveForWrite<Screen>(access, second.Value().Handle());
        REQUIRE(a && b);
        auto* firstScreen = a.Value().Get().Value();
        auto* secondScreen = b.Value().Get().Value();
        CHECK(firstScreen->FocusTarget() != secondScreen->FocusTarget());
        auto slider = game.ResolveForWrite<SliderWidget>(access, firstScreen->FocusTarget());
        auto other = game.Resolve<SliderWidget>(access, secondScreen->FocusTarget());
        REQUIRE(slider && other);
        CHECK(slider.Value().Get().Value()->StableId() == targetId);
        REQUIRE(slider.Value().Get().Value()->SetValue(0.25f));
        CHECK(other.Value().Get().Value()->Value() == 0.5f);
        Application app(allocator);
        auto stack = SharedPtr<ScreenStack>::TryMake(allocator, game, app, 3u);
        REQUIRE(stack);
        app.SetRoot(stack.Value());
        REQUIRE(stack.Value()->AddScreen(first.Value().Handle()));
        auto original = app.GetFocus(3);
        REQUIRE(original);
        CHECK(!app.GetFocus(0));
        REQUIRE(stack.Value()->AddScreen(second.Value().Handle()));
        CHECK(firstScreen->IsActive() == false && secondScreen->IsActive());
        REQUIRE(secondScreen->Dismiss());
        CHECK(app.GetFocus(3).Get() == original.Get());
        app.SetRoot({});
    }
}

} // namespace rocks::ui
