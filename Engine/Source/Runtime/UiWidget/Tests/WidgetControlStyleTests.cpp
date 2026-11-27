#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/Styles/GameWidgetStyles.h>
#include <UiFramework/Controls/Button.h>
#include <UiFramework/Controls/Slider.h>
#include <UiWidget/BorderWidget.generated.h>
#include <UiWidget/ButtonWidget.generated.h>
#include <UiWidget/ImageWidget.generated.h>
#include <UiWidget/SliderWidget.generated.h>
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
    REQUIRE(button.SetBackgroundColor({1, 0.5f, 1, 1}));
    CHECK(button.GetBackgroundColor() == LinearColor(1, 0.5f, 1, 1));
    // 기본은 게임 위젯 스타일 "Button"이다(UE DefaultStyleCache·UMGCoreStyle — UiToolkit 20.11절).
    CHECK(button.GetStyle() == GameButtonStyle());
    REQUIRE(button.SetStyle(PrimaryButtonStyle()));
    ButtonStyle bad = DefaultButtonStyle();
    bad.normalPadding = {-1, 0, 0, 0};
    CHECK(!button.SetStyle(bad));
    CHECK(button.GetStyle() == PrimaryButtonStyle());
    uint32 presses = 0, releases = 0;
    button.SetOnPressed(Delegate<void()>::CreateLambda([&]() noexcept { ++presses; }));
    button.SetOnReleased(Delegate<void()>::CreateLambda([&]() noexcept { ++releases; }));
    auto original = button.TakeElement(objects);
    REQUIRE(original);
    REQUIRE(original.Value()->Arrange(Geometry::Root({50, 30}, {})));
    // 버튼 스타일 여백 (12, 1.5, 12, 1.5)과 테두리 여백 (2, 3, 4, 5) 안쪽이다.
    CHECK(image.CachedElement()->GetGeometry().size == Vec2(50 - 24 - 6, 30 - 3 - 8));
    CHECK(StaticElementCast<Button>(*original.Value()).GetStyle() == PrimaryButtonStyle());
    CHECK(StaticElementCast<Button>(*original.Value()).GetColor() == LinearColor(1, 0.5f, 1, 1));
    CHECK(original.Value()->SendKey(0, input::InputEvent::KeyDown(input::Key::Enter)).IsHandled());
    CHECK(button.IsPressed() && presses == 1);
    CHECK(original.Value()->SendKey(0, input::InputEvent::KeyUp(input::Key::Enter)).IsHandled());
    CHECK(!button.IsPressed() && releases == 1);
    button.ReleaseElements();
    auto rebuilt = button.TakeElement(objects);
    REQUIRE(rebuilt);
    CHECK(rebuilt.Value() != original.Value());
    original.Value()->OnMouseEnter(0);
    CHECK(!button.IsHovered());
    CHECK(border.GetBrush().mirrorX && image.GetBrush().imageSize == Vec2(11, 13));
}

TEST(UiWidget_SliderTakesTheGameWidgetSliderStyle) {
    // UE USlider WidgetStyle = DefaultStyleCache의 UMGCoreStyle "Slider"(UiToolkit 20.11절).
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x554957, 95});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto treeRef = objects.NewObject<WidgetTree>(access, GeneratedType<WidgetTree>::Id());
    REQUIRE(treeRef);
    auto owned = objects.ResolveForWrite<WidgetTree>(access, treeRef.Value().Handle());
    REQUIRE(owned);
    auto& tree = *owned.Value().Get().Value();
    auto sliderRef = tree.ConstructWidget(objects, GeneratedType<SliderWidget>::Id(), "Slider");
    REQUIRE(sliderRef);
    auto s = objects.ResolveForWrite<SliderWidget>(access, sliderRef.Value().Handle());
    REQUIRE(s);
    auto live = s.Value().Get().Value()->TakeElement(objects);
    REQUIRE(live);
    CHECK(StaticElementCast<Slider>(*live.Value()).GetStyle() == GameSliderStyle());
}

} // namespace rocks::ui
