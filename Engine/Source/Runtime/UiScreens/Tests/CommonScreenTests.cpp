#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <TestSupport/Test.h>
#include <UiScreens/ScreenStackWidget.generated.h>
#include <UiScreens/ScreenSwitcherWidget.generated.h>
#include <UiScreens/ScreenTabListWidget.generated.h>
#include <UiScreens/HardwareVisibilityWidget.generated.h>
#include <UiScreens/Screen.generated.h>
#include <UiScreens/ScreenInputRouter.h>
#include <UiWidget/ButtonWidget.generated.h>
#include <UiWidget/WidgetTree.generated.h>
#include <UiWidget/WidgetSwitcherWidget.generated.h>

namespace rocks::ui {

TEST(UiScreens_ReflectedStackRoutesOnlyActiveActionsAndUpdatesDeviceVisibility) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterScreenTypes(modules));
    ObjectRegistry objects(allocator, types, {0x534352, 14});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto stackRoot =
        objects.NewObject<ScreenStackWidget>(access, GeneratedType<ScreenStackWidget>::Id());
    auto hintRoot = objects.NewObject<HardwareVisibilityWidget>(
        access, GeneratedType<HardwareVisibilityWidget>::Id());
    REQUIRE(stackRoot && hintRoot);
    auto stackRef = objects.ResolveForWrite<ScreenStackWidget>(access, stackRoot.Value().Handle());
    auto hintRef =
        objects.ResolveForWrite<HardwareVisibilityWidget>(access, hintRoot.Value().Handle());
    REQUIRE(stackRef && hintRef);
    auto* stack = stackRef.Value().Get().Value();
    auto* hint = hintRef.Value().Get().Value();
    Application app(allocator);
    REQUIRE(stack->BindPlayer(app, 3));
    auto entry = stack->TakeElement(objects);
    REQUIRE(entry);
    app.SetRoot(entry.Value());
    auto activeContainer = stack->NativeContainer();
    ScreenInputRouter router(allocator, app, *activeContainer);
    auto state = router.ObserveDevice();
    REQUIRE(state);
    REQUIRE(hint->SetDeviceMask(2));
    REQUIRE(hint->SetDeviceState(state.Value()));
    auto hintView = hint->TakeElement(objects);
    REQUIRE(hintView);
    CHECK(hintView.Value()->GetVisibility() == Visibility::Collapsed);
    const auto type = ClassRef<Screen>::Of<Screen>();
    auto first = stack->PushScreen(type);
    REQUIRE(first);
    auto page = objects.ResolveForWrite<Screen>(access, first.Value());
    REQUIRE(page);
    constexpr Guid domain{1, 2};
    constexpr Guid action{3, 4};
    page.Value().Get().Value()->SetActionDomain(domain);
    uint32 calls = 0;
    REQUIRE(page.Value().Get().Value()->BindAction(
        action, Delegate<bool()>::CreateLambda([&calls]() noexcept {
            ++calls;
            return true;
        })));
    REQUIRE(router.AddMapping({domain, action, input::Key::A, input::GamepadButton::North}));
    REQUIRE(router.Route(input::InputEvent::KeyDown(input::Key::A)));
    CHECK(calls == 1);
    auto second = stack->PushScreen(type);
    REQUIRE(second && stack->ScreenCount() == 2);
    REQUIRE(router.Route(input::InputEvent::KeyDown(input::Key::A)));
    CHECK(calls == 1);
    REQUIRE(stack->RemoveScreen(second.Value()));
    input::GamepadSnapshot pad{};
    pad.connected = true;
    pad.buttons = 1u << static_cast<uint32>(input::GamepadButton::North);
    REQUIRE(router.Route(input::InputEvent::Gamepad(0, pad)));
    CHECK(calls == 2 && hintView.Value()->GetVisibility() == Visibility::Visible);
    REQUIRE(router.Route(input::InputEvent::KeyDown(input::Key::A)));
    CHECK(hintView.Value()->GetVisibility() == Visibility::Collapsed);
    REQUIRE(stack->ClearScreens());
    CHECK(stack->ActiveScreen().IsNull());
    app.SetRoot({});
}

TEST(UiScreens_NamedTabChangesSwitcherPageAndSkipsDisabledTab) {
    SystemAllocator allocator;
    NamePool names(allocator);
    REQUIRE(names.Initialize());
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterScreenTypes(modules));
    ObjectRegistry objects(allocator, types, {0x534352, 15}, {.names = &names});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto treeRoot = objects.NewObject<WidgetTree>(access, GeneratedType<WidgetTree>::Id());
    REQUIRE(treeRoot);
    auto borrowed = objects.ResolveForWrite<WidgetTree>(access, treeRoot.Value().Handle());
    REQUIRE(borrowed);
    auto* tree = borrowed.Value().Get().Value();
    auto tabs = tree->ConstructWidget(objects, GeneratedType<ScreenTabListWidget>::Id(), "Tabs");
    auto switcher =
        tree->ConstructWidget(objects, GeneratedType<WidgetSwitcherWidget>::Id(), "Switcher");
    auto first = tree->ConstructWidget(objects, GeneratedType<ButtonWidget>::Id(), "FirstPage");
    auto second = tree->ConstructWidget(objects, GeneratedType<ButtonWidget>::Id(), "SecondPage");
    REQUIRE(tabs && switcher && first && second);
    auto tabBorrow = objects.ResolveForWrite<ScreenTabListWidget>(access, tabs.Value().Handle());
    auto pageBorrow =
        objects.ResolveForWrite<WidgetSwitcherWidget>(access, switcher.Value().Handle());
    REQUIRE(tabBorrow && pageBorrow);
    auto* bar = tabBorrow.Value().Get().Value();
    auto* pages = pageBorrow.Value().Get().Value();
    auto a = names.TryIntern("Options");
    auto b = names.TryIntern("Controls");
    REQUIRE(a && b);
    REQUIRE(bar->SetLinkedSwitcher(objects, switcher.Value().Handle()));
    REQUIRE(bar->RegisterTab(objects, a.Value(), first.Value().Handle()));
    REQUIRE(bar->RegisterTab(objects, b.Value(), second.Value().Handle()));
    REQUIRE(bar->SetButtonFactory(
        objects, ScreenTabListWidget::ButtonFactory::CreateLambda([&allocator](Name) noexcept {
            auto button = SharedPtr<Button>::TryMake(allocator, allocator);
            if (button) {
                button.Value()->SetDesiredSize({30, 20});
            }
            return button;
        })));
    auto view = bar->TakeElement(objects);
    REQUIRE(view);
    Application app(allocator);
    app.SetRoot(view.Value());
    REQUIRE(app.UpdatePaintCache(Geometry::Root({70, 20}, {}), 1));
    REQUIRE(app.RoutePointer({PointerEventType::Down, {45, 10}, 0}));
    REQUIRE(app.RoutePointer({PointerEventType::Up, {45, 10}, 0}));
    CHECK(pages->ActiveIndex() == 1 && bar->SelectedTab(objects) == b.Value());
    uint32 selectedEvents = 0;
    bar->SetOnTabSelected(
        Delegate<void(Name)>::CreateLambda([&selectedEvents](Name) noexcept { ++selectedEvents; }));
    REQUIRE(pages->SetActiveIndex(0));
    REQUIRE(pages->SetActiveIndex(1));
    CHECK(selectedEvents == 2 && bar->SelectedTab(objects) == b.Value());
    REQUIRE(bar->SetTabEnabled(a.Value(), false));
    REQUIRE(bar->StepTab(objects, true));
    CHECK(pages->ActiveIndex() == 1);
    REQUIRE(bar->SetTabEnabled(a.Value(), true));
    REQUIRE(bar->StepTab(objects, true));
    CHECK(pages->ActiveIndex() == 0);
    REQUIRE(bar->RemoveTab(objects, b.Value()));
    CHECK(pages->ChildCount() == 1 && bar->Tabs().Size() == 1);
    app.SetRoot({});
}

TEST(UiScreens_SwitcherRestoresExistingScreensByIndex) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterScreenTypes(modules));
    ObjectRegistry objects(allocator, types, {0x534352, 16});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto model =
        objects.NewObject<ScreenSwitcherWidget>(access, GeneratedType<ScreenSwitcherWidget>::Id());
    REQUIRE(model);
    auto pageModel = objects.ResolveForWrite<ScreenSwitcherWidget>(access, model.Value().Handle());
    REQUIRE(pageModel);
    auto* pages = pageModel.Value().Get().Value();
    Application app(allocator);
    REQUIRE(pages->BindPlayer(app, 2));
    auto view = pages->TakeElement(objects);
    REQUIRE(view);
    app.SetRoot(view.Value());
    const auto type = ClassRef<Screen>::Of<Screen>();
    auto first = pages->PushScreen(type);
    auto second = pages->PushScreen(type);
    REQUIRE(first && second);
    CHECK(pages->ActiveScreen() == first.Value());
    REQUIRE(pages->SetActivePage(1));
    CHECK(pages->ActiveScreen() == second.Value() && pages->ScreenCount() == 2);
    REQUIRE(pages->RemoveScreen(first.Value()));
    CHECK(pages->ActiveScreen() == second.Value() && pages->ScreenCount() == 1);
    REQUIRE(pages->ClearScreens());
    app.SetRoot({});
}

} // namespace rocks::ui
