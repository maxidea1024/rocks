#pragma once

#include <Graphics/Device.h>
#include <UiCore/DrawBatch.h>

namespace rocks::ui {

// 한 CPU 배치에 대응하는 장치 버퍼다. 장치와 배치는 이 객체보다 오래 살아야 한다.
class ROCKS_UIRENDER_API DrawBatchBuffers {
public:
    DrawBatchBuffers(
        graphics::Device& device, const DrawBatch& batch,
        graphics::PixelFormat targetFormat = graphics::PixelFormat::RGBA8UnormSrgb) noexcept;
    ~DrawBatchBuffers() noexcept;
    DrawBatchBuffers(const DrawBatchBuffers&) = delete;
    DrawBatchBuffers& operator=(const DrawBatchBuffers&) = delete;

    // Render owner lane에서 패스 시작 전에 호출한다. 실패한 업로드는 다음 호출에서 다시 채운다.
    Result<void> Upload() noexcept;

private:
    friend class DrawBatchRenderer;
    void Release() noexcept;
    Result<void> PrepareBlur(uint32 width, uint32 height) const noexcept;
    mutable graphics::TextureHandle backdrop_{};
    mutable graphics::PipelineHandle blurPipeline_{};
    mutable uint32 backdropWidth_ = 0, backdropHeight_ = 0;
    graphics::PixelFormat targetFormat_;
    graphics::Device& device_;
    const DrawBatch& batch_;
    graphics::BufferHandle vertices_{}, indices_{};
    uint64 revision_ = 0, topology_ = 0;
    bool ready_ = false;
};

} // namespace rocks::ui
