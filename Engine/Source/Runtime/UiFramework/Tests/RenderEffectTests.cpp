#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/DrawBatch.h>
#include <UiFramework/Effects/BackgroundBlur.h>
#include <UiFramework/Layout/Frame.h>
#include <UiFramework/Effects/PostBufferUpdate.h>
#include <UiFramework/Effects/RetainerBox.h>
#include <UiFramework/Effects/Viewport.h>

using namespace rocks;
using namespace rocks::ui;

TEST(UiFramework_BackdropEffectsPreservePaintOrderAndSeparateBatches) {
    SystemAllocator allocator;
    BackgroundBlur blur(allocator);
    PostBufferUpdate capture(allocator);
    Viewport scene(allocator);
    const auto area = Geometry::Root({32, 16}, {});
    REQUIRE(blur.Arrange(area));
    REQUIRE(capture.Arrange(area));
    REQUIRE(scene.Arrange(area));
    REQUIRE(!blur.SetStrength(-1));
    REQUIRE(blur.SetStrength(4));
    const SlotHandle target{3, 1};
    capture.SetDestination(target);
    scene.SetSceneTexture(target);
    PaintList list(allocator);
    REQUIRE(blur.Paint(list, 0));
    REQUIRE(capture.Paint(list, 0));
    REQUIRE(scene.Paint(list, 0));
    DrawBatch batch(allocator);
    REQUIRE(batch.Build(list));
    REQUIRE(batch.Commands().Size() == 4);
    CHECK(batch.Commands()[0].operation == PaintOperation::BlurBackdrop);
    CHECK(batch.Vertices()[0].uv.x == 4);
    CHECK(batch.Commands()[1].operation == PaintOperation::CopyBackdrop);
    CHECK(batch.Commands()[1].resource == target);
    CHECK(batch.Commands()[2].resource.IsNull());
    CHECK(batch.Commands()[3].resource == target);
    REQUIRE(blur.SetStrength(0));
    capture.SetDestination({});
    list.Clear();
    REQUIRE(blur.Paint(list, 0));
    REQUIRE(capture.Paint(list, 0));
    CHECK(list.Boxes().IsEmpty());
}

TEST(UiFramework_RetainedTextureReplacesOnlyChildPainting) {
    SystemAllocator allocator;
    RetainerBox box(allocator);
    auto child = SharedPtr<Frame>::TryMake(allocator, allocator);
    REQUIRE(child);
    REQUIRE(box.TryAddChild(child.Value()));
    REQUIRE(box.Arrange(Geometry::Root({30, 10}, {1, 0, 0, 1, 6, 7})));
    PaintList offscreen(allocator);
    REQUIRE(box.RecordContent(offscreen));
    REQUIRE(offscreen.Boxes().Size() == 1);
    CHECK(offscreen.Boxes()[0].geometry.render.tx == 0);
    CHECK(offscreen.Boxes()[0].geometry.render.ty == 0);
    CHECK(!box.SetPhase(2, 2));
    REQUIRE(box.SetPhase(1, 2));
    CHECK(box.NeedsCapture(0));
    const SlotHandle target{9, 1};
    box.SetRetainedTexture(target);
    CHECK(!box.NeedsCapture(0));
    CHECK(box.NeedsCapture(1));
    CHECK(box.HitTest({8, 9}) == child.Value().Get());
    PaintList painted(allocator);
    REQUIRE(box.Paint(painted, 0));
    REQUIRE(painted.Boxes().Size() == 1);
    CHECK(painted.Boxes()[0].resource == target);
    box.RequestRender();
    CHECK(box.NeedsCapture(0));
    box.SetRetainedTexture({});
    painted.Clear();
    REQUIRE(box.Paint(painted, 0));
    REQUIRE(painted.Boxes().Size() == 1);
    CHECK(painted.Boxes()[0].resource.IsNull());
}

TEST(UiFramework_DeclarativeBlurBindsItsStrengthWithoutChangingLayout) {
    SystemAllocator allocator;
    ElementBuildScope scope(allocator);
    struct Values {
        float radius = 2;
    } values;
    auto root = Build<RetainerBox>(
        {.content = Make<BackgroundBlur>({.strength = Bind(&values, &Values::radius)}),
         .phase = 1,
         .phaseCount = 3});
    REQUIRE(root);
    root.Value()->UpdateBoundAttributes();
    REQUIRE(root.Value()->Arrange(Geometry::Root({20, 10}, {})));
    PaintList list(allocator);
    REQUIRE(root.Value()->RecordContent(list));
    REQUIRE(list.Boxes().Size() == 1);
    CHECK(list.Boxes()[0].blurRadius == 2);
    values.radius = 5;
    root.Value()->UpdateBoundAttributes();
    list.Clear();
    REQUIRE(root.Value()->RecordContent(list));
    REQUIRE(list.Boxes().Size() == 1);
    CHECK(list.Boxes()[0].blurRadius == 5);
}
