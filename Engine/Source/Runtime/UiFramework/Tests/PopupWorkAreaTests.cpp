#include <Core/Memory/SystemAllocator.h>
#include <Core/ScopeExit.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/Button.h>
#include <UiFramework/Layout/AnchorCanvas.h>
#include <UiFramework/Layout/Frame.h>
#include <UiFramework/Layout/Overlay.h>
#include <UiFramework/Popups/MenuAnchor.h>

// 팝업 허용 영역 시험이다(Docs/Design/UiToolkit 9.1절 UW7d — 시스템 규칙). 창 호스트가 등록한
// 공급자의 영역(모니터 작업 영역)을 메뉴·하위 메뉴·툴팁이 개별 처리 없이 지키는지 단언한다.

namespace rocks::ui {

namespace {

// 창(400×300)의 오른쪽 아래가 작업 표시줄에 가려진 모니터다: 작업 영역은 (0, 0)–(350, 250).
constexpr Vec2 WindowSize{400, 300};
constexpr Vec2 WorkMax{350, 250};

Element::PopupWorkArea WorkArea(bool known) noexcept {
    return Element::PopupWorkArea::CreateLambda([known](Vec2, Box2<float>& area) noexcept {
        area = {{0, 0}, WorkMax};
        return known;
    });
}

struct PopupBench {
    SystemAllocator allocator;
    Application app{allocator};
    SharedPtr<Overlay> root;
    SharedPtr<AnchorCanvas> canvas;
    SharedPtr<MenuAnchor> anchor;
    SharedPtr<Frame> menu;

    bool Build(Vec4 offset, MenuPlacement placement) noexcept {
        auto madeRoot = SharedPtr<Overlay>::TryMake(allocator, allocator);
        auto madeCanvas = SharedPtr<AnchorCanvas>::TryMake(allocator, allocator);
        auto madeAnchor = SharedPtr<MenuAnchor>::TryMake(allocator, allocator);
        auto madeMenu = SharedPtr<Frame>::TryMake(allocator, allocator);
        if (!madeRoot || !madeCanvas || !madeAnchor || !madeMenu) {
            return false;
        }

        root = Move(madeRoot.Value());
        canvas = Move(madeCanvas.Value());
        anchor = Move(madeAnchor.Value());
        menu = Move(madeMenu.Value());
        menu->SetDesiredSize({120, 80});
        anchor->SetPlacement(placement);
        anchor->SetMenuFactory(MenuAnchor::MenuFactory::CreateLambda(
            [item = menu]() noexcept { return Result<SharedPtr<Element>>::Success(item); }));
        if (!root->TryAddChild(canvas) ||
            !canvas->TryAddSlot(anchor, {.offset = offset, .alignment = {0, 0}})) {
            return false;
        }

        app.SetRoot(root);
        return Step();
    }

    bool Step() noexcept {
        return static_cast<bool>(app.UpdatePaintCache(Geometry::Root(WindowSize, {}), 1));
    }

    bool OpenAndArrange() noexcept {
        return anchor->Open() && Step();
    }

    Vec2 MenuOrigin() const noexcept {
        auto bounds = menu->GetGeometry().WindowBounds();
        return bounds ? bounds.Value().min : Vec2{-1, -1};
    }
};

} // namespace

TEST(UiFramework_PopupLayerStaysInsideWorkArea) {
    PopupBench bench;
    REQUIRE(bench.Build({300, 10, 40, 20}, MenuPlacement::Below));
    REQUIRE(bench.app.SetPopupWorkArea(WorkArea(true)));
    REQUIRE(bench.OpenAndArrange());
    // 앵커 아래(30), 오른쪽 끝은 창(400)이 아니라 작업 영역(350)에서 밀린다: 350 - 120 = 230.
    CHECK((bench.MenuOrigin() == Vec2{230, 30}));
    // 팝업 층은 Overlay 전체를 덮고(바깥 누름을 놓치지 않는다) 내용만 작업 영역 안이다.
    REQUIRE(bench.root->ChildCount() == 2);
    CHECK((bench.root->ChildAt(1)->GetGeometry().size == WindowSize));
}

TEST(UiFramework_PopupFlipsAboveAtWorkAreaBottom) {
    PopupBench bench;
    REQUIRE(bench.Build({20, 200, 40, 20}, MenuPlacement::Below));
    REQUIRE(bench.app.SetPopupWorkArea(WorkArea(true)));
    REQUIRE(bench.OpenAndArrange());
    // 아래 공간(250 - 220 = 30)이 메뉴(80)보다 작고 위(200)가 넓어 위로 뒤집는다: 200 - 80 = 120.
    // 창 기준이었다면 아래(300 - 220 = 80)에 들어가 작업 표시줄 뒤로 숨었다.
    CHECK((bench.MenuOrigin() == Vec2{20, 120}));
}

TEST(UiFramework_SubMenuFlipsLeftAtWorkAreaRightEdge) {
    PopupBench bench;
    REQUIRE(bench.Build({200, 40, 80, 20}, MenuPlacement::Right));
    REQUIRE(bench.app.SetPopupWorkArea(WorkArea(true)));
    REQUIRE(bench.OpenAndArrange());
    // 오른쪽(280 + 120 = 400 > 350)이 모자라 왼쪽으로 연다: 200 - 120 = 80.
    CHECK((bench.MenuOrigin() == Vec2{80, 40}));
}

TEST(UiFramework_PopupUsesWindowWithoutWorkArea) {
    PopupBench bench;
    REQUIRE(bench.Build({300, 10, 40, 20}, MenuPlacement::Below));
    REQUIRE(bench.OpenAndArrange());
    // 공급자가 없으면 창 경계만 지킨다: 400 - 120 = 280.
    CHECK((bench.MenuOrigin() == Vec2{280, 30}));
    bench.anchor->Close();
    // 공급자가 모른다고 해도(false) 창 경계다.
    REQUIRE(bench.app.SetPopupWorkArea(WorkArea(false)));
    REQUIRE(bench.OpenAndArrange());
    CHECK((bench.MenuOrigin() == Vec2{280, 30}));
    bench.anchor->Close();
    // 공급자를 떼면 다시 창 경계다.
    REQUIRE(bench.app.SetPopupWorkArea(WorkArea(true)));
    REQUIRE(bench.app.SetPopupWorkArea({}));
    REQUIRE(bench.OpenAndArrange());
    CHECK((bench.MenuOrigin() == Vec2{280, 30}));
}

namespace {

double workTipClock = 0;
double ReadWorkTipClock() noexcept {
    return workTipClock;
}

} // namespace

TEST(UiFramework_ToolTipStaysInsideWorkAreaOnTemporaryHost) {
    // 루트가 Overlay가 아니면 Application이 툴팁 임시 호스트로 감싼다. 공급자는 그 호스트로도
    // 옮겨져야 한다.
    SystemAllocator allocator;
    Application app(allocator);
    auto root = SharedPtr<AnchorCanvas>::TryMake(allocator, allocator);
    auto owner = SharedPtr<Button>::TryMake(allocator, allocator);
    auto content = SharedPtr<Frame>::TryMake(allocator, allocator);
    REQUIRE(root && owner && content);
    content.Value()->SetDesiredSize({30, 10});
    REQUIRE(owner.Value()->SetToolTipFactory(Element::ToolTipFactory::CreateLambda(
        [element = content.Value()]() noexcept -> Result<SharedPtr<Element>> {
            return Result<SharedPtr<Element>>::Success(element);
        })));
    REQUIRE(root.Value()->TryAddSlot(owner.Value(), {.offset = {0, 240, 20, 10}, .alignment = {}}));
    app.SetRoot(root.Value());
    REQUIRE(app.SetPopupWorkArea(WorkArea(true)));
    workTipClock = 0;
    app.SetToolTipClock(&ReadWorkTipClock);
    ROCKS_ON_SCOPE_EXIT {
        app.SetToolTipClock(nullptr);
    };
    REQUIRE(app.SetToolTipTiming(0.5, 0));
    const auto frame = [&]() noexcept {
        return static_cast<bool>(app.UpdatePaintCache(Geometry::Root(WindowSize, {}), 1));
    };
    REQUIRE(frame());
    REQUIRE(app.SetFocus(0, owner.Value(), FocusCause::Navigation));
    workTipClock = 1;
    REQUIRE(frame());
    REQUIRE(frame());
    REQUIRE(app.IsToolTipOpen(0));
    auto bounds = content.Value()->GetGeometry().WindowBounds();
    REQUIRE(bounds);
    // 키보드 툴팁은 소유자(240..250) 아래(252..262)에 놓이는데, 창(300)에는 들어가도 작업
    // 영역(250)을 넘으므로 위로 뒤집는다: 240 - 2 - 10 = 228.
    CHECK(bounds.Value().min.y == 228);
    CHECK(bounds.Value().max.y <= WorkMax.y);
}

namespace {

// 창이 두 모니터에 걸쳤다: 왼쪽 모니터 A는 x 0–200, 오른쪽 B는 200–400(창 좌표).
constexpr float MonitorSplit = 200;

Element::PopupWorkArea TwoMonitors() noexcept {
    return Element::PopupWorkArea::CreateLambda([](Vec2 point, Box2<float>& area) noexcept {
        area = point.x < MonitorSplit ? Box2<float>{{0, 0}, {MonitorSplit, WindowSize.y}}
                                      : Box2<float>{{MonitorSplit, 0}, WindowSize};
        return true;
    });
}

} // namespace

TEST(UiFramework_PopupPicksMonitorOfAnchorTopLeft) {
    // 앵커(150..250)가 두 모니터에 걸쳤다. UE처럼 앵커 왼쪽 위(150)가 놓인 모니터 A 안에 둔다
    // (가운데 200은 B다): 200 - 120 = 80.
    PopupBench bench;
    REQUIRE(bench.Build({150, 10, 100, 20}, MenuPlacement::Below));
    REQUIRE(bench.app.SetPopupWorkArea(TwoMonitors()));
    REQUIRE(bench.OpenAndArrange());
    CHECK((bench.MenuOrigin() == Vec2{80, 30}));
    auto menuBounds = bench.menu->GetGeometry().WindowBounds();
    REQUIRE(menuBounds);
    CHECK(menuBounds.Value().max.x <= MonitorSplit);
}

TEST(UiFramework_PopupOnRightMonitorStaysOnRightMonitor) {
    PopupBench bench;
    REQUIRE(bench.Build({210, 10, 40, 20}, MenuPlacement::Below));
    REQUIRE(bench.app.SetPopupWorkArea(TwoMonitors()));
    REQUIRE(bench.OpenAndArrange());
    CHECK((bench.MenuOrigin() == Vec2{210, 30}));
}

TEST(UiFramework_CursorToolTipUsesCursorMonitor) {
    // 소유자(100..300)의 왼쪽 위는 A, 커서(250)는 B다. 커서에 붙는 툴팁은 커서 모니터(B)를
    // 쓴다(UE CalculateTooltipWindowPosition): 250 + 12 = 262(B 안에 들어간다).
    SystemAllocator allocator;
    Application app(allocator);
    auto root = SharedPtr<AnchorCanvas>::TryMake(allocator, allocator);
    auto owner = SharedPtr<Button>::TryMake(allocator, allocator);
    auto content = SharedPtr<Frame>::TryMake(allocator, allocator);
    REQUIRE(root && owner && content);
    content.Value()->SetDesiredSize({30, 10});
    REQUIRE(owner.Value()->SetToolTipFactory(Element::ToolTipFactory::CreateLambda(
        [element = content.Value()]() noexcept -> Result<SharedPtr<Element>> {
            return Result<SharedPtr<Element>>::Success(element);
        })));
    REQUIRE(
        root.Value()->TryAddSlot(owner.Value(), {.offset = {100, 90, 200, 30}, .alignment = {}}));
    app.SetRoot(root.Value());
    REQUIRE(app.SetPopupWorkArea(TwoMonitors()));
    workTipClock = 0;
    app.SetToolTipClock(&ReadWorkTipClock);
    ROCKS_ON_SCOPE_EXIT {
        app.SetToolTipClock(nullptr);
    };
    REQUIRE(app.SetToolTipTiming(0.5, 0));
    const auto frame = [&]() noexcept {
        return static_cast<bool>(app.UpdatePaintCache(Geometry::Root(WindowSize, {}), 1));
    };
    REQUIRE(frame());
    REQUIRE(app.RoutePointer({PointerEventType::Move, {250, 100}}));
    workTipClock = 1;
    REQUIRE(frame());
    REQUIRE(frame());
    REQUIRE(app.IsToolTipOpen(0));
    auto bounds = content.Value()->GetGeometry().WindowBounds();
    REQUIRE(bounds);
    CHECK(bounds.Value().min.x == 262);
}

TEST(UiFramework_PopupOutsidePressOnOtherMonitorStillCloses) {
    // 회귀(R16): 바깥 누름을 받는 층(콤보 등)이 작업 영역 교집합으로 줄면, 다른 모니터 쪽 창 영역
    // 누름이 아래 버튼으로 새고 메뉴가 열린 채 남았다.
    PopupBench bench;
    REQUIRE(bench.Build({20, 10, 40, 20}, MenuPlacement::Below));
    auto other = SharedPtr<Button>::TryMake(bench.allocator, bench.allocator);
    REQUIRE(other);
    uint32 clicked = 0;
    other.Value()->SetOnClicked(
        Delegate<void()>::CreateLambda([&clicked]() noexcept { ++clicked; }));
    REQUIRE(
        bench.canvas->TryAddSlot(other.Value(), {.offset = {300, 100, 40, 20}, .alignment = {}}));
    REQUIRE(bench.app.SetPopupWorkArea(TwoMonitors()));
    REQUIRE(bench.OpenAndArrange());
    REQUIRE(bench.anchor->IsOpen());
    REQUIRE(bench.app.RoutePointer({PointerEventType::Down, {320, 110}}));
    REQUIRE(bench.app.RoutePointer({PointerEventType::Up, {320, 110}}));
    CHECK(!bench.anchor->IsOpen() && clicked == 0);
}

TEST(UiFramework_SubMenuPushedUpAtWorkAreaBottom) {
    PopupBench bench;
    REQUIRE(bench.Build({20, 230, 80, 20}, MenuPlacement::Right));
    REQUIRE(bench.app.SetPopupWorkArea(WorkArea(true)));
    REQUIRE(bench.OpenAndArrange());
    // 옆에 여는 메뉴(80)가 작업 영역 아래(250)를 넘으면 위로 민다: 250 - 80 = 170.
    CHECK((bench.MenuOrigin() == Vec2{100, 170}));
}

TEST(UiFramework_PopupWorkAreaUnderScaledHost) {
    // 배율 2인 호스트(창 800×600, 로컬 400×300): 작업 영역은 창 좌표 (0, 0)–(350, 250)이라 로컬
    // (0, 0)–(175, 125)다. 앵커(로컬 300, 10) 아래 메뉴는 로컬 175 - 120 = 55, 창 좌표 110이다.
    PopupBench bench;
    REQUIRE(bench.Build({300, 10, 40, 20}, MenuPlacement::Below));
    REQUIRE(bench.app.SetPopupWorkArea(WorkArea(true)));
    REQUIRE(bench.anchor->Open());
    REQUIRE(bench.app.UpdatePaintCache(Geometry::Root(WindowSize, {2, 0, 0, 2, 0, 0}), 1));
    REQUIRE(bench.app.UpdatePaintCache(Geometry::Root(WindowSize, {2, 0, 0, 2, 0, 0}), 1));
    CHECK((bench.MenuOrigin() == Vec2{110, 60}));
}

} // namespace rocks::ui
