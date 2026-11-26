#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Controls/RadialSlider.h>
#include <UiWidget/RadialSliderWidget.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

TEST(UiWidget_RadialSliderUsesAnglesCaptureAndRebuildsSavedValues) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x554957, 1202});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto root = objects.NewObject<WidgetTree>(access, GeneratedType<WidgetTree>::Id());
    REQUIRE(root);
    auto tree = objects.ResolveForWrite<WidgetTree>(access, root.Value().Handle());
    REQUIRE(tree);
    auto entry = tree.Value().Get().Value()->ConstructWidget(
        objects, GeneratedType<RadialSliderWidget>::Id(), "Volume");
    REQUIRE(entry);
    auto widget = objects.ResolveForWrite<RadialSliderWidget>(access, entry.Value().Handle());
    REQUIRE(widget);
    auto* dial = widget.Value().Get().Value();
    REQUIRE(dial->SetLimits(10, 30, 4));
    REQUIRE(dial->SetArc(0, 360, 90));
    CHECK(!dial->SetArc(210, 120));
    dial->SetSnapToStep(true);
    dial->SetHandVisibility(true, true);
    REQUIRE(dial->SetColors({0.2f, 0.2f, 0.2f, 1}, {0, 1, 0, 1}, {1, 0, 0, 1}));
    uint32 changes = 0;
    dial->SetOnChanged(
        Delegate<void(float)>::CreateLambda([&changes](float) noexcept { ++changes; }));
    REQUIRE(dial->SetValue(14));
    auto built = dial->TakeElement(objects);
    REQUIRE(built);
    auto& native = static_cast<RadialSlider&>(*built.Value());
    // 회전 오프셋이 90도이므로 오른쪽 점은 원호의 중간이다. 로컬 좌표에 배율·이동도 적용한다.
    const auto area = Geometry::Root({100, 100}, {2, 0, 0, 2, 15, 10});
    REQUIRE(native.Arrange(area));
    const auto right = area.render.Apply({90, 50});
    const auto reply =
        native.OnPointerEvent({.type = PointerEventType::Down, .position = right, .pointerId = 7});
    CHECK(reply.handled && reply.capture);
    CHECK(dial->Value() == 22);
    CHECK(changes == 1);
    CHECK(!native.OnPointerEvent({.type = PointerEventType::Up, .position = right, .pointerId = 8})
               .handled);
    const auto release =
        native.OnPointerEvent({.type = PointerEventType::Up, .position = right, .pointerId = 7});
    CHECK(release.handled && release.releaseCapture);
    CHECK(native.OnKeyEvent(0, input::InputEvent::KeyDown(input::Key::Down)));
    CHECK(dial->Value() == 18 && changes == 2);

    dial->ReleaseElements();
    auto rebuilt = dial->TakeElement(objects);
    REQUIRE(rebuilt);
    auto& restored = static_cast<RadialSlider&>(*rebuilt.Value());
    CHECK(restored.GetValue() == 18);
    REQUIRE(restored.Arrange(area));
    PaintList paint(allocator);
    REQUIRE(restored.OnPaint(paint, 0));
    REQUIRE(paint.Boxes().Size() > 4);
    CHECK(paint.Boxes()[paint.Boxes().Size() - 1].color == LinearColor(1, 0, 0, 1));
    // 분리된 이전 요소의 입력은 재생성된 객체 값을 변경하지 않는다.
    CHECK(native.OnKeyEvent(0, input::InputEvent::KeyDown(input::Key::End)));
    CHECK(dial->Value() == 18 && changes == 2);
}

TEST(UiWidget_RadialSliderDragKeepsMaximumAcrossAngleSeam) {
    SystemAllocator allocator;
    RadialSlider dial(allocator);
    REQUIRE(dial.SetArc(0, 360));
    REQUIRE(dial.Arrange(Geometry::Root({100, 100}, {})));
    CHECK(dial.OnPointerEvent({.type = PointerEventType::Down, .position = {51, 90}}).capture);
    CHECK(dial.GetValue() > 0.99f);
    CHECK(dial.OnPointerEvent({.type = PointerEventType::Move, .position = {49, 90}}).handled);
    CHECK(dial.GetValue() == 1);
    dial.OnPointerCaptureLost(PointerSource::Mouse, 0, 0);
    CHECK(!dial.OnPointerEvent({.type = PointerEventType::Move, .position = {10, 50}}).handled);
}

} // namespace rocks::ui
