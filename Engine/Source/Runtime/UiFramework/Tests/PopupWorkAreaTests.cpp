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
    // 팝업 층 자체가 Overlay ∩ 작업 영역이다.
    REQUIRE(bench.root->ChildCount() == 2);
    CHECK((bench.root->ChildAt(1)->GetGeometry().size == WorkMax));
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

} // namespace rocks::ui
