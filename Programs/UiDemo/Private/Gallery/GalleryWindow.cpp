#include "GalleryWindow.h"

#include "MainMenu.h"

#include "ColorsPage.h"
#include "IconsPage.h"
#include "TextPage.h"

#include <Core/Diagnostics/Log.h>

namespace rocks::ui::gallery {

namespace {

inline constexpr LogCategory LogGallery{"UiDemo.Gallery"};

// 탭을 만들다 실패한 단계를 남긴다(self-test·실행 실패의 원인을 바로 알 수 있게).
template <typename T>
Result<SharedPtr<DockManager>> Fail(StringView step, const Result<T>& failed) noexcept {
    Log::Error(LogGallery, "Gallery {Step} failed: {Error}", step,
               StringView(ErrorCodeName(failed.Error())));
    return Result<SharedPtr<DockManager>>::Failure(failed.Error());
}

} // namespace

GalleryWindow::GalleryWindow(Allocator& allocator, DemoText& text, const GalleryTextures& textures,
                             const VectorImageAtlas* icons) noexcept
    : allocator_(allocator), strings_(allocator),
      context_{allocator, text, strings_, textures, icons}, definitions_(allocator),
      bindings_(allocator) {
}

Result<SharedPtr<MenuBar>>
GalleryWindow::BuildMainMenu(Application& app, platform::Window& window,
                             const SharedPtr<DockManager>& area) noexcept {
    if (!commands_) {
        return Result<SharedPtr<MenuBar>>::Failure(ErrorCode::NotReady);
    }

    return gallery::BuildMainMenu(context_, app, definitions_, bindings_, *commands_, window, area);
}

const SharedPtr<CommandList>& GalleryWindow::Commands() const noexcept {
    return commands_;
}

const GalleryGrid* GalleryWindow::Widgets() const noexcept {
    return widgets_.grid.Get();
}

const GalleryGrid* GalleryWindow::Layout() const noexcept {
    return layout_.grid.Get();
}

const SharedPtr<DockManager>& GalleryWindow::LayoutDocking() const noexcept {
    return layout_.docking;
}

Result<void> GalleryWindow::AddTab(DockManager& area, StringView id,
                                   const SharedPtr<Element>& content) noexcept {
    // 탭 이름은 UE TabTextStyle(NormalText)이고 색은 탭 상태 전경색을 따른다. 탭 역할은 UE
    // 갤러리처럼 Panel이다.
    auto header = MakeText(context_, id);
    if (!header) {
        return Result<void>::Failure(header.Error());
    }

    auto registered = area.RegisterTab({id, header.Value(), content, DockRole::Panel, false});
    if (!registered) {
        return registered;
    }

    return area.OpenTab(id);
}

Result<SharedPtr<DockManager>> GalleryWindow::Create() noexcept {
    if (auto loaded = strings_.Load(); !loaded) {
        return Fail("string table", loaded);
    }

    auto list = SharedPtr<CommandList>::TryMake(allocator_, allocator_, definitions_, bindings_);
    auto area = SharedPtr<DockManager>::TryMake(allocator_, allocator_);
    if (const ErrorCode error = FirstError(list, area); error != ErrorCode::None) {
        return Result<SharedPtr<DockManager>>::Failure(error);
    }

    commands_ = Move(list.Value());
    auto colors = BuildColorsPage(context_, DefaultThemeColors());
    if (!colors) {
        return Fail("colors page", colors);
    }

    auto text = BuildTextPage(context_);
    if (!text) {
        return Fail("text page", text);
    }

    auto widgets = BuildWidgetsPage(context_);
    if (!widgets) {
        return Fail("widgets page", widgets);
    }

    auto layout = BuildLayoutPage(context_);
    if (!layout) {
        return Fail("layout page", layout);
    }

    auto behavior = BuildBehaviorPage(context_);
    if (!behavior) {
        return Fail("behavior page", behavior);
    }

    auto effects = BuildEffectsPage(context_);
    if (!effects) {
        return Fail("effects page", effects);
    }

    auto iconsPage = BuildIconsPage(context_);
    if (!iconsPage) {
        return Fail("icons page", iconsPage);
    }

    widgets_ = Move(widgets.Value());
    layout_ = Move(layout.Value());
    behavior_ = Move(behavior.Value());
    effects_ = Move(effects.Value());
    DockManager& dock = *area.Value();
    auto result = AddTab(dock, ColorsTab, colors.Value());
    if (result) {
        result = AddTab(dock, TextTab, text.Value());
    }

    if (result) {
        result = AddTab(dock, WidgetsTab, widgets_.root);
    }

    if (result) {
        result = AddTab(dock, LayoutTab, layout_.root);
    }

    if (result) {
        result = AddTab(dock, BehaviorTab, behavior_.root);
    }

    if (result) {
        result = AddTab(dock, EffectsTab, effects_.root);
    }

    if (result) {
        result = AddTab(dock, IconsTab, iconsPage.Value());
    }

    // UE SetForegroundTab("Starship Widgets")다.
    if (result) {
        result = dock.ActivateTab(WidgetsTab);
    }

    if (!result) {
        return Fail("tab registration", result);
    }

    return area;
}

} // namespace rocks::ui::gallery
