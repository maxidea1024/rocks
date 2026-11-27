#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/HitGrid.h>
#include <UiCore/ManualClock.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Layout/Frame.h>
#include <UiFramework/Layout/Overlay.h>

// 비활성 Element의 hit 시험이다(Docs/Design/UiToolkit 20.5절 — UE GetBubblePath 235–246행,
// SlateUser.cpp 1351행): 비활성 버튼을 누르면 뒤 형제가 아니라 그 부모가 받고, 툴팁은 비활성이어도
// 뜬다.

namespace rocks::ui {

namespace {

constexpr Vec2 DisabledViewport{40, 20};
constexpr Vec2 DisabledCenter{20, 10};

struct DisabledBench {
    explicit DisabledBench(Allocator& allocator) noexcept : app(allocator) {
    }

    SharedPtr<Overlay> overlay;
    SharedPtr<Frame> back;
    SharedPtr<Frame> front;
    ManualClock clock;
    Application app;
};

bool BuildBench(Allocator& allocator, DisabledBench& bench) noexcept {
    auto overlay = SharedPtr<Overlay>::TryMake(allocator, allocator);
    auto back = SharedPtr<Frame>::TryMake(allocator, allocator);
    auto front = SharedPtr<Frame>::TryMake(allocator, allocator);
    if (!overlay || !back || !front || !overlay.Value()->TryAddSlot(back.Value(), 0) ||
        !overlay.Value()->TryAddSlot(front.Value(), 1)) {
        return false;
    }

    bench.overlay = overlay.Value();
    bench.back = back.Value();
    bench.front = front.Value();
    bench.front->SetEnabled(false);
    bench.app.SetClock(bench.clock.Function());
    bench.app.SetRoot(bench.overlay);
    return static_cast<bool>(bench.app.UpdatePaintCache(Geometry::Root(DisabledViewport, {}), 1));
}

} // namespace

TEST(UiFramework_DisabledHitCutsPathAtTheDisabledElement) {
    SystemAllocator allocator;
    DisabledBench bench(allocator);
    REQUIRE(BuildBench(allocator, bench));
    HitGrid grid(allocator);
    REQUIRE(grid.Update(bench.overlay));
    // 맨 위가 비활성이면 그 부모가 받는다(뒤 형제가 아니다).
    CHECK(grid.Find(DisabledCenter, *bench.overlay).Get() == bench.overlay.Get());
    CHECK(bench.overlay->HitTest(DisabledCenter) == bench.overlay.Get());
    // 활성을 무시하면 비활성 요소 자신이다(툴팁·영역 지도).
    CHECK(grid.Find(DisabledCenter, *bench.overlay, true).Get() == bench.front.Get());
    CHECK(bench.overlay->HitTest(DisabledCenter, true) == bench.front.Get());
    // 루트가 비활성이면 아무것도 받지 않는다.
    bench.overlay->SetEnabled(false);
    REQUIRE(bench.app.UpdatePaintCache(Geometry::Root(DisabledViewport, {}), 1));
    REQUIRE(grid.Update(bench.overlay));
    CHECK(!grid.Find(DisabledCenter, *bench.overlay));
    CHECK(bench.overlay->HitTest(DisabledCenter) == nullptr);
    // 다시 켜면 맨 위 요소가 받는다.
    bench.overlay->SetEnabled(true);
    bench.front->SetEnabled(true);
    REQUIRE(bench.app.UpdatePaintCache(Geometry::Root(DisabledViewport, {}), 1));
    REQUIRE(grid.Update(bench.overlay));
    CHECK(grid.Find(DisabledCenter, *bench.overlay).Get() == bench.front.Get());
}

TEST(UiFramework_DisabledElementStillShowsItsToolTip) {
    SystemAllocator allocator;
    DisabledBench bench(allocator);
    REQUIRE(BuildBench(allocator, bench));
    REQUIRE(bench.front->SetToolTipFactory(
        Element::ToolTipFactory::CreateLambda([&allocator]() noexcept {
            auto tip = SharedPtr<Frame>::TryMake(allocator, allocator);
            return tip ? Result<SharedPtr<Element>>::Success(tip.Value())
                       : Result<SharedPtr<Element>>::Failure(tip.Error());
        })));
    REQUIRE(bench.app.UpdatePaintCache(Geometry::Root(DisabledViewport, {}), 1));
    bench.app.BeginFrame();
    REQUIRE(bench.app.RoutePointer({PointerEventType::Move, DisabledCenter}));
    bench.clock.Advance(1);
    bench.app.BeginFrame();
    REQUIRE(bench.app.UpdatePaintCache(Geometry::Root(DisabledViewport, {}), 1));
    CHECK(bench.app.IsToolTipOpen(0));
}

} // namespace rocks::ui
