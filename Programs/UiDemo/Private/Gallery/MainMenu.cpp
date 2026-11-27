#include "MainMenu.h"

#include "../DemoLog.h"
#include "GalleryWindow.h"

#include <Core/Diagnostics/Log.h>

namespace rocks::ui::gallery {

namespace {

// 갤러리 메뉴 명령 ID 영역이다.
constexpr uint64 GalleryMenuDomain = 0x47414C4C4D454E55ull;

enum class MenuCommand : uint64 {
    New = 1,
    Open,
    Save,
    SaveAll,
    Exit,
    Undo,
    Redo,
    Cut,
    Copy,
    Paste,
    Preferences,
    Fullscreen,
    Documentation,
    About
};

Guid CommandId(MenuCommand command) noexcept {
    return {GalleryMenuDomain, static_cast<uint64>(command)};
}

struct CommandSpec {
    MenuCommand command;
    StringView label;
    StringView description;
    KeyChord chord;
};

// 메뉴 항목 아이콘이다(Engine/Content/Ui/Icons/Common).
constexpr VectorImageKey NewIcon = VectorImageKey::FromPath("Common/File");
constexpr VectorImageKey OpenIcon = VectorImageKey::FromPath("Common/FolderOpen");
constexpr VectorImageKey SaveIcon = VectorImageKey::FromPath("Common/Save");
constexpr VectorImageKey UndoIcon = VectorImageKey::FromPath("Common/Undo");
constexpr VectorImageKey RedoIcon = VectorImageKey::FromPath("Common/Redo");
constexpr VectorImageKey CutIcon = VectorImageKey::FromPath("Common/Cut");
constexpr VectorImageKey CopyIcon = VectorImageKey::FromPath("Common/Copy");
constexpr VectorImageKey PasteIcon = VectorImageKey::FromPath("Common/Paste");
constexpr VectorImageKey SettingsIcon = VectorImageKey::FromPath("Common/Settings");
constexpr VectorImageKey InfoIcon = VectorImageKey::FromPath("Common/Info");

// 최근 레벨 견본 수다 — 창 높이를 넘겨 메뉴 스크롤(R9)을 보인다.
constexpr usize RecentLevelCount = 40;

// 데모 명령은 실행을 로그로 남긴다(편집기가 붙으면 실제 동작으로 바뀐다).
Delegate<void()> LogCommand(StringView label) noexcept {
    return Delegate<void()>::CreateLambda(
        [label]() noexcept { Log::Info(LogUiDemo, "Menu command: {Command}", label); });
}

} // namespace

Result<SharedPtr<MenuBar>> BuildMainMenu(GalleryContext& context, Application& app,
                                         CommandSet& definitions, const BindingManager& bindings,
                                         CommandList& commands, platform::Window& window,
                                         const SharedPtr<DockManager>& area) noexcept {
    Allocator& allocator = context.allocator;
    // 명령 정의(UE UI_COMMAND — 라벨·설명·기본 단축키)다.
    const CommandSpec specs[] = {
        {MenuCommand::New,
         "New Level",
         "Creates a new level",
         {.key = input::Key::N, .control = true}},
        {MenuCommand::Open,
         "Open Level...",
         "Opens a level",
         {.key = input::Key::O, .control = true}},
        {MenuCommand::Save,
         "Save Current Level",
         "Saves the current level",
         {.key = input::Key::S, .control = true}},
        {MenuCommand::SaveAll,
         "Save All",
         "Saves all unsaved levels and assets",
         {.key = input::Key::S, .shift = true, .control = true}},
        {MenuCommand::Exit, "Exit", "Closes the gallery", {}},
        {MenuCommand::Undo,
         "Undo",
         "Undoes the last action",
         {.key = input::Key::Z, .control = true}},
        {MenuCommand::Redo,
         "Redo",
         "Redoes the last undone action",
         {.key = input::Key::Y, .control = true}},
        {MenuCommand::Cut, "Cut", "Cuts the selection", {.key = input::Key::X, .control = true}},
        {MenuCommand::Copy,
         "Copy",
         "Copies the selection",
         {.key = input::Key::C, .control = true}},
        {MenuCommand::Paste,
         "Paste",
         "Pastes the clipboard",
         {.key = input::Key::V, .control = true}},
        {MenuCommand::Preferences, "Editor Preferences...", "Opens the editor preferences", {}},
        {MenuCommand::Fullscreen,
         "Enable Fullscreen",
         "Expands the window across the entire monitor",
         {.key = input::Key::F11, .shift = true}},
        {MenuCommand::Documentation, "Documentation", "Opens the documentation (offline)", {}},
        {MenuCommand::About, "About Rocks UI Gallery", "Shows information about the gallery", {}}};
    for (const CommandSpec& spec : specs) {
        auto added = definitions.Add(CommandId(spec.command), spec.label, spec.description,
                                     {.primary = spec.chord, .secondary = {}});
        if (!added) {
            return Result<SharedPtr<MenuBar>>::Failure(added.Error());
        }

        CommandAction action;
        switch (spec.command) {
        case MenuCommand::Exit:
            action.execute =
                Delegate<void()>::CreateLambda([&window]() noexcept { window.RequestClose(); });
            break;
        case MenuCommand::Fullscreen:
            action.execute = Delegate<void()>::CreateLambda([&window]() noexcept {
                const bool windowed = window.GetWindowMode() == platform::WindowMode::Windowed;
                (void)window.SetWindowMode(windowed ? platform::WindowMode::WindowedFullscreen
                                                    : platform::WindowMode::Windowed);
            });
            action.isChecked = Delegate<bool()>::CreateLambda([&window]() noexcept {
                return window.GetWindowMode() != platform::WindowMode::Windowed;
            });
            break;
        case MenuCommand::Documentation:
            // 비활성 항목 견본이다(문서가 없는 오프라인 데모).
            action.execute = LogCommand(spec.label);
            action.canExecute = Delegate<bool()>::CreateLambda([]() noexcept { return false; });
            break;
        default:
            action.execute = LogCommand(spec.label);
            break;
        }

        auto mapped = commands.Map(CommandId(spec.command), Move(action));
        if (!mapped) {
            return Result<SharedPtr<MenuBar>>::Failure(mapped.Error());
        }
    }

    GalleryContext* target = &context;
    auto menuContext = SharedPtr<MenuContext>::TryMake(
        allocator, allocator, app,
        MenuLabelFactory::CreateLambda([target](StringView text, const FontStyle& font) noexcept {
            return MakeText(*target, text, font);
        }));
    if (!menuContext) {
        return Result<SharedPtr<MenuBar>>::Failure(menuContext.Error());
    }

    auto bar = SharedPtr<MenuBar>::TryMake(allocator, allocator, menuContext.Value());
    if (!bar) {
        return bar;
    }

    const CommandSet* set = &definitions;
    const BindingManager* binds = &bindings;
    CommandList* list = &commands;
    const auto command = [set, binds, list](MenuBuilder& menu, MenuCommand id,
                                            VectorImageKey icon = {},
                                            MenuCheckKind check = MenuCheckKind::None) noexcept {
        menu.AddCommand(CommandId(id), *set, *list, *binds, check, icon);
    };

    auto added = bar.Value()->AddMenu(
        "File", MenuFill::CreateLambda([command, list](MenuBuilder& menu) noexcept -> Result<void> {
            menu.BeginSection("Level");
            command(menu, MenuCommand::New, NewIcon);
            command(menu, MenuCommand::Open, OpenIcon);
            // 최근 파일 하위 메뉴(데모 목록 — 창보다 길어 스크롤한다).
            menu.AddSubMenu(
                "Recent Levels",
                MenuFill::CreateLambda([](MenuBuilder& recent) noexcept -> Result<void> {
                    constexpr StringView Levels[] = {"Level_00", "Level_01", "Level_02", "Level_03",
                                                     "Level_04", "Level_05", "Level_06", "Level_07",
                                                     "Level_08", "Level_09"};
                    constexpr StringView Areas[] = {"Forest", "Desert", "Harbor", "Castle"};
                    for (usize i = 0; i < RecentLevelCount; ++i) {
                        const StringView name = i < 10 ? Levels[i] : Areas[i % 4];
                        recent.AddEntry(name, {.execute = LogCommand(name)});
                    }

                    recent.AddSeparator();
                    recent.AddEntry("Clear Recent Levels",
                                    {.execute = LogCommand("Clear Recent Levels"),
                                     .toolTip = "Removes every level from this list"});
                    return Result<void>::Success();
                }));
            menu.BeginSection("Save");
            command(menu, MenuCommand::Save, SaveIcon);
            command(menu, MenuCommand::SaveAll);
            menu.BeginSection();
            // Alt+F4는 OS가 닫으므로 표시만 한다(명령으로 묶으면 닫기 요청이 두 번 간다).
            menu.AddEntry("Exit",
                          {.execute = Delegate<void()>::CreateLambda([list]() noexcept {
                               (void)list->Execute(CommandId(MenuCommand::Exit));
                           }),
                           .toolTip = "Closes the gallery"},
                          {.key = input::Key::F4, .alt = true});
            return Result<void>::Success();
        }));
    if (added) {
        added = bar.Value()->AddMenu(
            "Edit", MenuFill::CreateLambda([command](MenuBuilder& menu) noexcept -> Result<void> {
                command(menu, MenuCommand::Undo, UndoIcon);
                command(menu, MenuCommand::Redo, RedoIcon);
                menu.AddSeparator();
                command(menu, MenuCommand::Cut, CutIcon);
                command(menu, MenuCommand::Copy, CopyIcon);
                command(menu, MenuCommand::Paste, PasteIcon);
                menu.AddSeparator();
                command(menu, MenuCommand::Preferences, SettingsIcon);
                return Result<void>::Success();
            }));
    }

    if (added) {
        const DockManager* tabs = area.Get();
        DockManager* activate = area.Get();
        added = bar.Value()->AddMenu(
            "View", MenuFill::CreateLambda([command, tabs,
                                            activate](MenuBuilder& menu) noexcept -> Result<void> {
                // 탭 항목은 그 탭을 앞으로 가져온다(라디오 — 지금 앞에 있는 탭).
                menu.BeginSection("Tabs");
                constexpr StringView Tabs[] = {
                    GalleryWindow::ColorsTab, GalleryWindow::TextTab,     GalleryWindow::WidgetsTab,
                    GalleryWindow::LayoutTab, GalleryWindow::BehaviorTab, GalleryWindow::EffectsTab,
                    GalleryWindow::IconsTab};
                for (StringView id : Tabs) {
                    menu.AddEntry(
                        id, {.execute = Delegate<void()>::CreateLambda(
                                 [activate, id]() noexcept { (void)activate->ActivateTab(id); }),
                             .isChecked = Delegate<bool()>::CreateLambda(
                                 [tabs, id]() noexcept { return tabs->IsActive(id); }),
                             .check = MenuCheckKind::Radio});
                }

                menu.BeginSection("Window");
                command(menu, MenuCommand::Fullscreen, {}, MenuCheckKind::Check);
                return Result<void>::Success();
            }));
    }

    if (added) {
        added = bar.Value()->AddMenu(
            "Help", MenuFill::CreateLambda([command](MenuBuilder& menu) noexcept -> Result<void> {
                command(menu, MenuCommand::Documentation);
                menu.AddSeparator();
                command(menu, MenuCommand::About, InfoIcon);
                return Result<void>::Success();
            }));
    }

    if (added) {
        added = bar.Value()->RegisterAccelerators();
    }

    if (!added) {
        return Result<SharedPtr<MenuBar>>::Failure(added.Error());
    }

    return bar;
}

} // namespace rocks::ui::gallery
