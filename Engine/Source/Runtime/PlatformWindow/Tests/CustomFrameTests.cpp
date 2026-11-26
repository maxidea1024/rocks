#include <Core/Memory/SystemAllocator.h>
#include <PlatformWindow/Window.h>
#include <TestSupport/Test.h>

namespace rocks::platform {

namespace {

constexpr uint32 FrameWidth = 400;
constexpr uint32 FrameHeight = 300;
// 시험용 UI 영역: 위 30 px는 제목, 그 오른쪽 끝 세 칸(42 px)은 닫기·최대화·최소화, 왼쪽 30 px는
// 시스템 메뉴(앱 아이콘)다.
constexpr int32 TitleHeight = 30;
constexpr int32 ButtonWidth = 42;
constexpr int32 IconWidth = 30;

input::WindowRegion TestRegion(IntVec2 point) noexcept {
    if (point.y >= TitleHeight) {
        return input::WindowRegion::Client;
    }

    const int32 fromRight = static_cast<int32>(FrameWidth) - point.x;
    if (fromRight <= ButtonWidth) {
        return input::WindowRegion::CloseButton;
    }

    if (fromRight <= 2 * ButtonWidth) {
        return input::WindowRegion::MaximizeButton;
    }

    if (fromRight <= 3 * ButtonWidth) {
        return input::WindowRegion::MinimizeButton;
    }

    return point.x < IconWidth ? input::WindowRegion::SystemMenu : input::WindowRegion::Caption;
}

Result<UniquePtr<Window>> CreateCustom(Allocator& allocator, bool resizable) noexcept {
    return Window::Create(allocator, {.title = "CustomFrame",
                                      .width = FrameWidth,
                                      .height = FrameHeight,
                                      .resizable = resizable,
                                      .visible = false,
                                      .frame = WindowFrame::Custom});
}

} // namespace

TEST(PlatformWindow_CustomFrameHitZones) {
    // 직접 그린 테두리가 없는 플랫폼은 아래 Unsupported 시험이 맡는다.
    if (!Window::SupportsFrame(WindowFrame::Custom)) {
        return;
    }

    SystemAllocator allocator;
    auto window = CreateCustom(allocator, true);
    REQUIRE(window);
    Window& frame = *window.Value();
    CHECK(frame.Frame() == WindowFrame::Custom && frame.IsResizable());
    // OS 테두리가 없어 client가 창 크기 그대로다.
    CHECK(frame.Width() == FrameWidth && frame.Height() == FrameHeight);
    frame.SetRegionHitTest(Window::RegionHitTest::CreateStatic(&TestRegion));
    const int32 right = static_cast<int32>(FrameWidth) - 1;
    const int32 bottom = static_cast<int32>(FrameHeight) - 1;
    // 크기 조절 가장자리가 UI 영역보다 먼저다(96 DPI 기준 두께 5 이상).
    CHECK(frame.HitZone({0, 0}) == WindowZone::TopLeftBorder);
    CHECK(frame.HitZone({right, 0}) == WindowZone::TopRightBorder);
    CHECK(frame.HitZone({0, bottom}) == WindowZone::BottomLeftBorder);
    CHECK(frame.HitZone({right, bottom}) == WindowZone::BottomRightBorder);
    CHECK(frame.HitZone({0, 150}) == WindowZone::LeftBorder);
    CHECK(frame.HitZone({right, 150}) == WindowZone::RightBorder);
    CHECK(frame.HitZone({200, 0}) == WindowZone::TopBorder);
    CHECK(frame.HitZone({200, bottom}) == WindowZone::BottomBorder);
    // 모서리는 가장자리 안 축에서 다른 축을 두께 + 확장까지 넓혀 잡는다(UE 10 px 모서리).
    CHECK(frame.HitZone({CustomFrameResizeBorder + 2, 0}) == WindowZone::TopLeftBorder);
    CHECK(frame.HitZone({0, CustomFrameResizeBorder + 2}) == WindowZone::TopLeftBorder);
    // 안쪽은 UI 영역이다.
    const int32 y = 2 * CustomFrameResizeBorder + 5;
    CHECK(frame.HitZone({200, y}) == WindowZone::Caption);
    CHECK(frame.HitZone({2 * CustomFrameResizeBorder + 5, y}) == WindowZone::SystemMenu);
    CHECK(frame.HitZone({right - ButtonWidth - 10, y}) == WindowZone::MaximizeButton);
    CHECK(frame.HitZone({right - 2 * ButtonWidth - 10, y}) == WindowZone::MinimizeButton);
    CHECK(frame.HitZone({right - 20, y}) == WindowZone::CloseButton);
    CHECK(frame.HitZone({200, 150}) == WindowZone::Client);
    frame.SetRegionHitTest({});
}

TEST(PlatformWindow_CustomFrameFixedSizeHasNoEdgesOrMaximize) {
    // 직접 그린 테두리가 없는 플랫폼은 아래 Unsupported 시험이 맡는다.
    if (!Window::SupportsFrame(WindowFrame::Custom)) {
        return;
    }

    SystemAllocator allocator;
    auto window = CreateCustom(allocator, false);
    REQUIRE(window);
    Window& frame = *window.Value();
    frame.SetRegionHitTest(Window::RegionHitTest::CreateStatic(&TestRegion));
    // 크기 고정 창은 가장자리 판정이 없고 최대화도 하지 않는다(UE HasMaximizeBox false).
    CHECK(!frame.IsResizable());
    CHECK(frame.HitZone({0, 150}) == WindowZone::Client);
    CHECK(frame.HitZone({200, 0}) == WindowZone::Caption);
    frame.ToggleMaximize();
    CHECK(!frame.IsMaximized());
    frame.SetRegionHitTest({});
}

TEST(PlatformWindow_CustomFrameCloseRequestAndSystemFrameZones) {
    // 직접 그린 테두리가 없는 플랫폼은 아래 Unsupported 시험이 맡는다.
    if (!Window::SupportsFrame(WindowFrame::Custom)) {
        return;
    }

    SystemAllocator allocator;
    auto custom = CreateCustom(allocator, true);
    auto system = Window::Create(
        allocator,
        {.title = "SystemFrame", .width = FrameWidth, .height = FrameHeight, .visible = false});
    REQUIRE(custom && system);
    // 최대화 전환은 창을 화면에 띄우므로 자동 시험에서 빼고 사람 확인 목록에 둔다(최대화 때 화면 밖
    // 테두리 보정 포함). 숨긴 창은 최대화 상태가 아니다.
    CHECK(!custom.Value()->IsMaximized());
    // 닫기 요청은 OS 닫기 경로(WM_CLOSE)를 탄다.
    input::InputQueue queue(allocator);
    REQUIRE(queue.TryInitialize(64));
    custom.Value()->RequestClose();
    custom.Value()->PumpEvents(queue);
    CHECK(custom.Value()->IsCloseRequested());
    // OS 테두리 창은 가장자리를 OS가 판정하므로 UI 영역만 본다(끌기 영역 또는 클라이언트).
    CHECK(system.Value()->Frame() == WindowFrame::System);
    system.Value()->SetRegionHitTest(Window::RegionHitTest::CreateStatic(&TestRegion));
    CHECK(system.Value()->HitZone({0, 0}) == WindowZone::Caption);
    CHECK(system.Value()->HitZone({200, 150}) == WindowZone::Client);
    system.Value()->SetRegionHitTest({});
}

TEST(PlatformWindow_CustomFrameRejectedOnUnsupportedPlatforms) {
    if (Window::SupportsFrame(WindowFrame::Custom)) {
        return;
    }

    SystemAllocator allocator;
    auto window = CreateCustom(allocator, true);
    CHECK(!window && window.Error() == ErrorCode::Unsupported);
}

} // namespace rocks::platform
