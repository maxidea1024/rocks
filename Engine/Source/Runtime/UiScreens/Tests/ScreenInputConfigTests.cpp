#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiScreens/Screen.generated.h>
#include <UiFramework/Layout/Overlay.h>
#include <UiScreens/ScreenInputRouter.h>

// 화면 입력 설정 적용 시험이다(Docs/Design/UiToolkit 21.3절 — UE
// UCommonUIActionRouterBase::ApplyUIInputConfig, CommonUIActionRouterBase.cpp 1866–1960행): 설정이
// 바뀔 때만 적용, true를 부른 적 없이 무시 해제 없음, Menu 진입 때 키 풀기 한 번, 붙잡지 않으면
// 커서 보임.

namespace rocks::ui {

namespace {

struct RecordingInputHost final : ScreenInputHost {
    void SetIgnoreMoveInput(bool ignore) noexcept override {
        ++moveCalls;
        ignoreMove = ignore;
    }

    void SetIgnoreLookInput(bool ignore) noexcept override {
        ++lookCalls;
        ignoreLook = ignore;
    }

    void SetMouseCapture(bool isCaptured, bool isCursorVisible) noexcept override {
        ++captureCalls;
        captured = isCaptured;
        cursorVisible = isCursorVisible;
    }

    void FlushPressedKeys() noexcept override {
        ++flushes;
    }

    uint32 moveCalls = 0;
    uint32 lookCalls = 0;
    uint32 captureCalls = 0;
    uint32 flushes = 0;
    bool ignoreMove = false;
    bool ignoreLook = false;
    bool captured = false;
    bool cursorVisible = false;
};

// 초점을 받아 패드 키 취소(Application::CancelGamepadKeys) 횟수를 센다.
class InputConfigCancelProbe final : public Element {
    ROCKS_UI_ELEMENT(InputConfigCancelProbe, Element)

public:
    explicit InputConfigCancelProbe(Allocator& allocator) noexcept : Element(allocator) {
    }

    bool SupportsKeyboardFocus() const noexcept override {
        return true;
    }

    void OnKeyCancelled(uint32 userId, input::Key key) noexcept override {
        Element::OnKeyCancelled(userId, key);
        ++cancels;
    }

    uint32 cancels = 0;
};

} // namespace

TEST(UiScreens_InputConfigAppliesOnlyChangesInUeOrder) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterScreenTypes(modules));
    ObjectRegistry objects(allocator, types, {0x534352, 2});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    Application app(allocator);
    auto stack = SharedPtr<ScreenStack>::TryMake(allocator, objects, app, 3u);
    auto root = SharedPtr<Overlay>::TryMake(allocator, allocator);
    auto focused = SharedPtr<InputConfigCancelProbe>::TryMake(allocator, allocator);
    REQUIRE(stack && root && focused);
    REQUIRE(root.Value()->TryAddChild(stack.Value()));
    REQUIRE(root.Value()->TryAddChild(focused.Value()));
    app.SetRoot(root.Value());
    ScreenInputRouter router(allocator, app, *stack.Value());
    // 호스트가 없으면 적용하지 않는다.
    CHECK(!router.ApplyInputConfig());

    RecordingInputHost host;
    router.SetInputHost(&host);
    auto menu = stack.Value()->AddScreen(ClassRef<Screen>::Of<Screen>());
    REQUIRE(menu);
    // 기본 화면 설정(Menu, 커서 보임, 붙잡지 않음, 이동·시선 무시)의 첫 적용이다.
    CHECK(router.ApplyInputConfig());
    CHECK(host.moveCalls == 1 && host.ignoreMove);
    CHECK(host.lookCalls == 1 && host.ignoreLook);
    CHECK(host.flushes == 1);
    CHECK(host.captureCalls == 1 && !host.captured && host.cursorVisible);
    // 같은 설정은 다시 적용하지 않는다(입력 처리 중에도).
    CHECK(!router.ApplyInputConfig());
    (void)router.Route(input::InputEvent::KeyDown(input::Key::A));
    CHECK(host.moveCalls == 1 && host.captureCalls == 1 && host.flushes == 1);

    // 게임 화면으로 바꾼다: 무시를 풀고 붙잡아 커서를 숨긴다. Menu로 들어가지 않으니 키 풀기는
    // 없다.
    auto screen = objects.ResolveForWrite<Screen>(access, menu.Value());
    REQUIRE(screen);
    screen.Value().Get().Value()->SetInputConfig(
        {ScreenInputMode::Game, false, true, false, false});
    CHECK(router.ApplyInputConfig());
    CHECK(host.moveCalls == 2 && !host.ignoreMove);
    CHECK(host.lookCalls == 2 && !host.ignoreLook);
    CHECK(host.flushes == 1);
    CHECK(host.captureCalls == 2 && host.captured && !host.cursorVisible);
    // 붙잡지 않으면 showCursor가 false여도 커서는 보인다(1938–1949행).
    screen.Value().Get().Value()->SetInputConfig(
        {ScreenInputMode::Game, false, false, false, false});
    CHECK(router.ApplyInputConfig());
    CHECK(!host.captured && host.cursorVisible);
    CHECK(host.moveCalls == 2);

    // 호스트를 바꾸면 이전 설정 없음부터다 — 무시하지 않는 설정이면 무시 해제를 부르지 않는다.
    // 이전 모드는 GameAndMenu로 본다(UE GetActiveInputMode 기본 ECommonInputMode::All —
    // CommonUIActionRouterBase.h 94행): Game을 처음 적용하면 모드가 바뀐 것이므로 눌린 UI 패드
    // 키를 취소한다(Menu가 아니라 키 풀기는 없다).
    REQUIRE(app.SetFocus(3, focused.Value()));
    const uint32 cancelsBefore = focused.Value()->cancels;
    RecordingInputHost fresh;
    router.SetInputHost(&fresh);
    CHECK(router.ApplyInputConfig());
    CHECK(fresh.moveCalls == 0 && fresh.lookCalls == 0);
    CHECK(fresh.flushes == 0);
    CHECK(fresh.captureCalls == 1);
    CHECK(focused.Value()->cancels == cancelsBefore + 1);
    // GameAndMenu를 처음 적용하면 모드가 그대로이므로 취소하지 않는다.
    screen.Value().Get().Value()->SetInputConfig(
        {ScreenInputMode::GameAndMenu, false, false, false, false});
    RecordingInputHost third;
    router.SetInputHost(&third);
    CHECK(router.ApplyInputConfig());
    CHECK(focused.Value()->cancels == cancelsBefore + 1);
    CHECK(third.flushes == 0);
    // 화면이 없으면 Game 기본값이다(ScreenContainer::InputConfig) — GameAndMenu에서 바뀌므로 다시
    // 적용하고 패드 키를 취소한다.
    REQUIRE(stack.Value()->RemoveScreen(menu.Value()));
    CHECK(router.ApplyInputConfig());
    CHECK(focused.Value()->cancels == cancelsBefore + 2);
    CHECK(!router.ApplyInputConfig());
    app.SetRoot({});
}

} // namespace rocks::ui
