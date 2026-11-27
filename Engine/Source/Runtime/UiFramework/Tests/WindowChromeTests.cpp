#include "RegionMapCheck.h"

#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/Styles/CoreStyles.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Layout/Frame.h>
#include <UiFramework/Window/WindowChrome.h>

namespace rocks::ui {

namespace {

struct ChromeBench {
    SystemAllocator allocator;
    Application app{allocator};
    SharedPtr<WindowChrome> chrome;
    SharedPtr<WindowTitleBar> bar;
    SharedPtr<Frame> content;
    SharedPtr<TextLabel> title;
    uint32 minimized = 0;
    uint32 toggled = 0;
    uint32 closed = 0;
    Vec2 window{400, 300};

    bool Build() noexcept {
        auto madeChrome = SharedPtr<WindowChrome>::TryMake(allocator, allocator);
        auto madeBar = SharedPtr<WindowTitleBar>::TryMake(allocator, allocator);
        auto madeContent = SharedPtr<Frame>::TryMake(allocator, allocator);
        auto madeTitle = SharedPtr<TextLabel>::TryMake(allocator, allocator);
        if (!madeChrome || !madeBar || !madeContent || !madeTitle) {
            return false;
        }

        chrome = Move(madeChrome.Value());
        bar = Move(madeBar.Value());
        content = Move(madeContent.Value());
        title = Move(madeTitle.Value());
        // 글자 폭 10인 제목 "Title"(50 x 14)이다.
        constexpr usize TitleLength = 5;
        text::TextCluster clusters[TitleLength];
        for (usize i = 0; i < TitleLength; ++i) {
            clusters[i] = {.byteEnd = i + 1, .advance = 10};
        }

        if (!title->SetShapedText("Title", clusters, {}, 14, false)) {
            return false;
        }

        auto built = bar->Construct(
            {.title = title,
             .onMinimize = Delegate<void()>::CreateLambda([this]() noexcept { ++minimized; }),
             .onToggleMaximize = Delegate<void()>::CreateLambda([this]() noexcept { ++toggled; }),
             .onClose = Delegate<void()>::CreateLambda([this]() noexcept { ++closed; })});
        if (!built || !chrome->Construct({.titleBar = bar, .content = content})) {
            return false;
        }

        app.SetRoot(chrome);
        return Step();
    }

    bool Step() noexcept {
        return static_cast<bool>(app.UpdatePaintCache(Geometry::Root(window, {}), 1));
    }

    Box2<float> Bounds(const Element& element) const noexcept {
        auto bounds = element.GetGeometry().WindowBounds();
        return bounds ? bounds.Value() : Box2<float>{};
    }

    static Vec2 Center(Box2<float> box) noexcept {
        return (box.min + box.max) * 0.5f;
    }
};

} // namespace

TEST(UiFramework_WindowChromeLaysOutTitleBarAndBorder) {
    ChromeBench bench;
    REQUIRE(bench.Build());
    const WindowStyle& style = DefaultWindowStyle();
    const Vec4 inset = style.borderPadding;
    // 제목 막대는 테두리 여백 안 맨 위, 내용은 그 아래 나머지다.
    const auto bar = bench.Bounds(*bench.bar);
    CHECK(bar.min.x == inset.x && bar.min.y == inset.y && bar.max.x == bench.window.x - inset.z);
    const float barHeight = bar.max.y - bar.min.y;
    CHECK(barHeight >= style.buttonSize.y);
    // 내용은 제목 막대 아래에서 내용 여백만큼 떨어진다(창 외곽선에 붙지 않는다).
    const Vec4 pad = style.contentPadding;
    const auto body = bench.Bounds(*bench.content);
    CHECK(body.min.x == inset.x + pad.x && body.max.x == bench.window.x - inset.z - pad.z);
    CHECK(body.min.y == inset.y + barHeight + pad.y &&
          body.max.y == bench.window.y - inset.w - pad.w);
    // 버튼은 오른쪽 위에 최소화·최대화·닫기 순으로 붙는다.
    const auto close = bench.Bounds(*bench.bar->ControlButton(WindowButtonRole::Close));
    const auto maximize = bench.Bounds(*bench.bar->ControlButton(WindowButtonRole::Maximize));
    const auto minimize = bench.Bounds(*bench.bar->ControlButton(WindowButtonRole::Minimize));
    CHECK(close.max.x == bar.max.x && close.min.y == bar.min.y);
    CHECK(maximize.max.x == close.min.x && minimize.max.x == maximize.min.x);
    CHECK(close.max.x - close.min.x == style.buttonSize.x);
    // 제목은 막대 전체 폭의 가운데다.
    const auto title = bench.Bounds(*bench.title);
    CHECK(title.min.x + title.max.x == bar.min.x + bar.max.x);
    // 최대화하면 테두리·여백이 없어지고 최대화 버튼이 복원 모양이다.
    bench.chrome->SetMaximized(true);
    REQUIRE(bench.Step());
    CHECK(bench.Bounds(*bench.bar).min.x == 0 && bench.Bounds(*bench.bar).min.y == 0);
    CHECK(bench.bar->ControlButton(WindowButtonRole::Maximize)->IsMaximized());
}

TEST(UiFramework_WindowChromeAnswersWindowRegions) {
    ChromeBench bench;
    REQUIRE(bench.Build());
    const auto bar = bench.Bounds(*bench.bar);
    const auto at = [&](const Element& element) noexcept {
        return bench.app.QueryWindowRegion(ChromeBench::Center(bench.Bounds(element)));
    };
    // 제목 막대 빈 곳·제목 글자는 끌기 영역, 창 버튼은 각자 영역, 앱 아이콘은 시스템 메뉴다.
    CHECK(bench.app.QueryWindowRegion({(bar.min.x + bar.max.x) * 0.5f - 80, bar.min.y + 5}) ==
          input::WindowRegion::Caption);
    CHECK(at(*bench.title) == input::WindowRegion::Caption);
    CHECK(at(*bench.bar->ControlButton(WindowButtonRole::Minimize)) ==
          input::WindowRegion::MinimizeButton);
    CHECK(at(*bench.bar->ControlButton(WindowButtonRole::Maximize)) ==
          input::WindowRegion::MaximizeButton);
    CHECK(at(*bench.bar->ControlButton(WindowButtonRole::Close)) ==
          input::WindowRegion::CloseButton);
    CHECK(bench.app.QueryWindowRegion({bar.min.x + 8, bar.min.y + 8}) ==
          input::WindowRegion::SystemMenu);
    // 내용은 클라이언트다.
    CHECK(at(*bench.content) == input::WindowRegion::Client);
    // 창 backend가 읽는 영역 지도는 점마다 같은 답이다(Docs/Design/UiToolkit 19.2절). 버튼·아이콘은
    // 제목보다 앞에 있고, 메뉴가 없으므로 popupOpen이 아니다.
    IntVec2 mismatch;
    CHECK(test::RegionMapMatchesQuery(bench.app, {400, 300}, mismatch));
    input::WindowRegionMap map;
    bench.app.BuildWindowRegionMap(map);
    CHECK(!map.popupOpen && map.count > 0);
}

TEST(UiFramework_RegionMapIsEmptyBeforePaintAndFollowsFullscreen) {
    ChromeBench bench;
    input::WindowRegionMap map;
    // 그리기 전(hit grid 없음)에는 빈 지도다(전부 Client).
    bench.app.BuildWindowRegionMap(map);
    CHECK(map.count == 0 && !map.popupOpen);
    REQUIRE(bench.Build());
    bench.app.BuildWindowRegionMap(map);
    CHECK(map.count > 0);
    // 전체 화면에서 제목 막대는 Client로 답하므로 지도에서 Caption이 빠진다(창 버튼은 남는다).
    bench.chrome->SetFullscreen(true);
    REQUIRE(bench.Step());
    bench.app.BuildWindowRegionMap(map);
    bool caption = false;
    for (uint32 index = 0; index < map.count; ++index) {
        caption |= map.rects[index].region == input::WindowRegion::Caption;
    }

    CHECK(!caption);
    IntVec2 mismatch;
    CHECK(test::RegionMapMatchesQuery(bench.app, {400, 300}, mismatch));
}

TEST(UiFramework_WindowChromeButtonsInvokeActions) {
    ChromeBench bench;
    REQUIRE(bench.Build());
    const auto click = [&](WindowButtonRole role) noexcept {
        const Vec2 point = ChromeBench::Center(bench.Bounds(*bench.bar->ControlButton(role)));
        auto down = bench.app.RoutePointer({PointerEventType::Down, point});
        auto up = bench.app.RoutePointer({PointerEventType::Up, point});
        return down && up;
    };
    REQUIRE(click(WindowButtonRole::Minimize));
    REQUIRE(click(WindowButtonRole::Maximize));
    REQUIRE(click(WindowButtonRole::Close));
    CHECK(bench.minimized == 1 && bench.toggled == 1 && bench.closed == 1);
    // 창 버튼은 키보드 초점을 받지 않는다(UE IsFocusable false).
    CHECK(!bench.app.GetFocus(0));
    CHECK(!bench.bar->ControlButton(WindowButtonRole::Close)->SupportsKeyboardFocus());
}

TEST(UiFramework_WindowChromeRejectsMissingParts) {
    SystemAllocator allocator;
    auto chrome = SharedPtr<WindowChrome>::TryMake(allocator, allocator);
    REQUIRE(chrome);
    CHECK(chrome.Value()->Construct({}).Error() == ErrorCode::InvalidArgument);
}

TEST(UiFramework_WindowTitleBarStatesAndToolTips) {
    SystemAllocator allocator;
    auto bar = SharedPtr<WindowTitleBar>::TryMake(allocator, allocator);
    auto title = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    REQUIRE(bar && title);
    const text::TextCluster cluster{.byteEnd = 1, .advance = 10};
    REQUIRE(title.Value()->SetShapedText("T", {&cluster, 1}, {}, 14, false));
    // 툴팁 글자는 받은 그대로 레이블이 된다(시험용 shaping).
    auto made = bar.Value()->Construct(
        {.title = title.Value(),
         .canMaximize = false,
         .makeLabel =
             Delegate<Result<SharedPtr<TextLabel>>(StringView, const FontStyle&)>::CreateLambda(
                 [&allocator](StringView text,
                              const FontStyle& font) noexcept -> Result<SharedPtr<TextLabel>> {
                     auto label = SharedPtr<TextLabel>::TryMake(allocator, allocator);
                     if (!label) {
                         return label;
                     }

                     text::TextCluster clusters[16];
                     const usize count = text.ByteCount() < 16 ? text.ByteCount() : 16;
                     for (usize i = 0; i < count; ++i) {
                         clusters[i] = {.byteEnd = i + 1, .advance = font.size};
                     }

                     auto shaped = label.Value()->SetShapedText(
                         text.SliceBytes(0, count), {clusters, count}, {}, font.size, false);
                     return shaped ? Move(label)
                                   : Result<SharedPtr<TextLabel>>::Failure(shaped.Error());
                 })});
    REQUIRE(made);
    // 창이 최대화를 허용하지 않으면 최대화 버튼이 꺼진다(최소화·닫기는 켜짐).
    CHECK(!bar.Value()->ControlButton(WindowButtonRole::Maximize)->IsEnabled());
    CHECK(bar.Value()->ControlButton(WindowButtonRole::Minimize)->IsEnabled());
    CHECK(bar.Value()->ControlButton(WindowButtonRole::Close)->IsEnabled());
    // 창 버튼 툴팁 글자: 최대화 버튼은 최대화 중이면 "Restore Down"이다.
    const auto tipText = [&allocator](Element& button) noexcept -> String {
        auto tip = button.MakeToolTip();
        String text(allocator);
        if (tip && tip.Value()->ChildCount() == 1) {
            (void)text.TryAssign(static_cast<TextLabel&>(*tip.Value()->ChildAt(0)).GetText());
        }

        return text;
    };
    CHECK(tipText(*bar.Value()->ControlButton(WindowButtonRole::Close)).View() == "Close");
    CHECK(tipText(*bar.Value()->ControlButton(WindowButtonRole::Maximize)).View() == "Maximize");
    bar.Value()->SetMaximized(true);
    CHECK(tipText(*bar.Value()->ControlButton(WindowButtonRole::Maximize)).View() ==
          "Restore Down");
    // 비활성 창은 제목을 흐리게 그리고, 다시 활성이면 원래대로다.
    bar.Value()->SetActive(false);
    CHECK(!bar.Value()->IsActive() &&
          title.Value()->GetRenderOpacity() == InactiveWindowTitleOpacity);
    bar.Value()->SetActive(true);
    CHECK(title.Value()->GetRenderOpacity() == 1.0f);
    // 제목을 바꾸면 옛 제목은 떨어지고 새 제목은 잘림이 켜진다.
    auto renamed = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    REQUIRE(renamed);
    REQUIRE(bar.Value()->SetTitle(renamed.Value()));
    CHECK(!title.Value()->Parent() && renamed.Value()->Parent() == bar.Value().Get());
    CHECK(renamed.Value()->ClipsToBounds());
}

} // namespace rocks::ui
