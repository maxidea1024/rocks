#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/HitGrid.h>
#include <UiCore/ManualClock.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Layout/Frame.h>
#include <UiFramework/Layout/Overlay.h>
#include <UiFramework/Lists/Carousel.h>

// hit grid가 그리기 순서를 따르는지 시험한다(Docs/Design/UiToolkit 20.4절): Overlay의 z
// 순서(layer), 그리기 자식을 바꾸는 Carousel 슬라이드, 스냅샷 없는 HitTest와 grid의 같은 답.

namespace rocks::ui {

namespace {

constexpr Vec2 HitViewport{40, 20};

Result<SharedPtr<Frame>> MakeFrame(Allocator& allocator) noexcept {
    return SharedPtr<Frame>::TryMake(allocator, allocator);
}

} // namespace

TEST(UiFramework_HitGridFollowsOverlayPaintOrder) {
    SystemAllocator allocator;
    auto overlay = SharedPtr<Overlay>::TryMake(allocator, allocator);
    auto front = MakeFrame(allocator);
    auto back = MakeFrame(allocator);
    REQUIRE(overlay && front && back);
    // 먼저 넣었어도 z가 높은 쪽이 나중에 그려지고(위 layer) 위에서 맞는다.
    REQUIRE(overlay.Value()->TryAddSlot(front.Value(), 1));
    REQUIRE(overlay.Value()->TryAddSlot(back.Value(), 0));
    Application app(allocator);
    app.SetRoot(overlay.Value());
    REQUIRE(app.UpdatePaintCache(Geometry::Root(HitViewport, {}), 1));
    HitGrid grid(allocator);
    REQUIRE(grid.Update(overlay.Value()));
    const Vec2 center{20, 10};
    CHECK(grid.Find(center, *overlay.Value()).Get() == front.Value().Get());
    CHECK(overlay.Value()->HitTest(center) == front.Value().Get());
}

TEST(UiFramework_HitGridUsesCarouselSlidePaintChildren) {
    SystemAllocator allocator;
    auto carousel = SharedPtr<Carousel>::TryMake(allocator, allocator);
    REQUIRE(carousel);
    SharedPtr<Frame> contents[2];
    for (auto& content : contents) {
        auto made = MakeFrame(allocator);
        REQUIRE(made);
        content = made.Value();
        const SharedPtr<Frame> captured = content;
        auto page = SharedPtr<CarouselPage>::TryMake(
            allocator, allocator, CarouselPage::Factory::CreateLambda([captured]() noexcept {
                return Result<SharedPtr<Element>>::Success(captured);
            }));
        REQUIRE(page);
        REQUIRE(carousel.Value()->AddPage(page.Value()));
    }

    REQUIRE(carousel.Value()->Show(0, true, 0));
    ManualClock clock;
    Application app(allocator);
    app.SetClock(clock.Function());
    app.SetRoot(carousel.Value());
    const auto viewport = Geometry::Root(HitViewport, {});
    REQUIRE(app.UpdatePaintCache(viewport, 1));
    const Vec2 center{20, 10};
    CHECK(carousel.Value()->HitTest(center) == contents[0].Get());

    // 슬라이드가 시작되면(진행 0 — 시각이 흐르지 않음) 이전 쪽이 아직 화면에 있고 다음 쪽은 오른쪽
    // 밖이다. 그리기 자식(두 쪽)으로 걸으므로 이전 쪽이 맞는다 — 활성 자식만 걸으면 아무것도 안
    // 맞는다.
    REQUIRE(carousel.Value()->Show(1, true, 1));
    REQUIRE(app.UpdatePaintCache(viewport, 1));
    HitGrid grid(allocator);
    REQUIRE(grid.Update(carousel.Value()));
    CHECK(grid.Find(center, *carousel.Value()).Get() == contents[0].Get());
    CHECK(carousel.Value()->HitTest(center) == contents[0].Get());

    // 진행은 host 프레임 시각이다: 0.75초(속도 1)면 다음 쪽이 가운데를 덮는다.
    clock.Advance(0.75);
    REQUIRE(app.UpdatePaintCache(viewport, 1));
    REQUIRE(grid.Update(carousel.Value()));
    CHECK(grid.Find(center, *carousel.Value()).Get() == contents[1].Get());
}

} // namespace rocks::ui
