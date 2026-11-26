#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/AnchorCanvas.h>
#include <UiFramework/Application.h>
#include <UiFramework/MenuBar.h>
#include <UiFramework/MenuHeading.h>
#include <UiFramework/MenuSeparator.h>
#include <UiFramework/Overlay.h>
#include <UiFramework/WindowChrome.h>

namespace rocks::ui {

namespace {

double menuClock = 0;
double ReadMenuClock() noexcept {
    return menuClock;
}

// 글자마다 폭 6인 레이블을 만드는 시험 factory다.
Result<SharedPtr<TextLabel>> MakeMenuLabel(Allocator& allocator, StringView text,
                                           const FontStyle& font) noexcept {
    auto label = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    if (!label) {
        return label;
    }

    constexpr usize MaxClusters = 48;
    text::TextCluster clusters[MaxClusters];
    const usize count = text.ByteCount() < MaxClusters ? text.ByteCount() : MaxClusters;
    for (usize i = 0; i < count; ++i) {
        clusters[i] = {.byteEnd = i + 1, .advance = 6};
    }

    auto shaped = label.Value()->SetShapedText(text.SliceBytes(0, count), {clusters, count}, {},
                                               font.size + 4, false);
    return shaped ? Move(label) : Result<SharedPtr<TextLabel>>::Failure(shaped.Error());
}

struct MenuBench {
    SystemAllocator allocator;
    Application app{allocator};
    SharedPtr<Overlay> root;
    SharedPtr<AnchorCanvas> canvas;
    SharedPtr<MenuContext> context;
    SharedPtr<MenuBar> bar;
    uint32 saved = 0;
    uint32 recent = 0;
    bool enabled = false;
    bool checked = true;

    bool Build() noexcept {
        auto madeRoot = SharedPtr<Overlay>::TryMake(allocator, allocator);
        auto madeCanvas = SharedPtr<AnchorCanvas>::TryMake(allocator, allocator);
        auto madeContext = SharedPtr<MenuContext>::TryMake(
            allocator, allocator, app,
            MenuLabelFactory::CreateLambda([this](StringView text, const FontStyle& font) noexcept {
                return MakeMenuLabel(allocator, text, font);
            }));
        if (!madeRoot || !madeCanvas || !madeContext) {
            return false;
        }

        root = Move(madeRoot.Value());
        canvas = Move(madeCanvas.Value());
        context = Move(madeContext.Value());
        auto madeBar = SharedPtr<MenuBar>::TryMake(allocator, allocator, context);
        if (!madeBar) {
            return false;
        }

        bar = Move(madeBar.Value());
        auto added = bar->AddMenu(
            "File", MenuFill::CreateLambda([this](MenuBuilder& menu) noexcept {
                menu.AddEntry(
                    "Save",
                    {.execute = Delegate<void()>::CreateLambda([this]() noexcept { ++saved; })},
                    {.key = input::Key::S, .control = true});
                menu.AddEntry("Disabled", {.canExecute = Delegate<bool()>::CreateLambda(
                                               [this]() noexcept { return enabled; })});
                menu.AddSeparator();
                menu.AddSubMenu("Recent", MenuFill::CreateLambda([this](MenuBuilder& sub) noexcept {
                                    sub.AddEntry("Level",
                                                 {.execute = Delegate<void()>::CreateLambda(
                                                      [this]() noexcept { ++recent; })});
                                    return Result<void>::Success();
                                }));
                menu.AddSubMenu("Other", MenuFill::CreateLambda([](MenuBuilder& sub) noexcept {
                                    sub.AddEntry("Item", {});
                                    return Result<void>::Success();
                                }));
                menu.BeginSection("SECTION");
                menu.AddEntry("Toggle", {.isChecked = Delegate<bool()>::CreateLambda(
                                             [this]() noexcept { return checked; }),
                                         .check = MenuCheckKind::Check});
                return Result<void>::Success();
            }));
        if (added) {
            added = bar->AddMenu("Edit", MenuFill::CreateLambda([](MenuBuilder& menu) noexcept {
                                     menu.AddEntry("Undo", {});
                                     return Result<void>::Success();
                                 }));
        }

        if (!added || !root->TryAddChild(canvas) ||
            !canvas->TryAddSlot(bar, {.offset = {0, 0, 200, 24}, .alignment = {0, 0}})) {
            return false;
        }

        app.SetRoot(root);
        menuClock = 0;
        app.SetTimerClock(&ReadMenuClock);
        return Frame();
    }

    bool Frame() noexcept {
        return static_cast<bool>(app.UpdatePaintCache(Geometry::Root({400, 300}, {}), 1));
    }

    static Vec2 Center(const Element& element) noexcept {
        auto bounds = element.GetGeometry().WindowBounds();
        return bounds ? (bounds.Value().min + bounds.Value().max) * 0.5f : Vec2{};
    }

    bool Click(const Element& element) noexcept {
        const Vec2 point = Center(element);
        return app.RoutePointer({PointerEventType::Down, point}) &&
               app.RoutePointer({PointerEventType::Up, point});
    }

    bool Press(const Element& element) noexcept {
        return static_cast<bool>(app.RoutePointer({PointerEventType::Down, Center(element)}));
    }

    bool Release(const Element& element) noexcept {
        return static_cast<bool>(app.RoutePointer({PointerEventType::Up, Center(element)}));
    }

    bool Hover(const Element& element) noexcept {
        return static_cast<bool>(app.RoutePointer({PointerEventType::Move, Center(element)}));
    }

    // 열린 File 메뉴 패널이다.
    MenuPanel* FilePanel() const noexcept {
        auto* button = bar->Button(0);
        if (!button || !button->IsOpen()) {
            return nullptr;
        }

        // 막대 버튼의 메뉴는 가장 바깥 Overlay의 마지막 팝업 층 안에 있다.
        for (usize i = root->ChildCount(); i > 0; --i) {
            Element* layer = root->ChildAt(i - 1);
            if (layer->ChildCount() == 1) {
                return static_cast<MenuPanel*>(layer->ChildAt(0));
            }
        }

        return nullptr;
    }

    ~MenuBench() noexcept {
        app.SetTimerClock(nullptr);
    }
};

MenuRow* RowAt(MenuPanel& panel, usize index) noexcept {
    return static_cast<MenuRow*>(panel.ChildAt(index));
}

} // namespace

TEST(UiFramework_MenuBuilderLaysOutEntriesSeparatorsAndSections) {
    MenuBench bench;
    REQUIRE(bench.Build());
    REQUIRE(bench.Click(*bench.bar->Button(0)));
    REQUIRE(bench.Frame());
    MenuPanel* panel = bench.FilePanel();
    REQUIRE(panel);
    // Save · Disabled · 구분선 · Recent · Other · 구분선 · 제목 · Toggle (이어진 구분선은 하나).
    REQUIRE(panel->ChildCount() == 8);
    CHECK(panel->ChildAt(2)->GetVisibility() == Visibility::HitTestInvisible);
    CHECK(panel->ChildAt(5)->GetVisibility() == Visibility::HitTestInvisible);
    CHECK(panel->ChildAt(6)->GetVisibility() == Visibility::HitTestInvisible);
    // 단축키 글자는 FormatKeyChord 규칙이다.
    MenuRow* save = RowAt(*panel, 0);
    REQUIRE(save->ChildCount() == 2);
    CHECK(static_cast<TextLabel*>(save->ChildAt(1))->GetText() == "Ctrl+S");
    // 실행할 수 없는 항목은 꺼진다. 하위 메뉴·체크 표시.
    CHECK(!RowAt(*panel, 1)->IsEnabled());
    CHECK(RowAt(*panel, 3)->HasSubMenu());
    CHECK(RowAt(*panel, 7)->CheckKind() == MenuCheckKind::Check && RowAt(*panel, 7)->IsChecked());
    String chord(bench.allocator);
    REQUIRE(
        FormatKeyChord({.key = input::Key::S, .shift = true, .control = true, .alt = true}, chord));
    CHECK(chord.View() == "Ctrl+Alt+Shift+S");
}

TEST(UiFramework_MenuBarOpensSwitchesAndClosesOnExecute) {
    MenuBench bench;
    REQUIRE(bench.Build());
    MenuBarButton& file = *bench.bar->Button(0);
    MenuBarButton& edit = *bench.bar->Button(1);
    // 닫힌 동안 막대 위 hover로는 열지 않는다.
    REQUIRE(bench.Hover(edit));
    CHECK(bench.bar->OpenIndex() == IndexNone);
    // 누르는 순간(놓기 전) 연다 — 눌림 색이 들어갔다 빠지지 않는다.
    REQUIRE(bench.Press(file));
    CHECK(bench.bar->OpenIndex() == 0);
    REQUIRE(bench.Release(file));
    REQUIRE(bench.Frame());
    CHECK(bench.bar->OpenIndex() == 0);
    // 열린 동안 다른 막대 버튼에 올리면 그 메뉴로 바꾼다(메뉴 층이 막대를 덮지 않는다).
    REQUIRE(bench.Hover(edit));
    REQUIRE(bench.Frame());
    CHECK(bench.bar->OpenIndex() == 1 && !file.IsOpen());
    REQUIRE(bench.Hover(file));
    REQUIRE(bench.Frame());
    CHECK(bench.bar->OpenIndex() == 0);
    // 열린 막대 버튼을 다시 누르면 닫는다.
    REQUIRE(bench.Click(file));
    CHECK(bench.bar->OpenIndex() == IndexNone);
    // 다시 File을 열어 항목을 누르면 실행하고 메뉴 전체를 닫는다.
    REQUIRE(bench.Click(file));
    REQUIRE(bench.Frame());
    REQUIRE(bench.bar->OpenIndex() == 0);
    MenuPanel* panel = bench.FilePanel();
    REQUIRE(panel);
    REQUIRE(bench.Click(*RowAt(*panel, 0)));
    CHECK(bench.saved == 1 && bench.bar->OpenIndex() == IndexNone);
    // 바깥 누름은 메뉴를 닫는다.
    REQUIRE(bench.Click(file));
    REQUIRE(bench.Frame());
    auto outside = bench.app.RoutePointer({PointerEventType::Down, {350, 250}});
    REQUIRE(outside);
    // 바깥 누름은 메뉴만 닫고 아래 요소로 가지 않는다(소비).
    CHECK(outside.Value() && bench.bar->OpenIndex() == IndexNone);
}

TEST(UiFramework_MenuWindowChromeHostsMenuBarPopups) {
    // 회귀: 주 메뉴는 창 루트인 WindowChrome의 제목 막대 안에 있다. 위에 Overlay가 없어도 창
    // 테두리가 팝업 층을 받아야 한다.
    MenuBench bench;
    REQUIRE(bench.Build());
    REQUIRE(bench.canvas->RemoveChild(*bench.bar));
    auto titleBar = SharedPtr<WindowTitleBar>::TryMake(bench.allocator, bench.allocator);
    auto chrome = SharedPtr<WindowChrome>::TryMake(bench.allocator, bench.allocator);
    auto body = SharedPtr<AnchorCanvas>::TryMake(bench.allocator, bench.allocator);
    REQUIRE(titleBar && chrome && body);
    REQUIRE(titleBar.Value()->Construct({.leftContent = bench.bar}));
    REQUIRE(chrome.Value()->Construct({.titleBar = titleBar.Value(), .content = body.Value()}));
    bench.app.SetRoot(chrome.Value());
    REQUIRE(bench.Frame());
    MenuBarButton& file = *bench.bar->Button(0);
    REQUIRE(bench.Click(file));
    REQUIRE(bench.Frame());
    REQUIRE(file.IsOpen());
    // 팝업 층은 창 테두리의 마지막 자식이고 창 전체를 덮는다. 메뉴는 버튼 바로 아래다.
    REQUIRE(chrome.Value()->ChildCount() == 3);
    Element* layer = chrome.Value()->ChildAt(2);
    CHECK((layer->GetGeometry().size == Vec2{400, 300}));
    auto position = file.MenuPosition();
    auto bounds = file.GetGeometry().WindowBounds();
    REQUIRE(position && bounds);
    CHECK(position->y == bounds.Value().max.y);
    // 하위 메뉴도 같은 호스트에 붙고, 연 항목은 포인터가 하위 메뉴로 옮겨 가도 강조를 유지한다.
    auto* panel = static_cast<MenuPanel*>(file.MenuContent());
    REQUIRE(panel);
    MenuRow& recent = *RowAt(*panel, 3);
    REQUIRE(bench.Hover(recent));
    REQUIRE(bench.Frame());
    REQUIRE(recent.IsOpen() && chrome.Value()->ChildCount() == 4);
    auto* subPanel = recent.MenuContent();
    REQUIRE(subPanel);
    REQUIRE(bench.Hover(*subPanel->ChildAt(0)));
    REQUIRE(bench.Frame());
    CHECK(!recent.IsHovered() && recent.IsHighlighted());
    CHECK(file.IsOpen());
    // 창 테두리 누름(DismissPopups)도 호스트된 메뉴를 모두 닫는다.
    bench.app.DismissPopups();
    CHECK(!file.IsOpen() && chrome.Value()->ChildCount() == 2);
}

TEST(UiFramework_MenuSubMenuOpensOnHoverAndClobbersAfterDelay) {
    MenuBench bench;
    REQUIRE(bench.Build());
    REQUIRE(bench.Click(*bench.bar->Button(0)));
    REQUIRE(bench.Frame());
    MenuPanel* panel = bench.FilePanel();
    REQUIRE(panel);
    MenuRow& recent = *RowAt(*panel, 3);
    MenuRow& other = *RowAt(*panel, 4);
    // 하위 메뉴는 hover 즉시 연다(UE SubMenuOpenTime 0).
    REQUIRE(bench.Hover(recent));
    REQUIRE(bench.Frame());
    CHECK(recent.IsOpen() && panel->OpenSubMenu() == &recent);
    // 다른 하위 메뉴 항목으로 옮기면 지연(0.5초) 뒤 바꾼다.
    REQUIRE(bench.Hover(other));
    REQUIRE(bench.Frame());
    CHECK(recent.IsOpen() && !other.IsOpen());
    menuClock = SubMenuClobberDelay + 0.1;
    REQUIRE(bench.Frame());
    REQUIRE(bench.Frame());
    CHECK(!recent.IsOpen() && other.IsOpen());
    // 하위 메뉴 항목 실행도 메뉴 전체를 닫는다.
    REQUIRE(bench.Hover(recent));
    menuClock += SubMenuClobberDelay + 0.1;
    REQUIRE(bench.Frame());
    REQUIRE(bench.Frame());
    REQUIRE(recent.IsOpen());
    Element* subLayer = bench.root->ChildAt(bench.root->ChildCount() - 1);
    auto* subPanel = static_cast<MenuPanel*>(subLayer->ChildAt(0));
    REQUIRE(bench.Click(*RowAt(*subPanel, 0)));
    CHECK(bench.recent == 1 && bench.bar->OpenIndex() == IndexNone);
}

TEST(UiFramework_MenuKeyboardNavigation) {
    MenuBench bench;
    REQUIRE(bench.Build());
    // 키보드로 열면 첫 항목에 초점, 아래는 꺼진 항목을 건너뛴다(Save → Recent).
    REQUIRE(bench.bar->OpenMenu(0, true));
    REQUIRE(bench.Frame());
    MenuPanel* panel = bench.FilePanel();
    REQUIRE(panel);
    CHECK(bench.app.GetFocus(0).Get() == RowAt(*panel, 0));
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::KeyDown(input::Key::Down)));
    CHECK(bench.app.GetFocus(0).Get() == RowAt(*panel, 3));
    // 오른쪽은 하위 메뉴를 열고 그 첫 항목으로, 왼쪽은 닫고 돌아온다.
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::KeyDown(input::Key::Right)));
    REQUIRE(bench.Frame());
    CHECK(RowAt(*panel, 3)->IsOpen());
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::KeyDown(input::Key::Left)));
    REQUIRE(bench.Frame());
    CHECK(!RowAt(*panel, 3)->IsOpen() && bench.app.GetFocus(0).Get() == RowAt(*panel, 3));
    // 루트 메뉴에서 오른쪽(하위 메뉴가 아닌 항목)은 막대의 다음 메뉴로 간다.
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::KeyDown(input::Key::Up)));
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::KeyDown(input::Key::Right)));
    REQUIRE(bench.Frame());
    CHECK(bench.bar->OpenIndex() == 1);
    // 위는 끝에서 돌아간다. Esc는 메뉴를 닫는다.
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::KeyDown(input::Key::Escape)));
    CHECK(bench.bar->OpenIndex() == IndexNone);
}

TEST(UiFramework_MenuBarAccelerators) {
    MenuBench bench;
    REQUIRE(bench.Build());
    CommandSet definitions(bench.allocator);
    BindingManager bindings(bench.allocator);
    auto commands =
        SharedPtr<CommandList>::TryMake(bench.allocator, bench.allocator, definitions, bindings);
    REQUIRE(commands);
    REQUIRE(bench.bar->RegisterAccelerators(definitions, *commands.Value()));
    REQUIRE(bench.app.SetCommands({}, commands.Value()));
    // Alt+E는 Edit 메뉴를 연다.
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::KeyDown(input::Key::LeftAlt)));
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::KeyDown(input::Key::E)));
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::KeyUp(input::Key::LeftAlt)));
    REQUIRE(bench.Frame());
    CHECK(bench.bar->OpenIndex() == 1);
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::KeyDown(input::Key::Escape)));
    // F10은 첫 막대 버튼에 키보드 초점(외곽선)이다.
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::KeyDown(input::Key::F10)));
    CHECK(bench.app.GetFocus(0).Get() == bench.bar->Button(0));
    CHECK(bench.bar->Button(0)->IsFocusOutlineVisible());
}

} // namespace rocks::ui
