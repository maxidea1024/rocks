#pragma once

#include <UiFramework/Docking/DockManager.h>
#include <UiRender/DrawBatchRenderer.h>

// 갤러리 탭 캡처다(Docs/Design/UiToolkit 16.8절 `--snapshot`). tabs의 탭을 하나씩 앞에 띄워 그리고
// Saved/UiDemo/Gallery-<탭>.ppm으로 쓴다. 사람이 UE 갤러리와 나란히 비교하는 자료다.
namespace rocks::ui::gallery {

Result<void> CaptureGallery(Allocator& allocator, graphics::Device& device,
                            graphics::PipelineHandle pipeline, SlotArray<BrushTexture>& textures,
                            const SharedPtr<DockManager>& area, Span<const StringView> tabs,
                            uint32 width, uint32 height) noexcept;

} // namespace rocks::ui::gallery
