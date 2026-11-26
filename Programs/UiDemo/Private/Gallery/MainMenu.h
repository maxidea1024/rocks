#pragma once

#include "GalleryParts.h"

#include <PlatformWindow/Window.h>
#include <UiFramework/Docking/DockManager.h>
#include <UiFramework/MenuBar.h>

// 갤러리 창 주 메뉴다(Docs/Design/UiToolkit 9.1절 UW7c). File·Edit·View·Help를 제목 막대 왼쪽에
// 둔다. 명령은 갤러리 명령 목록에 정의·연결되어 단축키(Ctrl+S 등)로도 실행된다. 데모 명령은 실행을
// 로그로 남기고, Exit·Enable Fullscreen은 실제 창 동작을 한다. View의 탭 항목은 그 탭을 앞으로
// 가져온다(라디오 — 지금 앞에 있는 탭).
namespace rocks::ui::gallery {

// definitions·bindings·commands·window·area는 메뉴 막대보다 오래 살아야 한다.
Result<SharedPtr<MenuBar>> BuildMainMenu(GalleryContext& context, Application& app,
                                         CommandSet& definitions, const BindingManager& bindings,
                                         CommandList& commands, platform::Window& window,
                                         const SharedPtr<DockManager>& area) noexcept;

} // namespace rocks::ui::gallery
