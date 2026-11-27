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
    REQUIRE(button.OnPressed().TryAddLambda([&]() noexcept { ++presses; }));
    REQUIRE(button.OnReleased().TryAddLambda([&]() noexcept { ++releases; }));
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

TEST(UiWidget_WidgetEventsReachEverySubscriberAndDropLateElementEvents) {
    // UW9f(UiToolkit 21.5a절): Widget 사건은 리플렉션 Event다 — 구독이 여럿이면 모두 받고(UE
    // multicast OnClicked), 해제된 Element가 뒤늦게 보낸 사건은 재구성된 Widget에 가지 않는다.
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x554957, 99});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto treeRef = objects.NewObject<WidgetTree>(access, GeneratedType<WidgetTree>::Id());
    REQUIRE(treeRef);
    auto owned = objects.ResolveForWrite<WidgetTree>(access, treeRef.Value().Handle());
    REQUIRE(owned);
    auto buttonRef = owned.Value().Get().Value()->ConstructWidget(
        objects, GeneratedType<ButtonWidget>::Id(), "Button");
    REQUIRE(buttonRef);
    auto b = objects.ResolveForWrite<ButtonWidget>(access, buttonRef.Value().Handle());
    REQUIRE(b);
    auto& button = *b.Value().Get().Value();
    uint32 first = 0, second = 0;
    REQUIRE(button.OnClicked().TryAddLambda([&first]() noexcept { ++first; }));
    REQUIRE(button.OnClicked().TryAddLambda([&second]() noexcept { ++second; }));
    const auto click = [](Element& element) noexcept {
        (void)element.SendKey(0, input::InputEvent::KeyDown(input::Key::Enter));
        (void)element.SendKey(0, input::InputEvent::KeyUp(input::Key::Enter));
    };
    auto original = button.TakeElement(objects);
    REQUIRE(original);
    click(*original.Value());
    CHECK(first == 1 && second == 1);
    // 다시 만든 뒤 옛 Element의 클릭은 버린다.
    button.ReleaseElements();
    auto rebuilt = button.TakeElement(objects);
    REQUIRE(rebuilt);
    click(*original.Value());
    CHECK(first == 1 && second == 1);
    click(*rebuilt.Value());
    CHECK(first == 2 && second == 2);
}

} // namespace rocks::ui
