#pragma once

#include <UiRender/DrawBatchRenderer.h>

namespace rocks::ui {

// RetainerBox::RecordContent로 만든 배치를 창 패스 전에 이 표면에 그린다.
// 성공한 뒤 Resource()를 해당 박스의 SetRetainedTexture에 전달한다.
// Render lane 전용이며 장치와 브러시 등록소는 표면보다 오래 살아야 한다.
class ROCKS_UIRENDER_API RetainedSurface {
public:
    RetainedSurface(graphics::Device& device, SlotArray<BrushTexture>& textures) noexcept;
    ~RetainedSurface() noexcept;
    RetainedSurface(const RetainedSurface&) = delete;
    RetainedSurface& operator=(const RetainedSurface&) = delete;

    // solid는 타깃 형식에 맞춰 만든 파이프라인, composite는 출력 패스의
    // CreateRetainedPipeline 결과다. 실패하면 직전에 게시한 표면을 유지한다.
    Result<void> Render(const DrawBatchBuffers& buffers, uint32 width, uint32 height,
                        graphics::PixelFormat format, graphics::PipelineHandle solid,
                        graphics::PipelineHandle composite) noexcept;
    SlotHandle Resource() const noexcept;

private:
    struct Target {
        graphics::TextureHandle texture{};
        uint32 width = 0;
        uint32 height = 0;
        graphics::PixelFormat format = graphics::PixelFormat::RGBA8UnormSrgb;
    };

    graphics::Device& device_;
    SlotArray<BrushTexture>& textures_;
    Target visible_{};
    Target scratch_{};
    SlotHandle resource_{};
};

} // namespace rocks::ui
