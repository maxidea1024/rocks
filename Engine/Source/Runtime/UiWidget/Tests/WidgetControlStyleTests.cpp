#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Button.h>
#include <UiWidget/BorderWidget.generated.h>
#include <UiWidget/ButtonWidget.generated.h>
#include <UiWidget/ImageWidget.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

TEST(UiWidget_ButtonEventsAndBorderImageStyleSurviveRebuilds) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x554957, 94});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto treeRef = objects.NewObject<WidgetTree>(access, GeneratedType<WidgetTree>::Id());
    REQUIRE(treeRef);
    auto owned = objects.ResolveForWrite<WidgetTree>(access, treeRef.Value().Handle());
    REQUIRE(owned);
    auto& tree = *owned.Value().Get().Value();
    auto buttonRef = tree.ConstructWidget(objects, GeneratedType<ButtonWidget>::Id(), "Button");
    auto borderRef = tree.ConstructWidget(objects, GeneratedType<BorderWidget>::Id(), "Border");
    auto imageRef = tree.ConstructWidget(objects, GeneratedType<ImageWidget>::Id(), "Image");
    REQUIRE(buttonRef && borderRef && imageRef);
    auto b = objects.ResolveForWrite<ButtonWidget>(access, buttonRef.Value().Handle());
    auto p = objects.ResolveForWrite<BorderWidget>(access, borderRef.Value().Handle());
    auto i = objects.ResolveForWrite<ImageWidget>(access, imageRef.Value().Handle());
    REQUIRE(b && p && i);
    auto& button = *b.Value().Get().Value();
    auto& border = *p.Value().Get().Value();
    auto& image = *i.Value().Get().Value();
    REQUIRE(border.SetPadding({2, 3, 4, 5}));
    Brush brush{};
    brush.imageSize = {11, 13};
    brush.mirrorX = true;
    REQUIRE(image.SetBrush(brush));
    REQUIRE(image.SetTint({0, 1, 0, 1}));
    CHECK(image.GetBrush().mirrorX && image.GetBrush().tint == image.Tint());
    REQUIRE(border.SetBrush(brush));
    REQUIRE(border.AddChild(objects, imageRef.Value().Handle()));
    REQUIRE(button.AddChild(objects, borderRef.Value().Handle()));
    REQUIRE(button.SetTints({1, 1, 1, 1}, {0, 1, 0, 1}, {1, 0, 0, 1}));
    uint32 presses = 0, releases = 0;
    button.SetOnPressed(Delegate<void()>::CreateLambda([&]() noexcept { ++presses; }));
    button.SetOnReleased(Delegate<void()>::CreateLambda([&]() noexcept { ++releases; }));
    auto original = button.TakeElement(objects);
    REQUIRE(original);
    REQUIRE(original.Value()->Arrange(Geometry::Root({50, 30}, {})));
    CHECK(image.CachedElement()->GetGeometry().size == Vec2(44, 22));
    CHECK(original.Value()->OnKeyEvent(0, input::InputEvent::KeyDown(input::Key::Enter)));
    CHECK(button.IsPressed() && presses == 1);
    CHECK(original.Value()->OnKeyEvent(0, input::InputEvent::KeyUp(input::Key::Enter)));
    CHECK(!button.IsPressed() && releases == 1);
    button.ReleaseElements();
    auto rebuilt = button.TakeElement(objects);
    REQUIRE(rebuilt);
    CHECK(rebuilt.Value() != original.Value());
    original.Value()->OnMouseEnter(0);
    CHECK(!button.IsHovered());
    CHECK(border.GetBrush().mirrorX && image.GetBrush().imageSize == Vec2(11, 13));
}

} // namespace rocks::ui
