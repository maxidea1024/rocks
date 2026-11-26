#pragma once

#include <PlatformWindow/Window.h>
#include <UiFramework/Docking/DockManager.h>
#include <UiFramework/InputAdapter.h>
#include <UiFramework/WindowChrome.h>
#include <UiRender/VectorImageAtlas.h>
#include <UiRender/WidgetSurfaceHost.h>
#include <UiRender/DrawBatchRenderer.h>
#include <UiRender/DrawBatchBuffers.h>

namespace rocks::ui {

// 창마다 입력·배치·스왑 체인을 소유한다. 글꼴과 브러시 등 장치 자원은 창 사이에서 공유한다.
class DockWindows {
public:
    // 글자 레이블을 font로 shaping해 만든다(직접 그린 창 테두리의 제목과 창 버튼 툴팁).
    using TitleFactory = Delegate<Result<SharedPtr<TextLabel>>(StringView, const FontStyle&)>;

    // icons는 SVG brush를 래스터화할 atlas이고 imagePipeline으로 페이지를
    // 올린다(Docs/Design/UiToolkit 17.2절). 창들보다 오래 살아야 한다.
    DockWindows(Allocator& allocator, graphics::Device& device, graphics::PipelineHandle pipeline,
                SlotArray<BrushTexture>& textures, const SharedPtr<CommandList>& commands,
                VectorImageAtlas& icons, graphics::PipelineHandle imagePipeline,
                TitleFactory&& makeTitle) noexcept;
    // title은 직접 그린 창 테두리(WindowFrame::Custom)의 제목이다.
    Result<void> Run(UniquePtr<platform::Window> window, const SharedPtr<DockManager>& area,
                     StringView title) noexcept;

private:
    struct Surface {
        Surface(Allocator& allocator, graphics::Device& device, UniquePtr<platform::Window> window,
                const SharedPtr<DockManager>& area, SlotArray<BrushTexture>& textures) noexcept;
        ~Surface() noexcept;
        graphics::Device& device;
        UniquePtr<platform::Window> window;
        SharedPtr<DockManager> area;
        // 직접 그린 창 테두리다(창이 WindowFrame::Custom일 때만 — 루트가 된다).
        SharedPtr<WindowChrome> chrome;
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
    Result<SlotHandle> Add(UniquePtr<platform::Window> window, const SharedPtr<DockManager>& area,
                           StringView title) noexcept;
    // Custom 창이면 제목 막대와 테두리를 만들어 루트로 둔다(창 버튼은 그 창의 동작에 건다).
    Result<void> AttachChrome(Surface& surface, StringView title) noexcept;
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
    VectorImageAtlas& icons_;
    graphics::PipelineHandle imagePipeline_;
    TitleFactory makeTitle_;
    SlotArray<UniquePtr<Surface>> windows_;
    SlotHandle primary_{};
};

} // namespace rocks::ui
