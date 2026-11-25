#pragma once

#include "BehaviorPage.h"
#include "EffectsPage.h"
#include "LayoutPage.h"
#include "WidgetsPage.h"

#include <UiFramework/Commands.h>
#include <UiFramework/Docking/DockManager.h>

// UI 갤러리 창이다(UE SStarshipGallery, Docs/Design/UiToolkit 16장). 탭 묶음 하나에 "Colors"·
// "Text"·"Widgets"·"Layout"·"Behavior"·"Effects"를 두고 UE처럼 "Widgets"를 앞에 연다. 창이 열려
// 있는 동안 탭들의 콜백이 참조하는 상태(글꼴·문자열 표·공용 문맥)를 소유하므로 탭 내용보다 오래
// 살아야 한다.
//
// 사용 예:
//   gallery::GalleryWindow gallery(allocator, text);
//   auto area = gallery.Create();
//   DockWindows windows(allocator, device, pipeline, textures, gallery.Commands());
//   windows.Run(Move(window), area.Value());
namespace rocks::ui::gallery {

class GalleryWindow {
public:
    // 탭 ID다(레이아웃 저장에도 쓴다). UE 갤러리 탭 이름을 따른다.
    static constexpr StringView ColorsTab = "Colors";
    static constexpr StringView TextTab = "Text";
    static constexpr StringView WidgetsTab = "Widgets";
    static constexpr StringView LayoutTab = "Layout";
    static constexpr StringView BehaviorTab = "Behavior";
    static constexpr StringView EffectsTab = "Effects";
    static constexpr StringView IconsTab = "Icons";

    // icons는 Icons 탭이 보일 SVG 아이콘 목록이다(없으면 빈 탭). 창보다 오래 살아야 한다.
    GalleryWindow(Allocator& allocator, DemoText& text, const GalleryTextures& textures,
                  const VectorImageAtlas* icons = nullptr) noexcept;

    // 문자열 표를 읽고 탭을 만든다. 문자열 파일이 없으면 NotFound다.
    Result<SharedPtr<DockManager>> Create() noexcept;
    const SharedPtr<CommandList>& Commands() const noexcept;

    // 탭의 행 격자와 배치 탭의 도킹 영역이다(Create 뒤에만 유효하다 — self-test가 쓴다).
    const GalleryGrid* Widgets() const noexcept;
    const GalleryGrid* Layout() const noexcept;
    const SharedPtr<DockManager>& LayoutDocking() const noexcept;

private:
    Result<void> AddTab(DockManager& area, StringView id,
                        const SharedPtr<Element>& content) noexcept;

    Allocator& allocator_;
    GalleryStrings strings_;
    GalleryContext context_;
    CommandSet definitions_;
    BindingManager bindings_;
    SharedPtr<CommandList> commands_;
    WidgetsPage widgets_{};
    LayoutPage layout_{};
    BehaviorPage behavior_{};
    EffectsPage effects_{};
};

} // namespace rocks::ui::gallery
