#include <Core/Memory/SystemAllocator.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <TestSupport/Test.h>
#include <UiWidget/CheckBoxWidget.generated.h>
#include <UiWidget/UserWidget.generated.h>
#include <UiWidget/Widget.generated.h>
#include <UiWidget/WidgetAsset.generated.h>
#include <UiWidget/WidgetEventCatalog.h>
#include <UiWidget/WidgetEventIds.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

TEST(UiWidget_AssetEventSurvivesArchiveAndTargetsEachControllerInstance) {
    constexpr Guid setEnabled{0xa48ee231dc754a11ULL, 0x84ab87788d529380ULL};
    constexpr Guid setVolatile{0x89dec731cce04b22ULL, 0xacbc56a910b21878ULL};
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    REQUIRE(RegisterWidgetTypes(registration));

    auto catalog = WidgetEventCatalog::CreateStandard(allocator);
    REQUIRE(catalog);
    // UserWidget은 Widget의 파생 클래스다. 기반 클래스에 등록한 메서드도 결합한다.
    REQUIRE(
        catalog.Value().RegisterMethod(setEnabled, widgetSignatures::Boolean, &Widget::SetEnabled));
    REQUIRE(catalog.Value().RegisterMethod(setVolatile, widgetSignatures::Boolean,
                                           &Widget::SetForceVolatile));
    CHECK(!catalog.Value().RegisterMethod(setEnabled, widgetSignatures::Boolean,
                                          &Widget::SetEnabled));
    CHECK(!catalog.Value().RegisterMethod({1, 1}, widgetSignatures::Boolean,
                                          &UserWidget::OnConstruct));

    ObjectRegistry editor(allocator, types, {0x554957, 44});
    Array<uint8> bytes(allocator);
    {
        const auto access = editor.OwnerAccess();
        ObjectAccessScope scope(access);
        auto made = editor.NewObject<WidgetAsset>(access, GeneratedType<WidgetAsset>::Id());
        REQUIRE(made);
        auto asset = editor.ResolveForWrite<WidgetAsset>(access, made.Value().Handle());
        REQUIRE(asset);
        auto* blueprint = asset.Value().Get().Value();
        REQUIRE(blueprint->Initialize(editor));
        auto tree = editor.ResolveForWrite<WidgetTree>(access, blueprint->Tree());
        REQUIRE(tree);
        auto* root = tree.Value().Get().Value();
        auto box = root->ConstructWidget(editor, GeneratedType<CheckBoxWidget>::Id(), "toggle");
        REQUIRE(box);
        REQUIRE(root->SetRoot(editor, box.Value().Handle()));
        auto widget = editor.Resolve<Widget>(access, box.Value().Handle());
        REQUIRE(widget);
        const WidgetEventBinding event{widget.Value().Get().Value()->StableId(),
                                       widgetEvents::CheckBoxChanged, setEnabled,
                                       widgetSignatures::Boolean};
        REQUIRE(blueprint->AddEventBinding(event));
        CHECK(!blueprint->AddEventBinding(event));
        auto additional = event;
        additional.methodId = setVolatile;
        REQUIRE(blueprint->AddEventBinding(additional));
        CHECK(!blueprint->Compile(editor, allocator));
        REQUIRE(blueprint->Compile(editor, allocator, &catalog.Value()));
        ObjectGraphSaveOptions options;
        options.externals = GraphExternalPolicy::Include;
        auto saved = SaveObjectGraph(allocator, editor, access, made.Value().Handle(), options);
        REQUIRE(saved);
        bytes = Move(saved.Value());
        additional.eventId = widgetEvents::ButtonClicked;
        REQUIRE(blueprint->AddEventBinding(additional));
        auto invalid = blueprint->Compile(editor, allocator, &catalog.Value());
        REQUIRE(!invalid);
        CHECK(invalid.Error() == ErrorCode::TypeMismatch);
    }

    ObjectRegistry game(allocator, types, {0x554957, 45});
    const auto access = game.OwnerAccess();
    ObjectAccessScope scope(access);
    auto restored = LoadObjectGraph(allocator, game, types, access, bytes.View());
    REQUIRE(restored);
    auto asset = game.ResolveForWrite<WidgetAsset>(access, restored.Value().Handle());
    REQUIRE(asset);
    auto compiled = asset.Value().Get().Value()->Compile(game, allocator, &catalog.Value());
    REQUIRE(compiled);
    WidgetEventCatalog missing(allocator);
    auto failed = compiled.Value().Instantiate(game, {}, "Screen", &missing);
    REQUIRE(!failed);
    CHECK(failed.Error() == ErrorCode::NotFound);
    auto makeScreen = [&]() noexcept -> Result<RootRef<UserWidget>> {
        auto runtime = WidgetEventCatalog::CreateStandard(allocator);
        if (!runtime) {
            return Result<RootRef<UserWidget>>::Failure(runtime.Error());
        }
        auto methods = runtime.Value().RegisterMethod(setEnabled, widgetSignatures::Boolean,
                                                      &Widget::SetEnabled);
        if (methods) {
            methods = runtime.Value().RegisterMethod(setVolatile, widgetSignatures::Boolean,
                                                     &Widget::SetForceVolatile);
        }
        if (!methods) {
            return Result<RootRef<UserWidget>>::Failure(methods.Error());
        }
        return compiled.Value().Instantiate(game, {}, "Screen", &runtime.Value());
    };
    auto screen = makeScreen();
    auto second = compiled.Value().Instantiate(game, {}, "SecondScreen", &catalog.Value());
    REQUIRE(screen && second);
    auto owner = game.ResolveForWrite<UserWidget>(access, screen.Value().Handle());
    auto other = game.Resolve<UserWidget>(access, second.Value().Handle());
    REQUIRE(owner && other);
    auto* controller = owner.Value().Get().Value();
    auto host = controller->TakeElement(game);
    REQUIRE(host);
    auto* toggle = host.Value()->ChildAt(0);
    REQUIRE(toggle);
    controller->SetEnabled(false);
    REQUIRE(toggle->OnKeyEvent(0, input::InputEvent::KeyDown(input::Key::Space)));
    REQUIRE(toggle->OnKeyEvent(0, input::InputEvent::KeyUp(input::Key::Space)));
    CHECK(controller->IsEnabled());
    CHECK(controller->IsForceVolatile());
    REQUIRE(toggle->OnKeyEvent(0, input::InputEvent::KeyDown(input::Key::Space)));
    REQUIRE(toggle->OnKeyEvent(0, input::InputEvent::KeyUp(input::Key::Space)));
    CHECK(!controller->IsEnabled());
    CHECK(!controller->IsForceVolatile());
    CHECK(other.Value().Get().Value()->IsEnabled());
}

} // namespace rocks::ui
