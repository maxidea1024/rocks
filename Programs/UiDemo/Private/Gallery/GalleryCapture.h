#pragma once

#include <UiFramework/Docking/DockManager.h>
#include <UiRender/VectorImageAtlas.h>

// 갤러리 탭 캡처다(Docs/Design/UiToolkit 16.8절 `--snapshot`). tabs의 탭을 하나씩 앞에 띄워 그리고
// Saved/UiDemo/Gallery-<탭>.ppm으로 쓴다. 사람이 UE 갤러리와 나란히 비교하는 자료다.
namespace rocks::ui::gallery {

// renderer로 오프스크린 렌더 타깃에 그리고 화면 읽기로 받는다(Docs/Design/UiToolkit 18.5절).
Result<void> CaptureGallery(Allocator& allocator, UiRenderer& renderer,
                            const SharedPtr<DockManager>& area, VectorImageAtlas& icons,
                            Span<const StringView> tabs, uint32 width, uint32 height) noexcept;

} // namespace rocks::ui::gallery
