#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application/Application.h>

// hit grid 방향 탐색 시험이다(Docs/Design/UiToolkit 20.9절 — UE FHittestGrid::FindFocusableWidget
// 366–570행, FindNextFocusableWidgetDefault 598–640행): 진행 축으로 쓸어 넓힌 띠 밖의 후보로는 가지
// 않고, 띠 안에서는 가까운 쪽, 비활성 조상 아래는 건너뛰며, scope 밖은 후보가 아니다.

namespace rocks::ui {

namespace {

constexpr Vec2 SearchViewport{400, 200};

class SearchBox final : public Element {
public:
    explicit SearchBox(Allocator& allocator) noexcept : Element(allocator) {
    }

    bool SupportsKeyboardFocus() const noexcept override {
        return true;
    }
};

Result<SharedPtr<SearchBox>> Place(Allocator& allocator, Element& parent, Vec2 origin) noexcept {
    auto box = SharedPtr<SearchBox>::TryMake(allocator, allocator);
    if (!box) {
        return box;
    }

    if (auto added = parent.TryAddChild(box.Value()); !added) {
        return Result<SharedPtr<SearchBox>>::Failure(added.Error());
    }

    box.Value()->SetGeometry(Geometry::Root({20, 20}, {1, 0, 0, 1, origin.x, origin.y}));
    return box;
}

struct SearchBench {
    explicit SearchBench(Allocator& allocator) noexcept : app(allocator) {
    }

    SharedPtr<Element> root;
    Application app;

    bool Paint() noexcept {
        return static_cast<bool>(app.UpdatePaintCache(Geometry::Root(SearchViewport, {}), 1));
    }
};

bool MakeRoot(Allocator& allocator, SearchBench& bench) noexcept {
    auto root = SharedPtr<Element>::TryMake(allocator, allocator);
    if (!root) {
        return false;
    }

    bench.root = root.Value();
    bench.app.SetRoot(bench.root);
    return true;
}

} // namespace

TEST(UiFramework_DirectionalSearchStaysInsideTheSweptBand) {
    SystemAllocator allocator;
    SearchBench bench(allocator);
    REQUIRE(MakeRoot(allocator, bench));
    auto start = Place(allocator, *bench.root, {0, 0});
    // 오른쪽 아래(띠 밖)에만 후보가 있다 — 거리로는 가깝지만 UE는 가지 않는다.
    auto offBand = Place(allocator, *bench.root, {40, 60});
    REQUIRE(start && offBand);
    REQUIRE(bench.Paint());
    REQUIRE(bench.app.SetFocus(0, start.Value()));
    auto moved = bench.app.Navigate(0, FocusDirection::Right, {}, FocusCause::Navigation);
    REQUIRE(moved);
    CHECK(!moved.Value());
    CHECK(bench.app.GetFocus(0).Get() == start.Value().Get());
    // 띠 안의 후보 둘 중 가까운 쪽이다(먼 것은 다른 셀 줄이다).
    auto far = Place(allocator, *bench.root, {300, 0});
    auto near = Place(allocator, *bench.root, {140, 0});
    REQUIRE(far && near);
    REQUIRE(bench.Paint());
    REQUIRE(bench.app.Navigate(0, FocusDirection::Right, {}, FocusCause::Navigation));
    CHECK(bench.app.GetFocus(0).Get() == near.Value().Get());
    REQUIRE(bench.app.Navigate(0, FocusDirection::Right, {}, FocusCause::Navigation));
    CHECK(bench.app.GetFocus(0).Get() == far.Value().Get());
    // 왼쪽으로 돌아온다.
    REQUIRE(bench.app.Navigate(0, FocusDirection::Left, {}, FocusCause::Navigation));
    CHECK(bench.app.GetFocus(0).Get() == near.Value().Get());
}

TEST(UiFramework_DirectionalSearchSkipsCandidatesUnderDisabledAncestors) {
    SystemAllocator allocator;
    SearchBench bench(allocator);
    REQUIRE(MakeRoot(allocator, bench));
    auto start = Place(allocator, *bench.root, {0, 0});
    auto panel = SharedPtr<Element>::TryMake(allocator, allocator);
    REQUIRE(start && panel);
    REQUIRE(bench.root->TryAddChild(panel.Value()));
    panel.Value()->SetGeometry(Geometry::Root({60, 40}, {1, 0, 0, 1, 30, 0}));
    auto inside = Place(allocator, *panel.Value(), {40, 0});
    auto beyond = Place(allocator, *bench.root, {200, 0});
    REQUIRE(inside && beyond);
    panel.Value()->SetEnabled(false);
    REQUIRE(bench.Paint());
    REQUIRE(bench.app.SetFocus(0, start.Value()));
    REQUIRE(bench.app.Navigate(0, FocusDirection::Right, {}, FocusCause::Navigation));
    CHECK(bench.app.GetFocus(0).Get() == beyond.Value().Get());
}

TEST(UiFramework_DirectionalSearchRespectsTheScope) {
    SystemAllocator allocator;
    SearchBench bench(allocator);
    REQUIRE(MakeRoot(allocator, bench));
    auto popup = SharedPtr<Element>::TryMake(allocator, allocator);
    REQUIRE(popup);
    REQUIRE(bench.root->TryAddChild(popup.Value()));
    popup.Value()->SetGeometry(Geometry::Root({100, 40}, {}));
    auto first = Place(allocator, *popup.Value(), {0, 0});
    auto outside = Place(allocator, *bench.root, {150, 0});
    REQUIRE(first && outside);
    REQUIRE(bench.Paint());
    REQUIRE(bench.app.SetFocus(0, first.Value()));
    // 팝업 범위(scope) 안에서 찾으면 밖의 후보로 새지 않는다.
    auto moved =
        bench.app.Navigate(0, FocusDirection::Right, popup.Value(), FocusCause::Navigation);
    REQUIRE(moved);
    CHECK(!moved.Value());
    CHECK(bench.app.GetFocus(0).Get() == first.Value().Get());
    // 범위 없이 찾으면 간다.
    REQUIRE(bench.app.Navigate(0, FocusDirection::Right, {}, FocusCause::Navigation));
    CHECK(bench.app.GetFocus(0).Get() == outside.Value().Get());
}

TEST(UiFramework_DirectionalSearchWrapsAtTheBoundary) {
    SystemAllocator allocator;
    SearchBench bench(allocator);
    REQUIRE(MakeRoot(allocator, bench));
    auto top = Place(allocator, *bench.root, {0, 0});
    auto bottom = Place(allocator, *bench.root, {0, 150});
    REQUIRE(top && bottom);
    REQUIRE(bench.root->SetNavigationRule(FocusDirection::Down, {NavigationRule::Wrap, {}}));
    REQUIRE(bench.Paint());
    REQUIRE(bench.app.SetFocus(0, bottom.Value()));
    REQUIRE(bench.app.Navigate(0, FocusDirection::Down, {}, FocusCause::Navigation));
    CHECK(bench.app.GetFocus(0).Get() == top.Value().Get());
    // Stop이면 경계 밖으로도, 감아서도 가지 않는다.
    REQUIRE(bench.root->SetNavigationRule(FocusDirection::Down, {NavigationRule::Stop, {}}));
    REQUIRE(bench.app.SetFocus(0, bottom.Value()));
    REQUIRE(bench.app.Navigate(0, FocusDirection::Down, {}, FocusCause::Navigation));
    CHECK(bench.app.GetFocus(0).Get() == bottom.Value().Get());
}

} // namespace rocks::ui
