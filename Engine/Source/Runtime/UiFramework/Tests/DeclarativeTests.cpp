#include <Core/Memory/SystemAllocator.h>
#include <UiFramework/Layout/VerticalStack.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/Button.h>
#include <UiFramework/Layout/StackPanel.h>

namespace rocks::ui {

TEST(UiFramework_DeclarativeBindingsAndChildFailure) {
    SystemAllocator allocator;
    struct Model {
        LinearColor tint{1, 0, 0, 1};
        uint32 clicks = 0;
        void Click() noexcept {
            ++clicks;
        }
    };
    auto model = SharedPtr<Model>::TryMake(allocator);
    REQUIRE(model);
    CHECK(!Build<Frame>({}));
    ElementBuildScope scope(allocator);
    auto button = Build<Button>({.color = Bind(model.Value(), &Model::tint),
                                 .onClicked = Bind(model.Value(), &Model::Click)});
    REQUIRE(button);
    const StackPanel::SlotArgs slots[] = {{.size = Fill(1), .content = button},
                                          {.size = Auto, .padding = 2, .content = Make<Frame>({})}};
    auto panel = Build<VerticalStack>({.slots = slots});
    REQUIRE(panel);
    Application app(allocator);
    app.SetRoot(panel.Value().AsSharedPtr());
    auto viewport = Geometry::Root({140, 80}, {});
    REQUIRE(app.UpdatePaintCache(viewport, 1));
    CHECK(button.Value()->GetColor() == model.Value()->tint);
    auto idle = app.UpdatePaintCache(viewport, 1);
    REQUIRE(idle);
    CHECK(idle.Value()->LastUpdate().paintCalls == 0);
    model.Value()->tint = {0, 1, 0, 1};
    REQUIRE(app.UpdatePaintCache(viewport, 1));
    CHECK(button.Value()->GetColor() == model.Value()->tint);
    REQUIRE(app.RoutePointer({PointerEventType::Down, {14, 10}}));
    REQUIRE(app.RoutePointer({PointerEventType::Up, {14, 10}}));
    CHECK(model.Value()->clicks == 1);
    model.Value().Reset();
    REQUIRE(app.UpdatePaintCache(viewport, 1));
    CHECK(button.Value()->GetColor() == LinearColor(0, 1, 0, 1));

    auto detached = Build<Frame>({});
    REQUIRE(detached);
    const StackPanel::SlotArgs invalid[] = {{.content = detached},
                                            {.content = Make<Frame>({.padding = {-1, 0, 0, 0}})}};
    auto rejected = Build<VerticalStack>({.slots = invalid});
    REQUIRE(!rejected && rejected.Error() == ErrorCode::InvalidArgument);
    CHECK(detached.Value()->Parent() == nullptr);
}
} // namespace rocks::ui
