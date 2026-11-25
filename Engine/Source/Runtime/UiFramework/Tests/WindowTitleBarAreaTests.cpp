#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application.h>
#include <UiFramework/Button.h>
#include <UiFramework/WindowTitleBarArea.h>

namespace rocks::ui {

TEST(UiFramework_TitleBarDistinguishesCaptionAndInteractiveChild) {
    SystemAllocator allocator;
    auto area = SharedPtr<WindowTitleBarArea>::TryMake(allocator, allocator);
    auto button = SharedPtr<Button>::TryMake(allocator, allocator);
    REQUIRE(area && button);
    REQUIRE(area.Value()->SetPadding({10, 5, 10, 5}));
    REQUIRE(area.Value()->TryAddChild(button.Value()));
    Application app(allocator);
    app.SetRoot(area.Value());
    const auto geometry = Geometry::Root({100, 40}, {});
    REQUIRE(app.UpdatePaintCache(geometry, 1));
    CHECK(app.QueryWindowRegion({3, 3}) == input::WindowRegion::Caption);
    CHECK(app.QueryWindowRegion({20, 20}) == input::WindowRegion::Client);
    area.Value()->SetDoubleClickTogglesFullscreen(true);
    CHECK(app.QueryWindowRegion({3, 3}) == input::WindowRegion::FullscreenCaption);
    CHECK(app.QueryWindowRegion({103, 3}) == input::WindowRegion::Client);
}
} // namespace rocks::ui
