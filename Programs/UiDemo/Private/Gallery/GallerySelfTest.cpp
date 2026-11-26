#include "GallerySelfTest.h"
#include <Core/ScopeExit.h>
#include <UiCore/Styles/CoreStyles.h>

#include <UiFramework/Application.h>

namespace rocks::ui::gallery {

namespace {

// self-test 창 크기다(갤러리 기본 창과 같다).
constexpr Vec2 Viewport{1280, 800};
// 탭 끌기 판정 거리(5)를 넘기는 중간 이동 거리다.
constexpr float DragStep = 12;

enum Failure : int {
    LayoutFailed = 40,
    WidgetsNotForeground = 41,
    RowNotDisabled = 42,
    DisabledBrushMissing = 43,
    RowNotRestored = 44,
    HoverForegroundMissing = 45,
    TabSwitchFailed = 46,
    DockingMissing = 47,
    TabDragFailed = 48,
    FocusOutlineMissing = 49,
    FocusOutlineNotCleared = 50,
    ToolTipMissing = 51,
    TabHeaderMissing = 52,
    TabClickIgnored = 53
};

// 툴팁 확인용 시계다(지연을 기다리지 않고 결정적으로 연다).
double galleryTipClock = 0;
double ReadGalleryTipClock() noexcept {
    return galleryTipClock;
}

Optional<Vec2> Center(const Element* element) noexcept {
    if (!element) {
        return {};
    }

    auto bounds = element->GetGeometry().WindowBounds();
    if (!bounds) {
        return {};
    }

    return (bounds.Value().min + bounds.Value().max) * 0.5f;
}

bool Click(Application& app, Vec2 point) noexcept {
    auto down = app.RoutePointer({PointerEventType::Down, point});
    auto up = app.RoutePointer({PointerEventType::Up, point});
    return down && down.Value() && up && up.Value();
}

bool Drag(Application& app, Vec2 from, Vec2 to) noexcept {
    // 누름이 처리되지 않으면 끌기가 시작되지 않은 것이다.
    auto down = app.RoutePointer({PointerEventType::Down, from});
    if (!down || !down.Value()) {
        return false;
    }

    auto step = app.RoutePointer({PointerEventType::Move, from + Vec2{DragStep, DragStep}});
    auto move = app.RoutePointer({PointerEventType::Move, to});
    auto up = app.RoutePointer({PointerEventType::Up, to});
    return step && move && up && up.Value();
}

// within 안에 초점 외곽선(투명 채움 + Primary 또는 흰 외곽선 둥근 상자)이 있는지 본다.
bool HasFocusOutline(const CacheRoot& cache, const Element& within,
                     const ThemeColors& theme) noexcept {
    auto bounds = within.GetGeometry().WindowBounds();
    if (!bounds) {
        return false;
    }

    for (const PaintBox& box : cache.List().Boxes()) {
        auto boxBounds = box.geometry.WindowBounds();
        // Primary 바탕 버튼은 흰 외곽선이다(FocusOutlineOnAccentStyle).
        const bool outlined = box.outlineColor == theme.Get(ThemeSlot::Primary) ||
                              box.outlineColor == theme.Get(ThemeSlot::White);
        if (box.roundedBox && box.color.a == 0 && outlined && boxBounds &&
            boxBounds.Value().min.x >= bounds.Value().min.x &&
            boxBounds.Value().max.x <= bounds.Value().max.x) {
            return true;
        }
    }

    return false;
}

// bounds 안에 color(알파까지 같은) 상자가 있는지 본다.
bool HasBox(const CacheRoot& cache, const Element& within, LinearColor color) noexcept {
    auto bounds = within.GetGeometry().WindowBounds();
    if (!bounds) {
        return false;
    }

    for (const PaintBox& box : cache.List().Boxes()) {
        auto boxBounds = box.geometry.WindowBounds();
        if (!boxBounds) {
            continue;
        }

        const Vec2 middle = (boxBounds.Value().min + boxBounds.Value().max) * 0.5f;
        const bool inside = middle.x >= bounds.Value().min.x && middle.x <= bounds.Value().max.x &&
                            middle.y >= bounds.Value().min.y && middle.y <= bounds.Value().max.y;
        if (inside && box.color == color) {
            return true;
        }
    }

    return false;
}

} // namespace

int VerifyGallery(Allocator& allocator, GalleryWindow& gallery,
                  const SharedPtr<DockManager>& area) noexcept {
    Application app(allocator);
    app.SetRoot(area);
    if (!app.SetCommands({}, gallery.Commands())) {
        return LayoutFailed;
    }

    const auto viewport = Geometry::Root(Viewport, {});
    auto cache = app.UpdatePaintCache(viewport, 1);
    if (!cache) {
        return LayoutFailed;
    }

    if (!area->IsActive(GalleryWindow::WidgetsTab)) {
        return WidgetsNotForeground;
    }

    // 행 0(Primary Button)의 활성 체크 상자를 끄면 행이 비활성이고, 버튼은 비활성 전용 brush
    // RB(Dropdown)를 효과 없이(알파 1) 그린다.
    const GalleryGrid* widgets = gallery.Widgets();
    if (!widgets || widgets->RowCount() < 3) {
        return LayoutFailed;
    }

    const auto toggle = Center(widgets->RowToggle(0).Get());
    if (!toggle || !Click(app, *toggle) || widgets->RowContent(0)->IsEnabled()) {
        return RowNotDisabled;
    }

    cache = app.UpdatePaintCache(viewport, 1);
    const ThemeColors& theme = app.Theme();
    if (!cache ||
        !HasBox(*cache.Value(), *widgets->RowContent(0), theme.Get(ThemeSlot::Dropdown))) {
        return DisabledBrushMissing;
    }

    if (!Click(app, *toggle) || !widgets->RowContent(0)->IsEnabled()) {
        return RowNotRestored;
    }

    // 행 2(Simple Button)에 올리면 레이블이 ForegroundHover로 그려진다(보통은 Foreground).
    const SharedPtr<Element> simpleRow = widgets->RowContent(2);
    const auto simple = Center(simpleRow ? simpleRow->ChildAt(0) : nullptr);
    if (!simple || !app.RoutePointer({PointerEventType::Move, *simple})) {
        return HoverForegroundMissing;
    }

    cache = app.UpdatePaintCache(viewport, 1);
    if (!cache || !HasBox(*cache.Value(), *simpleRow, theme.Get(ThemeSlot::ForegroundHover))) {
        return HoverForegroundMissing;
    }

    // 행 0 안으로 Tab 이동하면 Primary Button에 초점 외곽선이 보이고, 누르면 사라진다(UW4f1).
    const SharedPtr<Element> primaryRow = widgets->RowContent(0);
    auto moved = app.MoveFocusWithin(0, primaryRow, true, true, FocusCause::Navigation);
    cache = app.UpdatePaintCache(viewport, 1);
    if (!moved || !moved.Value() || !cache ||
        !HasFocusOutline(*cache.Value(), *primaryRow, theme)) {
        return FocusOutlineMissing;
    }

    const auto primary = Center(app.GetFocus(0).Get());
    if (!primary || !Click(app, *primary)) {
        return FocusOutlineNotCleared;
    }

    cache = app.UpdatePaintCache(viewport, 1);
    if (!cache || HasFocusOutline(*cache.Value(), *primaryRow, theme)) {
        return FocusOutlineNotCleared;
    }

    // Tool Tip 행의 첫 버튼("Hover me")에 올리면 지연 뒤 밝은 글자 툴팁이 열린다(UW4t).
    Element* tipOwner = nullptr;
    for (usize row = 0; row < widgets->RowCount() && !tipOwner; ++row) {
        const SharedPtr<Element> content = widgets->RowContent(row);
        Element* first = content ? content->ChildAt(0) : nullptr;
        tipOwner = first && first->HasToolTip() ? first : nullptr;
    }

    const auto tipPoint = Center(tipOwner);
    galleryTipClock = 0;
    app.SetToolTipClock(&ReadGalleryTipClock);
    // 나타나기(fade)를 끄고 연 첫 프레임의 바탕을 그대로 본다(앱 설정은 끝에 되돌린다).
    const double savedDelay = app.ToolTipDelay();
    const double savedFade = app.ToolTipFade();
    (void)app.SetToolTipTiming(savedDelay, 0);
    ROCKS_ON_SCOPE_EXIT {
        app.SetToolTipClock(nullptr);
        (void)app.SetToolTipTiming(savedDelay, savedFade);
    };
    if (!tipPoint || !app.RoutePointer({PointerEventType::Move, *tipPoint})) {
        return ToolTipMissing;
    }

    galleryTipClock = 1;
    cache = app.UpdatePaintCache(viewport, 1);
    if (cache) {
        cache = app.UpdatePaintCache(viewport, 1);
    }

    bool bright = false;
    if (cache) {
        for (const PaintBox& box : cache.Value()->List().Boxes()) {
            bright = bright || (box.roundedBox && box.color == BrightToolTipFill);
        }
    }

    const bool tipOpen = app.IsToolTipOpen(0);
    app.CloseToolTip(0);
    if (!tipOpen || !bright) {
        return ToolTipMissing;
    }

    // 툴팁 층을 닫으면 Application이 감싸개를 풀고 캐시를 비우므로 다시 배치한다.
    if (!app.UpdatePaintCache(viewport, 1)) {
        return LayoutFailed;
    }

    // 탭 머리를 실제 위치로 눌러 배치 탭으로 간다.
    const auto layoutTab = Center(area->TabHeader(GalleryWindow::LayoutTab));
    if (!layoutTab) {
        return TabHeaderMissing;
    }

    if (!Click(app, *layoutTab)) {
        return TabClickIgnored;
    }

    if (!app.UpdatePaintCache(viewport, 1) || !area->IsActive(GalleryWindow::LayoutTab)) {
        return TabSwitchFailed;
    }

    // 배치 탭 도킹 영역: "Outliner" 탭을 "Viewport" 탭 머리 위로 끌어 같은 묶음에 넣는다.
    const SharedPtr<DockManager>& docking = gallery.LayoutDocking();
    if (!docking || !docking->IsActive("Viewport") || !docking->IsActive("Outliner")) {
        return DockingMissing;
    }

    const auto from = Center(docking->TabHeader("Outliner"));
    const auto to = Center(docking->TabHeader("Viewport"));
    if (!from || !to || !Drag(app, *from, *to) || !app.UpdatePaintCache(viewport, 1)) {
        return TabDragFailed;
    }

    if (docking->IsActive("Viewport") || !docking->IsActive("Outliner")) {
        return TabDragFailed;
    }

    return 0;
}

} // namespace rocks::ui::gallery
