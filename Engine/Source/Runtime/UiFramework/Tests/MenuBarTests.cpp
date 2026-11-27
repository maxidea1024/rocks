#include "RegionMapCheck.h"

#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Controls/Button.h>
#include <UiFramework/Layout/AnchorCanvas.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Menus/MenuBar.h>
#include <UiFramework/Menus/MenuHeading.h>
#include <UiFramework/Menus/MenuSeparator.h>
#include <UiFramework/Layout/Overlay.h>
#include <UiFramework/Window/WindowChrome.h>

namespace rocks::ui {

namespace {

constexpr uint8 Utf8ContinuationMask = 0xC0;
constexpr uint8 Utf8ContinuationBits = 0x80;

double menuClock = 0;
// 창 높이(300)를 넘는 메뉴 항목 수다.
constexpr usize LongMenuRows = 40;
constexpr VectorImageKey SaveIcon = VectorImageKey::FromPath("Common/Save");
double ReadMenuClock() noexcept {
    return menuClock;
}

// 코드 포인트마다 폭 6인 레이블을 만드는 시험 factory다(UTF-8 여러 byte 글자 — 넘침 표시 » — 는
// 한 클러스터).
Result<SharedPtr<TextLabel>> MakeMenuLabel(Allocator& allocator, StringView text,
                                           const FontStyle& font) noexcept {
    auto label = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    if (!label) {
        return label;
    }

    constexpr usize MaxClusters = 48;
    text::TextCluster clusters[MaxClusters];
    usize count = 0;
    usize bytes = 0;
    while (bytes < text.ByteCount() && count < MaxClusters) {
        // UTF-8 이어지는 byte(10xxxxxx)는 앞 글자에 붙인다.
        usize end = bytes + 1;
        while (end < text.ByteCount() && (static_cast<uint8>(text.Data()[end]) &
                                          Utf8ContinuationMask) == Utf8ContinuationBits) {
            ++end;
        }

        clusters[count++] = {.byteEnd = end, .advance = 6};
        bytes = end;
    }

    auto shaped = label.Value()->SetShapedText(text.SliceBytes(0, bytes), {clusters, count}, {},
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
    // 메뉴 밖의 초점 받는 요소(입력 상자 자리)다 — 초점 복원 시험용.
    SharedPtr<Button> editor;
    uint32 saved = 0;
    uint32 recent = 0;
    bool enabled = false;
    bool checked = true;

    bool Build() noexcept {
        auto madeRoot = SharedPtr<Overlay>::TryMake(allocator, allocator);
        auto madeCanvas = SharedPtr<AnchorCanvas>::TryMake(allocator, allocator);
        auto madeEditor = SharedPtr<Button>::TryMake(allocator, allocator);
        if (!madeEditor) {
            return false;
        }

        editor = Move(madeEditor.Value());
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
                    {.execute = Delegate<void()>::CreateLambda([this]() noexcept { ++saved; }),
                     .icon = SaveIcon,
                     .toolTip = "Saves the level"},
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
                menu.BeginSection("Section");
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

        // 창(300)보다 긴 메뉴다(스크롤 시험).
        if (added) {
            added = bar->AddMenu("View", MenuFill::CreateLambda([](MenuBuilder& menu) noexcept {
                                     for (usize i = 0; i < LongMenuRows; ++i) {
                                         menu.AddEntry("Row", {});
                                     }

                                     return Result<void>::Success();
                                 }));
        }

        if (!added || !root->TryAddChild(canvas) ||
            !canvas->TryAddSlot(bar, {.offset = {0, 0, 200, 24}, .alignment = {0, 0}}) ||
            !canvas->TryAddSlot(editor, {.offset = {300, 250, 60, 20}, .alignment = {0, 0}})) {
            return false;
        }

        app.SetRoot(root);
        menuClock = 0;
        app.SetClock(ClockFunction::CreateStatic(&ReadMenuClock));
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

    bool Key(input::Key key) noexcept {
        return static_cast<bool>(app.RouteKey(0, input::InputEvent::KeyDown(key))) &&
               static_cast<bool>(app.RouteKey(0, input::InputEvent::KeyUp(key)));
    }

    bool Wheel(const Element& element, float notches) noexcept {
        return static_cast<bool>(app.RoutePointer(
            {.type = PointerEventType::Wheel, .position = Center(element), .wheel = notches}));
    }

    // index 막대 메뉴의 열린 패널이다.
    MenuPanel* Panel(usize index) const noexcept {
        auto* button = bar->Button(index);
        return button ? StaticElementCast<MenuPanel>(button->MenuContent()) : nullptr;
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
                return StaticElementCast<MenuPanel>(layer->ChildAt(0));
            }
        }

        return nullptr;
    }

    ~MenuBench() noexcept {
        app.SetClock({});
    }
};

MenuRow* RowAt(MenuPanel& panel, usize index) noexcept {
    return StaticElementCast<MenuRow>(panel.ChildAt(index));
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
    CHECK(StaticElementCast<TextLabel>(save->ChildAt(1))->GetText() == "Ctrl+S");
    // 실행할 수 없는 항목은 꺼진다. 하위 메뉴·체크 표시.
    CHECK(!RowAt(*panel, 1)->IsEnabled());
    CHECK(RowAt(*panel, 3)->HasSubMenu());
    CHECK(RowAt(*panel, 7)->CheckKind() == MenuCheckKind::Check && RowAt(*panel, 7)->IsChecked());
    // 묶음 제목은 대문자로 보인다(UE SHeadingBlock).
    REQUIRE(panel->ChildAt(6)->ChildCount() == 1);
    CHECK(StaticElementCast<TextLabel>(panel->ChildAt(6)->ChildAt(0))->GetText() == "SECTION");
    // 아이콘과 설명 툴팁이 항목에 붙는다.
    CHECK(RowAt(*panel, 0)->Icon() == SaveIcon && RowAt(*panel, 0)->HasToolTip());
    CHECK(!RowAt(*panel, 3)->HasToolTip());
    String chord(bench.allocator);
    REQUIRE(
        FormatKeyChord({.key = input::Key::S, .shift = true, .control = true, .alt = true}, chord));
    CHECK(chord.View() == "Ctrl+Alt+Shift+S");
    // 단축키 없는 항목은 단축키 칸이 없고("None"을 쓰지 않는다), 빈 단축키는 글자가 없다.
    CHECK(RowAt(*panel, 3)->ChildCount() == 1);
    String empty(bench.allocator);
    REQUIRE(FormatKeyChord({}, empty));
    CHECK(empty.IsEmpty());
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
    auto* panel = StaticElementCast<MenuPanel>(file.MenuContent());
    REQUIRE(panel);
    // 팝업 위 지점은 Popup 영역이다(창 끝에 붙어도 크기 조절 가장자리보다 앞선다 — R6). 제목
    // 막대의 빈 곳은 Caption이다.
    CHECK(bench.app.QueryWindowRegion(MenuBench::Center(*RowAt(*panel, 0))) ==
          input::WindowRegion::Popup);
    CHECK(bench.app.QueryWindowRegion({220, 5}) == input::WindowRegion::Caption);
    // 영역 지도도 같은 답이고, 메뉴가 열려 있어 popupOpen이다(backend가 가장자리를 끈다 — 19.2절).
    {
        IntVec2 mismatch;
        CHECK(test::RegionMapMatchesQuery(bench.app, {400, 300}, mismatch));
        input::WindowRegionMap map;
        bench.app.BuildWindowRegionMap(map);
        CHECK(map.popupOpen);
        CHECK(map.Find({static_cast<int32>(MenuBench::Center(*RowAt(*panel, 0)).x),
                        static_cast<int32>(MenuBench::Center(*RowAt(*panel, 0)).y)}) ==
              input::WindowRegion::Popup);
    }

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
    auto* subPanel = StaticElementCast<MenuPanel>(subLayer->ChildAt(0));
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
    REQUIRE(bench.bar->RegisterAccelerators());
    // 막대가 자기 목록을 창 단위 명령으로 붙인다 — 두 번 붙일 수 없다.
    CHECK(bench.bar->RegisterAccelerators().Error() == ErrorCode::AlreadyExists);
    REQUIRE(bench.app.SetFocus(0, bench.editor, FocusCause::Pointer));
    // Alt+E는 Edit 메뉴를 열고 첫 항목에 키보드 초점을 둔다.
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::KeyDown(input::Key::LeftAlt)));
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::KeyDown(input::Key::E)));
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::KeyUp(input::Key::LeftAlt)));
    REQUIRE(bench.Frame());
    REQUIRE(bench.bar->OpenIndex() == 1);
    CHECK(bench.app.GetFocus(0).Get() == bench.Panel(1)->ChildAt(0));
    // Esc는 메뉴를 닫고 열기 전 초점(입력 상자)으로 돌아간다.
    REQUIRE(bench.Key(input::Key::Escape));
    CHECK(bench.bar->OpenIndex() == IndexNone && bench.app.GetFocus(0) == bench.editor);
    // F10은 첫 막대 버튼에 키보드 초점이다. 외곽선 대신 올림 강조로 보인다.
    REQUIRE(bench.Key(input::Key::F10));
    MenuBarButton& file = *bench.bar->Button(0);
    CHECK(bench.app.GetFocus(0).Get() == &file);
    CHECK(file.IsHighlighted() && !file.IsFocusOutlineVisible());
    // 막대에서 오른쪽은 옆 버튼, 아래는 그 메뉴 열기(첫 항목 초점)다.
    REQUIRE(bench.Key(input::Key::Right));
    CHECK(bench.app.GetFocus(0).Get() == bench.bar->Button(1));
    REQUIRE(bench.Key(input::Key::Down));
    REQUIRE(bench.Frame());
    CHECK(bench.bar->OpenIndex() == 1);
    // 메뉴를 닫으면 F10 전 초점(입력 상자)으로 돌아간다.
    REQUIRE(bench.Key(input::Key::Escape));
    CHECK(bench.app.GetFocus(0) == bench.editor);
    // F10을 두 번 누르면 막대에서 나가 원래 초점으로 돌아간다.
    REQUIRE(bench.Key(input::Key::F10));
    REQUIRE(bench.Key(input::Key::F10));
    CHECK(bench.app.GetFocus(0) == bench.editor);
    // 막대가 사라지면 가속키도 사라진다(해제된 막대를 부르지 않는다).
    REQUIRE(bench.canvas->RemoveChild(*bench.bar));
    bench.bar.Reset();
    auto handled = bench.app.RouteKey(0, input::InputEvent::KeyDown(input::Key::F10));
    REQUIRE(handled);
    CHECK(!handled.Value());
}

TEST(UiFramework_MenuRestoresFocusAfterMouseUse) {
    MenuBench bench;
    REQUIRE(bench.Build());
    REQUIRE(bench.app.SetFocus(0, bench.editor, FocusCause::Pointer));
    // 마우스로 열면 패널에 초점(외곽선 없음)을 둔다.
    REQUIRE(bench.Click(*bench.bar->Button(0)));
    REQUIRE(bench.Frame());
    MenuPanel* panel = bench.FilePanel();
    REQUIRE(panel);
    CHECK(bench.app.GetFocus(0).Get() == panel);
    // 항목을 마우스로 실행하면 열기 전 초점으로 돌아간 뒤 실행한다(Edit > Paste가 입력 상자로).
    REQUIRE(bench.Click(*RowAt(*panel, 0)));
    CHECK(bench.saved == 1 && bench.app.GetFocus(0) == bench.editor);
    // 바깥 누름으로 닫아도 돌아간다(누름은 삼킨다 — 입력 상자에 초점을 다시 주지 않는다).
    REQUIRE(bench.Click(*bench.bar->Button(0)));
    REQUIRE(bench.Frame());
    REQUIRE(bench.app.RoutePointer({PointerEventType::Down, {200, 200}}));
    CHECK(bench.bar->OpenIndex() == IndexNone && bench.app.GetFocus(0) == bench.editor);
    // 열린 막대 버튼을 다시 눌러 닫아도 돌아간다.
    REQUIRE(bench.Click(*bench.bar->Button(0)));
    REQUIRE(bench.Click(*bench.bar->Button(0)));
    CHECK(bench.bar->OpenIndex() == IndexNone && bench.app.GetFocus(0) == bench.editor);
}

TEST(UiFramework_MenuKeyboardAfterMouseOpen) {
    MenuBench bench;
    REQUIRE(bench.Build());
    REQUIRE(bench.Click(*bench.bar->Button(0)));
    REQUIRE(bench.Frame());
    MenuPanel* panel = bench.FilePanel();
    REQUIRE(panel);
    // 마우스로 연 메뉴에서 첫 아래 방향키는 첫 항목으로, 꺼진 항목은 건너뛴다.
    REQUIRE(bench.Key(input::Key::Down));
    CHECK(bench.app.GetFocus(0).Get() == RowAt(*panel, 0));
    CHECK(RowAt(*panel, 0)->IsHighlighted() && !RowAt(*panel, 0)->IsFocusOutlineVisible());
    REQUIRE(bench.Key(input::Key::Down));
    CHECK(bench.app.GetFocus(0).Get() == RowAt(*panel, 3));
    // 위는 처음에서 끝으로 돌아가고, Home·End는 처음·끝이다.
    REQUIRE(bench.Key(input::Key::Home));
    CHECK(bench.app.GetFocus(0).Get() == RowAt(*panel, 0));
    REQUIRE(bench.Key(input::Key::Up));
    CHECK(bench.app.GetFocus(0).Get() == RowAt(*panel, 7));
    REQUIRE(bench.Key(input::Key::Home));
    REQUIRE(bench.Key(input::Key::End));
    CHECK(bench.app.GetFocus(0).Get() == RowAt(*panel, 7));
    // Tab은 아래와 같다(메뉴 밖으로 빠지지 않는다).
    REQUIRE(bench.Key(input::Key::Tab));
    CHECK(bench.app.GetFocus(0).Get() == RowAt(*panel, 0));
    // hover는 초점을 옮겨 강조가 하나다(키보드로 고른 항목의 강조가 꺼진다).
    REQUIRE(bench.Hover(*RowAt(*panel, 4)));
    CHECK(bench.app.GetFocus(0).Get() == RowAt(*panel, 4));
    CHECK(!RowAt(*panel, 0)->IsHighlighted() && RowAt(*panel, 4)->IsHighlighted());
}

TEST(UiFramework_MenuSubMenuSurvivesPassingOverSibling) {
    MenuBench bench;
    REQUIRE(bench.Build());
    REQUIRE(bench.Click(*bench.bar->Button(0)));
    REQUIRE(bench.Frame());
    MenuPanel* panel = bench.FilePanel();
    REQUIRE(panel);
    MenuRow& recent = *RowAt(*panel, 3);
    MenuRow& other = *RowAt(*panel, 4);
    REQUIRE(bench.Hover(recent));
    REQUIRE(bench.Frame());
    REQUIRE(recent.IsOpen());
    auto* sub = StaticElementCast<MenuPanel>(recent.MenuContent());
    REQUIRE(sub);
    // 하위 메뉴 첫 항목은 부모 항목과 같은 높이에서 시작한다.
    auto rowBounds = recent.GetGeometry().WindowBounds();
    auto subRow = sub->ChildAt(0)->GetGeometry().WindowBounds();
    REQUIRE(rowBounds && subRow);
    CHECK(math::Abs(subRow.Value().min.y - rowBounds.Value().min.y) < 0.01f);
    // 형제 항목(Other)을 스치고 하위 메뉴로 들어가면 대기 중인 바꾸기가 취소된다.
    REQUIRE(bench.Hover(other));
    REQUIRE(bench.Frame());
    // 형제를 hover하는 동안 열린 항목의 강조는 꺼진다.
    CHECK(!recent.IsHighlighted());
    REQUIRE(bench.Hover(*sub->ChildAt(0)));
    REQUIRE(bench.Frame());
    menuClock = SubMenuClobberDelay + 0.1;
    REQUIRE(bench.Frame());
    REQUIRE(bench.Frame());
    CHECK(recent.IsOpen() && !other.IsOpen());
    // 하위 메뉴 안에 있는 동안 연 항목은 강조를 유지한다.
    CHECK(recent.IsHighlighted());
    // 하위 메뉴의 일반 항목에서 왼쪽은 하위 메뉴를 닫고 그 항목으로 돌아온다.
    REQUIRE(bench.Key(input::Key::Left));
    REQUIRE(bench.Frame());
    CHECK(!recent.IsOpen() && bench.app.GetFocus(0).Get() == &recent);
}

TEST(UiFramework_MenuDragReleaseExecutesItem) {
    MenuBench bench;
    REQUIRE(bench.Build());
    MenuBarButton& file = *bench.bar->Button(0);
    // 막대 버튼을 누르면(놓기 전) 열리고, 같은 버튼 위에서 놓으면 열린 채다.
    REQUIRE(bench.Press(file));
    REQUIRE(bench.Frame());
    REQUIRE(bench.Release(file));
    CHECK(file.IsOpen());
    REQUIRE(bench.Click(file));
    REQUIRE(!file.IsOpen());
    // 누른 채 끌어 항목 위에서 놓으면 그 항목을 실행한다(Windows·macOS).
    REQUIRE(bench.Press(file));
    REQUIRE(bench.Frame());
    MenuPanel* panel = bench.FilePanel();
    REQUIRE(panel);
    REQUIRE(bench.Hover(*RowAt(*panel, 0)));
    REQUIRE(bench.Release(*RowAt(*panel, 0)));
    CHECK(bench.saved == 1 && !file.IsOpen());
}

TEST(UiFramework_MenuLongPanelScrolls) {
    MenuBench bench;
    REQUIRE(bench.Build());
    REQUIRE(bench.bar->OpenMenu(2, true));
    REQUIRE(bench.Frame());
    REQUIRE(bench.Frame());
    MenuPanel* panel = bench.Panel(2);
    REQUIRE(panel);
    // 패널은 창 안으로 줄고 그 안에서 스크롤한다.
    CHECK(panel->GetGeometry().size.y <= 300 - 24);
    REQUIRE(panel->MaxScrollOffset() > 0);
    CHECK(panel->ScrollOffset() == 0);
    // 휠 한 칸은 48 px 내린다.
    REQUIRE(bench.Wheel(*panel, -1));
    REQUIRE(bench.Frame());
    CHECK(panel->ScrollOffset() == MenuWheelScrollStep);
    // End는 마지막 항목으로 가며 보이게 맞춘다.
    REQUIRE(bench.Key(input::Key::End));
    REQUIRE(bench.Frame());
    CHECK(panel->ScrollOffset() == panel->MaxScrollOffset());
    auto last = panel->ChildAt(LongMenuRows - 1)->GetGeometry().WindowBounds();
    auto view = panel->GetGeometry().WindowBounds();
    REQUIRE(last && view);
    CHECK(last.Value().max.y <= view.Value().max.y + 0.01f);
    // Home은 처음으로 돌아간다.
    REQUIRE(bench.Key(input::Key::Home));
    REQUIRE(bench.Frame());
    CHECK(panel->ScrollOffset() == 0);
}

TEST(UiFramework_MenuTypeAheadJumpsAndExecutesUniqueMatch) {
    MenuBench bench;
    REQUIRE(bench.Build());
    REQUIRE(bench.bar->OpenMenu(0, true));
    REQUIRE(bench.Frame());
    MenuPanel* panel = bench.FilePanel();
    REQUIRE(panel);
    // "O"로 시작하는 항목은 Other 하나 — 실행(하위 메뉴 열기)한다.
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::Text('O')));
    REQUIRE(bench.Frame());
    CHECK(RowAt(*panel, 4)->IsOpen());
    // "T"로 시작하는 항목은 Toggle 하나 — 실행하면 메뉴가 닫힌다.
    REQUIRE(bench.Key(input::Key::Left));
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::Text('t')));
    CHECK(bench.bar->OpenIndex() == IndexNone);
    // 맞는 항목이 없는 글자는 아무것도 하지 않는다.
    REQUIRE(bench.bar->OpenMenu(0, true));
    REQUIRE(bench.Frame());
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::Text('q')));
    CHECK(bench.bar->OpenIndex() == 0);
}

TEST(UiFramework_MenuBarOverflowHidesMenusThatDoNotFit) {
    MenuBench bench;
    REQUIRE(bench.Build());
    // 글자 폭 6 + 버튼 여백 24: File·Edit·View는 48씩, 넘침(»)은 한 글자라 30이다. 폭 100이면
    // 70(100 - 30)까지 File만 들어간다.
    REQUIRE(bench.canvas->RemoveChild(*bench.bar));
    REQUIRE(bench.canvas->TryAddSlot(bench.bar, {.offset = {0, 0, 100, 24}, .alignment = {0, 0}}));
    REQUIRE(bench.Frame());
    REQUIRE(bench.Frame());
    CHECK(!bench.bar->IsMenuHidden(0) && bench.bar->IsMenuHidden(1) && bench.bar->IsMenuHidden(2));
    MenuBarButton* overflow = bench.bar->OverflowButton();
    REQUIRE(overflow);
    CHECK(overflow->GetVisibility() == Visibility::Visible);
    // 넘침 메뉴는 숨은 메뉴들(Edit·View)을 하위 메뉴로 보인다.
    REQUIRE(bench.Click(*overflow));
    REQUIRE(bench.Frame());
    auto* panel = StaticElementCast<MenuPanel>(overflow->MenuContent());
    REQUIRE(panel);
    REQUIRE(panel->ChildCount() == 2);
    CHECK(RowAt(*panel, 0)->HasSubMenu() && RowAt(*panel, 0)->LabelText() == "Edit");
    // 넓히면 모두 보이고 넘침 버튼은 접힌다(열린 넘침 메뉴도 닫힌다).
    REQUIRE(bench.canvas->RemoveChild(*bench.bar));
    REQUIRE(bench.canvas->TryAddSlot(bench.bar, {.offset = {0, 0, 300, 24}, .alignment = {0, 0}}));
    REQUIRE(bench.Frame());
    REQUIRE(bench.Frame());
    CHECK(!bench.bar->IsMenuHidden(1) && !bench.bar->IsMenuHidden(2));
    REQUIRE(bench.Frame());
    CHECK(overflow->GetVisibility() == Visibility::Collapsed && !overflow->IsOpen());
}

TEST(UiFramework_MenuBarOverflowInTitleBarRecoversWhenWidened) {
    // 회귀(R16): 제목 막대는 왼쪽 내용에 min(원하는 폭, 남는 폭)을 준다. 숨은 메뉴가 원하는 크기를
    // 잃으면 다음 프레임에 앞 메뉴까지 연쇄로 접히고 넓혀도 돌아오지 않았다.
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
    const auto frame = [&](float width) noexcept {
        return static_cast<bool>(bench.app.UpdatePaintCache(Geometry::Root({width, 300}, {}), 1));
    };
    // 넉넉하면 모두 보인다.
    REQUIRE(frame(800));
    REQUIRE(frame(800));
    CHECK(!bench.bar->IsMenuHidden(0) && !bench.bar->IsMenuHidden(2));
    // 막대 폭을 100으로 줄이면(File 48 + 넘침 30 = 78은 들어가고 Edit까지 126은 안 들어간다) 뒤
    // 메뉴만 숨고, 여러 프레임이 지나도 File은 남는다(연쇄로 접히지 않는다).
    // 막대가 받을 수 있는 폭은 창 버튼 왼쪽 끝까지다.
    const auto& minimize = titleBar.Value()->ControlButton(WindowButtonRole::Minimize);
    REQUIRE(minimize);
    auto buttons = minimize->GetGeometry().WindowBounds();
    auto barBounds = bench.bar->GetGeometry().WindowBounds();
    REQUIRE(buttons && barBounds);
    const float available = buttons.Value().min.x - barBounds.Value().min.x;
    const float narrow = 800 - (available - 100);
    for (int i = 0; i < 4; ++i) {
        REQUIRE(frame(narrow));
    }

    CHECK(!bench.bar->IsMenuHidden(0) && bench.bar->IsMenuHidden(2));
    // 다시 넓히면 모두 돌아온다.
    for (int i = 0; i < 3; ++i) {
        REQUIRE(frame(800));
    }

    CHECK(!bench.bar->IsMenuHidden(1) && !bench.bar->IsMenuHidden(2));
}

TEST(UiFramework_MenuHiddenMenuAcceleratorOpensItsSubMenu) {
    MenuBench bench;
    REQUIRE(bench.Build());
    REQUIRE(bench.bar->RegisterAccelerators());
    REQUIRE(bench.canvas->RemoveChild(*bench.bar));
    REQUIRE(bench.canvas->TryAddSlot(bench.bar, {.offset = {0, 0, 100, 24}, .alignment = {0, 0}}));
    REQUIRE(bench.Frame());
    REQUIRE(bench.Frame());
    REQUIRE(bench.bar->IsMenuHidden(1));
    // Alt+E(숨은 Edit)는 넘침 메뉴를 열고 그 안의 Edit 하위 메뉴까지 연다(첫 항목 초점).
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::KeyDown(input::Key::LeftAlt)));
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::KeyDown(input::Key::E)));
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::KeyUp(input::Key::LeftAlt)));
    REQUIRE(bench.Frame());
    MenuBarButton* overflow = bench.bar->OverflowButton();
    REQUIRE(overflow && overflow->IsOpen());
    auto* panel = StaticElementCast<MenuPanel>(overflow->MenuContent());
    REQUIRE(panel);
    MenuRow* edit = RowAt(*panel, 0);
    REQUIRE(edit->IsOpen());
    auto* sub = StaticElementCast<MenuPanel>(edit->MenuContent());
    REQUIRE(sub);
    CHECK(bench.app.GetFocus(0).Get() == sub->ChildAt(0));
}

TEST(UiFramework_MenuReleaseOverSubMenuRowKeepsMenuOpen) {
    MenuBench bench;
    REQUIRE(bench.Build());
    MenuBarButton& file = *bench.bar->Button(0);
    REQUIRE(bench.Press(file));
    REQUIRE(bench.Frame());
    MenuPanel* panel = bench.FilePanel();
    REQUIRE(panel);
    // 하위 메뉴 항목 위에서 놓으면 실행 대신 하위 메뉴를 연 채로 메뉴가 남는다.
    REQUIRE(bench.Hover(*RowAt(*panel, 3)));
    REQUIRE(bench.Release(*RowAt(*panel, 3)));
    REQUIRE(bench.Frame());
    CHECK(file.IsOpen() && RowAt(*panel, 3)->IsOpen());
}

TEST(UiFramework_MenuKeyboardMoveHidesHoverHighlight) {
    MenuBench bench;
    REQUIRE(bench.Build());
    REQUIRE(bench.Click(*bench.bar->Button(0)));
    REQUIRE(bench.Frame());
    MenuPanel* panel = bench.FilePanel();
    REQUIRE(panel);
    MenuRow& save = *RowAt(*panel, 0);
    REQUIRE(bench.Hover(save));
    CHECK(save.IsHighlighted());
    // 포인터를 Save에 둔 채 아래 방향키: 강조는 키보드로 고른 Recent 하나다.
    REQUIRE(bench.Key(input::Key::Down));
    CHECK(!save.IsHighlighted() && RowAt(*panel, 3)->IsHighlighted());
    CHECK(panel->IsKeyboardNavigating());
    // 포인터가 다시 움직이면 hover 강조가 돌아온다.
    REQUIRE(bench.Hover(save));
    CHECK(!panel->IsKeyboardNavigating() && save.IsHighlighted());
}

TEST(UiFramework_MenuEnterOverEmptySpaceDoesNothing) {
    MenuBench bench;
    REQUIRE(bench.Build());
    REQUIRE(bench.Click(*bench.bar->Button(0)));
    REQUIRE(bench.Frame());
    MenuPanel* panel = bench.FilePanel();
    REQUIRE(panel);
    REQUIRE(bench.Hover(*RowAt(*panel, 0)));
    // 패널 여백(항목 밖)으로 옮기면 초점이 패널로 돌아간다 — 강조 없는 Save를 Enter가 실행하지
    // 않는다.
    auto bounds = panel->GetGeometry().WindowBounds();
    REQUIRE(bounds);
    REQUIRE(bench.app.RoutePointer({PointerEventType::Move, bounds.Value().min + Vec2{2, 2}}));
    CHECK(bench.app.GetFocus(0).Get() == panel);
    REQUIRE(bench.Key(input::Key::Enter));
    CHECK(bench.saved == 0 && bench.bar->OpenIndex() == 0);
}

TEST(UiFramework_MenuTypeAheadUsesTypedCharacter) {
    MenuBench bench;
    REQUIRE(bench.Build());
    REQUIRE(bench.bar->OpenMenu(0, true));
    REQUIRE(bench.Frame());
    MenuPanel* panel = bench.FilePanel();
    REQUIRE(panel);
    // 글자 점프는 입력 문자 기준이다(물리 키 O가 아니라 문자 'o'): Other 하나 — 하위 메뉴를 연다.
    REQUIRE(bench.app.RouteKey(0, input::InputEvent::Text('o')));
    REQUIRE(bench.Frame());
    CHECK(RowAt(*panel, 4)->IsOpen());
}

} // namespace rocks::ui
