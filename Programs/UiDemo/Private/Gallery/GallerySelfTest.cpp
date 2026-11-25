#include "GallerySelfTest.h"

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
    TabDragFailed = 48
};

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
    auto down = app.RoutePointer({PointerEventType::Down, from});
    auto step = app.RoutePointer({PointerEventType::Move, from + Vec2{DragStep, DragStep}});
    auto move = app.RoutePointer({PointerEventType::Move, to});
    auto up = app.RoutePointer({PointerEventType::Up, to});
    return down && step && move && up;
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

    // 탭 머리를 실제 위치로 눌러 배치 탭으로 간다. 머리는 탭 묶음 순서대로 앞에 있다.
    const Span<const SharedPtr<Element>> heads = area->ActiveChildren();
    constexpr usize LayoutHeader = 3;
    const auto layoutTab =
        Center(heads.Size() > LayoutHeader ? heads[LayoutHeader].Get() : nullptr);
    if (!layoutTab || !Click(app, *layoutTab) || !app.UpdatePaintCache(viewport, 1) ||
        !area->IsActive(GalleryWindow::LayoutTab)) {
        return TabSwitchFailed;
    }

    // 배치 탭 도킹 영역: "Outliner" 탭을 "Viewport" 탭 머리 위로 끌어 같은 묶음에 넣는다.
    const SharedPtr<DockManager>& docking = gallery.LayoutDocking();
    if (!docking || !docking->IsActive("Viewport") || !docking->IsActive("Outliner")) {
        return DockingMissing;
    }

    const Element* outliner = nullptr;
    const Element* viewportTab = nullptr;
    for (const SharedPtr<Element>& child : docking->ActiveChildren()) {
        const Element* label = child->ChildAt(0);
        const auto* text = label ? static_cast<const TextLabel*>(label) : nullptr;
        if (text && text->GetText() == "Outliner") {
            outliner = child.Get();
        } else if (text && text->GetText() == "Viewport") {
            viewportTab = child.Get();
        }
    }

    const auto from = Center(outliner);
    const auto to = Center(viewportTab);
    if (!from || !to || !Drag(app, *from, *to) || !app.UpdatePaintCache(viewport, 1)) {
        return TabDragFailed;
    }

    if (docking->IsActive("Viewport") || !docking->IsActive("Outliner")) {
        return TabDragFailed;
    }

    return 0;
}

} // namespace rocks::ui::gallery
