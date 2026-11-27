#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Application/InputAdapter.h>

// 키 navigation 변환 시험이다(Docs/Design/UiToolkit 20.7절 — UE SWidget::OnKeyDown 415–427행,
// FNavigationConfig, AttemptNavigation): 초점 요소가 방향키를 navigation으로 바꿔 조상은 그 키를
// 받지 않고, 초점이 없으면 navigation이 없으며, 대응을 바꿀 수 있고, 조상의 OnNavigation이 방향을
// 맡는다.

namespace rocks::ui {

namespace {

// 키를 세는 조상이다(초점은 받지 않는다). blockDown이면 아래 방향을 스스로 처리한다.
class KeySpy final : public Element {
public:
    explicit KeySpy(Allocator& allocator) noexcept : Element(allocator) {
    }

    KeyReply OnKeyEvent(const KeyEvent& key) noexcept override {
        ++keys;
        return Element::OnKeyEvent(key);
    }

    NavigationBoundary OnNavigation(uint32 user, FocusDirection direction) noexcept override {
        if (blockDown && direction == FocusDirection::Down) {
            ++handledDowns;
            return NavigationBoundary::Handled();
        }

        return Element::OnNavigation(user, direction);
    }

    int keys = 0;
    int handledDowns = 0;
    bool blockDown = false;
};

class FocusBox final : public Element {
public:
    explicit FocusBox(Allocator& allocator) noexcept : Element(allocator) {
    }

    bool SupportsKeyboardFocus() const noexcept override {
        return true;
    }
};

struct NavigationBench {
    explicit NavigationBench(Allocator& allocator) noexcept : app(allocator), adapter(app) {
    }

    SharedPtr<KeySpy> spy;
    SharedPtr<FocusBox> left;
    SharedPtr<FocusBox> right;
    SharedPtr<FocusBox> below;
    Application app;
    InputAdapter adapter;
};

bool Build(Allocator& allocator, NavigationBench& bench) noexcept {
    auto spy = SharedPtr<KeySpy>::TryMake(allocator, allocator);
    auto left = SharedPtr<FocusBox>::TryMake(allocator, allocator);
    auto right = SharedPtr<FocusBox>::TryMake(allocator, allocator);
    auto below = SharedPtr<FocusBox>::TryMake(allocator, allocator);
    if (!spy || !left || !right || !below || !spy.Value()->TryAddChild(left.Value()) ||
        !spy.Value()->TryAddChild(right.Value()) || !spy.Value()->TryAddChild(below.Value())) {
        return false;
    }

    bench.spy = spy.Value();
    bench.left = left.Value();
    bench.right = right.Value();
    bench.below = below.Value();
    // 왼쪽(0–10), 오른쪽(20–30), 왼쪽 아래(0–10, 20–30)에 놓는다.
    bench.spy->SetGeometry(Geometry::Root({40, 40}, {}));
    bench.left->SetGeometry(Geometry::Root({10, 10}, {}));
    bench.right->SetGeometry(Geometry::Root({10, 10}, {1, 0, 0, 1, 20, 0}));
    bench.below->SetGeometry(Geometry::Root({10, 10}, {1, 0, 0, 1, 0, 20}));
    bench.app.SetRoot(bench.spy);
    return static_cast<bool>(bench.app.SetFocus(0, bench.left));
}

bool Press(NavigationBench& bench, input::Key key) noexcept {
    const auto routed = bench.adapter.Route(input::InputEvent::KeyDown(key));
    (void)bench.adapter.Route(input::InputEvent::KeyUp(key));
    return routed && routed.Value();
}

} // namespace

TEST(UiFramework_FocusedElementTurnsArrowsIntoNavigation) {
    SystemAllocator allocator;
    NavigationBench bench(allocator);
    REQUIRE(Build(allocator, bench));
    // 초점 요소가 오른쪽을 navigation으로 바꾼다 — 조상은 그 키를 받지 않는다.
    CHECK(Press(bench, input::Key::Right));
    CHECK(bench.app.GetFocus(0).Get() == bench.right.Get());
    CHECK(bench.spy->keys == 1); // KeyUp만 올라왔다(KeyUp은 navigation이 아니다)
    // navigation이 아닌 키(Home)는 조상까지 올라간다.
    const int before = bench.spy->keys;
    CHECK(!Press(bench, input::Key::Home));
    CHECK(bench.spy->keys == before + 2);
    // 찾을 곳이 없어도 navigation 키는 처리된 것이다(UE SetNavigation).
    CHECK(Press(bench, input::Key::Right));
    CHECK(bench.app.GetFocus(0).Get() == bench.right.Get());
}

TEST(UiFramework_NoFocusMeansNoNavigation) {
    SystemAllocator allocator;
    NavigationBench bench(allocator);
    REQUIRE(Build(allocator, bench));
    REQUIRE(bench.app.SetFocus(0, SharedPtr<Element>{}));
    CHECK(!Press(bench, input::Key::Right));
    CHECK(!bench.app.GetFocus(0));
}

TEST(UiFramework_NavigationConfigRemapsKeysPerUser) {
    SystemAllocator allocator;
    NavigationBench bench(allocator);
    REQUIRE(Build(allocator, bench));
    NavigationConfig config;
    config.keyNavigation = false;
    REQUIRE(bench.app.SetNavigationConfig(0, config));
    // 키 navigation을 끄면 방향키는 navigation이 아니다 — 조상까지 올라간다.
    const int before = bench.spy->keys;
    CHECK(!Press(bench, input::Key::Right));
    CHECK(bench.app.GetFocus(0).Get() == bench.left.Get());
    CHECK(bench.spy->keys == before + 2);
    // 다른 키를 방향에 잇는다.
    config.keyNavigation = true;
    REQUIRE(config.SetKeyRule(input::Key::D, FocusDirection::Right));
    REQUIRE(bench.app.SetNavigationConfig(0, config));
    CHECK(Press(bench, input::Key::D));
    CHECK(bench.app.GetFocus(0).Get() == bench.right.Get());
    // 다른 사용자는 기본 대응이다.
    CHECK(bench.app.GetNavigationConfig(1).DirectionFromKey(
              input::InputEvent::KeyDown(input::Key::D), {}) == Optional<FocusDirection>{});
}

TEST(UiFramework_TabNavigatesOnlyWithoutCommandModifiers) {
    const NavigationConfig& config = NavigationConfig::Default();
    const auto tab = input::InputEvent::KeyDown(input::Key::Tab);
    CHECK(config.DirectionFromKey(tab, {}) == Optional<FocusDirection>(FocusDirection::Next));
    KeyChord shift;
    shift.shift = true;
    CHECK(config.DirectionFromKey(tab, shift) ==
          Optional<FocusDirection>(FocusDirection::Previous));
    KeyChord control;
    control.control = true;
    CHECK(!config.DirectionFromKey(tab, control));
    // KeyUp은 navigation이 아니고, 확인·뒤로 동작은 누름·뗌 모두다.
    CHECK(!config.DirectionFromKey(input::InputEvent::KeyUp(input::Key::Left), {}));
    CHECK(config.ActionFromKey(input::InputEvent::KeyUp(input::Key::Space)) ==
          NavigationAction::Accept);
    CHECK(config.ActionFromKey(input::InputEvent::KeyDown(input::Key::Escape)) ==
          NavigationAction::Back);
}

TEST(UiFramework_AncestorOnNavigationCanHandleADirection) {
    SystemAllocator allocator;
    NavigationBench bench(allocator);
    REQUIRE(Build(allocator, bench));
    bench.spy->blockDown = true;
    // 조상이 아래 방향을 스스로 처리하면 기하 탐색을 하지 않는다(초점 그대로).
    CHECK(Press(bench, input::Key::Down));
    CHECK(bench.spy->handledDowns == 1);
    CHECK(bench.app.GetFocus(0).Get() == bench.left.Get());
    // 처리하지 않으면 기하 탐색으로 아래 요소에 간다.
    bench.spy->blockDown = false;
    CHECK(Press(bench, input::Key::Down));
    CHECK(bench.app.GetFocus(0).Get() == bench.below.Get());
}

} // namespace rocks::ui
