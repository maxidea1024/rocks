#pragma once

#include <Graphics/Device.h>
#include <RenderBase/ShaderSet.h>
#include <UiCore/DrawBatch.h>

namespace rocks::ui {

// 한 CPU 배치에 대응하는 장치 버퍼다. 장치·셰이더 집합·배치는 이 객체보다 오래 살아야 한다.
class ROCKS_UIRENDER_API DrawBatchBuffers {
public:
    DrawBatchBuffers(
        graphics::Device& device, render::ShaderSet& shaders, const DrawBatch& batch,
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
    // UI pixel 셰이더의 텍스처·배경 slot을 셰이더 바인딩 layout에서 찾아 둔다(RenderBase 9.13절).
    // 이미지·마스크·보관 셰이더는 텍스처 하나(UiTexture)만 선언하므로 slot이 같아야 하고, 다르면
    // InvalidArgument다.
    Result<void> PrepareSlots() const noexcept;
    uint32 TextureSlot() const noexcept;
    uint32 BackdropSlot() const noexcept;
    Result<void> PrepareRoundedBox() const noexcept;
    mutable graphics::TextureHandle backdrop_{};
    mutable graphics::PipelineHandle blurPipeline_{};
    mutable graphics::PipelineHandle roundedPipeline_{};
    mutable uint32 backdropWidth_ = 0, backdropHeight_ = 0;
    mutable uint32 textureSlot_ = graphics::MaxResourceSlots;
    mutable uint32 backdropSlot_ = graphics::MaxResourceSlots;
    graphics::PixelFormat targetFormat_;
    graphics::Device& device_;
    render::ShaderSet& shaders_;
    const DrawBatch& batch_;
    graphics::BufferHandle vertices_{}, indices_{};
    uint64 revision_ = 0, topology_ = 0;
    bool ready_ = false;
};

} // namespace rocks::ui
