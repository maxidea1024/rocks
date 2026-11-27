#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/Button.h>
#include <UiFramework/Window/WindowTitleBarArea.h>

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
    CHECK(app.QueryWindowRegion({103, 3}) == input::WindowRegion::Client);
}

TEST(UiFramework_TitleBarInFullscreenIsClientAndTogglesOnDoubleClick) {
    SystemAllocator allocator;
    auto area = SharedPtr<WindowTitleBarArea>::TryMake(allocator, allocator);
    REQUIRE(area);
    int toggles = 0;
    area.Value()->SetDoubleClickTogglesFullscreen(true);
    area.Value()->SetOnRequestToggleFullscreen(
        Delegate<void()>::CreateLambda([&toggles]() noexcept { ++toggles; }));
    Application app(allocator);
    app.SetRoot(area.Value());
    REQUIRE(app.UpdatePaintCache(Geometry::Root({100, 40}, {}), 1));
    // 창 모드: 끌기 제목이고, 두 번 누르기는 OS가 처리하므로 위젯은 무시한다.
    PointerEvent twice{PointerEventType::Down, {3, 3}};
    twice.clickCount = 2;
    CHECK(app.QueryWindowRegion({3, 3}) == input::WindowRegion::Caption);
    CHECK(!area.Value()->OnPointerEvent(twice).handled);
    // 전체 화면: Client로 답해 끌어 옮기지 않고, 두 번 누르기만 전환을 요청한다(UE
    // SWindowTitleBarArea — 19.2절).
    area.Value()->SetFullscreen(true);
    CHECK(app.QueryWindowRegion({3, 3}) == input::WindowRegion::Client);
    PointerEvent once{PointerEventType::Down, {3, 3}};
    CHECK(!area.Value()->OnPointerEvent(once).handled);
    CHECK(toggles == 0);
    CHECK(area.Value()->OnPointerEvent(twice).handled);
    CHECK(toggles == 1);
    // 전환이 꺼져 있으면 두 번 누르기도 무시한다(UE bDoubleClickTogglesFullscreen).
    area.Value()->SetDoubleClickTogglesFullscreen(false);
    CHECK(!area.Value()->OnPointerEvent(twice).handled);
    CHECK(toggles == 1);
}
} // namespace rocks::ui
