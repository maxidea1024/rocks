#pragma once

#include <PlatformWindow/Window.h>
#include <UiFramework/Docking/DockManager.h>
#include <UiFramework/InputAdapter.h>
#include <UiRender/WidgetSurfaceHost.h>
#include <UiRender/DrawBatchRenderer.h>
#include <UiRender/DrawBatchBuffers.h>

namespace rocks::ui {

// 창마다 입력·배치·스왑 체인을 소유한다. 글꼴과 브러시 등 장치 자원은 창 사이에서 공유한다.
class DockWindows {
public:
    DockWindows(Allocator& allocator, graphics::Device& device, graphics::PipelineHandle pipeline,
                SlotArray<BrushTexture>& textures, const SharedPtr<CommandList>& commands) noexcept;
    Result<void> Run(UniquePtr<platform::Window> window,
                     const SharedPtr<DockManager>& area) noexcept;

private:
    struct Surface {
        Surface(Allocator& allocator, graphics::Device& device, UniquePtr<platform::Window> window,
                const SharedPtr<DockManager>& area, SlotArray<BrushTexture>& textures) noexcept;
        ~Surface() noexcept;
        graphics::Device& device;
        UniquePtr<platform::Window> window;
        SharedPtr<DockManager> area;
        Application app;
        InputAdapter adapter;
        input::InputQueue queue;
        Array<uint8> consumed;
        DrawBatch batch;
        DrawBatchBuffers buffers;
        WidgetSurfaceHost surfaces;
        uint64 frameNumber = 0;
        graphics::SwapChainHandle chain{};
        UniquePtr<platform::DragImage> dragImage;
        uint64 previewGesture = 0;
        IntVec2 previewPosition{};
        uint64 batchRevision = 0;
        bool presented = false;
        // 크기 조절 modal 루프 안의 실시간 그리기가 실패한 원인이다(Run이 다음 차례에 돌려준다).
        ErrorCode liveResizeError = ErrorCode::None;
    };
    Result<SlotHandle> Add(UniquePtr<platform::Window> window,
                           const SharedPtr<DockManager>& area) noexcept;
    Result<bool> Update(Surface& surface) noexcept;
    // 펌프 없이 크기 반영·배치·그리기·present를 한다. 크기 조절 콜백(UiToolkit 9.1절 UW7a)도 쓴다.
    Result<bool> Render(Surface& surface, bool refreshCursor) noexcept;
    Result<void> UpdateDragImage(Surface& surface) noexcept;
    Result<void> ReturnTabs(Surface& surface) noexcept;
    SharedPtr<DockManager> FindArea(Surface& source, Vec2 point, Vec2& clientPoint) noexcept;
    Result<void> FloatTab(Surface& source, StringView id, Vec2 point) noexcept;
    Allocator& allocator_;
    graphics::Device& device_;
    graphics::PipelineHandle pipeline_;
    SlotArray<BrushTexture>& textures_;
    SharedPtr<CommandList> commands_;
    SlotArray<UniquePtr<Surface>> windows_;
    SlotHandle primary_{};
};

} // namespace rocks::ui
