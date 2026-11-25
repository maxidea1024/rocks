#include "WorkspaceCapture.h"
#include <UiFramework/Application.h>
#include <UiFramework/InputAdapter.h>

namespace rocks::ui {

namespace {

bool Click(Application& app, Vec2 point) noexcept {
    auto down = app.RoutePointer({PointerEventType::Down, point});
    auto up = app.RoutePointer({PointerEventType::Up, point});
    return down && down.Value() && up && up.Value();
}

bool ShowsFill(const CacheRoot& cache, Vec2 point) noexcept {
    const auto accent = LinearColor::FromSRGB({124, 239, 220});
    for (const auto& box : cache.List().Boxes()) {
        if (box.color == accent && box.geometry.Contains(point)) {
            return true;
        }
    }

    return false;
}

bool ShowsPlacement(const CacheRoot& cache, Vec2 point) noexcept {
    for (const auto& box : cache.List().Boxes()) {
        if (box.color == LinearColor{0.10f, 0.42f, 0.90f, 0.38f} && box.geometry.Contains(point)) {
            return true;
        }
    }

    return false;
}

bool VerifySplitterCursor(Application& app) noexcept {
    using input::InputEvent;
    using input::MouseButton;
    using input::MouseCursor;

    InputAdapter adapter(app);
    struct Divider {
        Vec2 point;
        MouseCursor cursor;
    };
    constexpr Divider dividers[] = {{{716, 120}, MouseCursor::ResizeHorizontal},
                                    {{250, 528}, MouseCursor::ResizeVertical}};
    for (const auto& divider : dividers) {
        const Vec2 p = divider.point;
        if (!adapter.Route(InputEvent::MouseMove(p.x, p.y)) ||
            adapter.MouseCursor() != divider.cursor ||
            !adapter.Route(InputEvent::MouseDown(MouseButton::Left, p.x, p.y))) {
            return false;
        }

        auto outside = adapter.Route(InputEvent::MouseMove(-60, -40));
        if (!outside || !outside.Value() || adapter.MouseCursor() != divider.cursor) {
            return false;
        }

        // 구분선을 원래 위치에 되돌리고 버튼 해제와 캡처 상실을 각각 확인한다.
        if (!adapter.Route(InputEvent::MouseMove(p.x, p.y))) {
            return false;
        }

        if (divider.cursor == MouseCursor::ResizeHorizontal) {
            if (!adapter.Route(InputEvent::MouseUp(MouseButton::Left, p.x, p.y))) {
                return false;
            }
        } else if (!adapter.Route(InputEvent::CaptureLost())) {
            return false;
        }

        if (!adapter.Route(InputEvent::MouseMove(-60, -40)) ||
            adapter.MouseCursor() != MouseCursor::Arrow) {
            return false;
        }
    }

    return true;
}

bool VerifyOuterDocking(Allocator& allocator, Application& app,
                        const SharedPtr<DockManager>& root) noexcept {
    const auto viewport = Geometry::Root({1120, 680}, {});
    Array<uint8> original(allocator);
    if (!root->SaveLayout(1, original) || !app.UpdatePaintCache(viewport, 1)) {
        return false;
    }

    SharedPtr<Element> timeline;
    for (const auto& element : root->ActiveChildren()) {
        if (element->GetGeometry().Contains({100, 600})) {
            timeline = element;
        }
    }

    if (!timeline) {
        return false;
    }

    // 분할된 창의 바깥에 놓으면 미리보기와 실제 본문이 반대쪽 끝까지 닿아야 한다.
    struct Placement {
        Vec2 point, first, last, outside;
    };
    constexpr Placement cases[] = {
        {{1099, 340}, {570, 40}, {1115, 675}, {550, 400}},
        {{2, 400}, {5, 40}, {550, 675}, {570, 400}},
        {{560, 21}, {5, 40}, {1115, 330}, {500, 350}},
        {{700, 678}, {5, 375}, {1115, 675}, {500, 330}},
    };
    for (const auto& placement : cases) {
        if (!app.RoutePointer({PointerEventType::Down, {60, 540}}) ||
            !app.RoutePointer({PointerEventType::Move, placement.point})) {
            return false;
        }

        auto cache = app.UpdatePaintCache(viewport, 1);
        if (!cache || !ShowsPlacement(*cache.Value(), placement.first) ||
            !ShowsPlacement(*cache.Value(), placement.last) ||
            ShowsPlacement(*cache.Value(), placement.outside)) {
            return false;
        }

        if (!app.RoutePointer({PointerEventType::Up, placement.point}) ||
            !app.UpdatePaintCache(viewport, 1) ||
            !timeline->GetGeometry().Contains(placement.first) ||
            !timeline->GetGeometry().Contains(placement.last) ||
            timeline->GetGeometry().Contains(placement.outside) ||
            !root->RestoreLayout(1, original.View()) || !app.UpdatePaintCache(viewport, 1)) {
            return false;
        }
    }

    // 분리 창에서 돌아온 탭도 같은 루트 분할 경로로 배치한다.
    auto floating = SharedPtr<DockManager>::TryMake(allocator, allocator);
    return floating && root->TransferTabTo("Timeline", *floating.Value()) &&
           app.UpdatePaintCache(viewport, 1) &&
           root->AcceptDrop(*floating.Value(), "Timeline", cases[0].point) &&
           app.UpdatePaintCache(viewport, 1) && timeline->GetGeometry().Contains(cases[0].first) &&
           timeline->GetGeometry().Contains(cases[0].last) &&
           !timeline->GetGeometry().Contains(cases[0].outside) &&
           root->RestoreLayout(1, original.View()) && app.UpdatePaintCache(viewport, 1);
}

int VerifyDockWindows(Allocator& allocator, Application& main,
                      const SharedPtr<DockManager>& root) noexcept {
    auto made = SharedPtr<DockManager>::TryMake(allocator, allocator);
    if (!made || !root->TransferTabTo("Controls", *made.Value())) {
        return 30;
    }

    auto floating = Move(made.Value());
    Application second(allocator);
    second.SetRoot(floating);
    const auto viewport = Geometry::Root({640, 420}, {});
    if (!main.UpdatePaintCache(Geometry::Root({1120, 680}, {}), 1) ||
        !second.UpdatePaintCache(viewport, 1)) {
        return 31;
    }

    // 두 번째 창의 왼쪽 위를 첫 번째 창 기준 (1200, 0)으로 둔다.
    root->SetWindowHitTest(DockWindowHitTest::CreateLambda(
        [floating](Vec2 point, Vec2& local) noexcept -> SharedPtr<DockManager> {
            local = point - Vec2{1200, 0};
            return floating->GetGeometry().Contains(local) ? floating : SharedPtr<DockManager>{};
        }));
    const Vec2 rightGuide{1552, 224};
    constexpr bool cancelDrag[] = {true, false};
    for (bool cancel : cancelDrag) {
        const auto down = main.RoutePointer({PointerEventType::Down, {50, 12}});
        FloatingTabPreview preview{};
        const auto outside = main.RoutePointer({PointerEventType::Move, {1160, 200}});
        PaintList image(allocator);
        if (!down || !outside || !root->GetFloatingPreview(preview) ||
            preview.origin != Vec2{1110, 188} || !root->PaintFloatingPreview(image)) {
            return 36;
        }

        // 실제 패널의 토성 텍스처가 미리보기 본문에도 들어 있어야 한다.
        bool hasPlanet = false;
        for (const auto& box : image.Boxes()) {
            if (!box.resource.IsNull() && box.geometry.Contains(preview.size * 0.5f)) {
                hasPlanet = true;
            }
        }

        if (!hasPlanet) {
            return 37;
        }

        const auto move = main.RoutePointer({PointerEventType::Move, rightGuide});
        auto cache = second.UpdatePaintCache(viewport, 1);
        if (root->GetFloatingPreview(preview) || !down.Value() || !move || !move.Value() ||
            !cache || !ShowsPlacement(*cache.Value(), {550, 300}) ||
            ShowsPlacement(*cache.Value(), {80, 300})) {
            return 32;
        }

        const auto end = main.RoutePointer(
            {cancel ? PointerEventType::Cancel : PointerEventType::Up, rightGuide});
        cache = second.UpdatePaintCache(viewport, 1);
        if (!end || !cache || ShowsPlacement(*cache.Value(), {550, 300}) ||
            floating->IsOpen("Preview") == cancel) {
            return 33;
        }
    }

    root->SetWindowHitTest({});
    floating->CloseWindowTabs();
    if (!floating->ActiveChildren().IsEmpty() || !floating->TransferTabTo("Preview", *root) ||
        !floating->TransferTabTo("Controls", *root) || root->IsOpen("Preview") ||
        root->IsOpen("Controls") || !root->IsOpen("Timeline") || !root->OpenTab("Controls")) {
        return 34;
    }

    return 0;
}

} // namespace

int VerifyWorkspaceInput(Allocator& allocator, const SharedPtr<DockManager>& root,
                         const SharedPtr<CommandList>& commands) noexcept {
    Application app(allocator);
    app.SetRoot(root);
    if (!app.SetCommands({}, commands)) {
        return 19;
    }

    const auto viewport = Geometry::Root({1120, 680}, {});
    auto cache = app.UpdatePaintCache(viewport, 1);
    if (!cache || !ShowsFill(*cache.Value(), {100, 508}) || ShowsFill(*cache.Value(), {650, 508})) {
        return 20;
    }

    // 슬라이더를 끝으로 옮기고 체크박스의 한글 레이블과 초기화 버튼을 실제 좌표로 누른다.
    if (!Click(app, {1090, 232}) || !app.UpdatePaintCache(viewport, 1) ||
        !ShowsFill(*cache.Value(), {650, 508})) {
        return 21;
    }

    if (!Click(app, {850, 325}) || !app.UpdatePaintCache(viewport, 1) ||
        ShowsFill(*cache.Value(), {100, 508})) {
        return 22;
    }

    if (!Click(app, {850, 325}) || !app.UpdatePaintCache(viewport, 1) ||
        !ShowsFill(*cache.Value(), {100, 508})) {
        return 23;
    }

    if (!Click(app, {830, 389}) || !app.UpdatePaintCache(viewport, 1) ||
        ShowsFill(*cache.Value(), {100, 508})) {
        return 24;
    }

    if (!Click(app, {360, 610}) || !app.UpdatePaintCache(viewport, 1) ||
        !ShowsFill(*cache.Value(), {310, 508}) || ShowsFill(*cache.Value(), {600, 508})) {
        return 26;
    }

    const auto end = app.RouteKey(0, input::InputEvent::KeyDown(input::Key::End));
    if (!end || !end.Value() || !app.UpdatePaintCache(viewport, 1) ||
        !ShowsFill(*cache.Value(), {650, 508})) {
        return 27;
    }

    if (!app.UpdatePaintCache(viewport, 1) || cache.Value()->LastUpdate().paintCalls != 0) {
        return 25;
    }

    if (!VerifySplitterCursor(app)) {
        return 35;
    }

    if (!VerifyOuterDocking(allocator, app, root)) {
        return 38;
    }

    return VerifyDockWindows(allocator, app, root);
}

} // namespace rocks::ui
