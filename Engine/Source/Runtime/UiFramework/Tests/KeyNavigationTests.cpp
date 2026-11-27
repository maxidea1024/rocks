#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Application/InputAdapter.h>
#include <UiCore/ManualClock.h>

// 키 navigation 변환 시험이다(Docs/Design/UiToolkit 20.7절 — UE SWidget::OnKeyDown 415–427행,
// FNavigationConfig, AttemptNavigation): 초점 요소가 방향키를 navigation으로 바꿔 조상은 그 키를
// 받지 않고, 초점이 없으면 navigation이 없으며, 대응을 바꿀 수 있고, 조상의 OnNavigation이 방향을
// 맡는다.

namespace rocks::ui {

namespace {

// 키를 세는 조상이다(초점은 받지 않는다). blockDown이면 아래 방향을 스스로 처리한다.
class KeySpy final : public Element {
    ROCKS_UI_ELEMENT(KeySpy, Element)

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
    ROCKS_UI_ELEMENT(FocusBox, Element)

public:
    explicit FocusBox(Allocator& allocator) noexcept : Element(allocator) {
    }

    bool SupportsKeyboardFocus() const noexcept override {
        return true;
    }

    // 확인 동작(Enter·Space·패드 South)의 누름을 센다.
    KeyReply OnKeyEvent(const KeyEvent& key) noexcept override {
        if (key.action == NavigationAction::Accept &&
            key.input.type == input::InputEventType::KeyDown) {
            ++accepts;
            return KeyReply::Handled();
        }

        return Element::OnKeyEvent(key);
    }

    int accepts = 0;
};

input::GamepadSnapshot Pad(input::GamepadButton button) noexcept {
    input::GamepadSnapshot pad;
    pad.connected = true;
    pad.buttons = uint32{1} << static_cast<uint32>(button);
    return pad;
}

input::GamepadSnapshot Stick(float x, float y) noexcept {
    input::GamepadSnapshot pad;
    pad.connected = true;
    pad.axes[static_cast<uint32>(input::GamepadAxis::LeftX)] = x;
    pad.axes[static_cast<uint32>(input::GamepadAxis::LeftY)] = y;
    return pad;
}

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

bool PressNavigationKey(NavigationBench& bench, input::Key key) noexcept {
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
    CHECK(PressNavigationKey(bench, input::Key::Right));
    CHECK(bench.app.GetFocus(0).Get() == bench.right.Get());
    CHECK(bench.spy->keys == 1); // KeyUp만 올라왔다(KeyUp은 navigation이 아니다)
    // navigation이 아닌 키(Home)는 조상까지 올라간다.
    const int before = bench.spy->keys;
    CHECK(!PressNavigationKey(bench, input::Key::Home));
    CHECK(bench.spy->keys == before + 2);
    // 찾을 곳이 없어도 navigation 키는 처리된 것이다(UE SetNavigation).
    CHECK(PressNavigationKey(bench, input::Key::Right));
    CHECK(bench.app.GetFocus(0).Get() == bench.right.Get());
}

TEST(UiFramework_NoFocusMeansNoNavigation) {
    SystemAllocator allocator;
    NavigationBench bench(allocator);
    REQUIRE(Build(allocator, bench));
    REQUIRE(bench.app.SetFocus(0, SharedPtr<Element>{}));
    CHECK(!PressNavigationKey(bench, input::Key::Right));
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
    CHECK(!PressNavigationKey(bench, input::Key::Right));
    CHECK(bench.app.GetFocus(0).Get() == bench.left.Get());
    CHECK(bench.spy->keys == before + 2);
    // 다른 키를 방향에 잇는다.
    config.keyNavigation = true;
    REQUIRE(config.SetKeyRule(input::Key::D, FocusDirection::Right));
    REQUIRE(bench.app.SetNavigationConfig(0, config));
    CHECK(PressNavigationKey(bench, input::Key::D));
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
    CHECK(PressNavigationKey(bench, input::Key::Down));
    CHECK(bench.spy->handledDowns == 1);
    CHECK(bench.app.GetFocus(0).Get() == bench.left.Get());
    // 처리하지 않으면 기하 탐색으로 아래 요소에 간다.
    bench.spy->blockDown = false;
    CHECK(PressNavigationKey(bench, input::Key::Down));
    CHECK(bench.app.GetFocus(0).Get() == bench.below.Get());
}

TEST(UiFramework_GamepadButtonsTakeTheKeyPath) {
    SystemAllocator allocator;
    NavigationBench bench(allocator);
    REQUIRE(Build(allocator, bench));
    // D-pad는 초점 요소에서 navigation이 된다(20.8절). 누르고 있는 동안 다시 오지 않는다.
    auto routed = bench.app.RouteGamepadState(0, 0, Pad(input::GamepadButton::DPadRight));
    REQUIRE(routed);
    CHECK(routed.Value());
    CHECK(bench.app.GetFocus(0).Get() == bench.right.Get());
    REQUIRE(bench.app.RouteGamepadState(0, 0, Pad(input::GamepadButton::DPadRight)));
    CHECK(bench.app.GetFocus(0).Get() == bench.right.Get());
    REQUIRE(bench.app.RouteGamepadState(0, 0, input::GamepadSnapshot{.connected = true}));
    // South는 확인 동작이다(버튼류가 키 대신 동작으로 누름을 판정한다).
    REQUIRE(bench.app.RouteGamepadState(0, 0, Pad(input::GamepadButton::South)));
    CHECK(bench.right->accepts == 1);
    // 상태를 지우면 누르고 있던 버튼이 새 누름으로 다시 온다.
    bench.app.ResetGamepads();
    REQUIRE(bench.app.RouteGamepadState(0, 0, Pad(input::GamepadButton::South)));
    CHECK(bench.right->accepts == 2);
}

TEST(UiFramework_AnalogStickRepeatsOnFrameTime) {
    SystemAllocator allocator;
    NavigationBench bench(allocator);
    REQUIRE(Build(allocator, bench));
    ManualClock clock;
    bench.app.SetClock(clock.Function());
    bench.app.BeginFrame();
    // 임계값 밑은 navigation이 아니다.
    REQUIRE(bench.app.RouteGamepadState(0, 0, Stick(0.4f, 0)));
    CHECK(bench.app.GetFocus(0).Get() == bench.left.Get());
    // 넘는 순간 한 번 간다.
    REQUIRE(bench.app.RouteGamepadState(0, 0, Stick(0.6f, 0)));
    CHECK(bench.app.GetFocus(0).Get() == bench.right.Get());
    // 첫 반복은 0.5초 뒤다(그 전에는 다시 가지 않는다). 왼쪽으로 반복해 본다.
    REQUIRE(bench.app.RouteGamepadState(0, 0, Stick(-0.6f, 0)));
    CHECK(bench.app.GetFocus(0).Get() == bench.left.Get());
    REQUIRE(bench.app.RouteGamepadState(0, 0, Stick(0.6f, 0)));
    CHECK(bench.app.GetFocus(0).Get() == bench.right.Get());
    clock.Advance(0.4);
    bench.app.BeginFrame();
    REQUIRE(bench.app.SetFocus(0, bench.left));
    REQUIRE(bench.app.RouteGamepadState(0, 0, Stick(0.6f, 0)));
    CHECK(bench.app.GetFocus(0).Get() == bench.left.Get());
    clock.Advance(0.11);
    bench.app.BeginFrame();
    REQUIRE(bench.app.RouteGamepadState(0, 0, Stick(0.6f, 0)));
    CHECK(bench.app.GetFocus(0).Get() == bench.right.Get());
    // 그 뒤는 0.25초 간격이고, 세게 밀면(0.9 초과) 절반이다.
    CHECK(NavigationConfig::AnalogRepeatInterval(0.6f, 0) == 0.5);
    CHECK(NavigationConfig::AnalogRepeatInterval(0.6f, 1) == 0.25);
    CHECK(NavigationConfig::AnalogRepeatInterval(0.95f, 1) == 0.125);
    // 두 축이 모두 넘으면 더 크게 민 축이다(y 위가 양수).
    const auto& config = NavigationConfig::Default();
    CHECK(config.DirectionFromAnalog(0.6f, 0.8f) == Optional<FocusDirection>(FocusDirection::Up));
    CHECK(config.DirectionFromAnalog(-0.9f, -0.7f) ==
          Optional<FocusDirection>(FocusDirection::Left));
}

} // namespace rocks::ui
