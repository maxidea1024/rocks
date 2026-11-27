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

// 위 배치의 영역 지도다(앞 항목이 이긴다: 버튼·시스템 메뉴가 제목보다 앞).
input::WindowRegionMap TestRegionMap(bool popupOpen = false) noexcept {
    const int32 width = static_cast<int32>(FrameWidth);
    input::WindowRegionMap map;
    (void)map.TryAdd(
        {{{width - ButtonWidth, 0}, {width, TitleHeight}}, input::WindowRegion::CloseButton});
    (void)map.TryAdd({{{width - 2 * ButtonWidth, 0}, {width - ButtonWidth, TitleHeight}},
                      input::WindowRegion::MaximizeButton});
    (void)map.TryAdd({{{width - 3 * ButtonWidth, 0}, {width - 2 * ButtonWidth, TitleHeight}},
                      input::WindowRegion::MinimizeButton});
    (void)map.TryAdd({{{0, 0}, {IconWidth, TitleHeight}}, input::WindowRegion::SystemMenu});
    (void)map.TryAdd({{{0, 0}, {width, TitleHeight}}, input::WindowRegion::Caption});
    map.popupOpen = popupOpen;
    return map;
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
    frame.PublishRegionMap(TestRegionMap());
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
    // 열린 팝업(메뉴)이 있으면 크기 조절 가장자리를 끈다(UE AnyMenusVisible — 19.2절). 지도가
    // 그 점에 답한다.
    frame.PublishRegionMap(TestRegionMap(true));
    CHECK(frame.HitZone({200, 0}) == WindowZone::Caption);
    CHECK(frame.HitZone({0, 150}) == WindowZone::Client);
    CHECK(frame.HitZone({right, 0}) == WindowZone::CloseButton);
    frame.PublishRegionMap({});
    // 빈 지도는 전부 Client이고 가장자리만 남는다.
    CHECK(frame.HitZone({200, y}) == WindowZone::Client);
    CHECK(frame.HitZone({200, 0}) == WindowZone::TopBorder);
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
    frame.PublishRegionMap(TestRegionMap());
    // 크기 고정 창은 가장자리 판정이 없고 최대화도 하지 않는다(UE HasMaximizeBox false).
    CHECK(!frame.IsResizable());
    CHECK(frame.HitZone({0, 150}) == WindowZone::Client);
    CHECK(frame.HitZone({200, 0}) == WindowZone::Caption);
    frame.ToggleMaximize();
    CHECK(!frame.IsMaximized());
    frame.PublishRegionMap({});
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
    system.Value()->PublishRegionMap(TestRegionMap());
    CHECK(system.Value()->HitZone({0, 0}) == WindowZone::Caption);
    CHECK(system.Value()->HitZone({200, 150}) == WindowZone::Client);
    system.Value()->PublishRegionMap({});
}

TEST(PlatformWindow_WorkAreaAtReturnsMonitorWorkAreaInClientSpace) {
    SystemAllocator allocator;
    auto window = Window::Create(
        allocator,
        {.title = "WorkArea", .width = FrameWidth, .height = FrameHeight, .visible = false});
    if (!window) {
        // 창 backend가 없는 플랫폼이다.
        CHECK(window.Error() == ErrorCode::Unsupported);
        return;
    }

    // 팝업 허용 영역(UW7d): 창 client 지점이 놓인 모니터의 작업 영역이다. 비어 있지 않고, 창이
    // 그 모니터 위에 있으므로 client 원점 둘레를 덮는다.
    auto area = window.Value()->WorkAreaAt({0, 0});
    REQUIRE(area);
    CHECK(!area.Value().IsEmpty());
    // 새 창은 모니터 작업 영역 안에 놓이므로 client 원점을 덮는다.
    const IntRect rect = area.Value();
    CHECK(rect.min.x <= 0 && rect.min.y <= 0 && rect.max.x > 0 && rect.max.y > 0);
    // 같은 지점을 두 번 물으면 같은 답이다(창을 움직이지 않았다).
    auto again = window.Value()->WorkAreaAt({1, 1});
    REQUIRE(again);
    CHECK(again.Value() == area.Value());
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
